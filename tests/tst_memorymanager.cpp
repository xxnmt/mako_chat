#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>

#include "memorymanager.h"
#include "utterance.h"

// 独立于 MemoryManager 的原始连接，用于建 v1 库/直接校验表结构。
// 注意：QSqlDatabase 句柄必须在 removeDatabase 前析构（close() 内先释放 m_db）。
class RawDb
{
public:
    RawDb(const QString &path, const QString &tag)
        : m_conn(QStringLiteral("tst_raw_") + tag)
    {
        m_db = QSqlDatabase::addDatabase("QSQLITE", m_conn);
        m_db.setDatabaseName(path);
    }
    ~RawDb() { close(); }

    bool open() { return m_db.open(); }
    bool exec(const QString &sql)
    {
        QSqlQuery q(m_db);
        return q.exec(sql);
    }
    QVariant scalar(const QString &sql)
    {
        QSqlQuery q(m_db);
        if (!q.exec(sql) || !q.next()) {
            qWarning() << "scalar query failed:" << sql << q.lastError().text();
            return {};
        }
        return q.value(0);
    }
    QString lastError() const { return m_db.lastError().text(); }
    QStringList columns(const QString &table)
    {
        QStringList cols;
        QSqlQuery q(m_db);
        if (q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            while (q.next()) {
                cols << q.value(1).toString();
            }
        }
        return cols;
    }
    void close()
    {
        if (m_db.isOpen()) {
            m_db.close();
        }
        m_db = QSqlDatabase();
        if (QSqlDatabase::contains(m_conn)) {
            QSqlDatabase::removeDatabase(m_conn);
        }
    }

private:
    QString m_conn;
    QSqlDatabase m_db;
};

// 构造与 Phase 0 改造前一致的 v1 库，并写入存量数据
static bool createLegacyV1Db(const QString &path)
{
    RawDb db(path, "setup");
    if (!db.open()) {
        qWarning() << "open legacy db failed";
        return false;
    }
    const QStringList ddl = {
        "CREATE TABLE chat_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp DATETIME DEFAULT (datetime('now', 'localtime')),"
        "  user_input TEXT NOT NULL,"
        "  raw_reply TEXT NOT NULL)",
        "CREATE TABLE long_term_summary ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  summary_text TEXT NOT NULL,"
        "  covered_turn_end_id INTEGER NOT NULL,"
        "  source_ids TEXT NOT NULL,"
        "  is_dirty INTEGER DEFAULT 0,"
        "  created_at DATETIME DEFAULT (datetime('now', 'localtime')),"
        "  updated_at DATETIME DEFAULT (datetime('now', 'localtime')))",
        "CREATE TABLE episodic_memory ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  content TEXT NOT NULL,"
        "  event_time DATETIME DEFAULT (datetime('now', 'localtime')),"
        "  importance REAL DEFAULT 0.5,"
        "  type TEXT DEFAULT 'event',"
        "  status TEXT DEFAULT 'active',"
        "  last_accessed DATETIME DEFAULT (datetime('now', 'localtime')),"
        "  last_decay_at DATETIME DEFAULT (datetime('now', 'localtime')),"
        "  source_ids TEXT)",
        "CREATE TABLE relationship_state ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  dimension TEXT NOT NULL UNIQUE,"
        "  value REAL NOT NULL,"
        "  updated_at DATETIME DEFAULT (datetime('now', 'localtime')))",
        "INSERT INTO chat_history (user_input, raw_reply) VALUES ('旧输入', '[]')",
        "INSERT INTO long_term_summary (summary_text, covered_turn_end_id, source_ids) VALUES ('旧摘要', 1, '[]')",
        "INSERT INTO episodic_memory (content) VALUES ('旧事件')",
        "INSERT INTO relationship_state (dimension, value) VALUES ('intimacy', 42.0)",
        "INSERT INTO relationship_state (dimension, value) VALUES ('trust', 55.0)",
        "PRAGMA user_version = 1",
    };
    for (const QString &sql : ddl) {
        if (!db.exec(sql)) {
            qWarning() << "legacy DDL failed:" << sql << db.lastError();
            return false;
        }
    }
    return true;
}

static void makePetTurn(MemoryManager &mm, const QString &userText, const QString &replyText,
                        const QString &channel = Channel::kPet,
                        const QString &sessionId = Channel::kDefaultSession,
                        const QString &speaker = QStringLiteral("欧尼酱"))
{
    RequestMeta meta;
    meta.channel = channel;
    meta.sessionId = sessionId;
    meta.speaker = speaker;
    QVERIFY(mm.saveQATurn(userText, replyText, {}, meta));
}

class TestMemoryManager : public QObject
{
    Q_OBJECT

private slots:
    void legacyMigrationPreservesData()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("legacy.db");
        QVERIFY(createLegacyV1Db(dbPath));

        {
            MemoryManager mm(dbPath);

            RawDb raw(dbPath, "verify");
            QVERIFY(raw.open());

            // 版本推进到 v2
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 2);

