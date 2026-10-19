#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "sessionrouter.h"
#include "memorymanager.h"
#include "configmanager.h"
#include "utterance.h"

class TestSessionRouter : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // 保证本进程内 identity.json 不存在，验证默认身份行为
        QFile::remove(QCoreApplication::applicationDirPath() + "/app_data/config/identity.json");
    }

    void routesInputWithMetaAndBucketedHistory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("router.db"));

        RequestMeta petMeta;
        petMeta.channel = Channel::kPet;
        petMeta.sessionId = Channel::kDefaultSession;
        petMeta.speaker = QStringLiteral("欧尼酱");
        QVERIFY(mm.saveQATurn("历史输入", "[]", {}, petMeta));

        RequestMeta groupMeta;
        groupMeta.channel = Channel::kQQGroup;
        groupMeta.sessionId = "group:123";
        groupMeta.speaker = QStringLiteral("小明");
        QVERIFY(mm.saveQATurn("群聊历史", "[]", {}, groupMeta));

        SessionRouter router(&mm);

        struct Captured {
            int count = 0;
            QString text;
            QList<HistoryTurn> history;
            RequestMeta meta;
        } captured;

        connect(&router, &SessionRouter::requestReply, this,
                [&captured](const QString &text, const QList<HistoryTurn> &history,
                            const RequestMeta &meta) {
                    ++captured.count;
                    captured.text = text;
                    captured.history = history;
                    captured.meta = meta;
                });

        // 桌宠输入：历史只含 pet/default 桶
        router.submitInput(Channel::kPet, Channel::kDefaultSession,
                           QStringLiteral("欧尼酱"), QStringLiteral("你好"));
        QCOMPARE(captured.count, 1);
        QCOMPARE(captured.text, QStringLiteral("你好"));
        QCOMPARE(captured.history.size(), 1);
        QCOMPARE(captured.history.at(0).userInput, QStringLiteral("历史输入"));
        QCOMPARE(captured.meta.channel, Channel::kPet);
        QCOMPARE(captured.meta.sessionId, Channel::kDefaultSession);
        QCOMPARE(captured.meta.speaker, QStringLiteral("欧尼酱"));
        QCOMPARE(captured.meta.subject, QStringLiteral("user")); // Phase 0 恒为主人
        QVERIFY(!captured.meta.sensitive);

        // QQ 群输入：历史只含对应会话桶
        captured = Captured{};
        router.submitInput(Channel::kQQGroup, "group:123",
                           QStringLiteral("小明"), QStringLiteral("在吗"));
        QCOMPARE(captured.count, 1);
        QCOMPARE(captured.history.size(), 1);
        QCOMPARE(captured.history.at(0).userInput, QStringLiteral("群聊历史"));
        QCOMPARE(captured.meta.channel, Channel::kQQGroup);
        QCOMPARE(captured.meta.sessionId, QStringLiteral("group:123"));
        QCOMPARE(captured.meta.speaker, QStringLiteral("小明"));
        QCOMPARE(captured.meta.subject, QStringLiteral("user"));
    }

    void emptyInputDoesNotEmit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("empty.db"));
        SessionRouter router(&mm);

        int count = 0;
        connect(&router, &SessionRouter::requestReply, this,
                [&count](const QString &, const QList<HistoryTurn> &, const RequestMeta &) {
                    ++count;
                });

        router.submitInput(Channel::kPet, Channel::kDefaultSession,
                           QStringLiteral("欧尼酱"), QString());
        router.submitInput(Channel::kPet, Channel::kDefaultSession,
                           QStringLiteral("欧尼酱"), QStringLiteral("   "));
        QCOMPARE(count, 0);
    }

    void identityConfigFallsBackToDefaults()
    {
        ConfigManager &cfg = ConfigManager::instance();
        QVERIFY(cfg.getSelfQQAccounts().isEmpty());
        QVERIFY(cfg.getMasterQQAccounts().isEmpty());
        QVERIFY(!cfg.isMasterQQ("12345"));
        QVERIFY(!cfg.isBlindZone(Channel::kQQGroup, "group:123"));
        QVERIFY(cfg.getFollowExternalMood());

        const ProactiveConfig proactive = cfg.getProactiveConfig();
        QVERIFY(!proactive.enabled);
        QCOMPARE(proactive.dailyLimit, 3);
        QCOMPARE(proactive.cooldownSeconds, 900);
        QCOMPARE(proactive.idleSeconds, 1800);
        QCOMPARE(proactive.quietStart, QStringLiteral("23:00"));
        QCOMPARE(proactive.quietEnd, QStringLiteral("08:00"));
    }
};

QTEST_GUILESS_MAIN(TestSessionRouter)
#include "tst_sessionrouter.moc"
