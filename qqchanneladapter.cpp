#include "qqchanneladapter.h"
#include "debuglog.h"
#include "qqprotocolclient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

QQChannelAdapter::QQChannelAdapter(QObject *parent)
    : QObject(parent)
{
}

QJsonArray QQChannelAdapter::buildTextMessage(const QString &text)
{
    QJsonObject segment;
    segment["type"] = "text";
    QJsonObject data;
    data["text"] = text;
    segment["data"] = data;
    return QJsonArray{segment};
}

QString QQChannelAdapter::channelName() const
{
    // QQ 私聊/群聊由 Utterance.channel 动态区分
    return Channel::kQQPrivate;
}

void QQChannelAdapter::setClient(QQProtocolClient *client)
{
    m_client = client;
}

void QQChannelAdapter::sendUtterance(const Utterance &u)
{
    if (!m_client || !m_client->isConnected()) {
        qDebug() << "[QQChannel]未连接，消息未发送:" << u.channel << u.sessionId;
        return;
    }
    if (u.text.trimmed().isEmpty()) {
        return;
    }

    QJsonObject params;
    QString action;

    if (u.channel == Channel::kQQGroup) {
        action = QStringLiteral("send_group_msg");
        params["group_id"] = u.sessionId.toLongLong();
    } else if (u.channel == Channel::kQQPrivate) {
        action = QStringLiteral("send_private_msg");
        params["user_id"] = u.sessionId.toLongLong();
    } else {
        qDebug() << "[QQChannel]未知渠道，忽略:" << u.channel;
        return;
    }

    params["message"] = buildTextMessage(u.text);
    // 群聊场景可带 auto_escape，保持纯文本
    m_client->sendAction(action, params);
    qDebug() << "[QQChannel]发送:" << u.channel << u.sessionId
             << "chars=" << u.text.size();
    if (debugFullLogsEnabled()) {
        qDebug().noquote() << "[QQChannel][debug] 发送:" << u.text;
    }
}
