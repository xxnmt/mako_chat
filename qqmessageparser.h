#ifndef QQMESSAGEPARSER_H
#define QQMESSAGEPARSER_H

#include <QString>
#include <QDateTime>
#include <QJsonObject>

// OneBot v11 入站消息（私聊/群聊）的统一解析结果
struct QQInboundMessage
{
    bool valid = false;
    bool mentionedSelf = false;  // 群聊是否 @ 了机器人
    QString channel;       // qq_private / qq_group
    QString sessionId;     // 好友 QQ 或群号
    QString speaker;       // 群名片优先，否则昵称
    QString senderId;      // 发送者 QQ（稳定身份）
    QString messageId;     // 平台消息 ID（去重）
    QString replyToMessageId; // OneBot reply/quote 目标（可空）
    QString text;          // 纯文本（图片/语音等转为占位符）
    QString rawJson;       // 原始事件 JSON（raw_events.payload）
    QDateTime occurredAt;
    qlonglong selfId = 0;
    qlonglong senderUin = 0;
};

class QQMessageParser
{
public:
    static bool isMessageEvent(const QJsonObject &event);
    static bool isSelfMessage(const QJsonObject &event);

    // 解析 post_type=message 的私聊/群聊事件；不合法时 valid=false
    static QQInboundMessage parse(const QJsonObject &event);
};

#endif // QQMESSAGEPARSER_H
