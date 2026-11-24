#include <QtTest>
#include <QTemporaryDir>
#include "benchmarksnapshot.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QScopeGuard>

#include "configmanager.h"
#include "memorymanager.h"
#include "memoryevidence.h"
#include "memoryretrieval.h"
#include "memoryembedding.h"
#include "memoryindexworker.h"
#include "utterance.h"
#include "privatereview.h"

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
    // 直接插入一条仍 pending 的提醒：既用于"提醒链独占执行"的断言，也用于构造会被拒绝的
    // explicit_time_delegation 依据（v28 起时间委托不能再授权 contact now）。
    // turn 由调用方给唯一负值：既满足 UNIQUE(turn,due) 的幂等键，又不会命中任何真实轮次的删除触发器。
    bool insertReminder(qlonglong turn, const QString &channel, const QString &session,
                        int dueOffsetSeconds, const QString &message, qlonglong *id = nullptr)
    {
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO reminder(turn,channel,session,due,expires,message,state) "
                  "VALUES(?,?,?,unixepoch()+?,unixepoch()+3600,?,'pending')");
        q.addBindValue(turn);
        q.addBindValue(channel);
        q.addBindValue(session);
        q.addBindValue(dueOffsetSeconds);
        q.addBindValue(message);
        if (!q.exec()) {
            qWarning() << "insert reminder failed:" << q.lastError().text();
            return false;
        }
        if (id) *id = q.lastInsertId().toLongLong();
        return true;
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

