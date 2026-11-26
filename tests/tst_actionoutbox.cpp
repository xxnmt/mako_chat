#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "configmanager.h"
#include "memorymanager.h"
#include "utterance.h"
#include "actionorigin.h"
#include "actioncommitter.h"
#include "actionoutboxdispatcher.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QUuid>

// 独立于 MemoryManager 的原始连接：建测试库、直接验表结构、模拟租约过期/并发写入。
class RawDb
{
public:
    RawDb(const QString &path, const QString &tag)
        : m_conn(QStringLiteral("tst_actionoutbox_raw_") + tag)
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
    // 参数化执行：手工构造"网络渠道待发/已领取"的行时避免 SQL 字符串拼接。
    bool execPrepared(const QString &sql, const QVariantList &values)
    {
        QSqlQuery q(m_db);
        q.prepare(sql);
        for (const auto &value : values) q.addBindValue(value);
        if (!q.exec()) {
            qWarning() << "prepared exec failed:" << sql << q.lastError().text();
            return false;
        }
        return true;
    }
    // 插入一条 pending 提醒；turn 用全局递减负数，避免撞 UNIQUE(turn,due) 与删除触发器。
    bool insertReminder(qlonglong turn, const QString &channel, const QString &session,
                        int dueOffsetSeconds, int expiresOffsetSeconds, const QString &message,
                        qlonglong *id = nullptr)
    {
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO reminder(turn,channel,session,due,expires,message,state) "
                  "VALUES(?,?,?,unixepoch()+?,unixepoch()+?,?,'pending')");
        q.addBindValue(turn);
        q.addBindValue(channel);
        q.addBindValue(session);
        q.addBindValue(dueOffsetSeconds);
        q.addBindValue(expiresOffsetSeconds);
        q.addBindValue(message);
        if (!q.exec()) {
            qWarning() << "insert reminder failed:" << q.lastError().text();
            return false;
        }
        if (id) *id = q.lastInsertId().toLongLong();
        return true;
    }
    void close()
    {
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();
        if (QSqlDatabase::contains(m_conn))
            QSqlDatabase::removeDatabase(m_conn);
    }

private:
    QString m_conn;
    QSqlDatabase m_db;
};

// 主动联系/分享配置在 identity.json 的 proactive 段；测试里临时启用，析构时恢复。
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
    bool enableProactive(int contactDailyLimit = -1)
    {
        QJsonObject target{{"channel", Channel::kPet},
                           {"session_id", Channel::kDefaultSession}};
        QJsonObject proactive{{"enabled", true},
                              {"private_writing", false},
                              {"targets", QJsonArray{target}}};
        if (contactDailyLimit >= 0)
            proactive.insert("contact_daily_limit", contactDailyLimit);
        const QJsonObject root{{"proactive", proactive}};
        return ConfigManager::instance().writeConfigDocument(
            QStringLiteral("identity.json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact), nullptr);
    }
    // R12-2b：可提交的 OpenWake contact 配置。allow_contact 是独立开关（默认 false）；
    // 安静时段缺省 00:00-00:00（不设），可传覆盖值验证 quiet 拦截；目标始终主人桌宠域。
    bool enableOpenWakeContact(bool allowContact, int contactDailyLimit = -1,
                               const QString &quietStart = QStringLiteral("00:00"),
                               const QString &quietEnd = QStringLiteral("00:00"))
    {
        QJsonObject target{{"channel", Channel::kPet},
                           {"session_id", Channel::kDefaultSession}};
        QJsonObject proactive{{"enabled", true},
                              {"private_writing", false},
                              {"cooldown_seconds", 0},
                              {"quiet_hours",
                               QJsonObject{{"start", quietStart}, {"end", quietEnd}}},
                              {"targets", QJsonArray{target}}};
        if (contactDailyLimit >= 0)
            proactive.insert("contact_daily_limit", contactDailyLimit);
        const QJsonObject openWake{{"enabled", true},
                                   {"allow_contact", allowContact},
                                   {"initial_delay_seconds", 60},
                                   {"min_interval_seconds", 300},
                                   {"default_interval_seconds", 300},
                                   {"max_interval_seconds", 86400},
                                   {"max_non_open_jobs_when_due", 0},
                                   {"max_model_steps_per_wake", 3},
                                   {"max_per_day", 12},
                                   {"shared_budget_percent", 30}};
        const QJsonObject root{{"proactive", proactive},
                               {"cognition", QJsonObject{{"open_wake", openWake}}}};
        return ConfigManager::instance().writeConfigDocument(
            QStringLiteral("identity.json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact), nullptr);
    }

private:
    QString m_path;
    QByteArray m_original;
    bool m_existed = false;
};

// ===== R12-2b：verified proposal 提交 / 共享额度 / 回执 / 恢复的测试夹具 =====

// 造一条 open_wake 行（R12-2b 的 contact 来源）。state 用 deferred 以免撞
// open_wake_running_unique（同一 scope 只允许一个 running）；来源核验只读版本/run/scope/epoch。
static qlonglong seedOpenWakeRow(const QString &dbPath, const QString &tag,
                                 const QString &runId, qint64 epoch = 0)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return 0;
    if (!raw.execPrepared(
            QStringLiteral("INSERT INTO open_wake(scope_key,run_id,version,state,trigger_reason,"
                           "created_at,started_at,current_context_epoch) "
                           "VALUES('owner',?,1,'deferred','test',unixepoch(),unixepoch(),?)"),
            {runId, epoch}))
        return 0;
    return raw.scalar(QStringLiteral("SELECT last_insert_rowid()")).toLongLong();
}

// 服务端组装一条 wake contact 提案（claims 为空=纯 self_desire/问候）。
static QJsonObject wakeContactProposal(qlonglong wakeId, int version, const QString &runId,
                                       const QString &message, qint64 epoch = 0)
{
    const ActionOrigin origin = ActionOrigin::forWake(
        wakeId, version, runId, QStringLiteral("owner"), epoch,
        Channel::kPet, Channel::kDefaultSession);
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("contact")},
        {QStringLiteral("origin"), origin.toJson()},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), Channel::kPet},
                     {QStringLiteral("session_id"), Channel::kDefaultSession}}},
        {QStringLiteral("payload"),
         QJsonObject{{QStringLiteral("message"), message},
                     {QStringLiteral("reason"), QStringLiteral("想问候")},
                     {QStringLiteral("motivation"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("self_desire")},
                                  {QStringLiteral("summary"), QStringLiteral("想问候主人")}}},
                     {QStringLiteral("contact_draft"), message}}},
        {QStringLiteral("claims"), QJsonArray{}},
        {QStringLiteral("expires_at"), 0}};
}