            // 列扩展
            const QStringList historyCols = raw.columns("chat_history");
            QVERIFY2(historyCols.contains("channel"), qPrintable(historyCols.join(",")));
            QVERIFY(historyCols.contains("session_id"));
            QVERIFY(historyCols.contains("speaker"));
            QVERIFY(historyCols.contains("sensitive"));

            const QStringList summaryCols = raw.columns("long_term_summary");
            QVERIFY(summaryCols.contains("channel"));
            QVERIFY(summaryCols.contains("session_id"));

            QVERIFY(raw.columns("episodic_memory").contains("origin"));

            // 存量行按默认值归入 pet/default，数据不丢
            QCOMPARE(raw.scalar("SELECT user_input FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("旧输入"));
            QCOMPARE(raw.scalar("SELECT channel FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("pet"));
            QCOMPARE(raw.scalar("SELECT session_id FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("default"));
            QCOMPARE(raw.scalar("SELECT sensitive FROM chat_history LIMIT 1").toInt(), 0);
            QCOMPARE(raw.scalar("SELECT summary_text FROM long_term_summary LIMIT 1").toString(),
                     QStringLiteral("旧摘要"));
            QCOMPARE(raw.scalar("SELECT channel FROM long_term_summary LIMIT 1").toString(),
                     QStringLiteral("pet"));
            QCOMPARE(raw.scalar("SELECT origin FROM episodic_memory LIMIT 1").toString(),
                     QStringLiteral("pet"));

            // relationship_state 重建：存量归 subject='user'，值不变
            QVERIFY(raw.columns("relationship_state").contains("subject"));
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM relationship_state WHERE subject='user'").toInt(), 2);
            QCOMPARE(raw.scalar("SELECT value FROM relationship_state "
                                "WHERE subject='user' AND dimension='intimacy'").toDouble(), 42.0);

            // 新 UNIQUE(subject, dimension) 约束：不同 subject 可同维度共存
            QVERIFY(raw.exec("INSERT INTO relationship_state (subject, dimension, value) "
                             "VALUES ('qq:10001', 'intimacy', 10.0)"));
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM relationship_state").toInt(), 3);

            // raw_messages 只建不写
            const QStringList rawCols = raw.columns("raw_messages");
            QCOMPARE(rawCols.size(), 8);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_messages").toInt(), 0);
        }
    }

    void migrationIsIdempotent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("legacy.db");
        QVERIFY(createLegacyV1Db(dbPath));

        {
            MemoryManager first(dbPath);
        }
        {
            // 二次启动：user_version 已是 2，不再执行迁移，数据不重复
            MemoryManager second(dbPath);
            RawDb raw(dbPath, "verify2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 2);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM chat_history").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM relationship_state").toInt(), 2);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM long_term_summary").toInt(), 1);
        }
    }

    void freshDatabaseIsV2()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("fresh.db");

        {
            MemoryManager mm(dbPath);
            RawDb raw(dbPath, "fresh");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 2);
            QVERIFY(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('raw_messages')").toInt() > 0);

            // 默认关系维度：user / intimacy=30 / trust=30
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM relationship_state WHERE subject='user'").toInt(), 2);
            QCOMPARE(raw.scalar("SELECT value FROM relationship_state "
                                "WHERE subject='user' AND dimension='intimacy'").toDouble(), 30.0);
            QCOMPARE(raw.scalar("SELECT value FROM relationship_state "
                                "WHERE subject='user' AND dimension='trust'").toDouble(), 30.0);
        }
    }