// 主动联系/分享的配置在 identity.json 的 proactive 段。测试里临时启用，析构时恢复，
// 避免单例配置影响其它用例。
class ProactiveIdentityGuard
{
public:
    ProactiveIdentityGuard()
    {
        auto &config = ConfigManager::instance();
        m_path = QDir(config.getConfigDirPath()).filePath(QStringLiteral("identity.json"));
        QFile file(m_path);
        m_existed = file.exists();
        if (file.open(QIODevice::ReadOnly)) m_original = file.readAll();
    }
    ~ProactiveIdentityGuard()
    {
        auto &config = ConfigManager::instance();
        if (m_existed) {
            config.writeConfigDocument(QStringLiteral("identity.json"),
                                       m_original.isEmpty() ? QByteArray("{}") : m_original, nullptr);
        } else {
            QFile::remove(m_path);
            config.reloadIdentity();
        }
    }
    // private_writing 明确保持 false：主动联系的可达性不应依赖后台写作开关。
    // contactDailyLimit < 0 时不写该键，沿用配置默认值。
    bool enableContact(int contactDailyLimit = -1)
    {
        QJsonObject target{{"channel", Channel::kPet},
                           {"session_id", Channel::kDefaultSession}};
        QJsonObject proactive{{"enabled", true},
                              {"private_writing", false},
                              {"targets", QJsonArray{target}}};
        if (contactDailyLimit >= 0)
            proactive.insert("contact_daily_limit", contactDailyLimit);
        QJsonObject root{{"proactive", proactive}};
        return ConfigManager::instance().writeConfigDocument(
            QStringLiteral("identity.json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact), nullptr);
    }

    // 覆盖 proactive 的其它字段：析构时同样恢复原 identity.json。
    bool writeIdentity(const QByteArray &contents)
    {
        return ConfigManager::instance().writeConfigDocument(
            QStringLiteral("identity.json"), contents, nullptr);
    }

private:
    QString m_path;
    QByteArray m_original;
    bool m_existed = false;
};

// 回顾领取有 60s 冷却：把 thought_review_log 往前挪，模拟冷却已过。
static bool rewindReviewCooldown(const QString &dbPath, const QString &tag)
{
    RawDb clock(dbPath, tag);
    if (!clock.open()) return false;
    return clock.exec(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120"));
}

static RequestMeta concernTurnMeta(const QString &event)
{
    RequestMeta meta;
    meta.eventId = meta.runId = event;
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    return meta;
}

// 领取并提交一条 consider 回顾，形成一个开放关注；contact 非空时同时产生 proposed 联系。
static qint64 seedOpenConcern(MemoryManager &mm, const RequestMeta &meta, const QString &input,
                              const QString &note, const QJsonObject &concern = {},
                              const QJsonObject &contact = {})
{
    const qint64 turn = mm.recordInput(input, meta, true);
    if (turn <= 0 || !mm.recordQuiet(turn, "close")) return 0;
    const auto claim = mm.claimPrivateReflection(12, 60);
    if (claim["id"].toInteger() != turn || claim["version"].toInt() != 1) return 0;
    if (!mm.finishPrivateReflection(turn, 1, "consider", note, contact, {}, {}, QString{}, concern))
        return 0;
    return turn;
}

// 在测试库里插入一条 pending 提醒，返回提醒 id；用于 contact now 的合法"约定"依据。
static qint64 insertPendingReminder(const QString &dbPath, const QString &tag,
                                    const QString &channel = Channel::kPet,
                                    const QString &session = Channel::kDefaultSession,
                                    int dueOffsetSeconds = 300,
                                    const QString &message = QStringLiteral("测试提醒"))
{
    // 全局递减的负数 turn：同一进程内多次插入同一测试库时也不会撞 UNIQUE(turn,due)。
    static qlonglong serial = 0;
    RawDb raw(dbPath, tag);
    if (!raw.open()) return 0;
    qlonglong id = 0;
    if (!raw.insertReminder(--serial, channel, session, dueOffsetSeconds, message, &id)) return 0;
    return id;
}

// contact now 的 explicit_time_delegation 依据对象。v28 起这种依据一律被拒绝
// （时间委托由提醒链执行），保留帮助函数用于"被拒绝"的用例。
static QJsonObject pendingReminderBasis(qint64 reminderId)
{
    return QJsonObject{{"type", QStringLiteral("explicit_time_delegation")},
                       {"refs", QJsonArray{QStringLiteral("reminder/%1").arg(reminderId)}}};
}

// 合法的 new_owner_evidence 依据：造一条"关注来源之后主人真实说过的话"。
// reported 默认给 +1 小时：测试里关注/收口的 reported 都是"当前时刻"，用明显的未来偏移
// 保证"之后"的时序断言不会因用例执行慢而抖动；它保持 received 状态，不参与私下回顾/
// 到期关注的领取（那些查询都要求 reported<=now），避免自己变成需要回顾的新来源。
static qlonglong insertOwnerEvidenceTurn(MemoryManager &mm, const QString &text,
                                         int reportedOffsetSeconds = 3600,
                                         const QString &channel = Channel::kPet,
                                         const QString &sessionId = Channel::kDefaultSession)
{
    static qlonglong serial = 0;
    RequestMeta meta = concernTurnMeta(QStringLiteral("owner-evidence-%1").arg(++serial));
    meta.channel = channel;
    meta.sessionId = sessionId;
    meta.actorId = channel == Channel::kQQPrivate ? sessionId : QString();
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(reportedOffsetSeconds);
    return mm.recordInput(text, meta, true);
}

// 造一个 completed、有产物且尚未分享的活动；返回活动 id（activity_result 依据）。
static qlonglong seedCompletedActivity(MemoryManager &mm, const QString &userText,
                                       const QString &interestJson, const QString &draft)
{
    if (!mm.saveQATurn(userText, interestJson, {})) return 0;
    const auto thought = mm.nextThought(true);
    const qlonglong id = thought["id"].toInteger();
    const int version = thought["version"].toInt();
    if (id <= 0) return 0;
    if (!mm.finishThoughtReview(id, version,
            QJsonObject{{"status", "completed"}, {"reflection", "写好了两句"}, {"draft", draft}}))
        return 0;
    return id;
}

// contact now 的 new_owner_evidence 依据对象。
static QJsonObject ownerEvidenceBasis(qlonglong turn)
{
    return QJsonObject{{"type", QStringLiteral("new_owner_evidence")},
                       {"refs", QJsonArray{QStringLiteral("chat/%1").arg(turn)}}};
}

// contact now 的 activity_result 依据对象。
static QJsonObject activityResultBasis(qlonglong activityId)
{
    return QJsonObject{{"type", QStringLiteral("activity_result")},
                       {"refs", QJsonArray{QStringLiteral("thought/%1").arg(activityId)}}};
}

static QJsonObject contactNow(const QString &reason, const QString &message,
                              const QJsonObject &basis = {})
{
    QJsonObject contact{{"timing", QStringLiteral("now")},
                        {"reason", reason},
                        {"message", message}};
    if (!basis.isEmpty()) contact["basis"] = basis;
    return contact;
}

// 把关注改回"按时间到期"，并重新领取一次（版本 +1）。用于在同一条关注上尝试不同依据。
static QJsonObject reclaimDueConcern(MemoryManager &db, const QString &dbPath, qlonglong turn,
                                     const QString &tag)
{
    {
        RawDb raw(dbPath, tag + QStringLiteral("-due"));
        if (!raw.open()) return {};
        if (!raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                     "WHERE turn_id=%1").arg(turn))) return {};
    }
    if (!rewindReviewCooldown(dbPath, tag + QStringLiteral("-clock"))) return {};
    return db.claimDueConcern(12, 60);
}

class FailingEmbeddingProvider : public MemoryEmbeddingProvider
{
public:
    QString modelId() const override { return QStringLiteral("failing_test"); }
    int dimension() const override { return 8; }
    bool embed(const QString &, QVector<float> *, QString *error = nullptr) const override
    {
        if (error) *error = QStringLiteral("provider unavailable");
        return false;
    }
};

class VectorOnlyEmbeddingProvider : public MemoryEmbeddingProvider
{
public:
    QString modelId() const override { return QStringLiteral("vector_only_test"); }
    int dimension() const override { return 2; }
    bool embed(const QString &, QVector<float> *vector, QString *) const override
    {
        if (!vector) return false;
        *vector = {1.0f, 0.0f};
        return true;
    }
};

class TargetedEmbeddingProvider : public MemoryEmbeddingProvider
{
public:
    QString modelId() const override { return QStringLiteral("targeted_test"); }
    int dimension() const override { return 2; }
    bool embed(const QString &text, QVector<float> *vector, QString *) const override
    {
        if (!vector) return false;
        *vector = (text.contains(QStringLiteral("目标"))
                   || text.contains(QStringLiteral("查询")))
            ? QVector<float>{1.0f, 0.0f}
            : QVector<float>{0.0f, 1.0f};
        return true;
    }
};

class RecordingReranker : public MemoryReranker
{
public:
    QString id() const override { return QStringLiteral("recording"); }
    QList<MemoryEvidence> rerank(const QString &, const QList<MemoryEvidence> &candidates,
                                 int limit, QString *error = nullptr) const override
    {
        seen = candidates;
        if (fail) {
            if (error) *error = QStringLiteral("rerank timeout");
            return {};
        }
        return candidates.mid(0, qMax(0, limit));
    }
    mutable QList<MemoryEvidence> seen;
    bool fail = false;
};

class TestMemoryManager : public QObject
{
    Q_OBJECT

private slots:
    void interruptedV11MigrationRollsBack()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("migration.db");
        RawDb raw(path,"migration");
        QVERIFY(raw.open());
        QVERIFY(raw.exec("CREATE TABLE chat_history(id INTEGER PRIMARY KEY, state_updates TEXT)"));
        QVERIFY(raw.exec("INSERT INTO chat_history VALUES(1,'[]')"));
        QVERIFY(raw.exec("PRAGMA user_version=10"));
        QVERIFY(raw.exec("CREATE TABLE matter_revision(conflict INTEGER)"));
        {
            MemoryManager db(path);
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(),10);
            QVERIFY(!raw.columns("chat_history").contains("cognitive_event"));
            QVERIFY(raw.columns("cognitive_source").isEmpty());
        }
        QVERIFY(raw.exec("DROP TABLE matter_revision"));
        for (int reopen=0; reopen<2; ++reopen) {
            MemoryManager db(path);
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(),28);
            QCOMPARE(raw.scalar("SELECT state_updates FROM chat_history").toString(),QString("[]"));
            QVERIFY(db.matterHeads("user").isEmpty());
        }
    }

    void conversationClosureSurvivesRestartAndReopens()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("phase.db");
        RequestMeta meta;
        {
            MemoryManager db(path);
            meta.eventId = "closed";
            const auto closed = db.recordInput("先聊到这里", meta);
            QVERIFY(closed > 0);
            QVERIFY(db.recordQuiet(closed,"close"));
            QVERIFY(!db.interruptTurn(closed,"late_error"));
        }
        MemoryManager db(path);
        QCOMPARE(db.conversationPhase(meta),QString("close"));
        auto elsewhere = meta;
        elsewhere.channel = Channel::kQQPrivate;
        QVERIFY(db.conversationPhase(elsewhere).isEmpty());
        meta.eventId = "interrupted";
        const auto interrupted = db.recordInput("我想问件事", meta);
        QVERIFY(db.interruptTurn(interrupted, "network_error"));
        QCOMPARE(db.conversationPhase(meta),QString("close"));
        meta.eventId = "reopened";
        const auto reopened = db.recordInput("我还想接着聊", meta);
        QCOMPARE(db.getHistoryTurn(10,meta.channel,meta.sessionId,reopened).size(),2);
        SentenceText answer;
        answer.zhText = "好呀";
        QVERIFY(db.saveQATurn("我还想接着聊","{}",{answer},meta,reopened));
        QVERIFY(!db.saveQATurn("我还想接着聊","{}",{answer},meta,reopened));
        QCOMPARE(db.getTotalHistoryCount(),3);
        const auto turns = db.getHistoryTurn(0,10);
        QCOMPARE(turns[1].state,QString("interrupted"));
        QVERIFY(turns[1].rawReply.isEmpty());
        QVERIFY(turns[2].rawReply.isEmpty());
        QCOMPARE(turns[0].state,QString("handed_off"));
        QVERIFY(db.conversationPhase(meta).isEmpty());
    }

    void responseFailureRecoveryPreservesAudit()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("recovery.db");
        RequestMeta meta;
        meta.eventId = "response-failed";
        {
            MemoryManager db(path);
            QVERIFY(db.recordInput("一条有效输入",meta,true)>0);
            const auto source = db.pendingObservation()["source"].toInteger();
            for (int i=0;i<3;++i) db.deferObservation(source,"empty_content",true);
            RawDb raw(path,"downgrade-recovery");
            QVERIFY(raw.open());
            QVERIFY(raw.exec("DROP TRIGGER forget_cognitive_audit"));
            QVERIFY(raw.exec("DROP TRIGGER forget_answer_audit"));
            QVERIFY(raw.exec("DROP TABLE cognitive_audit"));
            QVERIFY(raw.exec("PRAGMA user_version=13"));
        }
        MemoryManager db(path);
        QVERIFY(!db.pendingObservation().isEmpty());
        RawDb raw(path,"verify-recovery");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_audit WHERE kind='legacy_response_failure'").toInt(),1);
        QCOMPARE(raw.scalar("SELECT json_extract(data,'$.attempts') FROM cognitive_audit").toInt(),3);
        QCOMPARE(raw.scalar("SELECT last_error FROM cognitive_source").toString(),QString("empty_content"));
        QVERIFY(db.deleteTurnByID(1));
        QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_audit").toInt(),0);
    }

    void receiptAndLegacyHistory()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("receipts.db"));
        RequestMeta meta;
        // v12 could store exactly this payload after a coordinator interruption.
        QVERIFY(db.saveQATurn("旧输入",R"({"conversation_action":"defer","sentences":[]})",{},meta));
        QVERIFY(db.conversationPhase(meta).isEmpty());
        meta.eventId = "receive-once";
        const auto id = db.recordInput("新输入",meta,true);
        QVERIFY(id > 0);
        QCOMPARE(db.pendingObservation()["input"].toString(),QString("新输入"));
        QCOMPARE(db.recordInput("重试",meta,true),0);
        QCOMPARE(db.getTotalHistoryCount(),2);
        QVERIFY(db.deleteTurnByID(id));
        QVERIFY(db.pendingObservation().isEmpty());
        QVERIFY(!db.recordQuiet(id,"defer"));
        SentenceText stale;
        stale.zhText = "迟到";
        QVERIFY(!db.saveQATurn("新输入","{}",{stale},meta,id));
        QCOMPARE(db.getTotalHistoryCount(),1);
        meta.persistTurn = false;
        QCOMPARE(db.recordInput("未授权留存",meta,true),0);
        QVERIFY(db.pendingObservation().isEmpty());
    }

    void historyEvidenceHonoursScopeAndRetention()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("evidence.db"));
        RequestMeta local;
        QVERIFY(db.saveQATurn("仅有会话记录","{}",{},local));
        auto sensitive = local;
        sensitive.sensitive = true;
        QVERIFY(db.saveQATurn("敏感记录","{}",{},sensitive));
        auto other = local;
        other.subject = "other";
        QVERIFY(db.saveQATurn("另一位发言者","{}",{},other));
        const auto future = QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs);
        const auto reports = db.historicalReports(local.subject,local.channel,local.sessionId,future,{},false);
        QCOMPARE(reports.size(),1);
        QCOMPARE(reports.first().toObject()["text"].toString(),QString("仅有会话记录"));
        QVERIFY(db.historicalReports(local.subject,local.channel,local.sessionId,future,{},true).isEmpty());
    }

    void revisionReplay()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("replay.db");
        RequestMeta meta;
        meta.eventId = "first";
        meta.channel = Channel::kQQGroup;
        meta.sessionId = "group";
        meta.actorId = "actor";
        auto change = [](const QString &quote) {
            return QJsonObject{{"matter",""},{"version",0},{"topic","活动安排"},{"quote",quote},
                {"status","confirmed"},{"understanding",quote},{"kind","self_report"}};
        };
        auto proposal = [](QJsonArray changes) {
            return QJsonObject{{"outcome","updated"},{"changes",changes}};
        };
        {
            MemoryManager db(path);
            QVERIFY(db.enqueueObservation(meta,"周二读书，周三上课"));
            QVERIFY(db.enqueueObservation(meta,"周二读书，周三上课"));
            const auto source = db.pendingObservation()["source"].toInteger();
            QVERIFY(!db.commitObservation(source,proposal({change("周二读书"),change("不存在")})));
            QVERIFY(db.matterHeads(meta.subject).isEmpty());
            QVERIFY(db.commitObservation(source,proposal({change("周二读书"),change("周三上课")})));
            QCOMPARE(db.matterHeads(meta.subject).size(),2);
            QVERIFY(db.pendingObservation().isEmpty());
            QVERIFY(!db.commitObservation(source,proposal({change("周二读书")})));
        }
        MemoryManager db(path);
        const auto group = meta;
        meta.channel = Channel::kQQPrivate;
        meta.eventId = "revision";
        auto revise = change("课可能延期");
        const auto head = db.matterHeads(meta.subject).last().toObject();
        QCOMPARE(head["quote"].toString(),QString("周三上课"));
        revise["matter"] = head["matter"];
        revise["status"] = "tentative";
        QVERIFY(db.enqueueObservation(meta,"课可能延期"));
        QVERIFY(db.saveQATurn("课可能延期","{}",{},meta));
        const auto source = db.pendingObservation()["source"].toInteger();
        QJsonObject conflict;
        QVERIFY(!db.commitObservation(source,proposal({revise}),nullptr,{},&conflict));
        QCOMPARE(conflict["matter"],head["matter"]);
        QCOMPARE(conflict["expected_version"].toInteger(),0);
        QCOMPARE(conflict["actual_version"].toInteger(),1);
        QCOMPARE(conflict["reason"].toString(),QString("version_mismatch"));
        revise["version"] = 1;
        QVERIFY(db.commitObservation(source,proposal({revise})));
        const auto heads = db.matterHeads(meta.subject);
        QCOMPARE(heads.size(),2);
        QCOMPARE(heads.first().toObject()["version"].toInteger(),source);
        QCOMPARE(heads.first().toObject()["status"].toString(),QString("tentative"));
        const auto projection = QJsonDocument(db.currentUnderstanding(group,"")).toJson();
        QVERIFY(!projection.contains(QString("延期").toUtf8()));
        QVERIFY(projection.contains("avoid_unprompted_followup"));
        meta.eventId = "other";
        meta.subject = "other";
        QVERIFY(db.enqueueObservation(meta,"课可能延期"));
        revise["version"] = heads.first().toObject()["version"];
        const auto other = db.pendingObservation()["source"].toInteger();
        QVERIFY(!db.commitObservation(other,proposal({revise})));
        QCOMPARE(heads.first().toObject()["initial_quote"].toString(),QString("周三上课"));
        QVERIFY(db.commitObservation(other,QJsonObject{{"outcome","no_change"},{"changes",QJsonArray{}}}));
        auto restore = group;
        restore.eventId = "restore";
        QVERIFY(db.enqueueObservation(restore,"还是照旧"));
        const auto restoredSource = db.pendingObservation()["source"].toInteger();
        revise["quote"] = "还是照旧";
        revise["understanding"] = head["quote"];
        revise["status"] = "confirmed";
        QVERIFY(db.commitObservation(restoredSource,proposal({revise})));
        QCOMPARE(db.matterHeads(group.subject).first().toObject()["understanding"],head["quote"]);
        QVERIFY(db.deleteTurnByID(1));
        QCOMPARE(db.matterHeads(group.subject).last().toObject()["version"].toInt(),1);
        QVERIFY(db.clearAllHistory());
        QVERIFY(!db.commitObservation(other,proposal({revise})));
        QVERIFY(db.matterHeads(group.subject).isEmpty());
    }

    void rejectedSourceDoesNotBlockLaterEvents()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("queue.db");
        RequestMeta meta;
        meta.eventId = "malformed";
        {
            MemoryManager db(path);
            QVERIFY(db.enqueueObservation(meta,"前一个事件"));
            const auto source = db.pendingObservation()["source"].toInteger();
            meta.eventId = "next";
            QVERIFY(db.enqueueObservation(meta,"后来一条消息"));
            db.deferObservation(source,"network_error");
            QVERIFY(db.pendingObservation().isEmpty());
            for (int i=0; i<3; ++i)
                db.deferObservation(source,"invalid_proposal",true);
        }
        MemoryManager db(path);
        const auto next = db.pendingObservation();
        QCOMPARE(next["input"].toString(),QString("后来一条消息"));
        QVERIFY(db.commitObservation(next["source"].toInteger(),
            QJsonObject{{"outcome","needs_evidence"},{"changes",QJsonArray{}}}));
        QVERIFY(db.pendingObservation().isEmpty());
        QVERIFY(db.matterHeads(meta.subject).isEmpty());
    }

    void crossChannelCorrectionSurvivesRestart()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("continuity.db");
        RequestMeta group;
        group.channel = Channel::kQQGroup;
        group.actorId = "10101";
        group.sessionId = "20202";
        auto direct = group;
        direct.channel = Channel::kQQPrivate;
        direct.sessionId = group.actorId;
        auto report = [](const QString &quote) {
            return QString::fromUtf8(QJsonDocument(QJsonObject{{"state_updates",
                QJsonArray{QJsonObject{{"topic","脚踝"},{"quote",quote}}}}}).toJson());
        };
        {
            MemoryManager db(path);
            QVERIFY(db.saveQATurn("脚踝还疼", report("脚踝还疼"), {}, group));
            QVERIFY(db.saveQATurn("脚踝好了", report("脚踝好了"), {}, direct));
            QVERIFY(db.saveQATurn("天气真好", report("脚踝又疼了"), {}, direct));
        }
        MemoryManager db(path);
        const auto personal = db.currentUnderstanding(RequestMeta{}, "脚踝");
        QCOMPARE(personal.size(), 1);
        QCOMPARE(personal.first().toObject()["self_report"].toString(), QString("脚踝好了"));
        const auto publicView = db.currentUnderstanding(group, "");
        QCOMPARE(publicView.first().toObject()["avoid_stale_followup"].toString(), QString("脚踝"));
        QVERIFY(!QJsonDocument(publicView).toJson().contains(QString("脚踝好了").toUtf8()));
        group.subject = "qq:99999";
        QVERIFY(db.currentUnderstanding(group, "").isEmpty());
        RawEventWrite legacy;
        legacy.platform = "qq";
        legacy.channel = Channel::kQQPrivate;
        legacy.sessionId = direct.sessionId;
        legacy.senderId = direct.actorId;
        legacy.eventType = "message";
        legacy.content = "脚踝已经恢复";
        QVERIFY(db.appendRawEvent(legacy));
        RetrievalRequest request;
        request.query = "脚踝";
        request.sourceKinds = {"raw"};
        request.accounts = {direct.actorId};
        request.includeOthers = false;
        MemoryRetrievalService retrieval(&db);
        QVERIFY(!retrieval.retrieve(request).evidence.isEmpty());
        request.accounts = {group.subject.mid(3)};
        QVERIFY(retrieval.retrieve(request).evidence.isEmpty());
        QVERIFY(db.clearAllHistory());
        QVERIFY(db.currentUnderstanding(RequestMeta{}, "").isEmpty());
    }

    void evidenceRetainsBothTimelineAndTopicMatches()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("recent.db"));
        RawEventWrite event;
        event.platform = "qq";
        event.channel = Channel::kQQPrivate;
        event.sessionId = event.senderId = "10101";
        event.eventType = "message";
        event.occurredAt = QDateTime::currentDateTime().addDays(-7);
        for (int i = 0; i < 32; ++i) {
            event.content = i < 16 ? QString("排练记录 %1").arg(i) : QString("日常记录 %1").arg(i);
            event.occurredAt = event.occurredAt.addSecs(60);
            event.messageId = QString::number(i);
            QVERIFY(db.appendRawEvent(event));
        }
        event.messageId = "new";
        event.content = "把工具还了";
        event.channel = Channel::kQQGroup;
        event.sessionId = "20002";
        event.occurredAt = event.occurredAt.addSecs(60);
        QVERIFY(db.appendRawEvent(event));
        RetrievalRequest request;
        request.query = "排练";
        request.sourceKinds = {"raw"};
        request.accounts = {event.senderId};
        request.includeOthers = false;
        request.includeUnmatched = true;
        const auto items = MemoryRetrievalService(&db).retrieve(request).evidence;
        QStringList texts;
        for (const auto &item : items)
            texts.append(item.text);
        QVERIFY(texts.contains(event.content));
        QVERIFY(texts.contains(QStringLiteral("排练记录 15")));
    }

    void privateThoughtReviewPersistsAndRespectsBudget()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("thought.db");
        qlonglong id;
        {
            MemoryManager db(path);
            QVERIFY(db.saveQATurn("试试写诗", R"({"personal_interest":"想写雨声"})", {}));
            auto thought = db.nextThought(true);
            id = thought["id"].toInteger();
            QVERIFY(id > 0);
            QVERIFY(db.claimThoughtReview(id,0,1,900));
            QVERIFY(!db.claimThoughtReview(id,0,1,900));
            const QJsonObject review{{"status","completed"},{"reflection","写好了两句"},
                                     {"draft","屋檐轻响，雨在窗上停了一停。"}};
            QVERIFY(db.finishThoughtReview(id,1,review));
            QVERIFY(!db.finishThoughtReview(id,1,review));
        }
        MemoryManager db(path);
        QVERIFY(db.nextThought(true).isEmpty());
        QCOMPARE(db.nextThought(false)["last_review"].toObject()["status"].toString(),QString("completed"));
        QVERIFY(db.deleteTurnByID(id));
        QVERIFY(db.nextThought(false).isEmpty());
    }

    void v16MigrationPreservesUnknownLifecycle()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("v16.db");
        { MemoryManager db(path); }
        {
            RawDb raw(path,"v16-semantics");
            QVERIFY(raw.open());
            QVERIFY(raw.exec("INSERT INTO cognitive_source(id,event,run,subject,channel,session,input,reported,outcome) "
                             "VALUES(1,'old','old','user','pet','default','tonight submit','2026-11-16T12:00:00Z','updated')"));
            QVERIFY(raw.exec("INSERT INTO matter_revision(matter,version,source,topic,status,quote,kind,event_time,understanding) "
                             "VALUES('old',1,1,'report','tentative','tonight submit','self_report','tonight','one report')"));
            QVERIFY(raw.exec("ALTER TABLE matter_revision DROP COLUMN semantics"));
            QVERIFY(raw.exec("PRAGMA user_version=16"));
        }
        {
            MemoryManager db(path);
            const auto head = db.matterHeads("user").first().toObject();
            QCOMPARE(head["status"].toString(),QString("tentative"));
            QCOMPARE(head["semantics"].toObject()["lifecycle"].toString(),QString("unknown"));
            QVERIFY(head["semantics"].toObject()["legacy"].toBool());
            const auto row = db.currentUnderstanding(RequestMeta{},"report").first().toObject();
            QCOMPARE(row["origin"].toObject()["event_time_expression"].toString(),QString("tonight"));
        }
        RawDb raw(path,"v17-semantics");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(),28);
        QCOMPARE(raw.scalar("SELECT count(*) FROM matter_revision").toInt(),1);
        QVERIFY(raw.scalar("SELECT semantics IS NULL FROM matter_revision").toBool());
    }

    void matterLifecycleAndTimeScopeSurviveRevision()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("lifecycle.db");
        RequestMeta meta;
        meta.eventId = "plan";
        meta.occurredAtUtc = QDateTime::fromString("2026-11-16T12:00:00Z",Qt::ISODate);
        {
            MemoryManager db(path);
            QVERIFY(db.enqueueObservation(meta,"submit the report tonight"));
            auto source = db.pendingObservation()["source"].toInteger();
            QJsonObject semantics{{"lifecycle","planned"},{"certainty","confirmed"},
                {"temporal_scope","occurrence"},{"completion_condition","report submitted"},{"time_expression","tonight"}};
            QJsonObject change{{"matter",""},{"version",0},{"topic","report"},{"quote","submit the report tonight"},
                {"kind","self_report"},{"status","tentative"},{"understanding","one report tonight"},
                {"event_time","tonight"},{"semantics",semantics}};
            auto proposal = [&]{return QJsonObject{{"semantics_version",1},{"outcome","updated"},{"changes",QJsonArray{change}}};};
            QVERIFY(db.commitObservation(source,proposal()));
            const auto head = db.matterHeads("user").first().toObject();
            meta.eventId = "done";
            meta.occurredAtUtc = meta.occurredAtUtc.addSecs(3600);
            QVERIFY(db.enqueueObservation(meta,"submitted; I do not know if it was read"));
            source = db.pendingObservation()["source"].toInteger();
            change["matter"] = head["matter"]; change["version"] = head["version"];
            change["quote"] = "submitted"; change["event_time"] = "";
            change["understanding"] = "submitted; recipient reading remains unknown";
            semantics["lifecycle"] = "completed";
            semantics["certainty"] = "tentative";
            semantics["time_expression"] = "next month";
            change["semantics"] = semantics;
            QString failure;
            QVERIFY(!db.commitObservation(source,proposal(),&failure));
            QCOMPARE(failure,QString("unsupported_time_scope"));
            QCOMPARE(db.matterHeads("user").first().toObject()["version"],head["version"]);
            semantics["time_expression"] = "tonight";
            change["semantics"] = semantics;
            change["event_time"] = "today, next week";
            QVERIFY(!db.commitObservation(source,proposal(),&failure));
            QCOMPARE(failure,QString("invalid_event_time"));
            db.deferObservation(source,failure,true);
            {
                RawDb raw(path,"retry-feedback");
                QVERIFY(raw.open());
                QVERIFY(raw.exec("UPDATE cognitive_source SET due=0 WHERE outcome IS NULL"));
            }
            QCOMPARE(db.pendingObservation(source)["previous_error"].toString(),failure);
            change["event_time"] = "";
            QVERIFY(db.commitObservation(source,proposal()));
            const auto row = db.currentUnderstanding(meta,"unrelated").first().toObject();
            QCOMPARE(row["status"].toString(),QString("ended"));
            QCOMPARE(row["semantics"].toObject()["certainty"].toString(),QString("tentative"));
            QCOMPARE(row["semantics"].toObject()["lifecycle"].toString(),QString("completed"));
            QCOMPARE(row["semantics"].toObject()["time_anchor"].toString(),QString("2026-11-16T12:00:00.000Z"));
            QCOMPARE(row["origin"].toObject()["quote"].toString(),QString("submit the report tonight"));
            QCOMPARE(row["self_report"].toObject()["quote"].toString(),QString("submitted"));
            meta.eventId = "missing";
            QVERIFY(db.enqueueObservation(meta,"another report"));
            source = db.pendingObservation()["source"].toInteger();
            change.remove("semantics");
            change["quote"] = "another report";
            change["version"] = db.matterHeads("user").first().toObject()["version"];
            QVERIFY(!db.commitObservation(source,proposal(),&failure));
            QCOMPARE(failure,QString("missing_semantics"));
        }
        MemoryManager reopened(path);
        QCOMPARE(reopened.matterHeads("user").first().toObject()["semantics"].toObject()["lifecycle"].toString(),QString("completed"));
        RawDb raw(path,"lifecycle");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT count(*) FROM matter_revision").toInt(),2);
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(),28);
    }

    void currentUnderstandingRetainsLatestSelfReport()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("self-report.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = "2014366825";
        meta.eventId = "promise";

        QVERIFY(db.enqueueObservation(meta, "我答应茉子，会努力调整作息"));
        const auto firstSource = db.pendingObservation()["source"].toInteger();
        const QJsonObject promise{{"matter",""},{"version",0},{"topic","作息"},
            {"quote","我答应茉子，会努力调整作息"},{"status","confirmed"},
            {"understanding","答应努力调整作息"},{"kind","self_report"}};
        QVERIFY(db.commitObservation(firstSource,
            QJsonObject{{"outcome","updated"},{"changes",QJsonArray{promise}}}));
        const auto head = db.matterHeads("user").first().toObject();

        meta.eventId = "followup";
        QVERIFY(db.enqueueObservation(meta, "还记得我刚才答应你什么了吗"));
        const auto secondSource = db.pendingObservation()["source"].toInteger();
        const QJsonObject inference{{"matter",head["matter"]},{"version",head["version"]},
            {"topic","作息"},{"quote","还记得我刚才答应你什么了吗"},
            {"status","confirmed"},{"understanding","可能在追问之前的承诺"},
            {"kind","inference"}};
        QVERIFY(db.commitObservation(secondSource,
            QJsonObject{{"outcome","updated"},{"changes",QJsonArray{inference}}}));

        const auto records = db.currentUnderstanding(meta, "作息");
        QVERIFY(!records.isEmpty());
        const auto record = records.first().toObject();
        QCOMPARE(record["source_kind"].toString(), QString("inference"));
        const auto selfReport = record["self_report"].toObject();
        QCOMPARE(selfReport["quote"].toString(), QString("我答应茉子，会努力调整作息"));
        QCOMPARE(selfReport["version"].toInteger(), qint64(1));
        QCOMPARE(selfReport["speaker_role"].toString(), QString("user"));
        QVERIFY(selfReport["evidence_ref"].toString().startsWith("source/"));
    }

    void writingResumesFromCommittedDraftAfterInterruption()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("writing.db");
        const QJsonObject chapter{{"status","active"},{"reflection","opening written"},
                                  {"draft","first paragraph"},{"next_step","second paragraph"}};
        qlonglong id = 0;
        int interruptedVersion = 0;
        {
            MemoryManager db(path);
            QVERIFY(db.saveQATurn("write", R"({"personal_interest":"rain"})", {}));
            PrivateReview writing(db);
            QSignalSpy requests(&writing, &PrivateReview::requested);
            QSignalSpy cancellations(&writing, &PrivateReview::cancelled);
            writing.wake(5,60);
            writing.wake(5,60);
            QCOMPARE(requests.size(),1);
            id = requests.first()[0].toLongLong();
            writing.receive(id,requests.first()[1].toInt(),chapter,{});
            // Advance only the stored due time and budget clock in this isolated fixture.
            RawDb clock(path,"advance-review");
            QVERIFY(clock.open());
            QVERIFY(clock.exec("UPDATE chat_history SET review_after=0"));
            QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
            writing.wake(5,60);
            QCOMPARE(requests.size(),2);
            interruptedVersion = requests.last()[1].toInt();
            writing.yieldAttention("incoming_message");
            QCOMPARE(cancellations.size(),1);
            writing.wake(5,60);
            QCOMPARE(requests.size(),2); // Interrupted work still consumed budget.
            const auto saved = db.nextThought(true)["last_review"].toObject();
            QCOMPARE(saved["draft"].toString(),QString("first paragraph"));
            QCOMPARE(saved["deferred_reason"].toString(),QString("incoming_message"));
            QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
        }
        MemoryManager db(path);
        PrivateReview restored(db);
        QSignalSpy requests(&restored, &PrivateReview::requested);
        QSignalSpy shares(&restored, &PrivateReview::shareRequested);
        restored.wake(5,60);
        QCOMPARE(requests.size(),1);
        const auto attempt = requests.first();
        const auto context = attempt[2].toJsonObject()["last_review"].toObject();
        QCOMPARE(context["next_step"].toString(),QString("second paragraph"));
        QCOMPARE(context["draft"].toString(),QString("first paragraph"));
        const QJsonObject finalDraft{{"status","completed"},{"reflection","finished"},
                                     {"draft","first paragraph and second paragraph"},{"share_with_owner",true}};
        restored.receive(id,interruptedVersion,finalDraft,{});
        QVERIFY(!db.finishThoughtReview(id,interruptedVersion,finalDraft));
        QCOMPARE(shares.size(),0);
        restored.receive(id,attempt[1].toInt(),finalDraft,{});
        QCOMPARE(shares.size(),1);
        QCOMPARE(db.nextThought(false)["last_review"].toObject()["draft"],finalDraft["draft"]);
        QVERIFY(db.nextThought(true).isEmpty());
    }

    void expiredWritingClaimRejectsOldAttempt()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("expired.db");
        qlonglong id = 0;
        int version = 0;
        {
            MemoryManager db(path);
            QVERIFY(db.saveQATurn("write", R"({"personal_interest":"a poem"})", {}));
            PrivateReview writing(db);
            QSignalSpy requests(&writing, &PrivateReview::requested);
            writing.wake(3,60);
            QCOMPARE(requests.size(),1);
            id = requests.first()[0].toLongLong();
            version = requests.first()[1].toInt();
            // Simulate losing the process without a graceful shutdown event.
        }
        MemoryManager db(path);
        PrivateReview writing(db);
        QSignalSpy requests(&writing, &PrivateReview::requested);
        writing.wake(3,60);
        QVERIFY(requests.isEmpty());
        RawDb clock(path,"expire-claim");
        QVERIFY(clock.open());
        QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-2000"));
        QVERIFY(clock.exec("UPDATE chat_history SET review_after=unixepoch()-1"));
        writing.wake(3,60);
        QCOMPARE(requests.size(),1);
        const int newVersion = requests.first()[1].toInt();
        QVERIFY(newVersion > version);
        QVERIFY(!db.deferThoughtReview(id,version,"stale"));
        writing.receive(id,newVersion,{{"status","active"},{"reflection","edited"},{"draft",QString(801,'x')}},{});
        QCOMPARE(db.nextThought(true)["last_review"].toObject()["deferred_reason"].toString(),
                 QString("invalid_or_stale_proposal"));
        QVERIFY(!db.nextThought(true)["last_review"].toObject().contains("draft"));
    }

    void writingCannotShareAfterAbandonOrDeletion()
    {
        for (const bool remove : {false,true}) {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("cancel.db"));
            QVERIFY(db.saveQATurn("write", R"({"personal_interest":"rain"})", {}));
            PrivateReview writing(db);
            QSignalSpy requests(&writing, &PrivateReview::requested);
            QSignalSpy shares(&writing, &PrivateReview::shareRequested);
            writing.wake(5,60);
            QCOMPARE(requests.size(),1);
            const auto id = requests.first()[0].toLongLong();
            const auto version = requests.first()[1].toInt();
            if (remove) QVERIFY(db.deleteTurnByID(id));
            else QVERIFY(db.applyThoughtAction(id,version,"abandon"));
            writing.receive(id,version,{{"status","active"},{"reflection","late"},{"draft","late"},{"share_with_owner",true}},{});
            QCOMPARE(shares.size(),0);
            QVERIFY(db.nextThought(true).isEmpty());
        }
    }

    void independentReflectionRespectsRestAndSourceLifetime()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("reflection.db"));
        RequestMeta meta;
        meta.eventId = meta.runId = "first";
        const auto first = db.recordInput("a conversation without a task",meta,true);
        QVERIFY(db.recordQuiet(first,"close"));
        QVERIFY(db.nextThought(true).isEmpty());
        const auto claim = db.claimPrivateReflection(2,60);
        QCOMPARE(claim["id"].toInteger(),first);
        QVERIFY(db.deferPrivateReflection(first,1));
        QVERIFY(!db.finishPrivateReflection(first,1,"consider","late"));
        QVERIFY(db.claimPrivateReflection(2,60).isEmpty());
        RawDb clock(dir.filePath("reflection.db"),"reflection-clock");
        QVERIFY(clock.open());
        QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
        QCOMPARE(db.claimPrivateReflection(2,60)["version"].toInt(),2);
        QVERIFY(db.finishPrivateReflection(first,2,"rest",""));
        QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
        QVERIFY(db.claimPrivateReflection(3,60).isEmpty());
        meta.eventId = meta.runId = "second";
        const auto second = db.recordInput("another ordinary experience",meta,true);
        QVERIFY(db.recordQuiet(second,"close"));
        QVERIFY(db.claimPrivateReflection(2,60).isEmpty());
        QCOMPARE(db.claimPrivateReflection(3,60)["id"].toInteger(),second);
        QVERIFY(db.finishPrivateReflection(second,1,"consider","own question"));
        QCOMPARE(db.recentPrivateReflection(meta)["own_reflection"].toString(),QString("own question"));
        meta.eventId = meta.runId = "third";
        const auto third = db.recordInput("unrelated later event",meta,true);
        QVERIFY(db.recordQuiet(third,"close"));
        QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
        QCOMPARE(db.claimPrivateReflection(4,60)["id"].toInteger(),third);
        QVERIFY(db.finishPrivateReflection(third,1,"consider","newer unrelated note"));
        QCOMPARE(db.recentPrivateReflection(meta)["source_turn"].toInteger(),third);
        QCOMPARE(db.recentPrivateReflection(meta,QJsonArray{second})["source_turn"].toInteger(),second);
        QVERIFY(db.deleteTurnByID(third));
        meta.channel=Channel::kQQGroup;
        QVERIFY(db.recentPrivateReflection(meta).isEmpty());
        QVERIFY(db.deleteTurnByID(second));
        QCOMPARE(clock.scalar("SELECT count(*) FROM private_reflection WHERE state='consider'").toInt(),0);
    }

    void reflectionReconsideration_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("retain") << QString{};
        QTest::newRow("revise") << QString("revise");
        QTest::newRow("resolve") << QString("resolve");
    }

    void reflectionReconsideration()
    {
        QFETCH(QString,action);
        QTemporaryDir dir;
        const auto path=dir.filePath("attention.db");
        qint64 oldTurn=0,newTurn=0;
        RequestMeta meta;
        {
            MemoryManager db(path);
            meta.eventId=meta.runId="first";
            meta.receivedAtUtc=QDateTime::currentDateTimeUtc().addSecs(-120);
            oldTurn=db.recordInput("opening is uncertain",meta,true);
            QVERIFY(db.recordQuiet(oldTurn,"close"));
            QCOMPARE(db.claimPrivateReflection(4,60)["id"].toInteger(),oldTurn);
            QVERIFY(db.finishPrivateReflection(oldTurn,1,"consider","I wonder when it opens"));
            RawDb clock(path,"attention-clock");
            QVERIFY(clock.open());
            QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
            meta.eventId=meta.runId="update";
            meta.receivedAtUtc=QDateTime::currentDateTimeUtc();
            newTurn=db.recordInput("the opening was confirmed",meta,true);
            QVERIFY(db.recordQuiet(newTurn,"close"));
            QCOMPARE(db.claimPrivateReflection(4,60)["id"].toInteger(),newTurn);
            // 新契约：与旧关注相关的当前输入不再要求"检索命中同一轮"，直接按关注内容匹配。
            // 这两句语义上都关于 opening，所以现在无需 rankedTurns 也能选中旧关注；
            // 真正的"无关输入不选中"由 tst_concern 的 priorReasonNoneWhenUnrelated 覆盖。
            const auto autoEvidence=db.reflectionHistory(newTurn,1,{});
            QCOMPARE(autoEvidence["prior_reason"].toString(),QStringLiteral("match"));
            QCOMPARE(autoEvidence["prior"].toObject()["source_turn"].toInteger(),oldTurn);
            const auto evidence=db.reflectionHistory(newTurn,1,{oldTurn});
            const auto prior=evidence["prior"].toObject();
            QCOMPARE(prior["source_turn"].toInteger(),oldTurn);
            const auto history=evidence["history"].toArray();
            QCOMPARE(history.size(),1);
            QVERIFY(!db.finishPrivateReflection(newTurn,2,"consider","stale",{},history,prior,action));
            auto invalid=prior;
            invalid["version"]=99;
            QVERIFY(!db.finishPrivateReflection(newTurn,1,"consider","stale",{},history,invalid,action));
            const auto choice=action=="resolve" ? "rest" : "consider";
            QVERIFY(db.finishPrivateReflection(newTurn,1,choice,"new perspective",{},history,prior,action));
        }
        MemoryManager db(path);
        QCOMPARE(db.recentPrivateReflection(meta,{oldTurn},true).isEmpty(),!action.isEmpty());
        QCOMPARE(db.recentPrivateReflection(meta,{newTurn},true).isEmpty(),action=="resolve");
        QVERIFY(db.deleteTurnByID(newTurn));
        QCOMPARE(db.recentPrivateReflection(meta,{oldTurn},true)["source_turn"].toInteger(),oldTurn);
    }

    void changedCrossChannelPolicyInvalidatesReview()
    {
        auto &config=ConfigManager::instance();
        QFile file(config.getConfigFilePath());
        const bool existed=file.open(QIODevice::ReadOnly);
        const auto original=existed ? file.readAll() : QByteArray{};
        file.close();
        const auto restore=qScopeGuard([&] {
            config.writeConfigDocument("setting.json",existed ? original : QByteArray("{}"),nullptr);
            if (!existed) QFile::remove(file.fileName());
        });
        QVERIFY(config.writeConfigDocument("setting.json",R"({"memory":{"owner_cross_channel_private":true}})",nullptr));
        QTemporaryDir dir;
        const auto path=dir.filePath("cross.db");
        MemoryManager db(path);
        RequestMeta meta;
        meta.runId=meta.eventId="first";
        meta.receivedAtUtc=QDateTime::currentDateTimeUtc().addSecs(-120);
        const auto first=db.recordInput("an earlier observation",meta,true);
        QVERIFY(db.recordQuiet(first,"close"));
        QCOMPARE(db.claimPrivateReflection(3,60)["id"].toInteger(),first);
        QVERIFY(db.finishPrivateReflection(first,1,"consider","a private question"));
        RawDb clock(path,"cross-clock");
        QVERIFY(clock.open());
        QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-120"));
        meta.runId=meta.eventId="second";
        meta.receivedAtUtc=QDateTime::currentDateTimeUtc();
        meta.channel="qq_private";
        meta.actorId=meta.sessionId="10001";
        const auto second=db.recordInput("the situation changed",meta,true);
        QVERIFY(db.recordQuiet(second,"close"));
        QCOMPARE(db.claimPrivateReflection(3,60)["id"].toInteger(),second);
        const auto data=db.reflectionHistory(second,1,{first});
        QCOMPARE(data["prior"].toObject()["source_turn"].toInteger(),first);
        QVERIFY(config.writeConfigDocument("setting.json",R"({"memory":{"owner_cross_channel_private":false}})",nullptr));
        QVERIFY(db.reflectionHistory(second,1,{first})["history"].toArray().isEmpty());
        QVERIFY(!db.finishPrivateReflection(second,1,"rest","",{},data["history"].toArray(),data["prior"].toObject(),"resolve"));
        QVERIFY(db.finishPrivateReflection(second,1,"rest",""));
    }

    void activityActionsRequireExpectedVersion()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("activity.db");
        {
            MemoryManager db(path);
            RequestMeta meta;
            QVERIFY(db.saveQATurn("试试写诗", R"({"personal_interest":"想写雨声"})", {}, meta));
            auto thought = db.nextThought(true);
            const qlonglong id = thought["id"].toInteger();
            const int version = thought["version"].toInt();
            QVERIFY(id > 0);

            QVERIFY(db.finishThoughtReview(id, version,
                QJsonObject{{"status","active"},{"reflection","继续写"},
                            {"progress","写了两句"},{"next_step","写第三句"},
                            {"draft","屋檐轻响，雨在窗上停了一停。"},
                            {"review_hours",24}}));
            auto after = db.nextThought(false);
            const int v2 = after["version"].toInt();
            QCOMPARE(v2, version + 1);

            QVERIFY(!db.applyThoughtAction(id,v2,"resume"));
            QVERIFY(db.nextThought(true).isEmpty());
            QCOMPARE(db.nextThought(false)["version"].toInt(),v2);
            QVERIFY(!db.applyThoughtAction(id,v2,"reschedule",0,""));
            QVERIFY(db.applyThoughtAction(id, v2, "pause"));
            QVERIFY(db.nextThought(true).isEmpty());
            QVERIFY(!db.canShareThought(id, v2 + 1));
            QVERIFY(!db.finishThoughtReview(id, v2,
                QJsonObject{{"status","active"},{"reflection","迟到的写作"}}));

            QVERIFY(db.applyThoughtAction(id, v2 + 1, "resume"));
            auto resumed = db.nextThought(true);
            QCOMPARE(resumed["id"].toInteger(), id);
            const int v3 = resumed["version"].toInt();

            QVERIFY(db.applyThoughtAction(id,v3,"reschedule",120,"revisit later"));
            QVERIFY(db.nextThought(true).isEmpty());
            QCOMPARE(db.nextThought(false)["last_review"].toObject()["schedule_reason"].toString(),QString("revisit later"));
            QVERIFY(db.applyThoughtAction(id, v3+1, "abandon"));
            QVERIFY(!db.hasActiveThought("user"));
            QVERIFY(db.nextThought(true).isEmpty());
        }
    }

    void activityActionAppliesFromReply()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("activity-reply.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = "2014366825";
        meta.eventId = "activity-a";
        QVERIFY(db.saveQATurn("想写点东西", R"({"personal_interest":"写雨声"})", {}, meta));
        auto thought = db.nextThought(true);
        const qlonglong id = thought["id"].toInteger();
        const int version = thought["version"].toInt();
        QVERIFY(id > 0);
        QVERIFY(db.applyThoughtAction(id, version, "pause"));

        meta.eventId = "activity-b";
        const QString reply = QStringLiteral(
            R"({"activity_action":{"action":"resume","activity_ref":"thought/%1","expected_version":%2}})")
            .arg(id).arg(version + 1);
        QVERIFY(db.saveQATurn("继续写", reply, {}, meta));
        auto resumed = db.nextThought(true);
        QCOMPARE(resumed["id"].toInteger(), id);
    }

    void currentUnderstandingCanIncludeAudiencePrivate()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("audience-private.db"));
        RequestMeta privateMeta;
        privateMeta.channel = Channel::kQQPrivate;
        privateMeta.sessionId = privateMeta.actorId = QStringLiteral("2014366825");
        privateMeta.eventId = QStringLiteral("audience-private");
        QVERIFY(db.enqueueObservation(privateMeta, QStringLiteral("脚踝已经好了")));
        const auto source = db.pendingObservation()["source"].toInteger();
        const QJsonObject change{{"matter",""},{"version",0},{"topic","脚踝"},
            {"quote","脚踝已经好了"},{"status","confirmed"},
            {"understanding","脚踝已恢复"},{"kind","self_report"}};
        QVERIFY(db.commitObservation(source,
            QJsonObject{{"outcome","updated"},{"changes",QJsonArray{change}}}));

        RequestMeta group;
        group.channel = Channel::kQQGroup;
        group.sessionId = QStringLiteral("20202");
        group.actorId = QStringLiteral("owner");
        const auto hidden = db.currentUnderstanding(group, QStringLiteral("脚踝"));
        const auto included = db.currentUnderstanding(group, QStringLiteral("脚踝"),
                                                      nullptr, {}, true);
        QVERIFY(!QJsonDocument(hidden).toJson().contains("脚踝已经好了"));
        QVERIFY(QJsonDocument(included).toJson().contains("脚踝已经好了"));
        QVERIFY(!included.isEmpty());
        QCOMPARE(included.first().toObject()["self_report"].toObject()["quote"].toString(),
                 QStringLiteral("脚踝已经好了"));
    }

    void evidenceOwnershipComparesViewerIdentity()
    {
        const QJsonObject row{{"id", 7},
                              {"source_ref", "chat/7:user"},
                              {"source_type", "chat"},
                              {"derived", "original"},
                              {"text", "这是别人的私聊"},
                              {"speaker_id", "qq:2014366825"},
                              {"speaker", "朋友"},
                              {"speaker_role", "user"},
                              {"channel", "qq_private"},
                              {"session", "2014366825"},
                              {"speech_at", "2026-11-13T10:00:00.000Z"}};
        const auto ownerView = MemoryEvidence::fromChunk(row, "owner", true);
        QVERIFY(!ownerView.own);
        const auto selfView = MemoryEvidence::fromChunk(row, "qq:2014366825", false);
        QVERIFY(selfView.own);
        const auto assistantRow = QJsonObject{{"id", 8},
                                               {"source_ref", "chat/8:assistant"},
                                               {"source_type", "chat"},
                                               {"derived", "assistant_expression"},
                                               {"text", "好呀"},
                                               {"speaker_id", "mako"},
                                               {"speaker_role", "assistant"},
                                               {"channel", "qq_private"},
                                               {"session", "2014366825"}};
        QVERIFY(!MemoryEvidence::fromChunk(assistantRow, "owner", true).own);
    }

    void retrievalServiceIncludesCurrentChannelAndKeepsSpeaker()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("retrieval.db"));
        db.seedIdentity({QStringLiteral("2014366825")});
        RawEventWrite event;
        event.platform = QStringLiteral("qq");
        event.channel = Channel::kQQPrivate;
        event.sessionId = event.senderId = QStringLiteral("2014366825");
        event.eventType = QStringLiteral("message");
        event.content = QStringLiteral("脚踝已经好了");
        event.messageId = QStringLiteral("m-retrieval");
        event.occurredAt = QDateTime::currentDateTime();
        QVERIFY(db.appendRawEvent(event));

        RetrievalRequest request;
        request.subject = QStringLiteral("user");
        request.accounts = {QStringLiteral("2014366825")};
        request.terms = {QStringLiteral("脚踝")};
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = QStringLiteral("2014366825");
        request.excludeCurrent = false;
        MemoryRetrievalService service(&db);
        const auto result = service.retrieve(request);
        QVERIFY(!result.evidence.isEmpty());
        const auto &evidence = result.evidence.first();
        QCOMPARE(evidence.channel, Channel::kQQPrivate);
        QCOMPARE(evidence.speakerId, QStringLiteral("owner"));
        QCOMPARE(evidence.speakerRole, QStringLiteral("user"));
        QCOMPARE(evidence.sourceType, QStringLiteral("raw"));
        QCOMPARE(evidence.derived, QStringLiteral("original"));
        QVERIFY(evidence.speechAt.isValid());
        QCOMPARE(evidence.text, QStringLiteral("脚踝已经好了"));

        request.excludeCurrent = true;
        QVERIFY(service.retrieve(request).evidence.isEmpty());

        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.subject = QStringLiteral("user");
        meta.speaker = QStringLiteral("主人");
        const auto situation = MemorySituation::fromRequest(meta, false);
        QCOMPARE(situation.about, QStringLiteral("unknown"));
        QCOMPARE(situation.audienceKind, QStringLiteral("group"));
        QCOMPARE(situation.modality, QStringLiteral("text"));
        QVERIFY(situation.toPromptBlock().contains("current_local_time: "
            + situation.currentTime.toString(Qt::ISODateWithMs)));
        QVERIFY(situation.toPromptBlock().contains(QStringLiteral("speaker: owner")));
    }

    void searchIndexRebuildsAndRetrieves()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("index.db"));
        db.seedIdentity({QStringLiteral("2014366825")});
        RawEventWrite event;
        event.platform = QStringLiteral("qq");
        event.channel = Channel::kQQPrivate;
        event.sessionId = event.senderId = QStringLiteral("2014366825");
        event.senderName = QStringLiteral("主人");
        event.eventType = QStringLiteral("message");
        event.messageId = QStringLiteral("index-1");
        event.content = QStringLiteral("脚踝已经好了，周末去散步");
        event.occurredAt = QDateTime::currentDateTime().addSecs(-60);
        QVERIFY(db.appendRawEvent(event));
        event.messageId = QStringLiteral("index-2");
        event.content = QStringLiteral("今天想写一段雨声");
        event.occurredAt = QDateTime::currentDateTime();
        QVERIFY(db.appendRawEvent(event));
        RequestMeta chatMeta;
        chatMeta.channel = Channel::kQQPrivate;
        chatMeta.sessionId = chatMeta.actorId = QStringLiteral("2014366825");
        chatMeta.eventId = QStringLiteral("index-chat");
        QVERIFY(db.saveQATurn(QStringLiteral("今晚早点睡"),
                              QStringLiteral(R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ"}]})"),
                              {}, chatMeta));

        QString error;
        QVERIFY2(db.rebuildSearchIndex(&error), qPrintable(error));
        QVERIFY(db.memoryChunkCount() >= 2);
        const auto indexStatus = db.searchIndexStatus();
        QCOMPARE(indexStatus.value(QStringLiteral("chunks")).toInt(), db.memoryChunkCount());
        QCOMPARE(indexStatus.value(QStringLiteral("state")).toObject()
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("complete"));
        QVERIFY(indexStatus.value(QStringLiteral("state")).toObject()
                    .value(QStringLiteral("duration_ms")).toString().toInt() >= 0);
        QVERIFY(indexStatus.value(QStringLiteral("chunks_by_source")).toObject()
                    .value(QStringLiteral("raw")).toInt() >= 1);
        const auto rows = db.searchMemoryChunks({QStringLiteral("脚踝")});
        QVERIFY(!rows.isEmpty());
        const auto hash = rows.first().toObject().value(QStringLiteral("content_hash")).toString();
        QVERIFY(!hash.isEmpty());

        auto provider = std::make_shared<LocalHashEmbeddingProvider>(64);
        const auto chunks = db.memoryChunksForEmbedding();
        for (const auto &value : chunks) {
            const auto chunk = value.toObject();
            QVector<float> vector;
            QVERIFY(provider->embed(chunk[QStringLiteral("text")].toString(), &vector));
            const QByteArray blob(reinterpret_cast<const char *>(vector.constData()),
                                  vector.size() * int(sizeof(float)));
            QVERIFY(db.storeChunkEmbedding(chunk[QStringLiteral("id")].toInteger(),
                                           provider->modelId(), vector.size(), blob,
                                           chunk[QStringLiteral("content_hash")].toString()));
        }
        QString embedError;
        QVERIFY2(db.embedSearchChunks(provider.get(), 2000, &embedError),
                 qPrintable(embedError));
        QVERIFY(db.searchIndexStatus().value(QStringLiteral("embeddings")).toInt() > 0);
        auto reranker = std::make_shared<IdentityMemoryReranker>();
        MemoryRetrievalService service(&db, provider, reranker);
        RetrievalRequest request;
        request.query = QStringLiteral("脚踝");
        request.subject = QStringLiteral("user");
        request.accounts = {QStringLiteral("2014366825")};
        request.terms = {QStringLiteral("脚踝")};
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = QStringLiteral("2014366825");
        request.excludeCurrent = false;
        request.useIndex = true;
        const auto result = service.retrieve(request);
        QVERIFY(!result.evidence.isEmpty());
        QCOMPARE(result.backend, QStringLiteral("index"));
        QVERIFY(result.latencyMs >= 0);
        QCOMPARE(result.evidence.first().speakerId, QStringLiteral("owner"));
        QCOMPARE(result.evidence.first().derived, QStringLiteral("original"));
        QVERIFY(result.evidence.first().handle.startsWith(QStringLiteral("chunk/")));
        MemoryRetrievalService defaultRerank(&db, provider);
        const auto defaultResult = defaultRerank.retrieve(request);
        QVERIFY(defaultResult.evidence.size() <= request.rerankLimit);

        auto rawOnly = request;
        rawOnly.sourceKinds = {QStringLiteral("raw")};
        const auto rawOnlyResult = defaultRerank.retrieve(rawOnly);
        for (const auto &item : rawOnlyResult.evidence)
            QCOMPARE(item.sourceType, QStringLiteral("raw"));

        // 结构化来源可独立查询；严格原文模式排除派生资料。
        QVERIFY(db.addLongTermSummary(QStringLiteral("legacy-only"), 1, QStringLiteral("[]")));
        auto structuredRequest = request;
        structuredRequest.query = QStringLiteral("legacy-only");
        structuredRequest.terms = {QStringLiteral("legacy-only")};
        structuredRequest.limit = 1;
        const auto withSummary = defaultRerank.retrieve(structuredRequest);
        QVERIFY(withSummary.evidence.size() <= 1);
        auto noAux = structuredRequest;
        noAux.includeAuxiliarySources = false;
        noAux.sourceKinds = {QStringLiteral("summary")};
        QVERIFY(defaultRerank.retrieve(noAux).evidence.isEmpty());
        auto summaryOnly = structuredRequest;
        summaryOnly.sourceKinds = {QStringLiteral("summary")};
        QVERIFY(!defaultRerank.retrieve(summaryOnly).evidence.isEmpty());

        MemoryRetrievalService degraded(&db, std::make_shared<FailingEmbeddingProvider>());
        const auto degradedResult = degraded.retrieve(request);
        QCOMPARE(degradedResult.status, QStringLiteral("degraded"));
        QVERIFY(!degradedResult.evidence.isEmpty());

        // Rebuild is idempotent at the chunk level.
        const int count = db.memoryChunkCount();
        QVERIFY(db.rebuildSearchIndex(&error));
        QCOMPARE(db.memoryChunkCount(), count);

        const auto turns = db.getHistoryTurn(10, chatMeta.channel, chatMeta.sessionId);
        QVERIFY(!turns.isEmpty());
        QVERIFY(db.deleteTurnByID(turns.first().id));
        QVERIFY(db.memoryChunkCount() < count);

        QVERIFY2(db.clearSearchIndex(&error), qPrintable(error));
        QCOMPARE(db.memoryChunkCount(), 0);
        MemoryRetrievalService fallback(&db);
        request.sourceKinds = {"raw"};
        QVERIFY(fallback.retrieve(request).evidence.isEmpty());
        request.sourceKinds = {"summary"};
        QVERIFY(!fallback.retrieve(request).evidence.isEmpty());
    }

    void indexWorkerRebuilds()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("worker.db");
        {
            MemoryManager db(path);
            RawEventWrite event;
            event.platform = QStringLiteral("qq");
            event.channel = Channel::kQQGroup;
            event.sessionId = QStringLiteral("30001");
            event.senderId = QStringLiteral("20001");
            event.eventType = QStringLiteral("message");
            event.content = QStringLiteral("worker indexing");
            event.occurredAt = QDateTime::currentDateTime();
            QVERIFY(db.appendRawEvent(event));
        }
        MemoryManager db(path);
        QVERIFY(db.clearSearchIndex());
        MemoryRetrievalConfig config;
        config.autoBackfill = true;
        MemoryIndexWorker worker(db, config);
        QSignalSpy drained(&worker, &MemoryIndexWorker::drained);
        QTRY_COMPARE(drained.size(), 1);
        QVERIFY(drained.first().at(0).toBool());
        QCOMPARE(db.memoryChunkCount(), 1);
    }

    void onlineEmbeddingsFollowCommittedMessages()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("online.db"));
        MemoryRetrievalConfig config;
        config.embeddingEnabled = true;
        config.embeddingProvider = "local_hash_test";
        config.embeddingDimension = 32;
        auto provider = std::make_shared<LocalHashEmbeddingProvider>(32);
        RawEventWrite event;
        event.platform = "qq";
        event.channel = Channel::kQQGroup;
        event.sessionId = "30001";
        event.senderId = "20001";
        event.eventType = "message";
        event.content = "backlog";
        event.occurredAt = QDateTime::currentDateTime();
        QVERIFY(db.appendRawEvent(event));
        MemoryIndexWorker worker(db, config);
        QSignalSpy drained(&worker, &MemoryIndexWorker::drained);
        QTRY_COMPARE(drained.size(), 1);
        QCOMPARE(db.searchIndexStatus()["embeddings"].toInt(), 1);

        RequestMeta meta;
        meta.eventId = "pet-receipt";
        meta.subject = "user";
        const auto turn = db.recordInput("updated schedule", meta, false);
        QVERIFY(turn > 0);
        QTRY_COMPARE(db.searchIndexStatus()["embeddings"].toInt(), 2);
        const auto userChunk = db.searchMemoryChunks({"schedule"}).first().toObject();
        const auto id = userChunk["id"].toInteger();
        QVERIFY(!db.chunkEmbedding(id, provider->modelId()).isEmpty());
        RetrievalRequest request;
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = "owner";
        request.viewerPersonId = "owner";
        request.viewerIsOwner = true;
        request.sourceKinds = {"chat"};
        request.requireTermMatch = true;
        request.query = "rescheduled";
        const auto result = MemoryRetrievalService(&db, provider).retrieve(request);
        QCOMPARE(result.lexicalCount, 0);
        QCOMPARE(result.vectorCount, 1);
        QVERIFY(!result.evidence.isEmpty());
        QCOMPARE(result.evidence.first().text, QStringLiteral("updated schedule"));

        SentenceText reply;
        reply.zhText = "acknowledged";
        QVERIFY(db.saveQATurn("updated schedule", "{}", {reply}, meta, turn));
        event.content = "online group input";
        QVERIFY(db.appendRawEvent(event));
        QTRY_COMPARE(db.searchIndexStatus()["embeddings"].toInt(), 4);
        QVERIFY(!db.chunkEmbedding(id, provider->modelId()).isEmpty());
        QVERIFY(db.memoryChunksMissingEmbedding(1, provider->modelId()).isEmpty());
    }

    void onlineEmbeddingRetriesWithoutAnotherMessage()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("retry.db");
        MemoryManager db(path);
        RawDb raw(path, "retry");
        QVERIFY(raw.open());
        QVERIFY(raw.exec("CREATE TRIGGER busy_embedding BEFORE INSERT ON memory_embeddings "
                         "BEGIN SELECT RAISE(ABORT,'temporary failure'); END"));
        makePetTurn(db, "retry after failure", "");
        MemoryRetrievalConfig config;
        config.embeddingEnabled = true;
        config.embeddingProvider = "local_hash_test";
        MemoryIndexWorker worker(db, config);
        QSignalSpy drained(&worker, &MemoryIndexWorker::drained);
        QTRY_COMPARE(drained.size(), 1);
        QVERIFY(!drained.first().at(0).toBool());
        QCOMPARE(db.searchIndexStatus()["embeddings"].toInt(), 0);
        QVERIFY(raw.exec("DROP TRIGGER busy_embedding"));
        QTRY_COMPARE(drained.size(), 2);
        QVERIFY(drained.last().at(0).toBool());
        QCOMPARE(db.searchIndexStatus()["embeddings"].toInt(), 1);
        const auto chunk = db.memoryChunksForEmbedding().first().toObject();
        QVERIFY(raw.exec("DELETE FROM chat_history"));
        QVERIFY(db.storeChunkEmbedding(chunk["id"].toInteger(), "stale", 1,
                                       QByteArray(4, 0), chunk["content_hash"].toString()));
        QCOMPARE(db.searchIndexStatus()["embeddings"].toInt(), 0);
    }

    void reopensV14DatabaseWithoutMemoryLoss()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("v14-reopen.db");
        {
            MemoryManager db(path);
            RequestMeta meta;
            meta.eventId = QStringLiteral("v14-history");
            QVERIFY(db.saveQATurn(QStringLiteral("旧版本历史"),
                                  QStringLiteral("{}"), {}, meta));
            QVERIFY(db.memoryChunkCount() > 0);
        }
        {
            RawDb raw(path, "v15-downgrade");
            QVERIFY(raw.open());
            QVERIFY(raw.exec("DROP TABLE memory_embeddings"));
            QVERIFY(raw.exec("DROP TABLE memory_chunk_sources"));
            QVERIFY(raw.exec("DROP TABLE memory_chunks"));
            QVERIFY(raw.exec("DROP TABLE memory_index_state"));
            QVERIFY(raw.exec("PRAGMA user_version=14"));
        }
        {
            MemoryManager db(path);
            QCOMPARE(db.getTotalHistoryCount(), 1);
            QCOMPARE(db.memoryChunkCount(), 0);
            QString error;
            QVERIFY2(db.rebuildSearchIndex(&error), qPrintable(error));
            QVERIFY(db.memoryChunkCount() > 0);
        }
        RawDb raw(path, "v15-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);
    }

    void databaseBackupIsConsistent()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("backup-source.db");
        const QString backup = dir.filePath("backup-copy.db");
        {
            MemoryManager db(path);
            RequestMeta meta;
            meta.eventId = QStringLiteral("backup-turn");
            QVERIFY(db.saveQATurn(QStringLiteral("备份我"), QStringLiteral("{}"), {}, meta));
            QVERIFY(db.memoryChunkCount() > 0);
            QString error;
            QVERIFY2(db.backupDatabase(backup, &error), qPrintable(error));
        }
        RawDb copy(backup, "backup-copy");
        QVERIFY(copy.open());
        QCOMPARE(copy.scalar("SELECT count(*) FROM chat_history").toInt(), 1);
        QCOMPARE(copy.scalar("PRAGMA user_version").toInt(), 28);
        QVERIFY(copy.scalar("SELECT count(*) FROM memory_chunks").toInt() > 0);
    }

    void historicalReplayIsolatesAllMemoryReaders()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("archive.db");
        { MemoryManager archive(path); }
        RawDb archive(path, "replay-original");
        QVERIFY(archive.open());
        for (int day : {10, 12}) {
            const QString date = QString("2026-11-%1 08:00:00").arg(day);
            const QString stamp = QString("'%1'").arg(date);
            const QString id = QString::number(day);
            QVERIFY(archive.exec("INSERT INTO chat_history(id,timestamp,user_input,raw_reply) VALUES("+id+","+stamp+",'turn','')"));
            QVERIFY(archive.exec("INSERT INTO raw_events(id,platform,channel,session_id,event_type,occurred_at,content) "
                                 "VALUES("+id+",'qq','qq_group','g','message',"+stamp+",'raw')"));
            QVERIFY(archive.exec("UPDATE raw_events SET content_recorded_at="+stamp+" WHERE id="+id));
            QVERIFY(archive.exec("INSERT INTO interactions(id,channel,session_id,started_at,ended_at,source_event_ids,"
                                 "segmentation_method,summary_status,safe_summary) VALUES("+id+",'qq_group','g',"+stamp+
                                 ","+stamp+",'["+id+"]','test','ready','summary')"));
            QVERIFY(archive.exec("UPDATE interactions SET content_recorded_at="+stamp+" WHERE id="+id));
            QVERIFY(archive.exec("INSERT INTO memory_claims(interaction_id,statement,source_event_ids,source_channel,"
                                 "source_session_id,updated_at,valid_time_start) VALUES(10,'claim','[10]','qq_group','g',"
                                 +stamp+",'2026-11-01')"));
            QVERIFY(archive.exec("INSERT INTO long_term_summary(summary_text,source_ids,covered_turn_end_id,updated_at)"
                                 " VALUES('summary','[10]',10,"+stamp+")"));
            QVERIFY(archive.exec("INSERT INTO cognitive_source(id,event,subject,channel,session,input,reported,outcome) "
                                 "VALUES("+id+","+id+",'user','pet','default','fact','2026-11-01','updated')"));
            QVERIFY(archive.exec("INSERT INTO matter_revision(matter,version,source,topic,status,quote,kind,understanding,"
                                 "recorded) VALUES('matter',"+id+","+id+",'topic','active','fact','self_report','fact',"+stamp+")"));
        }
        BenchmarkSnapshot snapshot;
        QVERIFY(snapshot.open(path, {{"now","2026-11-11T12:00:00Z"}}, {}));
        RawDb replay(snapshot.path(), "replay-check");
        QVERIFY(replay.open());
        for (const QString table : {"chat_history","raw_events","memory_claims","long_term_summary","matter_revision"}) {
            QVERIFY2(replay.scalar("SELECT count(*) FROM "+table).toInt() == 1, qPrintable(table));
            QCOMPARE(archive.scalar("SELECT count(*) FROM "+table).toInt(), 2);
        }
        QCOMPARE(replay.scalar("SELECT count(*) FROM interactions WHERE summary_status='ready'").toInt(), 1);
        QCOMPARE(replay.scalar("SELECT version FROM matter_revision").toInt(), 10);
    }

    void vectorRecallWorksWithoutLexicalHit()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("vector-only.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = QStringLiteral("10101");
        meta.eventId = QStringLiteral("vector-only");
        QVERIFY(db.saveQATurn(QStringLiteral("完全无关的旧内容"),
                              QStringLiteral("{}"), {}, meta));
        QString error;
        QVERIFY2(db.rebuildSearchIndex(&error), qPrintable(error));
        auto provider = std::make_shared<VectorOnlyEmbeddingProvider>();
        QVERIFY2(db.embedSearchChunks(provider.get(), 2000, &error), qPrintable(error));

        RetrievalRequest request;
        request.query = QStringLiteral("查询专用");
        request.terms = {QStringLiteral("查询专用")};
        request.subject = QStringLiteral("user");
        request.viewerPersonId = QStringLiteral("owner");
        request.viewerIsOwner = true;
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = QStringLiteral("10101");
        request.excludeCurrent = false;
        MemoryRetrievalService service(&db, provider);
        const auto result = service.retrieve(request);
        QVERIFY(!result.evidence.isEmpty());
        QVERIFY(result.vectorCount > 0);
        QCOMPARE(result.backend, QStringLiteral("index"));
        bool found = false;
        for (const auto &item : result.evidence)
            if (item.text.contains(QStringLiteral("完全无关的旧内容")))
                found = true;
        QVERIFY(found);
    }

    void embeddingWalksAllChunksInBatches()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("embedding-batches.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = QStringLiteral("10101");
        for (int i = 0; i < 3; ++i) {
            meta.eventId = QStringLiteral("batch-%1").arg(i);
            QVERIFY(db.saveQATurn(QStringLiteral("批量向量测试 %1").arg(i),
                                  QStringLiteral("{}"), {}, meta));
        }
        QString error;
        QVERIFY2(db.rebuildSearchIndex(&error), qPrintable(error));
        LocalHashEmbeddingProvider provider(8);
        QVERIFY2(db.embedSearchChunks(&provider, 2, &error), qPrintable(error));
        const auto status = db.searchIndexStatus();
        QCOMPARE(status.value(QStringLiteral("embeddings")).toInt(), 3);
        QVERIFY2(db.embedSearchChunks(&provider, 2, &error), qPrintable(error));
        QCOMPARE(db.searchIndexStatus().value(QStringLiteral("embeddings")).toInt(), 3);
    }

    void candidateBudgetDoesNotClaimHistoricalCoverage()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("budget.db"));
        for (int i=0; i<4; ++i) makePetTurn(db, QString("shared subject %1").arg(i), "");
        RetrievalRequest request;
        request.query = "shared";
        request.excludeCurrent = false;
        request.limit = request.rerankLimit = 1;
        auto result = MemoryRetrievalService(&db).retrieve(request);
        QVERIFY(result.truncated);
        QCOMPARE(result.status, QString("complete"));
        QCOMPARE(result.evidence.size(), 1);
        QCOMPARE(result.toJson()["coverage_status"].toString(), QString("unknown"));
        auto failing = std::make_shared<FailingEmbeddingProvider>();
        result = MemoryRetrievalService(&db, failing).retrieve(request);
        QCOMPARE(result.status, QString("degraded"));
        QVERIFY(!result.evidence.isEmpty());
        QVERIFY(!result.degradedReason.isEmpty());
    }

    void vectorRecallScansBeyondFirstTwoThousandChunks()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("vector-range.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = QStringLiteral("bulk");
        for (int i = 0; i < 2001; ++i) {
            meta.eventId = QStringLiteral("bulk-%1").arg(i);
            const QString text = i == 2000
                ? QStringLiteral("独特目标内容") : QStringLiteral("填充内容 %1").arg(i);
            QVERIFY(db.saveQATurn(text, QStringLiteral("{}"), {}, meta));
        }
        QString error;
        QVERIFY2(db.rebuildSearchIndex(&error), qPrintable(error));
        auto provider = std::make_shared<TargetedEmbeddingProvider>();
        QVERIFY2(db.embedSearchChunks(provider.get(), 4000, &error), qPrintable(error));

        RetrievalRequest request;
        request.query = QStringLiteral("查询");
        request.terms = {QStringLiteral("查询")};
        request.subject = QStringLiteral("user");
        request.viewerPersonId = QStringLiteral("owner");
        request.viewerIsOwner = true;
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = QStringLiteral("other");
        request.excludeCurrent = false;
        request.useIndex = true;
        request.candidateLimit = 40;
        request.rerankLimit = 10;
        MemoryRetrievalService service(&db);
        service.setEmbeddingProvider(provider);
        const auto result = service.retrieve(request);
        bool found = false;
        for (const auto &item : result.evidence)
            if (item.text == QStringLiteral("独特目标内容"))
                found = true;
        QVERIFY(found);
        QVERIFY(result.evidence.size() <= 10);
    }

    // 重要事件：episodic_memory.importance 进入 MemoryEvidence，并参与本地启发式重排。
    void episodicImportanceBoostsRerank()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("episodic-importance.db"));
        const QDateTime eventTime = QDateTime::currentDateTime().addSecs(-60);
        // 两条内容等长、词面命中相同的情景记忆，只有 importance 不同。
        QVERIFY(db.addEpisodicMemory(QStringLiteral("荷包蛋记录甲"), QStringLiteral("event"), 0.1,
                                     eventTime));
        QVERIFY(db.addEpisodicMemory(QStringLiteral("荷包蛋记录乙"), QStringLiteral("event"), 0.95,
                                     eventTime));

        RetrievalRequest request;
        request.query = QStringLiteral("荷包蛋");
        request.subject = QStringLiteral("user");
        const auto items = MemoryRetrievalService(&db).retrieve(request).evidence;
        QVERIFY(items.size() >= 2);
        QStringList texts;
        for (const auto &item : items)
            texts.append(item.text);
        // 同词面命中下，importance 更高的情景记忆排在前面。
        QCOMPARE(texts.value(0), QStringLiteral("荷包蛋记录乙"));
        QVERIFY(texts.contains(QStringLiteral("荷包蛋记录甲")));
        QCOMPARE(items.first().sourceType, QStringLiteral("episode"));
        QVERIFY(qAbs(items.first().importance - 0.95) < 1e-6);
        // toJson 仅在 importance>0 时输出该字段。
        const auto highJson = items.first().toJson();
        QVERIFY(highJson.contains(QStringLiteral("importance")));
        QVERIFY(qAbs(highJson.value(QStringLiteral("importance")).toDouble() - 0.95) < 1e-6);
        for (const auto &item : items) {
            if (item.text == QStringLiteral("荷包蛋记录甲")) {
                QVERIFY(qAbs(item.importance - 0.1) < 1e-6);
                QVERIFY(item.toJson().contains(QStringLiteral("importance")));
            }
        }
    }

    void rerankReceivesFilteredDedupedBoundedCandidatesAndFallsBack()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("rerank-scope.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = QStringLiteral("A");
        for (int i = 0; i < 2; ++i) {
            meta.eventId = QStringLiteral("a-%1").arg(i);
            QVERIFY(db.saveQATurn(QStringLiteral("重复文本"), QStringLiteral("{}"), {}, meta));
        }
        meta.sessionId = meta.actorId = QStringLiteral("B");
        meta.eventId = QStringLiteral("b-1");
        QVERIFY(db.saveQATurn(QStringLiteral("另一会话"), QStringLiteral("{}"), {}, meta));
        QString error;
        QVERIFY2(db.rebuildSearchIndex(&error), qPrintable(error));
        auto provider = std::make_shared<VectorOnlyEmbeddingProvider>();
        QVERIFY2(db.embedSearchChunks(provider.get(), 100, &error), qPrintable(error));

        RetrievalRequest request;
        request.query = QStringLiteral("重复");
        request.terms = {QStringLiteral("重复")};
        request.subject = QStringLiteral("user");
        request.viewerPersonId = QStringLiteral("owner");
        request.viewerIsOwner = true;
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = QStringLiteral("other");
        request.excludeCurrent = false;
        request.useIndex = true;
        request.sourceKinds = {QStringLiteral("chat")};
        request.allowedSources = {QStringLiteral("qq_private|A")};
        request.candidateLimit = 2;
        request.rerankLimit = 2;

        auto recorder = std::make_shared<RecordingReranker>();
        MemoryRetrievalService service(&db);
        service.setEmbeddingProvider(provider);
        service.setReranker(recorder);
        const auto result = service.retrieve(request);
        QVERIFY(!recorder->seen.isEmpty());
        QVERIFY(recorder->seen.size() <= 2);
        QSet<QString> sessions;
        QSet<QString> texts;
        for (const auto &item : recorder->seen) {
            sessions.insert(item.session);
            texts.insert(item.text);
        }
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(texts.size(), recorder->seen.size());
        QVERIFY(!result.evidence.isEmpty());

        recorder->fail = true;
        const auto degraded = service.retrieve(request);
        QVERIFY(!degraded.evidence.isEmpty());
        QCOMPARE(degraded.status, QStringLiteral("degraded"));
        QVERIFY(degraded.degradedReason.contains(QStringLiteral("timeout")));
    }

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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);

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

            // raw_messages → raw_events（旧表删除，v5 后 18 列）
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='raw_messages'").toInt(), 0);
            const QStringList rawCols = raw.columns("raw_events");
            QCOMPARE(rawCols.size(), 20);
            QVERIFY(rawCols.contains("content_recorded_at"));
            QVERIFY(rawCols.contains("message_id"));
            QVERIFY(rawCols.contains("sender_id"));
            QVERIFY(rawCols.contains("event_type"));
            QVERIFY(rawCols.contains("visibility"));
            QVERIFY(rawCols.contains("person_id"));
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_events").toInt(), 0);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='persons'").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='person_identities'").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='memory_claims'").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='group_membership_periods'").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                                "AND name='group_membership_snapshots'").toInt(), 1);

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
            // 二次启动：user_version 已是 6，不再执行迁移，数据不重复
            MemoryManager second(dbPath);
            RawDb raw(dbPath, "verify2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);
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

    void appendPrivacyBlockedScrubsExistingRow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("blocked.db");
        MemoryManager mm(dbPath);

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30001"), QStringLiteral("message"),
                                  QStringLiteral("原始秘密"), QStringLiteral("m-blocked"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString::fromUtf8(R"({"raw":"秘密"})"),
                                  QDateTime::currentDateTime(), false,
                                  QStringLiteral("channel")));

        QVERIFY(mm.appendPrivacyBlockedEvent(
            QStringLiteral("qq"), Channel::kQQGroup, QStringLiteral("30001"),
            QStringLiteral("m-blocked"), QStringLiteral("20002"),
            QStringLiteral("小明"), QDateTime::currentDateTime(),
            {QStringLiteral("phone")}, QStringLiteral("r3")));

        RawDb raw(dbPath, "blocked");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_events").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT content IS NULL FROM raw_events").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT payload IS NULL FROM raw_events").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT event_type FROM raw_events").toString(),
                 QStringLiteral("privacy_blocked"));
        QCOMPARE(raw.scalar("SELECT redaction_status FROM raw_events").toString(),
                 QStringLiteral("blocked"));
        QCOMPARE(raw.scalar("SELECT processed FROM raw_events").toInt(), 1);
        QVERIFY(raw.scalar("SELECT redaction_rules FROM raw_events").toString()
                    .contains(QStringLiteral("phone")));
        QCOMPARE(raw.scalar("SELECT policy_version FROM raw_events").toString(),
                 QStringLiteral("r3"));
    }

    void appendRawEventWritesRedactionFields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("redacted.db");
        MemoryManager mm(dbPath);

        RawEventWrite event;
        event.platform = QStringLiteral("qq");
        event.channel = Channel::kQQGroup;
        event.sessionId = QStringLiteral("30001");
        event.eventType = QStringLiteral("message");
        event.content = QStringLiteral("有人提到 [REDACTED]");
        event.messageId = QStringLiteral("m-redacted");
        event.senderId = QStringLiteral("20002");
        event.senderName = QStringLiteral("小明");
        event.payloadJson = QString::fromUtf8(R"({"text":"[REDACTED]"})");
        event.occurredAt = QDateTime::currentDateTime();
        event.sensitive = true;
        event.visibility = QStringLiteral("channel");
        event.redactionStatus = QStringLiteral("redacted");
        event.redactionRules = QString::fromUtf8(R"(["phone"])");
        event.policyVersion = QStringLiteral("r3");
        QVERIFY(mm.appendRawEvent(event));

        RawDb raw(dbPath, "redacted");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT redaction_status FROM raw_events").toString(),
                 QStringLiteral("redacted"));
        QCOMPARE(raw.scalar("SELECT sensitive FROM raw_events").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT policy_version FROM raw_events").toString(),
                 QStringLiteral("r3"));
    }

    void interactionBatchAndSummary()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("interactions.db");
        MemoryManager mm(dbPath);

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30001"), QStringLiteral("message"),
                                  QStringLiteral("第一条"), QStringLiteral("m-1"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明")));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30001"), QStringLiteral("message"),
                                  QStringLiteral("第二条"), QStringLiteral("m-2"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明")));

        const QList<RawEvent> events = mm.getUnprocessedGroupEvents(
            QStringLiteral("30001"), 10);
        QCOMPARE(events.size(), 2);

        InteractionDraft draft;
        draft.channel = Channel::kQQGroup;
        draft.sessionId = QStringLiteral("30001");
        draft.startedAt = events.first().occurredAt;
        draft.endedAt = events.last().occurredAt;
        draft.sourceEventIds = {QString::number(events.first().id),
                                QString::number(events.last().id)};
        draft.segmentationMethod = QStringLiteral("time_window");
        draft.confidence = 0.5;
        draft.participants.append(qMakePair(QStringLiteral("20002"),
                                            QStringLiteral("小明")));

        const qlonglong interactionId = mm.createInteractionBatch(
            draft, {events.first().id, events.last().id});
        QVERIFY(interactionId > 0);
        QCOMPARE(mm.countUnprocessedGroupEvents(QStringLiteral("30001")), 0);
        QCOMPARE(mm.getMemoryCursorEventId(Channel::kQQGroup,
                                           QStringLiteral("30001")),
                 events.last().id);

        QCOMPARE(mm.getPendingInteractionIds(10).size(), 1);
        QCOMPARE(mm.getInteractionSourceEvents(interactionId).size(), 2);

        QVERIFY(mm.saveInteractionSummary(interactionId,
                                          QStringLiteral("群友聊了两句"),
                                          QStringLiteral("ready")));
        const QList<InteractionSummary> summaries =
            mm.getRecentInteractionSummaries(Channel::kQQGroup,
                                             QStringLiteral("30001"), 5);
        QCOMPARE(summaries.size(), 1);
        QCOMPARE(summaries.first().summaryText, QStringLiteral("群友聊了两句"));

        RawDb raw(dbPath, "interactions");
        QVERIFY(raw.open());
        QVERIFY(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                           "AND name='interactions'").toInt() == 1);
        QVERIFY(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                           "AND name='interaction_participants'").toInt() == 1);
    }

    void interactionSummaryRetriesThenFails()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("interaction_retry.db");
        MemoryManager mm(dbPath);

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30002"), QStringLiteral("message"),
                                  QStringLiteral("待摘要"), QStringLiteral("m-3"),
                                  QStringLiteral("in"), QStringLiteral("20003"),
                                  QStringLiteral("小红")));
        const QList<RawEvent> events = mm.getUnprocessedGroupEvents(
            QStringLiteral("30002"), 10);
        InteractionDraft draft;
        draft.channel = Channel::kQQGroup;
        draft.sessionId = QStringLiteral("30002");
        draft.startedAt = events.first().occurredAt;
        draft.endedAt = events.first().occurredAt;
        draft.sourceEventIds = {QString::number(events.first().id)};
        const qlonglong id = mm.createInteractionBatch(draft, {events.first().id});
        QVERIFY(id > 0);

        QVERIFY(mm.markInteractionSummaryFailed(id));
        QCOMPARE(mm.getPendingInteractionIds(10).size(), 1);
        QVERIFY(mm.markInteractionSummaryFailed(id));
        QCOMPARE(mm.getPendingInteractionIds(10).size(), 1);
        QVERIFY(mm.markInteractionSummaryFailed(id));
        QCOMPARE(mm.getPendingInteractionIds(10).size(), 0);
    }

    void recentGroupContextFindsUnprocessedMessage()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("recent_context.db");
        MemoryManager mm(dbPath);

        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30009"), QStringLiteral("message"),
                                  QStringLiteral("日常闲聊"), QStringLiteral("rc-1"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(),
                                  now.addSecs(-120)));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30009"), QStringLiteral("message"),
                                  QStringLiteral("我提议吃火锅"), QStringLiteral("rc-2"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(),
                                  now.addSecs(-60)));

        RecentContextQuery query;
        query.allowedGroupIds = {QStringLiteral("30009")};
        query.queryText = QStringLiteral("谁提议吃火锅");
        query.mode = RecentContextMode::TargetedLookup;
        query.since = now.addSecs(-3600);
        query.perGroupCandidateLimit = 50;
        query.itemLimit = 10;
        query.charBudget = 1200;

        const RecentContextBundle bundle = mm.searchRecentGroupContext(query);
        QVERIFY(!bundle.items.isEmpty());
        bool found = false;
        for (const RecentContextItem &item : bundle.items) {
            if (item.sanitizedText.contains(QStringLiteral("火锅"))) {
                found = true;
            }
        }
        QVERIFY(found);
    }

    void recentContextIsReadOnlyAndFiltersBlocked()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("recent_readonly.db");
        MemoryManager mm(dbPath);

        const QDateTime now = QDateTime::currentDateTime();
        RawEventWrite redacted;
        redacted.platform = QStringLiteral("qq");
        redacted.channel = Channel::kQQGroup;
        redacted.sessionId = QStringLiteral("30010");
        redacted.eventType = QStringLiteral("message");
        redacted.content = QStringLiteral("@770951546 这是 [REDACTED] 内容");
        redacted.messageId = QStringLiteral("rc-redacted");
        redacted.senderId = QStringLiteral("20002");
        redacted.senderName = QStringLiteral("小明");
        redacted.occurredAt = now.addSecs(-60);
        redacted.sensitive = true;
        redacted.redactionStatus = QStringLiteral("redacted");
        QVERIFY(mm.appendRawEvent(redacted));
        auto ordinary = redacted;
        ordinary.messageId = "rc-ordinary";
        ordinary.content = "@770951546 ordinary message";
        ordinary.sensitive = false;
        ordinary.redactionStatus = "none";
        QVERIFY(mm.appendRawEvent(ordinary));

        QVERIFY(mm.appendPrivacyBlockedEvent(
            QStringLiteral("qq"), Channel::kQQGroup, QStringLiteral("30010"),
            QStringLiteral("rc-blocked"), QStringLiteral("20002"),
            QStringLiteral("小明"), now.addSecs(-30),
            {QStringLiteral("rule")}, QStringLiteral("r3")));

        const int beforeUnprocessed = mm.countUnprocessedGroupEvents(
            QStringLiteral("30010"));
        const qlonglong beforeCursor = mm.getMemoryCursorEventId(
            Channel::kQQGroup, QStringLiteral("30010"));
        const QList<qlonglong> beforePending = mm.getPendingInteractionIds(10);

        RecentContextQuery query;
        query.allowedGroupIds = {QStringLiteral("30010")};
        query.viewerIds = {QStringLiteral("20002")};
        query.viewerPersonId = QStringLiteral("qq:20002");
        query.queryText = QStringLiteral("刚才群里聊什么了");
        query.mode = RecentContextMode::RecentOverview;
        query.since = now.addSecs(-3600);
        query.perGroupCandidateLimit = 50;
        query.itemLimitOverview = 50;
        query.overviewCharBudget = 2200;
        const RecentContextBundle bundle = mm.searchRecentGroupContext(query);

        QCOMPARE(bundle.items.size(), 1);
        QVERIFY(bundle.items.first().isViewer);
        QVERIFY(bundle.items.first().sanitizedText.contains("ordinary message"));
        QVERIFY(!bundle.items.first().sanitizedText.contains(QStringLiteral("770951546")));
        QCOMPARE(mm.countUnprocessedGroupEvents(QStringLiteral("30010")),
                 beforeUnprocessed);
        QCOMPARE(mm.getMemoryCursorEventId(Channel::kQQGroup,
                                           QStringLiteral("30010")),
                 beforeCursor);
        QCOMPARE(mm.getPendingInteractionIds(10), beforePending);
    }

    void recentContextSharesKnowledgeBoundaryBeforeBudget()
    {
        auto &config = ConfigManager::instance();
        const auto directory = config.getConfigDirPath();
        const auto settingPath = QDir(directory).filePath("setting.json");
        const auto identityPath = QDir(directory).filePath("identity.json");
        auto read = [](const QString &path) {
            QFile file(path);
            file.open(QIODevice::ReadOnly);
            return file.readAll();
        };
        const bool hadSetting = QFile::exists(settingPath), hadIdentity = QFile::exists(identityPath);
        const auto settings = read(settingPath), identity = read(identityPath);
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && file.write(data) == data.size();
        };
        const auto restore = qScopeGuard([&] {
            if (hadSetting) write(settingPath, settings); else QFile::remove(settingPath);
            if (hadIdentity) write(identityPath, identity); else QFile::remove(identityPath);
            config.loadSetting();
            config.reloadIdentity();
        });
        QVERIFY(write(settingPath, R"({"memory":{"privacy_rules":[{"id":"test","type":"keyword","value":"restricted","action":"block"}]}})"));
        QVERIFY(write(identityPath, R"({"memory":{"blind_zones":[{"channel":"qq_group","session_id":"hidden"}]}})"));
        QVERIFY(config.loadSetting());
        config.reloadIdentity();
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("boundary.db"));
        const auto now = QDateTime::currentDateTime();
        RawEventWrite event;
        event.platform = "qq";
        event.channel = Channel::kQQGroup;
        event.sessionId = "visible";
        event.senderId = "20002";
        event.eventType = "message";
        event.content = "eligible message";
        event.occurredAt = now.addSecs(-120);
        QVERIFY(db.appendRawEvent(event));
        for (const auto &variant : {"sensitive", "redacted", "out", "restricted", "hidden", "future", "other"}) {
            auto excluded = event;
            excluded.occurredAt = now.addSecs(-60);
            excluded.content = variant;
            if (excluded.content == "sensitive") excluded.sensitive = true;
            if (excluded.content == "redacted") excluded.redactionStatus = "redacted";
            if (excluded.content == "out") excluded.direction = "out";
            if (excluded.content == "hidden") excluded.sessionId = "hidden";
            if (excluded.content == "future") excluded.occurredAt = now.addSecs(60);
            if (excluded.content == "other") excluded.senderId = "30003";
            QVERIFY(db.appendRawEvent(excluded));
        }
        LocalHashEmbeddingProvider embedding;
        QString embedError;
        QVERIFY2(db.embedSearchChunks(&embedding, 1, &embedError), qPrintable(embedError));
        const auto excludedChunks = db.memoryChunksMissingEmbedding(20, embedding.modelId());
        QSet<QString> excludedTexts;
        for (const auto &chunk : excludedChunks)
            excludedTexts.insert(chunk.toObject()["text"].toString());
        QCOMPARE(excludedTexts, (QSet<QString>{"hidden", "restricted"}));
        RecentContextQuery query;
        query.allowedGroupIds = {"visible", "hidden"};
        query.viewerPersonId = "qq:20002";
        query.onlyViewer = true;
        query.since = now.addSecs(-3600);
        query.until = now;
        query.maxGroups = 1;
        query.perGroupCandidateLimit = 1;
        query.mode = RecentContextMode::RecentOverview;
        const auto result = db.searchRecentGroupContext(query);
        QCOMPARE(result.items.size(), 1);
        QCOMPARE(result.items.first().sanitizedText, QString("eligible message"));
        RetrievalRequest request;
        request.query = "restricted";
        request.sourceKinds = {"raw"};
        request.excludeCurrent = false;
        const auto indexed = MemoryRetrievalService(&db).retrieve(request);
        for (const auto &item : indexed.evidence) {
            QVERIFY(item.text != "restricted");
            QVERIFY(item.session != "hidden");
            QVERIFY(item.text != "out");
            QVERIFY(item.text != "sensitive");
            QVERIFY(item.text != "redacted");
        }
    }

    void recentContextDoesNotCrossGroups()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("recent_scope.db");
        MemoryManager mm(dbPath);

        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30011"), QStringLiteral("message"),
                                  QStringLiteral("A群消息"), QStringLiteral("rg-a"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(),
                                  now.addSecs(-60)));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30012"), QStringLiteral("message"),
                                  QStringLiteral("B群消息"), QStringLiteral("rg-b"),
                                  QStringLiteral("in"), QStringLiteral("20003"),
                                  QStringLiteral("小红"), QString(),
                                  now.addSecs(-30)));

        RecentContextQuery query;
        query.allowedGroupIds = {QStringLiteral("30011")};
        query.queryText = QStringLiteral("刚才群里聊什么了");
        query.mode = RecentContextMode::RecentOverview;
        query.since = now.addSecs(-3600);
        const RecentContextBundle bundle = mm.searchRecentGroupContext(query);

        QCOMPARE(bundle.items.size(), 1);
        QCOMPARE(bundle.items.first().sourceGroupId, QStringLiteral("30011"));
        QCOMPARE(bundle.items.first().sanitizedText, QStringLiteral("A群消息"));
    }

    void recentContextIgnoresSummaryState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath("recent_states.db");
        MemoryManager mm(dbPath);

        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30013"), QStringLiteral("message"),
                                  QStringLiteral("状态测试消息"), QStringLiteral("rs-1"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(),
                                  now.addSecs(-60)));
        const QList<RawEvent> events = mm.getUnprocessedGroupEvents(
            QStringLiteral("30013"), 10);
        QCOMPARE(events.size(), 1);

        InteractionDraft draft;
        draft.channel = Channel::kQQGroup;
        draft.sessionId = QStringLiteral("30013");
        draft.startedAt = events.first().occurredAt;
        draft.endedAt = events.first().occurredAt;
        const qlonglong id = mm.createInteractionBatch(draft, {events.first().id});
        QVERIFY(id > 0);

        RecentContextQuery query;
        query.allowedGroupIds = {QStringLiteral("30013")};
        query.queryText = QStringLiteral("刚才群里聊什么了");
        query.mode = RecentContextMode::RecentOverview;
        query.since = now.addSecs(-3600);
        QCOMPARE(mm.searchRecentGroupContext(query).items.size(), 1); // pending

        QVERIFY(mm.markInteractionSummaryFailed(id));
        QCOMPARE(mm.searchRecentGroupContext(query).items.size(), 1); // pending after retry
        QVERIFY(mm.markInteractionSummaryFailed(id));
        QVERIFY(mm.markInteractionSummaryFailed(id));
        QCOMPARE(mm.searchRecentGroupContext(query).items.size(), 1); // failed

        QVERIFY(mm.saveInteractionSummary(id, QStringLiteral("ok"), QStringLiteral("ready")));
        QCOMPARE(mm.searchRecentGroupContext(query).items.size(), 1); // ready
    }

    void personIdentitySeededAndBackfilled()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("identity.db"));

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30020"), QStringLiteral("message"),
                                  QStringLiteral("老板消息"), QStringLiteral("id-1"),
                                  QStringLiteral("in"), QStringLiteral("20001"),
                                  QStringLiteral("群主"), QString(), QDateTime::currentDateTime()));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30020"), QStringLiteral("message"),
                                  QStringLiteral("成员消息"), QStringLiteral("id-2"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(), QDateTime::currentDateTime()));

        mm.seedIdentity({QStringLiteral("20001")});
        const QList<RawEvent> events =
            mm.getUnprocessedGroupEvents(QStringLiteral("30020"), 10);
        QCOMPARE(events.size(), 2);
        QHash<QString, QString> persons;
        for (const RawEvent &event : events) {
            persons.insert(event.senderId, event.personId);
        }
        QCOMPARE(persons.value(QStringLiteral("20001")), QStringLiteral("owner"));
        QCOMPARE(persons.value(QStringLiteral("20002")), QStringLiteral("qq:20002"));

        const QStringList aliases =
            mm.getPersonAliases(QStringLiteral("owner"), QStringLiteral("30020"), 10);
        QVERIFY(aliases.contains(QStringLiteral("群主")));
        QCOMPARE(mm.resolvePersonId(QStringLiteral("qq"), QStringLiteral("20001")),
                 QStringLiteral("owner"));
        QCOMPARE(mm.resolvePersonId(QStringLiteral("qq"), QStringLiteral("20002")),
                 QStringLiteral("qq:20002"));
    }

    void seedRealDatabaseCopy()
    {
        const QString source = qEnvironmentVariable("MAKO_REAL_DB");
        if (source.isEmpty() || !QFile::exists(source)) {
            QSKIP("MAKO_REAL_DB not set");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString copy = dir.filePath("real.db");
        QVERIFY(QFile::copy(source, copy));

        MemoryManager mm(copy);
        mm.seedIdentity({QStringLiteral("2014366825")});
        RawDb raw(copy, "real_verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM raw_events WHERE person_id IS NULL").toInt(), 0);
        QCOMPARE(raw.scalar("SELECT COUNT(*) FROM interaction_participants "
                            "WHERE person_id IS NULL").toInt(), 0);
        QVERIFY(raw.scalar("SELECT COUNT(*) FROM person_identities").toInt() > 0);
    }

    void membershipSnapshotPeriodsAndFailClosed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("membership.db"));
        mm.seedIdentity({QStringLiteral("20001")});

        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.replaceGroupMembershipSnapshot(
            QStringLiteral("40001"),
            {{QStringLiteral("20001"), QStringLiteral("群主")},
             {QStringLiteral("20002"), QStringLiteral("小明")}},
            now));
        QVERIFY(mm.hasValidGroupMembershipSnapshot(QStringLiteral("40001"), 60, now));
        QCOMPARE(mm.getOpenGroupMemberIds(QStringLiteral("40001")).size(), 2);
        QCOMPARE(mm.getOpenGroupsForUser(QStringLiteral("20002"), 10),
                 QStringList{QStringLiteral("40001")});

        QVERIFY(mm.replaceGroupMembershipSnapshot(
            QStringLiteral("40001"),
            {{QStringLiteral("20002"), QStringLiteral("小明")}},
            now.addSecs(10)));
        QCOMPARE(mm.getOpenGroupMemberIds(QStringLiteral("40001")),
                 QStringList{QStringLiteral("20002")});

        QVERIFY(mm.replaceGroupMembershipSnapshot(
            QStringLiteral("40002"),
            {{QStringLiteral("20002"), QStringLiteral("小明")}}, now));
        QVERIFY(mm.markGroupMembershipSnapshotFailed(
            QStringLiteral("40002"), QStringLiteral("timeout"), now));
        QVERIFY(!mm.hasValidGroupMembershipSnapshot(QStringLiteral("40002"), 60, now));
        QCOMPARE(mm.getOpenGroupMemberIds(QStringLiteral("40002")),
                 QStringList{QStringLiteral("20002")});
        QCOMPARE(mm.getOpenGroupMemberIds(QStringLiteral("40001")),
                 QStringList{QStringLiteral("20002")});

        QVERIFY(mm.openGroupMembershipPeriod(
            QStringLiteral("40003"), QStringLiteral("20003"),
            QStringLiteral("小红"), QStringLiteral("group_increase"), now));
        QVERIFY(mm.getOpenGroupMemberIds(QStringLiteral("40003"))
                    .contains(QStringLiteral("20003")));
        QVERIFY(mm.closeGroupMembershipPeriod(
            QStringLiteral("40003"), QStringLiteral("20003"), now.addSecs(5)));
        QVERIFY(!mm.getOpenGroupMemberIds(QStringLiteral("40003"))
                     .contains(QStringLiteral("20003")));
        QVERIFY(mm.closeGroupMembershipPeriod(
            QStringLiteral("40003"), QStringLiteral("20003"), now.addSecs(6)));

        QVERIFY(!mm.hasValidGroupMembershipSnapshot(
            QStringLiteral("40001"), 60, now.addSecs(2 * 60 * 60)));
    }

    void onlyViewerRecentContextFiltersOthers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("only_viewer.db"));
        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30022"), QStringLiteral("message"),
                                  QStringLiteral("我自己的消息"), QStringLiteral("ov-1"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(), now.addSecs(-60)));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30022"), QStringLiteral("message"),
                                  QStringLiteral("别人的消息"), QStringLiteral("ov-2"),
                                  QStringLiteral("in"), QStringLiteral("20003"),
                                  QStringLiteral("小红"), QString(), now.addSecs(-30)));

        RecentContextQuery query;
        query.allowedGroupIds = {QStringLiteral("30022")};
        query.viewerIds = {QStringLiteral("20002")};
        query.viewerPersonId = QStringLiteral("qq:20002");
        query.onlyViewer = true;
        query.queryText = QStringLiteral("刚才群里聊什么了");
        query.mode = RecentContextMode::RecentOverview;
        query.since = now.addSecs(-3600);
        const RecentContextBundle bundle = mm.searchRecentGroupContext(query);
        QCOMPARE(bundle.items.size(), 1);
        QCOMPARE(bundle.items.first().sanitizedText, QStringLiteral("我自己的消息"));
    }

    void summarySegmentsAndClaimsUsePersonIds()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("claims.db"));
        mm.seedIdentity({QStringLiteral("20001")});

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30021"), QStringLiteral("message"),
                                  QStringLiteral("吃了烤肉拌饭"), QStringLiteral("id-1"),
                                  QStringLiteral("in"), QStringLiteral("20001"),
                                  QStringLiteral("群主"), QString(), QDateTime::currentDateTime()));
        const QList<RawEvent> events =
            mm.getUnprocessedGroupEvents(QStringLiteral("30021"), 10);
        QCOMPARE(events.size(), 1);

        InteractionDraft draft;
        draft.channel = Channel::kQQGroup;
        draft.sessionId = QStringLiteral("30021");
        draft.startedAt = events.first().occurredAt;
        draft.endedAt = events.first().occurredAt;
        draft.participants = {{QStringLiteral("20001"), QStringLiteral("群主")}};
        const qlonglong id = mm.createInteractionBatch(draft, {events.first().id});
        QVERIFY(id > 0);

        const QString segments = QStringLiteral(
            "[{\"person_id\":\"owner\",\"display_name\":\"群主\","
            "\"text\":\"吃了烤肉拌饭\"}]");
        ClaimDraft claim;
        claim.assertedByPersonId = QStringLiteral("owner");
        claim.aboutPersonId = QStringLiteral("owner");
        claim.privacyOwnerPersonId = QStringLiteral("owner");
        claim.statement = QStringLiteral("主人吃了烤肉拌饭");
        ClaimDraft thirdParty;
        thirdParty.assertedByPersonId = QStringLiteral("qq:20002");
        thirdParty.aboutPersonId = QStringLiteral("owner");
        thirdParty.privacyOwnerPersonId = QStringLiteral("owner");
        thirdParty.statement = QStringLiteral("第三方陈述");
        QVERIFY(mm.saveInteractionSummaryWithClaims(
            id, QStringLiteral("群主：吃了烤肉拌饭"), QStringLiteral("person_refs"),
            segments, {claim, thirdParty}, QStringLiteral("ready")));

        const QList<InteractionSummary> summaries =
            mm.getRecentInteractionSummaries(Channel::kQQGroup,
                                             QStringLiteral("30021"), 5);
        QCOMPARE(summaries.size(), 1);
        QCOMPARE(summaries.first().summaryFormat, QStringLiteral("person_refs"));
        QCOMPARE(summaries.first().summarySegments, segments);

        const QList<ClaimDraft> claims = mm.getClaimsForInteraction(id);
        QCOMPARE(claims.size(), 2);
        QCOMPARE(claims.first().assertedByPersonId, QStringLiteral("owner"));
        QCOMPARE(claims.first().aboutPersonId, QStringLiteral("owner"));
        QCOMPARE(claims.first().privacyOwnerPersonId, QStringLiteral("owner"));
        QCOMPARE(claims.first().claimStatus, QStringLiteral("active"));
        QCOMPARE(claims.at(1).claimStatus, QStringLiteral("quarantine"));
        QCOMPARE(mm.getActiveClaims(10).size(), 1);
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

    // B4：主动联系门控解耦后的重试状态机（rejected 补一次；unknown/已送达不重发）。
    void contactRetriesOnceAfterRejection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("contact-retry.db");
        MemoryManager db(path);

        const auto message = QStringLiteral("在忙吗");
        const auto note = QStringLiteral("荷包蛋好不好吃");
        // contact now 必须有可核验依据：这里用"关注之后主人真实说过的一句话"（new_owner_evidence）。
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("明天想吃荷包蛋"));
        QVERIFY(evidence > 0);
        const qint64 turn = seedOpenConcern(db, concernTurnMeta(QStringLiteral("contact-retry-a")),
                                            QStringLiteral("明天想做荷包蛋，不知道好不好吃"), note,
                                            QJsonObject{},
                                            contactNow(QStringLiteral("又有新消息"), message,
                                                       ownerEvidenceBasis(evidence)));
        QVERIFY(turn > 0);
        // private_writing=false（enableContact 里固定为 false）时仍可领取并投递。
        QCOMPARE(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession), message);
        QVERIFY(db.recordContactOutcome(turn, 1, "rejected"));
        // 渠道拒绝一次：允许补发一次。
        QCOMPARE(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession), message);
        QVERIFY(db.recordContactOutcome(turn, 1, "rejected"));
        // 第二次拒绝后不再领取。
        QVERIFY(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());

        // unknown：协议层可能已送达，不重发。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("contact-retry-clock")));
        const qint64 unknownTurn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("contact-retry-b")),
            QStringLiteral("最近想看海"), QStringLiteral("想看海"),
            QJsonObject{}, contactNow(QStringLiteral("想问问"), QStringLiteral("最近还好吗"),
                                      ownerEvidenceBasis(evidence)));
        QVERIFY(unknownTurn > 0);
        QVERIFY(!db.claimReflectionContact(unknownTurn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        QVERIFY(db.recordContactOutcome(unknownTurn, 1, "unknown"));
        QVERIFY(db.claimReflectionContact(unknownTurn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());

        // protocol_accepted：已送达，不重发。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("contact-retry-clock2")));
        const qint64 acceptedTurn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("contact-retry-c")),
            QStringLiteral("昨天的饭很香"), QStringLiteral("饭菜很香"),
            QJsonObject{}, contactNow(QStringLiteral("想问问"), QStringLiteral("吃饭了吗"),
                                      ownerEvidenceBasis(evidence)));
        QVERIFY(acceptedTurn > 0);
        QVERIFY(!db.claimReflectionContact(acceptedTurn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        QVERIFY(db.recordContactOutcome(acceptedTurn, 1, "protocol_accepted"));
        QVERIFY(db.claimReflectionContact(acceptedTurn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());

        RawDb raw(path, "contact-retry-raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM reflection_contact WHERE turn_id=%1").arg(turn)).toInt(), 2);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=%1").arg(turn)).toString(),
                 QStringLiteral("rejected"));
        QVERIFY(raw.scalar(QStringLiteral("SELECT last_attempt_at FROM reflection_contact WHERE turn_id=%1").arg(turn)).toLongLong() > 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM reflection_contact WHERE turn_id=%1").arg(unknownTurn)).toInt(), 1);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=%1").arg(acceptedTurn)).toString(),
                 QStringLiteral("protocol_accepted"));
        // 依据字段与联系意图同一行落库，供事后追溯"凭什么现在联系"。
        QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1").arg(turn)).toString(),
                 QStringLiteral("new_owner_evidence"));
        QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1").arg(turn)).toString()
                    .contains(QStringLiteral("chat/%1").arg(evidence)));
        // 只有仍可重试的联系会进入重试清单。
        QVERIFY(db.retryableReflectionContacts(4).isEmpty());
    }

    // B4：主动消息写历史/原始流水，下次对话能回认；且不参与回顾/认识更新。
    void proactiveMessageRecordedForRecall()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("proactive-record.db");
        MemoryManager db(path);
        const qlonglong id = db.recordProactiveMessage(Channel::kQQPrivate, QStringLiteral("2014366825"),
                                                       QStringLiteral("晚安呀"), QStringLiteral("contact"),
                                                       QStringLiteral("reflection/1"));
        QVERIFY(id > 0);
        RawDb raw(path, "proactive-record");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT turn_kind FROM chat_history").toString(), QStringLiteral("proactive"));
        QCOMPARE(raw.scalar("SELECT user_input IS NULL FROM chat_history").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT turn_state FROM chat_history").toString(), QStringLiteral("handed_off"));
        QCOMPARE(raw.scalar("SELECT speaker FROM chat_history").toString(), QStringLiteral("mako"));
        QCOMPARE(raw.scalar("SELECT subject FROM chat_history").toString(), QStringLiteral("user"));
        QCOMPARE(raw.scalar("SELECT sensitive FROM chat_history").toInt(), 0);
        QVERIFY(raw.scalar("SELECT cognitive_event IS NULL FROM chat_history").toBool());
        const auto reply = QJsonDocument::fromJson(
            raw.scalar("SELECT raw_reply FROM chat_history").toString().toUtf8()).object();
        QCOMPARE(reply["sentences"].toArray().first().toObject()["zh_text"].toString(), QStringLiteral("晚安呀"));
        QCOMPARE(reply["delivery"].toObject()["kind"].toString(), QStringLiteral("contact"));
        QCOMPARE(reply["delivery"].toObject()["ref"].toString(), QStringLiteral("reflection/1"));
        // 原始事件：direction='out'，平台按渠道推导。
        QCOMPARE(raw.scalar("SELECT count(*) FROM raw_events WHERE direction='out'").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT content FROM raw_events WHERE direction='out'").toString(), QStringLiteral("晚安呀"));
        QCOMPARE(raw.scalar("SELECT platform FROM raw_events WHERE direction='out'").toString(), QStringLiteral("qq"));
        QCOMPARE(raw.scalar("SELECT event_type FROM raw_events WHERE direction='out'").toString(), QStringLiteral("message"));
        QCOMPARE(raw.scalar("SELECT sensitive FROM raw_events WHERE direction='out'").toInt(), 0);
        QCOMPARE(raw.scalar("SELECT redaction_status FROM raw_events WHERE direction='out'").toString(), QStringLiteral("none"));
        // 索引收录主动文本（assistant 块），且没有空的 turn_user 块。
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM memory_chunks WHERE source_type='chat' "
                                           "AND source_ref LIKE 'chat/%1:%%' AND text LIKE '%%晚安呀%%'").arg(id)).toInt(), 1);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM memory_chunks WHERE source_type='chat' "
                                           "AND source_ref LIKE 'chat/%1:%%' AND chunk_kind='turn_user'").arg(id)).toInt(), 0);
        // 历史回读：getHistoryTurn 能看到这一轮，turnKind='proactive'、没有用户输入。
        const QList<HistoryTurn> turns = db.getHistoryTurn(5, Channel::kQQPrivate, QStringLiteral("2014366825"));
        QCOMPARE(turns.size(), 1);
        QCOMPARE(turns.first().turnKind, QStringLiteral("proactive"));
        QVERIFY(turns.first().userInput.isEmpty());
        QCOMPARE(turns.first().speaker, QStringLiteral("mako"));
        // 不是用户发言：不进入私下回顾（回顾只领 turn_kind='reply' 的轮次）。
        QVERIFY(db.claimPrivateReflection(12, 60).isEmpty());
    }

    // B4：分享回执写 thought_result + delivery_ledger；已送达/未知不重发，被拒只补一次。
    void shareOutcomeBlocksRepeat()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("share-outcome.db");
        MemoryManager db(path);

        QVERIFY(db.saveQATurn(QStringLiteral("试试写诗"), R"({"personal_interest":"想写雨声"})", {}));
        const auto first = db.nextThought(true);
        const qlonglong firstId = first["id"].toInteger();
        const int firstVersion = first["version"].toInt();
        QVERIFY(firstId > 0);
        QVERIFY(db.finishThoughtReview(firstId, firstVersion,
            QJsonObject{{"status","completed"},{"reflection","写好了两句"},
                        {"draft","屋檐轻响，雨在窗上停了一停。"}}));
        const int sharedVersion = firstVersion + 1;
        QVERIFY(db.canShareThought(firstId, sharedVersion));

        // 渠道拒绝：允许补发一次；第二次拒绝后不再分享。
        QVERIFY(db.recordShareOutcome(firstId, sharedVersion, "rejected",
                                      Channel::kPet, Channel::kDefaultSession));
        QVERIFY(db.canShareThought(firstId, sharedVersion));
        QVERIFY(db.recordShareOutcome(firstId, sharedVersion, "rejected",
                                      Channel::kPet, Channel::kDefaultSession));
        QVERIFY(!db.canShareThought(firstId, sharedVersion));

        // 已送达（protocol_accepted）不再分享。
        QVERIFY(db.saveQATurn(QStringLiteral("再写一段"), R"({"personal_interest":"想写风声"})", {}));
        const auto second = db.nextThought(true);
        const qlonglong secondId = second["id"].toInteger();
        const int secondVersion = second["version"].toInt();
        QVERIFY(secondId > 0 && secondId != firstId);
        QVERIFY(db.finishThoughtReview(secondId, secondVersion,
            QJsonObject{{"status","completed"},{"reflection","写好了两句"},
                        {"draft","风从窗缝里钻进来。"}}));
        const int secondShared = secondVersion + 1;
        QVERIFY(db.canShareThought(secondId, secondShared));
        QVERIFY(db.recordShareOutcome(secondId, secondShared, "protocol_accepted",
                                      Channel::kQQPrivate, QStringLiteral("2014366825")));
        QVERIFY(!db.canShareThought(secondId, secondShared));

        RawDb raw(path, "share-outcome");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.share_outcome') "
                                           "FROM chat_history WHERE id=%1").arg(firstId)).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger WHERE kind='share'")).toInt(), 2);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='share' AND ref='thought:%1:%2'")
                                .arg(firstId).arg(sharedVersion)).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE kind='share' AND ref='thought:%1:%2'")
                                .arg(firstId).arg(sharedVersion)).toInt(), 2);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='share' AND ref='thought:%1:%2'")
                                .arg(secondId).arg(secondShared)).toString(),
                 QStringLiteral("protocol_accepted"));
    }

    // B4：v24 迁移结构 + 领取/回执在 delivery_ledger 上的状态。
    void deliveryLedgerStates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("ledger.db");
        {
            MemoryManager db(path);
        }
        {
            RawDb raw(path, "ledger-schema");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);
            const QStringList contact = raw.columns("reflection_contact");
            for (const auto &column : {QStringLiteral("attempts"), QStringLiteral("last_outcome"),
                                       QStringLiteral("last_attempt_at"),
                                       QStringLiteral("basis_type"), QStringLiteral("basis_refs")})
                QVERIFY2(contact.contains(column), qPrintable(column));
            const QStringList ledger = raw.columns("delivery_ledger");
            for (const auto &column : {QStringLiteral("kind"), QStringLiteral("ref"), QStringLiteral("channel"),
                                       QStringLiteral("session"), QStringLiteral("text"), QStringLiteral("state"),
                                       QStringLiteral("attempts"), QStringLiteral("last_error"),
                                       QStringLiteral("created_at"), QStringLiteral("updated_at")})
                QVERIFY2(ledger.contains(column), qPrintable(column));
        }

        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        MemoryManager db(path);
        // contact now 的合法依据：关注之后主人的一条真实发言（new_owner_evidence）。
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("后来又说了一句"));
        QVERIFY(evidence > 0);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("ledger-a")),
            QStringLiteral("明天想做荷包蛋，不知道好不好吃"), QStringLiteral("荷包蛋好不好吃"),
            QJsonObject{}, contactNow(QStringLiteral("又有新消息"), QStringLiteral("在忙吗"),
                                      ownerEvidenceBasis(evidence)));
        QVERIFY(turn > 0);
        QVERIFY(!db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        const auto ref = QStringLiteral("reflection:%1:1").arg(turn);
        {
            RawDb raw(path, "ledger-claimed");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toString(),
                     QStringLiteral("claimed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT text FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toString(),
                     QStringLiteral("在忙吗"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toInt(), 0);
        }
        QVERIFY(db.recordContactOutcome(turn, 1, "rejected"));
        {
            RawDb raw(path, "ledger-rejected");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toString(),
                     QStringLiteral("rejected"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toInt(), 1);
        }
        // 重试领取后状态回到 claimed，再次终态写 protocol_accepted 时不再累加失败次数。
        QVERIFY(!db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        QVERIFY(db.recordContactOutcome(turn, 1, "protocol_accepted"));
        {
            RawDb raw(path, "ledger-accepted");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE kind='contact' AND ref='%1'").arg(ref)).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=%1").arg(turn)).toString(),
                     QStringLiteral("protocol_accepted"));
        }
    }

    // R3：普通回复的投递账本状态机。claim → 回执落终态；只有 rejected 且 attempts<2
    // 进入补发清单；unknown 表示协议层可能已送达，永不重发。
    void replyDeliveryLedgerTracksOutcomes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("reply-delivery.db");
        MemoryManager db(path);

        // 首次 claim：恰好一行，state=claimed、attempts=0。
        const auto acceptedRef = QStringLiteral("reply:530:0");
        QVERIFY(db.recordDelivery(QStringLiteral("reply"), acceptedRef, Channel::kQQPrivate,
                                  QStringLiteral("2014366825"), QStringLiteral("你好")));
        {
            RawDb raw(path, "reply-delivery-claimed");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger WHERE kind='reply'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='reply' AND ref='%1'").arg(acceptedRef)).toString(),
                     QStringLiteral("claimed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE kind='reply' AND ref='%1'").arg(acceptedRef)).toInt(), 0);
        }
        // 协议层接受：终态，不再可补发。
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("reply"), acceptedRef,
                                         QStringLiteral("protocol_accepted")));
        QVERIFY(db.retryableDeliveries(QStringLiteral("reply")).isEmpty());

        // 渠道明确拒绝：进入补发清单，返回字段可回读。
        const auto rejectedRef = QStringLiteral("reply:531:0");
        QVERIFY(db.recordDelivery(QStringLiteral("reply"), rejectedRef, Channel::kQQGroup,
                                  QStringLiteral("30021"), QStringLiteral("第一句")));
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("reply"), rejectedRef,
                                         QStringLiteral("rejected"),
                                         QStringLiteral("channel_unavailable")));
        {
            const QJsonArray retries = db.retryableDeliveries(QStringLiteral("reply"));
            QCOMPARE(retries.size(), 1);
            const QJsonObject job = retries.first().toObject();
            QCOMPARE(job["kind"].toString(), QStringLiteral("reply"));
            QCOMPARE(job["ref"].toString(), rejectedRef);
            QCOMPARE(job["channel"].toString(), Channel::kQQGroup);
            QCOMPARE(job["session"].toString(), QStringLiteral("30021"));
            QCOMPARE(job["text"].toString(), QStringLiteral("第一句"));
            QCOMPARE(job["attempts"].toInt(), 0);
            QCOMPARE(job["state"].toString(), QStringLiteral("rejected"));
        }
        // 补发前先占一次尝试：state 回到 claimed，因此下一拍不会重复领取。
        QVERIFY(db.recordDeliveryAttempt(QStringLiteral("reply"), rejectedRef));
        QVERIFY(db.retryableDeliveries(QStringLiteral("reply")).isEmpty());
        {
            RawDb raw(path, "reply-delivery-attempt");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE kind='reply' AND ref='%1'").arg(rejectedRef)).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='reply' AND ref='%1'").arg(rejectedRef)).toString(),
                     QStringLiteral("claimed"));
        }
        // 再被拒（attempts=1）：仍允许最后一次补发。
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("reply"), rejectedRef,
                                         QStringLiteral("rejected")));
        QCOMPARE(db.retryableDeliveries(QStringLiteral("reply")).size(), 1);
        QVERIFY(db.recordDeliveryAttempt(QStringLiteral("reply"), rejectedRef));
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("reply"), rejectedRef,
                                         QStringLiteral("rejected")));
        // attempts=2：不再补发。
        QVERIFY(db.retryableDeliveries(QStringLiteral("reply")).isEmpty());

        // unknown：协议层可能已送达，attempts 再小也永不进入补发清单。
        const auto unknownRef = QStringLiteral("reply:532:0");
        QVERIFY(db.recordDelivery(QStringLiteral("reply"), unknownRef, Channel::kQQPrivate,
                                  QStringLiteral("2014366825"), QStringLiteral("不确定")));
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("reply"), unknownRef,
                                         QStringLiteral("unknown")));
        QVERIFY(db.retryableDeliveries(QStringLiteral("reply")).isEmpty());

        // 写失败返回 false 而不是抛异常（行不存在 / 空键）。
        QVERIFY(!db.recordDeliveryOutcome(QStringLiteral("reply"), QStringLiteral("reply:999:0"),
                                          QStringLiteral("rejected")));
        QVERIFY(!db.recordDeliveryAttempt(QStringLiteral("reply"), QStringLiteral("reply:999:0")));
        QVERIFY(!db.recordDelivery(QString(), acceptedRef, QString(), QString(), QString()));
        QVERIFY(db.retryableDeliveries(QStringLiteral("reply"), 0).isEmpty());
    }

    // R3：同一 (kind,ref) 重复 claim 不清零 attempts，只刷新 text/路由（重启后重发同一句）。
    void deliveryUpsertPreservesAttempts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("delivery-upsert.db");
        MemoryManager db(path);
        const auto kind = QStringLiteral("reply");
        const auto ref = QStringLiteral("reply:601:0");
        QVERIFY(db.recordDelivery(kind, ref, Channel::kQQPrivate,
                                  QStringLiteral("2014366825"), QStringLiteral("旧文本")));
        QVERIFY(db.recordDeliveryAttempt(kind, ref));
        QVERIFY(db.recordDeliveryOutcome(kind, ref, QStringLiteral("rejected"),
                                         QStringLiteral("channel_unavailable")));
        // 重启后重发同一句：upsert 刷新路由/原文，但 attempts 保留。
        QVERIFY(db.recordDelivery(kind, ref, Channel::kQQGroup,
                                  QStringLiteral("30021"), QStringLiteral("新文本")));
        {
            RawDb raw(path, "delivery-upsert");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger WHERE kind='reply'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM delivery_ledger WHERE ref='%1'").arg(ref)).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE ref='%1'").arg(ref)).toString(),
                     QStringLiteral("claimed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT text FROM delivery_ledger WHERE ref='%1'").arg(ref)).toString(),
                     QStringLiteral("新文本"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT channel FROM delivery_ledger WHERE ref='%1'").arg(ref)).toString(),
                     Channel::kQQGroup);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT session FROM delivery_ledger WHERE ref='%1'").arg(ref)).toString(),
                     QStringLiteral("30021"));
        }
    }

    // M1 接线覆盖：到期关注经 PrivateReview 统一唤醒，claim 的上下文带 kind=concern。
    void dueConcernRoutedThroughPrivateReview()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-review.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("concern-review-a")),
            QStringLiteral("明天想做荷包蛋，不知道好不好吃"), QStringLiteral("荷包蛋好不好吃"),
            QJsonObject{{"topic", QStringLiteral("荷包蛋")},
                        {"watch", QStringLiteral("味道如何")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        {
            RawDb raw(path, "concern-review-due");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                            "WHERE turn_id=%1").arg(turn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-review-clock")));

        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        review.wake(12, 60);
        QCOMPARE(requested.size(), 1);
        const auto context = requested.first()[2].toJsonObject();
        QCOMPARE(context["kind"].toString(), QStringLiteral("concern"));
        QCOMPARE(context["concern"].toObject()["topic"].toString(), QStringLiteral("荷包蛋"));
        review.receive(requested.first()[0].toLongLong(), requested.first()[1].toInt(),
                       QJsonObject{{"concern_action", "keep"},
                                   {"forget", QJsonObject{{"next_check_after_minutes", 0}}}}, {});
        RawDb raw(path, "concern-review-state");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn)).toString(),
                 QStringLiteral("consider"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT next_check_at FROM private_reflection WHERE turn_id=%1").arg(turn)).toLongLong(),
                 qlonglong(0));
    }

    // B3：到期关注的领取、了结与主动联系意图（不依赖新用户消息）。
    void dueConcernResolveAndContact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-due.db");
        MemoryManager db(path);
        const qint64 resolveTurn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("concern-due-a")),
            QStringLiteral("明天要做一顿早饭"), QStringLiteral("不知道他有没有吃早饭"),
            QJsonObject{{"topic", QStringLiteral("早饭")},
                        {"watch", QStringLiteral("他有没有吃、什么时候吃")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(resolveTurn > 0);
        {
            RawDb raw(path, "concern-due-backfill");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT root_turn FROM private_reflection WHERE turn_id=%1")
                                    .arg(resolveTurn)).toLongLong(), resolveTurn);
            QVERIFY(raw.scalar(QStringLiteral("SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                                   .arg(resolveTurn)).toLongLong() > 0);
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                            "WHERE turn_id=%1").arg(resolveTurn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-due-clock")));
        const auto claimed = db.claimDueConcern(12, 60);
        QCOMPARE(claimed["id"].toInteger(), resolveTurn);
        QCOMPARE(claimed["kind"].toString(), QStringLiteral("concern"));
        QVERIFY(db.finishConcernReview(resolveTurn, claimed["version"].toInt(),
                                       QJsonObject{{"concern_action", "resolve"},
                                                   {"kind", "answer"}}));
        QVERIFY(db.concernByHead(resolveTurn).isEmpty());
        {
            RawDb raw(path, "concern-due-state");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(resolveTurn)).toString(), QStringLiteral("resolved"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT resolved_kind FROM private_reflection WHERE turn_id=%1")
                                    .arg(resolveTurn)).toString(), QStringLiteral("answer"));
        }

        // 第二个关注：到期后决定"现在联系"，应留下 proposed 联系记录并保持开放。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-due-clock2")));
        const qint64 contactTurn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("concern-due-b")),
            QStringLiteral("他说晚饭随便吃点"), QStringLiteral("晚饭到底吃了什么"),
            QJsonObject{{"topic", QStringLiteral("晚饭")},
                        {"watch", QStringLiteral("他最后吃了什么")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(contactTurn > 0);
        {
            RawDb raw(path, "concern-due-due2");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                            "WHERE turn_id=%1").arg(contactTurn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-due-clock3")));
        const auto claimed2 = db.claimDueConcern(12, 60);
        QCOMPARE(claimed2["id"].toInteger(), contactTurn);
        // contact now 的合法依据：关注来源之后主人真实说过的一句话（new_owner_evidence）。
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("晚饭吃了面"));
        QVERIFY(evidence > 0);
        QVERIFY(db.finishConcernReview(contactTurn, claimed2["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(
                                                        QStringLiteral("主人后来提到晚饭"),
                                                        QStringLiteral("晚饭吃了吗？"),
                                                        ownerEvidenceBasis(evidence))}}));
        {
            RawDb raw(path, "concern-due-contact");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                    .arg(contactTurn)).toString(), QStringLiteral("now"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                    .arg(contactTurn)).toString(), QStringLiteral("proposed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                    .arg(contactTurn)).toString(), QStringLiteral("new_owner_evidence"));
            QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                                   .arg(contactTurn)).toString()
                        .contains(QStringLiteral("chat/%1").arg(evidence)));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(contactTurn)).toString(), QStringLiteral("consider"));
            QVERIFY(raw.scalar(QStringLiteral("SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                                   .arg(contactTurn)).toLongLong() > 0);
            // 到期回顾的决策流水照旧，依据核验通过时记录 contact 与 timing。
            QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1")
                                    .arg(contactTurn)).toString(), QStringLiteral("contact"));
        }
    }

    // v27：contact now 缺少 basis 时服务端拒绝投递，但要把关注按 keep 放回队列，不能无限 defer。
    void contactNowRequiresBasis()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-missing.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("basis-missing-a")),
            QStringLiteral("明天想做荷包蛋，不知道好不好吃"), QStringLiteral("荷包蛋好不好吃"),
            QJsonObject{{"topic", QStringLiteral("荷包蛋")},
                        {"watch", QStringLiteral("味道如何")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("basis-missing"));
        QCOMPARE(claim["id"].toInteger(), turn);
        const auto before = QDateTime::currentSecsSinceEpoch();
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("到点了"),
                                                                          QStringLiteral("在忙吗"))}}));
        RawDb raw(path, "basis-missing-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toInt(), 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("consider"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT claimed_by IS NULL FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toInt(), 1);
        // 按 keep 落库：约 30 分钟后可再检查，而不是卡在 pending。
        QVERIFY(raw.scalar(QStringLiteral("SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                               .arg(turn)).toLongLong() >= before + 1700);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                           "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                 QStringLiteral("contact_rejected"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT contact_timing IS NULL FROM concern_revision WHERE turn=%1 "
                                           "ORDER BY id DESC LIMIT 1").arg(turn)).toInt(), 1);
        // 原 note 不被改写。
        QCOMPARE(raw.scalar(QStringLiteral("SELECT note FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("荷包蛋好不好吃"));
    }

    // v27：编造 chat/<id> 的"主人证据"同样被拒绝。
    void contactNowRejectsFabricatedEvidence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-fabricated.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("basis-fabricated-a")),
            QStringLiteral("他说晚上会早点睡"), QStringLiteral("有没有真的早睡"),
            QJsonObject{{"topic", QStringLiteral("作息")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const QJsonObject fabricated{{"type", QStringLiteral("new_owner_evidence")},
                                     {"refs", QJsonArray{QStringLiteral("chat/999999")}}};
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("basis-fabricated"));
        QCOMPARE(claim["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("有话要说"),
                                                                          QStringLiteral("睡了吗"),
                                                                          fabricated)}}));
        RawDb raw(path, "basis-fabricated-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toInt(), 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("consider"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                           "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                 QStringLiteral("contact_rejected"));
    }

    // v28：关注之后主人的真实发言（new_owner_evidence）是合法依据，且依据随联系意图落库。
    void contactNowAcceptsOwnerEvidenceBasis()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-owner.db");
        MemoryManager db(path);
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("荷包蛋我吃过了"));
        QVERIFY(evidence > 0);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("basis-ok-a")),
            QStringLiteral("明天想做荷包蛋，不知道好不好吃"), QStringLiteral("荷包蛋好不好吃"),
            QJsonObject{{"topic", QStringLiteral("荷包蛋")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("basis-ok"));
        QCOMPARE(claim["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("主人后来提过"),
                                                                          QStringLiteral("在忙吗"),
                                                                          ownerEvidenceBasis(evidence))}}));
        RawDb raw(path, "basis-ok-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("now"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("proposed"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("new_owner_evidence"));
        QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                               .arg(turn)).toString()
                    .contains(QStringLiteral("chat/%1").arg(evidence)));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("consider"));
    }

    // v27：收口（close/defer）之前的发言不能当新证据；收口之后的新发言才可以。
    void contactNowRejectsPreClosureEvidence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-closure.db");
        MemoryManager db(path);
        RequestMeta metaA = concernTurnMeta(QStringLiteral("closure-a"));
        metaA.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-300);
        const qint64 turn = seedOpenConcern(
            db, metaA, QStringLiteral("周末想去看展览"), QStringLiteral("展览怎么样"),
            QJsonObject{{"topic", QStringLiteral("展览")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        // 先有一段收口的交流：B 是 quiet close，形成 recent_closure(close)。
        RequestMeta metaB = concernTurnMeta(QStringLiteral("closure-b"));
        metaB.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-120);
        const qint64 closed = db.recordInput(QStringLiteral("先聊到这里吧"), metaB, true);
        QVERIFY(closed > 0);
        QVERIFY(db.recordQuiet(closed, "close"));
        const auto closure = db.recentConversationClosure(Channel::kPet, Channel::kDefaultSession,
                                                          QStringLiteral("user"));
        QCOMPARE(closure["action"].toString(), QStringLiteral("close"));
        QCOMPARE(closure["turn"].toInteger(), closed);
        QVERIFY(closure["age_seconds"].toInteger() >= 0);
        QVERIFY(!closure["reported_at"].toString().isEmpty());
        // 关注来源自身不晚于自己，不能算"之后的新证据"。
        const auto claimA = reclaimDueConcern(db, path, turn, QStringLiteral("closure-claim-a"));
        QCOMPARE(claimA["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimA["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("到点了"),
                                                                          QStringLiteral("展览怎么样"),
                                                                          ownerEvidenceBasis(turn))}}));
        {
            RawDb raw(path, "closure-a-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("consider"));
        }
        // 收口那一轮自身也不能算"收口之后"。
        const auto claimB = reclaimDueConcern(db, path, turn, QStringLiteral("closure-claim-b"));
        QCOMPARE(claimB["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimB["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("到点了"),
                                                                          QStringLiteral("先聊到这里吧"),
                                                                          ownerEvidenceBasis(closed))}}));
        {
            RawDb raw(path, "closure-b-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toInt(), 0);
        }
        // 收口之后主人真的又说话了（未收口的新轮次），这才是合法的新证据。
        RequestMeta metaC = concernTurnMeta(QStringLiteral("closure-c"));
        metaC.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-10);
        const qint64 newer = db.recordInput(QStringLiteral("展览我去啦，很好看"), metaC, true);
        QVERIFY(newer > 0);
        const auto claimC = reclaimDueConcern(db, path, turn, QStringLiteral("closure-claim-c"));
        QCOMPARE(claimC["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimC["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("展览有了新进展"),
                                                                          QStringLiteral("展览怎么样"),
                                                                          ownerEvidenceBasis(newer))}}));
        RawDb raw(path, "closure-c-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("now"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("new_owner_evidence"));
        QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                               .arg(turn)).toString()
                    .contains(QStringLiteral("chat/%1").arg(newer)));
    }

    // v27：已分享过的活动结果不能当"有了新结果"；未分享的完成结果可以通过。
    void contactNowRejectsAlreadySharedArtifact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-artifact.db");
        MemoryManager db(path);
        // 活动一：完成且有产物，并已作为分享送达。
        QVERIFY(db.saveQATurn(QStringLiteral("试试写诗"), R"({"personal_interest":"想写雨声"})", {}));
        const auto first = db.nextThought(true);
        const qlonglong firstId = first["id"].toInteger();
        const int firstVersion = first["version"].toInt();
        QVERIFY(firstId > 0);
        QVERIFY(db.finishThoughtReview(firstId, firstVersion,
            QJsonObject{{"status", "completed"}, {"reflection", "写好了两句"},
                        {"draft", "屋檐轻响，雨在窗上停了一停。"}}));
        const int sharedVersion = firstVersion + 1;
        const auto shareRef = QStringLiteral("thought:%1:%2").arg(firstId).arg(sharedVersion);
        QVERIFY(db.recordDelivery(QStringLiteral("share"), shareRef, Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("屋檐轻响，雨在窗上停了一停。")));
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("share"), shareRef,
                                         QStringLiteral("protocol_accepted")));
        // 活动二：同样完成且有产物，但还没有分享过。
        QVERIFY(db.saveQATurn(QStringLiteral("再写一段"), R"({"personal_interest":"想写风声"})", {}));
        const auto second = db.nextThought(true);
        const qlonglong secondId = second["id"].toInteger();
        const int secondVersion = second["version"].toInt();
        QVERIFY(secondId > 0 && secondId != firstId);
        QVERIFY(db.finishThoughtReview(secondId, secondVersion,
            QJsonObject{{"status", "completed"}, {"reflection", "写好了两句"},
                        {"draft", "风从窗缝里钻进来。"}}));
        const auto activity = db.activityById(secondId);
        QCOMPARE(activity["result"].toObject()["status"].toString(), QStringLiteral("completed"));
        QVERIFY(!activity["result"].toObject()["artifact_ref"].toString().isEmpty());

        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("basis-artifact-a")),
            QStringLiteral("最近想写点东西"), QStringLiteral("写出来了吗"),
            QJsonObject{{"topic", QStringLiteral("写作")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        // 已分享的产物不算"还没告诉主人"的新结果。
        const auto claimShared = reclaimDueConcern(db, path, turn, QStringLiteral("artifact-shared"));
        QCOMPARE(claimShared["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimShared["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("写好了"),
                                                                          QStringLiteral("写了一段"),
                                                                          activityResultBasis(firstId))}}));
        {
            RawDb raw(path, "artifact-shared-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("consider"));
        }
        // 未分享的另一份完成产物可以通过。
        const auto claimFresh = reclaimDueConcern(db, path, turn, QStringLiteral("artifact-fresh"));
        QCOMPARE(claimFresh["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimFresh["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("写好了"),
                                                                          QStringLiteral("写了一段"),
                                                                          activityResultBasis(secondId))}}));
        RawDb raw(path, "artifact-fresh-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("activity_result"));
        QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                               .arg(turn)).toString()
                    .contains(QStringLiteral("thought/%1").arg(secondId)));
    }

    // v27：茉子自己的主动消息（turn_kind='proactive'）不是主人委托，不能当新证据。
    void contactNowRejectsProactiveEvidence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-proactive.db");
        MemoryManager db(path);
        const qlonglong proactive = db.recordProactiveMessage(
            Channel::kPet, Channel::kDefaultSession, QStringLiteral("晚安呀"),
            QStringLiteral("contact"), QStringLiteral("reflection/1"));
        QVERIFY(proactive > 0);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("basis-proactive-a")),
            QStringLiteral("他说最近睡得晚"), QStringLiteral("有没有早点睡"),
            QJsonObject{{"topic", QStringLiteral("作息")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("basis-proactive"));
        QCOMPARE(claim["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(QStringLiteral("想问问"),
                                                                          QStringLiteral("睡了吗"),
                                                                          ownerEvidenceBasis(proactive))}}));
        RawDb raw(path, "basis-proactive-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toInt(), 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("consider"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                           "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                 QStringLiteral("contact_rejected"));
    }

    // v27：新来源回顾里的 contact now 同样需要依据；被拒时不写 reflection_contact，但保留 consider。
    void reflectionContactAlsoRequiresBasis()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("reflection-basis.db");
        MemoryManager db(path);
        RequestMeta meta = concernTurnMeta(QStringLiteral("reflection-basis-a"));
        const qint64 turn = db.recordInput(QStringLiteral("今天有点累"), meta, true);
        QVERIFY(turn > 0);
        QVERIFY(db.recordQuiet(turn, "close"));
        const auto claim = db.claimPrivateReflection(12, 60);
        QCOMPARE(claim["id"].toInteger(), turn);
        QVERIFY(db.finishPrivateReflection(turn, claim["version"].toInt(), QStringLiteral("consider"),
                                           QStringLiteral("担心主人休息不够"),
                                           contactNow(QStringLiteral("想问问"), QStringLiteral("早点睡")),
                                           {}, {}, QString{}, QJsonObject{}));
        RawDb raw(path, "reflection-basis-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toInt(), 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("consider"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT note FROM private_reflection WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("担心主人休息不够"));
    }

    // 每日"现在联系"上限：达到上限后不再领取，0 表示不限制。
    void contactDailyLimitBlocks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-daily-limit.db");
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact(1));
        MemoryManager db(path);
        // 三条联系意图共用同一条合法依据：关注之后主人的真实发言。
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("后来又聊了几句"));
        QVERIFY(evidence > 0);
        const auto seed = [&](const QString &event, const QString &input, const QString &note,
                              const QString &message) {
            return seedOpenConcern(db, concernTurnMeta(event), input, note, QJsonObject{},
                                   contactNow(QStringLiteral("又有新消息"), message,
                                              ownerEvidenceBasis(evidence)));
        };

        const qint64 first = seed(QStringLiteral("contact-limit-a"), QStringLiteral("第一件事"),
                                  QStringLiteral("第一件事怎么样了"), QStringLiteral("在忙吗"));
        QVERIFY(first > 0);
        QCOMPARE(db.claimReflectionContact(first, 1, Channel::kPet, Channel::kDefaultSession),
                 QStringLiteral("在忙吗"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("contact-limit-clock")));
        const qint64 second = seed(QStringLiteral("contact-limit-b"), QStringLiteral("第二件事"),
                                   QStringLiteral("第二件事怎么样了"), QStringLiteral("吃了吗"));
        QVERIFY(second > 0);
        // 当天已有一条未 withheld 的 contact 流水，limit=1 时第二条被挡住。
        QVERIFY(db.claimReflectionContact(second, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());

        // 0 表示不限制：放宽后同一条可领取；再新增一条也能领取。
        QVERIFY(identity.enableContact(0));
        QCOMPARE(db.claimReflectionContact(second, 1, Channel::kPet, Channel::kDefaultSession),
                 QStringLiteral("吃了吗"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("contact-limit-clock2")));
        const qint64 third = seed(QStringLiteral("contact-limit-c"), QStringLiteral("第三件事"),
                                  QStringLiteral("第三件事怎么样了"), QStringLiteral("睡了吗"));
        QVERIFY(third > 0);
        QVERIFY(!db.claimReflectionContact(third, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
    }

    // v28：时间委托不再授权 contact(now)。提醒链独占执行，回顾不能代表它再发第二次。
    void explicitTimeDelegationRejectedAndReminderRemains()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-basis-delegation.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("delegation-a")),
            QStringLiteral("明天早上想早起"), QStringLiteral("到点提醒主人起床"),
            QJsonObject{{"topic", QStringLiteral("起床")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const qint64 reminder = insertPendingReminder(path, QStringLiteral("delegation-reminder"));
        QVERIFY(reminder > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("delegation"));
        QCOMPARE(claim["id"].toInteger(), turn);
        // 提醒仍作为"知道有这个提醒"的信息给出，但明确标注不能当 now 依据。
        const auto reminders = claim["basis"].toObject()["reminders"].toArray();
        QVERIFY(!reminders.isEmpty());
        QCOMPARE(reminders.first().toObject()["usable_for_now"].toBool(), false);
        QVERIFY(!reminders.first().toObject()["note"].toString().isEmpty());
        const auto contact = contactNow(QStringLiteral("提醒快到了"), QStringLiteral("起来了吗"),
                                        pendingReminderBasis(reminder));
        QCOMPARE(db.validateContactBasis(Channel::kPet, Channel::kDefaultSession, turn, contact),
                 QStringLiteral("time_delegation_owned_by_reminder"));
        // 提交仍成功（关注按 keep 放回），但不产生可投递的联系行。
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contact}}));
        RawDb raw(path, "delegation-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toInt(), 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                           "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                 QStringLiteral("contact_rejected"));
        // 提醒仍是 pending：只由提醒链在到期时发送，一次委托不会变成两次外部动作。
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM reminder WHERE id=%1").arg(reminder)).toString(),
                 QStringLiteral("pending"));
        QVERIFY(!db.hasProposedContact(turn, claim["version"].toInt()));
    }

    // v28：领取投递前必须用持久化依据重验；提出后失效的依据不能再被领走（TOCTOU）。
    void contactBasisRevalidatedOnClaim()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("contact-basis-revalidate.db");
        MemoryManager db(path);
        const qlonglong activityId = seedCompletedActivity(
            db, QStringLiteral("试试写诗"), R"({"personal_interest":"想写雨声"})",
            QStringLiteral("屋檐轻响，雨在窗上停了一停。"));
        QVERIFY(activityId > 0);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("revalidate-a")),
            QStringLiteral("最近想写点东西"), QStringLiteral("写出来了吗"),
            QJsonObject{{"next_check_after_minutes", 60}},
            contactNow(QStringLiteral("写好了"), QStringLiteral("写了一段"),
                       activityResultBasis(activityId)));
        QVERIFY(turn > 0);
        // 提出后活动结果已经作为分享送达：领取时重验失败，落 withheld 且不发送。
        const auto shareRef = QStringLiteral("thought:%1:%2").arg(activityId).arg(7);
        QVERIFY(db.recordDelivery(QStringLiteral("share"), shareRef, Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("屋檐轻响，雨在窗上停了一停。")));
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("share"), shareRef,
                                         QStringLiteral("protocol_accepted")));
        QVERIFY(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        RawDb raw(path, "revalidate-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("withheld"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("basis_expired"));
        QVERIFY(db.retryableReflectionContacts(4).isEmpty());
    }

    // v28：v27 之前落库的 now contact 没有 basis，在新授权模型下不可领取（兜底 old-row 绕过）。
    void legacyContactWithoutBasisNotClaimable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("contact-legacy-no-basis.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("legacy-no-basis-a")),
            QStringLiteral("旧版本留下的关注"), QStringLiteral("旧关注"),
            QJsonObject{{"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        // 直接插入一条 v27 风格的 now contact：没有 basis_type/basis_refs。
        {
            RawDb raw(path, "legacy-no-basis-insert");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome,channel,session,"
                "basis_type,basis_refs) VALUES(%1,1,'now','旧理由','旧消息','proposed','pet','default',NULL,NULL)")
                .arg(turn)));
        }
        QVERIFY(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        RawDb raw(path, "legacy-no-basis-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("withheld"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("basis_expired"));
        QVERIFY(db.retryableReflectionContacts(4).isEmpty());
    }

    // v28：share 回执 unknown 表示协议层可能已送达，按已分享处理，不能再当"新结果"联系。
    void shareUnknownCountsAsPossiblyShared()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-share-unknown.db");
        MemoryManager db(path);
        const qlonglong activityId = seedCompletedActivity(
            db, QStringLiteral("试试写诗"), R"({"personal_interest":"想写雨声"})",
            QStringLiteral("屋檐轻响，雨在窗上停了一停。"));
        QVERIFY(activityId > 0);
        const auto shareRef = QStringLiteral("thought:%1:%2").arg(activityId).arg(3);
        QVERIFY(db.recordDelivery(QStringLiteral("share"), shareRef, Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("屋檐轻响，雨在窗上停了一停。")));
        QVERIFY(db.recordDeliveryOutcome(QStringLiteral("share"), shareRef,
                                         QStringLiteral("unknown")));
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("share-unknown-a")),
            QStringLiteral("最近想写点东西"), QStringLiteral("写出来了吗"),
            QJsonObject{{"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("share-unknown"));
        QCOMPARE(claim["id"].toInteger(), turn);
        const auto contact = contactNow(QStringLiteral("写好了"), QStringLiteral("写了一段"),
                                        activityResultBasis(activityId));
        QCOMPARE(db.validateContactBasis(Channel::kPet, Channel::kDefaultSession, turn, contact),
                 QStringLiteral("artifact_already_shared"));
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contact}}));
        RawDb raw(path, "share-unknown-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toInt(), 0);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                           "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                 QStringLiteral("contact_rejected"));
    }

    // v28：只有真正落库为 proposed 的 now 联系才算"提出了联系"（观察层据此 emit）。
    void contactRejectedDoesNotEmitProposed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("contact-has-proposed.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("has-proposed-a")),
            QStringLiteral("明天想做荷包蛋"), QStringLiteral("荷包蛋好不好吃"),
            QJsonObject{{"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("has-proposed"));
        const int version = claim["version"].toInt();
        QVERIFY(!db.hasProposedContact(turn, version));
        // 依据被拒：finishConcernReview 返回 true，但没有 proposed 行。
        const qint64 reminder = insertPendingReminder(path, QStringLiteral("has-proposed-reminder"));
        QVERIFY(reminder > 0);
        QVERIFY(db.finishConcernReview(turn, version,
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(
                                                        QStringLiteral("提醒快到了"),
                                                        QStringLiteral("在忙吗"),
                                                        pendingReminderBasis(reminder))}}));
        QVERIFY(!db.hasProposedContact(turn, version));
        // 合法依据提交之后才真的有了 proposed 联系。
        const auto claim2 = reclaimDueConcern(db, path, turn, QStringLiteral("has-proposed-2"));
        QCOMPARE(claim2["id"].toInteger(), turn);
        const int version2 = claim2["version"].toInt();
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("后来主人又说话了"));
        QVERIFY(evidence > 0);
        QVERIFY(db.finishConcernReview(turn, version2,
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(
                                                        QStringLiteral("主人后来提过"),
                                                        QStringLiteral("在忙吗"),
                                                        ownerEvidenceBasis(evidence))}}));
        QVERIFY(db.hasProposedContact(turn, version2));
        // 投递层领取后进入 dispatching，不再是"待投递的 proposed"。
        QVERIFY(!db.claimReflectionContact(turn, version2, Channel::kPet,
                                           Channel::kDefaultSession).isEmpty());
        QVERIFY(!db.hasProposedContact(turn, version2));
    }

    // v28：主人私聊的收口跨渠道生效；pet 里说过晚安能挡住 QQ 私聊的主动触达。
    void crossChannelClosureBlocksContact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-cross-closure.db");
        MemoryManager db(path);
        const QString qqSession = QStringLiteral("2014366825");
        RequestMeta qqMeta = concernTurnMeta(QStringLiteral("cross-closure-a"));
        qqMeta.channel = Channel::kQQPrivate;
        qqMeta.sessionId = qqSession;
        qqMeta.actorId = qqSession;
        qqMeta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-600);
        const qint64 turn = seedOpenConcern(
            db, qqMeta, QStringLiteral("有点担心脚伤"), QStringLiteral("脚伤好没好"),
            QJsonObject{{"topic", QStringLiteral("脚伤")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        // 关注来源之后、收口之前的一条 QQ 发言。
        const qlonglong beforeClose = insertOwnerEvidenceTurn(
            db, QStringLiteral("今天在家休息"), -300, Channel::kQQPrivate, qqSession);
        QVERIFY(beforeClose > 0);
        // pet 里收口（晚安）：同一 subject 的 pet 收口也应挡住 QQ 私聊的主动触达。
        RequestMeta petCloseMeta = concernTurnMeta(QStringLiteral("cross-closure-pet"));
        petCloseMeta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-10);
        const qlonglong petClose = db.recordInput(QStringLiteral("晚安啦"), petCloseMeta, true);
        QVERIFY(petClose > 0);
        QVERIFY(db.recordQuiet(petClose, "close"));
        QCOMPARE(db.recentConversationClosure(Channel::kQQPrivate, qqSession,
                                              QStringLiteral("user"))["action"].toString(),
                 QStringLiteral("close"));
        // 收口之前的 QQ 发言不能再当"收口之后的新证据"。
        const auto claimA = reclaimDueConcern(db, path, turn, QStringLiteral("cross-closure-claim-a"));
        QCOMPARE(claimA["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimA["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(
                                                        QStringLiteral("该问问了"),
                                                        QStringLiteral("脚好点了吗"),
                                                        ownerEvidenceBasis(beforeClose))}}));
        {
            RawDb raw(path, "cross-closure-a-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                               "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                     QStringLiteral("contact_rejected"));
        }
        // 收口之后 QQ 里又有新发言：这才是合法的新证据。
        const qlonglong afterClose = insertOwnerEvidenceTurn(
            db, QStringLiteral("脚好多了"), 5, Channel::kQQPrivate, qqSession);
        QVERIFY(afterClose > 0);
        const auto claimB = reclaimDueConcern(db, path, turn, QStringLiteral("cross-closure-claim-b"));
        QCOMPARE(claimB["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claimB["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contactNow(
                                                        QStringLiteral("主人说好多了"),
                                                        QStringLiteral("那就好"),
                                                        ownerEvidenceBasis(afterClose))}}));
        RawDb raw(path, "cross-closure-b-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("now"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("new_owner_evidence"));
    }

    // v28：dispatching 崩溃遗留按 at-most-once 落 unknown，不重发。
    void staleDispatchingBecomesUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("contact-stale-dispatch.db");
        MemoryManager db(path);
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("后来又聊了几句"));
        QVERIFY(evidence > 0);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("stale-dispatch-a")),
            QStringLiteral("明天想做荷包蛋"), QStringLiteral("荷包蛋好不好吃"),
            QJsonObject{}, contactNow(QStringLiteral("又有新消息"), QStringLiteral("在忙吗"),
                                      ownerEvidenceBasis(evidence)));
        QVERIFY(turn > 0);
        QCOMPARE(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession),
                 QStringLiteral("在忙吗"));
        QCOMPARE(db.recoverStaleContacts(600), 0); // 刚领取，不算过期遗留
        {
            RawDb raw(path, "stale-dispatch-mark");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE reflection_contact SET last_attempt_at=unixepoch()-7200 "
                                            "WHERE turn_id=%1").arg(turn)));
        }
        QCOMPARE(db.recoverStaleContacts(600), 1);
        QCOMPARE(db.recoverStaleContacts(600), 0); // 幂等：终态不再重复恢复
        RawDb raw(path, "stale-dispatch-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("unknown"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("dispatch_abandoned"));
        // unknown 可能已送达：不再领取、也不再进入补发清单。
        QVERIFY(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        QVERIFY(db.retryableReflectionContacts(4).isEmpty());
    }

    // v28 基线/已知边界：依据只核验时序与来源，不判断语义相关性。
    // "脚伤"的关注在主人真实说了"买了苹果"之后仍可通过 new_owner_evidence——
    // 这是当前已知语义边界，模拟层需要服务端做相关性判断；这里不引入关键词规则。
    void semanticallyUnrelatedBasisStillPasses()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-semantic-unrelated.db");
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("semantic-a")),
            QStringLiteral("脚扭伤了，有点担心"), QStringLiteral("脚伤好没好"),
            QJsonObject{{"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        const auto claim = reclaimDueConcern(db, path, turn, QStringLiteral("semantic"));
        QCOMPARE(claim["id"].toInteger(), turn);
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("今天买了苹果"));
        QVERIFY(evidence > 0);
        const auto contact = contactNow(QStringLiteral("主人后来提到别的事"),
                                        QStringLiteral("在忙吗"),
                                        ownerEvidenceBasis(evidence));
        QCOMPARE(db.validateContactBasis(Channel::kPet, Channel::kDefaultSession, turn, contact),
                 QString());
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "contact"},
                                                   {"contact", contact}}));
        RawDb raw(path, "semantic-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("now"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                .arg(turn)).toString(), QStringLiteral("new_owner_evidence"));
    }

    // v28 迁移：v27 库里旧的 now contact（无 basis）升级时统一置 withheld 并留审计。
    void v28MigrationWithholdsLegacyNowContact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("contact-v27-legacy.db");
        {
            MemoryManager db(path);
        }
        {
            RawDb raw(path, "v27-legacy-insert");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome) "
                "VALUES(99001,1,'now','旧约定','旧消息','proposed')")));
            QVERIFY(raw.exec(QStringLiteral("PRAGMA user_version=27")));
        }
        MemoryManager reopened(path);
        RawDb raw(path, "v27-legacy-verify");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 28);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=99001"))
                     .toString(), QStringLiteral("withheld"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM reflection_contact WHERE turn_id=99001"))
                     .toString(), QStringLiteral("missing_basis_at_v28"));
    }

    // 关注默认间隔取 proactive.concern_default_check_minutes（默认 720 分钟）。
    void concernDefaultUsesConfig()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        // 显式写入不含新键的 proactive 段，确保读到默认值而不是其它用例留下的配置。
        QVERIFY(identity.enableContact());
        QCOMPARE(ConfigManager::instance().getProactiveConfig().concernDefaultCheckMinutes, 720);

        const auto nextCheckAfterConsider = [](MemoryManager &db, const QString &path,
                                               const RequestMeta &meta, const QString &input,
                                               const QString &note, const QString &tag) -> qint64 {
            const qint64 turn = db.recordInput(input, meta, true);
            if (turn <= 0 || !db.recordQuiet(turn, "close")) return -1;
            const auto claim = db.claimPrivateReflection(12, 60);
            if (claim["id"].toInteger() != turn) return -1;
            if (!db.finishPrivateReflection(turn, 1, "consider", note)) return -1;
            RawDb raw(path, tag);
            if (!raw.open()) return -1;
            return raw.scalar(QStringLiteral("SELECT next_check_at FROM private_reflection "
                                             "WHERE turn_id=%1").arg(turn)).toLongLong();
        };

        const QString path = dir.filePath("concern-default.db");
        qint64 nextCheck = -1;
        {
            MemoryManager db(path);
            const qint64 before = QDateTime::currentSecsSinceEpoch();
            nextCheck = nextCheckAfterConsider(
                db, path, concernTurnMeta(QStringLiteral("concern-default")),
                QStringLiteral("周末可能下雨"), QStringLiteral("周末天气怎么样"),
                QStringLiteral("concern-default"));
            QVERIFY2(qAbs(nextCheck - (before + 720 * 60)) <= 120,
                     qPrintable(QString::number(nextCheck)));
        }

        // 换一个非默认值：说明间隔确实来自配置，而不是硬编码的 720。
        QVERIFY(identity.writeIdentity(R"({"proactive":{"concern_default_check_minutes":60}})"));
        QCOMPARE(ConfigManager::instance().getProactiveConfig().concernDefaultCheckMinutes, 60);
        const QString customPath = dir.filePath("concern-default-custom.db");
        qint64 customNext = -1;
        {
            MemoryManager custom(customPath);
            const qint64 customBefore = QDateTime::currentSecsSinceEpoch();
            customNext = nextCheckAfterConsider(
                custom, customPath, concernTurnMeta(QStringLiteral("concern-default-custom")),
                QStringLiteral("明天可能降温"), QStringLiteral("降温了没有"),
                QStringLiteral("concern-default-custom"));
            QVERIFY2(qAbs(customNext - (customBefore + 60 * 60)) <= 120,
                     qPrintable(QString::number(customNext)));
        }
    }

    // 保留期：来源 reported 超过 concern_retention_days 的关注不再按时间到期领取。
    void dueConcernRespectsRetention()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-retention.db");
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(R"({"proactive":{"concern_retention_days":14}})"));
        MemoryManager db(path);
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("concern-retention-a")),
            QStringLiteral("明天想去看展览"), QStringLiteral("展览怎么样"),
            QJsonObject{{"topic", QStringLiteral("展览")},
                        {"watch", QStringLiteral("有没有去")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        {
            RawDb raw(path, "concern-retention-setup");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                            "WHERE turn_id=%1").arg(turn)));
            // 关注来源已过去 30 天，超过保留期 14 天。
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE cognitive_source SET reported=datetime('now','-30 day')")));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-retention-clock")));
        QVERIFY(db.claimDueConcern(12, 60).isEmpty());

        // 保留期只拦"按时间到期"；来源恢复为近期后同一条关注仍可领取。
        {
            RawDb raw(path, "concern-retention-restore");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE cognitive_source SET reported=datetime('now')")));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("concern-retention-clock2")));
        const auto claimed = db.claimDueConcern(12, 60);
        QCOMPARE(claimed["id"].toInteger(), turn);
        QCOMPARE(claimed["kind"].toString(), QStringLiteral("concern"));
    }

    // 启动恢复：租约过期仍 pending 的回顾要放回可重领状态，不能一直卡住。
    void staleReflectionsRecovered()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("stale-reflections.db");
        MemoryManager db(path);
        const qint64 sourceTurn = db.recordInput(QStringLiteral("一条还没回顾完的发言"),
                                                 concernTurnMeta(QStringLiteral("stale-source")), true);
        QVERIFY(sourceTurn > 0);
        QVERIFY(db.recordQuiet(sourceTurn, "close"));
        const auto sourceClaim = db.claimPrivateReflection(12, 60);
        QCOMPARE(sourceClaim["id"].toInteger(), sourceTurn);
        {
            RawDb raw(path, "stale-source-lease");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET eligible_at=unixepoch()-7200 "
                                            "WHERE turn_id=%1").arg(sourceTurn)));
        }
        QCOMPARE(db.recoverStaleReflections(1800), 1);
        {
            RawDb raw(path, "stale-source-state");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(sourceTurn)).toString(), QStringLiteral("deferred"));
        }

        // 到期关注的领取中断应回到 consider（可再次按到期领取），而不是 deferred。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("stale-due-clock")));
        const qint64 dueTurn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("stale-due")),
            QStringLiteral("今天要不要吃早饭"), QStringLiteral("想知道他吃没吃"),
            QJsonObject{{"topic", QStringLiteral("早饭")},
                        {"watch", QStringLiteral("他吃没吃")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(dueTurn > 0);
        {
            RawDb raw(path, "stale-due-mark");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                            "WHERE turn_id=%1").arg(dueTurn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("stale-due-clock2")));
        const auto dueClaim = db.claimDueConcern(12, 60);
        QCOMPARE(dueClaim["id"].toInteger(), dueTurn);
        {
            RawDb raw(path, "stale-due-lease");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET eligible_at=unixepoch()-7200 "
                                            "WHERE turn_id=%1").arg(dueTurn)));
        }
        QCOMPARE(db.recoverStaleReflections(1800), 1);
        {
            RawDb raw(path, "stale-due-state");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(dueTurn)).toString(), QStringLiteral("consider"));
            QVERIFY(raw.scalar(QStringLiteral("SELECT claimed_by FROM private_reflection WHERE turn_id=%1")
                                   .arg(dueTurn)).isNull());
        }
    }

    // 到期回顾的 revise 会原地覆盖旧想法，必须留下可追溯的前后快照。
    void dueConcernReviewWritesRevision()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("concern-revision.db");
        MemoryManager db(path);
        const QString beforeNote = QStringLiteral("原来只是好奇他吃了什么");
        const qint64 turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("revision-a")),
            QStringLiteral("他说晚饭随便吃点"), beforeNote,
            QJsonObject{{"topic", QStringLiteral("晚饭")},
                        {"watch", QStringLiteral("他最后吃了什么")},
                        {"next_check_after_minutes", 60}});
        QVERIFY(turn > 0);
        {
            RawDb raw(path, "revision-mark");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                            "WHERE turn_id=%1").arg(turn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("revision-clock")));
        const auto claim = db.claimDueConcern(12, 60);
        QCOMPARE(claim["id"].toInteger(), turn);
        QVERIFY(db.finishConcernReview(turn, claim["version"].toInt(),
                                       QJsonObject{{"concern_action", "revise"},
                                                   {"note", "现在更在意他有没有好好吃饭"},
                                                   {"forget", QJsonObject{{"next_check_after_minutes", 120}}}}));
        RawDb raw(path, "revision-raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM concern_revision WHERE turn=%1")
                                .arg(turn)).toInt(), 1);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1")
                                .arg(turn)).toString(), QStringLiteral("revise"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT note_before FROM concern_revision WHERE turn=%1")
                                .arg(turn)).toString(), beforeNote);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT note_after FROM concern_revision WHERE turn=%1")
                                .arg(turn)).toString(), QStringLiteral("现在更在意他有没有好好吃饭"));
        QVERIFY(raw.scalar(QStringLiteral("SELECT next_check_at FROM concern_revision WHERE turn=%1")
                               .arg(turn)).toLongLong() > 0);
    }

    // B1：注意力事件状态机（v25）。同 (kind,ref) 幂等；claim 按 priority,id；终态不可回退。
    void attentionEventLifecycle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("attention-events.db");
        {
            MemoryManager db(path);
            RawDb schema(path, "attention-schema");
            QVERIFY(schema.open());
            QCOMPARE(schema.scalar("PRAGMA user_version").toInt(), 28);
            const QStringList eventColumns = schema.columns("attention_event");
            for (const auto &column : {QStringLiteral("kind"), QStringLiteral("ref"),
                                       QStringLiteral("payload"), QStringLiteral("state"),
                                       QStringLiteral("priority"), QStringLiteral("reason"),
                                       QStringLiteral("created_at"), QStringLiteral("claimed_at"),
                                       QStringLiteral("done_at")})
                QVERIFY2(eventColumns.contains(column), qPrintable(column));
            const QStringList stateColumns = schema.columns("attention_state");
            QVERIFY(stateColumns.contains("key"));
            QVERIFY(stateColumns.contains("value"));
            QVERIFY(stateColumns.contains("updated_at"));
            QCOMPARE(schema.scalar("SELECT count(*) FROM sqlite_master WHERE type='index' "
                                   "AND name='attention_event_pending'").toInt(), 1);

            const auto id = db.enqueueAttentionEvent(
                QStringLiteral("message"), QStringLiteral("evt-1"),
                QJsonObject{{QStringLiteral("channel"), QStringLiteral("pet")}},
                QStringLiteral("arrived"), 50);
            QVERIFY(id > 0);
            // 同 (kind,ref) 第二次入队返回同一行 id，不产生新行。
            QCOMPARE(db.enqueueAttentionEvent(QStringLiteral("message"), QStringLiteral("evt-1")), id);
            const auto initial = db.attentionBacklog();
            QCOMPARE(initial["pending"].toObject()["message"].toInt(), 1);
            QCOMPARE(initial["by_state"].toObject()["pending"].toInt(), 1);
            QVERIFY(initial["oldest_age_seconds"].toInteger() >= 0);

            const auto claimed = db.claimAttentionEvent();
            QCOMPARE(claimed["id"].toInteger(), id);
            QCOMPARE(claimed["kind"].toString(), QStringLiteral("message"));
            QCOMPARE(claimed["ref"].toString(), QStringLiteral("evt-1"));
            QCOMPARE(claimed["payload"].toObject()["channel"].toString(), QStringLiteral("pet"));
            QCOMPARE(claimed["priority"].toInt(), 50);
            QVERIFY(db.claimAttentionEvent().isEmpty()); // 已无 pending
            QCOMPARE(db.attentionBacklog()["by_state"].toObject()["claimed"].toInt(), 1);

            QVERIFY(db.completeAttentionEvent(id, QStringLiteral("done"), QStringLiteral("ok")));
            QVERIFY(!db.completeAttentionEvent(id, QStringLiteral("done"))); // 终态不可再次变更
            const auto finished = db.attentionBacklog();
            QVERIFY(finished["pending"].toObject().isEmpty());
            QCOMPARE(finished["by_state"].toObject()["done"].toInt(), 1);
            QCOMPARE(finished["oldest_age_seconds"].toInteger(), qint64(0));

            // priority 越小越先领取：recovery(10) 先于 message(50)。
            const auto low = db.enqueueAttentionEvent(QStringLiteral("message"), QStringLiteral("late"), {}, {}, 50);
            const auto high = db.enqueueAttentionEvent(QStringLiteral("recovery"), QStringLiteral("turn:1"), {}, {}, 10);
            QVERIFY(low > 0 && high > 0);
            QCOMPARE(db.claimAttentionEvent()["id"].toInteger(), high);
            QCOMPARE(db.claimAttentionEvent()["id"].toInteger(), low);
        }
        // 重开连接：事件与状态都在库里。
        MemoryManager reopened(path);
        const auto persisted = reopened.attentionBacklog();
        QCOMPARE(persisted["by_state"].toObject()["done"].toInt(), 1);
        QCOMPARE(persisted["by_state"].toObject()["claimed"].toInt(), 2);
    }

    // B1：崩溃恢复。received 轮次超时后幂等入队 recovery（最高优先级），可被中断并落终态。
    void attentionRecoveryEvents()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("attention-recovery.db");
        MemoryManager db(path);
        RequestMeta meta;
        meta.eventId = meta.runId = QStringLiteral("interrupted-receipt");
        const qint64 turn = db.recordInput(QStringLiteral("这一轮没有收到回复"), meta, true);
        QVERIFY(turn > 0);
        {
            RawDb clock(path, "recovery-clock");
            QVERIFY(clock.open());
            // recordInput 的 timestamp 是本地时间；挪到 2 分钟前模拟进程中断。
            QVERIFY(clock.exec(QStringLiteral(
                "UPDATE chat_history SET timestamp=datetime('now','localtime','-120 seconds') WHERE id=%1").arg(turn)));
        }
        QCOMPARE(db.enqueueRecoveryEvents(60), 1);
        QCOMPARE(db.enqueueRecoveryEvents(60), 0); // 幂等：同轮次不再重复入队
        const auto event = db.claimAttentionEvent();
        QCOMPARE(event["kind"].toString(), QStringLiteral("recovery"));
        QCOMPARE(event["payload"].toObject()["turn"].toInteger(), turn);
        QCOMPARE(event["payload"].toObject()["channel"].toString(), meta.channel);
        QCOMPARE(event["payload"].toObject()["session"].toString(), meta.sessionId);
        QCOMPARE(event["priority"].toInt(), 10);
        QVERIFY(db.interruptTurn(turn, QStringLiteral("restart_recovery")));
        QVERIFY(db.completeAttentionEvent(event["id"].toInteger(), QStringLiteral("done"),
                                          QStringLiteral("recovered")));
        QCOMPARE(db.attentionBacklog()["by_state"].toObject()["done"].toInt(), 1);
        // 已恢复的轮次不再被扫描到（turn_state 已不是 received）。
        QCOMPARE(db.enqueueRecoveryEvents(60), 0);
    }

    // B1：当前活动/切换原因键值存档跨重启保持。
    void attentionStatePersists()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("attention-state.db");
        {
            MemoryManager db(path);
            db.setAttentionState(QStringLiteral("current_activity"), QStringLiteral("thought/492"));
            db.setAttentionState(QStringLiteral("last_switch_reason"), QStringLiteral("incoming_message"));
            QCOMPARE(db.attentionState(QStringLiteral("current_activity")), QStringLiteral("thought/492"));
        }
        MemoryManager reopened(path);
        QCOMPARE(reopened.attentionState(QStringLiteral("current_activity")), QStringLiteral("thought/492"));
        QCOMPARE(reopened.attentionState(QStringLiteral("last_switch_reason")), QStringLiteral("incoming_message"));
        QVERIFY(reopened.attentionState(QStringLiteral("missing")).isEmpty());
        // 同一 key 只保留最新值（切换离开活动时清空）。
        reopened.setAttentionState(QStringLiteral("current_activity"), QString());
        QVERIFY(reopened.attentionState(QStringLiteral("current_activity")).isEmpty());
        reopened.setAttentionState(QStringLiteral("current_activity"), QStringLiteral("thought/500"));
        QCOMPARE(reopened.attentionState(QStringLiteral("current_activity")), QStringLiteral("thought/500"));
    }

    // B2：活动档案补全：来源轮次/目标类型/时间戳/产物引用；无产物不可分享。
    void activityStoresSourceAndArtifact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("activity-source.db");
        MemoryManager db(path);
        QVERIFY(db.saveQATurn(QStringLiteral("想写点雨声"), R"({"personal_interest":"写雨声"})", {}));
        const auto thought = db.nextThought(true);
        const qlonglong id = thought["id"].toInteger();
        const int version = thought["version"].toInt();
        QVERIFY(id > 0);

        RawDb raw(path, "activity-source");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.source_turn') "
                                           "FROM chat_history WHERE id=%1").arg(id)).toLongLong(), id);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.goal_kind') "
                                           "FROM chat_history WHERE id=%1").arg(id)).toString(),
                 QStringLiteral("interest"));
        QVERIFY(!raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.created_at') "
                                           "FROM chat_history WHERE id=%1").arg(id)).toString().isEmpty());
        QVERIFY(raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.artifact_ref') "
                                          "FROM chat_history WHERE id=%1").arg(id)).toString().isEmpty());
        // 没有实际产物（只写着"在思考"）时不可分享。
        QVERIFY(!db.canShareThought(id, version));

        const QJsonObject draft{{"status","completed"},{"reflection","写好了两句"},
                                {"draft","屋檐轻响，雨在窗上停了一停。"}};
        QVERIFY(db.finishThoughtReview(id, version, draft));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.artifact_ref') "
                                           "FROM chat_history WHERE id=%1").arg(id)).toString(),
                 QStringLiteral("thought/%1").arg(id));
        QVERIFY(!raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.updated_at') "
                                           "FROM chat_history WHERE id=%1").arg(id)).toString().isEmpty());
        // 回顾不能改掉创建时的来源。
        QCOMPARE(raw.scalar(QStringLiteral("SELECT json_extract(thought_result,'$.source_turn') "
                                           "FROM chat_history WHERE id=%1").arg(id)).toLongLong(), id);
        QVERIFY(db.canShareThought(id, version + 1));
    }

    // B2：来源轮次删除后活动随之消失，迟到回调/分享都不能复活它。
    void deletedActivitySourceCannotShare()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("activity-deleted.db"));
        QVERIFY(db.saveQATurn(QStringLiteral("想写点雨声"), R"({"personal_interest":"写雨声"})", {}));
        const auto thought = db.nextThought(true);
        const qlonglong id = thought["id"].toInteger();
        const int version = thought["version"].toInt();
        QVERIFY(id > 0);
        QVERIFY(db.finishThoughtReview(id, version,
            QJsonObject{{"status","active"},{"reflection","继续写"},{"draft","第一句"}}));
        QVERIFY(db.canShareThought(id, version + 1));
        QVERIFY(db.deleteTurnByID(id));
        QVERIFY(db.nextThought(true).isEmpty());
        QVERIFY(db.nextThought(false).isEmpty());
        QVERIFY(!db.canShareThought(id, version + 1));
        // 迟到回调同样被行存在性/版本校验拒绝。
        QVERIFY(!db.finishThoughtReview(id, version + 1,
            QJsonObject{{"status","active"},{"reflection","迟到"}}));
    }
};

