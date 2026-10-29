#ifndef QQPROTOCOLCLIENT_H
#define QQPROTOCOLCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>

#include "configmanager.h"

// OneBot v11 WebSocket 客户端（QQ 协议端作为 WS 服务端）
// 只负责连接/重连/收发与事件转发；消息解析见 QQMessageParser。
class QQProtocolClient : public QObject
{
    Q_OBJECT
public:
    explicit QQProtocolClient(QObject *parent = nullptr);
    ~QQProtocolClient() override;

    void setConfig(const QQConfig &config);
    void start();
    void stop();
    bool isConnected() const;

    // 发送 OneBot 动作（自动带 echo）
    void sendAction(const QString &action, const QJsonObject &params = QJsonObject(),
                    const QString &echo = QString());

signals:
    void connected();
    void disconnected();
    void messageEvent(const QJsonObject &event);
    void noticeEvent(const QJsonObject &event);
    void actionResponse(const QString &echo, const QJsonValue &data, int retcode);
    void errorOccurred(const QString &error);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onSocketError(QAbstractSocket::SocketError error);

private:
    void openSocket();
    void scheduleReconnect();
    void sendPayload(const QString &action, const QJsonObject &params, const QString &echo);

    QWebSocket m_socket;
    QTimer m_reconnectTimer;
    QQConfig m_config;
    bool m_running = false;
    bool m_wasConnected = false;
    int m_backoffMs = 3000;

    struct PendingAction {
        QString action;
        QJsonObject params;
        int attempts = 0;
    };
    QHash<QString, PendingAction> m_pendingActions;
};

#endif // QQPROTOCOLCLIENT_H
