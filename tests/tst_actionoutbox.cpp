#include <QScopeGuard>
#include <QWebSocketServer>
#include "qqchanneladapter.h"
#include "qqprotocolclient.h"
#include "testactionverification.h"
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
#include <QTime>
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
    // R12-2c-2：关闭主动联系（A12）：已入 Outbox 的行必须在 sender 前被拦截（零发送）。
    // open_wake.allow_contact 仍保持可用，单独验证 proactive.enabled 的撤回效果。
    bool disableProactive()
    {
        QJsonObject proactive{{"enabled", false},
                              {"private_writing", false},
                              {"targets", QJsonArray{}}};
        const QJsonObject openWake{{"enabled", true},
                                   {"allow_contact", true},
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
                                       const QString &message, qint64 epoch = 0,
                                       const QString &channel = Channel::kPet,
                                       const QString &session = Channel::kDefaultSession)
{
    const ActionOrigin origin = ActionOrigin::forWake(
        wakeId, version, runId, QStringLiteral("owner"), epoch,
        channel, session);
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("contact")},
        {QStringLiteral("origin"), origin.toJson()},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), channel},
                     {QStringLiteral("session_id"), session}}},
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

// persist(proposed) → markVerified(写入内容指纹/request_id/fence 快照)；返回 action_key，失败返回空串。
// R12-2c-2：verified 只能经 MemoryManager::markActionProposalVerified 推进（生产路径由
// PrivateReview 传入 LLMService 的 verifier request_id；测试这里用固定前缀的假 id）。
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
    const QString requestId = QStringLiteral("test-request-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!markTestActionProposalVerified(db, key, semanticStatus, requestId, &error)) {
        qWarning() << "markActionProposalVerified failed" << key << error;
        return {};
    }
    return key;
}