// 造一条 completed 且带真实成果 + share_intent 的 internal 活动（artifact_version=2）。
static qlonglong seedInternalCompletedActivity(const QString &dbPath, const QString &tag,
                                               const QString &draft, QString *hash = nullptr)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return 0;
    if (!raw.execPrepared(
            QStringLiteral("INSERT INTO internal_activity(scope_key,origin_kind,origin_ref,"
                           "run_id,subject,channel,session_id,interest,status,version,result_json,"
                           "artifact_ref,review_after,created_at,updated_at) "
                           "VALUES('owner','foreground',NULL,NULL,'user','pet','default',"
                           "'写雨声','completed',2,'{}',NULL,0,unixepoch(),unixepoch())"),
            QVariantList{}))
        return 0;
    const qlonglong id = raw.scalar(QStringLiteral("SELECT last_insert_rowid()")).toLongLong();
    if (id <= 0) return 0;
    const QString ref = QStringLiteral("activity/%1").arg(id);
    const QString computedHash = QString::fromLatin1(
        QCryptographicHash::hash(draft.trimmed().toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16));
    const QJsonObject intent{
        {QStringLiteral("state"), QStringLiteral("saved")},
        {QStringLiteral("artifact_ref"), ref},
        {QStringLiteral("artifact_version"), 2},
        {QStringLiteral("content_hash"), computedHash},
        {QStringLiteral("requested_at"), QDateTime::currentSecsSinceEpoch()},
        {QStringLiteral("version"), 1}};
    const QJsonObject result{
        {QStringLiteral("status"), QStringLiteral("completed")},
        {QStringLiteral("draft"), draft},
        {QStringLiteral("artifact_ref"), ref},
        {QStringLiteral("artifact_version"), 2},
        {QStringLiteral("artifact_hash"), computedHash},
        {QStringLiteral("completed_at"), QDateTime::currentSecsSinceEpoch()},
        {QStringLiteral("reflection"), QStringLiteral("写好了两句")},
        {QStringLiteral("share_intent"), intent}};
    if (!raw.execPrepared(
            QStringLiteral("UPDATE internal_activity SET result_json=?,artifact_ref=? WHERE id=?"),
            {QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)), ref, id}))
        return 0;
    if (hash) *hash = computedHash;
    return id;
}

static QJsonObject internalShareProposal(qlonglong activityId, int artifactVersion,
                                         const QString &draft)
{
    const ActionOrigin origin = ActionOrigin::forActivity(
        activityId, artifactVersion, QStringLiteral("owner"),
        Channel::kPet, Channel::kDefaultSession);
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("activity_share")},
        {QStringLiteral("origin"), origin.toJson()},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), Channel::kPet},
                     {QStringLiteral("session_id"), Channel::kDefaultSession}}},
        {QStringLiteral("payload"),
         QJsonObject{{QStringLiteral("draft"), draft}}},
        {QStringLiteral("claims"), QJsonArray{}},
        {QStringLiteral("expires_at"), 0}};
}

// persist(proposed) → CAS(verified)；返回 action_key，失败返回空串。
static QString persistVerifiedProposal(MemoryManager &db, const QJsonObject &proposal,
                                       const QString &semanticStatus = QStringLiteral("not_required"))
{
    QString error;
    const auto stored = db.persistActionProposal(proposal, &error);
    if (stored.contains(QStringLiteral("error"))) {
        qWarning() << "persistActionProposal failed" << error;
        return {};
    }
    const QString key = stored.value(QStringLiteral("action_key")).toString();
    if (!db.updateActionProposalState(key, QStringLiteral("proposed"),
                                      QStringLiteral("verified"), semanticStatus, QString(),
                                      &error)) {
        qWarning() << "verified CAS failed" << key << error;
        return {};
    }
    return key;
}

static void rewindReviewCooldown(const QString &dbPath, const QString &tag)
{
    RawDb clock(dbPath, tag);
    if (!clock.open()) return;
    clock.exec(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120"));
}

static RequestMeta actionMeta(const QString &event)
{
    RequestMeta meta;
    meta.eventId = meta.runId = event;
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    return meta;
}

// 造一条"关注之后主人真实说过的话"，作为 contact now 的可核验依据。
static qlonglong insertOwnerEvidenceTurn(MemoryManager &mm, const QString &text,
                                         int reportedOffsetSeconds = 3600)
{
    static qlonglong serial = 0;
    RequestMeta meta = actionMeta(QStringLiteral("owner-evidence-%1").arg(++serial));
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(reportedOffsetSeconds);
    return mm.recordInput(text, meta, true);
}

static QJsonObject ownerEvidenceBasis(qlonglong turn)
{
    return QJsonObject{{"type", QStringLiteral("new_owner_evidence")},
                       {"refs", QJsonArray{QStringLiteral("chat/%1").arg(turn)}}};
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

// 领取并提交一条 consider 回顾，形成一个开放关注；contact 非空时同时产生 proposed 联系。
static qint64 seedOpenConcern(MemoryManager &mm, const RequestMeta &meta, const QString &input,
                              const QString &note, const QJsonObject &contact = {})
{
    const qint64 turn = mm.recordInput(input, meta, true);
    if (turn <= 0 || !mm.recordQuiet(turn, "close")) return 0;
    const auto claim = mm.claimPrivateReflection(12, 60);
    if (claim["id"].toInteger() != turn || claim["version"].toInt() != 1) return 0;
    if (!mm.finishPrivateReflection(turn, 1, "consider", note, contact, {}, {}, QString{}, {}))
        return 0;
    return turn;
}

static qint64 insertReminder(const QString &dbPath, const QString &tag, int dueOffsetSeconds,
                             const QString &message)
{
    static qlonglong serial = 0;
    RawDb raw(dbPath, tag);
    if (!raw.open()) return 0;
    qlonglong id = 0;
    if (!raw.insertReminder(--serial, Channel::kPet, Channel::kDefaultSession,
                            dueOffsetSeconds, 3600, message, &id)) return 0;
    return id;
}

// 造一个 completed、有产物、可分享的活动；返回 id 与本次回顾后的版本。
// R12-1b：前台 personal_interest 已改走 internal_activity，本测试显式构造 legacy 存量活动，
// 验证旧分享/Outbox 路径仍兼容（不伪造聊天用户轮次，只是补历史活动列）。
static qlonglong seedCompletedActivity(const QString &dbPath, MemoryManager &mm,
                                       const QString &userText,
                                       const QString &interestJson, const QString &draft,
                                       int *sharedVersion = nullptr)
{
    Q_UNUSED(mm)
    RawDb raw(dbPath, QStringLiteral("seed-completed-activity"));
    if (!raw.open()) return 0;
    if (!raw.execPrepared(
            QStringLiteral("INSERT INTO chat_history(user_input,raw_reply,channel,session_id,"
                           "speaker,sensitive,turn_kind,subject,thought,thought_result,"
                           "thought_version,review_after,timestamp) "
                           "VALUES(?,\'\',\'pet\',\'default\',\'assistant\',0,\'reply\',\'user\',?,?,1,0,datetime(\'now\'))"),
            {userText, QStringLiteral("写雨声"), QStringLiteral("{}")}))
        return 0;
    const qlonglong id = raw.scalar(QStringLiteral("SELECT last_insert_rowid()")).toLongLong();
    if (id <= 0) return 0;
    const QJsonObject result{
        {QStringLiteral("status"), QStringLiteral("completed")},
        {QStringLiteral("reflection"), QStringLiteral("写好了两句")},
        {QStringLiteral("draft"), draft},
        {QStringLiteral("artifact_ref"), QStringLiteral("thought/%1").arg(id)}};
    if (!raw.execPrepared(
            QStringLiteral("UPDATE chat_history SET thought_result=? WHERE id=?"),
            {QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)), id}))
        return 0;
    if (sharedVersion) *sharedVersion = 1;
    return id;
}

