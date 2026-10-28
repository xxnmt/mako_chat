#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "memorypolicy.h"
#include "utterance.h"

class TestMemoryPolicy : public QObject
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

    static QQReplyPolicy replyPolicy()
    {
        QQReplyPolicy policy;
        policy.privateMode = QStringLiteral("allowlist");
        policy.groupMode = QStringLiteral("mention");
        policy.privateAllow = {20001LL, 20002LL};
        policy.groupAllow = {30001LL};
        return policy;
    }

    static QQGroupMemoryConfig groupMemory()
    {
        QQGroupMemoryConfig config;
        config.enabled = true;
        config.groupAllow = {30001LL};
        config.modeByGroup.insert(QStringLiteral("30001"),
                                  QQGroupMemoryMode::StrictWhitelist);
        config.senderAllowByGroup.insert(QStringLiteral("30001"), {20002LL});
        return config;
    }

    static MemoryPolicyConfig memoryPolicy()
    {
        return MemoryPolicyConfig{};
    }

    static QStringList masters()
    {
        return {QStringLiteral("20001")};
    }

private slots:
    void strictNonWhitelistDrops()
    {
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("99999"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groupMemory(), memoryPolicy(), masters());
        QVERIFY(!decision.persistRaw);
        QVERIFY(!decision.persistChat);
        QVERIFY(!decision.allowReply);
    }

    void strictWhitelistRepliesAndRecords()
    {
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("20002"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groupMemory(), memoryPolicy(), masters());
        QVERIFY(decision.persistRaw);
        QVERIFY(decision.persistChat);
        QVERIFY(decision.allowReply);
        QVERIFY(!decision.ephemeralReply);
    }

    void strictWhitelistWithoutMentionRecordsOnly()
    {
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("20002"),
                        QStringLiteral("30001"), false),
            replyPolicy(), groupMemory(), memoryPolicy(), masters());
        QVERIFY(decision.persistRaw);
        QVERIFY(!decision.persistChat);
        QVERIFY(!decision.allowReply);
    }

    void authorizedNonWhitelistRecordsOnly()
    {
        QQGroupMemoryConfig groups = groupMemory();
        groups.modeByGroup[QStringLiteral("30001")] =
            QQGroupMemoryMode::GroupAuthorized;
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("99999"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groups, memoryPolicy(), masters());
        QVERIFY(decision.persistRaw);
        QVERIFY(!decision.persistChat);
        QVERIFY(!decision.allowReply);
    }

    void authorizedNonWhitelistMentionStillDoesNotReply()
    {
        QQGroupMemoryConfig groups = groupMemory();
        groups.modeByGroup[QStringLiteral("30001")] =
            QQGroupMemoryMode::GroupAuthorized;
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("99999"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groups, memoryPolicy(), masters());
        QVERIFY(!decision.allowReply);
    }

    void authorizedGroupMemoryOffDropsNonWhitelist()
    {
        QQGroupMemoryConfig groups = groupMemory();
        groups.enabled = false;
        groups.modeByGroup[QStringLiteral("30001")] =
            QQGroupMemoryMode::GroupAuthorized;
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("99999"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groups, memoryPolicy(), masters());
        QVERIFY(!decision.persistRaw);
        QVERIFY(!decision.allowReply);
    }

    void longTermOffKeepsShortContext()
    {
        MemoryPolicyConfig memory = memoryPolicy();
        memory.longTermMemoryEnabled = false;
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("20002"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groupMemory(), memory, masters());
        QVERIFY(!decision.persistRaw);
        QVERIFY(decision.persistChat);
        QVERIFY(decision.allowReply);
    }

    void shortContextOffIsEphemeral()
    {
        MemoryPolicyConfig memory = memoryPolicy();
        memory.shortContextEnabled = false;
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("20002"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groupMemory(), memory, masters());
        QVERIFY(decision.persistRaw);
        QVERIFY(!decision.persistChat);
        QVERIFY(decision.allowReply);
        QVERIFY(decision.ephemeralReply);
    }

    void privateOwnerAndAllowlistPersist()
    {
        const auto owner = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQPrivate, QStringLiteral("20001"),
                        QStringLiteral("20001")),
            replyPolicy(), groupMemory(), memoryPolicy(), masters());
        QVERIFY(owner.persistRaw);
        QVERIFY(owner.persistChat);
        QVERIFY(owner.allowReply);

        const auto friendDecision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQPrivate, QStringLiteral("20002"),
                        QStringLiteral("20002")),
            replyPolicy(), groupMemory(), memoryPolicy(), masters());
        QVERIFY(friendDecision.persistRaw);
        QVERIFY(friendDecision.persistChat);
        QVERIFY(friendDecision.allowReply);
    }

    void privateUnknownDropsByDefault()
    {
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQPrivate, QStringLiteral("99999"),
                        QStringLiteral("99999")),
            replyPolicy(), groupMemory(), memoryPolicy(), masters());
        QVERIFY(!decision.persistRaw);
        QVERIFY(!decision.persistChat);
        QVERIFY(!decision.allowReply);
    }

    void privateAllUnknownIsEphemeral()
    {
        QQReplyPolicy policy = replyPolicy();
        policy.privateMode = QStringLiteral("all");
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQPrivate, QStringLiteral("99999"),
                        QStringLiteral("99999")),
            policy, groupMemory(), memoryPolicy(), masters());
        QVERIFY(!decision.persistRaw);
        QVERIFY(!decision.persistChat);
        QVERIFY(decision.allowReply);
        QVERIFY(decision.ephemeralReply);
    }

    void groupOptOutStillAllowsWhitelistReply()
    {
        QQGroupMemoryConfig groups = groupMemory();
        groups.enabled = false;
        const auto decision = QQInboundPolicyEngine::decide(
            makeMessage(Channel::kQQGroup, QStringLiteral("20002"),
                        QStringLiteral("30001"), true),
            replyPolicy(), groups, memoryPolicy(), masters());
        QVERIFY(!decision.persistRaw);
        QVERIFY(decision.persistChat);
        QVERIFY(decision.allowReply);
    }

    void sanitizeRedactsExactKeywordAndPattern()
    {
        QList<PrivacyRule> rules;
        rules.append({QStringLiteral("exact"), QStringLiteral("exact"),
                      QStringLiteral("秘密"), QStringLiteral("redact")});
        rules.append({QStringLiteral("keyword"), QStringLiteral("keyword"),
                      QStringLiteral("Secret"), QStringLiteral("redact")});
        rules.append({QStringLiteral("phone"), QStringLiteral("pattern"),
                      QStringLiteral("\\b1\\d{10}\\b"), QStringLiteral("redact")});

        const SanitizeResult result = SensitiveContentFilter::sanitize(
            QStringLiteral("秘密 secret 电话13800138000"), QString(), rules);
        QVERIFY(!result.blocked);
        QVERIFY(result.redacted);
        QVERIFY(!result.text.contains(QStringLiteral("秘密")));
        QVERIFY(!result.text.contains(QStringLiteral("secret"), Qt::CaseInsensitive));
        QVERIFY(!result.text.contains(QStringLiteral("13800138000")));
    }

    void sanitizeBlockClearsText()
    {
        QList<PrivacyRule> rules;
        rules.append({QStringLiteral("block"), QStringLiteral("exact"),
                      QStringLiteral("绝密"), QStringLiteral("block")});
        const SanitizeResult result = SensitiveContentFilter::sanitize(
            QStringLiteral("这是绝密内容"), QString(), rules);
        QVERIFY(result.blocked);
        QVERIFY(result.text.isEmpty());
        QVERIFY(result.ruleIds.contains(QStringLiteral("block")));
    }

    void sanitizeJsonPayload()
    {
        QList<PrivacyRule> rules;
        rules.append({QStringLiteral("secret"), QStringLiteral("keyword"),
                      QStringLiteral("secret"), QStringLiteral("redact")});
        rules.append({QStringLiteral("phone"), QStringLiteral("pattern"),
                      QStringLiteral("\\b1\\d{10}\\b"), QStringLiteral("redact")});
        const QString payload = QString::fromUtf8(
            R"({"text":"secret","nested":{"phone":"13800138000"}})");
        const SanitizeResult result = SensitiveContentFilter::sanitize(
            QStringLiteral("clean"), payload, rules);
        QVERIFY(!result.blocked);
        QVERIFY(!result.payloadDropped);
        QVERIFY(result.redacted);
        QVERIFY(!result.payload.contains(QStringLiteral("secret")));
        QVERIFY(!result.payload.contains(QStringLiteral("13800138000")));
        QVERIFY(QJsonDocument::fromJson(result.payload.toUtf8()).isObject());
    }

    void sanitizePayloadBlockDropsPayload()
    {
        QList<PrivacyRule> rules;
        rules.append({QStringLiteral("block"), QStringLiteral("exact"),
                      QStringLiteral("绝密"), QStringLiteral("block")});
        const SanitizeResult result = SensitiveContentFilter::sanitize(
            QStringLiteral("clean"),
            QString::fromUtf8(R"({"secret":"绝密"})"), rules);
        QVERIFY(!result.blocked);
        QVERIFY(result.payloadDropped);
        QVERIFY(result.payload.isEmpty());
        QVERIFY(result.redacted);
    }

    void invalidRegexIsIgnored()
    {
        QList<PrivacyRule> rules;
        rules.append({QStringLiteral("bad"), QStringLiteral("pattern"),
                      QStringLiteral("["), QStringLiteral("redact")});
        const SanitizeResult result = SensitiveContentFilter::sanitize(
            QStringLiteral("safe"), QString(), rules);
        QVERIFY(!result.blocked);
        QVERIFY(!result.redacted);
        QCOMPARE(result.text, QStringLiteral("safe"));
    }

    void emptyRulesAreClean()
    {
        const SanitizeResult result = SensitiveContentFilter::sanitize(
            QStringLiteral("hello"), QString::fromUtf8(R"({"a":"b"})"), {});
        QVERIFY(!result.blocked);
        QVERIFY(!result.redacted);
        QCOMPARE(result.text, QStringLiteral("hello"));
        QCOMPARE(result.payload, QStringLiteral(R"({"a":"b"})"));
    }
};

QTEST_GUILESS_MAIN(TestMemoryPolicy)
#include "tst_memorypolicy.moc"