// R12-2c-2：造一条已 verified 并成功提交到 Outbox 的 open wake contact；返回 action_key。
// 仅入 Outbox（发送由测试里的 dispatcher/mock sender 决定），供 A12/A13/A16 使用。
static QString seedCommittedWakeContact(MemoryManager &db, const QString &dbPath,
                                        const QString &tag, const QString &message,
                                        qint64 expiresAt = 0)
{
    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qlonglong wakeId = seedOpenWakeRow(dbPath, tag, runId);
    if (wakeId <= 0) return {};
    QJsonObject proposal = wakeContactProposal(wakeId, 1, runId, message);
    if (expiresAt != 0) proposal[QStringLiteral("expires_at")] = expiresAt;
    const QString key = persistVerifiedProposal(db, proposal);
    if (key.isEmpty()) return {};
    ActionCommitter committer(&db);
    const auto committed = committer.commitOpenWakeContact(key);
    if (committed.contains(QStringLiteral("error"))) {
        qWarning() << "seedCommittedWakeContact failed"
                   << committed.value(QStringLiteral("error")).toString();
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
    void qqOutboxProtocolBoundary_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("Q01-accepted") << QStringLiteral("none");
        QTest::newRow("Q02-Q05-revoked") << QStringLiteral("revoke");
        QTest::newRow("Q03-context") << QStringLiteral("context");
        QTest::newRow("Q04-retry-revoked") << QStringLiteral("retry");
    }

    void qqOutboxProtocolBoundary()
    {
        QFETCH(QString, change);
        ProactiveIdentityGuard identityGuard;
        auto &config = ConfigManager::instance();
        QFile settings(QDir(config.getConfigDirPath()).filePath(QStringLiteral("setting.json")));
        const bool hadSettings = settings.open(QIODevice::ReadOnly);
        const auto original = hadSettings ? settings.readAll() : QByteArray("{}");
        settings.close();
        const auto restoreSettings = qScopeGuard([&] {
            config.writeConfigDocument(QStringLiteral("setting.json"), original, nullptr);
            if (!hadSettings) settings.remove();
        });
        const auto setCrossChannel = [&](bool enabled) {
            auto document = QJsonDocument::fromJson(original).object();
            auto memory = document.value("memory").toObject();
            memory.insert("owner_cross_channel_private", enabled);
            document.insert("memory", memory);
            return config.writeConfigDocument(QStringLiteral("setting.json"),
                QJsonDocument(document).toJson(QJsonDocument::Compact), nullptr);
        };
        const QString channel = Channel::kQQPrivate;
        const QString session = QStringLiteral("10001");
        QJsonObject identity{
            {"identity", QJsonObject{{"master", QJsonObject{{"qq", QJsonArray{session}}}}}},
            {"proactive", QJsonObject{{"enabled", true}, {"contact_daily_limit", 0},
                {"quiet_hours", QJsonObject{{"start", "00:00"}, {"end", "00:00"}}},
                {"targets", QJsonArray{QJsonObject{{"channel", channel}, {"session_id", session}}}}}},
            {"cognition", QJsonObject{{"open_wake", QJsonObject{{"enabled", true}, {"allow_contact", true}}}}}};
        const auto writeIdentity = [&] {
            return config.writeConfigDocument(QStringLiteral("identity.json"),
                QJsonDocument(identity).toJson(QJsonDocument::Compact), nullptr);
        };
        QVERIFY(writeIdentity());
        QVERIFY(setCrossChannel(true));
        QVERIFY(config.isMasterQQ(session));
        QVERIFY(config.getMemoryPolicyConfig().agent.ownerCrossChannelPrivate);
        QVERIFY(config.getProactiveConfig().targets.contains(qMakePair(channel, session)));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto path = temp.filePath(QStringLiteral("qq-outbox.db"));
        MemoryManager db(path);
        const auto run = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto wake = seedOpenWakeRow(path, QStringLiteral("qq-outbox"), run);
        QVERIFY(wake > 0);
        const auto proposal = wakeContactProposal(wake, 1, run, QStringLiteral("Hello"),
                                                   0, channel, session);
        const auto key = persistVerifiedProposal(db, proposal);
        QVERIFY(!key.isEmpty());
        if (change == QStringLiteral("none")) {
            QVERIFY(setCrossChannel(false));
            const auto denied = db.commitVerifiedActionProposal(key);
            QCOMPARE(denied.value("error").toString(), QStringLiteral("target_unreachable"));
            QCOMPARE(denied.value("detail").toString(), QStringLiteral("target_not_visible"));
            QVERIFY(setCrossChannel(true));
        }
        const auto result = db.commitVerifiedActionProposal(key);
        const auto diagnostic = QJsonDocument(QJsonObject{
            {"result", result}, {"action_key", key}, {"origin", proposal.value("origin")},
            {"target", proposal.value("target")}, {"state", db.actionProposal(key).value("state")}
        }).toJson(QJsonDocument::Compact);
        QVERIFY2(!result.contains("error"), diagnostic.constData());

        QWebSocketServer server(QStringLiteral("outbox"), QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QStringList frames;
        connect(&server, &QWebSocketServer::newConnection, &server, [&] {
            auto *peer = server.nextPendingConnection();
            peer->setParent(&server);
            connect(peer, &QWebSocket::textMessageReceived, &server, [&, peer](const QString &text) {
                const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
                if (frame.value("action") != QStringLiteral("send_private_msg")) return;
                const auto echo = frame.value("echo").toString();
                frames.append(echo);
                const bool reject = echo == key && change == QStringLiteral("retry");
                peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"echo", echo}, {"status", reject ? "failed" : "ok"},
                    {"retcode", reject ? 100 : 0}}).toJson(QJsonDocument::Compact)));
            });
        });
        QQProtocolClient client;
        QQConfig qq;
        qq.enabled = true;
        qq.wsUrl = QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort());
        client.setConfig(qq);
        client.start();
        QTRY_VERIFY(client.isConnected());
        QQChannelAdapter adapter;
        adapter.setClient(&client);
        ActionOutboxDispatcher dispatcher(&db);
        dispatcher.setSender([&](const Utterance &u) {
            return adapter.sendOutboxUtterance(u, [&, actionKey = u.deliveryEcho] {
                return db.validateOutboxActionBeforeSend(actionKey);
            });
        });
        connect(&client, &QQProtocolClient::sendResult, &dispatcher,
                [&](const QString &echo, const QString &outcome) {
            if (echo == key) dispatcher.onSendResult(echo, outcome);
        });
        QSignalSpy settled(&dispatcher, &ActionOutboxDispatcher::actionSettled);
        const bool paced = change == QStringLiteral("revoke") || change == QStringLiteral("context");
        if (paced) {
            QVERIFY(client.sendAction(QStringLiteral("send_private_msg"), {}, QStringLiteral("lead"), 800));
            QTRY_VERIFY(frames.contains(QStringLiteral("lead")));
        }
        dispatcher.pollOnce();
        if (change == QStringLiteral("none")) {
            QTRY_COMPARE(settled.size(), 1);
            QCOMPARE(frames.count(key), 1);
            QCOMPARE(settled.first().at(1).toString(), QStringLiteral("protocol_accepted"));
            return;
        }
        if (change == QStringLiteral("context")) {
            RequestMeta owner;
            owner.subject = QStringLiteral("user");
            owner.eventId = owner.runId = QStringLiteral("interrupt-pacing");
            QVERIFY(db.recordInput(QStringLiteral("New owner input"), owner, true) > 0);
        } else {
            if (!paced) QTRY_COMPARE(settled.size(), 1);
            auto cognition = identity.value("cognition").toObject();
            auto openWake = cognition.value("open_wake").toObject();
            openWake.insert("allow_contact", false);
            cognition.insert("open_wake", openWake);
            identity.insert("cognition", cognition);
            QVERIFY(writeIdentity());
        }
        if (paced) {
            QVERIFY(client.sendAction(QStringLiteral("send_private_msg"), {}, QStringLiteral("tail")));
            QTRY_VERIFY(frames.contains(QStringLiteral("tail")));
            QCOMPARE(frames.count(key), 0);
        } else {
            dispatcher.pollOnce();
            QTest::qWait(4200);
            QCOMPARE(frames.count(key), 1);
        }
    }

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
        // 安静窗口按“当前时刻 ±2 分钟”动态构造，避免 23:59 这类边界让测试依赖运行时刻。
        const QTime quietNow = QTime::currentTime();
        QVERIFY(identity.enableOpenWakeContact(true, -1,
                                               quietNow.addSecs(-120).toString(QStringLiteral("HH:mm")),
                                               quietNow.addSecs(120).toString(QStringLiteral("HH:mm"))));
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

    // ===== R12-2c-2：提案提交事务边界 / Action Context Fence / 发送前最终复验 =====

    // A07：核验通过后权威 payload 被改写 → 提交按 verified 内容指纹拒绝，不写动作/Outbox。
    void verifiedContentTamperIsRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a07-tamper.db"));
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeId = seedOpenWakeRow(path, QStringLiteral("a07-wake"), runId);
        QVERIFY(wakeId > 0);
        const QString key = persistVerifiedProposal(
            db, wakeContactProposal(wakeId, 1, runId, QStringLiteral("在忙吗？")));
        QVERIFY(!key.isEmpty());
        {
            // 核验时是"在忙吗？"，落库后改成另一句话：内容指纹必须对不上。
            RawDb raw(path, QStringLiteral("a07-tamper-raw"));
            QVERIFY(raw.open());
            QVERIFY(raw.execPrepared(
                QStringLiteral("UPDATE action_proposal SET payload_json=? WHERE action_key=?"),
                {QStringLiteral(R"({"message":"晚安呀","reason":"想问候"})"), key}));
        }
        const auto rejected = committer.commitOpenWakeContact(key);
        QCOMPARE(rejected.value(QStringLiteral("error")).toString(),
                 QStringLiteral("policy_rejected"));
        QCOMPARE(rejected.value(QStringLiteral("detail")).toString(),
                 QStringLiteral("verified_content_mismatch"));
        {
            RawDb raw(path, QStringLiteral("a07-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM cognitive_action")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
        }
    }

    // A08：核验后主人私聊新消息推进 fence epoch → 旧提案 expired，不提交、不发送。
    void verifiedProposalExpiresAfterOwnerPrivateInput()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a08-fence.db"));
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeId = seedOpenWakeRow(path, QStringLiteral("a08-wake"), runId);
        QVERIFY(wakeId > 0);
        const QString key = persistVerifiedProposal(
            db, wakeContactProposal(wakeId, 1, runId, QStringLiteral("在忙吗？")));
        QVERIFY(!key.isEmpty());
        const qint64 epochBefore = db.actionContextEpoch();
        // 相关的主人私聊输入（pet/subject=user/reply）→ bump。
        QVERIFY(db.recordInput(QStringLiteral("我在忙，晚点说"), actionMeta(QStringLiteral("a08-owner")),
                               false) > 0);
        QCOMPARE(db.actionContextEpoch(), epochBefore + 1);
        const auto committed = committer.commitOpenWakeContact(key);
        QCOMPARE(committed.value(QStringLiteral("error")).toString(),
                 QStringLiteral("stale_origin"));
        {
            RawDb raw(path, QStringLiteral("a08-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("expired"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT cancel_reason FROM action_proposal "
                                               "WHERE action_key='") + key
                                    + QStringLiteral("'")).toString(),
                     QStringLiteral("context_epoch_changed"));
        }
    }

    // A09：无关群聊消息不推进 fence epoch，也不取消主人的合法行动。
    void unrelatedGroupMessageDoesNotCancelOwnerAction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a09-group.db"));
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const qlonglong wakeId = seedOpenWakeRow(path, QStringLiteral("a09-wake"), runId);
        QVERIFY(wakeId > 0);
        const QString key = persistVerifiedProposal(
            db, wakeContactProposal(wakeId, 1, runId, QStringLiteral("早安")));
        QVERIFY(!key.isEmpty());
        const qint64 epochBefore = db.actionContextEpoch();
        RequestMeta group = actionMeta(QStringLiteral("a09-group-msg"));
        group.channel = Channel::kQQGroup;
        group.sessionId = QStringLiteral("group:123");
        group.speaker = QStringLiteral("群友");
        QVERIFY(db.recordInput(QStringLiteral("群里随便聊聊"), group, false) > 0);
        QCOMPARE(db.actionContextEpoch(), epochBefore); // 群聊不 bump
        const auto committed = committer.commitOpenWakeContact(key);
        QVERIFY2(!committed.contains(QStringLiteral("error")),
                 qPrintable(committed.value(QStringLiteral("error")).toString()));
        {
            RawDb raw(path, QStringLiteral("a09-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("committed"));
        }
    }

    // A10：两个独立连接先后提交同一 action_key → 唯一动作/Outbox（幂等复用或 CAS 拒绝）。
    void duplicateCommitAcrossConnectionsCreatesOneOutbox()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a10-duplicate.db"));
        QString key;
        {
            MemoryManager setup(path);
            const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const qlonglong wakeId = seedOpenWakeRow(path, QStringLiteral("a10-wake"), runId);
            QVERIFY(wakeId > 0);
            key = persistVerifiedProposal(
                setup, wakeContactProposal(wakeId, 1, runId, QStringLiteral("在忙吗？")));
            QVERIFY(!key.isEmpty());
        }
        {
            MemoryManager first(path, nullptr, QStringLiteral("a10-conn-1"));
            MemoryManager second(path, nullptr, QStringLiteral("a10-conn-2"));
            const auto r1 = ActionCommitter(&first).commitOpenWakeContact(key);
            QVERIFY2(!r1.contains(QStringLiteral("error")),
                     qPrintable(r1.value(QStringLiteral("error")).toString()));
            const auto r2 = ActionCommitter(&second).commitOpenWakeContact(key);
            // 第二个独立写者要么幂等复用既有动作，要么看到提案已 committed 而拒绝；
            // 两种情况都不允许出现第二条动作/Outbox。
            if (!r2.contains(QStringLiteral("error")))
                QCOMPARE(r2.value(QStringLiteral("idempotent")).toBool(), true);
        }
        {
            RawDb raw(path, QStringLiteral("a10-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM cognitive_action")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal "
                                               "WHERE state='committed'")).toInt(), 1);
        }
    }

    // A11：两个独立连接各自提交不同提案，共享 contact 每日额度仍然不超额。
    void separateConnectionsHonorSharedContactQuota()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true, 1));
        const QString path = dir.filePath(QStringLiteral("a11-quota.db"));
        QString keyA, keyB;
        {
            MemoryManager setup(path);
            const QString runA = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString runB = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const qlonglong wakeA = seedOpenWakeRow(path, QStringLiteral("a11-wake-a"), runA);
            const qlonglong wakeB = seedOpenWakeRow(path, QStringLiteral("a11-wake-b"), runB);
            QVERIFY(wakeA > 0 && wakeB > 0);
            keyA = persistVerifiedProposal(
                setup, wakeContactProposal(wakeA, 1, runA, QStringLiteral("早安")));
            keyB = persistVerifiedProposal(
                setup, wakeContactProposal(wakeB, 1, runB, QStringLiteral("午安")));
            QVERIFY(!keyA.isEmpty() && !keyB.isEmpty());
        }
        {
            MemoryManager first(path, nullptr, QStringLiteral("a11-conn-1"));
            MemoryManager second(path, nullptr, QStringLiteral("a11-conn-2"));
            const auto r1 = ActionCommitter(&first).commitOpenWakeContact(keyA);
            QVERIFY2(!r1.contains(QStringLiteral("error")),
                     qPrintable(r1.value(QStringLiteral("error")).toString()));
            const auto r2 = ActionCommitter(&second).commitOpenWakeContact(keyB);
            QCOMPARE(r2.value(QStringLiteral("error")).toString(), QStringLiteral("limit"));
        }
        {
            RawDb raw(path, QStringLiteral("a11-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM cognitive_action")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger "
                                               "WHERE kind='contact' AND state<>'withheld'")).toInt(),
                     1);
            // 被额度挡住的提案保留 verified，等待下一次 drain（不丢、不发送）。
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + keyB + QStringLiteral("'")).toString(),
                     QStringLiteral("verified"));
        }
    }

    // A12：已入 Outbox 后关闭主动联系 → sender 前拦截，零发送、终态 withheld、不重领。
    void disabledProactiveBlocksQueuedContactBeforeSend()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a12-disabled.db"));
        MemoryManager db(path);
        const QString key = seedCommittedWakeContact(db, path, QStringLiteral("a12-wake"),
                                                     QStringLiteral("在忙吗？"));
        QVERIFY(!key.isEmpty());
        // 已入 Outbox 之后关闭主动联系：send 前必须拦截。
        QVERIFY(identity.disableProactive());
        ActionOutboxDispatcher dispatcher(&db);
        int sends = 0;
        dispatcher.setSender([&sends](const Utterance &) {
            ++sends;
            return true;
        });
        dispatcher.pollOnce();
        QCOMPARE(sends, 0);
        {
            RawDb raw(path, QStringLiteral("a12-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("failed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger "
                                               "WHERE kind='contact' AND ref='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT cancel_reason FROM action_proposal "
                                               "WHERE action_key='") + key
                                    + QStringLiteral("'")).toString(),
                     QStringLiteral("proactive_disabled"));
        }
        // 终态行不可重领，也不会再次调用 sender。
        dispatcher.pollOnce();
        QCOMPARE(sends, 0);
    }

    // A13：已入 Outbox 后进入安静时段 → 保留 queued（不占发送次数、零发送）；解除后发一次。
    void quietHoursDeferQueuedContactWithoutSending()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a13-quiet.db"));
        MemoryManager db(path);
        const QString key = seedCommittedWakeContact(db, path, QStringLiteral("a13-wake"),
                                                     QStringLiteral("早安"));
        QVERIFY(!key.isEmpty());
        const QTime now = QTime::currentTime();
        QVERIFY(identity.enableOpenWakeContact(true, -1,
                                               now.addSecs(-3600).toString(QStringLiteral("HH:mm")),
                                               now.addSecs(3600).toString(QStringLiteral("HH:mm"))));
        ActionOutboxDispatcher dispatcher(&db);
        int sends = 0;
        dispatcher.setSender([&sends](const Utterance &) {
            ++sends;
            return true;
        });
        dispatcher.pollOnce();
        QCOMPARE(sends, 0);
        {
            RawDb raw(path, QStringLiteral("a13-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("queued"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("committed"));
        }
        // 安静时段解除：下一拍正常发送一次（pet=handed_off）。
        QVERIFY(identity.enableOpenWakeContact(true));
        dispatcher.pollOnce();
        QCOMPARE(sends, 1);
    }

    // A14：withheld 结算会把 outbox 置 failed（attempts 可能 <2）；这类失败行不得重领。
    void withheldFailedRowIsNeverReclaimed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableProactive());
        const QString path = dir.filePath(QStringLiteral("a14-withheld.db"));
        MemoryManager db(path);
        ActionCommitter committer(&db);
        const qint64 reminder = insertReminder(path, QStringLiteral("a14-reminder"), -10,
                                               QStringLiteral("该睡了"));
        QVERIFY(reminder > 0);
        const auto committed = committer.commitReminderDelivery(reminder);
        QVERIFY2(!committed.contains(QStringLiteral("error")),
                 qPrintable(committed.value(QStringLiteral("error")).toString()));
        const QString key = committed.value(QStringLiteral("action_key")).toString();
        QVERIFY(!db.claimOutbox(QStringLiteral("a14-first"), 60).isEmpty());
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("withheld"),
                                      QStringLiteral("pre_send_invalid")));
        {
            RawDb raw(path, QStringLiteral("a14-pre"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("failed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT attempts FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
        }
        // 不能因为 attempts<2 把 failed 行重新领出来（key 的 action 已 withheld）。
        QVERIFY(db.claimOutbox(QStringLiteral("a14-second"), 60).isEmpty());
        QVERIFY(db.claimOutbox(QStringLiteral("a14-third"), 60).isEmpty());
    }

    // A15：verified 提案在停机期间过期；重启后 drain 只落 expired，绝不补发。
    void expiredVerifiedProposalIsNotResentAfterRestart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a15-expired.db"));
        QString key;
        {
            MemoryManager db(path);
            const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const qlonglong wakeId = seedOpenWakeRow(path, QStringLiteral("a15-wake"), runId);
            QVERIFY(wakeId > 0);
            QJsonObject proposal = wakeContactProposal(wakeId, 1, runId,
                                                       QStringLiteral("在忙吗？"));
            proposal[QStringLiteral("expires_at")] = QDateTime::currentSecsSinceEpoch() + 60;
            key = persistVerifiedProposal(db, proposal);
            QVERIFY(!key.isEmpty());
            RawDb raw(path, QStringLiteral("a15-expire"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE action_proposal SET expires_at=unixepoch()-5 WHERE action_key='") + key
                + QStringLiteral("'")));
        }
        {
            // 重启：启动恢复（drain）只能把过期 verified 落 expired，不补建 Outbox。
            MemoryManager reopened(path);
            const auto drained = reopened.drainVerifiedActionProposals(8);
            QCOMPARE(drained.value(QStringLiteral("committed")).toInt(), 0);
            QCOMPARE(drained.value(QStringLiteral("scanned")).toInt(), 0);
        }
        {
            RawDb raw(path, QStringLiteral("a15-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_proposal WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("expired"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
        }
    }

    // A16：动作已领取/在渠道队列排队后语境失效 → 最后一个可取消边界（queue sender 复验）
    //       必须拦截；随后取消落终态，不会重领。
    void channelQueueBoundaryBlocksInvalidatedAction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a16-boundary.db"));
        MemoryManager db(path);
        const QString key = seedCommittedWakeContact(db, path, QStringLiteral("a16-wake"),
                                                     QStringLiteral("在忙吗？"));
        QVERIFY(!key.isEmpty());
        // 模拟 dispatcher 已领取、动作正在渠道队列里等待真正发送。
        const auto job = db.claimOutbox(QStringLiteral("a16-dispatcher"), 300);
        QCOMPARE(job.value(QStringLiteral("action_key")).toString(), key);
        // 排队期间主人又说话了：fence epoch 推进，旧动作失效。
        QVERIFY(db.recordInput(QStringLiteral("先别发了"),
                               actionMeta(QStringLiteral("a16-owner")), false) > 0);
        // mock queue sender 在真正入队前的最后复验：必须拦截。
        bool sent = false;
        const auto guard = db.validateOutboxActionBeforeSend(key);
        QVERIFY(!guard.value(QStringLiteral("ok")).toBool());
        QVERIFY(guard.value(QStringLiteral("permanent")).toBool());
        QCOMPARE(guard.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("context_epoch_changed"));
        if (guard.value(QStringLiteral("ok")).toBool()) sent = true;
        QVERIFY(!sent);
        QVERIFY(db.cancelOutboxAction(key, guard.value(QStringLiteral("reason")).toString()));
        {
            RawDb raw(path, QStringLiteral("a16-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("failed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger "
                                               "WHERE kind='contact' AND ref='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT cancel_reason FROM action_proposal "
                                               "WHERE action_key='") + key
                                    + QStringLiteral("'")).toString(),
                     QStringLiteral("context_epoch_changed"));
        }
        QVERIFY(db.claimOutbox(QStringLiteral("a16-after"), 60).isEmpty());
    }

    // A16 补充：dispatcher 领取时就复验（不需要 sender 参与），语境失效零发送。
    void dispatcherRevalidatesBeforeSender()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a16b-dispatcher.db"));
        MemoryManager db(path);
        const QString key = seedCommittedWakeContact(db, path, QStringLiteral("a16b-wake"),
                                                     QStringLiteral("早安"));
        QVERIFY(!key.isEmpty());
        QVERIFY(db.recordInput(QStringLiteral("先别发"),
                               actionMeta(QStringLiteral("a16b-owner")), false) > 0);
        ActionOutboxDispatcher dispatcher(&db);
        int sends = 0;
        dispatcher.setSender([&sends](const Utterance &) {
            ++sends;
            return true;
        });
        dispatcher.pollOnce();
        QCOMPARE(sends, 0);
        {
            RawDb raw(path, QStringLiteral("a16b-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("failed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("withheld"));
        }
    }

    // ===== R12-2c-3：回执与认知历史闭环 =====

    // A17：同一 action_key 重复成功回执/对账只写一条主动历史；history_projected 置 1。
    void receiptDuplicateProjectsHistoryOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a17-receipt.db"));
        MemoryManager db(path);
        const QString message = QStringLiteral("晚安，早点睡。");
        const QString key = seedCommittedWakeContact(db, path, QStringLiteral("a17-wake"), message);
        QVERIFY(!key.isEmpty());
        const auto job = db.claimOutbox(QStringLiteral("a17-sender"), 300);
        QCOMPARE(job.value(QStringLiteral("action_key")).toString(), key);
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("handed_off")));
        // 第一次回执写历史；随后重复回执 + 两次启动对账都不得重复写。
        QVERIFY(db.recordActionReceipt(key, QStringLiteral("handed_off"), QStringLiteral("ok")));
        QVERIFY(db.recordActionReceipt(key, QStringLiteral("handed_off"), QStringLiteral("ok")));
        QVERIFY(db.reconcileActionReceipts() >= 0);
        {
            const QString historyRef = key.section(QLatin1Char(':'), 0, 2);
            RawDb raw(path, QStringLiteral("a17-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM chat_history WHERE turn_kind='proactive' "
                         "AND json_extract(raw_reply,'$.delivery.kind')='contact' "
                         "AND json_extract(raw_reply,'$.delivery.ref')='")
                         + historyRef + QStringLiteral("'")).toInt(),
                     1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT history_projected FROM action_proposal "
                                               "WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger "
                                               "WHERE kind='contact' AND ref='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("handed_off"));
        }
    }

    // A18：unknown 冻结后迟到的成功回执不得写成功历史、也不得重发。
    void lateSuccessAfterUnknownNeverWritesHistory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a18-unknown.db"));
        MemoryManager db(path);
        const QString key = seedCommittedWakeContact(db, path, QStringLiteral("a18-wake"),
                                                     QStringLiteral("在吗？"));
        QVERIFY(!key.isEmpty());
        const auto job = db.claimOutbox(QStringLiteral("a18-sender"), 300);
        QCOMPARE(job.value(QStringLiteral("action_key")).toString(), key);
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("unknown"),
                                      QStringLiteral("timeout")));
        QVERIFY(db.recordActionReceipt(key, QStringLiteral("unknown")));
        {
            RawDb raw(path, QStringLiteral("a18-before-late"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT history_projected FROM action_proposal "
                                               "WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     0);
        }
        // 迟到的明确接受：状态机冻结（settle 返回 true 但不改状态）。
        QVERIFY(db.settleOutboxAction(key, QStringLiteral("protocol_accepted")));
        QVERIFY(db.recordActionReceipt(key, QStringLiteral("protocol_accepted")));
        {
            RawDb raw(path, QStringLiteral("a18-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT history_projected FROM action_proposal "
                                               "WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM cognitive_action "
                                               "WHERE action_key='")
                                    + key + QStringLiteral("'")).toString(),
                     QStringLiteral("unknown"));
        }
        // 不重发：unknown 行不可再领取。
        QVERIFY(db.claimOutbox(QStringLiteral("a18-after"), 60).isEmpty());
        // 不存在的动作：只审计，绝不写 ledger（存在性校验）。
        {
            RawDb raw(path, QStringLiteral("a18-missing-before"));
            QVERIFY(raw.open());
            const int ledgerBefore = raw.scalar(QStringLiteral(
                "SELECT count(*) FROM delivery_ledger WHERE kind='contact'")).toInt();
            QVERIFY(raw.open());
            QVERIFY(!db.recordActionReceipt(QStringLiteral("wake:999999:1:contact"),
                                            QStringLiteral("handed_off")));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM delivery_ledger WHERE kind='contact'")).toInt(),
                     ledgerBefore);
            // 未提交（无提案）的新来源动作：不能借回执把状态写进 ledger/历史。
            const QString uncommittedKey = QStringLiteral("wake:888888:1:contact");
            QVERIFY(raw.execPrepared(
                QStringLiteral("INSERT INTO cognitive_action(action_key,kind,subject,channel,"
                               "session_id,payload_json,state,last_outcome) "
                               "VALUES(?,'OpenWakeContact','user','pet','default',"
                               "'{\"message\":\"未提交\"}','handed_off','handed_off')"),
                {uncommittedKey}));
            QVERIFY(raw.execPrepared(
                QStringLiteral("INSERT INTO action_outbox(action_id,action_key,channel,"
                               "session_id,payload_json,state) "
                               "SELECT id,?,'pet','default','{}','dispatched' FROM cognitive_action "
                               "WHERE action_key=?"),
                {uncommittedKey, uncommittedKey}));
            QVERIFY(!db.recordActionReceipt(uncommittedKey, QStringLiteral("handed_off")));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM delivery_ledger WHERE kind='contact'")).toInt(),
                     ledgerBefore);
        }
    }

    // A19：回执已落库但历史投影前崩溃 → 重启对账补齐且唯一；已投影不回退、不重复。
    void receiptHistoryCrashIsReconciledOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a19-crash.db"));
        QString key;
        {
            MemoryManager db(path);
            key = seedCommittedWakeContact(db, path, QStringLiteral("a19-wake"),
                                           QStringLiteral("我出门了。"));
            QVERIFY(!key.isEmpty());
            const auto job = db.claimOutbox(QStringLiteral("a19-sender"), 300);
            QCOMPARE(job.value(QStringLiteral("action_key")).toString(), key);
            // settle 落库后进程崩溃：此时还没有调用 recordActionReceipt。
            QVERIFY(db.settleOutboxAction(key, QStringLiteral("handed_off")));
            RawDb raw(path, QStringLiteral("a19-crash-check"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT history_projected FROM action_proposal "
                                               "WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     0);
        }
        {
            // 重启：启动对账把未投影的成功回执补齐，且只补一次。
            MemoryManager reopened(path);
            QCOMPARE(reopened.reconcileActionReceipts(), 1);
            QCOMPARE(reopened.reconcileActionReceipts(), 0);
            RawDb raw(path, QStringLiteral("a19-reopened"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT history_projected FROM action_proposal "
                                               "WHERE action_key='")
                                    + key + QStringLiteral("'")).toInt(),
                     1);
        }
        {
            // 再次重启：已投影不回退、不重复。
            MemoryManager again(path);
            QCOMPARE(again.reconcileActionReceipts(), 0);
            RawDb raw(path, QStringLiteral("a19-again"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     1);
            QVERIFY(raw.exec(QStringLiteral("DELETE FROM memory_chunks WHERE source_type='chat'")));
            QCOMPARE(again.reconcileActionReceipts(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM memory_chunks WHERE "
                                               "source_type='chat' AND chunk_kind='turn_assistant'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 1);
        }
    }

    // A20：成功回执数量超过固定 64 条时，后续行不得饿死；多批扫描全部投影且幂等。
    void reconcileScansBeyondFirstBatch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a20-bulk.db"));
        MemoryManager db(path);
        const int total = 70;
        {
            RawDb raw(path, QStringLiteral("a20-seed"));
            QVERIFY(raw.open());
            for (int index = 0; index < total; ++index) {
                const qlonglong wakeId = 90000 + index;
                const QString key = QStringLiteral("wake:%1:1:contact").arg(wakeId);
                const QString payload = QStringLiteral(
                    R"({"message":"m%1","sentences":[{"zh_text":"m%1"}],)"
                    R"("channel":"pet","session":"default"})").arg(index);
                QVERIFY(raw.execPrepared(
                    QStringLiteral("INSERT INTO cognitive_action(action_key,kind,subject,channel,"
                                   "session_id,payload_json,state,last_outcome,attempts) "
                                   "VALUES(?,'OpenWakeContact','user','pet','default',?,"
                                   "'handed_off','handed_off',1)"),
                    {key, payload}));
                const qlonglong actionId =
                    raw.scalar(QStringLiteral("SELECT last_insert_rowid()")).toLongLong();
                QVERIFY(actionId > 0);
                QVERIFY(raw.execPrepared(
                    QStringLiteral("INSERT INTO action_outbox(action_id,action_key,channel,"
                                   "session_id,payload_json,state,attempts) "
                                   "VALUES(?,?,'pet','default',?,'dispatched',1)"),
                    {actionId, key, payload}));
                QVERIFY(raw.execPrepared(
                    QStringLiteral("INSERT INTO action_proposal(action_key,kind,origin_kind,"
                                   "origin_ref,origin_version,scope,target_channel,target_session,"
                                   "payload_json,claims_json,state,committed_action_key,"
                                   "history_projected,action_context_epoch,created_at,updated_at) "
                                   "VALUES(?,'contact','wake',?,1,'owner','pet','default',"
                                   "'{}','[]','committed',?,0,0,unixepoch(),unixepoch())"),
                    {key, QStringLiteral("wake/%1").arg(wakeId), key}));
            }
        }
        // 一次对账有界推进多批；70 条都应投影，不能只处理最早 64 条。
        const int reconciled = db.reconcileActionReceipts();
        QCOMPARE(reconciled, total);
        {
            RawDb raw(path, QStringLiteral("a20-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     total);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal "
                                               "WHERE history_projected=1")).toInt(),
                     total);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal "
                                               "WHERE history_projected=0")).toInt(),
                     0);
        }
        QCOMPARE(db.reconcileActionReceipts(), 0);
    }

    // A21：旧 wake 的迟到回执不得覆盖最新 wake 的 last_outcome；最新 wake 回执才写。
    void oldWakeReceiptDoesNotOverwriteLatestState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a21-wake.db"));
        MemoryManager db(path);
        const QString firstKey = seedCommittedWakeContact(db, path, QStringLiteral("a21-wake-a"),
                                                         QStringLiteral("第一次"));
        QVERIFY(!firstKey.isEmpty());
        const QString secondKey = seedCommittedWakeContact(db, path, QStringLiteral("a21-wake-b"),
                                                          QStringLiteral("第二次"));
        QVERIFY(!secondKey.isEmpty());
        const qlonglong firstWakeId = firstKey.section(QLatin1Char(':'), 1, 1).toLongLong();
        const qlonglong secondWakeId = secondKey.section(QLatin1Char(':'), 1, 1).toLongLong();
        QVERIFY(firstWakeId > 0 && secondWakeId > 0 && firstWakeId != secondWakeId);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        {
            RawDb raw(path, QStringLiteral("a21-clock"));
            QVERIFY(raw.open());
            // 最新 wake 是 second；sentinel 用于证明旧回执没有覆盖。
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE open_wake_state SET last_wake_id=%1,last_outcome='wake_b_state' "
                "WHERE scope_key='owner'").arg(secondWakeId)));
        }
        // 先处理旧 wake 的迟到成功回执。
        QVERIFY(!db.claimOutbox(QStringLiteral("a21-sender"), 300).isEmpty());
        QVERIFY(db.settleOutboxAction(firstKey, QStringLiteral("handed_off")));
        QVERIFY(db.recordActionReceipt(firstKey, QStringLiteral("handed_off")));
        {
            RawDb raw(path, QStringLiteral("a21-after-old"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM open_wake_state "
                                               "WHERE scope_key='owner'")).toString(),
                     QStringLiteral("wake_b_state"));
            // 历史仍应记录旧 wake 确实说过的话（只保护调度状态，不删除事实）。
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive' AND "
                                               "json_extract(raw_reply,'$.delivery.ref')='wake:")
                                    + QString::number(firstWakeId)
                                    + QStringLiteral(":1'")).toInt(),
                     1);
        }
        // 最新 wake 的回执才允许写 last_outcome。
        QVERIFY(!db.claimOutbox(QStringLiteral("a21-sender-2"), 300).isEmpty());
        QVERIFY(db.settleOutboxAction(secondKey, QStringLiteral("handed_off")));
        QVERIFY(db.recordActionReceipt(secondKey, QStringLiteral("handed_off")));
        {
            RawDb raw(path, QStringLiteral("a21-after-new"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT last_outcome FROM open_wake_state "
                                               "WHERE scope_key='owner'")).toString(),
                     QStringLiteral("contact_handed_off"));
        }
    }

    // ===== R12-2c-3：internal activity share 端到端恢复 =====

    // A22：崩溃窗口恢复——已完成+saved 意图、无提案时被有界领取一次；已有提案/成功回执后
    // 不重复自动分享。完整闭环：成果→意图→提案→verified→commit→Outbox→回执→历史→下一轮可见。
    void shareRecoveryAndFullLoop()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a22-share.db"));
        QString draft = QStringLiteral("檐下风铃响了一声。");
        QString hash;
        qlonglong activityId = 0;
        QString actionKey;
        {
            MemoryManager db(path);
            activityId = seedInternalCompletedActivity(path, QStringLiteral("a22-seed"), draft,
                                                       &hash);
            QVERIFY(activityId > 0);
            // 模拟 ActivityReview 已原子保存 share_intent，但在发出核验信号前崩溃。
            const auto claimed = db.claimPendingActivityShareIntents(4, true);
            QCOMPARE(claimed.size(), 1);
            const QJsonObject row = claimed.first().toObject();
            QCOMPARE(row.value(QStringLiteral("ref")).toString(),
                     QStringLiteral("activity/%1").arg(activityId));
            QCOMPARE(row.value(QStringLiteral("artifact_version")).toInt(), 2);
            QCOMPARE(row.value(QStringLiteral("draft")).toString(), draft);
            // 非 force + 节流窗口内不重复领取；不会把所有历史成果自动加入队列。
            QCOMPARE(db.claimPendingActivityShareIntents(4, false).size(), 0);

            // 正常 verifier→verified→commit 链（服务端组装提案；这里模拟核验通过）。
            const QString key = persistVerifiedProposal(
                db, internalShareProposal(activityId, 2, draft));
            QVERIFY(!key.isEmpty());
            actionKey = key;
            ActionCommitter committer(&db);
            const auto committed = committer.commitInternalActivityShare(key);
            QVERIFY2(!committed.contains(QStringLiteral("error")),
                     qPrintable(committed.value(QStringLiteral("error")).toString()));
            // 已有同成果版本提案：恢复扫描不再重复自动分享。
            QCOMPARE(db.claimPendingActivityShareIntents(4, true).size(), 0);
            {
                RawDb raw(path, QStringLiteral("a22-committed"));
                QVERIFY(raw.open());
                QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(),
                         1);
                QCOMPARE(raw.scalar(QStringLiteral("SELECT status FROM internal_activity "
                                                   "WHERE id=%1").arg(activityId)).toString(),
                         QStringLiteral("completed"));
            }

            // Outbox 唯一发送 + 协议接受回执 + 历史投影；回执绑定原成果 ref/version/hash。
            const auto job = db.claimOutbox(QStringLiteral("a22-sender"), 300);
            QCOMPARE(job.value(QStringLiteral("action_key")).toString(), key);
            QVERIFY(db.settleOutboxAction(key, QStringLiteral("protocol_accepted")));
            QVERIFY(db.recordActionReceipt(key, QStringLiteral("protocol_accepted")));
            {
                RawDb raw(path, QStringLiteral("a22-receipt"));
                QVERIFY(raw.open());
                QCOMPARE(raw.scalar(QStringLiteral(
                             "SELECT json_extract(result_json,'$.share_outcome') "
                             "FROM internal_activity WHERE id=%1").arg(activityId)).toString(),
                         QStringLiteral("protocol_accepted"));
                QCOMPARE(raw.scalar(QStringLiteral(
                             "SELECT json_extract(result_json,'$.share_artifact_ref') "
                             "FROM internal_activity WHERE id=%1").arg(activityId)).toString(),
                         QStringLiteral("activity/%1").arg(activityId));
                QCOMPARE(raw.scalar(QStringLiteral(
                             "SELECT json_extract(result_json,'$.share_artifact_version') "
                             "FROM internal_activity WHERE id=%1").arg(activityId)).toInt(),
                         2);
                QCOMPARE(raw.scalar(QStringLiteral(
                             "SELECT json_extract(result_json,'$.share_artifact_hash') "
                             "FROM internal_activity WHERE id=%1").arg(activityId)).toString(),
                         hash);
                QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                                   "WHERE turn_kind='proactive'")).toInt(),
                         1);
                // 成功分享后不再因无回应/重复扫描自动重发。
                QCOMPARE(db.claimPendingActivityShareIntents(4, true).size(), 0);
                QVERIFY(db.claimOutbox(QStringLiteral("a22-again"), 60).isEmpty());
            }
            // 下一轮快照可见性：活动仍是 completed，成果目录与模型投影仍能读到。
            const auto recent = db.recentActivityResultsForModel(
                8, Channel::kPet, Channel::kDefaultSession, QStringLiteral("user"), true);
            bool found = false;
            for (const auto &value : recent) {
                if (value.toObject().value(QStringLiteral("ref")).toString()
                    == QStringLiteral("activity/%1").arg(activityId))
                    found = true;
            }
            QVERIFY(found);
            QVERIFY(!db.activityForModel(QStringLiteral("activity/%1").arg(activityId),
                                         Channel::kPet, Channel::kDefaultSession,
                                         QStringLiteral("user"), true)
                         .isEmpty());
        }
        // 重启后再次对账（已投影/已成功）不重复写历史、不重发。
        {
            MemoryManager reopened(path);
            QCOMPARE(reopened.reconcileActionReceipts(), 0);
            QCOMPARE(reopened.claimPendingActivityShareIntents(4, true).size(), 0);
            RawDb raw(path, QStringLiteral("a22-reopen"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history "
                                               "WHERE turn_kind='proactive'")).toInt(),
                     1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
        }
        // 另一个活动的意图在重启窗口里被恢复（force=true 立即重试）。
        {
            QString draft2 = QStringLiteral("雨停之前写完这一页。");
            const qlonglong secondId = seedInternalCompletedActivity(
                path, QStringLiteral("a22-seed-2"), draft2);
            QVERIFY(secondId > 0);
            MemoryManager reopened(path);
            QCOMPARE(reopened.claimPendingActivityShareIntents(4, true).size(), 1);
        }
        // The verifier might have received the claimed request just before the crash.
        {
            MemoryManager reopened(path);
            QCOMPARE(reopened.claimPendingActivityShareIntents(4, true).size(), 0);
            RawDb raw(path, QStringLiteral("a22-unknown-verification"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT json_extract(result_json,'$.share_intent.recovery_reason') "
                         "FROM internal_activity ORDER BY id DESC LIMIT 1")).toString(),
                     QStringLiteral("verification_outcome_unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 1);
        }
        Q_UNUSED(actionKey);
    }

    // A23：成果修改/撤回后旧意图失效（withheld 审计、不发送旧成果、不改完成状态）。
    void staleShareIntentIsWithheld()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableOpenWakeContact(true));
        const QString path = dir.filePath(QStringLiteral("a23-stale.db"));
        const QString draft = QStringLiteral("旧成果的第一版。");
        MemoryManager db(path);
        const qlonglong activityId = seedInternalCompletedActivity(
            path, QStringLiteral("a23-seed"), draft);
        QVERIFY(activityId > 0);
        {
            // 成果版本前进（3），但旧 share_intent 仍绑定版本 2：旧意图失效。
            RawDb raw(path, QStringLiteral("a23-revise"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE internal_activity SET result_json=json_set(result_json,"
                "'$.artifact_version',3,'$.draft','新成果的第二版。'),"
                "version=3 WHERE id=%1").arg(activityId)));
        }
        QCOMPARE(db.claimPendingActivityShareIntents(4, true).size(), 0);
        {
            RawDb raw(path, QStringLiteral("a23-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT json_extract(result_json,'$.share_intent.recovery_state') "
                         "FROM internal_activity WHERE id=%1").arg(activityId)).toString(),
                     QStringLiteral("withheld"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT status FROM internal_activity "
                                               "WHERE id=%1").arg(activityId)).toString(),
                     QStringLiteral("completed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            // 再次扫描不会再重复审计/发送。
            QCOMPARE(db.claimPendingActivityShareIntents(4, true).size(), 0);
        }
        // 撤回的意图不自动恢复。
        const QString draft2 = QStringLiteral("撤回后的成果。");
        const qlonglong withdrawnId = seedInternalCompletedActivity(
            path, QStringLiteral("a23-withdraw"), draft2);
        QVERIFY(withdrawnId > 0);
        {
            RawDb raw(path, QStringLiteral("a23-withdraw-mark"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE internal_activity SET result_json=json_set(result_json,"
                "'$.share_intent.state','withdrawn') WHERE id=%1").arg(withdrawnId)));
        }
        QCOMPARE(db.claimPendingActivityShareIntents(4, true).size(), 0);
        {
            RawDb raw(path, QStringLiteral("a23-withdraw-post"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT status FROM internal_activity "
                                               "WHERE id=%1").arg(withdrawnId)).toString(),
                     QStringLiteral("completed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal "
                                               "WHERE origin_ref='activity/%1'")
                                    .arg(withdrawnId)).toInt(),
                     0);
        }
    }

};

QTEST_GUILESS_MAIN(TestActionOutbox)
#include "tst_actionoutbox.moc"
