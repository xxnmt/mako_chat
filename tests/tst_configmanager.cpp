#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTime>

#include "configmanager.h"
#include "debuglog.h"
#include "utterance.h"

class TestConfigManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // ConfigManager 以 applicationDirPath/app_data/config 为配置目录；
        // 测试可执行文件位于 <build>/tests，因此不会影响正式配置。
        m_configDir = QCoreApplication::applicationDirPath() + "/app_data/config";
        QVERIFY(QDir().mkpath(m_configDir));
        m_identityPath = m_configDir + "/identity.json";

        QJsonObject root;

        QJsonObject self;
        self.insert("qq", QJsonArray{"10001", "10002"});
        QJsonObject master;
        master.insert("qq", QJsonArray{"20001", "20002"});
        QJsonObject identity;
        identity.insert("self", self);
        identity.insert("master", master);
        root.insert("identity", identity);

        QJsonObject zone;
        zone.insert("channel", Channel::kQQGroup);
        zone.insert("session_id", "group:123456");
        QJsonObject memory;
        memory.insert("blind_zones", QJsonArray{zone});
        root.insert("memory", memory);

        QJsonObject appearance;
        appearance.insert("follow_external_mood", false);
        root.insert("appearance", appearance);

        QJsonObject quiet;
        quiet.insert("start", "22:30");
        quiet.insert("end", "07:00");
        QJsonObject target1;
        target1.insert("channel", Channel::kPet);
        target1.insert("session_id", Channel::kDefaultSession);
        QJsonObject target2;
        target2.insert("channel", Channel::kQQGroup);
        target2.insert("session_id", "group:123456");
        QJsonObject schedule;
        schedule.insert("at", "07:30");
        schedule.insert("type", "fixed");

        QJsonObject proactive;
        proactive.insert("enabled", true);
        proactive.insert("daily_limit", 5);
        proactive.insert("cooldown_seconds", 600);
        proactive.insert("idle_seconds", 900);
        proactive.insert("quiet_hours", quiet);
        proactive.insert("targets", QJsonArray{target1, target2});
        proactive.insert("schedule", QJsonArray{schedule});
        root.insert("proactive", proactive);

        QFile file(m_identityPath);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "无法写入 identity.json");
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }

    void cleanupTestCase()
    {
        QFile::remove(m_identityPath);
        QFile::remove(m_configDir + "/setting.json");
    }

    void parsesQQAccountLists()
    {
        ConfigManager &cfg = ConfigManager::instance();
        QCOMPARE(cfg.getSelfQQAccounts(), (QStringList{"10001", "10002"}));
        QCOMPARE(cfg.getMasterQQAccounts(), (QStringList{"20001", "20002"}));
    }

    void masterLookupIsExact()
    {
        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.isMasterQQ("20001"));
        QVERIFY(cfg.isMasterQQ("20002"));
        QVERIFY(!cfg.isMasterQQ("10001")); // self 不算 master
        QVERIFY(!cfg.isMasterQQ("99999"));
        QVERIFY(!cfg.isMasterQQ(""));      // 空 uin 不应误判
    }

    void blindZoneLooksUpByDoubleKey()
    {
        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.isBlindZone(Channel::kQQGroup, "group:123456"));
        QVERIFY(!cfg.isBlindZone(Channel::kQQGroup, "group:000000"));
        QVERIFY(!cfg.isBlindZone(Channel::kPet, "group:123456")); // 渠道不同
    }

    void parsesAppearanceFlag()
    {
        QVERIFY(!ConfigManager::instance().getFollowExternalMood());
    }

    void parsesProactiveConfig()
    {
        const ProactiveConfig cfg = ConfigManager::instance().getProactiveConfig();
        QVERIFY(cfg.enabled);
        QCOMPARE(cfg.dailyLimit, 5);
        QCOMPARE(cfg.cooldownSeconds, 600);
        QCOMPARE(cfg.idleSeconds, 900);
        QCOMPARE(cfg.quietStart, QStringLiteral("22:30"));
        QCOMPARE(cfg.quietEnd, QStringLiteral("07:00"));
        QCOMPARE(cfg.targets.size(), 2);
        QCOMPARE(cfg.targets.at(0).first, Channel::kPet);
        QCOMPARE(cfg.targets.at(0).second, Channel::kDefaultSession);
        QCOMPARE(cfg.targets.at(1).first, Channel::kQQGroup);
        QCOMPARE(cfg.schedule.size(), 1);
        QCOMPARE(cfg.schedule.at(0).at, QStringLiteral("07:30"));
        QCOMPARE(cfg.schedule.at(0).type, QStringLiteral("fixed"));
    }

    void shortMemoryLengthHasDefault()
    {
        // 测试目录下可能自动生成默认 setting.json，取默认值 15
        QCOMPARE(ConfigManager::instance().getShortMemoryLength(), 15);
    }

    void parsesTimeAndSummaryConfig()
    {
        QJsonObject memory;
        memory.insert("short_term_length", 12);
        memory.insert("summary_length", 8);
        QJsonObject time;
        time.insert("night_start", "21:30");
        time.insert("night_end", "06:30");
        time.insert("blush_lasting", 45);
        time.insert("emotion_lasting", 20);
        time.insert("distance_lasting", 25);
        QJsonObject root;
        root.insert("memory", memory);
        root.insert("time", time);
        writeSettingFile(root);

        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.loadSetting());
        QCOMPARE(cfg.getShortMemoryLength(), 12);
        QCOMPARE(cfg.getSummaryMemoryLength(), 8);
        QCOMPARE(cfg.getNightStartTime(), QTime(21, 30));
        QCOMPARE(cfg.getNightEndTime(), QTime(6, 30));
        QCOMPARE(cfg.getBlushLasting(), 45);
        QCOMPARE(cfg.getEmotionLasting(), 20);
        QCOMPARE(cfg.getDistanceLasting(), 25);
    }

    void timeConfigFallsBackToDefaults()
    {
        QJsonObject memory;
        memory.insert("short_term_length", 15);
        QJsonObject root;
        root.insert("memory", memory);
        writeSettingFile(root);

        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.loadSetting());
        QCOMPARE(cfg.getSummaryMemoryLength(), 15);
        QCOMPARE(cfg.getNightStartTime(), QTime(22, 0));
        QCOMPARE(cfg.getNightEndTime(), QTime(7, 0));
        QCOMPARE(cfg.getBlushLasting(), 30);
        QCOMPARE(cfg.getEmotionLasting(), 15);
        QCOMPARE(cfg.getDistanceLasting(), 15);
    }

    void configValuesAreClamped()
    {
        QJsonObject time;
        time.insert("blush_lasting", 0);
        time.insert("emotion_lasting", 99999);
        time.insert("distance_lasting", -5);
        QJsonObject root;
        root.insert("time", time);
        writeSettingFile(root);

        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.loadSetting());
        QCOMPARE(cfg.getBlushLasting(), 1);
        QCOMPARE(cfg.getEmotionLasting(), 3600);
        QCOMPARE(cfg.getDistanceLasting(), 1);
    }

    void memoryPolicyDefaultsAreConservative()
    {
        writeSettingFile(QJsonObject{});

        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.loadSetting());
        const MemoryPolicyConfig policy = cfg.getMemoryPolicyConfig();
        QVERIFY(policy.shortContextEnabled);
        QVERIFY(policy.longTermMemoryEnabled);
        QCOMPARE(policy.policyVersion, QStringLiteral("r3"));
        QVERIFY(policy.privacyRules.isEmpty());
        QVERIFY(!cfg.getDebugFullLogs());
        QVERIFY(!debugFullLogsEnabled());

        const QQGroupMemoryConfig groups = cfg.getQQGroupMemoryConfig();
        QVERIFY(!groups.enabled);
        QVERIFY(groups.groupAllow.isEmpty());
        QVERIFY(groups.senderAllowByGroup.isEmpty());
        QCOMPARE(groups.retentionDays, 30);
    }

    void parsesMemoryPolicyAndGroupMemory()
    {
        QJsonObject memory;
        memory.insert("short_context_enabled", false);
        memory.insert("long_term_memory_enabled", false);
        memory.insert("debug_full_logs", true);
        memory.insert("privacy_policy_version", QStringLiteral("test-r3"));

        QJsonObject rule;
        rule.insert("id", QStringLiteral("phone"));
        rule.insert("type", QStringLiteral("pattern"));
        rule.insert("value", QStringLiteral("\\b1\\d{10}\\b"));
        rule.insert("action", QStringLiteral("redact"));
        QJsonObject blockRule;
        blockRule.insert("id", QStringLiteral("real-name"));
        blockRule.insert("type", QStringLiteral("exact"));
        blockRule.insert("value", QStringLiteral("张三"));
        blockRule.insert("action", QStringLiteral("block"));
        memory.insert("privacy_rules", QJsonArray{rule, blockRule});

        QJsonObject qq;
        qq.insert("group_memory_enabled", true);
        qq.insert("group_memory_allow", QJsonArray{30001});
        qq.insert("group_retention_days", 7);
        QJsonObject modes;
        modes.insert("30001", QStringLiteral("group_authorized"));
        qq.insert("group_memory_mode", modes);
        QJsonObject senderAllow;
        senderAllow.insert("30001", QJsonArray{20002, 20003});
        qq.insert("group_sender_allowlist", senderAllow);

        QJsonObject root;
        root.insert("memory", memory);
        root.insert("qq", qq);
        writeSettingFile(root);

        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.loadSetting());

        const MemoryPolicyConfig policy = cfg.getMemoryPolicyConfig();
        QVERIFY(!policy.shortContextEnabled);
        QVERIFY(!policy.longTermMemoryEnabled);
        QVERIFY(cfg.getDebugFullLogs());
        QVERIFY(debugFullLogsEnabled());
        QCOMPARE(policy.policyVersion, QStringLiteral("test-r3"));
        QCOMPARE(policy.privacyRules.size(), 2);
        QCOMPARE(policy.privacyRules.at(0).id, QStringLiteral("phone"));
        QCOMPARE(policy.privacyRules.at(1).action, QStringLiteral("block"));

        const QQGroupMemoryConfig groups = cfg.getQQGroupMemoryConfig();
        QVERIFY(groups.enabled);
        QCOMPARE(groups.groupAllow, (QList<qlonglong>{30001}));
        QCOMPARE(groups.retentionDays, 7);
        QCOMPARE(groups.modeByGroup.value(QStringLiteral("30001")),
                 QQGroupMemoryMode::GroupAuthorized);
        QCOMPARE(groups.senderAllowByGroup.value(QStringLiteral("30001")).size(), 2);

        const QQConfig qqConfig = cfg.getQQConfig();
        QVERIFY(qqConfig.groupMemory.enabled);
        QCOMPARE(qqConfig.groupMemory.groupAllow, (QList<qlonglong>{30001}));

        // save/reload 双向一致
        QVERIFY(cfg.saveSetting());
        QVERIFY(cfg.loadSetting());
        QCOMPARE(cfg.getMemoryPolicyConfig().privacyRules.size(), 2);
        QCOMPARE(cfg.getQQGroupMemoryConfig().retentionDays, 7);
    }

private:
    void writeSettingFile(const QJsonObject &root)
    {
        QFile file(m_configDir + "/setting.json");
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "无法写入 setting.json");
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }

    QString m_configDir;
    QString m_identityPath;
};

QTEST_GUILESS_MAIN(TestConfigManager)
#include "tst_configmanager.moc"
