#include <QtTest>

#include "memoryoutputguard.h"
#include "utterance.h"

class TestMemoryOutputGuard : public QObject
{
    Q_OBJECT

private:
    static QList<PrivacyRule> rules()
    {
        return {
            {QStringLiteral("exact-rule"), QStringLiteral("exact"),
             QStringLiteral("绝密原文"), QStringLiteral("block")},
            {QStringLiteral("keyword-rule"), QStringLiteral("keyword"),
             QStringLiteral("secretword"), QStringLiteral("redact")},
            {QStringLiteral("pattern-rule"), QStringLiteral("pattern"),
             QStringLiteral("\\b1\\d{10}\\b"), QStringLiteral("redact")}
        };
    }

    static RequestMeta groupMeta()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("738559702");
        meta.subject = QStringLiteral("qq:20002");
        return meta;
    }

private slots:
    void cleanTextPasses()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("今天天气不错"), groupMeta(), rules());
        QVERIFY(!result.blocked);
        QVERIFY(result.ruleIds.isEmpty());
    }

    void exactRuleBlocks()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("这里有绝密原文"), groupMeta(), rules());
        QVERIFY(result.blocked);
        QVERIFY(result.ruleIds.contains(QStringLiteral("exact-rule")));
        QCOMPARE(result.reason, QStringLiteral("privacy_rule"));
    }

    void keywordRuleIsCaseInsensitive()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("This is SecretWord"), groupMeta(), rules());
        QVERIFY(result.blocked);
        QVERIFY(result.ruleIds.contains(QStringLiteral("keyword-rule")));
    }

    void patternRuleBlocks()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("电话 13800138000"), groupMeta(), rules());
        QVERIFY(result.blocked);
        QVERIFY(result.ruleIds.contains(QStringLiteral("pattern-rule")));
    }

    void forbiddenFactBlocks()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("她提到过蓝色钥匙"),
            groupMeta(), {}, {QStringLiteral("蓝色钥匙")});
        QVERIFY(result.blocked);
        QVERIFY(result.ruleIds.contains(QStringLiteral("forbidden_fact")));
    }

    void groupQqIdentifierBlocks()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("他的号是 qq:123456"), groupMeta(), {});
        QVERIFY(result.blocked);
        QVERIFY(result.ruleIds.contains(QStringLiteral("identity:qq")));
    }

    void privateQqIdentifierIsAllowed()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.subject = QStringLiteral("qq:20002");
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("他的号是 qq:123456"), meta, {});
        QVERIFY(!result.blocked);
    }

    void emptyRulesAndFactsPass()
    {
        const OutputGuardResult result = MemoryOutputGuard::check(
            QStringLiteral("普通回复"), groupMeta(), {}, {});
        QVERIFY(!result.blocked);
    }
};

QTEST_GUILESS_MAIN(TestMemoryOutputGuard)
#include "tst_memoryoutputguard.moc"
