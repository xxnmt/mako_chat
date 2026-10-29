#include "qqprotocolclient.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>
#include <QDateTime>

QQProtocolClient::QQProtocolClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_socket, &QWebSocket::connected, this, &QQProtocolClient::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &QQProtocolClient::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &QQProtocolClient::onTextMessageReceived);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(&m_socket, &QWebSocket::errorOccurred, this, &QQProtocolClient::onSocketError);
#else
    connect(&m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &QQProtocolClient::onSocketError);
#endif

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &QQProtocolClient::openSocket);
}

QQProtocolClient::~QQProtocolClient()
{
    stop();
}

void QQProtocolClient::setConfig(const QQConfig &config)
{
    m_config = config;
}

bool QQProtocolClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void QQProtocolClient::start()
{
    if (!m_config.enabled) {
        qDebug() << "[QQ]未启用（setting.json qq.enabled=false）";
        return;
    }
    if (m_running) {
        return;
    }
    m_running = true;
    openSocket();
}

void QQProtocolClient::stop()
{
    m_running = false;
    m_reconnectTimer.stop();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.close();
    }
}

void QQProtocolClient::openSocket()
{
    if (!m_running) {
        return;
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        return;
    }

    QNetworkRequest request{QUrl(m_config.wsUrl)};
    if (!m_config.accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_config.accessToken).toUtf8());
    }

    m_socket.open(request);
}

void QQProtocolClient::onConnected()
{
    m_backoffMs = 3000;
    m_wasConnected = true;
    qDebug() << "[QQ]WebSocket 已连接";
    emit connected();

    // 连接后自检：确认协议端登录账号
    sendAction(QStringLiteral("get_login_info"), QJsonObject(), QStringLiteral("login"));
}

void QQProtocolClient::onDisconnected()
{
    qDebug() << "[QQ]WebSocket 断开";
    if (m_wasConnected) {
        m_wasConnected = false;
        emit disconnected();
    }
    m_pendingActions.clear();
    scheduleReconnect();
}

void QQProtocolClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    const QString message = m_socket.errorString();
    qDebug() << "[QQ]WebSocket 错误:" << message;
    emit errorOccurred(message);
    scheduleReconnect();
}

void QQProtocolClient::scheduleReconnect()
{
    if (!m_running || m_reconnectTimer.isActive()) {
        return;
    }
    qDebug() << "[QQ]" << m_backoffMs << "ms 后重连";
    m_reconnectTimer.start(m_backoffMs);
    m_backoffMs = qMin(m_backoffMs * 2, 30000);
}

void QQProtocolClient::sendAction(const QString &action, const QJsonObject &params,
                                  const QString &echo)
{
    if (!isConnected()) {
        qDebug() << "[QQ]动作发送失败（未连接）:" << action;
        return;
    }
    const QString actualEcho = echo.isEmpty()
        ? QString("%1-%2").arg(action).arg(QDateTime::currentMSecsSinceEpoch())
        : echo;
    if (action.startsWith(QStringLiteral("send_"))) {
        m_pendingActions.insert(actualEcho, PendingAction{action, params, 0});
    }
    qDebug() << "[QQ]发送动作:" << action << "echo=" << actualEcho;
    sendPayload(action, params, actualEcho);
}

void QQProtocolClient::sendPayload(const QString &action, const QJsonObject &params,
                                   const QString &echo)
{
    QJsonObject root;
    root["action"] = action;
    root["params"] = params;
    root["echo"] = echo;
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void QQProtocolClient::onTextMessageReceived(const QString &message)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qDebug() << "[QQ]收到非 JSON 报文:" << message.left(200);
        return;
    }

    const QJsonObject obj = doc.object();

    // 动作响应（echo 匹配）
    if (obj.contains("echo")) {
        const QString echo = obj.value("echo").toString();
        const bool ok = obj.value("status").toString() == QStringLiteral("ok");
        emit actionResponse(echo, obj.value("data"), obj.value("retcode").toInt());
        if (echo == QStringLiteral("login")) {
            const QJsonObject data = obj.value("data").toObject();
            qDebug() << "[QQ]登录账号:" << data.value("user_id").toVariant().toString()
                     << data.value("nickname").toString();
            return;
        }

        auto it = m_pendingActions.find(echo);
        if (!ok) {
            qDebug() << "[QQ]动作失败:" << echo
                     << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            const int retcode = obj.value("retcode").toInt();
            if (it != m_pendingActions.end() && retcode == 100 && it->attempts < 1
                && it->action.startsWith(QStringLiteral("send_"))) {
                it->attempts++;
                const QString retryEcho = echo;
                qDebug() << "[QQ]发送失败 retcode=100，4 秒后重试一次:" << retryEcho;
                QTimer::singleShot(4000, this, [this, retryEcho]() {
                    auto retryIt = m_pendingActions.find(retryEcho);
                    if (retryIt != m_pendingActions.end() && isConnected()) {
                        sendPayload(retryIt->action, retryIt->params, retryEcho);
                    }
                });
            } else if (it != m_pendingActions.end()) {
                m_pendingActions.erase(it);
            }
        } else if (it != m_pendingActions.end()) {
            m_pendingActions.erase(it);
        }
        return;
    }

    if (obj.value("post_type").toString() == QStringLiteral("message")) {
        emit messageEvent(obj);
    }
}
