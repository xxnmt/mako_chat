#include "qqchanneladapter.h"
#include <QDebug>

QQChannelAdapter::QQChannelAdapter(QObject *parent)
    : QObject(parent)
{
}

QString QQChannelAdapter::channelName() const
{
    // 占位返回；Phase 1 实际按会话动态区分 qq_private / qq_group
    return Channel::kQQPrivate;
}

void QQChannelAdapter::sendUtterance(const Utterance &u)
{
    // Phase 1: 按 u.channel / u.sessionId 分发到 OneBot send_private_msg / send_group_msg
    qDebug() << "[QQChannel](占位)未发送:" << u.channel << u.sessionId << u.text;
}
