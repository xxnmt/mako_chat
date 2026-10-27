#include <QtTest>

#include "qqreplypolicy.h"
#include "utterance.h"

class TestQQReplyPolicy : public QObject
{
    Q_OBJECT

private:
    static QQInboundMessage makeMessage(const QString &channel,
                                        const QString &senderId,
                                        const QString &sessionId,
                                        bool mentionedSelf = false)
    {
        QQInboundMessage message;
        message.valid = true;
        message.channel = channel;
        message.senderId = senderId;
        message.sessionId = sessionId;
        message.mentionedSelf = mentionedSelf;
        return message;
    }

    static QQReplyPolicy policy()
    {
        QQReplyPolicy p;
        p.privateMode = QStringLiteral("allowlist");
        p.groupMode = QStringLiteral("mention");
        p.privateAllow = {20001LL, 20002LL};
        p.groupAllow = {30001LL};
        p.masterAnyGroup = false;
        return p;
    }

    static QStringList masters()
    {
        return {QStringLiteral("20001")};
    }

private slots:
    void masterPrivateReplies()
    {
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQPrivate, "20001", "20001"),
                     policy(), masters()),
                 QQReplyDecision::Reply);
    }

    void allowedFriendReplies()
    {
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQPrivate, "20002", "20002"),
                     policy(), masters()),
                 QQReplyDecision::Reply);
    }

    void unknownFriendDrops()
    {
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQPrivate, "999999", "999999"),
                     policy(), masters()),
                 QQReplyDecision::Drop);
    }

    void approvedGroupMentionReplies()
    {
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQGroup, "20002", "30001", true),
                     policy(), masters()),
                 QQReplyDecision::Reply);
    }

    void approvedGroupWithoutMentionRecordsOnly()
    {
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQGroup, "20002", "30001", false),
                     policy(), masters()),
                 QQReplyDecision::RecordOnly);
    }

    void unapprovedGroupDrops()
    {
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQGroup, "20002", "30002", true),
                     policy(), masters()),
                 QQReplyDecision::Drop);
    }

    void masterAnyGroupCanReplyWhenEnabled()
    {
        QQReplyPolicy p = policy();
        p.masterAnyGroup = true;
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQGroup, "20001", "30002", true),
                     p, masters()),
                 QQReplyDecision::Reply);
    }

    void groupModeNoneDropsEvenApprovedGroup()
    {
        QQReplyPolicy p = policy();
        p.groupMode = QStringLiteral("none");
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQGroup, "20001", "30001", true),
                     p, masters()),
                 QQReplyDecision::Drop);
    }

    void privateModeAllRepliesUnknown()
    {
        QQReplyPolicy p = policy();
        p.privateMode = QStringLiteral("all");
        QCOMPARE(QQReplyPolicyEngine::decide(
                     makeMessage(Channel::kQQPrivate, "999999", "999999"),
                     p, masters()),
                 QQReplyDecision::Reply);
    }
};

QTEST_GUILESS_MAIN(TestQQReplyPolicy)
#include "tst_qqreplypolicy.moc"
