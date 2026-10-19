#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "configmanager.h"
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

private:
    QString m_configDir;
    QString m_identityPath;
};

QTEST_GUILESS_MAIN(TestConfigManager)
#include "tst_configmanager.moc"
