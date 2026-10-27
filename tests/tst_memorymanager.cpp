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

            // 版本推进到 v3
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 4);

            // 列扩展（v2 渠道 + v3 轮次）
            const QStringList historyCols = raw.columns("chat_history");
            QVERIFY2(historyCols.contains("channel"), qPrintable(historyCols.join(",")));
            QVERIFY(historyCols.contains("session_id"));
            QVERIFY(historyCols.contains("speaker"));
            QVERIFY(historyCols.contains("sensitive"));
            QVERIFY(historyCols.contains("turn_kind"));
            QVERIFY(historyCols.contains("trigger_event_ids"));
            QVERIFY(historyCols.contains("subject"));
            // v3：user_input 放开 NOT NULL
            QCOMPARE(raw.scalar("SELECT \"notnull\" FROM pragma_table_info('chat_history') "
                                "WHERE name='user_input'").toInt(), 0);

            const QStringList summaryCols = raw.columns("long_term_summary");
            QVERIFY(summaryCols.contains("channel"));
            QVERIFY(summaryCols.contains("session_id"));

            const QStringList episodicCols = raw.columns("episodic_memory");
            QVERIFY(episodicCols.contains("origin"));
            QVERIFY(episodicCols.contains("visibility"));
            QVERIFY(episodicCols.contains("subject"));
            QVERIFY(episodicCols.contains("session_id"));
            QCOMPARE(raw.scalar("SELECT visibility FROM episodic_memory LIMIT 1").toString(),
                     QStringLiteral("private"));
            QCOMPARE(raw.scalar("SELECT subject FROM episodic_memory LIMIT 1").toString(),
                     QStringLiteral("user"));
            QCOMPARE(raw.scalar("SELECT session_id FROM episodic_memory LIMIT 1").toString(),
                     QStringLiteral("default"));

            // 存量行按默认值归入 pet/default，数据不丢
            QCOMPARE(raw.scalar("SELECT user_input FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("旧输入"));
            QCOMPARE(raw.scalar("SELECT channel FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("pet"));
            QCOMPARE(raw.scalar("SELECT session_id FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("default"));
            QCOMPARE(raw.scalar("SELECT sensitive FROM chat_history LIMIT 1").toInt(), 0);
            QCOMPARE(raw.scalar("SELECT turn_kind FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("reply"));
            QCOMPARE(raw.scalar("SELECT subject FROM chat_history LIMIT 1").toString(),
                     QStringLiteral("user"));
            QCOMPARE(raw.scalar("SELECT trigger_event_ids IS NULL FROM chat_history LIMIT 1").toInt(), 1);
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

            // raw_messages → raw_events（旧表删除，新表 15 列）
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='raw_messages'").toInt(), 0);
            const QStringList rawCols = raw.columns("raw_events");
            QCOMPARE(rawCols.size(), 15);
            QVERIFY(rawCols.contains("message_id"));
            QVERIFY(rawCols.contains("sender_id"));
            QVERIFY(rawCols.contains("event_type"));
            QVERIFY(rawCols.contains("visibility"));
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_events").toInt(), 0);

            // 蒸馏游标表
            QVERIFY(raw.columns("memory_cursor").contains("last_event_id"));
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
            // 二次启动：user_version 已是 3，不再执行迁移，数据不重复
            MemoryManager second(dbPath);
            RawDb raw(dbPath, "verify2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 4);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM chat_history").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM relationship_state").toInt(), 2);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM long_term_summary").toInt(), 1);
        }
    }

    void freshDatabaseIsV4()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("fresh.db");

        {
            MemoryManager mm(dbPath);
            RawDb raw(dbPath, "fresh");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 4);
            QVERIFY(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('raw_events')").toInt() > 0);
            QVERIFY(raw.scalar("SELECT COUNT(*) FROM pragma_table_info('memory_cursor')").toInt() > 0);

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
        QCOMPARE(raw.scalar("SELECT turn_kind FROM chat_history").toString(), QStringLiteral("reply"));
        QCOMPARE(raw.scalar("SELECT subject FROM chat_history").toString(), QStringLiteral("qq:10001"));
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

    void migratesFromV2PreservingData()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("v2.db");
        QVERIFY(createLegacyV1Db(dbPath));
        {
            RawDb raw(dbPath, "v2setup");
            QVERIFY(raw.open());
            const QStringList ddl = {
                "ALTER TABLE chat_history ADD COLUMN channel TEXT NOT NULL DEFAULT 'pet'",
                "ALTER TABLE chat_history ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'",
                "ALTER TABLE chat_history ADD COLUMN speaker TEXT",
                "ALTER TABLE chat_history ADD COLUMN sensitive INTEGER NOT NULL DEFAULT 0",
                "CREATE INDEX idx_history_session ON chat_history(channel, session_id)",
                "ALTER TABLE long_term_summary ADD COLUMN channel TEXT NOT NULL DEFAULT 'pet'",
                "ALTER TABLE long_term_summary ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'",
                "ALTER TABLE episodic_memory ADD COLUMN origin TEXT NOT NULL DEFAULT 'pet'",
                "CREATE TABLE relationship_state_v2 ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  subject TEXT NOT NULL DEFAULT 'user',"
                "  dimension TEXT NOT NULL,"
                "  value REAL NOT NULL,"
                "  updated_at DATETIME DEFAULT (datetime('now','localtime')),"
                "  UNIQUE(subject, dimension))",
                "INSERT INTO relationship_state_v2 (subject, dimension, value) "
                "SELECT 'user', dimension, value FROM relationship_state",
                "DROP TABLE relationship_state",
                "ALTER TABLE relationship_state_v2 RENAME TO relationship_state",
                "CREATE TABLE raw_messages ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  channel TEXT NOT NULL,"
                "  session_id TEXT NOT NULL,"
                "  speaker TEXT,"
                "  content TEXT NOT NULL,"
                "  timestamp DATETIME DEFAULT (datetime('now','localtime')),"
                "  sensitive INTEGER NOT NULL DEFAULT 0,"
                "  consolidated INTEGER NOT NULL DEFAULT 0)",
                "INSERT INTO raw_messages (channel, session_id, speaker, content, sensitive) "
                "VALUES ('qq_group', 'group:1', '小明', '旧群消息', 1)",
                "PRAGMA user_version = 2",
            };
            for (const QString &sql : ddl) {
                QVERIFY2(raw.exec(sql), qPrintable(sql + " | " + raw.lastError()));
            }
        }

        MemoryManager mm(dbPath);
        Q_UNUSED(mm);
        RawDb raw(dbPath, "v2verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 4);
        QCOMPARE(raw.scalar("SELECT user_input FROM chat_history").toString(), QStringLiteral("旧输入"));
        QCOMPARE(raw.scalar("SELECT turn_kind FROM chat_history").toString(), QStringLiteral("reply"));
        QCOMPARE(raw.scalar("SELECT subject FROM chat_history").toString(), QStringLiteral("user"));
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='raw_messages'").toInt(), 0);
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_events").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT platform FROM raw_events").toString(), QStringLiteral("qq"));
        QCOMPARE(raw.scalar("SELECT channel FROM raw_events").toString(), QStringLiteral("qq_group"));
        QCOMPARE(raw.scalar("SELECT sender_name FROM raw_events").toString(), QStringLiteral("小明"));
        QCOMPARE(raw.scalar("SELECT event_type FROM raw_events").toString(), QStringLiteral("message"));
        QCOMPARE(raw.scalar("SELECT visibility FROM raw_events").toString(), QStringLiteral("private"));
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM memory_cursor").toInt(), 0);
    }

    void proactiveTurnAllowsNullInput()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("proactive.db");
        MemoryManager mm(dbPath);

        RequestMeta meta;
        meta.turnKind = QStringLiteral("proactive");
        meta.subject = QStringLiteral("user");
        QVERIFY(mm.saveQATurn(QString(), "[]", {}, meta));

        RawDb raw(dbPath, "proactive");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT user_input IS NULL FROM chat_history").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT turn_kind FROM chat_history").toString(),
                 QStringLiteral("proactive"));
        QCOMPARE(raw.scalar("SELECT subject FROM chat_history").toString(),
                 QStringLiteral("user"));

        const QList<HistoryTurn> turns = mm.getHistoryTurn(10, Channel::kPet, Channel::kDefaultSession);
        QCOMPARE(turns.size(), 1);
        QCOMPARE(turns.at(0).turnKind, QStringLiteral("proactive"));
        QVERIFY(turns.at(0).userInput.isEmpty());
    }

    void appendRawEventDedupAndFields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("raw.db");
        MemoryManager mm(dbPath);

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup, QStringLiteral("group:123"),
                                  QStringLiteral("message"), QStringLiteral("今晚开黑吗"),
                                  QStringLiteral("m-1001"), QStringLiteral("in"),
                                  QStringLiteral("10001"), QStringLiteral("小明")));
        // 同 message_id 重复上报 → INSERT OR IGNORE
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup, QStringLiteral("group:123"),
                                  QStringLiteral("message"), QStringLiteral("重复"),
                                  QStringLiteral("m-1001")));

        RawDb raw(dbPath, "raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_events").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT sender_id FROM raw_events").toString(), QStringLiteral("10001"));
        QCOMPARE(raw.scalar("SELECT sender_name FROM raw_events").toString(), QStringLiteral("小明"));
        QCOMPARE(raw.scalar("SELECT event_type FROM raw_events").toString(), QStringLiteral("message"));
        QCOMPARE(raw.scalar("SELECT direction FROM raw_events").toString(), QStringLiteral("in"));
        QCOMPARE(raw.scalar("SELECT visibility FROM raw_events").toString(), QStringLiteral("private"));
        QCOMPARE(raw.scalar("SELECT processed FROM raw_events").toInt(), 0);
    }

    void episodicOriginAndVisibilityFilters()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("episodic.db"));

        QVERIFY(mm.addEpisodicMemory(QStringLiteral("桌宠私密"), QStringLiteral("event"), 0.5,
                                     QDateTime(), QString(), QStringLiteral("pet"),
                                     QStringLiteral("private"), QStringLiteral("user"),
                                     QStringLiteral("default")));
        QVERIFY(mm.addEpisodicMemory(QStringLiteral("群A记忆"), QStringLiteral("event"), 0.6,
                                     QDateTime(), QString(), QStringLiteral("qq_group"),
                                     QStringLiteral("channel"), QStringLiteral("qq:10001"),
                                     QStringLiteral("group:123")));
        QVERIFY(mm.addEpisodicMemory(QStringLiteral("群B记忆"), QStringLiteral("event"), 0.7,
                                     QDateTime(), QString(), QStringLiteral("qq_group"),
                                     QStringLiteral("channel"), QStringLiteral("qq:10001"),
                                     QStringLiteral("group:999")));

        QCOMPARE(mm.getActiveEpisodicMemories().size(), 3);
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QStringLiteral("qq_group")).size(), 2);
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QString(), QStringLiteral("private")).size(), 1);
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QStringLiteral("pet"),
                                              QStringLiteral("channel")).size(), 0);

        // v4 会话隔离：同渠道不同群互不可见
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QStringLiteral("qq_group"), QString(),
                                              QStringLiteral("group:123")).size(), 1);
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QStringLiteral("qq_group"), QString(),
                                              QStringLiteral("group:999")).size(), 1);
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QStringLiteral("qq_group"), QString(),
                                              QStringLiteral("group:000")).size(), 0);
        QCOMPARE(mm.getActiveEpisodicMemories(0.2, 20, QStringLiteral("qq_group"),
                                              QStringLiteral("channel"),
                                              QStringLiteral("group:123")).size(), 1);
    }

    void memoryCursorUpsert()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("cursor_table.db"));

        QCOMPARE(mm.getMemoryCursorEventId(Channel::kPet, Channel::kDefaultSession), qlonglong(0));
        QVERIFY(mm.updateMemoryCursor(Channel::kPet, Channel::kDefaultSession, 42, 7));
        QCOMPARE(mm.getMemoryCursorEventId(Channel::kPet, Channel::kDefaultSession), qlonglong(42));
        QVERIFY(mm.updateMemoryCursor(Channel::kPet, Channel::kDefaultSession, 43, 8));
        QCOMPARE(mm.getMemoryCursorEventId(Channel::kPet, Channel::kDefaultSession), qlonglong(43));
        QCOMPARE(mm.getMemoryCursorEventId(Channel::kQQGroup, QStringLiteral("group:123")), qlonglong(0));
    }
};

QTEST_GUILESS_MAIN(TestMemoryManager)
#include "tst_memorymanager.moc"
