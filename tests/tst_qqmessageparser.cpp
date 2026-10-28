#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "qqmessageparser.h"
#include "utterance.h"

class TestQQMessageParser : public QObject
{
    Q_OBJECT

private:
    static QJsonObject parseJson(const char *json)
    {
        return QJsonDocument::fromJson(QByteArray(json)).object();
    }

private slots:
    void parsesPrivateTextMessage()
    {
        const QJsonObject event = parseJson(R"({
            "post_type":"message","message_type":"private","sub_type":"friend",
            "self_id":10001,"user_id":20001,"message_id":158380796,
            "time":1770000000,
            "sender":{"user_id":20001,"nickname":"测试用户","card":""},
            "message":[{"type":"text","data":{"text":"测试1"}}]
        })");

        const QQInboundMessage message = QQMessageParser::parse(event);
        QVERIFY(message.valid);
        QCOMPARE(message.channel, Channel::kQQPrivate);
        QCOMPARE(message.sessionId, QStringLiteral("20001"));
        QCOMPARE(message.speaker, QStringLiteral("测试用户"));
        QCOMPARE(message.senderId, QStringLiteral("20001"));
        QCOMPARE(message.messageId, QStringLiteral("158380796"));
        QCOMPARE(message.text, QStringLiteral("测试1"));
        QVERIFY(message.occurredAt.isValid());
        QVERIFY(!message.mentionedSelf);
        QVERIFY(!QQMessageParser::isSelfMessage(event));
    }

    void parsesGroupAtAndText()
    {
        const QJsonObject event = parseJson(R"({
            "post_type":"message","message_type":"group",
            "self_id":10001,"group_id":30001,"message_id":-664218619,
            "time":1770000000,
            "sender":{"user_id":20001,"nickname":"测试用户","card":"群名片"},
            "message":[
                {"type":"at","data":{"qq":"10001"}},
                {"type":"text","data":{"text":" 测试2"}}
            ]
        })");

        const QQInboundMessage message = QQMessageParser::parse(event);
        QVERIFY(message.valid);
        QCOMPARE(message.channel, Channel::kQQGroup);
        QCOMPARE(message.sessionId, QStringLiteral("30001"));
        QCOMPARE(message.speaker, QStringLiteral("群名片"));
        QCOMPARE(message.messageId, QStringLiteral("-664218619"));
        QVERIFY(message.text.contains(QStringLiteral("@10001")));
        QVERIFY(message.text.contains(QStringLiteral("测试2")));
        QVERIFY(message.mentionedSelf);
    }

    void parsesReplyTarget()
    {
        const QJsonObject event = parseJson(R"({
            "post_type":"message","message_type":"group",
            "self_id":10001,"group_id":30001,"message_id":11,"time":1770000000,
            "sender":{"user_id":20002,"nickname":"B"},
            "message":[
                {"type":"reply","data":{"id":"10"}},
                {"type":"text","data":{"text":"回复你"}}
            ]
        })");
        const QQInboundMessage message = QQMessageParser::parse(event);
        QVERIFY(message.valid);
        QCOMPARE(message.replyToMessageId, QStringLiteral("10"));
        QCOMPARE(message.text, QStringLiteral("回复你"));
    }

    void groupWithoutMentionIsNotMentioned()
    {
        const QJsonObject event = parseJson(R"({
            "post_type":"message","message_type":"group",
            "self_id":10001,"group_id":30001,"message_id":10,"time":1770000000,
            "sender":{"user_id":20001,"nickname":"A"},
            "message":[{"type":"text","data":{"text":"随便聊聊"}}]
        })");
        const QQInboundMessage message = QQMessageParser::parse(event);
        QVERIFY(message.valid);
        QVERIFY(!message.mentionedSelf);
    }

    void convertsMediaSegmentsToPlaceholders()
    {
        const QJsonObject event = parseJson(R"({
            "post_type":"message","message_type":"private",
            "self_id":10001,"user_id":20001,"message_id":1,"time":1770000000,
            "sender":{"user_id":20001,"nickname":"A"},
            "message":[
                {"type":"image","data":{"file":"a.jpg"}},
                {"type":"record","data":{"file":"a.amr"}},
                {"type":"face","data":{"id":"1"}}
            ]
        })");

        const QQInboundMessage message = QQMessageParser::parse(event);
        QVERIFY(message.valid);
        QCOMPARE(message.text, QStringLiteral("[图片][语音][表情]"));
    }

    void detectsSelfMessage()
    {
        const QJsonObject selfEvent = parseJson(R"({
            "post_type":"message","message_type":"private",
            "self_id":10001,"user_id":20001,"message_id":2,"time":1770000000,
            "sender":{"user_id":10001,"nickname":"测试小号"},
            "message":[{"type":"text","data":{"text":"自己发的"}}]
        })");
        QVERIFY(QQMessageParser::isSelfMessage(selfEvent));

        const QJsonObject otherEvent = parseJson(R"({
            "post_type":"message","message_type":"private",
            "self_id":10001,"user_id":20001,"message_id":3,"time":1770000000,
            "sender":{"user_id":20001,"nickname":"主人"},
            "message":[{"type":"text","data":{"text":"hi"}}]
        })");
        QVERIFY(!QQMessageParser::isSelfMessage(otherEvent));
    }

    void ignoresNonMessageEvent()
    {
        const QJsonObject event = parseJson(R"({"post_type":"meta_event","meta_event_type":"heartbeat"})");
        QVERIFY(!QQMessageParser::isMessageEvent(event));
        QVERIFY(!QQMessageParser::parse(event).valid);
    }
};

QTEST_GUILESS_MAIN(TestQQMessageParser)
#include "tst_qqmessageparser.moc"