// 手工放一条网络渠道（QQ）的 queued 动作 + outbox 行，绕开 committer 的领域校验，
// 用于验证"入队成功但未拿到协议回执"的状态机。
static bool insertQueuedNetworkAction(const QString &dbPath, const QString &tag,
                                      const QString &actionKey, const QString &kind,
                                      const QString &channel, const QString &session,
                                      const QString &payloadJson)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return false;
    if (!raw.execPrepared(
            QStringLiteral("INSERT INTO cognitive_action(action_key,kind,trigger_ref,subject,"
                           "channel,session_id,payload_json,state) "
                           "VALUES(?,?,'manual','user',?,?,?,'queued')"),
            {actionKey, kind, channel, session, payloadJson}))
        return false;
    return raw.execPrepared(
        QStringLiteral("INSERT INTO action_outbox(action_id,action_key,channel,session_id,"
                       "payload_json,state,attempts) "
                       "SELECT id,?,?,?,payload_json,'queued',0 FROM cognitive_action "
                       "WHERE action_key=?"),
        {actionKey, channel, session, actionKey});
}

class TestActionOutbox : public QObject
{
    Q_OBJECT
private slots:
    // 1. contact 合法提交后同时存在 cognitive_action/action_outbox/reflection_contact=dispatching；
    //    失败提交（无 basis / 无提醒）不产生任何行。
    void commitIsAtomicWithOutbox()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("atomic.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);

        const qlonglong evidence = insertOwnerEvidenceTurn(db, QStringLiteral("明天想去看海"));
        QVERIFY(evidence > 0);
        const qint64 turn = seedOpenConcern(
            db, actionMeta(QStringLiteral("atomic-a")), QStringLiteral("想去看海"),
            QStringLiteral("看海"), contactNow(QStringLiteral("有新消息"), QStringLiteral("在吗"),
                                               ownerEvidenceBasis(evidence)));
        QVERIFY(turn > 0);
        const auto committed = committer.commitReflectionContact(turn, 1);
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        QCOMPARE(committed.value("action_key").toString(),
                 QStringLiteral("reflection:%1:1:contact").arg(turn));
        {
            RawDb raw(path, "atomic-ok");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(),
                     QStringLiteral("dispatching"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox")).toString(),
                     QStringLiteral("queued"));
        }

        // 失败提交：没有可核验依据的 contact now 在提交层必须失败且不写任何行。
        rewindReviewCooldown(path, QStringLiteral("atomic-rewind"));
        const qint64 rejectedTurn = seedOpenConcern(
            db, actionMeta(QStringLiteral("atomic-b")), QStringLiteral("随便聊聊"),
            QStringLiteral("聊天"), contactNow(QStringLiteral("想问问"), QStringLiteral("在吗")));
        QVERIFY(rejectedTurn > 0);
        QVERIFY(committer.commitReflectionContact(rejectedTurn, 1).contains(QStringLiteral("error")));
        // 不存在的提醒同样只返回错误。
        QVERIFY(committer.commitReminderDelivery(987654).contains(QStringLiteral("error")));
        {
            RawDb raw(path, "atomic-fail");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                    .arg(rejectedTurn)).toInt(),
                     0);
        }
    }

    // 2. 同一 contact/reminder/share 重复 commit 幂等：action_key 唯一、outbox 只有一行。
    void actionKeyUnique()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("unique.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);

        // contact
        const qlonglong evidence = insertOwnerEvidenceTurn(db, QStringLiteral("昨天说过想跑步"));
        const qint64 turn = seedOpenConcern(
            db, actionMeta(QStringLiteral("unique-a")), QStringLiteral("想跑步"),
            QStringLiteral("跑步"), contactNow(QStringLiteral("新消息"), QStringLiteral("跑了吗"),
                                               ownerEvidenceBasis(evidence)));
        QVERIFY(turn > 0);
        QVERIFY(!committer.commitReflectionContact(turn, 1).contains(QStringLiteral("error")));
        const auto contactAgain = committer.commitReflectionContact(turn, 1);
        QCOMPARE(contactAgain.value("idempotent").toBool(), true);
        QCOMPARE(contactAgain.value("state").toString(), QStringLiteral("queued"));

        // reminder
        const qint64 reminder = insertReminder(path, QStringLiteral("unique-rem"), -10,
                                               QStringLiteral("该喝水了"));
        QVERIFY(reminder > 0);
        QVERIFY(!committer.commitReminderDelivery(reminder).contains(QStringLiteral("error")));
        const auto reminderAgain = committer.commitReminderDelivery(reminder);
        QCOMPARE(reminderAgain.value("idempotent").toBool(), true);

        // share
        int sharedVersion = 0;
        const qlonglong activityId = seedCompletedActivity(
            path, db, QStringLiteral("想写雨声"), R"({"personal_interest":"写雨声"})",
            QStringLiteral("屋檐轻响。"), &sharedVersion);
        QVERIFY(activityId > 0);
        QVERIFY(!committer.commitActivityShare(activityId, sharedVersion, QStringLiteral("屋檐轻响。"))
                     .contains(QStringLiteral("error")));
        const auto shareAgain = committer.commitActivityShare(activityId, sharedVersion,
                                                              QStringLiteral("屋檐轻响。"));
        QCOMPARE(shareAgain.value("idempotent").toBool(), true);

        RawDb raw(path, "unique-raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action").toInt(), 3);
        QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 3);
        QCOMPARE(raw.scalar("SELECT count(DISTINCT action_key) FROM cognitive_action").toInt(), 3);
        QCOMPARE(raw.scalar("SELECT count(*) FROM delivery_ledger WHERE kind='share'").toInt(), 1);
    }

    // 3. claimOutbox 串行租约：连续两次拿到不同 action；同一 action 不会被并发领取。
    void claimOutboxSingleLease()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("lease.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 first = insertReminder(path, QStringLiteral("lease-a"), -10, QStringLiteral("第一"));
        const qint64 second = insertReminder(path, QStringLiteral("lease-b"), -10, QStringLiteral("第二"));
        QVERIFY(first > 0 && second > 0);
        QVERIFY(!committer.commitReminderDelivery(first).contains(QStringLiteral("error")));
        QVERIFY(!committer.commitReminderDelivery(second).contains(QStringLiteral("error")));

        const auto job1 = db.claimOutbox(QStringLiteral("owner-a"), 60);
        const auto job2 = db.claimOutbox(QStringLiteral("owner-a"), 60);
        QVERIFY(!job1.isEmpty());
        QVERIFY(!job2.isEmpty());
        QVERIFY(job1.value("action_key").toString() != job2.value("action_key").toString());
        // 两行都已在 dispatching：第三个领取者拿不到任何东西。
        QVERIFY(db.claimOutbox(QStringLiteral("owner-b"), 60).isEmpty());
        RawDb raw(path, "lease-raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox WHERE state='dispatching'").toInt(), 2);
        QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox WHERE lease_owner IS NOT NULL").toInt(), 2);
    }

    // 4. rejected 受控重试一次：settle rejected → outbox 回 queued、attempts=1；
    //    第二次 rejected → 不再领取。
    void rejectedRetriesOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("retry.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 reminder = insertReminder(path, QStringLiteral("retry-a"), -10,
                                               QStringLiteral("该休息了"));
        QVERIFY(reminder > 0);
        const auto committed = committer.commitReminderDelivery(reminder);
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString key = committed.value("action_key").toString();

        const auto first = db.claimOutbox(QStringLiteral("owner"), 60);
        QCOMPARE(first.value("action_key").toString(), key);
        QCOMPARE(first.value("attempts").toInt(), 1);
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("rejected"), QStringLiteral("submit_failed")));
        {
            RawDb raw(path, "retry-raw1");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("queued"));
            QCOMPARE(raw.scalar("SELECT attempts FROM action_outbox").toInt(), 1);
        }
        const auto second = db.claimOutbox(QStringLiteral("owner"), 60);
        QCOMPARE(second.value("action_key").toString(), key);
        QCOMPARE(second.value("attempts").toInt(), 2);
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("rejected"), QStringLiteral("submit_failed")));
        QVERIFY(db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
        {
            RawDb raw(path, "retry-raw2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("failed"));
            QCOMPARE(raw.scalar("SELECT attempts FROM action_outbox").toInt(), 2);
        }
    }

    // 5. unknown 永不重发：协议层可能已送达 → 冻结，不再被 claim。
    void unknownNeverResent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("unknown.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 reminder = insertReminder(path, QStringLiteral("unknown-a"), -10,
                                               QStringLiteral("该睡了"));
        QVERIFY(reminder > 0);
        const auto committed = committer.commitReminderDelivery(reminder);
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString key = committed.value("action_key").toString();
        QVERIFY(!db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("unknown"), QStringLiteral("timeout")));
        QVERIFY(db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
        // 迟到的明确接受也不能把 unknown 改回已送达（冻结语义，避免重复发送）。
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("protocol_accepted")));
        RawDb raw(path, "unknown-raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("unknown"));
        QCOMPARE(raw.scalar("SELECT state FROM cognitive_action").toString(), QStringLiteral("unknown"));
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM reminder WHERE id=%1").arg(reminder)).toString(),
                 QStringLiteral("unknown"));
    }

    // 6. 领取后租约过期 → recoverStale → action/outbox/领域状态=unknown，且不再领取。
    void staleDispatchingBecomesUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("stale.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 reminder = insertReminder(path, QStringLiteral("stale-a"), -10,
                                               QStringLiteral("该出门了"));
        QVERIFY(reminder > 0);
        const auto committed = committer.commitReminderDelivery(reminder);
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString key = committed.value("action_key").toString();
        QVERIFY(!db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
        {
            RawDb raw(path, "stale-expire");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE action_outbox SET lease_until=unixepoch()-7200 WHERE action_key='%1'").arg(key)));
        }
        QCOMPARE(db.recoverStaleOutbox(600), 1);
        QCOMPARE(db.recoverStaleOutbox(600), 0); // 幂等
        {
            RawDb raw(path, "stale-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("unknown"));
            QCOMPARE(raw.scalar("SELECT state FROM cognitive_action").toString(), QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM reminder WHERE id=%1").arg(reminder)).toString(),
                     QStringLiteral("unknown"));
        }
        QVERIFY(db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
    }

    // 7. reminder pending 只提交一次；settle accepted 后状态正确、不再有第二个外发。
    void reminderDeliveryOnlyOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("reminder.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 reminder = insertReminder(path, QStringLiteral("reminder-a"), -10,
                                               QStringLiteral("记得回消息"));
        QVERIFY(reminder > 0);
        const auto first = committer.commitReminderDelivery(reminder);
        QVERIFY2(!first.contains("error"), qPrintable(first.value("error").toString()));
        const auto second = committer.commitReminderDelivery(reminder);
        QCOMPARE(second.value("idempotent").toBool(), true);
        {
            RawDb raw(path, "reminder-raw1");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox WHERE action_key='reminder:%1:delivery'")
                                    .arg(reminder)).toInt(), 1);
        }
        const auto job = db.claimOutbox(QStringLiteral("owner"), 60);
        QCOMPARE(job.value("action_key").toString(), first.value("action_key").toString());
        QVERIFY(db.settleOutboxAction(job.value("action_key").toString(),
                                      QStringLiteral("protocol_accepted"), QStringLiteral("ok")));
        {
            RawDb raw(path, "reminder-raw2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM reminder WHERE id=%1").arg(reminder)).toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("dispatched"));
            QCOMPARE(raw.scalar("SELECT last_outcome FROM cognitive_action").toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar("SELECT count(*) FROM delivery_ledger WHERE kind='reminder'").toInt(), 1);
        }
        // 已送达后再次提交仍是幂等，不会产生第二个外发。
        QCOMPARE(committer.commitReminderDelivery(reminder).value("idempotent").toBool(), true);
        QVERIFY(db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
    }

    // 8. share settle unknown 后，再次 commitActivityShare 返回已存在/拒绝，不产生第二个 outbox。
    void shareUnknownBlocksReshare()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("share.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        int sharedVersion = 0;
        const qlonglong activityId = seedCompletedActivity(
            path, db, QStringLiteral("想写风声"), R"({"personal_interest":"写风声"})",
            QStringLiteral("风从窗缝里钻进来。"), &sharedVersion);
        QVERIFY(activityId > 0);
        const auto committed = committer.commitActivityShare(
            activityId, sharedVersion, QStringLiteral("风从窗缝里钻进来。"));
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString key = committed.value("action_key").toString();
        const auto job = db.claimOutbox(QStringLiteral("owner"), 60);
        QCOMPARE(job.value("action_key").toString(), key);
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("unknown"), QStringLiteral("timeout")));
        // 同一版本 again：动作已存在，幂等返回 unknown；不会建第二个 outbox。
        const auto again = committer.commitActivityShare(
            activityId, sharedVersion, QStringLiteral("风从窗缝里钻进来。"));
        QCOMPARE(again.value("idempotent").toBool(), true);
        QCOMPARE(again.value("state").toString(), QStringLiteral("unknown"));
        QVERIFY(!db.canShareThought(activityId, sharedVersion));
        QVERIFY(db.claimOutbox(QStringLiteral("owner"), 60).isEmpty());
        RawDb raw(path, "share-raw");
        QVERIFY(raw.open());
        QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
        QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action").toInt(), 1);
        QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='share' AND ref='thought:%1:%2'")
                                .arg(activityId).arg(sharedVersion)).toString(),
                 QStringLiteral("unknown"));
    }

    // 9. dispatcher 是 outbox 的唯一消费者：多句合并成一条消息、echo=action_key 一条动作一次发送；
    //    没有 sender 时不会领取；渠道回执按 echo 归一到 action_key 并升级终态。
    void dispatcherIsTheOnlyOutboxConsumer()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("dispatcher.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 reminder = insertReminder(path, QStringLiteral("dispatch-a"), -10,
                                               QStringLiteral("该吃饭了"));
        QVERIFY(reminder > 0);
        const auto committed = committer.commitReminderDelivery(reminder);
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString key = committed.value("action_key").toString();

        // 没有 sender 的 dispatcher 不得领取（避免"领了却发不出去"卡住 outbox）。
        ActionOutboxDispatcher idle(&db);
        idle.pollOnce();
        {
            RawDb raw(path, "dispatch-idle");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("queued"));
        }

        // 多句合并：手工放一条含两句 payload 的 action，dispatcher 必须一次发送整条文本。
        {
            RawDb raw(path, "dispatch-merge");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO cognitive_action(action_key,kind,trigger_ref,subject,channel,session_id,"
                "payload_json,state) VALUES('manual:merge:1','SpontaneousContact','reflection:1:1',"
                "'user','pet','default',"
                "'{\"sentences\":[{\"zh_text\":\"第一句\"},{\"zh_text\":\"第二句\"}],"
                "\"channel\":\"pet\",\"session\":\"default\"}','queued')")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,state,attempts) "
                "SELECT id,'manual:merge:1','pet','default',payload_json,'queued',0 "
                "FROM cognitive_action WHERE action_key='manual:merge:1'")));
        }

        struct Sent { Utterance utterance; bool accepted = true; };
        QList<Sent> sent;
        QList<QPair<QString, QString>> settled;
        ActionOutboxDispatcher dispatcher(&db);
        dispatcher.setSender([&sent](const Utterance &u) {
            Sent item;
            item.utterance = u;
            sent.append(item);
            return true;
        });
        connect(&dispatcher, &ActionOutboxDispatcher::actionSettled,
                this, [&settled](const QString &actionKey, const QString &outcome,
                                 const QJsonObject &) {
            settled.append(qMakePair(actionKey, outcome));
        });

        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.first().utterance.text, QStringLiteral("该吃饭了"));
        QCOMPARE(sent.first().utterance.deliveryEcho, key);
        QCOMPARE(sent.first().utterance.channel, Channel::kPet);
        QCOMPARE(sent.first().utterance.sessionId, Channel::kDefaultSession);
        QCOMPARE(settled.size(), 1);
        QCOMPARE(settled.first().first, key);
        QCOMPARE(settled.first().second, QStringLiteral("handed_off"));

        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 2);
        QCOMPARE(sent.at(1).utterance.text, QStringLiteral("第一句\n第二句"));
        QCOMPARE(sent.at(1).utterance.deliveryEcho, QStringLiteral("manual:merge:1"));
        // 再轮一次：两条都已 dispatched，不会重复发送。
        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 2);
        {
            RawDb raw(path, "dispatch-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox WHERE state='dispatched'").toInt(), 2);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatched"));
        }

        // QQ 回执：echo=action_key → 提交后保持 dispatching，收到 protocol_accepted 才落终态。
        {
            RawDb raw(path, "dispatch-qq");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO cognitive_action(action_key,kind,trigger_ref,subject,channel,session_id,"
                "payload_json,state) VALUES('thought:7:2:share','ActivityShare','thought:7:2',"
                "'user','qq_private','10001',"
                "'{\"draft\":\"风从窗缝里钻进来。\",\"channel\":\"qq_private\",\"session\":\"10001\"}',"
                "'queued')")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,state,attempts) "
                "SELECT id,'thought:7:2:share','qq_private','10001',payload_json,'queued',0 "
                "FROM cognitive_action WHERE action_key='thought:7:2:share'")));
        }
        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 3);
        QCOMPARE(sent.last().utterance.deliveryEcho, QStringLiteral("thought:7:2:share"));
        dispatcher.onSendResult(QStringLiteral("thought:7:2:share"), QStringLiteral("protocol_accepted"));
        {
            RawDb raw(path, "dispatch-qq2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox "
                                               "WHERE action_key='thought:7:2:share'")).toString(),
                     QStringLiteral("dispatched"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM cognitive_action "
                                               "WHERE action_key='thought:7:2:share'")).toString(),
                     QStringLiteral("protocol_accepted"));
        }
        // 回执被拒：outbox 回 queued（attempts<2），下一次 poll 受控重试一次。
        {
            RawDb raw(path, "dispatch-reject");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO cognitive_action(action_key,kind,trigger_ref,subject,channel,session_id,"
                "payload_json,state) VALUES('thought:8:2:share','ActivityShare','thought:8:2',"
                "'user','qq_private','10001','{\"draft\":\"再来一次\",\"channel\":\"qq_private\","
                "\"session\":\"10001\"}','queued')")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,state,attempts) "
                "SELECT id,'thought:8:2:share','qq_private','10001',payload_json,'queued',0 "
                "FROM cognitive_action WHERE action_key='thought:8:2:share'")));
        }
        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 4);
        dispatcher.onSendResult(QStringLiteral("thought:8:2:share"), QStringLiteral("rejected"));
        {
            RawDb raw(path, "dispatch-reject2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox WHERE action_key='thought:8:2:share'")
                         .toString(),
                     QStringLiteral("queued"));
        }
        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 5);
        dispatcher.onSendResult(QStringLiteral("thought:8:2:share"), QStringLiteral("rejected"));
        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 5); // 第二次拒绝后终止，不再发送
        // 启动恢复入口在 dispatcher 上也存在（转发给 MemoryManager）。
        QCOMPARE(dispatcher.recoverStale(600), 0);
    }

    // 10. P1：QQ 入队成功不等于送达。pollOnce 后 outbox/action 必须仍是 dispatching，
    //     不 emit handed_off；等到 onSendResult(protocol_accepted) 才落终态。
    void qqEnqueueAwaitsReceipt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("qq-await.db");
        MemoryManager db(path);
        const QString key = QStringLiteral("reminder:424242:delivery");
        QVERIFY(insertQueuedNetworkAction(
            path, QStringLiteral("qq-await-seed"), key, QStringLiteral("ReminderDelivery"),
            Channel::kQQPrivate, QStringLiteral("10001"),
            QStringLiteral(R"({"message":"早点睡","channel":"qq_private","session":"10001"})")));

        struct Sent { Utterance utterance; bool accepted = true; };
        QList<Sent> sent;
        QList<QPair<QString, QString>> settled;
        ActionOutboxDispatcher dispatcher(&db);
        dispatcher.setSender([&sent](const Utterance &u) {
            Sent item;
            item.utterance = u;
            sent.append(item);
            return true;
        });
        connect(&dispatcher, &ActionOutboxDispatcher::actionSettled,
                this, [&settled](const QString &actionKey, const QString &outcome,
                                 const QJsonObject &) {
            settled.append(qMakePair(actionKey, outcome));
        });

        dispatcher.pollOnce();
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.first().utterance.channel, Channel::kQQPrivate);
        QCOMPARE(sent.first().utterance.sessionId, QStringLiteral("10001"));
        QCOMPARE(sent.first().utterance.text, QStringLiteral("早点睡"));
        QCOMPARE(sent.first().utterance.deliveryEcho, key);
        // 没有协议回执前：既不能 settle handed_off，也不能把 outbox 标成 dispatched。
        QVERIFY(settled.isEmpty());
        {
            RawDb raw(path, "qq-await-raw1");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatching"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatching"));
            // 租约从 60s 提到 300s：回执窗口必须足够长（启动恢复阈值仍是 600s）。
            const int lease = raw.scalar(QStringLiteral(
                "SELECT lease_until-unixepoch() FROM action_outbox WHERE action_key='%1'").arg(key)).toInt();
            QVERIFY2(lease >= 290 && lease <= 300, qPrintable(QString::number(lease)));
        }

        dispatcher.onSendResult(key, QStringLiteral("protocol_accepted"));
        QCOMPARE(settled.size(), 1);
        QCOMPARE(settled.first().first, key);
        QCOMPARE(settled.first().second, QStringLiteral("protocol_accepted"));
        {
            RawDb raw(path, "qq-await-raw2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatched"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("handed_off"));
            QCOMPARE(raw.scalar(QStringLiteral(
                                    "SELECT last_outcome FROM cognitive_action WHERE action_key='%1'").arg(key))
                         .toString(),
                     QStringLiteral("protocol_accepted"));
        }
    }

    // 11. P1：入队后、回执前崩溃 → 租约过期后 recoverStale(600) 冻结为 unknown，
    //     绝不重发；迟到的 protocol_accepted 也不能把 unknown 改回已送达。
    void qqCrashBeforeReceiptBecomesUnknown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("qq-crash.db");
        MemoryManager db(path);
        const QString key = QStringLiteral("reminder:515151:delivery");
        QVERIFY(insertQueuedNetworkAction(
            path, QStringLiteral("qq-crash-seed"), key, QStringLiteral("ReminderDelivery"),
            Channel::kQQPrivate, QStringLiteral("10001"),
            QStringLiteral(R"({"message":"别忘了喝水","channel":"qq_private","session":"10001"})")));

        ActionOutboxDispatcher dispatcher(&db);
        dispatcher.setSender([](const Utterance &) { return true; });
        dispatcher.pollOnce();
        {
            RawDb raw(path, "qq-crash-expire");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatching"));
            // 模拟"提交后进程崩溃、重启时租约已过期超过 600s 阈值"。
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE action_outbox SET lease_until=unixepoch()-7200,"
                "dispatched_at=unixepoch()-7200 WHERE action_key='%1'").arg(key)));
        }
        QCOMPARE(dispatcher.recoverStale(600), 1);
        {
            RawDb raw(path, "qq-crash-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("unknown"));
        }
        // unknown 不可重领；迟到的明确接受也必须被冻结语义忽略。
        QVERIFY(db.claimOutbox(QStringLiteral("owner-after-crash"), 300).isEmpty());
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("protocol_accepted")));
        {
            RawDb raw(path, "qq-crash-frozen");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("unknown"));
        }
        // unknown 不代表"茉子说过"（历史回写资格由 dispatcher 统一判定）。
        QVERIFY(!ActionOutboxDispatcher::proactiveHistoryEligible(QStringLiteral("unknown"),
                                                                  Channel::kQQPrivate));
    }

    // 12. P2：start() 不再隐式恢复。stale dispatching 行在 start() + 一次轮询后保持原状，
    //     只有显式 recoverStale(600) 才会冻结为 unknown（恢复点唯一）。
    void startDoesNotImplicitlyRecover()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath("start-recover.db");
        MemoryManager db(path);
        const QString key = QStringLiteral("reminder:616161:delivery");
        QVERIFY(insertQueuedNetworkAction(
            path, QStringLiteral("start-recover-seed"), key, QStringLiteral("ReminderDelivery"),
            Channel::kQQPrivate, QStringLiteral("10001"),
            QStringLiteral(R"({"message":"该睡了","channel":"qq_private","session":"10001"})")));
        // 模拟上一进程领取后崩溃：行已是 dispatching，且租约早已过期。
        QVERIFY(!db.claimOutbox(QStringLiteral("crashed-process"), 60).isEmpty());
        {
            RawDb raw(path, "start-recover-expire");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE action_outbox SET lease_until=unixepoch()-7200 WHERE action_key='%1'").arg(key)));
        }

        QList<QPair<QString, QString>> settled;
        ActionOutboxDispatcher dispatcher(&db);
        dispatcher.setSender([](const Utterance &) { return true; });
        connect(&dispatcher, &ActionOutboxDispatcher::actionSettled,
                this, [&settled](const QString &actionKey, const QString &outcome,
                                 const QJsonObject &) {
            settled.append(qMakePair(actionKey, outcome));
        });
        dispatcher.start(100);
        QTest::qWait(250); // 让启动后的轮询跑几拍：不能发生隐式恢复，也不需要发送
        {
            RawDb raw(path, "start-recover-still");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatching"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("dispatching"));
        }
        QVERIFY(settled.isEmpty());
        dispatcher.stop();
        QCOMPARE(dispatcher.recoverStale(600), 1);
        {
            RawDb raw(path, "start-recover-done");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(key)).toString(),
                     QStringLiteral("unknown"));
        }
    }

    // 13. P1：主动历史回写资格。"茉子说过"只能写至少已经交给渠道的结果。
    void proactiveHistoryEligibility()
    {
        QVERIFY(ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("protocol_accepted"), Channel::kQQPrivate));
        QVERIFY(ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("protocol_accepted"), Channel::kPet));
        QVERIFY(ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("handed_off"), Channel::kPet));
        // QQ 的 handed_off 只是入队，不是送达。
        QVERIFY(!ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("handed_off"), Channel::kQQPrivate));
        // unknown/rejected/withheld 一律不能写成"她说过"。
        QVERIFY(!ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("unknown"), Channel::kQQPrivate));
        QVERIFY(!ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("unknown"), Channel::kPet));
        QVERIFY(!ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("rejected"), Channel::kQQPrivate));
        QVERIFY(!ActionOutboxDispatcher::proactiveHistoryEligible(
            QStringLiteral("withheld"), Channel::kPet));
    }
    // 14. R12-2b：verified 提案提交一次动作/Outbox/账本；重复 action_key 幂等不重复 outbox。
    void verifiedProposalCommitsOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath("verified-commit.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeId = seedOpenWakeRow(path, QStringLiteral("vc-wake"), runId);
        QVERIFY(wakeId > 0);
        const QString key = persistVerifiedProposal(
            db, wakeContactProposal(wakeId, 1, runId, QStringLiteral("在忙吗？")));
        QVERIFY(!key.isEmpty());
        QCOMPARE(key, QStringLiteral("wake:%1:1:contact").arg(wakeId));
        {
            RawDb raw(path, QStringLiteral("vc-pre"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal")).toString(),
                     QStringLiteral("verified"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
        }
        const auto committed = committer.commitOpenWakeContact(key);
        QVERIFY2(!committed.contains(QStringLiteral("error")),
                 qPrintable(committed.value(QStringLiteral("error")).toString()));
        QCOMPARE(committed.value(QStringLiteral("proposal_state")).toString(),
                 QStringLiteral("committed"));
        QCOMPARE(committed.value(QStringLiteral("action_key")).toString(), key);
        {
            RawDb raw(path, QStringLiteral("vc-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM cognitive_action")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger "
                                               "WHERE kind='contact'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal")).toString(),
                     QStringLiteral("committed"));
            // 不写 legacy contact/reflection，不伪造聊天历史。
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 0);
        }
        // 重复提交：返回既有一行（idempotent=true），不重复建动作/Outbox。
        const auto again = committer.commitVerifiedActionProposal(key);
        QCOMPARE(again.value(QStringLiteral("idempotent")).toBool(), true);
        QCOMPARE(again.value(QStringLiteral("action_key")).toString(), key);
        {
            RawDb raw(path, QStringLiteral("vc-idem"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM cognitive_action")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
        }
    }

    // 15. R12-2b：allow_contact 独立开关。false 时任何 contact 都不入 outbox，提案落 withheld；
    //     true + 安静时段 → quiet（保留 verified 供恢复，不发送）；共享 contact 每日额度对
    //     wake 来源同样生效（所有来源统一统计 delivery_ledger）。
    void verifiedContactHonorsSwitchesAndGate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        const QString path = dir.filePath("verified-gates.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);

        // (1) allow_contact=false：提交层拒绝并落 withheld，不发送、不写动作行。
        QVERIFY(identity.enableOpenWakeContact(false));
        const QString runA = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeA = seedOpenWakeRow(path, QStringLiteral("vg-disabled"), runA);
        QVERIFY(wakeA > 0);
        const QString keyA = persistVerifiedProposal(
            db, wakeContactProposal(wakeA, 1, runA, QStringLiteral("在吗")));
        QVERIFY(!keyA.isEmpty());
        const auto disabled = committer.commitOpenWakeContact(keyA);
        QCOMPARE(disabled.value(QStringLiteral("error")).toString(),
                 QStringLiteral("allow_contact_disabled"));
        {
            RawDb raw(path, QStringLiteral("vg-disabled-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal")).toString(),
                     QStringLiteral("withheld"));
        }

        // (2) allow_contact=true 但当前处于安静时段：quiet 是可恢复失败，提案保持 verified。
        QVERIFY(identity.enableOpenWakeContact(true, -1, QStringLiteral("00:00"),
                                               QStringLiteral("23:59")));
        const QString runB = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeB = seedOpenWakeRow(path, QStringLiteral("vg-quiet"), runB);
        const QString keyB = persistVerifiedProposal(
            db, wakeContactProposal(wakeB, 1, runB, QStringLiteral("在吗")));
        QVERIFY(!keyB.isEmpty());
        const auto quiet = committer.commitOpenWakeContact(keyB);
        QCOMPARE(quiet.value(QStringLiteral("error")).toString(), QStringLiteral("quiet"));
        {
            RawDb raw(path, QStringLiteral("vg-quiet-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='"
                                               "wake:%1:1:contact'").arg(wakeB)).toString(),
                     QStringLiteral("verified"));
        }
        // 恢复：安静时段解除后，定时 drain 会把保留的 verified 提案补提交一次。
        QVERIFY(identity.enableOpenWakeContact(true));
        const auto resumed = db.drainVerifiedActionProposals(8);
        QCOMPARE(resumed.value(QStringLiteral("committed")).toInt(), 1);
        {
            RawDb raw(path, QStringLiteral("vg-quiet-resumed"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger "
                                               "WHERE kind='contact'")).toInt(), 1);
        }

        // (3) 共享额度：contact_daily_limit=2，第一条提交成功，第二条被 limit 挡住。
        QVERIFY(identity.enableOpenWakeContact(true, 2));
        const QString runC = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeC = seedOpenWakeRow(path, QStringLiteral("vg-limit-a"), runC);
        const QString keyC = persistVerifiedProposal(
            db, wakeContactProposal(wakeC, 1, runC, QStringLiteral("早安")));
        QVERIFY(!committer.commitOpenWakeContact(keyC).contains(QStringLiteral("error")));
        const QString runD = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeD = seedOpenWakeRow(path, QStringLiteral("vg-limit-b"), runD);
        const QString keyD = persistVerifiedProposal(
            db, wakeContactProposal(wakeD, 1, runD, QStringLiteral("午安")));
        const auto limited = committer.commitOpenWakeContact(keyD);
        QCOMPARE(limited.value(QStringLiteral("error")).toString(), QStringLiteral("limit"));
        {
            RawDb raw(path, QStringLiteral("vg-limit-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 2);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger "
                                               "WHERE kind='contact'")).toInt(), 2);
        }

        // (4) 来源失效（open_wake 版本已变）：stale_origin，提案落 withheld，不发送。
        const QString runE = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeE = seedOpenWakeRow(path, QStringLiteral("vg-stale"), runE);
        const QString keyE = persistVerifiedProposal(
            db, wakeContactProposal(wakeE, 1, runE, QStringLiteral("晚安")));
        {
            RawDb raw(path, QStringLiteral("vg-stale-bump"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE open_wake SET version=2 WHERE id=%1").arg(wakeE)));
        }
        const auto stale = committer.commitOpenWakeContact(keyE);
        QCOMPARE(stale.value(QStringLiteral("error")).toString(), QStringLiteral("stale_origin"));
        {
            RawDb raw(path, QStringLiteral("vg-stale-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 2);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='"
                                               "wake:%1:1:contact'").arg(wakeE)).toString(),
                     QStringLiteral("withheld"));
        }
    }

    // 16. R12-2b：internal 活动分享提交只入 Outbox；settle/回执回写 share_outcome，
    //     不改变活动完成状态、不伪造 chat/reflection；未知/撤回/成果版本不符都拒绝或冻结。
    void internalShareCommitAndReceipt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath("internal-share.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        QString hash;
        const QString draft = QStringLiteral("屋檐轻响。");
        const qlonglong activityId = seedInternalCompletedActivity(
            path, QStringLiteral("is-seed"), draft, &hash);
        QVERIFY(activityId > 0);
        const QString key = persistVerifiedProposal(
            db, internalShareProposal(activityId, 2, draft));
        QVERIFY(!key.isEmpty());
        QCOMPARE(key, QStringLiteral("activity:%1:2:share").arg(activityId));
        const auto committed = committer.commitInternalActivityShare(key);
        QVERIFY2(!committed.contains(QStringLiteral("error")),
                 qPrintable(committed.value(QStringLiteral("error")).toString()));
        {
            RawDb raw(path, QStringLiteral("is-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM cognitive_action")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger "
                                               "WHERE kind='share'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT status FROM internal_activity WHERE id=%1")
                                    .arg(activityId)).toString(),
                     QStringLiteral("completed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact")).toInt(), 0);
        }
        // 协议接受回执：账本终态 + 活动 result_json.share_outcome/share_version，状态仍 completed。
        const auto job = db.claimOutbox(QStringLiteral("test"), 60);
        QCOMPARE(job.value(QStringLiteral("action_key")).toString(), key);
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("protocol_accepted")));
        {
            RawDb raw(path, QStringLiteral("is-settled"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT status FROM internal_activity WHERE id=%1")
                                    .arg(activityId)).toString(),
                     QStringLiteral("completed"));
            QCOMPARE(raw.scalar(QStringLiteral(
                                    "SELECT json_extract(result_json,'$.share_outcome') "
                                    "FROM internal_activity WHERE id=%1").arg(activityId)).toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar(QStringLiteral(
                                    "SELECT json_extract(result_json,'$.share_version') "
                                    "FROM internal_activity WHERE id=%1").arg(activityId)).toInt(),
                     2);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger "
                                               "WHERE kind='share' AND ref='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("protocol_accepted"));
        }
        // 回执幂等：重复成功回执/迟到的 unknown 都不回退成功字段。
        QVERIFY(db.recordActionReceipt(key, QStringLiteral("protocol_accepted")));
        QVERIFY(db.recordActionReceipt(key, QStringLiteral("unknown")));
        {
            RawDb raw(path, QStringLiteral("is-idem"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                                    "SELECT json_extract(result_json,'$.share_outcome') "
                                    "FROM internal_activity WHERE id=%1").arg(activityId)).toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger "
                                               "WHERE kind='share' AND ref='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("protocol_accepted"));
        }

        // 撤回：share_intent 被置 withdrawn 后，同成果版本的新提案提交失败（artifact_mismatch）。
        const QString draft2 = QStringLiteral("风从窗缝里钻进来。");
        const qlonglong withdrawnId = seedInternalCompletedActivity(
            path, QStringLiteral("is-withdraw-seed"), draft2);
        QVERIFY(withdrawnId > 0);
        {
            RawDb raw(path, QStringLiteral("is-withdraw"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE internal_activity SET result_json=json_set(result_json,"
                "'$.share_intent.state','withdrawn') WHERE id=%1").arg(withdrawnId)));
        }
        const QString withdrawnKey = persistVerifiedProposal(
            db, internalShareProposal(withdrawnId, 2, draft2));
        const auto withdrawn = committer.commitInternalActivityShare(withdrawnKey);
        QCOMPARE(withdrawn.value(QStringLiteral("error")).toString(),
                 QStringLiteral("artifact_mismatch"));
        {
            RawDb raw(path, QStringLiteral("is-withdraw-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + withdrawnKey + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
        }
    }

    // 17. R12-2b：internal 分享的成果版本不符 → artifact_mismatch；过期 → expired（不发送）。
    void internalShareRejectsStaleArtifactAndExpiry()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath("share-stale.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const QString draft = QStringLiteral("雨声落在瓦上。");
        const qlonglong activityId = seedInternalCompletedActivity(
            path, QStringLiteral("ss-seed"), draft);
        QVERIFY(activityId > 0);

        // 错误版本（1 ≠ 成果版本 2）：persist 层不校验，提交层必须 artifact_mismatch。
        QJsonObject wrongVersion = internalShareProposal(activityId, 1, draft);
        const QString wrongKey = persistVerifiedProposal(db, wrongVersion);
        QVERIFY(!wrongKey.isEmpty());
        const auto mismatch = committer.commitInternalActivityShare(wrongKey);
        QCOMPARE(mismatch.value(QStringLiteral("error")).toString(),
                 QStringLiteral("artifact_mismatch"));

        // 过期提案：verified 但 expires_at 已过 → 提交拒绝并落 expired。
        QJsonObject expiring = internalShareProposal(activityId, 2, draft);
        expiring[QStringLiteral("expires_at")] = QDateTime::currentSecsSinceEpoch() - 5;
        const QString expiringKey = persistVerifiedProposal(db, expiring);
        QVERIFY(!expiringKey.isEmpty());
        const auto expired = committer.commitInternalActivityShare(expiringKey);
        QCOMPARE(expired.value(QStringLiteral("error")).toString(), QStringLiteral("stale_origin"));
        {
            RawDb raw(path, QStringLiteral("ss-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + expiringKey + QStringLiteral("'")).toString(),
                     QStringLiteral("expired"));
        }
    }

    // 18. R12-2b：启动/定时 drain verified 提案；已 committed 不重发，outbox 只建一次。
    void drainVerifiedProposalsIsIdempotent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath("drain.db");
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const QString runA = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString runB = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeA = seedOpenWakeRow(path, QStringLiteral("dr-a"), runA);
        const qlonglong wakeB = seedOpenWakeRow(path, QStringLiteral("dr-b"), runB);
        QVERIFY(wakeA > 0 && wakeB > 0);
        QVERIFY(!persistVerifiedProposal(db, wakeContactProposal(wakeA, 1, runA,
                                                                 QStringLiteral("早安"))).isEmpty());
        QVERIFY(!persistVerifiedProposal(db, wakeContactProposal(wakeB, 1, runB,
                                                                 QStringLiteral("午安"))).isEmpty());
        const auto first = db.drainVerifiedActionProposals(8);
        QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), 2);
        QCOMPARE(first.value(QStringLiteral("committed")).toInt(), 2);
        {
            RawDb raw(path, QStringLiteral("dr-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 2);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal "
                                               "WHERE state='committed'")).toInt(), 2);
        }
        // 第二次 drain：没有 verified 提案可做，不会重复建 outbox。
        QCOMPARE(committer.drainVerifiedActionProposals(8).value(QStringLiteral("scanned")).toInt(), 0);
        {
            RawDb raw(path, QStringLiteral("dr-idem"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 2);
        }
    }

};

QTEST_GUILESS_MAIN(TestActionOutbox)
#include "tst_actionoutbox.moc"