    void historyBucketsAreIsolated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("bucket.db"));

        makePetTurn(mm, "A", "ra");
        makePetTurn(mm, "B", "rb");
        makePetTurn(mm, "C", "rc", Channel::kQQGroup, "group:123", QStringLiteral("小明"));

        const QList<HistoryTurn> pet = mm.getHistoryTurn(10, Channel::kPet, Channel::kDefaultSession);
        QCOMPARE(pet.size(), 2);
        QCOMPARE(pet.at(0).userInput, QStringLiteral("A"));
        QCOMPARE(pet.at(1).userInput, QStringLiteral("B"));

        const QList<HistoryTurn> group = mm.getHistoryTurn(10, Channel::kQQGroup, "group:123");
        QCOMPARE(group.size(), 1);
        QCOMPARE(group.at(0).userInput, QStringLiteral("C"));

        // 同渠道不同会话也隔离
        QCOMPARE(mm.getHistoryTurn(10, Channel::kQQGroup, "group:999").size(), 0);
        QCOMPARE(mm.getHistoryTurn(10, Channel::kPet, "other").size(), 0);

        // 管理界面分页不过滤渠道
        QCOMPARE(mm.getTotalHistoryCount(), 3);
        const QList<HistoryTurn> page = mm.getHistoryTurn(0, 2);
        QCOMPARE(page.size(), 2);
        QCOMPARE(page.at(0).userInput, QStringLiteral("C")); // id DESC
        QCOMPARE(mm.getHistoryTurn(2, 2).size(), 1);
    }

    void metaIsPersistedPerTurn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("meta.db");
        MemoryManager mm(dbPath);

        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = "group:123";
        meta.speaker = QStringLiteral("小明");
        meta.subject = QStringLiteral("qq:10001");
        meta.sensitive = true;
        QVERIFY(mm.saveQATurn("敏感内容", "[]", {}, meta));

        RawDb raw(dbPath, "meta");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT channel FROM chat_history").toString(), Channel::kQQGroup);
        QCOMPARE(raw.scalar("SELECT session_id FROM chat_history").toString(), QStringLiteral("group:123"));
        QCOMPARE(raw.scalar("SELECT speaker FROM chat_history").toString(), QStringLiteral("小明"));
        QCOMPARE(raw.scalar("SELECT sensitive FROM chat_history").toInt(), 1);
    }

    void unsummarizedCursorIsBucketed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("cursor.db");
        MemoryManager mm(dbPath);

        makePetTurn(mm, "A", "ra");
        makePetTurn(mm, "B", "rb");
        makePetTurn(mm, "C", "rc", Channel::kQQGroup, "group:123", QStringLiteral("小明"));

        RawDb raw(dbPath, "cursor");
        QVERIFY(raw.open());
        const qlonglong idA = raw.scalar("SELECT id FROM chat_history WHERE user_input='A'").toLongLong();
        const qlonglong idB = raw.scalar("SELECT id FROM chat_history WHERE user_input='B'").toLongLong();

        // 摘要只覆盖 A（pet/default）
        QVERIFY(mm.addLongTermSummary("summary-1", idA, "[]", Channel::kPet, Channel::kDefaultSession));

        qlonglong lastEnd = -1;
        QList<qlonglong> sourceIds;
        QList<HistoryTurn> petTurns = mm.getUnsummarizedTurns(lastEnd, sourceIds,
                                                             Channel::kPet, Channel::kDefaultSession);
        QCOMPARE(petTurns.size(), 1);
        QCOMPARE(petTurns.at(0).userInput, QStringLiteral("B"));
        QCOMPARE(lastEnd, idB);
        QCOMPARE(sourceIds, QList<qlonglong>{idB});

        // 其他渠道不受 pet 摘要游标影响
        QList<HistoryTurn> groupTurns = mm.getUnsummarizedTurns(lastEnd, sourceIds,
                                                               Channel::kQQGroup, "group:123");
        QCOMPARE(groupTurns.size(), 1);
        QCOMPARE(groupTurns.at(0).userInput, QStringLiteral("C"));

        QList<HistoryTurn> none = mm.getUnsummarizedTurns(lastEnd, sourceIds,
                                                          Channel::kQQGroup, "group:999");
        QVERIFY(none.isEmpty());
        QCOMPARE(lastEnd, qlonglong(-1));
    }

    void summaryBucketsAreIsolated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("summary.db"));

        QVERIFY(mm.addLongTermSummary("宠物摘要", 1, "[]", Channel::kPet, Channel::kDefaultSession));
        QVERIFY(mm.addLongTermSummary("群聊摘要", 2, "[]", Channel::kQQGroup, "group:123"));

        const QList<LongTermSummary> pet = mm.getLatestSummaries(5, Channel::kPet, Channel::kDefaultSession);
        QCOMPARE(pet.size(), 1);
        QCOMPARE(pet.at(0).summaryText, QStringLiteral("宠物摘要"));

        const QList<LongTermSummary> group = mm.getLatestSummaries(5, Channel::kQQGroup, "group:123");
        QCOMPARE(group.size(), 1);
        QCOMPARE(group.at(0).summaryText, QStringLiteral("群聊摘要"));

        QVERIFY(mm.getLatestSummaries(5, Channel::kQQGroup, "group:999").isEmpty());
    }

    void relationshipSubjectsAreIsolated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("rel.db"));

        QVERIFY(mm.upsertRelationshipState("intimacy", 5.0)); // 默认 subject=user，30+5
        QVERIFY(mm.upsertRelationshipState("intimacy", 7.0, "qq:10001")); // 新主体，30+7

        const QList<RelationshipState> userStates = mm.getRelationshipStates();
        QCOMPARE(userStates.size(), 2);
        for (const RelationshipState &s : userStates) {
            QCOMPARE(s.subject, QStringLiteral("user"));
            if (s.dimension == "intimacy") {
                QCOMPARE(s.value, 35.0);
            }
        }

        const QList<RelationshipState> qqStates = mm.getRelationshipStates("qq:10001");
        QCOMPARE(qqStates.size(), 1);
        QCOMPARE(qqStates.at(0).subject, QStringLiteral("qq:10001"));
        QCOMPARE(qqStates.at(0).dimension, QStringLiteral("intimacy"));
        QCOMPARE(qqStates.at(0).value, 37.0);

        QVERIFY(mm.getRelationshipStates("qq:99999").isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestMemoryManager)
#include "tst_memorymanager.moc"
