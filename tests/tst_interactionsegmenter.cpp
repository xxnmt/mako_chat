#include <QtTest>

#include "interactionsegmenter.h"
#include "utterance.h"

class TestInteractionSegmenter : public QObject
{
    Q_OBJECT

private:
    static RawEvent event(qlonglong id, const QDateTime &time,
                          const QString &senderId = QStringLiteral("20002"),
                          const QString &messageId = QString(),
                          const QString &replyTo = QString())
    {
        RawEvent e;
        e.id = id;
        e.channel = Channel::kQQGroup;
        e.sessionId = QStringLiteral("30001");
        e.messageId = messageId.isEmpty() ? QString::number(id) : messageId;
        e.senderId = senderId;
        e.senderName = QStringLiteral("群友");
        e.eventType = QStringLiteral("message");
        e.content = QStringLiteral("消息");
        e.occurredAt = time;
        if (!replyTo.isEmpty()) {
            e.payloadJson = QStringLiteral(
                R"({"post_type":"message","message_type":"group","group_id":30001,"self_id":10001,"sender":{"user_id":20002},"message":[{"type":"reply","data":{"id":"%1"}},{"type":"text","data":{"text":"回复"}}]})")
                .arg(replyTo);
        }
        return e;
    }

    static InteractionConfig config()
    {
        InteractionConfig c;
        c.gapSeconds = 90;
        c.maxEvents = 20;
        c.maxMinutes = 10;
        return c;
    }

private slots:
    void splitsOnGap()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const QList<RawEvent> events = {
            event(1, start),
            event(2, start.addSecs(30)),
            event(3, start.addSecs(130))
        };
        const auto drafts = InteractionSegmenter::segment(events, config());
        QCOMPARE(drafts.size(), 2);
        QCOMPARE(drafts.at(0).sourceEventIds.size(), 2);
        QCOMPARE(drafts.at(1).sourceEventIds.size(), 1);
    }

    void splitsOnMaxEvents()
    {
        InteractionConfig c = config();
        c.maxEvents = 2;
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const QList<RawEvent> events = {
            event(1, start), event(2, start.addSecs(10)),
            event(3, start.addSecs(20))
        };
        const auto drafts = InteractionSegmenter::segment(events, c);
        QCOMPARE(drafts.size(), 2);
        QCOMPARE(drafts.at(0).sourceEventIds.size(), 2);
        QCOMPARE(drafts.at(1).sourceEventIds.size(), 1);
    }

    void explicitReplyJoinsSegment()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const QList<RawEvent> events = {
            event(1, start, QStringLiteral("20002"), QStringLiteral("m1")),
            event(2, start.addSecs(200), QStringLiteral("20003"),
                  QStringLiteral("m2"), QStringLiteral("m1"))
        };
        const auto drafts = InteractionSegmenter::segment(events, config());
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(drafts.at(0).sourceEventIds.size(), 2);
        QCOMPARE(drafts.at(0).segmentationMethod, QStringLiteral("explicit_link"));
        QCOMPARE(drafts.at(0).confidence, 0.8);
    }

    void participantsAreDeduplicated()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const QList<RawEvent> events = {
            event(1, start, QStringLiteral("20002")),
            event(2, start.addSecs(10), QStringLiteral("20002")),
            event(3, start.addSecs(20), QStringLiteral("20003"))
        };
        const auto drafts = InteractionSegmenter::segment(events, config());
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(drafts.at(0).participants.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestInteractionSegmenter)
#include "tst_interactionsegmenter.moc"
