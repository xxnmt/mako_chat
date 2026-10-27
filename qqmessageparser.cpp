#include "qqmessageparser.h"
#include "utterance.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace {

qlonglong toLongLong(const QJsonValue &value)
{
    if (value.isDouble()) {
        return static_cast<qlonglong>(value.toDouble());
    }
    if (value.isString()) {
        return value.toString().toLongLong();
    }
    return 0;
}

QString idToString(const QJsonValue &value)
{
    if (value.isDouble()) {
        return QString::number(static_cast<qlonglong>(value.toDouble()));
    }
    return value.toString();
}

QString segmentText(const QJsonObject &segment)
{
    const QString type = segment.value("type").toString();
    const QJsonObject data = segment.value("data").toObject();

    if (type == QStringLiteral("text")) {
        return data.value("text").toString();
    }
    if (type == QStringLiteral("at")) {
        const QString qq = data.value("qq").toString();
        return qq == QStringLiteral("all") ? QStringLiteral("@全体成员")
                                           : QStringLiteral("@%1").arg(qq);
    }
    if (type == QStringLiteral("image")) {
        return QStringLiteral("[图片]");
    }
    if (type == QStringLiteral("record")) {
        return QStringLiteral("[语音]");
    }
    if (type == QStringLiteral("face")) {
        return QStringLiteral("[表情]");
    }
    if (type == QStringLiteral("reply")) {
        return QString();
    }
    return QStringLiteral("[%1]").arg(type);
}

} // namespace

bool QQMessageParser::isMessageEvent(const QJsonObject &event)
{
    return event.value("post_type").toString() == QStringLiteral("message");
}

bool QQMessageParser::isSelfMessage(const QJsonObject &event)
{
    const qlonglong selfId = toLongLong(event.value("self_id"));
    const qlonglong senderUin = toLongLong(event.value("sender").toObject().value("user_id"));
    return selfId != 0 && senderUin != 0 && selfId == senderUin;
}

QQInboundMessage QQMessageParser::parse(const QJsonObject &event)
{
    QQInboundMessage message;
    if (!isMessageEvent(event)) {
        return message;
    }

    const QString messageType = event.value("message_type").toString();
    if (messageType == QStringLiteral("private")) {
        message.channel = Channel::kQQPrivate;
        message.sessionId = idToString(event.value("user_id"));
    } else if (messageType == QStringLiteral("group")) {
        message.channel = Channel::kQQGroup;
        message.sessionId = idToString(event.value("group_id"));
    } else {
        return message;
    }

    const QJsonObject sender = event.value("sender").toObject();
    message.senderUin = toLongLong(sender.value("user_id"));
    message.selfId = toLongLong(event.value("self_id"));
    message.senderId = idToString(sender.value("user_id"));
    message.speaker = sender.value("card").toString();
    if (message.speaker.isEmpty()) {
        message.speaker = sender.value("nickname").toString();
    }
    if (message.speaker.isEmpty()) {
        message.speaker = message.senderId;
    }

    message.messageId = idToString(event.value("message_id"));
    message.occurredAt = QDateTime::fromSecsSinceEpoch(toLongLong(event.value("time")));

    // message 既可能是消息段数组，也可能是 CQ 码字符串；这里主要支持数组
    QString text;
    const QJsonValue messageValue = event.value("message");
    if (messageValue.isArray()) {
        const QJsonArray segments = messageValue.toArray();
        for (const QJsonValue &segmentValue : segments) {
            const QJsonObject segment = segmentValue.toObject();
            if (segment.value("type").toString() == QStringLiteral("at")) {
                const QJsonValue atValue = segment.value("data").toObject().value("qq");
                const QString atQq = atValue.isDouble()
                    ? QString::number(static_cast<qlonglong>(atValue.toDouble()))
                    : atValue.toString();
                if (atQq == QString::number(message.selfId)) {
                    message.mentionedSelf = true;
                }
            }
            text += segmentText(segment);
        }
    } else if (messageValue.isString()) {
        text = messageValue.toString();
    }
    message.text = text.trimmed();
    message.rawJson = QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact));
    message.valid = !message.sessionId.isEmpty();
    return message;
}