QTEST_GUILESS_MAIN(TestMemoryManager)
    void dashScopeCredentialsLoadAndSaveFromSetting()
    {
        auto &config = ConfigManager::instance();
        const QString path = QDir(config.getConfigDirPath()).filePath("setting.json");
        QFile original(path);
        const bool existed = original.exists();
        QByteArray previous;
        if (existed) {
            QVERIFY(original.open(QIODevice::ReadOnly));
            previous = original.readAll();
            original.close();
        }

        QJsonObject api;
        api.insert("deepseek_api_key", QStringLiteral("sk-deepseek-test"));
        api.insert("dashscope_api_key", QStringLiteral("sk-dashscope-test"));
        api.insert("dashscope_base_url",
                   QStringLiteral("https://ws-example.cn-beijing.maas.aliyuncs.com"));
        QJsonObject retrieval;
        retrieval.insert("embedding_enabled", true);
        retrieval.insert("embedding_provider", QStringLiteral("dashscope"));
        retrieval.insert("embedding_model", QStringLiteral("text-embedding-v4"));
        retrieval.insert("embedding_dimension", 1024);
        retrieval.insert("rerank_enabled", false);
        QJsonObject memory;
        memory.insert("retrieval", retrieval);
        QJsonObject root;
        root.insert("api", api);
        root.insert("memory", memory);

        QFile written(path);
        QVERIFY(written.open(QIODevice::WriteOnly | QIODevice::Truncate));
        written.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        written.close();

        QVERIFY(config.loadSetting());
        QCOMPARE(config.getDashScopeApiKey(), QStringLiteral("sk-dashscope-test"));
        QCOMPARE(config.getDashScopeBaseUrl(),
                 QStringLiteral("https://ws-example.cn-beijing.maas.aliyuncs.com"));
        const auto loaded = config.getMemoryPolicyConfig().retrieval;
        QVERIFY(!loaded.rerankEnabled);
        QCOMPARE(loaded.embeddingEndpoint,
                 QStringLiteral("https://ws-example.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/embeddings"));
        QCOMPARE(loaded.rerankEndpoint,
                 QStringLiteral("https://ws-example.cn-beijing.maas.aliyuncs.com/api/v1/services/rerank/text-rerank/text-rerank"));

        QVERIFY(config.saveSetting());
        QFile savedFile(path);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(savedFile.readAll()).object();
        savedFile.close();
        QCOMPARE(saved.value("api").toObject().value("dashscope_api_key").toString(),
                 QStringLiteral("sk-dashscope-test"));
        QVERIFY(saved.value("memory").toObject().value("retrieval").isObject());

        if (existed) {
            QFile restore(path);
            QVERIFY(restore.open(QIODevice::WriteOnly | QIODevice::Truncate));
            restore.write(previous);
            restore.close();
        } else {
            QFile::remove(path);
        }
        config.loadSetting();
    }

#include "tst_memorymanager.moc"
