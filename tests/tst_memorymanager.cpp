#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>

#include "configmanager.h"
#include "memorymanager.h"
#include "memoryevidence.h"
#include "memoryretrieval.h"
#include "memoryembedding.h"
#include "memoryindexworker.h"
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(),15);
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
        const auto evidence = db.personalEvidence("user", {direct.actorId}, {"脚踝"});
        QVERIFY(QJsonDocument(evidence).toJson().contains(legacy.content.toUtf8()));
        QVERIFY(db.personalEvidence(group.subject, {group.subject.mid(3)}, {"脚踝"}).isEmpty());
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
        const auto items = db.personalEvidence("user", {event.senderId},
            {"排练"}, true, Channel::kPet, Channel::kDefaultSession);
        QStringList texts;
        for (const auto &item : items)
            texts.append(item.toObject()["text"].toString());
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
            QVERIFY(db.claimThoughtReview(id,1,900));
            QVERIFY(!db.claimThoughtReview(id,1,900));
            const QJsonObject review{{"status","completed"},{"reflection","写好了两句"},
                                     {"draft","屋檐轻响，雨在窗上停了一停。"}};
            QVERIFY(db.finishThoughtReview(id,0,review));
            QVERIFY(!db.finishThoughtReview(id,0,review));
        }
        MemoryManager db(path);
        QVERIFY(db.nextThought(true).isEmpty());
        QCOMPARE(db.nextThought(false)["last_review"].toObject()["status"].toString(),QString("completed"));
        QVERIFY(db.deleteTurnByID(id));
        QVERIFY(db.nextThought(false).isEmpty());
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

            QVERIFY(db.applyThoughtAction(id, v2, "pause"));
            QVERIFY(db.nextThought(true).isEmpty());
            QVERIFY(!db.canShareThought(id, v2 + 1));
            QVERIFY(!db.finishThoughtReview(id, v2,
                QJsonObject{{"status","active"},{"reflection","迟到的写作"}}));

            QVERIFY(db.applyThoughtAction(id, v2 + 1, "resume"));
            auto resumed = db.nextThought(true);
            QCOMPARE(resumed["id"].toInteger(), id);
            const int v3 = resumed["version"].toInt();

            QVERIFY(db.applyThoughtAction(id, v3, "abandon"));
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

        // 索引非空时，关闭回退也不能偷偷读取 old adapter。
        QVERIFY(db.addLongTermSummary(QStringLiteral("legacy-only"), 1, QStringLiteral("[]")));
        auto legacyRequest = request;
        legacyRequest.query = QStringLiteral("legacy-only");
        legacyRequest.terms = {QStringLiteral("legacy-only")};
        legacyRequest.limit = 1;
        const auto withFallback = defaultRerank.retrieve(legacyRequest);
        QVERIFY(withFallback.evidence.size() <= 1);
        auto noAux = legacyRequest;
        noAux.includeAuxiliarySources = false;
        noAux.sourceKinds = {QStringLiteral("summary")};
        QVERIFY(defaultRerank.retrieve(noAux).evidence.isEmpty());
        auto noFallback = legacyRequest;
        noFallback.allowLegacyFallback = false;
        noFallback.sourceKinds = {QStringLiteral("summary")};
        QVERIFY(defaultRerank.retrieve(noFallback).evidence.isEmpty());

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
        const auto fallbackResult = fallback.retrieve(request);
        QVERIFY(!fallbackResult.evidence.isEmpty());
        QCOMPARE(fallbackResult.status, QStringLiteral("complete"));
        QCOMPARE(fallbackResult.backend, QStringLiteral("legacy"));
        auto noLegacy = request;
        noLegacy.allowLegacyFallback = false;
        QVERIFY(fallback.retrieve(noLegacy).evidence.isEmpty());
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
        MemoryIndexWorker worker(path);
        QSignalSpy finished(&worker, &MemoryIndexWorker::finished);
        worker.run();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toBool(), true);
        QVERIFY(finished.first().at(2).toInt() > 0);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 15);
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
        QCOMPARE(copy.scalar("PRAGMA user_version").toInt(), 15);
        QVERIFY(copy.scalar("SELECT count(*) FROM memory_chunks").toInt() > 0);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 15);

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
            QCOMPARE(rawCols.size(), 19);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 15);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 15);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 15);
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
        QVERIFY(bundle.items.first().sanitizedText.contains(QStringLiteral("[REDACTED]")));
        QVERIFY(!bundle.items.first().sanitizedText.contains(QStringLiteral("770951546")));
        QCOMPARE(mm.countUnprocessedGroupEvents(QStringLiteral("30010")),
                 beforeUnprocessed);
        QCOMPARE(mm.getMemoryCursorEventId(Channel::kQQGroup,
                                           QStringLiteral("30010")),
                 beforeCursor);
        QCOMPARE(mm.getPendingInteractionIds(10), beforePending);
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
