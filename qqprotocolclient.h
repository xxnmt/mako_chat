#ifndef QQPROTOCOLCLIENT_H
#define QQPROTOCOLCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QQueue>

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

    // 发送 OneBot 动作（唯一 echo）。send_* 按回执串行；true 表示接受提交/入队，非送达证明。
    bool sendAction(const QString &action, const QJsonObject &params = QJsonObject(),
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
    bool sendPayload(const QString &action, const QJsonObject &params, const QString &echo);
    void sendNextAction();
    void finishSendAction();

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
        bool waitingRetry = false;
    };
    QHash<QString, PendingAction> m_pendingActions;
    QQueue<QString> m_sendQueue;
    QTimer m_sendTimeout;
    QString m_activeSend;
};

#endif // QQPROTOCOLCLIENT_H
