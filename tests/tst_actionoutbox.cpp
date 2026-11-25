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
#include "actioncommitter.h"
#include "actionoutboxdispatcher.h"

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

private:
    QString m_path;
    QByteArray m_original;
    bool m_existed = false;
};

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
static qlonglong seedCompletedActivity(MemoryManager &mm, const QString &userText,
                                       const QString &interestJson, const QString &draft,
                                       int *sharedVersion = nullptr)
{
    if (!mm.saveQATurn(userText, interestJson, {})) return 0;
    const auto thought = mm.nextThought(true);
    const qlonglong id = thought["id"].toInteger();
    const int version = thought["version"].toInt();
    if (id <= 0) return 0;
    if (!mm.finishThoughtReview(id, version,
            QJsonObject{{"status", "completed"}, {"reflection", "写好了两句"}, {"draft", draft}}))
        return 0;
    if (sharedVersion) *sharedVersion = version + 1;
    return id;
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
            db, QStringLiteral("想写雨声"), R"({"personal_interest":"写雨声"})",
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
            db, QStringLiteral("想写风声"), R"({"personal_interest":"写风声"})",
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

        // QQ 回执：echo=action_key → settle 把 handed_off 升级成 protocol_accepted 终态。
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
};

QTEST_GUILESS_MAIN(TestActionOutbox)
#include "tst_actionoutbox.moc"
