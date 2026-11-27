#include <QtTest>
#include <QTemporaryDir>
#include <QDateTime>
#include <QCryptographicHash>
#include "benchmarksnapshot.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QScopeGuard>
#include <QSet>

#include "configmanager.h"
#include "memorymanager.h"
#include "memoryevidence.h"
#include "memoryretrieval.h"
#include "memoryembedding.h"
#include "memoryindexworker.h"
#include "utterance.h"
#include "privatereview.h"
#include "activityref.h"
#include "actionorigin.h"

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
    // R12-1b：直接造一条"存量 legacy 活动"行（chat_history.thought + thought_result）。
    // 新 saveQATurn 不再写 thought，所以仍在验证旧路径的用例必须这样显式造存量数据。
    bool insertLegacyThought(const QString &interest, const QString &channel,
                             const QString &session, const QString &userText,
                             qlonglong *id = nullptr)
    {
        const auto nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO chat_history(user_input,raw_reply,channel,session_id,speaker,"
                  "sensitive,turn_kind,subject,thought,thought_result,review_after,timestamp) "
                  "VALUES(?,'',?,?,?,0,'reply','user',?,?,0,datetime('now','localtime'))");
        q.addBindValue(userText);
        q.addBindValue(channel);
        q.addBindValue(session);
        q.addBindValue(QStringLiteral("欧尼酱"));
        q.addBindValue(interest);
        const QJsonObject initial{
            {QStringLiteral("status"), QStringLiteral("active")},
            {QStringLiteral("goal_kind"), QStringLiteral("interest")},
            {QStringLiteral("created_at"), nowIso},
            {QStringLiteral("updated_at"), nowIso},
            {QStringLiteral("artifact_ref"), QString{}}};
        q.addBindValue(QString::fromUtf8(QJsonDocument(initial).toJson(QJsonDocument::Compact)));
        if (!q.exec()) {
            qWarning() << "insert legacy thought failed:" << q.lastError().text();
            return false;
        }
        const qlonglong turnId = q.lastInsertId().toLongLong();
        if (turnId <= 0) return false;
        // source_turn 必须等于所在行 id（与旧 saveQATurn 的初始档案一致）。
        QSqlQuery source(m_db);
        source.prepare(QStringLiteral(
            "UPDATE chat_history SET thought_result=json_set(thought_result,'$.source_turn',?) "
            "WHERE id=?"));
        source.addBindValue(turnId);
        source.addBindValue(turnId);
        if (!source.exec()) return false;
        if (id) *id = turnId;
        return true;
    }
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

// R12-0 日志断言用：捕获 qInfo/qDebug 行（安装期间不转发到默认处理器）。
class MessageCapture
{
public:
    struct Line {
        QtMsgType type;
        QString message;
    };
    MessageCapture()
    {
        s_active = this;
        m_previous = qInstallMessageHandler(&MessageCapture::handle);
    }
    ~MessageCapture()
    {
        qInstallMessageHandler(m_previous);
        s_active = nullptr;
    }
    QList<Line> lines() const { return m_lines; }

private:
    static void handle(QtMsgType type, const QMessageLogContext &, const QString &message)
    {
        if (!s_active) return;
        s_active->m_lines.append({type, message});
    }
    static MessageCapture *s_active;
    QtMessageHandler m_previous = nullptr;
    QList<Line> m_lines;
};
MessageCapture *MessageCapture::s_active = nullptr;

// R12-1a：从 PrivateReview::requested 的 context 取服务端 origin，注入测试 proposal。
// 生产路径要求所有 proposal（含失败回调）都带合法 _origin；测试不得降低该校验。
static QJsonObject proposalWithOrigin(const QJsonObject &requestedContext, QJsonObject proposal)
{
    proposal.insert(QStringLiteral("_origin"),
                    requestedContext.value(QStringLiteral("origin")).toObject());
    return proposal;
}

static QJsonObject originProposal(const QJsonObject &requestedContext)
{
    return QJsonObject{{QStringLiteral("_origin"),
                        requestedContext.value(QStringLiteral("origin")).toObject()}};
}

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

// R12-0 退避测试：造一条可被领取的新来源回顾（recordInput + close 收口）。
static qlonglong seedReflectionSource(MemoryManager &db, const QString &tag)
{
    RequestMeta meta;
    meta.eventId = meta.runId = QStringLiteral("backoff-source-%1").arg(tag);
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    const auto turn = db.recordInput(QStringLiteral("今天把窗台擦干净了"), meta, true);
    if (turn <= 0 || !db.recordQuiet(turn, QStringLiteral("close"))) return 0;
    return turn;
}

// private_reflection.eligible_at 相对当前时间的秒数（用于断言退避/暂停窗口）。
static qlonglong deferredSeconds(const QString &dbPath, qlonglong turn, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return -1;
    return raw.scalar(QStringLiteral("SELECT eligible_at-unixepoch() FROM private_reflection "
                                     "WHERE turn_id=%1").arg(turn)).toLongLong();
}

// 模拟"退避时间已过"：把 eligible_at 拨回过去并回拨回顾冷却，使下一轮 wake 能重新领取。
static bool rewindReflectionAttempt(const QString &dbPath, qlonglong turn, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return false;
    if (!raw.exec(QStringLiteral("UPDATE private_reflection SET eligible_at=unixepoch()-1 "
                                 "WHERE turn_id=%1").arg(turn)))
        return false;
    return raw.exec(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120"));
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
// reported 默认给 +1 小时：测试里关注来源的 reported 是"当前时刻"，用明显的未来偏移
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

// R12-1b：造一条存量 legacy 活动（旧 saveQATurn 会写 chat_history.thought）。
// 定义在文件后部；这里先声明，供更早的测试辅助函数使用。
static qlonglong seedLegacyThought(MemoryManager &mm, const QString &interest,
                                   const QString &channel = Channel::kPet,
                                   const QString &session = Channel::kDefaultSession,
                                   const QString &userText = QStringLiteral("legacy write"));

// 造一个 completed、有产物且尚未分享的活动的 legacy 存档；返回活动 id（activity_result 依据）。
// R12-1b：新 saveQATurn 不再写 legacy thought，这里显式造存量行来验证 legacy 适配路径。
static qlonglong seedCompletedActivity(MemoryManager &mm, const QString &userText,
                                       const QString &interestJson, const QString &draft)
{
    Q_UNUSED(interestJson)
    const qlonglong seeded = seedLegacyThought(mm, QStringLiteral("legacy activity interest"),
                                               Channel::kPet, Channel::kDefaultSession, userText);
    if (seeded <= 0) return 0;
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

// ===== R12-1a 测试辅助 =====
// 把 SQLite 里与回顾调度相关的时间整体往回拨 seconds 秒，模拟"退避/暂停/冷却已过"，
// 让用例不必真的等待。只用于测试夹具。
static bool advanceReviewClock(const QString &dbPath, const QString &tag, int seconds)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return false;
    const QString secs = QString::number(seconds);
    const QStringList statements{
        QStringLiteral("UPDATE thought_review_log SET started_at=started_at-") + secs,
        QStringLiteral("UPDATE private_reflection SET eligible_at=eligible_at-") + secs
            + QStringLiteral(",next_check_at=CASE WHEN next_check_at>0 THEN next_check_at-")
            + secs + QStringLiteral(" ELSE 0 END,updated_at=updated_at-") + secs,
        QStringLiteral("UPDATE review_failure SET "
                       "next_retry_at=CASE WHEN next_retry_at IS NULL THEN NULL ELSE next_retry_at-")
            + secs
            + QStringLiteral(" END,suspended_until=CASE WHEN suspended_until IS NULL THEN NULL "
                             "ELSE suspended_until-")
            + secs
            + QStringLiteral(" END,last_attempt_at=CASE WHEN last_attempt_at IS NULL THEN NULL "
                             "ELSE last_attempt_at-")
            + secs + QStringLiteral(" END,updated_at=updated_at-") + secs,
        QStringLiteral("UPDATE chat_history SET review_after=CASE WHEN coalesce(review_after,0)>0 "
                       "THEN review_after-")
            + secs + QStringLiteral(" ELSE review_after END"),
        // R12-1b：内部活动的时间同样回拨（completed/abandoned 的 0 保持不变）。
        QStringLiteral("UPDATE internal_activity SET review_after=CASE WHEN review_after>0 "
                       "THEN review_after-")
            + secs + QStringLiteral(" ELSE review_after END,updated_at=updated_at-") + secs,
        QStringLiteral("UPDATE open_wake_state SET next_due_at=next_due_at-") + secs
            + QStringLiteral(",suspended_until=CASE WHEN suspended_until IS NULL THEN NULL "
                             "ELSE suspended_until-")
            + secs + QStringLiteral(" END,updated_at=updated_at-") + secs,
        QStringLiteral("UPDATE open_wake SET next_due_at=CASE WHEN next_due_at IS NULL THEN NULL "
                       "ELSE next_due_at-")
            + secs + QStringLiteral(" END,lease_until=CASE WHEN lease_until IS NULL THEN NULL "
                                    "ELSE lease_until-")
            + secs + QStringLiteral(" END")};
    for (const auto &statement : statements)
        if (!raw.exec(statement)) return false;
    return true;
}

// 把 OpenWake 的调度时间拨到"现在到期"。
static bool makeOpenWakeDue(const QString &dbPath, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return false;
    return raw.exec(QStringLiteral("UPDATE open_wake_state SET next_due_at=unixepoch()-1"));
}

// 把今日回顾预算行统一拨到一分钟前：清冷却但不逐次累计偏移，减少跨本地午夜的用例抖动。
static bool rewindReviewBudgetClock(const QString &dbPath, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return false;
    return raw.exec(QStringLiteral("UPDATE thought_review_log SET started_at=unixepoch()-60"));
}

// 相对 now 的秒数差是否落在期望值附近（容忍用例执行的少量抖动）。
static bool withinSeconds(qlonglong value, qlonglong expected, qlonglong tolerance)
{
    return value >= expected - tolerance && value <= expected + tolerance;
}

// ===== R12-1b 测试辅助 =====
// 直接数表行数（独立连接；调用时不能有未提交的写事务）。
static int tableCount(const QString &dbPath, const QString &table, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return -1;
    return raw.scalar(QStringLiteral("SELECT count(*) FROM %1").arg(table)).toInt();
}

// R12-1b：造一条存量 legacy 活动（旧 saveQATurn 会写 chat_history.thought）。
// 仍在验证 legacy 适配/旧分享链/旧动作路径的用例用它准备数据；前台新写入走 internal 活动。
static qlonglong seedLegacyThought(MemoryManager &mm, const QString &interest,
                                   const QString &channel,
                                   const QString &session,
                                   const QString &userText)
{
    static qlonglong serial = 0;
    RawDb raw(mm.databasePath(),
              QStringLiteral("legacy-thought-%1").arg(++serial));
    if (!raw.open()) return 0;
    qlonglong id = 0;
    if (!raw.insertLegacyThought(interest, channel, session, userText, &id)) return 0;
    return id;
}

// 把内部活动拨到"现在到期"，避免等待 30 分钟回顾间隔。
static bool makeInternalActivityDue(const QString &dbPath, qlonglong id, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return false;
    return raw.exec(QStringLiteral(
        "UPDATE internal_activity SET review_after=unixepoch()-1 WHERE id=%1").arg(id));
}

static QVariant internalActivityColumn(const QString &dbPath, qlonglong id,
                                       const QString &column, const QString &tag)
{
    RawDb raw(dbPath, tag);
    if (!raw.open()) return {};
    return raw.scalar(QStringLiteral("SELECT %1 FROM internal_activity WHERE id=%2")
                          .arg(column).arg(id));
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

// R12-2c-2：构造 v32 形状的 action_proposal（不含 v33 七列）+ attention_state。
// withReflectionTable=false 时故意缺少 private_reflection，让迁移事务最后一步
// （concern_resolution 的 DELETE 触发器）失败，用于验证整个 v33 迁移事务回滚。
static bool seedV32ProposalFixture(RawDb &raw, bool withReflectionTable)
{
    bool ok = raw.exec(QStringLiteral(
        "CREATE TABLE action_proposal("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "action_key TEXT NOT NULL UNIQUE,"
        "kind TEXT NOT NULL,"
        "origin_kind TEXT NOT NULL,"
        "origin_ref TEXT NOT NULL,"
        "origin_version INTEGER NOT NULL,"
        "run_id TEXT,"
        "scope TEXT NOT NULL,"
        "context_epoch INTEGER NOT NULL DEFAULT 0,"
        "target_channel TEXT NOT NULL,"
        "target_session TEXT NOT NULL,"
        "payload_json TEXT NOT NULL,"
        "claims_json TEXT NOT NULL DEFAULT '[]',"
        "semantic_status TEXT,"
        "state TEXT NOT NULL DEFAULT 'proposed',"
        "expires_at INTEGER NOT NULL DEFAULT 0,"
        "committed_action_key TEXT,"
        "error_code TEXT,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL)"));
    ok = ok && raw.exec(QStringLiteral(
        "CREATE TABLE attention_state(key TEXT PRIMARY KEY,value TEXT,"
        "updated_at INTEGER NOT NULL DEFAULT 0)"));
    // v32 时代已有的动作/Outbox 行：v33 迁移绝不能改写它们。
    ok = ok && raw.exec(QStringLiteral(
        "CREATE TABLE cognitive_action(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "action_key TEXT NOT NULL UNIQUE,kind TEXT NOT NULL,channel TEXT,session_id TEXT,"
        "payload_json TEXT,state TEXT)"));
    ok = ok && raw.exec(QStringLiteral(
        "CREATE TABLE action_outbox(id INTEGER PRIMARY KEY AUTOINCREMENT,action_id INTEGER,"
        "action_key TEXT,channel TEXT,session_id TEXT,payload_json TEXT,state TEXT,"
        "attempts INTEGER NOT NULL DEFAULT 0)"));
    ok = ok && raw.exec(QStringLiteral(
        "INSERT INTO cognitive_action(action_key,kind,channel,session_id,payload_json,state) "
        "VALUES('v32-legacy-action','SpontaneousContact','pet','default','{}','queued')"));
    ok = ok && raw.exec(QStringLiteral(
        "INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,state) "
        "VALUES(1,'v32-legacy-action','pet','default','{}','queued')"));
    if (withReflectionTable)
        ok = ok && raw.exec(QStringLiteral(
            "CREATE TABLE private_reflection(turn_id INTEGER PRIMARY KEY)"));
    ok = ok && raw.exec(QStringLiteral(
        "INSERT INTO action_proposal(action_key,kind,origin_kind,origin_ref,origin_version,"
        "scope,target_channel,target_session,payload_json,claims_json,state,expires_at,"
        "created_at,updated_at) VALUES('concern:9:1:contact','contact','concern','concern/9',1,"
        "'owner','pet','default','{\"message\":\"在吗\"}','[]','verified',0,"
        "unixepoch(),unixepoch())"));
    ok = ok && raw.exec(QStringLiteral("PRAGMA user_version=32"));
    return ok;
}

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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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
            QVERIFY(seedLegacyThought(db, QStringLiteral("想写雨声")) > 0);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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

    // R12-0：wake 返回类型化原因，区分无候选/预算/在途与真正启动。
    void wakeReportsTypedReasons()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // R12-1b：ActivityReview 受私下写入门控；旧路径用例显式开启。
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(dir.filePath("wake-reasons.db"));
        PrivateReview review(db);

        // 空库：没有到期活动/关注/新来源。
        auto result = review.wake(5, 60);
        QVERIFY(!result.started);
        QCOMPARE(result.kind, QStringLiteral("none"));
        QCOMPARE(result.reason, QStringLiteral("no_candidate"));

        // 造一条到期活动；dailyLimit=0 时应报 daily_limit 而不是 no_candidate。
        QVERIFY(seedLegacyThought(db, QStringLiteral("雨声")) > 0);
        result = review.wake(0, 60);
        QVERIFY(!result.started);
        QCOMPARE(result.kind, QStringLiteral("activity"));
        QCOMPARE(result.reason, QStringLiteral("daily_limit"));

        // 正常领取：started + kind=activity + thought/<id> 引用 + 版本。
        result = review.wake(5, 60);
        QVERIFY(result.started);
        QCOMPARE(result.kind, QStringLiteral("activity"));
        QCOMPARE(result.reason, QStringLiteral("started"));
        QVERIFY(result.sourceRef.startsWith(QStringLiteral("thought/")));
        QVERIFY(result.version > 0);

        // 已有在途回顾：重复 wake 不重复领取。
        result = review.wake(5, 60);
        QVERIFY(!result.started);
        QCOMPARE(result.kind, QStringLiteral("activity"));
        QCOMPARE(result.reason, QStringLiteral("busy"));
        QVERIFY(result.sourceRef.startsWith(QStringLiteral("thought/")));
    }

    // R12-0：仅 Reflection 强制失败退避；120/300/900 → 连续失败达到上限 suspended(86400)。
    void reflectionFailureBackoff()
    {
        ProactiveIdentityGuard guard;
        const auto writeBackoffConfig = [&guard](int maxFailures) {
            const QJsonObject cognition{
                {QStringLiteral("review_backoff_seconds"), QJsonArray{120, 300, 900}},
                {QStringLiteral("max_review_failures"), maxFailures}};
            return guard.writeIdentity(QJsonDocument(
                QJsonObject{{QStringLiteral("cognition"), cognition}})
                .toJson(QJsonDocument::Compact));
        };
        // 连续失败：前 N-1 次按退避序列，第 N 次 suspended(86400)，之后暂停期内不得重领。
        const auto runScenario = [](int maxFailures, const QList<int> &backoffExpectations) {
            QTemporaryDir dir;
            if (!dir.isValid()) return false;
            const auto path = dir.filePath(QStringLiteral("backoff-%1.db").arg(maxFailures));
            MemoryManager db(path);
            const auto turn = seedReflectionSource(db, QStringLiteral("scenario-%1").arg(maxFailures));
            if (turn <= 0) return false;
            PrivateReview review(db);
            QSignalSpy requested(&review, &PrivateReview::requested);
            bool sawSuspended = false;
            for (int attempt = 0; attempt <= backoffExpectations.size(); ++attempt) {
                const auto wake = review.wake(12, 60);
                if (!wake.started || wake.kind != QStringLiteral("reflection")) return false;
                const auto id = requested.last()[0].toLongLong();
                const int version = requested.last()[1].toInt();
                const auto origin = originProposal(requested.last()[2].toJsonObject());
                const bool lastAttempt = attempt == backoffExpectations.size();
                if (lastAttempt) {
                    MessageCapture capture;
                    review.receive(id, version, origin, QStringLiteral("network_error"));
                    for (const auto &line : capture.lines())
                        if (line.message.contains(QStringLiteral("[Review] suspended")))
                            sawSuspended = true;
                } else {
                    review.receive(id, version, origin, QStringLiteral("network_error"));
                }
                const auto wait = deferredSeconds(
                    path, turn,
                    QStringLiteral("deferred-%1-%2").arg(maxFailures).arg(attempt));
                if (lastAttempt) {
                    // 超过上限：suspended 24h。
                    if (wait < 86390 || wait > 86410) return false;
                    if (!sawSuspended) return false;
                    // 暂停期内再次 wake 不能重新领取。
                    const auto blocked = review.wake(12, 60);
                    if (blocked.started || blocked.reason != QStringLiteral("no_candidate"))
                        return false;
                } else {
                    const int expected = backoffExpectations.at(attempt);
                    if (wait < expected - 5 || wait > expected + 5) return false;
                    // 模拟退避时间已过，进入下一次失败尝试。
                    if (!rewindReflectionAttempt(
                            path, turn,
                            QStringLiteral("rewind-%1-%2").arg(maxFailures).arg(attempt)))
                        return false;
                }
            }
            return true;
        };

        // max=3：120 → 300 → 第 3 次 suspended(86400)。
        QVERIFY(writeBackoffConfig(3));
        QVERIFY(runScenario(3, {120, 300}));
        // max=4：120 → 300 → 900 → 第 4 次 suspended(86400)（覆盖 900 这一档）。
        QVERIFY(writeBackoffConfig(4));
        QVERIFY(runScenario(4, {120, 300, 900}));

        // yieldAttention（用户插话/关停）走原有立即 defer，不累计失败：
        // 让出后第一次真实失败仍使用第一档 120 秒。
        QVERIFY(writeBackoffConfig(3));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("backoff-yield.db"));
        MemoryManager db(path);
        const auto turn = seedReflectionSource(db, QStringLiteral("yield"));
        QVERIFY(turn > 0);
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        auto wake = review.wake(12, 60);
        QVERIFY(wake.started);
        review.yieldAttention(QStringLiteral("incoming_message"));
        const auto immediate = deferredSeconds(path, turn, QStringLiteral("deferred-yield"));
        QVERIFY2(immediate >= -2 && immediate <= 2, qPrintable(QString::number(immediate)));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("yield-clock")));
        wake = review.wake(12, 60);
        QVERIFY(wake.started);
        review.receive(requested.last()[0].toLongLong(), requested.last()[1].toInt(),
                       originProposal(requested.last()[2].toJsonObject()),
                       QStringLiteral("network_error"));
        const auto wait = deferredSeconds(path, turn, QStringLiteral("deferred-yield-fail"));
        QVERIFY2(wait >= 115 && wait <= 125, qPrintable(QString::number(wait)));
    }

    // R12-1a：关注保留期结束后终止定时重试：清 failure 行、next_check_at=0、state 仍 consider，
    // note/topic/fact 保留；日志可审计；后续合法新证据仍能匹配/更新。
    void concernRetentionTerminationAudited()
    {
        // 固定保留期，避免依赖测试目录里可能残留的 identity.json。
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(
            QByteArray(R"({"proactive":{"concern_retention_days":14}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("concern-retention-terminated.db");
        MemoryManager db(path);
        const qlonglong turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("retention-terminated")),
            QStringLiteral("周末想去看一场电影"), QStringLiteral("电影好不好看"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("电影")},
                        {QStringLiteral("watch"), QStringLiteral("评价如何")},
                        {QStringLiteral("next_check_after_minutes"), 60}});
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("concern/%1").arg(turn);
        // 先把关注按到期领取并制造一次失败，确认存在 failure 行 + next_check_at=next_retry。
        {
            RawDb raw(path, "retention-terminated-mark");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                "WHERE turn_id=%1").arg(turn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("retention-terminated-clock")));
        const auto claim = db.claimDueConcern(12, 60);
        QCOMPARE(claim.value(QStringLiteral("id")).toInteger(), turn);
        QVERIFY(db.recordReviewFailure(QStringLiteral("concern"), ref,
                                       QStringLiteral("generate"),
                                       QStringLiteral("network_error"), true));
        {
            RawDb raw(path, "retention-terminated-before");
            QVERIFY(raw.open());
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                        .arg(turn)).toLongLong() > 0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM review_failure "
                         "WHERE source_kind='concern' AND source_ref='%1'").arg(ref)).toInt(), 1);
        }
        // 把来源时间回拨到 30 天前（超过保留期 14 天）；claim 前应先终止定时重试。
        {
            RawDb raw(path, "retention-terminated-backdate");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE cognitive_source SET reported=datetime('now','-30 day') "
                "WHERE event=(SELECT cognitive_event FROM chat_history WHERE id=%1)").arg(turn)));
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE chat_history SET timestamp=datetime('now','-30 day') "
                "WHERE id=%1").arg(turn)));
            // 即便 next_check_at 已被失败行排到未来，也按 ended 保留期终止。
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET next_check_at=unixepoch()+3600 "
                "WHERE turn_id=%1").arg(turn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("retention-terminated-clock-2")));
        int expiredLogs = 0;
        {
            MessageCapture capture;
            QVERIFY(db.claimDueConcern(12, 60).isEmpty());
            for (const auto &line : capture.lines()) {
                if (!line.message.contains(QStringLiteral("[Review] retry_expired kind=concern")))
                    continue;
                ++expiredLogs;
                QVERIFY2(line.message.contains(QStringLiteral("ref=%1").arg(ref)),
                         qPrintable(line.message));
                QVERIFY2(line.message.contains(QStringLiteral("retention_days=14")),
                         qPrintable(line.message));
                QVERIFY2(line.message.contains(QStringLiteral("reported=")),
                         qPrintable(line.message));
            }
        }
        QVERIFY(expiredLogs >= 1);
        // 终止结果：failure 行清除、next_check_at=0、state 仍 consider、note/topic 保留。
        {
            RawDb raw(path, "retention-terminated-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM review_failure "
                         "WHERE source_kind='concern' AND source_ref='%1'").arg(ref)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("consider"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                         .arg(turn)).toLongLong(), qlonglong(0));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT note FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("电影好不好看"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT topic FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("电影"));
        }
        // 已终止的关注不再重复打终止日志（不具备定时重试资格）。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("retention-terminated-clock-3")));
        {
            MessageCapture capture;
            QVERIFY(db.claimDueConcern(12, 60).isEmpty());
            int repeated = 0;
            for (const auto &line : capture.lines())
                if (line.message.contains(QStringLiteral("[Review] retry_expired")))
                    ++repeated;
            QCOMPARE(repeated, 0);
        }
        // 后续合法新证据仍能命中（state=consider，note/topic 未被删除）。
        const auto matched = db.matchedConcerns(
            concernTurnMeta(QStringLiteral("retention-viewer")), {},
            QStringLiteral("电影好不好看"), 0, 2);
        QVERIFY(!matched.isEmpty());
        QCOMPARE(matched.first().toObject().value(QStringLiteral("ref")).toString(),
                 QStringLiteral("reflection/%1").arg(turn));
    }

    // R12-1a：失败回调时（领取后、失败回执前）源时间已越过保留期 -> 立即终止重试，
    // 返回 error=retry_expired，不写 failure 行；关注本身与记忆事实保留。
    void concernFailureAfterRetentionTerminatesRetry()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(
            QByteArray(R"({"proactive":{"concern_retention_days":1}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("concern-failure-expired.db");
        MemoryManager db(path);
        const qlonglong turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("failure-expired")),
            QStringLiteral("想知道他明天几点出门"), QStringLiteral("他几点出门"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("出门")},
                        {QStringLiteral("next_check_after_minutes"), 60}});
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("concern/%1").arg(turn);
        {
            RawDb raw(path, "failure-expired-mark");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                "WHERE turn_id=%1").arg(turn)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("failure-expired-clock")));
        const auto claim = db.claimDueConcern(12, 60);
        QCOMPARE(claim.value(QStringLiteral("id")).toInteger(), turn);
        // 领取之后把源 reported 回拨到保留期之外，再送失败回执：应终止而不是写重试。
        {
            RawDb raw(path, "failure-expired-backdate");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE cognitive_source SET reported=datetime('now','-3 day') "
                "WHERE event=(SELECT cognitive_event FROM chat_history WHERE id=%1)").arg(turn)));
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE chat_history SET timestamp=datetime('now','-3 day') "
                "WHERE id=%1").arg(turn)));
        }
        QString recordError;
        int expiredLogs = 0;
        {
            MessageCapture capture;
            QVERIFY(!db.recordReviewFailure(QStringLiteral("concern"), ref,
                                            QStringLiteral("generate"),
                                            QStringLiteral("network_error"), true,
                                            &recordError));
            for (const auto &line : capture.lines())
                if (line.message.contains(QStringLiteral("[Review] retry_expired kind=concern")))
                    ++expiredLogs;
        }
        QCOMPARE(recordError, QStringLiteral("retry_expired"));
        QVERIFY(expiredLogs >= 1);
        {
            RawDb raw(path, "failure-expired-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM review_failure WHERE source_kind='concern' "
                         "AND source_ref='%1'").arg(ref)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("consider"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                         .arg(turn)).toLongLong(), qlonglong(0));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT note FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("他几点出门"));
        }
    }

    // R12-1a：失败行 next_retry_at 已越过保留期（当前时间还没越过）也终止：不必等下一个
    // 重试到点，直接清 failure 行 + next_check_at=0，保留关注与事实。
    void concernScheduledRetryBeyondRetentionTerminated()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(
            QByteArray(R"({"proactive":{"concern_retention_days":1}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("concern-retry-beyond-retention.db");
        MemoryManager db(path);
        const qlonglong turn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("retry-beyond-retention")),
            QStringLiteral("展览下周还开不开"), QStringLiteral("展览还开不开"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("展览")},
                        {QStringLiteral("next_check_after_minutes"), 60}});
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("concern/%1").arg(turn);
        // 直接构造失败行：next_retry_at 在保留期截止之后（reported≈now，保留期 1 天，
        // next_retry=now+2 天），但 next_check_at 仍在未来 2 天，当前时间并未越过截止。
        {
            RawDb raw(path, "retry-beyond-retention-setup");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                "next_check_at=unixepoch()+172800 WHERE turn_id=%1").arg(turn)));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO review_failure(source_kind,source_ref,consecutive_failures,"
                "next_retry_at,suspended_until,last_stage,last_reason,last_attempt_at,updated_at) "
                "VALUES('concern','%1',4,unixepoch()+172800,unixepoch()+172800,"
                "'generate','network_error',unixepoch(),unixepoch())").arg(ref)));
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("retry-beyond-retention-clock")));
        int expiredLogs = 0;
        {
            MessageCapture capture;
            QVERIFY(db.claimDueConcern(12, 60).isEmpty());
            for (const auto &line : capture.lines())
                if (line.message.contains(QStringLiteral("[Review] retry_expired kind=concern")))
                    ++expiredLogs;
        }
        QVERIFY(expiredLogs >= 1);
        {
            RawDb raw(path, "retry-beyond-retention-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM review_failure WHERE source_kind='concern' "
                         "AND source_ref='%1'").arg(ref)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("consider"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                         .arg(turn)).toLongLong(), qlonglong(0));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT note FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("展览还开不开"));
        }
    }

    void writingResumesFromCommittedDraftAfterInterruption()
    {
        QTemporaryDir dir;
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        const auto path = dir.filePath("writing.db");
        const QJsonObject chapter{{"status","active"},{"reflection","opening written"},
                                  {"draft","first paragraph"},{"next_step","second paragraph"}};
        qlonglong id = 0;
        int interruptedVersion = 0;
        {
            MemoryManager db(path);
            QVERIFY(seedLegacyThought(db, QStringLiteral("rain")) > 0);
            PrivateReview writing(db);
            QSignalSpy requests(&writing, &PrivateReview::requested);
            QSignalSpy cancellations(&writing, &PrivateReview::cancelled);
            writing.wake(5,60);
            writing.wake(5,60);
            QCOMPARE(requests.size(),1);
            id = requests.first()[0].toLongLong();
            writing.receive(id,requests.first()[1].toInt(),
                            proposalWithOrigin(requests.first()[2].toJsonObject(), chapter),{});
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
        restored.receive(id,attempt[1].toInt(),
                         proposalWithOrigin(attempt[2].toJsonObject(), finalDraft),{});
        QCOMPARE(shares.size(),1);
        QCOMPARE(db.nextThought(false)["last_review"].toObject()["draft"],finalDraft["draft"]);
        QVERIFY(db.nextThought(true).isEmpty());
    }

    void expiredWritingClaimRejectsOldAttempt()
    {
        QTemporaryDir dir;
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        const auto path = dir.filePath("expired.db");
        qlonglong id = 0;
        int version = 0;
        {
            MemoryManager db(path);
            QVERIFY(seedLegacyThought(db, QStringLiteral("a poem")) > 0);
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
        writing.receive(id,newVersion,
                        proposalWithOrigin(requests.first()[2].toJsonObject(),
                                           QJsonObject{{"status","active"},
                                                       {"reflection","edited"},
                                                       {"draft",QString(801,'x')}}),{});
        QCOMPARE(db.nextThought(true)["last_review"].toObject()["deferred_reason"].toString(),
                 QString("invalid_or_stale_proposal"));
        QVERIFY(!db.nextThought(true)["last_review"].toObject().contains("draft"));
    }

    void writingCannotShareAfterAbandonOrDeletion()
    {
        for (const bool remove : {false,true}) {
            QTemporaryDir dir;
            ProactiveIdentityGuard identity;
            if (!identity.writeIdentity(QByteArray(
                    R"({"proactive":{"private_writing":true}})")))
                return;
            MemoryManager db(dir.filePath("cancel.db"));
            QVERIFY(seedLegacyThought(db, QStringLiteral("rain")) > 0);
            PrivateReview writing(db);
            QSignalSpy requests(&writing, &PrivateReview::requested);
            QSignalSpy shares(&writing, &PrivateReview::shareRequested);
            writing.wake(5,60);
            QCOMPARE(requests.size(),1);
            const auto id = requests.first()[0].toLongLong();
            const auto version = requests.first()[1].toInt();
            if (remove) QVERIFY(db.deleteTurnByID(id));
            else QVERIFY(db.applyThoughtAction(id,version,"abandon"));
            writing.receive(id,version,
                            proposalWithOrigin(requests.first()[2].toJsonObject(),
                                               QJsonObject{{"status","active"},
                                                           {"reflection","late"},
                                                           {"draft","late"},
                                                           {"share_with_owner",true}}),{});
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
            QVERIFY(seedLegacyThought(db, QStringLiteral("想写雨声")) > 0);
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
        // R12-1b：activity_action 由私下写作门控；本用例只验证引用/版本解析。
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(dir.filePath("activity-reply.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = "2014366825";
        meta.eventId = "activity-a";
        QVERIFY(seedLegacyThought(db, QStringLiteral("写雨声")) > 0);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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
        QCOMPARE(copy.scalar("PRAGMA user_version").toInt(), 33);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);

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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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

        QVERIFY(seedLegacyThought(db, QStringLiteral("想写雨声")) > 0);
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
        QVERIFY(seedLegacyThought(db, QStringLiteral("想写风声")) > 0);
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
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
            const QStringList contact = raw.columns("reflection_contact");
            for (const auto &column : {QStringLiteral("attempts"), QStringLiteral("last_outcome"),
                                       QStringLiteral("last_attempt_at"),
                                       QStringLiteral("basis_type"), QStringLiteral("basis_refs"),
                                       QStringLiteral("motivation_json"), QStringLiteral("claims_json"),
                                       QStringLiteral("semantic_status")})
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
                       proposalWithOrigin(context,
                                          QJsonObject{{"concern_action", "keep"},
                                                      {"forget", QJsonObject{{"next_check_after_minutes", 0}}}}),
                       {});
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

    // 收口机制已删除：close/defer 之前的发言不再因"刚收口"被拒绝；
    // 只要它晚于关注来源，仍可作为 new_owner_evidence。
    void contactNowAcceptsEvidenceFromBeforeClosure()
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
        // 先有一段收口的交流：B 是 quiet close。close 仍作为前台状态记录，但不再参与门禁。
        RequestMeta metaB = concernTurnMeta(QStringLiteral("closure-b"));
        metaB.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-120);
        const qint64 closed = db.recordInput(QStringLiteral("先聊到这里吧"), metaB, true);
        QVERIFY(closed > 0);
        QVERIFY(db.recordQuiet(closed, "close"));
        QCOMPARE(db.conversationPhase(metaB), QStringLiteral("close"));
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
        // 收口之前的发言（晚于关注来源）：收口不再拦截，应该被接受。
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
            QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("now"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("proposed"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("new_owner_evidence"));
            QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                                   .arg(turn)).toString()
                        .contains(QStringLiteral("chat/%1").arg(closed)));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM private_reflection WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("consider"));
        }
        // 收口之后主人真的又说话了：同样通过（时序仍只按关注来源判断）。
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
        QVERIFY(seedLegacyThought(db, QStringLiteral("想写雨声")) > 0);
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
        QVERIFY(seedLegacyThought(db, QStringLiteral("想写风声")) > 0);
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

    // 收口机制已删除：pet 里刚 close 过也不再挡住 QQ 私聊的主动联系；
    // 依据仍只要求晚于关注来源（时序检查保留）。
    void crossChannelClosureNoLongerBlocksContact()
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
        // pet 里收口（晚安）：close 仍作为前台状态记录，但不再挡住 QQ 私聊的主动联系。
        RequestMeta petCloseMeta = concernTurnMeta(QStringLiteral("cross-closure-pet"));
        petCloseMeta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-10);
        const qlonglong petClose = db.recordInput(QStringLiteral("晚安啦"), petCloseMeta, true);
        QVERIFY(petClose > 0);
        QVERIFY(db.recordQuiet(petClose, "close"));
        QCOMPARE(db.conversationPhase(petCloseMeta), QStringLiteral("close"));
        // 收口之前的 QQ 发言（晚于关注来源）不再被拦截，可以作为新证据。
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
            QCOMPARE(raw.scalar(QStringLiteral("SELECT timing FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("now"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT basis_type FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(), QStringLiteral("new_owner_evidence"));
            QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                                   .arg(turn)).toString()
                        .contains(QStringLiteral("chat/%1").arg(beforeClose)));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT action FROM concern_revision WHERE turn=%1 "
                                               "ORDER BY id DESC LIMIT 1").arg(turn)).toString(),
                     QStringLiteral("contact"));
        }
        // 收口之后 QQ 里又有新发言：同样通过。
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
        QVERIFY(raw.scalar(QStringLiteral("SELECT basis_refs FROM reflection_contact WHERE turn_id=%1")
                               .arg(turn)).toString()
                    .contains(QStringLiteral("chat/%1").arg(afterClose)));
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
        QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
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

    // 保留期：来源 reported 超过 concern_retention_days 后，定时到期回顾不再领取；
    // R12-1a 起同时终止定时重试（next_check_at=0），但关注本身与 note/topic/事实保留，
    // 后续合法新证据仍可命中/更新。
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
        // 定时重试资格被终止：state 仍 consider、next_check_at=0、note/topic 保留。
        {
            RawDb raw(path, "concern-retention-terminated");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("consider"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_check_at FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toLongLong(), qlonglong(0));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT note FROM private_reflection WHERE turn_id=%1").arg(turn))
                         .toString(), QStringLiteral("展览怎么样"));
        }
        // 合法新证据仍能命中同一个关注（不因保留期终止丢失关注/事实）。
        const auto matched = db.matchedConcerns(
            concernTurnMeta(QStringLiteral("concern-retention-viewer")), {},
            QStringLiteral("展览怎么样"), 0, 2);
        QVERIFY(!matched.isEmpty());
        QCOMPARE(matched.first().toObject().value(QStringLiteral("ref")).toString(),
                 QStringLiteral("reflection/%1").arg(turn));
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
            QCOMPARE(schema.scalar("PRAGMA user_version").toInt(), 33);
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
        QVERIFY(seedLegacyThought(db, QStringLiteral("写雨声")) > 0);
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
        QVERIFY(seedLegacyThought(db, QStringLiteral("写雨声")) > 0);
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

    // 切片 B：contact 改为"域 CAS + cognitive_action + action_outbox 同事务提交"后，
    // 旧直发路径（claimReflectionContact）不再能领到同一意图；唯一状态机是
    // commit → claimOutbox → settle。重复提交幂等、outbox 只有一行。
    void outboxContactCommitDisablesDirectClaim()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("outbox-contact.db");
        MemoryManager db(path);
        const auto evidence = insertOwnerEvidenceTurn(db, QStringLiteral("明天想去看海"));
        QVERIFY(evidence > 0);
        const qint64 turn = seedOpenConcern(db, concernTurnMeta(QStringLiteral("outbox-contact-a")),
                                            QStringLiteral("想去看海"), QStringLiteral("看海"),
                                            QJsonObject{},
                                            contactNow(QStringLiteral("有新消息"), QStringLiteral("在吗"),
                                                       ownerEvidenceBasis(evidence)));
        QVERIFY(turn > 0);
        const auto committed = db.commitReflectionContactAction(turn, 1, QStringLiteral("test"));
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString actionKey = QStringLiteral("reflection:%1:1:contact").arg(turn);
        QCOMPARE(committed.value("action_key").toString(), actionKey);
        QCOMPARE(committed.value("state").toString(), QStringLiteral("queued"));
        {
            RawDb raw(path, "outbox-contact-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(),
                     QStringLiteral("dispatching"));
        }
        // 硬禁用旧路径：同一意图已经交给 outbox，claimReflectionContact 不能再领取（否则会新旧同时发）。
        QVERIFY(db.claimReflectionContact(turn, 1, Channel::kPet, Channel::kDefaultSession).isEmpty());
        // 幂等：重复提交返回既有状态，不产生第二条 outbox。
        const auto again = db.commitReflectionContactAction(turn, 1);
        QCOMPARE(again.value("idempotent").toBool(), true);
        {
            RawDb raw(path, "outbox-contact-raw2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
        }
        const auto job = db.claimOutbox(QStringLiteral("test-owner"), 60);
        QCOMPARE(job.value("action_key").toString(), actionKey);
        QCOMPARE(job.value("kind").toString(), QStringLiteral("SpontaneousContact"));
        const auto sentences = job.value("payload").toObject().value("sentences").toArray();
        QCOMPARE(sentences.size(), 1);
        QCOMPARE(sentences.first().toObject().value("zh_text").toString(), QStringLiteral("在吗"));
        // 首次领取后不会再被第二个领取者拿到（同一行不会并发领取）。
        QVERIFY(db.claimOutbox(QStringLiteral("test-owner-2"), 60).isEmpty());
        QVERIFY(db.settleOutboxAction(actionKey, QStringLiteral("protocol_accepted"), QStringLiteral("ok")));
        {
            RawDb raw(path, "outbox-contact-raw3");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT outcome FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar("SELECT state FROM cognitive_action").toString(),
                     QStringLiteral("handed_off"));
            QCOMPARE(raw.scalar("SELECT last_outcome FROM cognitive_action").toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(),
                     QStringLiteral("dispatched"));
        }
    }

    // 切片 B：失败提交（依据被拒/无提醒/未到期）不写任何 action/outbox 行（原子性）。
    void outboxRejectedCommitWritesNothing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("outbox-reject.db");
        MemoryManager db(path);
        // contact now 没有可核验依据：finishPrivateReflection 不写 reflection_contact，commit 必须失败。
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("outbox-reject-clock")));
        const qint64 turn = seedOpenConcern(db, concernTurnMeta(QStringLiteral("outbox-reject-a")),
                                            QStringLiteral("随便聊聊"), QStringLiteral("聊聊天"),
                                            QJsonObject{},
                                            contactNow(QStringLiteral("想问问"), QStringLiteral("在吗")));
        QVERIFY(turn > 0);
        const auto rejected = db.commitReflectionContactAction(turn, 1);
        QVERIFY(rejected.contains("error"));
        // 不存在的提醒 / 未到期的提醒：同样不写行。
        QVERIFY(db.commitReminderDeliveryAction(987654).contains("error"));
        const qint64 notDue = insertPendingReminder(path, QStringLiteral("outbox-not-due"),
                                                     Channel::kPet, Channel::kDefaultSession,
                                                     3600, QStringLiteral("一小时后"));
        QVERIFY(notDue > 0);
        QVERIFY(db.commitReminderDeliveryAction(notDue).contains("error"));
        {
            RawDb raw(path, "outbox-reject-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action").toInt(), 0);
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM reflection_contact WHERE turn_id=%1")
                                    .arg(turn)).toInt(), 0);
        }
    }

    // 切片 B：提醒与分享的提交/领取/回执状态机；提醒提交后旧 claimDueReminder 不可再领取。
    void outboxReminderAndShareStateMachine()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("outbox-reminder-share.db");
        MemoryManager db(path);
        const qint64 reminder = insertPendingReminder(path, QStringLiteral("outbox-due"),
                                                       Channel::kPet, Channel::kDefaultSession,
                                                       -10, QStringLiteral("该喝水了"));
        QVERIFY(reminder > 0);
        const auto reminderCommit = db.commitReminderDeliveryAction(reminder);
        QVERIFY2(!reminderCommit.contains("error"),
                 qPrintable(reminderCommit.value("error").toString()));
        const QString reminderKey = QStringLiteral("reminder:%1:delivery").arg(reminder);
        QCOMPARE(reminderCommit.value("action_key").toString(), reminderKey);
        // 旧直发路径已不可领取（reminder.state 已离开 pending）。
        QVERIFY(db.claimDueReminder(Channel::kPet, Channel::kDefaultSession).isEmpty());
        // 重复提交幂等。
        QCOMPARE(db.commitReminderDeliveryAction(reminder).value("idempotent").toBool(), true);
        const auto reminderJob = db.claimOutbox(QStringLiteral("test-owner"), 60);
        QCOMPARE(reminderJob.value("action_key").toString(), reminderKey);
        QCOMPARE(reminderJob.value("payload").toObject().value("message").toString(),
                 QStringLiteral("该喝水了"));
        QVERIFY(db.settleOutboxAction(reminderKey, QStringLiteral("protocol_accepted")));
        {
            RawDb raw(path, "outbox-reminder-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox").toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM reminder WHERE id=%1").arg(reminder)).toString(),
                     QStringLiteral("protocol_accepted"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(reminderKey)).toString(),
                     QStringLiteral("handed_off"));
        }

        // 分享：completed + 产物 + 版本匹配才可提交；unknown 冻结后同一版本不能再提交。
        QVERIFY(seedLegacyThought(db, QStringLiteral("写雨声")) > 0);
        const auto thought = db.nextThought(true);
        const qlonglong activityId = thought["id"].toInteger();
        const int version = thought["version"].toInt();
        QVERIFY(activityId > 0);
        QVERIFY(db.finishThoughtReview(activityId, version,
            QJsonObject{{"status","completed"},{"reflection","写好了"},
                        {"draft","屋檐轻响，雨在窗上停了一停。"}}));
        const int sharedVersion = version + 1;
        QVERIFY(db.canShareThought(activityId, sharedVersion));
        const auto shareCommit = db.commitActivityShareAction(
            activityId, sharedVersion, QStringLiteral("屋檐轻响，雨在窗上停了一停。"));
        QVERIFY2(!shareCommit.contains("error"), qPrintable(shareCommit.value("error").toString()));
        const QString shareKey = QStringLiteral("thought:%1:%2:share").arg(activityId).arg(sharedVersion);
        QCOMPARE(shareCommit.value("action_key").toString(), shareKey);
        {
            RawDb raw(path, "outbox-share-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='share' AND ref='thought:%1:%2'")
                                    .arg(activityId).arg(sharedVersion)).toString(),
                     QStringLiteral("claimed"));
        }
        const auto shareJob = db.claimOutbox(QStringLiteral("test-owner"), 60);
        QCOMPARE(shareJob.value("action_key").toString(), shareKey);
        QCOMPARE(shareJob.value("payload").toObject().value("draft").toString(),
                 QStringLiteral("屋檐轻响，雨在窗上停了一停。"));
        QVERIFY(db.settleOutboxAction(shareKey, QStringLiteral("unknown"), QStringLiteral("timeout")));
        {
            RawDb raw(path, "outbox-share-raw2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(shareKey)).toString(),
                     QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM delivery_ledger WHERE kind='share' AND ref='thought:%1:%2'")
                                    .arg(activityId).arg(sharedVersion)).toString(),
                     QStringLiteral("unknown"));
        }
        // unknown 冻结：同一版本不再允许再分享；重复提交幂等返回已有动作，不产生第二个 outbox。
        QVERIFY(!db.canShareThought(activityId, sharedVersion));
        const auto again = db.commitActivityShareAction(
            activityId, sharedVersion, QStringLiteral("屋檐轻响，雨在窗上停了一停。"));
        QCOMPARE(again.value("idempotent").toBool(), true);
        QCOMPARE(again.value("state").toString(), QStringLiteral("unknown"));
        QVERIFY(db.claimOutbox(QStringLiteral("test-owner"), 60).isEmpty());
        {
            RawDb raw(path, "outbox-share-raw3");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT count(*) FROM cognitive_action WHERE kind='ActivityShare'").toInt(), 1);
            QCOMPARE(raw.scalar("SELECT count(*) FROM action_outbox WHERE action_key LIKE 'thought:%'").toInt(), 1);
        }
    }

    // 切片 B：拒绝后受控重试一次；租约过期的 dispatching 残留恢复为 unknown 且不再领取。
    void outboxRetryAndStaleRecovery()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath("outbox-retry.db");
        MemoryManager db(path);
        const qint64 reminder = insertPendingReminder(path, QStringLiteral("outbox-retry-due"),
                                                       Channel::kPet, Channel::kDefaultSession,
                                                       -10, QStringLiteral("该休息了"));
        QVERIFY(reminder > 0);
        const auto committed = db.commitReminderDeliveryAction(reminder);
        QVERIFY2(!committed.contains("error"), qPrintable(committed.value("error").toString()));
        const QString actionKey = committed.value("action_key").toString();
        const auto first = db.claimOutbox(QStringLiteral("owner-1"), 60);
        QCOMPARE(first.value("action_key").toString(), actionKey);
        QCOMPARE(first.value("attempts").toInt(), 1);
        // 第一次被拒：outbox 回 queued，允许受控重试一次。
        QVERIFY(db.settleOutboxAction(actionKey, QStringLiteral("rejected"), QStringLiteral("submit_failed")));
        {
            RawDb raw(path, "outbox-retry-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("SELECT state FROM action_outbox").toString(), QStringLiteral("queued"));
            QCOMPARE(raw.scalar("SELECT attempts FROM action_outbox").toInt(), 1);
        }
        const auto second = db.claimOutbox(QStringLiteral("owner-1"), 60);
        QCOMPARE(second.value("action_key").toString(), actionKey);
        QCOMPARE(second.value("attempts").toInt(), 2);
        QVERIFY(db.settleOutboxAction(actionKey, QStringLiteral("rejected"), QStringLiteral("submit_failed")));
        // 第二次被拒后终止：不再领取。
        QVERIFY(db.claimOutbox(QStringLiteral("owner-1"), 60).isEmpty());
        // 领取后崩溃：租约过期 → recoverStaleOutbox 冻结为 unknown，绝不重发。
        const qint64 reminder2 = insertPendingReminder(path, QStringLiteral("outbox-stale-due"),
                                                        Channel::kPet, Channel::kDefaultSession,
                                                        -10, QStringLiteral("该睡觉了"));
        QVERIFY(reminder2 > 0);
        const auto committed2 = db.commitReminderDeliveryAction(reminder2);
        QVERIFY2(!committed2.contains("error"), qPrintable(committed2.value("error").toString()));
        const QString staleKey = committed2.value("action_key").toString();
        const auto claimed = db.claimOutbox(QStringLiteral("owner-1"), 60);
        QCOMPARE(claimed.value("action_key").toString(), staleKey);
        {
            RawDb raw(path, "outbox-stale-expire");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE action_outbox SET lease_until=unixepoch()-7200 WHERE action_key='%1'")
                .arg(staleKey)));
        }
        QCOMPARE(db.recoverStaleOutbox(600), 1);
        QCOMPARE(db.recoverStaleOutbox(600), 0); // 幂等：终态不再重复恢复
        {
            RawDb raw(path, "outbox-stale-raw");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM action_outbox WHERE action_key='%1'")
                                    .arg(staleKey)).toString(),
                     QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(staleKey)).toString(),
                     QStringLiteral("unknown"));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM reminder WHERE id=%1").arg(reminder2)).toString(),
                     QStringLiteral("unknown"));
        }
        QVERIFY(db.claimOutbox(QStringLiteral("owner-1"), 60).isEmpty());
        // unknown 的迟到回执不能把它改回已送达：冻结语义。
        QVERIFY(db.settleOutboxAction(staleKey, QStringLiteral("protocol_accepted")));
        {
            RawDb raw(path, "outbox-stale-raw2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("SELECT state FROM cognitive_action WHERE action_key='%1'")
                                    .arg(staleKey)).toString(),
                     QStringLiteral("unknown"));
        }
    }

    // ===== R12-1a：v31 迁移（幂等重放；旧事实/Outbox 完整）=====
    void v31MigrationReplaysIdempotently()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("v31-migration.db"));
        {
            // 先建一个完整(v31)库，再压回 v30 旧形状，模拟真实 v30 库重开升级。
            MemoryManager seed(path);
            makePetTurn(seed, QStringLiteral("窗边有光"), QStringLiteral("嗯，我看见了"));
            RequestMeta meta;
            meta.eventId = QStringLiteral("v31-legacy-event");
            QVERIFY(seed.recordInput(QStringLiteral("旧输入仍在"), meta) > 0);
        }
        {
            RawDb raw(path, "v31-downgrade");
            QVERIFY(raw.open());
            // 旧 thought_review_log 行（v30 没有 source_* 列），用重建表模拟旧形状并保留数据。
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO thought_review_log(turn_id,started_at) VALUES(1,unixepoch())")));
            QVERIFY(raw.exec(QStringLiteral(
                "ALTER TABLE thought_review_log RENAME TO thought_review_log_v31")));
            QVERIFY(raw.exec(QStringLiteral(
                "CREATE TABLE thought_review_log(turn_id INTEGER,started_at INTEGER NOT NULL)")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO thought_review_log(turn_id,started_at) "
                "SELECT turn_id,started_at FROM thought_review_log_v31")));
            QVERIFY(raw.exec(QStringLiteral("DROP TABLE thought_review_log_v31")));
            for (const auto &table : {QStringLiteral("open_wake_state"), QStringLiteral("open_wake"),
                                      QStringLiteral("internal_activity"), QStringLiteral("review_failure")})
                QVERIFY(raw.exec(QStringLiteral("DROP TABLE IF EXISTS %1").arg(table)));
            // action/outbox 存量提交行必须原样保留（迁移不碰发送路径）。
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO cognitive_action(action_key,kind,channel,session_id,payload_json,state) "
                "VALUES('v31-legacy-action','SpontaneousContact','pet','default','{}','queued')")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,state) "
                "VALUES(1,'v31-legacy-action','pet','default','{}','queued')")));
            QVERIFY(raw.exec(QStringLiteral("PRAGMA user_version=30")));
        }
        { MemoryManager upgraded(path); }
        {
            RawDb raw(path, "v31-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
            for (const auto &table : {QStringLiteral("open_wake_state"), QStringLiteral("open_wake"),
                                      QStringLiteral("internal_activity"), QStringLiteral("review_failure")})
                QCOMPARE(raw.scalar(QStringLiteral(
                             "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%1'")
                             .arg(table)).toInt(), 1);
            const QStringList reviewColumns = raw.columns("thought_review_log");
            for (const auto &column : {QStringLiteral("source_kind"), QStringLiteral("source_ref"),
                                       QStringLiteral("run_id")})
                QVERIFY2(reviewColumns.contains(column), qPrintable(column));
            // 旧行 source_* 保持 NULL；旧表数据未被改写。
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT count(*) FROM thought_review_log WHERE source_kind IS NULL")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 2);
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT state FROM cognitive_action WHERE action_key='v31-legacy-action'")).toString(),
                QStringLiteral("queued"));
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT count(*) FROM action_outbox WHERE action_key='v31-legacy-action'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM review_failure")).toInt(), 0);
        }
        // 重复启动：v31 迁移幂等重放，不重复建表/不改旧行。
        for (int reopen = 0; reopen < 2; ++reopen) { MemoryManager again(path); }
        {
            RawDb raw(path, "v31-verify-replay");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM thought_review_log")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM open_wake")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 2);
        }
    }

    // ===== R12-2a/R12-2c-2：v32→v33 迁移（幂等重放；旧数据/Outbox 完整）=====
    void v32MigrationReplaysIdempotently()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("v32-migration.db"));
        {
            MemoryManager seed(path);
            makePetTurn(seed, QStringLiteral("窗边有光"), QStringLiteral("嗯，我看见了"));
        }
        {
            // 压回 v31 旧形状：没有 action_proposal 表；旧 action/outbox 行必须原样保留。
            RawDb raw(path, "v32-downgrade");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("DROP TABLE IF EXISTS action_proposal")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO cognitive_action(action_key,kind,channel,session_id,payload_json,state) "
                "VALUES('v32-legacy-action','SpontaneousContact','pet','default','{}','queued')")));
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,state) "
                "VALUES(1,'v32-legacy-action','pet','default','{}','queued')")));
            QVERIFY(raw.exec(QStringLiteral("PRAGMA user_version=31")));
        }
        { MemoryManager upgraded(path); }
        {
            RawDb raw(path, "v32-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='action_proposal'"))
                         .toInt(), 1);
            const QStringList columns = raw.columns("action_proposal");
            for (const auto &column : {QStringLiteral("action_key"), QStringLiteral("kind"),
                                       QStringLiteral("origin_kind"), QStringLiteral("origin_ref"),
                                       QStringLiteral("origin_version"), QStringLiteral("run_id"),
                                       QStringLiteral("scope"), QStringLiteral("context_epoch"),
                                       QStringLiteral("target_channel"), QStringLiteral("target_session"),
                                       QStringLiteral("payload_json"), QStringLiteral("claims_json"),
                                       QStringLiteral("semantic_status"), QStringLiteral("state"),
                                       QStringLiteral("expires_at"),
                                       QStringLiteral("committed_action_key"),
                                       QStringLiteral("error_code"), QStringLiteral("created_at"),
                                       QStringLiteral("updated_at"),
                                       // v33（R12-2c-2）：verified 内容绑定/取消审计/fence 快照
                                       QStringLiteral("verified_payload_hash"),
                                       QStringLiteral("verified_claims_hash"),
                                       QStringLiteral("verified_request_id"),
                                       QStringLiteral("verified_at"),
                                       QStringLiteral("history_projected"),
                                       QStringLiteral("cancel_reason"),
                                       QStringLiteral("action_context_epoch")})
                QVERIFY2(columns.contains(column), qPrintable(column));
            // 旧数据/Outbox 完整：迁移只新增表，不改写任何既有行。
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT state FROM cognitive_action WHERE action_key='v32-legacy-action'")).toString(),
                QStringLiteral("queued"));
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT count(*) FROM action_outbox WHERE action_key='v32-legacy-action'")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal")).toInt(), 0);
            // action_key UNIQUE 是幂等的最后防线。
            const QString insertSql = QStringLiteral(
                "INSERT INTO action_proposal(action_key,kind,origin_kind,origin_ref,origin_version,"
                "scope,target_channel,target_session,payload_json,state,created_at,updated_at) "
                "VALUES('concern:1:1:contact','contact','concern','concern/1',1,'owner','pet',"
                "'default','{}','proposed',unixepoch(),unixepoch())");
            QVERIFY(raw.exec(insertSql));
            QVERIFY(!raw.exec(insertSql));
        }
        // 重复启动：v32 迁移幂等重放，不重复建表/不清空已存提案。
        for (int reopen = 0; reopen < 2; ++reopen) { MemoryManager again(path); }
        {
            RawDb raw(path, "v32-verify-replay");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar("PRAGMA user_version").toInt(), 33);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral(
                "SELECT count(*) FROM action_outbox WHERE action_key='v32-legacy-action'")).toInt(), 1);
        }
    }

    // ===== R12-2c-2：正式 v32→v33 迁移（verified 绑定/取消审计/fence 列）=====
    void v33MigratesFromV32WithVerifiedBindings()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("v33-from-v32.db"));
        {
            RawDb raw(path, "v33-from-v32-create");
            QVERIFY(raw.open());
            QVERIFY(seedV32ProposalFixture(raw, true));
        }
        {
            MemoryManager migrated(path);
            // 迁移不重置既有 fence 值（此库没有 → 读回 0）。
            QCOMPARE(migrated.actionContextEpoch(), qint64(0));
        }
        {
            RawDb raw(path, "v33-from-v32-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("PRAGMA user_version")).toInt(), 33);
            const QStringList columns = raw.columns("action_proposal");
            for (const auto &column : {QStringLiteral("verified_payload_hash"),
                                       QStringLiteral("verified_claims_hash"),
                                       QStringLiteral("verified_request_id"),
                                       QStringLiteral("verified_at"),
                                       QStringLiteral("history_projected"),
                                       QStringLiteral("cancel_reason"),
                                       QStringLiteral("action_context_epoch")})
                QVERIFY2(columns.contains(column), qPrintable(column));
            // 旧行完整保留；新列对旧行取 NULL/0（不追溯改写，也不伪造绑定）。
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM action_proposal WHERE action_key='concern:9:1:contact'"))
                         .toString(),
                     QStringLiteral("verified"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT json_extract(payload_json,'$.message') FROM action_proposal "
                         "WHERE action_key='concern:9:1:contact'")).toString(),
                     QStringLiteral("在吗"));
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT verified_payload_hash FROM action_proposal "
                        "WHERE action_key='concern:9:1:contact'")).isNull());
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT verified_claims_hash FROM action_proposal "
                        "WHERE action_key='concern:9:1:contact'")).isNull());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT history_projected FROM action_proposal "
                         "WHERE action_key='concern:9:1:contact'")).toInt(),
                     0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT action_context_epoch FROM action_proposal "
                         "WHERE action_key='concern:9:1:contact'")).toInt(),
                     0);
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT cancel_reason FROM action_proposal "
                        "WHERE action_key='concern:9:1:contact'")).isNull());
            // 旧动作/Outbox 完整：迁移只加列，不改写既有提交行。
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM cognitive_action WHERE action_key='v32-legacy-action'"))
                         .toString(),
                     QStringLiteral("queued"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM action_outbox WHERE action_key='v32-legacy-action'"))
                         .toInt(),
                     1);
        }
        // 重复打开：v33 幂等重放，不重复加列、不改旧行。
        for (int reopen = 0; reopen < 2; ++reopen) { MemoryManager again(path); }
        {
            RawDb raw(path, "v33-from-v32-reopen");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("PRAGMA user_version")).toInt(), 33);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal")).toInt(), 1);
        }
    }

    // v33 迁移失败必须整体回滚：PRAGMA user_version 保持 32，新列一个都不能留下，
    // 旧提案行原样保留。失败注入：库缺少 private_reflection，迁移事务末尾的
    // concern_resolution 触发器创建失败。
    void v33MigrationFailureRollsBackAllDdl()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("v33-fail.db"));
        {
            RawDb raw(path, "v33-fail-create");
            QVERIFY(raw.open());
            QVERIFY(seedV32ProposalFixture(raw, false));
        }
        { MemoryManager failed(path); } // 构造器内部 rollback；进程不受影响
        {
            RawDb raw(path, "v33-fail-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral("PRAGMA user_version")).toInt(), 32);
            const QStringList columns = raw.columns("action_proposal");
            for (const auto &column : {QStringLiteral("verified_payload_hash"),
                                       QStringLiteral("verified_claims_hash"),
                                       QStringLiteral("verified_request_id"),
                                       QStringLiteral("verified_at"),
                                       QStringLiteral("history_projected"),
                                       QStringLiteral("cancel_reason"),
                                       QStringLiteral("action_context_epoch")})
                QVERIFY2(!columns.contains(column), qPrintable(column));
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_proposal")).toInt(), 1);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM action_proposal WHERE action_key='concern:9:1:contact'"))
                         .toString(),
                     QStringLiteral("verified"));
            // 失败回滚不能丢/改旧动作与 Outbox 行。
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM cognitive_action "
                         "WHERE action_key='v32-legacy-action'")).toInt(),
                     1);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM action_outbox "
                         "WHERE action_key='v32-legacy-action'")).toInt(),
                     1);
        }
    }

    // R12-2c-2：Action Context Fence 的 attention_state 持久化/单调递增/重启恢复。
    void actionContextEpochPersistsAndBumps()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("action-fence.db"));
        {
            MemoryManager db(path);
            QCOMPARE(db.actionContextEpoch(), qint64(0));
            QCOMPARE(db.bumpActionContextEpoch(QStringLiteral("owner_private_input")), qint64(1));
            QCOMPARE(db.bumpActionContextEpoch(QStringLiteral("share_withdrawn")), qint64(2));
            QCOMPARE(db.actionContextEpoch(), qint64(2));
            // 只做审计：最新原因/更新时间可读。
            QCOMPARE(db.attentionState(QStringLiteral("action_context_reason")),
                     QStringLiteral("share_withdrawn"));
            QVERIFY(!db.attentionState(QStringLiteral("action_context_updated_at")).isEmpty());
            // 普通 setAttentionState 不参与 fence（不会覆盖 epoch）。
            db.setAttentionState(QStringLiteral("current_activity"), QStringLiteral("activity/1"));
            QCOMPARE(db.actionContextEpoch(), qint64(2));
        }
        {
            // 重启后从 attention_state 原值读回，绝不清零。
            MemoryManager reopened(path);
            QCOMPARE(reopened.actionContextEpoch(), qint64(2));
            QCOMPARE(reopened.bumpActionContextEpoch(QStringLiteral("owner_private_input")),
                     qint64(3));
        }
    }

    // R12-2c-2：取消审计——未终态提案回 expired + cancel_reason；重复取消幂等。
    void cancelActionProposalAuditsAndExpires()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("cancel-proposal.db"));
        MemoryManager db(path);
        const auto origin = ActionOrigin::forConcern(11, 1, QStringLiteral("owner"),
                                                     Channel::kPet, Channel::kDefaultSession);
        const QString key = origin.actionKey(QStringLiteral("contact"));
        const QJsonObject proposal{
            {QStringLiteral("origin"), origin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("contact")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("message"), QStringLiteral("在吗")},
                         {QStringLiteral("reason"), QStringLiteral("自己想到")}}},
            {QStringLiteral("claims"), QJsonArray{}},
            {QStringLiteral("expires_at"), 0}};
        QString error;
        QVERIFY(!db.persistActionProposal(proposal, &error).contains(QStringLiteral("error")));
        QVERIFY(db.markActionProposalVerified(key, QStringLiteral("entailed"),
                                              QStringLiteral("req-cancel"), &error));
        QVERIFY(db.cancelActionProposal(key, QStringLiteral("context_epoch_changed")));
        auto row = db.actionProposal(key);
        QCOMPARE(row.value(QStringLiteral("state")).toString(), QStringLiteral("expired"));
        QCOMPARE(row.value(QStringLiteral("cancel_reason")).toString(),
                 QStringLiteral("context_epoch_changed"));
        // 幂等：重复取消不改变终态、不报错。
        QVERIFY(db.cancelActionProposal(key, QStringLiteral("context_epoch_changed")));
        row = db.actionProposal(key);
        QCOMPARE(row.value(QStringLiteral("state")).toString(), QStringLiteral("expired"));
    }

    // R12-2a：ActionProposal 持久化 —— 幂等/冲突/CAS/过期/重启可读。
    void actionProposalPersistsIdempotentlyWithCasAndExpiry()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("proposal.db"));
        const auto origin = ActionOrigin::forConcern(7, 2, QStringLiteral("owner"),
                                                     Channel::kPet, Channel::kDefaultSession);
        QVERIFY(origin.isValid());
        const QString key = origin.actionKey(QStringLiteral("contact"));
        QCOMPARE(key, QStringLiteral("concern:7:2:contact"));
        const QJsonObject payload{{QStringLiteral("message"), QStringLiteral("在忙吗")},
                                  {QStringLiteral("reason"), QStringLiteral("自己想到")}};
        const QJsonObject proposal{{QStringLiteral("origin"), origin.toJson()},
                                   {QStringLiteral("kind"), QStringLiteral("contact")},
                                   {QStringLiteral("payload"), payload},
                                   {QStringLiteral("claims"), QJsonArray{}},
                                   {QStringLiteral("expires_at"), QJsonValue(4102444800LL)}};
        {
            MemoryManager db(path);
            // 非法输入：伪造身份/缺失 target/不支持的 kind 组合一律拒绝。
            auto invalidOrigin = proposal;
            invalidOrigin[QStringLiteral("origin")] =
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("concern")},
                            {QStringLiteral("id"), -1},
                            {QStringLiteral("version"), 1}};
            QString invalidError;
            db.persistActionProposal(invalidOrigin, &invalidError);
            QCOMPARE(invalidError, QStringLiteral("invalid_origin_id"));
            auto missingTarget = proposal;
            missingTarget[QStringLiteral("origin")] = origin.toJson();
            QJsonObject noTargetJson = origin.toJson();
            noTargetJson.remove(QStringLiteral("target_channel"));
            noTargetJson.remove(QStringLiteral("target_session"));
            missingTarget[QStringLiteral("origin")] = noTargetJson;
            QString targetError;
            db.persistActionProposal(missingTarget, &targetError);
            QCOMPARE(targetError, QStringLiteral("invalid_target"));
            auto wrongKind = proposal;
            wrongKind[QStringLiteral("kind")] = QStringLiteral("activity_share");
            QString kindError;
            db.persistActionProposal(wrongKind, &kindError);
            QCOMPARE(kindError, QStringLiteral("origin_kind_not_allowed"));

            // 首次落库：state=proposed，semantic_status 为空（服务端尚未核验），target 来自 origin。
            QString error;
            const auto stored = db.persistActionProposal(proposal, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(stored.value(QStringLiteral("action_key")).toString(), key);
            QCOMPARE(stored.value(QStringLiteral("state")).toString(), QStringLiteral("proposed"));
            QCOMPARE(stored.value(QStringLiteral("semantic_status")).toString(), QString());
            QCOMPARE(stored.value(QStringLiteral("origin")).toObject()
                         .value(QStringLiteral("id")).toInteger(), qlonglong(7));
            QCOMPARE(stored.value(QStringLiteral("target")).toObject()
                         .value(QStringLiteral("channel")).toString(), QString(Channel::kPet));
            QCOMPARE(stored.value(QStringLiteral("target")).toObject()
                         .value(QStringLiteral("session_id")).toString(),
                     QString(Channel::kDefaultSession));

            // 同 key 同内容幂等：返回已有行，绝不覆盖成新 payload。
            QString againError;
            const auto again = db.persistActionProposal(proposal, &againError);
            QVERIFY2(againError.isEmpty(), qPrintable(againError));
            QCOMPARE(again.value(QStringLiteral("idempotent")).toBool(), true);
            QCOMPARE(again.value(QStringLiteral("id")).toInteger(),
                     stored.value(QStringLiteral("id")).toInteger());
            // 同 key 不同 payload → conflict，库里仍是第一次的正文。
            auto changed = proposal;
            changed[QStringLiteral("payload")] =
                QJsonObject{{QStringLiteral("message"), QStringLiteral("换一句")}};
            QString conflictError;
            const auto conflict = db.persistActionProposal(changed, &conflictError);
            QCOMPARE(conflictError, QStringLiteral("conflict"));
            QCOMPARE(conflict.value(QStringLiteral("error")).toString(), QStringLiteral("conflict"));
            QCOMPARE(db.actionProposal(key).value(QStringLiteral("payload")).toObject()
                         .value(QStringLiteral("message")).toString(), QStringLiteral("在忙吗"));

            // 调用方伪造派生字段一律拒绝。
            auto forgedKey = proposal;
            forgedKey[QStringLiteral("action_key")] = QStringLiteral("concern:7:3:contact");
            QString forgedKeyError;
            db.persistActionProposal(forgedKey, &forgedKeyError);
            QCOMPARE(forgedKeyError, QStringLiteral("action_key_mismatch"));
            auto forgedTarget = proposal;
            forgedTarget[QStringLiteral("target")] =
                QJsonObject{{QStringLiteral("channel"), Channel::kQQGroup},
                            {QStringLiteral("session_id"), QStringLiteral("group:1")}};
            QString forgedTargetError;
            db.persistActionProposal(forgedTarget, &forgedTargetError);
            QCOMPARE(forgedTargetError, QStringLiteral("target_mismatch"));
            auto forgedSemantic = proposal;
            forgedSemantic[QStringLiteral("semantic_status")] = QStringLiteral("entailed");
            QString forgedSemanticError;
            db.persistActionProposal(forgedSemantic, &forgedSemanticError);
            QCOMPARE(forgedSemanticError, QStringLiteral("semantic_status_forbidden"));

            // CAS：R12-2c-2 起旧入口不再允许直接推进 verified（必须先写内容绑定）。
            QString casError;
            QVERIFY(!db.updateActionProposalState(key, QStringLiteral("proposed"),
                                                  QStringLiteral("verified"),
                                                  QStringLiteral("entailed"), {}, &casError));
            QCOMPARE(casError, QStringLiteral("verified_binding_required"));
            // 唯一入口：markActionProposalVerified 写入 payload/claims 指纹、request_id、
            // verified_at 与提案生成时的 fence epoch 快照。
            QVERIFY(db.markActionProposalVerified(key, QStringLiteral("entailed"),
                                                  QStringLiteral("req-memory-manager"),
                                                  &casError));
            const auto verifiedRow = db.actionProposal(key);
            QCOMPARE(verifiedRow.value(QStringLiteral("semantic_status")).toString(),
                     QStringLiteral("entailed"));
            QCOMPARE(verifiedRow.value(QStringLiteral("state")).toString(),
                     QStringLiteral("verified"));
            QCOMPARE(verifiedRow.value(QStringLiteral("verified_request_id")).toString(),
                     QStringLiteral("req-memory-manager"));
            QCOMPARE(verifiedRow.value(QStringLiteral("verified_payload_hash")).toString().size(), 16);
            QCOMPARE(verifiedRow.value(QStringLiteral("verified_claims_hash")).toString().size(), 16);
            QVERIFY(verifiedRow.value(QStringLiteral("verified_at")).toInteger() > 0);
            QVERIFY(verifiedRow.value(QStringLiteral("action_context_epoch")).toInteger() >= 0);
            // 重复标记核验通过是幂等的（不覆盖首次绑定）。
            QVERIFY(db.markActionProposalVerified(key, QStringLiteral("entailed"),
                                                  QStringLiteral("req-memory-manager"),
                                                  &casError));
            // 前态不匹配（已经从 verified 推进过）仍然拒绝。
            QVERIFY(!db.updateActionProposalState(key, QStringLiteral("proposed"),
                                                  QStringLiteral("withheld"),
                                                  QStringLiteral("insufficient"), {}, &casError));
            QCOMPARE(casError, QStringLiteral("cas_conflict"));
            // committed 只能由 markProposalCommitted 写；commit key 必须非空。
            QString commitError;
            QVERIFY(!db.markProposalCommitted(key, QString(), &commitError));
            QCOMPARE(commitError, QStringLiteral("invalid_committed_action_key"));
            QVERIFY(db.markProposalCommitted(
                key, QStringLiteral("reflection:7:2:contact"), &commitError));
            const auto committed = db.actionProposal(key);
            QCOMPARE(committed.value(QStringLiteral("state")).toString(),
                     QStringLiteral("committed"));
            QCOMPARE(committed.value(QStringLiteral("committed_action_key")).toString(),
                     QStringLiteral("reflection:7:2:contact"));
            QVERIFY(!db.markProposalCommitted(key, QStringLiteral("other"), &commitError));

            // 过期：proposed/verified 且明确设置过期时间的提案 → expired；重复调用幂等。
            const auto expiringOrigin = ActionOrigin::forReflection(
                9, 1, QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession);
            const QString expiringKey = expiringOrigin.actionKey(QStringLiteral("contact"));
            const QJsonObject expiringProposal{
                {QStringLiteral("origin"), expiringOrigin.toJson()},
                {QStringLiteral("kind"), QStringLiteral("contact")},
                {QStringLiteral("payload"),
                 QJsonObject{{QStringLiteral("message"), QStringLiteral("还记得吗")}}},
                {QStringLiteral("expires_at"), QJsonValue(1000LL)}};
            QString expiringError;
            QCOMPARE(db.persistActionProposal(expiringProposal, &expiringError)
                         .value(QStringLiteral("state")).toString(), QStringLiteral("proposed"));
            QCOMPARE(db.expireActionProposals(2000), 1);
            QCOMPARE(db.expireActionProposals(2000), 0);
            QCOMPARE(db.actionProposal(expiringKey).value(QStringLiteral("state")).toString(),
                     QStringLiteral("expired"));
            // expires_at=0 表示不过期：上面的 committed 行不受影响。
            QCOMPARE(db.actionProposal(key).value(QStringLiteral("state")).toString(),
                     QStringLiteral("committed"));
        }
        // 重启后可读，状态/来源版本保持。
        {
            MemoryManager reopened(path);
            const auto row = reopened.actionProposal(key);
            QCOMPARE(row.value(QStringLiteral("state")).toString(), QStringLiteral("committed"));
            QCOMPARE(row.value(QStringLiteral("origin_version")).toInt(), 2);
            QCOMPARE(row.value(QStringLiteral("origin_ref")).toString(),
                     QStringLiteral("concern/7"));
        }
    }

    // R12-1a：四类来源的失败计数/退避/源状态持久化（各来源一次 -> 计数递增）。
    void reviewFailureBackoffAcrossSources()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(QByteArray(
            R"({"cognition":{"review_backoff_seconds":[120,300,900],"max_review_failures":4}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("review-failure.db"));
        MemoryManager db(path);
        const qlonglong now = QDateTime::currentSecsSinceEpoch();

        // reflection：领取后失败 -> deferred + 120s；再领取失败 -> 300s（version 增长不重置计数）。
        const auto reflectionTurn = seedReflectionSource(db, QStringLiteral("failure-reflection"));
        QVERIFY(reflectionTurn > 0);
        const QString reflectionRef = QStringLiteral("reflection/%1").arg(reflectionTurn);
        auto claim = db.claimPrivateReflection(12, 60);
        QCOMPARE(claim.value("id").toInteger(), reflectionTurn);
        const int reflectionVersion = claim.value("version").toInt();
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), reflectionRef,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        auto failure = db.reviewFailure(QStringLiteral("reflection"), reflectionRef);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        const qlonglong reflectionRetry = failure.value("next_retry_at").toInteger();
        QVERIFY(withinSeconds(reflectionRetry - now, 120, 10));
        QCOMPARE(failure.value("suspended_until").toInteger(), qlonglong(0));
        {
            RawDb raw(path, "failure-reflection-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(reflectionTurn)).toString(),
                     QStringLiteral("deferred"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT eligible_at FROM private_reflection WHERE turn_id=%1").arg(reflectionTurn)).toLongLong(),
                     reflectionRetry);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT claimed_by FROM private_reflection WHERE turn_id=%1").arg(reflectionTurn)).isNull(), true);
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-reflection-clock"), 400));
        claim = db.claimPrivateReflection(12, 60);
        QCOMPARE(claim.value("version").toInt(), reflectionVersion + 1);
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), reflectionRef,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        failure = db.reviewFailure(QStringLiteral("reflection"), reflectionRef);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 2);
        QVERIFY(withinSeconds(failure.value("next_retry_at").toInteger() - now, 300, 10));

        // concern：到期回顾失败 -> consider + next_check_at=next_retry；内容修订重置计数。
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-concern-prep"), 120));
        const auto concernTurn = seedOpenConcern(
            db, concernTurnMeta(QStringLiteral("failure-concern")),
            QStringLiteral("周末想去看一场电影"), QStringLiteral("电影好不好看"),
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("电影")},
                        {QStringLiteral("watch"), QStringLiteral("评价如何")},
                        {QStringLiteral("next_check_after_minutes"), 1}});
        QVERIFY(concernTurn > 0);
        const QString concernRef = QStringLiteral("concern/%1").arg(concernTurn);
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-concern-clock-0"), 120));
        auto concernClaim = db.claimDueConcern(12, 60);
        QCOMPARE(concernClaim.value("id").toInteger(), concernTurn);
        const int concernVersion = concernClaim.value("version").toInt();
        QVERIFY(db.recordReviewFailure(QStringLiteral("concern"), concernRef,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        auto concernFailure = db.reviewFailure(QStringLiteral("concern"), concernRef);
        QCOMPARE(concernFailure.value("consecutive_failures").toInt(), 1);
        const qlonglong concernRetry = concernFailure.value("next_retry_at").toInteger();
        QVERIFY(withinSeconds(concernRetry - now, 120, 10));
        {
            RawDb raw(path, "failure-concern-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(concernTurn)).toString(),
                     QStringLiteral("consider"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_check_at FROM private_reflection WHERE turn_id=%1").arg(concernTurn)).toLongLong(),
                     concernRetry);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT eligible_at FROM private_reflection WHERE turn_id=%1").arg(concernTurn)).toLongLong(),
                     concernRetry);
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-concern-clock-1"), 400));
        concernClaim = db.claimDueConcern(12, 60);
        QCOMPARE(concernClaim.value("version").toInt(), concernVersion + 1);
        QVERIFY(db.recordReviewFailure(QStringLiteral("concern"), concernRef,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("concern"), concernRef)
                     .value("consecutive_failures").toInt(), 2);
        // 关注内容实质修订（note/topic 变化）-> 计数重置为 0 后再计本次。
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-concern-clock-2"), 400));
        {
            RawDb raw(path, "failure-concern-revise");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET note='要不要约她一起看',topic='新安排' "
                "WHERE turn_id=%1").arg(concernTurn)));
        }
        concernClaim = db.claimDueConcern(12, 60);
        QVERIFY(!concernClaim.isEmpty());
        QVERIFY(db.recordReviewFailure(QStringLiteral("concern"), concernRef,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("concern"), concernRef)
                     .value("consecutive_failures").toInt(), 1);

        // activity：chat_history 的 review_after/deferred_reason/thought_version 同步更新。
        QVERIFY(seedLegacyThought(db, QStringLiteral("雨声")) > 0);
        const auto thought = db.nextThought(true);
        const qlonglong activityId = thought.value("id").toInteger();
        const int activityVersion = thought.value("version").toInt();
        QVERIFY(activityId > 0);
        const QString activityRef = QStringLiteral("thought/%1").arg(activityId);
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("failure-activity-clock-0")));
        QVERIFY(db.claimThoughtReview(activityId, activityVersion, 12, 60));
        QVERIFY(db.recordReviewFailure(QStringLiteral("activity"), activityRef,
                                       QStringLiteral("generate"), QStringLiteral("truncated"), true));
        auto activityFailure = db.reviewFailure(QStringLiteral("activity"), activityRef);
        QCOMPARE(activityFailure.value("consecutive_failures").toInt(), 1);
        const qlonglong activityRetry = activityFailure.value("next_retry_at").toInteger();
        QVERIFY(withinSeconds(activityRetry - now, 120, 10));
        {
            RawDb raw(path, "failure-activity-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT review_after FROM chat_history WHERE id=%1").arg(activityId)).toLongLong(),
                     activityRetry);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT thought_version FROM chat_history WHERE id=%1").arg(activityId)).toInt(),
                     activityVersion + 2);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT json_extract(thought_result,'$.deferred_reason') "
                         "FROM chat_history WHERE id=%1").arg(activityId)).toString(),
                     QStringLiteral("truncated"));
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-activity-clock-1"), 400));
        QVERIFY(db.claimThoughtReview(activityId, activityVersion + 2, 12, 60));
        QVERIFY(db.recordReviewFailure(QStringLiteral("activity"), activityRef,
                                       QStringLiteral("generate"), QStringLiteral("truncated"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("activity"), activityRef)
                     .value("consecutive_failures").toInt(), 2);

        // open_wake：running -> deferred/lease 清空，调度状态推到 next_retry。
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-open-wake-prep"), 120));
        QString wakeReason;
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        auto wake = db.claimOpenWake(12, 60, 12, 100, 4, &wakeReason);
        QVERIFY2(!wake.isEmpty(), qPrintable(wakeReason));
        const qlonglong wakeId = wake.value("id").toInteger();
        const int wakeVersion = wake.value("version").toInt();
        const QString wakeRef = QStringLiteral("wake/%1:%2").arg(wakeId).arg(wakeVersion);
        QVERIFY(db.recordReviewFailure(QStringLiteral("open_wake"), wakeRef,
                                       QStringLiteral("generate"), QStringLiteral("json_parse"), true));
        failure = db.reviewFailure(QStringLiteral("open_wake"), wakeRef);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        const qlonglong wakeRetry = failure.value("next_retry_at").toInteger();
        QVERIFY(withinSeconds(wakeRetry - now, 120, 10));
        {
            RawDb raw(path, "failure-open-wake-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(wakeId)).toString(),
                     QStringLiteral("deferred"));
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT lease_until FROM open_wake WHERE id=%1").arg(wakeId)).isNull());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_due_at FROM open_wake WHERE id=%1").arg(wakeId)).toLongLong(),
                     wakeRetry);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT next_due_at FROM open_wake_state WHERE scope_key='owner'")).toLongLong(),
                     wakeRetry);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT consecutive_failures FROM open_wake_state WHERE scope_key='owner'")).toInt(), 1);
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-open-wake-clock"), 400));
        wake = db.claimOpenWake(12, 60, 12, 100, 4, &wakeReason);
        QVERIFY2(!wake.isEmpty(), qPrintable(wakeReason));
        const QString wakeRef2 = QStringLiteral("wake/%1:%2")
            .arg(wake.value("id").toInteger()).arg(wake.value("version").toInt());
        QVERIFY(db.recordReviewFailure(QStringLiteral("open_wake"), wakeRef2,
                                       QStringLiteral("generate"), QStringLiteral("json_parse"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("open_wake"), wakeRef2)
                     .value("consecutive_failures").toInt(), 2);
        {
            RawDb raw(path, "failure-open-wake-verify-2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT consecutive_failures FROM open_wake_state WHERE scope_key='owner'")).toInt(), 2);
        }
    }

    // R12-1a：来源实质修订重置连续失败计数；正常领取的 version 增长不重置。
    void reviewFailureContentRevisionResetsCount()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(QByteArray(
            R"({"cognition":{"review_backoff_seconds":[120,300,900],"max_review_failures":4}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("revision-reset.db"));
        MemoryManager db(path);
        const auto turn = seedReflectionSource(db, QStringLiteral("revision-reset-source"));
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("reflection/%1").arg(turn);
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                     .value("consecutive_failures").toInt(), 1);
        // 正常重新领取：version 从 1 -> 2，计数应继续累计而不是被当作修订。
        QVERIFY(advanceReviewClock(path, QStringLiteral("revision-reset-clock-1"), 400));
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                     .value("consecutive_failures").toInt(), 2);
        // 来源内容实质修订：note/topic 变化 -> 计数重置后再计本次（1）。
        QVERIFY(advanceReviewClock(path, QStringLiteral("revision-reset-clock-2"), 400));
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        {
            RawDb raw(path, "revision-reset-revise");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET note='换成另一件在意的事',topic='新话题' "
                "WHERE turn_id=%1").arg(turn)));
        }
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        const auto failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        QVERIFY(withinSeconds(failure.value("next_retry_at").toInteger()
                                  - QDateTime::currentSecsSinceEpoch(), 120, 10));
        QVERIFY(failure.value("last_stage").toString() == QStringLiteral("generate"));
    }

    // R12-1a：策略/权限拦截与用户主动让出 countsAsFailure=false：只记 stage/reason，不退避。
    void reviewFailureNonCountingDoesNotBackOff()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(QByteArray(
            R"({"cognition":{"review_backoff_seconds":[120,300,900],"max_review_failures":4}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("non-counting.db"));
        MemoryManager db(path);
        const auto turn = seedReflectionSource(db, QStringLiteral("non-counting-source"));
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("reflection/%1").arg(turn);
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        qlonglong eligibleBefore = 0;
        {
            RawDb raw(path, "non-counting-before");
            QVERIFY(raw.open());
            eligibleBefore = raw.scalar(QStringLiteral(
                "SELECT eligible_at FROM private_reflection WHERE turn_id=%1").arg(turn)).toLongLong();
        }
        // 没有失败行时：counts=false 只落事实，计数为 0，不写源状态。
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("verify"), QStringLiteral("permission_denied"),
                                       false));
        auto failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 0);
        QCOMPARE(failure.value("next_retry_at").toInteger(), qlonglong(0));
        QCOMPARE(failure.value("suspended_until").toInteger(), qlonglong(0));
        QCOMPARE(failure.value("last_stage").toString(), QStringLiteral("verify"));
        QCOMPARE(failure.value("last_reason").toString(), QStringLiteral("permission_denied"));
        {
            RawDb raw(path, "non-counting-after");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn)).toString(),
                     QStringLiteral("pending"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT eligible_at FROM private_reflection WHERE turn_id=%1").arg(turn)).toLongLong(),
                     eligibleBefore);
        }
        // 真正失败一次：从 0 计到 1，并写回 deferred/120s。
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        const qlonglong retry = failure.value("next_retry_at").toInteger();
        QVERIFY(withinSeconds(retry - QDateTime::currentSecsSinceEpoch(), 120, 10));
        // 再记一次 counts=false：计数/退避不变，只更新 stage/reason。
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("yield"), QStringLiteral("user_interrupt"),
                                       false));
        failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        QCOMPARE(failure.value("next_retry_at").toInteger(), retry);
        QCOMPARE(failure.value("last_stage").toString(), QStringLiteral("yield"));
        QCOMPARE(failure.value("last_reason").toString(), QStringLiteral("user_interrupt"));
        // 活动来源的 yield 同样只记录事实，不改 review_after/version。
        QVERIFY(seedLegacyThought(db, QStringLiteral("写作")) > 0);
        const auto thought = db.nextThought(true);
        const qlonglong activityId = thought.value("id").toInteger();
        const int activityVersion = thought.value("version").toInt();
        QVERIFY(activityId > 0);
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("non-counting-activity-clock")));
        QVERIFY(db.claimThoughtReview(activityId, activityVersion, 12, 60));
        const int claimedVersion = activityVersion + 1;
        QVERIFY(db.recordReviewFailure(QStringLiteral("activity"),
                                       QStringLiteral("thought/%1").arg(activityId),
                                       QStringLiteral("yield"), QStringLiteral("user_interrupt"),
                                       false));
        QVERIFY(db.reviewFailure(QStringLiteral("activity"),
                                 QStringLiteral("thought/%1").arg(activityId)).value("consecutive_failures").toInt() == 0);
        {
            RawDb raw(path, "non-counting-activity-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT thought_version FROM chat_history WHERE id=%1").arg(activityId)).toInt(),
                     claimedVersion);
        }
    }

    // R12-1a：连续失败达到上限 -> suspended(86400)；到期后允许一次恢复，并允许超过 24h 窗口重领。
    void reviewFailureSuspendedRecoversBeyondWindow()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(QByteArray(
            R"({"cognition":{"review_backoff_seconds":[120,300,900],"max_review_failures":4}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("suspended.db"));
        MemoryManager db(path);
        const auto turn = seedReflectionSource(db, QStringLiteral("suspended-source"));
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("reflection/%1").arg(turn);
        for (int attempt = 1; attempt <= 4; ++attempt) {
            const auto claim = db.claimPrivateReflection(12, 60);
            QVERIFY2(!claim.isEmpty(), qPrintable(QStringLiteral("claim %1").arg(attempt)));
            QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                           QStringLiteral("generate"),
                                           QStringLiteral("network_error"), true));
            const auto failure = db.reviewFailure(QStringLiteral("reflection"), ref);
            QCOMPARE(failure.value("consecutive_failures").toInt(), attempt);
            if (attempt < 4) {
                // backoff 序列 120/300/900；至少拨过当前档位，才能进入下一次领取。
                QVERIFY(advanceReviewClock(path, QStringLiteral("suspended-clock-%1").arg(attempt),
                                           1200));
            }
        }
        auto failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 4);
        const qlonglong suspended = failure.value("suspended_until").toInteger();
        QVERIFY(withinSeconds(suspended - QDateTime::currentSecsSinceEpoch(), 86400, 10));
        QCOMPARE(failure.value("next_retry_at").toInteger(), suspended);
        // 暂停期内不得重领。
        QVERIFY(db.claimPrivateReflection(12, 60).isEmpty());
        // 把来源拨到 24h 窗口之外：正常情况下新来源已不可领取，失败恢复路径应允许重领。
        {
            RawDb raw(path, "suspended-backdate");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE cognitive_source SET reported=datetime('now','-2 day') "
                "WHERE event=(SELECT cognitive_event FROM chat_history WHERE id=%1)").arg(turn)));
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("suspended-clock-recover"), 86401));
        const auto recoveredClaim = db.claimPrivateReflection(12, 60);
        QVERIFY2(!recoveredClaim.isEmpty(), "失败任务在暂停到期后应可重领（即使源超出 24h）");
        failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 0);   // 恢复：计数清零
        QCOMPARE(failure.value("suspended_until").toInteger(), qlonglong(0));
        QCOMPARE(failure.value("last_reason").toString(), QStringLiteral("network_error")); // 保留
        // 恢复后再次失败按 1,2,... 重新累计。
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        failure = db.reviewFailure(QStringLiteral("reflection"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        QVERIFY(withinSeconds(failure.value("next_retry_at").toInteger()
                                  - QDateTime::currentSecsSinceEpoch(), 120, 10));
    }

    // R12-1a：源已删除/已终态时失败行被清理且返回可审计错误码，不静默残留。
    void reviewFailureDeletedSourceIsAudited()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("deleted-source.db"));
        MemoryManager db(path);
        const auto turn = seedReflectionSource(db, QStringLiteral("deleted-source"));
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("reflection/%1").arg(turn);
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QVERIFY(!db.reviewFailure(QStringLiteral("reflection"), ref).isEmpty());
        // 删除来源轮次（既有触发器会级联删 private_reflection）。
        QVERIFY(db.deleteTurnByID(turn));
        MessageCapture capture;
        QString error;
        QVERIFY(!db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                        QStringLiteral("generate"), QStringLiteral("network_error"),
                                        true, &error));
        QCOMPARE(error, QStringLiteral("source_missing"));
        QVERIFY(db.reviewFailure(QStringLiteral("reflection"), ref).isEmpty());
        bool sawAudit = false;
        for (const auto &line : capture.lines())
            if (line.message.contains(QStringLiteral("[Review] failure_dropped"))) sawAudit = true;
        QVERIFY(sawAudit);
    }

    // R12-1a：存储写入失败不报成功，且事务回滚不留半写状态。
    void reviewFailureWriteFailureRollsBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("write-failure.db"));
        MemoryManager db(path);
        QVERIFY(seedLegacyThought(db, QStringLiteral("写作")) > 0);
        const auto thought = db.nextThought(true);
        const qlonglong activityId = thought.value("id").toInteger();
        QVERIFY(activityId > 0);
        {
            // 模拟存储层故障：迁移后表被破坏。
            RawDb raw(path, "write-failure-broken");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("DROP TABLE review_failure")));
        }
        QString error;
        QVERIFY(!db.recordReviewFailure(QStringLiteral("activity"),
                                        QStringLiteral("thought/%1").arg(activityId),
                                        QStringLiteral("generate"), QStringLiteral("db_error"),
                                        true, &error));
        QCOMPARE(error, QStringLiteral("db_error"));
        {
            RawDb raw(path, "write-failure-verify");
            QVERIFY(raw.open());
            // seedLegacyThought 显式把新行的 review_after 初始化为 0；失败回滚后应保持 0。
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT review_after FROM chat_history WHERE id=%1").arg(activityId)).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT thought_version FROM chat_history WHERE id=%1").arg(activityId)).toInt(), 0);
        }
    }

    // R12-1a：管理员重置（清行 + 源立即变为可领取）与成功后的 clear。
    void reviewFailureResetAndClear()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(QByteArray(
            R"({"cognition":{"review_backoff_seconds":[120,300,900],"max_review_failures":4}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("reset.db"));
        MemoryManager db(path);
        const auto turn = seedReflectionSource(db, QStringLiteral("reset-source"));
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("reflection/%1").arg(turn);
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                     .value("consecutive_failures").toInt(), 1);
        // 管理员重置：清行并把源 eligible_at 拨到 now（deferred 状态仍可被 claim 重新领取）。
        QVERIFY(db.resetReviewFailure(QStringLiteral("reflection"), ref));
        QVERIFY(db.reviewFailure(QStringLiteral("reflection"), ref).isEmpty());
        {
            RawDb raw(path, "reset-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn)).toString(),
                     QStringLiteral("deferred"));
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT eligible_at FROM private_reflection WHERE turn_id=%1").arg(turn)).toLongLong()
                    <= QDateTime::currentSecsSinceEpoch());
        }
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("reset-clock")));
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        // 成功提交后由调用方 clear；重复 clear 幂等。
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"), true));
        QVERIFY(db.clearReviewFailure(QStringLiteral("reflection"), ref));
        QVERIFY(db.reviewFailure(QStringLiteral("reflection"), ref).isEmpty());
        QVERIFY(db.clearReviewFailure(QStringLiteral("reflection"), ref));
    }

    // R12-1a：OpenWake 初始排期、每日独立预算取整与共享总量。
    void openWakeScheduleAndBudgetRounding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("open-wake-budget.db"));
        MemoryManager db(path);
        QString error;
        const qlonglong now = QDateTime::currentSecsSinceEpoch();
        QVERIFY2(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 3600, &error),
                 qPrintable(error));
        auto state = db.openWakeState();
        QVERIFY(!state.isEmpty());
        QVERIFY(withinSeconds(state.value("next_due_at").toInteger() - now, 3600, 10));
        // 幂等：已存在调度行时再次 ensure 不改变 due。
        const qlonglong originalDue = state.value("next_due_at").toInteger();
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0, &error));
        QCOMPARE(db.openWakeState().value("next_due_at").toInteger(), originalDue);
        QVERIFY(makeOpenWakeDue(path, "ow-budget-due"));
        // daily=3, shared=30% -> cap=max(1,0)=1；首个领取成功。
        QString reason;
        auto claimed = db.claimOpenWake(3, 60, 12, 30, 4, &reason);
        QVERIFY2(!claimed.isEmpty(), qPrintable(reason));
        QCOMPARE(claimed.value("state").toString(), QStringLiteral("running"));
        QCOMPARE(claimed.value("version").toInt(), 1);
        QVERIFY(claimed.value("run_id").toString().size() >= 32);
        QCOMPARE(db.openWakeState().value("non_open_since_due").toInt(), 0);
        QVERIFY(db.finishOpenWake(claimed.value("id").toInteger(), claimed.value("version").toInt(),
                                  QStringLiteral("rest"), QStringLiteral("先把这一阵的光收起来"),
                                  {}, 30, &error));
        auto budget = db.reviewBudgetUsage();
        QCOMPARE(budget.value("open_wake").toInt(), 1);
        QVERIFY(budget.value("by_source").toObject().contains(QStringLiteral("open_wake")));
        // 冷却过去但独立预算已用完（cap=1）-> budget。
        QVERIFY(makeOpenWakeDue(path, "ow-budget-due-2"));
        QVERIFY(advanceReviewClock(path, "ow-budget-clock", 120));
        QVERIFY(db.claimOpenWake(3, 60, 12, 30, 4, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("budget"));
        // daily=1：取整至少 1 次机会（cap=min(12,min(1,max(1,0)))=1）。
        {
            const auto path2 = dir.filePath(QStringLiteral("open-wake-daily1.db"));
            MemoryManager db2(path2);
            QVERIFY(db2.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            auto dailyOne = db2.claimOpenWake(1, 60, 12, 30, 4, &reason);
            QVERIFY2(!dailyOne.isEmpty(), qPrintable(reason));
            QVERIFY(db2.finishOpenWake(dailyOne.value("id").toInteger(),
                                       dailyOne.value("version").toInt(),
                                       QStringLiteral("rest"), QString(), {}, 5));
        }
        // cap 12：daily=100, 30% -> 30，再被 openWakeDailyLimit=12 截断。
        {
            const auto path3 = dir.filePath(QStringLiteral("open-wake-cap12.db"));
            MemoryManager db3(path3);
            QVERIFY(db3.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            for (int index = 0; index < 12; ++index) {
                if (index > 0) {
                    QVERIFY(makeOpenWakeDue(path3, QStringLiteral("ow-cap12-due-%1").arg(index)));
                    QVERIFY(rewindReviewBudgetClock(path3, QStringLiteral("ow-cap12-clock-%1").arg(index)));
                }
                const auto claim = db3.claimOpenWake(100, 60, 12, 30, 4, &reason);
                QVERIFY2(!claim.isEmpty(),
                         qPrintable(QStringLiteral("cap12 claim %1 reason %2").arg(index).arg(reason)));
                QVERIFY(db3.finishOpenWake(claim.value("id").toInteger(),
                                           claim.value("version").toInt(),
                                           QStringLiteral("reflect"),
                                           QStringLiteral("第 %1 次回顾").arg(index), {}, 5));
            }
            QVERIFY(makeOpenWakeDue(path3, "ow-cap12-due-block"));
            QVERIFY(rewindReviewBudgetClock(path3, "ow-cap12-clock-block"));
            QVERIFY(db3.claimOpenWake(100, 60, 12, 30, 4, &reason).isEmpty());
            QCOMPARE(reason, QStringLiteral("budget"));
            QCOMPARE(db3.reviewBudgetUsage().value("open_wake").toInt(), 12);
        }
    }

    // R12-1a：OpenWake 拒绝 dailyLimit=0 / 独立上限 0；共享预算与冷却生效。
    void openWakeBudgetGatesAndCooldown()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("open-wake-gates.db"));
        MemoryManager db(path);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        QString reason;
        // dailyLimit=0 禁止所有后台认知。
        QVERIFY(db.claimOpenWake(0, 60, 12, 100, 4, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("budget"));
        // 独立上限为 0 -> 无机会。
        QVERIFY(db.claimOpenWake(12, 60, 0, 100, 4, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("budget"));
        auto first = db.claimOpenWake(12, 300, 12, 100, 4, &reason);
        QVERIFY2(!first.isEmpty(), qPrintable(reason));
        QVERIFY(db.finishOpenWake(first.value("id").toInteger(), first.value("version").toInt(),
                                  QStringLiteral("rest"), QString(), {}, 5));
        // 共享冷却：300s 未过 -> cooldown。
        QVERIFY(makeOpenWakeDue(path, "ow-gates-due"));
        QVERIFY(db.claimOpenWake(12, 300, 12, 100, 4, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("cooldown"));
        // 共享每日总量：open wake 占用后 dailyLimit=1 时新来源回顾被总量挡住。
        {
            const auto path2 = dir.filePath(QStringLiteral("open-wake-shared.db"));
            MemoryManager db2(path2);
            QVERIFY(db2.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            auto wake = db2.claimOpenWake(1, 60, 12, 100, 4, &reason);
            QVERIFY2(!wake.isEmpty(), qPrintable(reason));
            QVERIFY(db2.finishOpenWake(wake.value("id").toInteger(), wake.value("version").toInt(),
                                       QStringLiteral("rest"), QString(), {}, 5));
            const auto turn = seedReflectionSource(db2, QStringLiteral("ow-shared-source"));
            QVERIFY(turn > 0);
            QVERIFY(rewindReviewCooldown(path2, QStringLiteral("ow-shared-clock")));
            QString reflectionReason;
            QVERIFY(db2.claimPrivateReflection(1, 60, &reflectionReason).isEmpty());
            QCOMPARE(reflectionReason, QStringLiteral("daily_limit"));
        }
    }

    // R12-1a：running 唯一/租约；启动恢复只恢复一次；过期租约可被下一次领取就地打断。
    void openWakeRunningLeaseAndRecovery()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("open-wake-lease.db"));
        MemoryManager db(path);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        QString reason;
        auto first = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!first.isEmpty(), qPrintable(reason));
        const qlonglong firstId = first.value("id").toInteger();
        {
            RawDb raw(path, "ow-lease-first");
            QVERIFY(raw.open());
            const qlonglong lease = raw.scalar(QStringLiteral(
                "SELECT lease_until FROM open_wake WHERE id=%1").arg(firstId)).toLongLong();
            QVERIFY(withinSeconds(lease - QDateTime::currentSecsSinceEpoch(), 1800, 10));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM open_wake WHERE scope_key='owner' AND state='running'")).toInt(), 1);
        }
        // 同一 scope 已有 running（租约有效）-> busy。
        QVERIFY(db.claimOpenWake(12, 60, 12, 100, 4, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("busy"));
        // 租约过期：启动恢复 -> interrupted，下一次调用幂等返回 0。
        {
            RawDb raw(path, "ow-lease-expire");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE open_wake SET lease_until=unixepoch()-7200 WHERE id=%1").arg(firstId)));
        }
        QCOMPARE(db.recoverOpenWakeLeases(1800), 1);
        QCOMPARE(db.recoverOpenWakeLeases(1800), 0);
        {
            RawDb raw(path, "ow-lease-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(firstId)).toString(),
                     QStringLiteral("interrupted"));
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT lease_until FROM open_wake WHERE id=%1").arg(firstId)).isNull());
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT next_due_at FROM open_wake_state WHERE scope_key='owner'")).toLongLong()
                    <= QDateTime::currentSecsSinceEpoch());
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("ow-lease-clock"), 120));
        auto second = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!second.isEmpty(), qPrintable(reason));
        const qlonglong secondId = second.value("id").toInteger();
        // 领取时遇到过期租约：就地标记 interrupted 再插入新 running（唯一索引不冲突）。
        {
            RawDb raw(path, "ow-lease-expire-2");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE open_wake SET lease_until=unixepoch()-7200 WHERE id=%1").arg(secondId)));
        }
        QVERIFY(advanceReviewClock(path, QStringLiteral("ow-lease-clock-2"), 120));
        auto third = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!third.isEmpty(), qPrintable(reason));
        {
            RawDb raw(path, "ow-lease-verify-2");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(secondId)).toString(),
                     QStringLiteral("interrupted"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM open_wake WHERE scope_key='owner' AND state='running'")).toInt(), 1);
        }
    }

    // R12-1a：finish（rest/reflect）CAS、非法 decision/超长 note 拒绝、defer/cancel、recent notes。
    void openWakeFinishDeferCancelAndNotes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("open-wake-lifecycle.db"));
        MemoryManager db(path);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        QString reason;
        QString error;
        auto first = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!first.isEmpty(), qPrintable(reason));
        const qlonglong firstId = first.value("id").toInteger();
        const int firstVersion = first.value("version").toInt();
        // 非法 decision / note 超长：拒绝且不落库（仍是 running）。
        QVERIFY(!db.finishOpenWake(firstId, firstVersion, QStringLiteral("invalid_decision_value"), QString(),
                                   {}, 30, &error));
        QCOMPARE(error, QStringLiteral("invalid_decision"));
        QVERIFY(!db.finishOpenWake(firstId, firstVersion, QStringLiteral("rest"),
                                   QString(241, u'x'), {}, 30, &error));
        QCOMPARE(error, QStringLiteral("invalid_note"));
        {
            RawDb raw(path, "ow-lifecycle-open");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(firstId)).toString(),
                     QStringLiteral("running"));
        }
        // rest 也成功提交，并写 note/decision；next_due 按分钟 clamp。
        QVERIFY2(db.finishOpenWake(firstId, firstVersion, QStringLiteral("rest"),
                                   QStringLiteral("先把手边的光收好"), {}, 10, &error),
                 qPrintable(error));
        {
            RawDb raw(path, "ow-lifecycle-rest");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(firstId)).toString(),
                     QStringLiteral("committed"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT decision FROM open_wake WHERE id=%1").arg(firstId)).toString(),
                     QStringLiteral("rest"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT note FROM open_wake WHERE id=%1").arg(firstId)).toString(),
                     QStringLiteral("先把手边的光收好"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT last_outcome FROM open_wake_state WHERE scope_key='owner'")).toString(),
                     QStringLiteral("rest"));
            QVERIFY(withinSeconds(raw.scalar(QStringLiteral(
                         "SELECT next_due_at FROM open_wake_state WHERE scope_key='owner'")).toLongLong()
                                      - QDateTime::currentSecsSinceEpoch(), 600, 10));
        }
        // 已完成行不能再次 CAS 提交。
        QVERIFY(!db.finishOpenWake(firstId, firstVersion, QStringLiteral("rest"), QString(),
                                   {}, 10, &error));
        // recent notes：只返回 committed 且 note 非空的记录，ref=wake/<id>:<version>。
        auto notes = db.recentOpenWakeNotes(2);
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().toObject().value("ref").toString(),
                 QStringLiteral("wake/%1:%2").arg(firstId).arg(firstVersion));
        QCOMPARE(notes.first().toObject().value("note").toString(),
                 QStringLiteral("先把手边的光收好"));
        QCOMPARE(notes.first().toObject().value("decision").toString(), QStringLiteral("rest"));
        QVERIFY(!notes.first().toObject().value("at").toString().isEmpty());
        // defer：用户插话/关停路径，不增加失败计数，next_due=now。
        QVERIFY(makeOpenWakeDue(path, "ow-lifecycle-due-2"));
        QVERIFY(advanceReviewClock(path, "ow-lifecycle-clock-2", 120));
        auto second = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!second.isEmpty(), qPrintable(reason));
        const qlonglong secondId = second.value("id").toInteger();
        QVERIFY(db.deferOpenWake(secondId, second.value("version").toInt(), 0,
                                 QStringLiteral("user_interrupt")));
        {
            RawDb raw(path, "ow-lifecycle-deferred");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(secondId)).toString(),
                     QStringLiteral("deferred"));
            QVERIFY(raw.scalar(QStringLiteral(
                        "SELECT lease_until FROM open_wake WHERE id=%1").arg(secondId)).isNull());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT consecutive_failures FROM open_wake_state WHERE scope_key='owner'")).toInt(), 0);
        }
        QVERIFY(db.reviewFailure(QStringLiteral("open_wake"),
                                 QStringLiteral("wake/%1:%2").arg(secondId)
                                     .arg(second.value("version").toInt())).isEmpty());
        // reflect：重新领取后提交 reflect，note 进入 recent notes。
        QVERIFY(advanceReviewClock(path, "ow-lifecycle-clock-3", 120));
        auto third = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!third.isEmpty(), qPrintable(reason));
        const qlonglong thirdId = third.value("id").toInteger();
        QVERIFY2(db.finishOpenWake(thirdId, third.value("version").toInt(),
                                   QStringLiteral("reflect"),
                                   QStringLiteral("雨声的段落还能再细一点"), {}, 30, &error),
                 qPrintable(error));
        notes = db.recentOpenWakeNotes(2);
        QCOMPARE(notes.size(), 2);
        QCOMPARE(notes.first().toObject().value("decision").toString(), QStringLiteral("reflect"));
        QCOMPARE(notes.first().toObject().value("ref").toString(),
                 QStringLiteral("wake/%1:%2").arg(thirdId).arg(third.value("version").toInt()));
        QCOMPARE(notes.last().toObject().value("decision").toString(), QStringLiteral("rest"));
        // cancel：终态，不影响失败计数。
        QVERIFY(makeOpenWakeDue(path, "ow-lifecycle-due-4"));
        QVERIFY(advanceReviewClock(path, "ow-lifecycle-clock-4", 120));
        auto fourth = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!fourth.isEmpty(), qPrintable(reason));
        const qlonglong fourthId = fourth.value("id").toInteger();
        QVERIFY(db.cancelOpenWake(fourthId, fourth.value("version").toInt(),
                                  QStringLiteral("shutdown")));
        {
            RawDb raw(path, "ow-lifecycle-cancelled");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(fourthId)).toString(),
                     QStringLiteral("cancelled"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT last_outcome FROM open_wake_state WHERE scope_key='owner'")).toString(),
                     QStringLiteral("cancelled"));
        }
    }

    // R12-1a：公平计数持久化、按"实际成功领取"递增、OpenWake 领取清零、重启不重置。
    void openWakeFairnessCounterPersists()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("open-wake-fairness.db"));
        {
            MemoryManager db(path);
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            const auto turn = seedReflectionSource(db, QStringLiteral("fairness-source"));
            QVERIFY(turn > 0);
            QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
            QCOMPARE(db.openWakeState().value("non_open_since_due").toInt(), 1);
            // 没有候选的 tick 不增加计数。
            QVERIFY(rewindReviewCooldown(path, QStringLiteral("fairness-clock")));
            QVERIFY(db.claimDueConcern(12, 60).isEmpty());
            QCOMPARE(db.openWakeState().value("non_open_since_due").toInt(), 1);
            // 活动领取同样递增（按成功领取的任务计数）。
            QVERIFY(seedLegacyThought(db, QStringLiteral("雨声")) > 0);
            const auto thought = db.nextThought(true);
            QVERIFY(thought.value("id").toInteger() > 0);
            QVERIFY(rewindReviewCooldown(path, QStringLiteral("fairness-clock-activity")));
            QVERIFY(db.claimThoughtReview(thought.value("id").toInteger(),
                                          thought.value("version").toInt(), 12, 60));
            QCOMPARE(db.openWakeState().value("non_open_since_due").toInt(), 2);
        }
        // 重开数据库：计数仍在（持久化，进程重启不清零）。
        {
            MemoryManager db(path);
            QCOMPARE(db.openWakeState().value("non_open_since_due").toInt(), 2);
            QVERIFY(makeOpenWakeDue(path, "fairness-due"));
            QVERIFY(advanceReviewClock(path, "fairness-clock-wake", 120));
            QString reason;
            const auto wake = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
            QVERIFY2(!wake.isEmpty(), qPrintable(reason));
            QCOMPARE(db.openWakeState().value("non_open_since_due").toInt(), 0);
        }
    }

    // R12-1a：internal_activity 只读骨架（可见 activity/<id>，legacy 仍可见，旧接口不变）。
    void internalActivityReadSkeleton()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("internal-activity.db"));
        MemoryManager db(path);
        {
            RawDb raw(path, "internal-activity-insert");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO internal_activity(scope_key,origin_kind,channel,session_id,interest,"
                "status,version,result_json,artifact_ref,review_after,created_at,updated_at) "
                "VALUES('owner','internal','pet','default','把窗边的光写进段落','active',0,'{}','',"
                "0,unixepoch(),unixepoch())")));
        }
        QVERIFY(db.hasActiveInternalActivity());
        QCOMPARE(db.currentActivities().size(), 0);   // 旧接口只读 legacy，不受影响
        auto unified = db.currentActivitiesUnified(8);
        QCOMPARE(unified.size(), 1);
        {
            const auto row = unified.first().toObject();
            QCOMPARE(row.value("origin").toString(), QStringLiteral("internal"));
            QVERIFY(row.value("ref").toString().startsWith(QStringLiteral("activity/")));
            QCOMPARE(row.value("interest").toString(), QStringLiteral("把窗边的光写进段落"));
            QCOMPARE(row.value("status").toString(), QStringLiteral("active"));
        }
        // legacy 活动仍然可见，并带 origin=legacy；旧 currentActivities 返回形状不变。
        QVERIFY(seedLegacyThought(db, QStringLiteral("雨声")) > 0);
        QCOMPARE(db.currentActivities().size(), 1);
        QCOMPARE(db.currentActivities().first().toObject().value("ref").toString().left(8),
                 QStringLiteral("thought/"));
        unified = db.currentActivitiesUnified(8);
        QCOMPARE(unified.size(), 2);
        bool sawLegacy = false, sawInternal = false;
        for (const auto &value : unified) {
            const auto row = value.toObject();
            const auto origin = row.value("origin").toString();
            const auto ref = row.value("ref").toString();
            if (origin == QStringLiteral("legacy") && ref.startsWith(QStringLiteral("thought/")))
                sawLegacy = true;
            if (origin == QStringLiteral("internal") && ref.startsWith(QStringLiteral("activity/")))
                sawInternal = true;
        }
        QVERIFY(sawLegacy);
        QVERIFY(sawInternal);
        // paused 的内部活动仍算"当前活动"。
        {
            RawDb raw(path, "internal-activity-pause");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE internal_activity SET status='paused' WHERE origin_kind='internal'")));
        }
        QVERIFY(db.hasActiveInternalActivity());
        QCOMPARE(db.currentActivitiesUnified(8).size(), 2);
    }

    // R12-1b：两张表的相同整数 id 不能互相串线；引用必须带前缀解析。
    void activityRefDistinguishesSameIntegerId()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-ref.db"));
        MemoryManager db(path);
        // legacy id=1：正常聊天写入的 thought 行。
        QVERIFY(seedLegacyThought(db, QStringLiteral("雨声")) > 0);
        const auto thought = db.nextThought(true);
        QCOMPARE(thought.value("id").toInteger(), qlonglong(1));

        // internal id=1：模拟历史/并发遗留的内部活动（createActivity 的单 active 规则
        // 会拒绝与 legacy 同时 active，这里直接用夹具落行验证身份分离）。
        {
            RawDb raw(path, "activity-ref-internal");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO internal_activity(scope_key,origin_kind,origin_ref,run_id,subject,"
                "channel,session_id,interest,status,version,result_json,artifact_ref,"
                "review_after,created_at,updated_at) "
                "VALUES('owner','foreground','chat/1','run-ref-1','user','pet','default',"
                "'把窗边的光写进段落','active',1,'{\"goal\":\"写一段光\"}','',"
                "0,unixepoch(),unixepoch())")));
        }

        ActivityRef parsed;
        QString parseError;
        QVERIFY(ActivityRef::parse(QStringLiteral("thought/1"), &parsed, &parseError));
        QVERIFY(parsed.kind == ActivityRef::LegacyThought);
        QCOMPARE(parsed.id, qlonglong(1));
        QVERIFY(parsed.ref() == QStringLiteral("thought/1"));
        QCOMPARE(parsed.originKind(), QStringLiteral("legacy"));
        QVERIFY(ActivityRef::parse(QStringLiteral("activity/1"), &parsed, &parseError));
        QVERIFY(parsed.kind == ActivityRef::InternalActivity);
        QCOMPARE(parsed.id, qlonglong(1));
        QVERIFY(parsed.ref() == QStringLiteral("activity/1"));
        QCOMPARE(parsed.originKind(), QStringLiteral("internal"));
        // 裸整数/未知前缀不是身份，必须拒绝。
        QVERIFY(!ActivityRef::parse(QStringLiteral("1"), &parsed, &parseError));
        QCOMPARE(parseError, QStringLiteral("invalid_ref"));
        QVERIFY(!ActivityRef::parse(QString(), &parsed, &parseError));
        QCOMPARE(parseError, QStringLiteral("empty_ref"));
        QVERIFY(!ActivityRef::parse(QStringLiteral("legacy/1"), &parsed, &parseError));
        QCOMPARE(parseError, QStringLiteral("unknown_prefix"));
        QVERIFY(!(ActivityRef::legacy(1) == ActivityRef::internal(1)));
        QVERIFY(ActivityRef::legacy(1).ref() != ActivityRef::internal(1).ref());

        const auto legacyView = db.activityByRef(QStringLiteral("thought/1"));
        const auto internalView = db.activityByRef(QStringLiteral("activity/1"));
        QCOMPARE(legacyView.value("origin").toString(), QStringLiteral("legacy"));
        QCOMPARE(internalView.value("origin").toString(), QStringLiteral("internal"));
        QCOMPARE(legacyView.value("interest").toString(), QStringLiteral("雨声"));
        QCOMPARE(internalView.value("goal").toString(), QStringLiteral("写一段光"));
        QVERIFY(db.activityByRef(QStringLiteral("activity/999")).isEmpty());
        QVERIFY(db.activityByRef(QStringLiteral("1")).isEmpty());

        // 合并列表两条 ref 不同（不允许按整数 id 合并/去重）。
        const auto unified = db.currentActivitiesUnified(8);
        QCOMPARE(unified.size(), 2);
        QSet<QString> refs;
        for (const auto &value : unified)
            refs.insert(value.toObject().value("ref").toString());
        QVERIFY(refs.contains(QStringLiteral("thought/1")));
        QVERIFY(refs.contains(QStringLiteral("activity/1")));

        // 两存储都有 active（历史并发才可能出现）：恢复诊断要报告跨存储冲突，但不删除。
        const auto recovered = db.recoverActivities(QStringLiteral("owner"));
        bool sawConflict = false;
        for (const auto &value : recovered) {
            const auto row = value.toObject();
            if (row.value("kind").toString() == QStringLiteral("cross_store_conflict"))
                sawConflict = true;
        }
        QVERIFY(sawConflict);
        QCOMPARE(db.currentActivitiesUnified(8).size(), 2);
    }

    // R12-1b：createActivity 幂等键、单 active 与事务回滚（不半写）。
    void createActivityIdempotentAndTransactional()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-create.db"));
        QString error;
        {
        MemoryManager db(path);
        const auto first = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("雨声段落"), QStringLiteral("写完这段"), QStringLiteral("foreground"),
            QStringLiteral("chat/7"), QStringLiteral("run-create-1"), &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        QCOMPARE(first.value("origin").toString(), QStringLiteral("internal"));
        QCOMPARE(first.value("status").toString(), QStringLiteral("active"));
        QCOMPARE(first.value("version").toInt(), 1);
        QVERIFY(first.value("ref").toString().startsWith(QStringLiteral("activity/")));
        QVERIFY(first.value("review_after").toInteger()
                > QDateTime::currentSecsSinceEpoch());
        const qlonglong id = first.value("id").toInteger();
        QVERIFY(id > 0);

        // 同 (origin_kind, origin_ref, run_id) 重复创建：返回同一活动，不重复落行。
        const auto again = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("雨声段落"), QStringLiteral("写完这段"), QStringLiteral("foreground"),
            QStringLiteral("chat/7"), QStringLiteral("run-create-1"), &error);
        QVERIFY2(!again.isEmpty(), qPrintable(error));
        QCOMPARE(again.value("ref").toString(), first.value("ref").toString());
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "create-count-1"), 1);

        // 不同 runId 但已有 active：拒绝，且不写任何半成品。
        error.clear();
        const auto blocked = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("另一件事"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/8"), QStringLiteral("run-create-2"), &error);
        QVERIFY(blocked.isEmpty());
        QCOMPARE(error, QStringLiteral("already_active"));
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "create-count-2"), 1);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "create-count-3"), 0);

        // 参数校验：非 owner scope、legacy 来源、空 runId、空兴趣都必须拒绝。
        error.clear();
        QVERIFY(db.createActivity(QStringLiteral("group:1"), Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("x"), QString(),
                                  QStringLiteral("foreground"), QString(), QStringLiteral("r"),
                                  &error).isEmpty());
        QCOMPARE(error, QStringLiteral("invalid_scope"));
        error.clear();
        QVERIFY(db.createActivity(QStringLiteral("owner"), Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("x"), QString(),
                                  QStringLiteral("legacy"), QString(), QStringLiteral("r"),
                                  &error).isEmpty());
        QCOMPARE(error, QStringLiteral("invalid_origin_kind"));
        error.clear();
        QVERIFY(db.createActivity(QStringLiteral("owner"), Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("x"), QString(),
                                  QStringLiteral("foreground"), QString(), QString(),
                                  &error).isEmpty());
        QCOMPARE(error, QStringLiteral("missing_run_id"));
        }

        // 事务失败（DROP internal_activity 模拟存储故障）：不产生活动、不写聊天行。
        QTemporaryDir brokenDir;
        QVERIFY(brokenDir.isValid());
        const auto brokenPath = brokenDir.filePath(QStringLiteral("activity-create-broken.db"));
        MemoryManager broken(brokenPath);
        {
            RawDb raw(brokenPath, "create-broken-drop");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("DROP TABLE internal_activity")));
        }
        error.clear();
        const auto failed = broken.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("写不成"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/9"), QStringLiteral("run-create-broken"), &error);
        QVERIFY(failed.isEmpty());
        QCOMPARE(error, QStringLiteral("db_error"));
        QCOMPARE(tableCount(brokenPath, QStringLiteral("chat_history"), "create-broken-count"), 0);
    }

    // R12-1b：单 active/paused 跨两存储统一（create 拒绝 + resume 跨存储检查）。
    void singleActiveAcrossStores()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("single-active.db"));
        MemoryManager db(path);
        QString error;
        // 已有 legacy active：createActivity 必须拒绝。
        QVERIFY(seedLegacyThought(db, QStringLiteral("雨声")) > 0);
        const auto thought = db.nextThought(true);
        const qlonglong legacyId = thought.value("id").toInteger();
        const int legacyVersion = thought.value("version").toInt();
        QVERIFY(legacyId > 0);
        QVERIFY(db.createActivity(QStringLiteral("owner"), Channel::kPet,
                                  Channel::kDefaultSession, QStringLiteral("光"), QString(),
                                  QStringLiteral("foreground"), QStringLiteral("chat/1"),
                                  QStringLiteral("run-single-1"), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("already_active"));
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "single-count-1"), 0);

        // 收口 legacy（completed 不再算当前活动）后，允许创建 internal。
        QVERIFY(db.finishThoughtReview(legacyId, legacyVersion,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("雨声")}}));
        const auto internal = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("把光写进段落"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/2"), QStringLiteral("run-single-2"), &error);
        QVERIFY2(!internal.isEmpty(), qPrintable(error));
        QCOMPARE(internal.value("status").toString(), QStringLiteral("active"));
        const QString ref = internal.value("ref").toString();
        QVERIFY(db.hasActiveActivity(QStringLiteral("owner")));
        QCOMPARE(db.hasActiveActivity(QStringLiteral("group:123")), false);

        // 暂停当前 internal（历史数据里可能还留了另一个 active，用夹具构造）。
        const auto paused = db.applyActivityAction(ref, 1, QStringLiteral("pause"), -1,
                                                   QStringLiteral("先停一下"), &error);
        QVERIFY2(!paused.isEmpty(), qPrintable(error));
        QCOMPARE(paused.value("status").toString(), QStringLiteral("paused"));
        {
            RawDb raw(path, "single-active-extra");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO internal_activity(scope_key,origin_kind,channel,session_id,interest,"
                "status,created_at,updated_at) VALUES('owner','foreground','pet','default',"
                "'历史并发留下的另一件事','active',unixepoch(),unixepoch())")));
        }
        QVERIFY(db.hasActiveActivity(QStringLiteral("owner")));
        // resume 必须发现另一个 active（排除自身后仍有），拒绝把双开写回来。
        error.clear();
        QVERIFY(db.applyActivityAction(ref, 2, QStringLiteral("resume"), -1,
                                       QStringLiteral("继续"), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("already_active"));
    }

    // R12-1b：internal 活动生命周期 create -> claim -> finish(active) -> finish(completed)。
    void internalActivityLifecycle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-life.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("雨声段落"), QStringLiteral("写完雨声段落"),
            QStringLiteral("foreground"), QStringLiteral("chat/11"),
            QStringLiteral("run-life-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value("ref").toString();
        const qlonglong id = created.value("id").toInteger();
        QCOMPARE(created.value("version").toInt(), 1);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "life-chat"), 0);
        // 刚创建（review_after=now+1800）还不是到期候选。
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());

        // 未到期不可领取。
        QString reason;
        QVERIFY(db.claimActivity(ref, 1, 12, 60, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("not_due"));

        // 到期领取：version+1，返回活动上下文。
        QVERIFY(makeInternalActivityDue(path, id, "life-due-1"));
        QCOMPARE(db.nextDueActivity(QStringLiteral("owner")).value("ref").toString(), ref);
        auto claim = db.claimActivity(ref, 1, 12, 60, &reason);
        QVERIFY2(!claim.isEmpty(), qPrintable(reason));
        QCOMPARE(claim.value("version").toInt(), 2);
        QCOMPARE(claim.value("activity").toObject().value("status").toString(),
                 QStringLiteral("active"));

        // finish(active)：写入 next_step/review_hours，仍算当前活动。
        auto finished = db.finishActivity(ref, 2,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("先写开头")},
                        {QStringLiteral("next_step"), QStringLiteral("写第二段")},
                        {QStringLiteral("review_hours"), 12}}, &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        QCOMPARE(finished.value("status").toString(), QStringLiteral("active"));
        QCOMPARE(finished.value("version").toInt(), 3);
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value("next_step").toString(), QStringLiteral("写第二段"));
        QVERIFY(withinSeconds(view.value("review_after").toInteger()
                                  - QDateTime::currentSecsSinceEpoch(),
                              12 * 3600, 30));
        QCOMPARE(db.currentActivitiesUnified(8).size(), 1);

        // 再到期领取后 completed：version+1、artifact_ref=activity/<id>、review_after=0。
        QVERIFY(makeInternalActivityDue(path, id, "life-due-2"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("life-clock")));
        claim = db.claimActivity(ref, 3, 12, 60, &reason);
        QVERIFY2(!claim.isEmpty(), qPrintable(reason));
        QCOMPARE(claim.value("version").toInt(), 4);
        finished = db.finishActivity(ref, 4,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("雨声段落完成")},
                        {QStringLiteral("next_step"), QStringLiteral("不该出现在 current")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        QCOMPARE(finished.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(finished.value("version").toInt(), 5);
        QCOMPARE(finished.value("artifact_ref").toString(),
                 QStringLiteral("activity/%1").arg(id));
        view = db.activityByRef(ref);
        QCOMPARE(view.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(view.value("review_after").toInteger(), qlonglong(0));
        // completed 的过时 next_step 不再出现在 current；也不再是到期候选。
        QVERIFY(db.currentActivitiesUnified(8).isEmpty());
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());
        // 旧版本不能把已完成活动复活。
        QVERIFY(db.finishActivity(ref, 4,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("迟到回调")}},
            &error).isEmpty());
    }

    // R12-1b：统一动作状态机（pause/resume/abandon/reschedule/complete）与 CAS。
    void activityActionsPauseResumeAbandonRescheduleComplete()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-actions.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理书桌"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/21"), QStringLiteral("run-act-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value("ref").toString();

        QVERIFY(db.applyActivityAction(ref, 1, QStringLiteral("explode"), -1, QString(),
                                       &error).isEmpty());
        QCOMPARE(error, QStringLiteral("invalid_action"));

        auto action = db.applyActivityAction(ref, 1, QStringLiteral("pause"), -1,
                                             QStringLiteral("眼睛累了"), &error);
        QVERIFY2(!action.isEmpty(), qPrintable(error));
        QCOMPARE(action.value("status").toString(), QStringLiteral("paused"));
        QCOMPARE(action.value("version").toInt(), 2);
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value("status").toString(), QStringLiteral("paused"));
        QCOMPARE(view.value("paused_reason").toString(), QStringLiteral("眼睛累了"));

        // 版本 CAS 与非法转换。
        error.clear();
        QVERIFY(db.applyActivityAction(ref, 1, QStringLiteral("resume"), -1, QString(),
                                       &error).isEmpty());
        QCOMPARE(error, QStringLiteral("cas_conflict"));
        error.clear();
        QVERIFY(db.applyActivityAction(ref, 2, QStringLiteral("pause"), -1, QString(),
                                       &error).isEmpty());
        QCOMPARE(error, QStringLiteral("invalid_transition"));

        // resume：回到 active 并重排 30 分钟。
        const qlonglong beforeResume = QDateTime::currentSecsSinceEpoch();
        action = db.applyActivityAction(ref, 2, QStringLiteral("resume"), 30,
                                        QStringLiteral("回来了"), &error);
        QVERIFY2(!action.isEmpty(), qPrintable(error));
        QCOMPARE(action.value("status").toString(), QStringLiteral("active"));
        view = db.activityByRef(ref);
        QVERIFY(withinSeconds(view.value("review_after").toInteger() - beforeResume, 1800, 30));
        QVERIFY(!view.contains(QStringLiteral("paused_reason")));

        // reschedule：只改时间；越界拒绝。
        action = db.applyActivityAction(ref, 3, QStringLiteral("reschedule"), 30,
                                        QStringLiteral("晚上再做"), &error);
        QVERIFY2(!action.isEmpty(), qPrintable(error));
        QCOMPARE(action.value("version").toInt(), 4);
        error.clear();
        QVERIFY(db.applyActivityAction(ref, 4, QStringLiteral("reschedule"), 20000,
                                       QStringLiteral("越界"), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("invalid_argument"));

        // abandon：终态，review_after=0，不能再 resume。
        action = db.applyActivityAction(ref, 4, QStringLiteral("abandon"), -1,
                                        QStringLiteral("不做了"), &error);
        QVERIFY2(!action.isEmpty(), qPrintable(error));
        QCOMPARE(action.value("status").toString(), QStringLiteral("abandoned"));
        view = db.activityByRef(ref);
        QCOMPARE(view.value("review_after").toInteger(), qlonglong(0));
        error.clear();
        QVERIFY(db.applyActivityAction(ref, 5, QStringLiteral("resume"), -1, QString(),
                                       &error).isEmpty());
        QCOMPARE(error, QStringLiteral("invalid_transition"));

        // complete：没有 draft/reflection 时拒绝；有产物后完成并写 artifact_ref。
        const auto second = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("收尾"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/22"), QStringLiteral("run-act-2"), &error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        const QString secondRef = second.value("ref").toString();
        error.clear();
        QVERIFY(db.applyActivityAction(secondRef, 1, QStringLiteral("complete"), -1,
                                       QStringLiteral("完成"), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("missing_artifact"));
        QVERIFY(!db.finishActivity(secondRef, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("开始收尾")},
                        {QStringLiteral("draft"), QStringLiteral("收尾草稿")}},
            &error).isEmpty());
        const auto completed = db.applyActivityAction(secondRef, 2,
            QStringLiteral("complete"), -1, QStringLiteral("收尾完成"), &error);
        QVERIFY2(!completed.isEmpty(), qPrintable(error));
        QCOMPARE(completed.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(completed.value("version").toInt(), 3);
        const auto done = db.activityByRef(secondRef);
        QCOMPARE(done.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(done.value("artifact_ref").toString(),
                 QStringLiteral("activity/%1").arg(second.value("id").toInteger()));
        QCOMPARE(done.value("review_after").toInteger(), qlonglong(0));
    }

    // R12-1b：defer 不改 lifecycle；重启后仍可读；recoverActivities 幂等。
    void deferAndRecovery()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-defer.db"));
        QString ref;
        qlonglong id = 0;
        {
            MemoryManager db(path);
            QString error;
            const auto created = db.createActivity(
                QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
                QStringLiteral("观察窗外的树"), QString(), QStringLiteral("open_wake"),
                QStringLiteral("wake/5:2"), QStringLiteral("run-defer-1"), &error);
            QVERIFY2(!created.isEmpty(), qPrintable(error));
            ref = created.value("ref").toString();
            id = created.value("id").toInteger();
            QCOMPARE(db.activityByRef(ref).value("status").toString(), QStringLiteral("active"));
            // defer(0)：只把 review_after 推到当前，lifecycle 不变。
            QVERIFY(db.deferActivity(ref, 1, 0, QStringLiteral("network interrupted")));
            const auto view = db.activityByRef(ref);
            QCOMPARE(view.value("status").toString(), QStringLiteral("active"));
            QCOMPARE(view.value("version").toInt(), 2);
            QVERIFY(view.value("review_after").toInteger()
                    <= QDateTime::currentSecsSinceEpoch());
            QCOMPARE(view.value("deferred_reason").toString(),
                     QStringLiteral("network interrupted"));
            QCOMPARE(db.nextDueActivity().value("ref").toString(), ref);
        }
        // 重启（重开 DB）：current/nextDue 仍可读；recoverActivities 只读且幂等。
        {
            MemoryManager db(path);
            const auto current = db.currentActivitiesUnified(8);
            QCOMPARE(current.size(), 1);
            QCOMPARE(current.first().toObject().value("ref").toString(), ref);
            QCOMPARE(db.nextDueActivity().value("ref").toString(), ref);
            QVERIFY(db.hasActiveActivity(QStringLiteral("owner")));
            const auto recovered = db.recoverActivities(QStringLiteral("owner"));
            QVERIFY(recovered.size() >= 1);
            const auto again = db.recoverActivities(QStringLiteral("owner"));
            QCOMPARE(again.size(), recovered.size());
            // 只读恢复：活动没被删除，也没有被伪造成完成。
            QCOMPARE(db.activityByRef(ref).value("status").toString(), QStringLiteral("active"));
            QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "defer-count"), 1);
            QCOMPARE(db.currentActivitiesUnified(8).size(), 1);
            QCOMPARE(internalActivityColumn(path, id, QStringLiteral("version"), "defer-verify")
                         .toInt(), 2);
        }
    }

    // R12-1b：分享意愿只落 result_json，不建 Outbox/ledger、不写聊天行。
    void shareIntentStoredNotSent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-share.db"));
        QString error;
        {
        MemoryManager db(path);
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("光线练习"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/31"), QStringLiteral("run-share-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value("ref").toString();
        // 先写一版草稿（未完成，不带分享），再带成果与分享意愿完成。
        auto finished = db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("写了开头")},
                        {QStringLiteral("draft"), QStringLiteral("光落在桌角")}}, &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        finished = db.finishActivity(ref, finished.value("version").toInt(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("光落在桌角的完整段落")},
                        {QStringLiteral("share_with_owner"), true},
                        {QStringLiteral("share_request"), QStringLiteral("想给你看这段")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        const int version = finished.value("version").toInt();
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(view.value("artifact_ref").toString(), ref);
        QCOMPARE(view.value("artifact_version").toInt(), version);
        const QString artifactHash = view.value("artifact_hash").toString();
        QCOMPARE(artifactHash.size(), 16);
        // 分享意愿与完成同一事务：绑定实际成果 ref/版本/哈希，不保存任意 draft。
        auto intent = view.value("share_intent").toObject();
        QCOMPARE(intent.value("state").toString(), QStringLiteral("saved"));
        QCOMPARE(intent.value("artifact_ref").toString(), ref);
        QCOMPARE(intent.value("artifact_version").toInt(), version);
        QCOMPARE(intent.value("content_hash").toString(), artifactHash);
        QCOMPARE(intent.value("version").toInt(), 1);
        QCOMPARE(intent.value("request").toString(), QStringLiteral("想给你看这段"));
        QVERIFY(intent.contains(QStringLiteral("requested_at")));
        QVERIFY(!intent.contains(QStringLiteral("draft")));

        // 修改：显式 share 版本 2，历史保留旧版本。
        error.clear();
        QVERIFY2(db.setActivityShareIntent(ref, version, QStringLiteral("modify"),
                                           QStringLiteral("再改一句请求"), 2, &error),
                 qPrintable(error));
        intent = db.activityByRef(ref).value("share_intent").toObject();
        QCOMPARE(intent.value("state").toString(), QStringLiteral("saved"));
        QCOMPARE(intent.value("version").toInt(), 2);
        QCOMPARE(intent.value("request").toString(), QStringLiteral("再改一句请求"));
        QCOMPARE(intent.value("history").toArray().size(), 1);
        QCOMPARE(intent.value("history").toArray().first().toObject()
                     .value("version").toInt(), 1);

        // 撤回：state=withdrawn，保留历史；share_offered 变回 false。
        error.clear();
        QVERIFY2(db.setActivityShareIntent(ref, version, QStringLiteral("withdraw"),
                                           QString(), 3, &error),
                 qPrintable(error));
        intent = db.activityByRef(ref).value("share_intent").toObject();
        QCOMPARE(intent.value("state").toString(), QStringLiteral("withdrawn"));
        QCOMPARE(intent.value("version").toInt(), 3);
        QVERIFY(intent.contains(QStringLiteral("withdrawn_at")));
        QCOMPARE(intent.value("history").toArray().size(), 2);
        QCOMPARE(db.activityByRef(ref).value("share_offered").toBool(), false);

        // share 版本必须严格递增；活动 CAS 仍生效。
        error.clear();
        QVERIFY(!db.setActivityShareIntent(ref, version, QStringLiteral("share"),
                                           QStringLiteral("重复版本"), 3, &error));
        QCOMPARE(error, QStringLiteral("share_version_conflict"));
        error.clear();
        QVERIFY(!db.setActivityShareIntent(ref, version + 1, QStringLiteral("share"),
                                           QStringLiteral("过期版本"), 4, &error));
        QCOMPARE(error, QStringLiteral("cas_conflict"));
        // 旧签名保留兼容：自动递增 share 版本。
        error.clear();
        QVERIFY2(db.setActivityShareIntent(ref, version, QStringLiteral("重新分享"), &error),
                 qPrintable(error));
        intent = db.activityByRef(ref).value("share_intent").toObject();
        QCOMPARE(intent.value("version").toInt(), 4);
        QCOMPARE(intent.value("state").toString(), QStringLiteral("saved"));

        // 未完成、没有成果的活动不能产生分享意愿。
        const auto second = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("第二件"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/32"), QStringLiteral("run-share-2"), &error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        const QString secondRef = second.value("ref").toString();
        const auto secondActive = db.finishActivity(secondRef, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("写着")},
                        {QStringLiteral("draft"), QStringLiteral("未完成稿")}}, &error);
        QVERIFY2(!secondActive.isEmpty(), qPrintable(error));
        error.clear();
        QVERIFY(!db.setActivityShareIntent(secondRef,
                                           secondActive.value("version").toInt(),
                                           QStringLiteral("share"), QStringLiteral("提前分享"),
                                           1, &error));
        QCOMPARE(error, QStringLiteral("missing_artifact"));
        // 没有任何发送/账本/聊天行副作用。
        QCOMPARE(tableCount(path, QStringLiteral("delivery_ledger"), "share-ledger"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("cognitive_action"), "share-action"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("action_outbox"), "share-outbox"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "share-chat"), 0);
        }

        // legacy 分享路径保持原样：finish 后 canShareThought 仍可用，且不产生账本行。
        QTemporaryDir legacyDir;
        QVERIFY(legacyDir.isValid());
        const auto legacyPath = legacyDir.filePath(QStringLiteral("share-legacy.db"));
        {
        MemoryManager legacy(legacyPath);
        QVERIFY(seedLegacyThought(legacy, QStringLiteral("旧写作")) > 0);
        const auto thought = legacy.nextThought(true);
        const qlonglong legacyId = thought.value("id").toInteger();
        const int legacyVersion = thought.value("version").toInt();
        QVERIFY(legacyId > 0);
        QVERIFY(legacy.finishThoughtReview(legacyId, legacyVersion,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("旧草稿")}}));
        QVERIFY(legacy.canShareThought(legacyId, legacyVersion + 1));
        // legacy 不支持新的分享意愿接口，但旧路径没有变。
        error.clear();
        QVERIFY(!legacy.setActivityShareIntent(QStringLiteral("thought/%1").arg(legacyId),
                                                legacyVersion + 1, QStringLiteral("旧"),
                                                &error));
        QCOMPARE(error, QStringLiteral("unsupported_legacy"));
        QCOMPARE(tableCount(legacyPath, QStringLiteral("delivery_ledger"), "legacy-share-ledger"),
                 0);
        }
    }

    // R12-1b：review_failure 同时支持 thought/<id> 与 activity/<id>；退避/修订重置/重置。
    void failureBackoffSupportsActivityRefs()
    {
        ProactiveIdentityGuard guard;
        QVERIFY(guard.writeIdentity(QByteArray(
            R"({"cognition":{"review_backoff_seconds":[120,300,900],"max_review_failures":4}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-failure.db"));
        QString error;
        QString reason;
        {
        MemoryManager db(path);
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("修改段落"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/41"), QStringLiteral("run-failure-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value("ref").toString();
        const qlonglong id = created.value("id").toInteger();
        QVERIFY(makeInternalActivityDue(path, id, "failure-due-0"));

        // 预算为 0：与 legacy 一样不能绕开共享预算，失败回滚（version 不变）。
        QVERIFY(db.claimActivity(ref, 1, 0, 60, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("daily_limit"));
        QCOMPARE(internalActivityColumn(path, id, QStringLiteral("version"), "failure-budget")
                     .toInt(), 1);

        auto claim = db.claimActivity(ref, 1, 12, 60, &reason);
        QVERIFY2(!claim.isEmpty(), qPrintable(reason));
        QCOMPARE(claim.value("version").toInt(), 2);

        const qlonglong failureNow = QDateTime::currentSecsSinceEpoch();
        QVERIFY(db.recordReviewFailure(QStringLiteral("activity"), ref,
                                       QStringLiteral("generate"), QStringLiteral("network_error"),
                                       true, &error));
        auto failure = db.reviewFailure(QStringLiteral("activity"), ref);
        QCOMPARE(failure.value("consecutive_failures").toInt(), 1);
        const qlonglong retry = failure.value("next_retry_at").toInteger();
        QVERIFY(withinSeconds(retry - failureNow, 120, 10));
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value("status").toString(), QStringLiteral("active"));
        QCOMPARE(view.value("version").toInt(), 3);
        QCOMPARE(view.value("review_after").toInteger(), retry);
        QCOMPARE(view.value("deferred_reason").toString(), QStringLiteral("network_error"));
        // 退避期间不能绕过（not_due）。
        reason.clear();
        QVERIFY(db.claimActivity(ref, 3, 12, 60, &reason).isEmpty());
        QCOMPARE(reason, QStringLiteral("not_due"));

        // 退避时间过后可重领，连续失败计数递增。
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-clock-1"), 400));
        claim = db.claimActivity(ref, 3, 12, 60, &reason);
        QVERIFY2(!claim.isEmpty(), qPrintable(reason));
        QCOMPARE(claim.value("version").toInt(), 4);
        QVERIFY(db.recordReviewFailure(QStringLiteral("activity"), ref,
                                       QStringLiteral("generate"), QStringLiteral("truncated"),
                                       true, &error));
        QCOMPARE(db.reviewFailure(QStringLiteral("activity"), ref)
                     .value("consecutive_failures").toInt(), 2);

        // 内容实质修订后失败：计数重置为 1（旧失败链不再成立）。
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-clock-2"), 400));
        {
            RawDb raw(path, "failure-revise");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE internal_activity SET interest='修改后的段落' WHERE id=%1").arg(id)));
        }
        claim = db.claimActivity(ref, 5, 12, 60, &reason);
        QVERIFY2(!claim.isEmpty(), qPrintable(reason));
        QCOMPARE(claim.value("version").toInt(), 6);
        QVERIFY(db.recordReviewFailure(QStringLiteral("activity"), ref,
                                       QStringLiteral("generate"), QStringLiteral("truncated"),
                                       true, &error));
        QCOMPARE(db.reviewFailure(QStringLiteral("activity"), ref)
                     .value("consecutive_failures").toInt(), 1);

        // 管理员重置：清失败行并把源拨回可领取；内部活动仍保留。
        QVERIFY(db.resetReviewFailure(QStringLiteral("activity"), ref));
        QVERIFY(db.reviewFailure(QStringLiteral("activity"), ref).isEmpty());
        QCOMPARE(internalActivityColumn(path, id, QStringLiteral("review_after"), "failure-reset")
                     .toLongLong(), qlonglong(0));
        QVERIFY(advanceReviewClock(path, QStringLiteral("failure-clock-3"), 400));
        claim = db.claimActivity(ref, 7, 12, 60, &reason);
        QVERIFY2(!claim.isEmpty(), qPrintable(reason));
        QCOMPARE(claim.value("version").toInt(), 8);
        QVERIFY(db.clearReviewFailure(QStringLiteral("activity"), ref));
        }

        // 旧引用 thought/<id> 同样正常工作（claimActivity legacy 分支 + recordReviewFailure）。
        QTemporaryDir legacyDir;
        QVERIFY(legacyDir.isValid());
        const auto legacyPath = legacyDir.filePath(QStringLiteral("activity-failure-legacy.db"));
        {
        MemoryManager legacy(legacyPath);
        QVERIFY(seedLegacyThought(legacy, QStringLiteral("雨声")) > 0);
        const auto thought = legacy.nextThought(true);
        const qlonglong legacyId = thought.value("id").toInteger();
        const int legacyVersion = thought.value("version").toInt();
        QVERIFY(legacyId > 0);
        QVERIFY(rewindReviewCooldown(legacyPath, QStringLiteral("legacy-failure-clock")));
        reason.clear();
        QVERIFY2(!legacy.claimActivity(QStringLiteral("thought/%1").arg(legacyId),
                                       legacyVersion, 12, 60, &reason).isEmpty(),
                 qPrintable(reason));
        const QString legacyRef = QStringLiteral("thought/%1").arg(legacyId);
        QVERIFY(legacy.recordReviewFailure(QStringLiteral("activity"), legacyRef,
                                           QStringLiteral("generate"), QStringLiteral("truncated"),
                                           true, &error));
        QCOMPARE(legacy.reviewFailure(QStringLiteral("activity"), legacyRef)
                     .value("consecutive_failures").toInt(), 1);
        QCOMPARE(legacy.activityByRef(legacyRef).value("deferred_reason").toString(),
                 QStringLiteral("truncated"));
        QVERIFY(legacy.resetReviewFailure(QStringLiteral("activity"), legacyRef));
        QVERIFY(legacy.reviewFailure(QStringLiteral("activity"), legacyRef).isEmpty());
        }
    }

    // R12-1b：前台 personal_interest 统一创建 internal 活动（不再写 chat_history.thought），
    // 同一 meta.eventId 重复保存不重复建活动；current/nextDue 与统一读取可见。
    void foregroundPersonalInterestCreatesInternalActivity()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("foreground-activity.db"));
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(path);
        RequestMeta meta;
        meta.eventId = QStringLiteral("foreground-event-1");
        meta.runId = QStringLiteral("foreground-run-1");
        meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
        QVERIFY(db.saveQATurn(QStringLiteral("我想写点东西"),
                              QStringLiteral(R"({"personal_interest":"雨声段落"})"), {}, meta));
        // 只有这一行聊天记录；legacy thought 列保持空（新代码不写）。
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "fg-chat"), 1);
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "fg-act"), 1);
        {
            RawDb raw(path, "fg-thought-null");
            QVERIFY(raw.open());
            QVERIFY(raw.scalar(QStringLiteral(
                "SELECT coalesce(thought,'') FROM chat_history LIMIT 1")).toString().isEmpty());
        }
        const auto unified = db.currentActivitiesUnified(8);
        QCOMPARE(unified.size(), 1);
        const auto row = unified.first().toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        QVERIFY(ref.startsWith(QStringLiteral("activity/")));
        QCOMPARE(row.value(QStringLiteral("origin")).toString(), QStringLiteral("internal"));
        QCOMPARE(row.value(QStringLiteral("origin_kind")).toString(), QStringLiteral("foreground"));
        QCOMPARE(row.value(QStringLiteral("interest")).toString(), QStringLiteral("雨声段落"));
        QCOMPARE(row.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        QCOMPARE(row.value(QStringLiteral("version")).toInt(), 1);
        QCOMPARE(row.value(QStringLiteral("run_id")).toString(),
                 QStringLiteral("foreground-event-1"));
        QVERIFY(row.value(QStringLiteral("origin_ref")).toString()
                    .startsWith(QStringLiteral("chat/")));
        QVERIFY(db.hasActiveActivity(QStringLiteral("owner")));
        // 刚创建时 review_after 在未来：不是到期候选；拨到到期后 nextDue 指向统一引用。
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());
        QVERIFY(makeInternalActivityDue(path, row.value(QStringLiteral("id")).toInteger(),
                                        "fg-due"));
        QCOMPARE(db.nextDueActivity(QStringLiteral("owner"))
                     .value(QStringLiteral("ref")).toString(), ref);
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("version")).toInt(), 1);

        // 同一 meta.eventId 重复保存：不产生第二个活动（单 active/幂等）。
        QVERIFY(db.saveQATurn(QStringLiteral("还想写"),
                              QStringLiteral(R"({"personal_interest":"雨声段落"})"), {}, meta));
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "fg-act-2"), 1);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "fg-chat-2"), 2);
    }

    // R12-1b：privateWriting=false 时前台不创建活动（不退回 legacy thought），并留可审计日志。
    void foregroundCreateRequiresPrivateWriting()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("foreground-disabled.db"));
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":false}})")));
        MemoryManager db(path);
        bool sawSkippedLog = false;
        {
            MessageCapture capture;
            QVERIFY(db.saveQATurn(QStringLiteral("我想写点东西"),
                                  QStringLiteral(R"({"personal_interest":"雨声段落"})"), {}));
            for (const auto &line : capture.lines())
                if (line.message.contains(
                        QStringLiteral("[Activity] foreground_create_skipped"
                                       " writing_disabled")))
                    sawSkippedLog = true;
        }
        QVERIFY(sawSkippedLog);
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "fg-off-act"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "fg-off-chat"), 1);
        {
            RawDb raw(path, "fg-off-thought");
            QVERIFY(raw.open());
            QVERIFY(raw.scalar(QStringLiteral(
                "SELECT coalesce(thought,'') FROM chat_history LIMIT 1")).toString().isEmpty());
        }
        QVERIFY(!db.hasActiveActivity(QStringLiteral("owner")));
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());
    }

    // R12-1b：OpenWake start_activity 与完成 wake 在同一事务：成功同时落活动+committed；
    // 活动冲突时整体失败，不产生半写（wake 仍 running，无内部活动）。
    void openWakeStartActivityTransactional()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("openwake-start-activity.db"));
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(path);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        QVERIFY(makeOpenWakeDue(path, QStringLiteral("ow-due-0")));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("ow-clock-0")));
        QString reason;
        const auto wake = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!wake.isEmpty(), qPrintable(reason));
        const qlonglong wakeId = wake.value(QStringLiteral("id")).toInteger();
        const int wakeVersion = wake.value(QStringLiteral("version")).toInt();
        const QString runId = wake.value(QStringLiteral("run_id")).toString();

        QJsonObject result;
        QString error;
        QVERIFY2(db.finishOpenWakeWithActivity(
                     wakeId, wakeVersion, QStringLiteral("start_activity"),
                     QStringLiteral("想起一段雨声"),
                     QJsonObject{{QStringLiteral("mode"), QStringLiteral("after")},
                                 {QStringLiteral("after_minutes"), 30}},
                     30,
                     QJsonObject{{QStringLiteral("interest"), QStringLiteral("写雨声段落")},
                                 {QStringLiteral("goal"), QStringLiteral("先写开头")},
                                 {QStringLiteral("initial_review_after_minutes"), 120}},
                     &result, &error),
                 qPrintable(error));
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "ow-act"), 1);
        QCOMPARE(result.value(QStringLiteral("origin_kind")).toString(),
                 QStringLiteral("open_wake"));
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        const QString activityRef = result.value(QStringLiteral("ref")).toString();
        QVERIFY(activityRef.startsWith(QStringLiteral("activity/")));
        QCOMPARE(result.value(QStringLiteral("run_id")).toString(), runId);
        QCOMPARE(result.value(QStringLiteral("origin_ref")).toString(),
                 QStringLiteral("wake/%1").arg(wakeId));
        QCOMPARE(result.value(QStringLiteral("goal")).toString(), QStringLiteral("先写开头"));
        QVERIFY(withinSeconds(result.value(QStringLiteral("review_after")).toInteger()
                                  - QDateTime::currentSecsSinceEpoch(),
                              120 * 60, 30));
        {
            RawDb raw(path, "ow-committed");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(wakeId)).toString(),
                     QStringLiteral("committed"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT decision FROM open_wake WHERE id=%1").arg(wakeId)).toString(),
                     QStringLiteral("start_activity"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM open_wake WHERE state='running'")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT last_outcome FROM open_wake_state WHERE scope_key='owner'"))
                         .toString(),
                     QStringLiteral("start_activity"));
            // start_activity 不写聊天/outbox/账本，也不产生用户行。
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM chat_history")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM action_outbox")).toInt(), 0);
            QCOMPARE(raw.scalar(QStringLiteral("SELECT count(*) FROM delivery_ledger")).toInt(), 0);
        }

        // 单 active 冲突：已有 active 活动时，整次 OpenWake 失败且绝不半写（活动数不变、
        // 本次 wake 仍 running 由失败状态机处理）。
        QVERIFY(makeOpenWakeDue(path, QStringLiteral("ow-due-1")));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("ow-clock-1")));
        reason.clear();
        const auto secondWake = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!secondWake.isEmpty(), qPrintable(reason));
        const qlonglong secondId = secondWake.value(QStringLiteral("id")).toInteger();
        const int secondVersion = secondWake.value(QStringLiteral("version")).toInt();
        error.clear();
        QJsonObject conflictResult;
        QVERIFY(!db.finishOpenWakeWithActivity(
            secondId, secondVersion, QStringLiteral("start_activity"), QString(),
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("after")},
                        {QStringLiteral("after_minutes"), 30}},
            30,
            QJsonObject{{QStringLiteral("interest"), QStringLiteral("第二件事")}},
            &conflictResult, &error));
        QCOMPARE(error, QStringLiteral("already_active"));
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "ow-act-conflict"), 1);
        {
            RawDb raw(path, "ow-conflict-verify");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(secondId)).toString(),
                     QStringLiteral("running"));
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT decision FROM open_wake WHERE id=%1").arg(secondId)).isNull(),
                     true);
        }
        // privateWriting=false 时内存层也拒绝 start_activity（OpenWake 仍可 rest/reflect）。
        ProactiveIdentityGuard disabled;
        QVERIFY(disabled.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":false}})")));
        error.clear();
        QVERIFY(!db.finishOpenWakeWithActivity(
            secondId, secondVersion, QStringLiteral("start_activity"), QString(),
            QJsonObject{}, 30, QJsonObject{{QStringLiteral("interest"), QStringLiteral("x")}},
            nullptr, &error));
        QCOMPARE(error, QStringLiteral("writing_disabled"));
    }

    // R12-1b：internal 活动的 ActivityReview 全链路：统一领取 → 继续创作 → draft/artifact；
    // share_with_owner 只保存 share_intent，不 emit shareRequested、不写账本/outbox/聊天行。
    void activityReviewInternalClaimFinishAndShareIntent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-review-internal.db"));
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(path);
        QString createError;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("雨声段落"), QStringLiteral("写完雨声段落"),
            QStringLiteral("foreground"), QStringLiteral("chat/77"),
            QStringLiteral("run-review-internal"), &createError);
        QVERIFY2(!created.isEmpty(), qPrintable(createError));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        const qlonglong id = created.value(QStringLiteral("id")).toInteger();
        QVERIFY(makeInternalActivityDue(path, id, "review-internal-due"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("review-internal-clock")));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        QSignalSpy shares(&review, &PrivateReview::shareRequested);
        const auto wake = review.wake(12, 60);
        QVERIFY2(wake.started, qPrintable(wake.reason));
        QCOMPARE(wake.kind, QStringLiteral("activity"));
        QCOMPARE(wake.sourceRef, ref);
        QCOMPARE(requested.size(), 1);
        const auto context = requested.last().at(2).toJsonObject();
        QCOMPARE(context.value(QStringLiteral("activity_ref")).toString(), ref);
        QCOMPARE(context.value(QStringLiteral("ref")).toString(), ref);
        QCOMPARE(context.value(QStringLiteral("origin_kind")).toString(),
                 QStringLiteral("internal"));
        QCOMPARE(context.value(QStringLiteral("id")).toInteger(), id);
        QCOMPARE(context.value(QStringLiteral("origin")).toObject()
                     .value(QStringLiteral("ref")).toString(), ref);
        QCOMPARE(context.value(QStringLiteral("last_review")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        const int claimedVersion = requested.last().at(1).toInt();
        QCOMPARE(claimedVersion, 2);
        const auto origin = context.value(QStringLiteral("origin")).toObject();
        review.receive(id, claimedVersion,
                       QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                   {QStringLiteral("reflection"), QStringLiteral("写完了")},
                                   {QStringLiteral("progress"), QStringLiteral("写了两段")},
                                   {QStringLiteral("next_step"), QStringLiteral("不再显示")},
                                   {QStringLiteral("draft"), QStringLiteral("雨声落在窗台")},
                                   {QStringLiteral("share_with_owner"), true},
                                   {QStringLiteral("_origin"), origin}},
                       {});
        QCOMPARE(shares.size(), 0);
        const auto view = db.activityByRef(ref);
        QCOMPARE(view.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(view.value(QStringLiteral("artifact_ref")).toString(), ref);
        QCOMPARE(view.value(QStringLiteral("draft")).toString(), QStringLiteral("雨声落在窗台"));
        QCOMPARE(view.value(QStringLiteral("version")).toInt(), claimedVersion + 1);
        // 分享意愿在 finishActivity 的同一事务原子写入，绑定成果 ref/版本/哈希。
        const auto shareIntent = view.value(QStringLiteral("share_intent")).toObject();
        QCOMPARE(shareIntent.value(QStringLiteral("state")).toString(), QStringLiteral("saved"));
        QCOMPARE(shareIntent.value(QStringLiteral("artifact_ref")).toString(), ref);
        QCOMPARE(shareIntent.value(QStringLiteral("artifact_version")).toInt(),
                 claimedVersion + 1);
        QCOMPARE(shareIntent.value(QStringLiteral("content_hash")).toString(),
                 view.value(QStringLiteral("artifact_hash")).toString());
        QCOMPARE(shareIntent.value(QStringLiteral("version")).toInt(), 1);
        // 分享意愿只落库：没有发送/账本/outbox/聊天行。
        QCOMPARE(tableCount(path, QStringLiteral("action_outbox"), "review-share-outbox"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("delivery_ledger"), "review-share-ledger"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "review-share-chat"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("open_wake"), "review-share-wake"), 0);
        QVERIFY(db.reviewFailure(QStringLiteral("activity"), ref).isEmpty());
        // 失败路径走统一 review_failure（internal ref）：迟到/非法提案不能写坏状态。
        QVERIFY(makeInternalActivityDue(path, id, "review-internal-due-2"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("review-internal-clock-2")));
        // completed 的活动不再是到期候选。
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());

        // 用户插话让出：统一 deferActivity 只推回顾时间，不改 lifecycle（status 仍 active）。
        QString secondError;
        const auto second = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("第二件事"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/78"), QStringLiteral("run-review-internal-2"), &secondError);
        QVERIFY2(!second.isEmpty(), qPrintable(secondError));
        const QString secondRef = second.value(QStringLiteral("ref")).toString();
        const qlonglong secondId = second.value(QStringLiteral("id")).toInteger();
        QVERIFY(makeInternalActivityDue(path, secondId, "review-internal-due-3"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("review-internal-clock-3")));
        const auto secondWake = review.wake(12, 60);
        QVERIFY2(secondWake.started, qPrintable(secondWake.reason));
        QCOMPARE(secondWake.sourceRef, secondRef);
        review.yieldAttention(QStringLiteral("incoming_message"));
        const auto yielded = db.activityByRef(secondRef);
        QCOMPARE(yielded.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        QVERIFY(yielded.value(QStringLiteral("review_after")).toInteger()
                <= QDateTime::currentSecsSinceEpoch());
    }

    // R12-1b：前台 activity_action 支持 internal 引用；非法/过期版本只留 audit，不写半状态。
    void activityActionSupportsInternalRef()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-action-internal.db"));
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(path);
        QString createError;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理书桌"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/88"), QStringLiteral("run-action-internal"), &createError);
        QVERIFY2(!created.isEmpty(), qPrintable(createError));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        RequestMeta meta;
        meta.eventId = QStringLiteral("action-event-1");
        meta.runId = QStringLiteral("action-run-1");
        meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
        QVERIFY(db.saveQATurn(
            QStringLiteral("先停一下"),
            QStringLiteral(R"({"activity_action":{"action":"pause","activity_ref":"%1","expected_version":1}})").arg(ref),
            {}, meta));
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("status")).toString(),
                 QStringLiteral("paused"));
        // 过期版本 resume：审计 + 状态不变（paused/version 2）。
        meta.eventId = QStringLiteral("action-event-2");
        QVERIFY(db.saveQATurn(
            QStringLiteral("继续"),
            QStringLiteral(R"({"activity_action":{"action":"resume","activity_ref":"%1","expected_version":1}})").arg(ref),
            {}, meta));
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("status")).toString(),
                 QStringLiteral("paused"));
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("version")).toInt(), 2);
        {
            RawDb raw(path, "action-audit");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM cognitive_audit "
                         "WHERE kind='activity_action_rejected'")).toInt(), 1);
        }
        // 正确版本 resume 生效。
        meta.eventId = QStringLiteral("action-event-3");
        QVERIFY(db.saveQATurn(
            QStringLiteral("继续写"),
            QStringLiteral(R"({"activity_action":{"action":"resume","activity_ref":"%1","expected_version":2}})").arg(ref),
            {}, meta));
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("status")).toString(),
                 QStringLiteral("active"));
        // 非法引用：audit，不写半状态。
        meta.eventId = QStringLiteral("action-event-4");
        QVERIFY(db.saveQATurn(
            QStringLiteral("乱写"),
            QStringLiteral(R"({"activity_action":{"action":"pause","activity_ref":"bogus/1","expected_version":1}})"),
            {}, meta));
        // legacy 引用同样走统一校验：非法 action 只留 audit，旧活动状态不变。
        const qlonglong legacyId = seedLegacyThought(db, QStringLiteral("旧活动"));
        QVERIFY(legacyId > 0);
        meta.eventId = QStringLiteral("action-event-5");
        QVERIFY(db.saveQATurn(
            QStringLiteral("旧动作"),
            QStringLiteral(R"({"activity_action":{"action":"explode","activity_ref":"thought/%1","expected_version":0}})").arg(legacyId),
            {}, meta));
        QCOMPARE(db.activityByRef(QStringLiteral("thought/%1").arg(legacyId))
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("active"));
        {
            RawDb raw(path, "action-audit-legacy");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM cognitive_audit "
                         "WHERE kind='activity_action_rejected'")).toInt(), 3);
        }
    }

    // R12-1b：legacy active 存在时，前台 personal_interest 拒绝创建 internal（单 active 跨存储），
    // 且不退回 legacy thought 写入。
    void foregroundCreateRejectsWhenLegacyActive()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("foreground-legacy-active.db"));
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        MemoryManager db(path);
        QVERIFY(seedLegacyThought(db, QStringLiteral("存量活动")) > 0);
        bool sawAlreadyActive = false;
        {
            MessageCapture capture;
            QVERIFY(db.saveQATurn(QStringLiteral("再写一件"),
                                  QStringLiteral(R"({"personal_interest":"新活动"})"), {}));
            for (const auto &line : capture.lines())
                if (line.message.contains(
                        QStringLiteral("[Activity] foreground_create_skipped already_active")))
                    sawAlreadyActive = true;
        }
        QVERIFY(sawAlreadyActive);
        QCOMPARE(tableCount(path, QStringLiteral("internal_activity"), "fg-legacy-act"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "fg-legacy-chat"), 2);
        {
            RawDb raw(path, "fg-legacy-thought");
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(*) FROM chat_history WHERE thought IS NOT NULL")).toInt(), 1);
        }
    }

    // R12-1b：internal 活动不写 chat_history/检索索引，也不从旧活动读取接口漏出。
    void noChatHistoryPollution()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-pollution.db"));
        MemoryManager db(path);
        const int chatBefore = tableCount(path, QStringLiteral("chat_history"), "pollution-chat-0");
        const int chunksBefore = tableCount(path, QStringLiteral("memory_chunks"), "pollution-chunk-0");
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("雨声里的秘密句子"), QStringLiteral("写完这句"),
            QStringLiteral("foreground"), QStringLiteral("chat/51"),
            QStringLiteral("run-pollution-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "pollution-chat-1"), chatBefore);
        QCOMPARE(tableCount(path, QStringLiteral("memory_chunks"), "pollution-chunk-1"), chunksBefore);
        QCOMPARE(tableCount(path, QStringLiteral("raw_events"), "pollution-raw"), 0);
        // 检索/最近上下文/旧活动接口都读不到内部活动正文。
        QVERIFY(db.searchMemoryChunks(QStringList{QStringLiteral("雨声里的秘密句子")}, 10).isEmpty());
        QVERIFY(db.currentActivities().isEmpty());
        QVERIFY(db.currentMotivations(8).isEmpty());
        QVERIFY(db.recentOwnerObservations(QStringLiteral("user"), Channel::kPet,
                                           Channel::kDefaultSession, 8).isEmpty());
        QVERIFY(db.recentSelfExpressions(QStringLiteral("user"), Channel::kPet,
                                         Channel::kDefaultSession, 8).isEmpty());
        QVERIFY(db.activeCommitments(QStringLiteral("group:123"), QStringLiteral("s"), 8).isEmpty());
        // unified 明确带 scope_key/channel，供调用方按 scope 过滤（群聊/第三方不读 owner 域）。
        const auto unified = db.currentActivitiesUnified(8);
        QCOMPARE(unified.size(), 1);
        const auto row = unified.first().toObject();
        QCOMPARE(row.value("scope_key").toString(), QStringLiteral("owner"));
        QCOMPARE(row.value("origin").toString(), QStringLiteral("internal"));
        QCOMPARE(row.value("channel").toString(), Channel::kPet);
    }

    // R12-1b 补充：completed 必须能指向真实成果；reflection-only 不能完成；
    // 纯思考必须有 completion_condition；外部活动不能伪造完成。
    void activityCompletionRequiresRealArtifact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-authenticity.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理旧照片"), QStringLiteral("按年份分组"),
            QStringLiteral("foreground"), QStringLiteral("chat/61"),
            QStringLiteral("run-authentic-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        // 只有内部认识（reflection/progress/mood）→ 不能完成，也不自动改完成。
        QVERIFY(db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("想了很多")},
                        {QStringLiteral("progress"), QStringLiteral("看了一半")},
                        {QStringLiteral("mood"), QStringLiteral("很安静")}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("missing_artifact"));
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        QCOMPARE(view.value(QStringLiteral("version")).toInt(), 1);
        QVERIFY(!view.contains(QStringLiteral("artifact_ref")));
        // 纯思考：没有 completion_condition → 拒绝；有 completion_condition 但无 reflection → 拒绝。
        error.clear();
        QVERIFY(db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("activity_kind"), QStringLiteral("thought")},
                        {QStringLiteral("reflection"), QStringLiteral("想清楚了")}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("missing_completion_condition"));
        error.clear();
        QVERIFY(db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("activity_kind"), QStringLiteral("thought")},
                        {QStringLiteral("completion_condition"), QStringLiteral("能复述结论")}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("missing_reflection"));
        // 纯思考带完成条件与认识：可完成，但不产生任何 artifact。
        error.clear();
        const auto thoughtDone = db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("activity_kind"), QStringLiteral("thought")},
                        {QStringLiteral("completion_condition"), QStringLiteral("能复述结论")},
                        {QStringLiteral("reflection"), QStringLiteral("想清楚了")}},
            &error);
        QVERIFY2(!thoughtDone.isEmpty(), qPrintable(error));
        QVERIFY(!thoughtDone.contains(QStringLiteral("artifact_ref")));
        view = db.activityByRef(ref);
        QCOMPARE(view.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QVERIFY(!view.contains(QStringLiteral("artifact_ref")));
        QVERIFY(!view.contains(QStringLiteral("artifact_hash")));
        // 状态诊断码：缺失 / 类型错误 / 枚举非法互不混淆。
        error.clear();
        QVERIFY(db.finishActivity(ref, 2, QJsonObject{}, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("status_missing"));
        error.clear();
        QVERIFY(db.finishActivity(ref, 2,
            QJsonObject{{QStringLiteral("status"), 7}}, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("status_type_invalid"));
        error.clear();
        QVERIFY(db.finishActivity(ref, 2,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("exploded")}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("status_enum_invalid"));

        // 外部活动（browser/painting 等）没有执行工具：不能 completed，也不能带外部结果。
        const auto second = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("看展"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/62"), QStringLiteral("run-authentic-2"), &error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        const QString secondRef = second.value(QStringLiteral("ref")).toString();
        error.clear();
        QVERIFY(db.finishActivity(secondRef, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("activity_kind"), QStringLiteral("external")},
                        {QStringLiteral("draft"), QStringLiteral("假装写完了")}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("unsupported_external_activity"));
        error.clear();
        QVERIFY(db.finishActivity(secondRef, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("external_action"), QStringLiteral("browser")},
                        {QStringLiteral("draft"), QStringLiteral("假装写完了")}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("unsupported_external_activity"));
        QCOMPARE(db.activityByRef(secondRef).value(QStringLiteral("status")).toString(),
                 QStringLiteral("active"));
        // 文字创作：draft 真实存在 → complete 并写 artifact_ref/版本/hash/completed_at。
        error.clear();
        const QString finalDraft = QStringLiteral("第一段写窗边的光。第二段写桌上的影子。");
        const auto done = db.finishActivity(secondRef, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), finalDraft}},
            &error);
        QVERIFY2(!done.isEmpty(), qPrintable(error));
        QCOMPARE(done.value(QStringLiteral("artifact_ref")).toString(), secondRef);
        QCOMPARE(done.value(QStringLiteral("artifact_version")).toInt(),
                 done.value(QStringLiteral("version")).toInt());
        const QString expectedHash = QString::fromLatin1(
            QCryptographicHash::hash(finalDraft.toUtf8(), QCryptographicHash::Sha256)
                .toHex().left(16));
        QCOMPARE(done.value(QStringLiteral("artifact_hash")).toString(), expectedHash);
        view = db.activityByRef(secondRef);
        QCOMPARE(view.value(QStringLiteral("artifact_ref")).toString(), secondRef);
        QCOMPARE(view.value(QStringLiteral("artifact_version")).toInt(),
                 done.value(QStringLiteral("version")).toInt());
        QCOMPARE(view.value(QStringLiteral("artifact_hash")).toString(), expectedHash);
        QVERIFY(view.contains(QStringLiteral("completed_at")));
        // 迟到/过期版本不能复活或改写已完成活动。
        error.clear();
        QVERIFY(db.finishActivity(secondRef, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")}},
            &error).isEmpty());
        QCOMPARE(db.activityByRef(secondRef).value(QStringLiteral("status")).toString(),
                 QStringLiteral("completed"));
    }

    // R12-1b 补充：已有完整 draft 可沿用完成（不要求每次重写全文）；完成目录有界可读。
    void recentActivityResultsDirectoryAndExistingDraft()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-results.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("雨声段落"), QStringLiteral("写完这一段"),
            QStringLiteral("foreground"), QStringLiteral("chat/71"),
            QStringLiteral("run-results-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        const qlonglong id = created.value(QStringLiteral("id")).toInteger();
        // 先保存一份完整草稿（本次创作成果）。
        const QString draft = QStringLiteral("雨声落在窗台，灯还亮着。");
        auto saved = db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("写了开头")},
                        {QStringLiteral("draft"), draft},
                        {QStringLiteral("next_step"), QStringLiteral("写第二段")}},
            &error);
        QVERIFY2(!saved.isEmpty(), qPrintable(error));
        // 到期回顾时只补充认识、不重写全文：沿用已持久化 draft 完成。
        QVERIFY(makeInternalActivityDue(path, id, "results-due"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("results-clock")));
        QString claimReason;
        const auto claim = db.claimActivity(ref, 2, 12, 60, &claimReason);
        QVERIFY2(!claim.isEmpty(), qPrintable(claimReason));
        const int claimedVersion = claim.value(QStringLiteral("version")).toInt();
        error.clear();
        const auto finished = db.finishActivity(ref, claimedVersion,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("收尾了")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        const int completedVersion = finished.value(QStringLiteral("version")).toInt();
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value(QStringLiteral("draft")).toString(), draft);
        QCOMPARE(view.value(QStringLiteral("artifact_version")).toInt(), completedVersion);
        QCOMPARE(view.value(QStringLiteral("artifact_hash")).toString(),
                 QString::fromLatin1(
                     QCryptographicHash::hash(draft.toUtf8(), QCryptographicHash::Sha256)
                         .toHex().left(16)));
        // 完成目录：完成时间倒序目录字段，不含过时 next_step；completed 不在 current/nextDue。
        const auto recent = db.recentActivityResults(QStringLiteral("owner"), 8);
        QCOMPARE(recent.size(), 1);
        const auto row = recent.first().toObject();
        QCOMPARE(row.value(QStringLiteral("ref")).toString(), ref);
        QCOMPARE(row.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(row.value(QStringLiteral("artifact_ref")).toString(), ref);
        QCOMPARE(row.value(QStringLiteral("artifact_version")).toInt(), completedVersion);
        QCOMPARE(row.value(QStringLiteral("title")).toString(), QStringLiteral("雨声段落"));
        QCOMPARE(row.value(QStringLiteral("summary")).toString(), QStringLiteral("收尾了"));
        QVERIFY(row.contains(QStringLiteral("completed_at")));
        QVERIFY(!row.contains(QStringLiteral("next_step")));
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());
        QVERIFY(db.currentActivitiesUnified(8).isEmpty());
        // 模型可见目录：owner 私聊可见，群聊/第三方/长记忆关闭为空。
        QCOMPARE(db.recentActivityResultsForModel(8, Channel::kPet,
                                                  Channel::kDefaultSession,
                                                  QStringLiteral("user"), true).size(), 1);
        QVERIFY(db.recentActivityResultsForModel(8, Channel::kQQGroup,
                                                 QStringLiteral("30001"),
                                                 QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.recentActivityResultsForModel(8, Channel::kQQPrivate,
                                                 QStringLiteral("2014366825"),
                                                 QStringLiteral("qq:10001"), true).isEmpty());
        QVERIFY(db.recentActivityResultsForModel(8, Channel::kPet,
                                                 Channel::kDefaultSession,
                                                 QStringLiteral("user"), false).isEmpty());
        // 模拟后续持久化了一版更新的 draft/artifact_version：目录必须显示新版本，不冒充旧版本。
        {
            RawDb raw(path, "results-new-version");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE internal_activity SET version=version+1,"
                "result_json=json_set(result_json,'$.draft','雨声落下后的第二版完整稿',"
                "'$.artifact_version',version+1) WHERE id=%1").arg(id)));
        }
        const auto updated = db.recentActivityResults(QStringLiteral("owner"), 8);
        QCOMPARE(updated.size(), 1);
        QCOMPARE(updated.first().toObject().value(QStringLiteral("version")).toInt(),
                 completedVersion + 1);
        QCOMPARE(updated.first().toObject().value(QStringLiteral("artifact_version")).toInt(),
                 completedVersion + 1);
        QVERIFY(!db.recentActivityResults(QStringLiteral("owner"), 8).first().toObject()
                     .value(QStringLiteral("artifact_version")).isUndefined());

        // legacy completed（只读兼容）：同样进入完成目录，缺哈希时按行版本兜底。
        QTemporaryDir legacyDir;
        QVERIFY(legacyDir.isValid());
        const auto legacyPath = legacyDir.filePath(QStringLiteral("activity-results-legacy.db"));
        MemoryManager legacy(legacyPath);
        QVERIFY(seedLegacyThought(legacy, QStringLiteral("旧成果")) > 0);
        const auto thought = legacy.nextThought(true);
        QVERIFY(legacy.finishThoughtReview(thought.value("id").toInteger(),
                                           thought.value("version").toInt(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("旧成果写完了")},
                        {QStringLiteral("draft"), QStringLiteral("旧成果正文")}}));
        const auto legacyRecent = legacy.recentActivityResults(QStringLiteral("owner"), 8);
        QCOMPARE(legacyRecent.size(), 1);
        QCOMPARE(legacyRecent.first().toObject().value(QStringLiteral("ref")).toString(),
                 QStringLiteral("thought/%1").arg(thought.value("id").toInteger()));
        QVERIFY(!legacyRecent.first().toObject().value(QStringLiteral("artifact_version"))
                     .isUndefined());
    }

    // R12-1b 补充：统一权限投影（sensitive/盲区/第三方/群聊/长记忆关闭）；嵌套字段不泄漏；
    // 盲区活动不可见且不阻塞其它合法活动。
    void activityProjectionVisibilityAndNestedFields()
    {
        auto &config = ConfigManager::instance();
        QFile settingFile(config.getConfigFilePath());
        const bool settingExisted = settingFile.open(QIODevice::ReadOnly);
        const QByteArray settingOriginal = settingExisted ? settingFile.readAll() : QByteArray{};
        settingFile.close();
        auto restoreSetting = qScopeGuard([&] {
            config.writeConfigDocument(QStringLiteral("setting.json"),
                                       settingExisted ? settingOriginal : QByteArray("{}"),
                                       nullptr);
            if (!settingExisted) QFile::remove(config.getConfigFilePath());
        });
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"memory":{"blind_zones":[{"channel":"pet","session_id":"quiet-room"}]}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-projection.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理旧照片"), QStringLiteral("私人暗号目标"),
            QStringLiteral("foreground"), QStringLiteral("chat/81"),
            QStringLiteral("run-projection-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        const QString draftWithSecret = QStringLiteral("正文里提到了私人暗号但不该泄漏");
        const auto finished = db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("整理好了")},
                        {QStringLiteral("draft"), draftWithSecret},
                        {QStringLiteral("next_step"), QStringLiteral("过时步骤")},
                        {QStringLiteral("share_with_owner"), true},
                        {QStringLiteral("share_request"), QStringLiteral("私人暗号请求")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        // owner 私聊可见；嵌套的 goal/draft/share_request 被隐私规则过滤，不泄漏。
        const auto ownerView = db.activityForModel(
            ref, Channel::kPet, Channel::kDefaultSession, QStringLiteral("user"), true);
        QVERIFY(!ownerView.isEmpty());
        QCOMPARE(ownerView.value(QStringLiteral("ref")).toString(), ref);
        // owner 私聊可见自己的活动正文；completed 的过时 next_step 不进入投影。
        QVERIFY(ownerView.contains(QStringLiteral("goal")));
        QVERIFY(ownerView.contains(QStringLiteral("draft")));
        QVERIFY(ownerView.contains(QStringLiteral("artifact_ref")));
        QVERIFY(!ownerView.contains(QStringLiteral("next_step")));
        const auto ownerShare = ownerView.value(QStringLiteral("share_intent")).toObject();
        QVERIFY(ownerShare.contains(QStringLiteral("request")));
        // 群聊/第三方/长记忆关闭：整活动不可见（current/nextDue/recent 也不含）。
        QVERIFY(db.activityForModel(ref, Channel::kQQGroup, QStringLiteral("30001"),
                                    QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.activityForModel(ref, Channel::kQQPrivate, QStringLiteral("2014366825"),
                                    QStringLiteral("qq:10001"), true).isEmpty());
        QVERIFY(db.activityForModel(ref, Channel::kPet, Channel::kDefaultSession,
                                    QStringLiteral("user"), false).isEmpty());
        QVERIFY(db.currentActivitiesForModel(8, Channel::kQQGroup,
                                             QStringLiteral("30001"),
                                             QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.recentActivityResultsForModel(8, Channel::kQQGroup,
                                                 QStringLiteral("30001"),
                                                 QStringLiteral("user"), true).isEmpty());
        // 先建一个 clean active 活动，让"长记忆关闭时 nextDue 为空"有真实候选。
        QString cleanError;
        const auto clean = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理书桌"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/83"), QStringLiteral("run-projection-clean"), &cleanError);
        QVERIFY2(!clean.isEmpty(), qPrintable(cleanError));
        const auto cleanRef = clean.value(QStringLiteral("ref")).toString();
        QVERIFY(!cleanRef.isEmpty());
        {
            RawDb raw(path, "projection-clean-due");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE internal_activity SET review_after=unixepoch()-1 WHERE id=%1")
                                 .arg(clean.value(QStringLiteral("id")).toInteger())));
        }
        QVERIFY(!db.activityForModel(cleanRef, Channel::kPet, Channel::kDefaultSession,
                                     QStringLiteral("user"), true).isEmpty());
        // 长记忆服务端配置关闭：投影、current、recent、nextDue 全部为空。
        QVERIFY(config.writeConfigDocument(QStringLiteral("setting.json"),
            R"({"memory":{"long_term_memory_enabled":false}})", nullptr));
        QVERIFY(db.activityForModel(ref, Channel::kPet, Channel::kDefaultSession,
                                    QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.currentActivitiesForModel(8, Channel::kPet, Channel::kDefaultSession,
                                             QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.recentActivityResultsForModel(8, Channel::kPet, Channel::kDefaultSession,
                                                 QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.nextDueActivity(QStringLiteral("owner")).isEmpty());
        QVERIFY(config.writeConfigDocument(QStringLiteral("setting.json"), QByteArray("{}"),
                                           nullptr));

        // 盲区活动：自身会话命中 blind_zone → 不可见；也不阻塞上面那个合法活动。
        {
            RawDb raw(path, "projection-blind-insert");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "INSERT INTO internal_activity(scope_key,origin_kind,origin_ref,run_id,subject,"
                "channel,session_id,interest,status,version,result_json,artifact_ref,"
                "review_after,created_at,updated_at) "
                "VALUES('owner','foreground','chat/82','run-projection-blind','user','pet',"
                "'quiet-room','盲区里的活动','active',1,'{}',NULL,unixepoch()-10,unixepoch(),unixepoch())")));
        }
        const QString blindRef = QStringLiteral("activity/%1").arg(
            RawDb(path, "projection-blind-id").scalar(
                QStringLiteral("SELECT max(id) FROM internal_activity")).toLongLong());
        QVERIFY(db.activityForModel(blindRef, Channel::kPet, Channel::kDefaultSession,
                                    QStringLiteral("user"), true).isEmpty());
        QVERIFY(db.activityForModel(blindRef, Channel::kPet, QStringLiteral("quiet-room"),
                                    QStringLiteral("user"), true).isEmpty());
        const auto due = db.nextDueActivity(QStringLiteral("owner"));
        QVERIFY(!due.isEmpty());
        QVERIFY(due.value(QStringLiteral("ref")).toString() != blindRef);
        QCOMPARE(due.value(QStringLiteral("ref")).toString(), cleanRef);
        // legacy 敏感行：sensitive=1 不进入投影。
        const qlonglong sensitiveId = seedLegacyThought(db, QStringLiteral("敏感旧活动"));
        QVERIFY(sensitiveId > 0);
        {
            RawDb raw(path, "projection-sensitive");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE chat_history SET sensitive=1 WHERE id=%1").arg(sensitiveId)));
        }
        QVERIFY(db.activityForModel(QStringLiteral("thought/%1").arg(sensitiveId),
                                    Channel::kPet, Channel::kDefaultSession,
                                    QStringLiteral("user"), true).isEmpty());
    }

    // R12-1b 补充：分享意愿与完成同事务原子失败（触发约束 / 存储错误）整体回滚，
    // 不出现"半完成/半意愿"。
    void shareIntentAtomicRollback()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-share-atomic.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("原子提交"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/91"), QStringLiteral("run-share-atomic"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        // 分享请求形状非法：整体拒绝，状态/版本不变。
        error.clear();
        QVERIFY(db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("真实成果")},
                        {QStringLiteral("share_request"), 42}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("share_intent_invalid"));
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("version")).toInt(), 1);
        // 未完成却请求分享：拒绝（无成果不能分享）。
        error.clear();
        QVERIFY(db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("active")},
                        {QStringLiteral("reflection"), QStringLiteral("继续")},
                        {QStringLiteral("draft"), QStringLiteral("还没完成")},
                        {QStringLiteral("share_with_owner"), true}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("share_intent_invalid"));
        // 模拟提交阶段存储约束失败：触发 ABORT 让同事务的完成+分享整体回滚。
        {
            RawDb raw(path, "share-atomic-trigger");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "CREATE TRIGGER tst_share_abort BEFORE UPDATE ON internal_activity "
                "WHEN NEW.status='completed' AND instr(NEW.result_json,'share_intent')>0 "
                "BEGIN SELECT RAISE(ABORT,'tst_share_abort'); END")));
        }
        error.clear();
        QVERIFY(db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("真实成果")},
                        {QStringLiteral("share_with_owner"), true}},
            &error).isEmpty());
        QCOMPARE(error, QStringLiteral("db_error"));
        auto view = db.activityByRef(ref);
        QCOMPARE(view.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        QCOMPARE(view.value(QStringLiteral("version")).toInt(), 1);
        QVERIFY(!view.contains(QStringLiteral("share_intent")));
        QVERIFY(!view.value(QStringLiteral("share_offered")).toBool());
        {
            RawDb raw(path, "share-atomic-drop");
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("DROP TRIGGER tst_share_abort")));
        }
        // 去掉故障后同一次完成+分享成功；share_intent 与成果版本/哈希一致。
        error.clear();
        const auto finished = db.finishActivity(ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写完了")},
                        {QStringLiteral("draft"), QStringLiteral("真实成果")},
                        {QStringLiteral("share_with_owner"), true}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        view = db.activityByRef(ref);
        const auto shareIntent = view.value(QStringLiteral("share_intent")).toObject();
        QCOMPARE(shareIntent.value(QStringLiteral("state")).toString(), QStringLiteral("saved"));
        QCOMPARE(shareIntent.value(QStringLiteral("artifact_ref")).toString(), ref);
        QCOMPARE(shareIntent.value(QStringLiteral("artifact_version")).toInt(),
                 view.value(QStringLiteral("version")).toInt());
        QCOMPARE(shareIntent.value(QStringLiteral("content_hash")).toString(),
                 view.value(QStringLiteral("artifact_hash")).toString());
        QCOMPARE(tableCount(path, QStringLiteral("action_outbox"), "share-atomic-outbox"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("delivery_ledger"), "share-atomic-ledger"), 0);
        QCOMPARE(tableCount(path, QStringLiteral("chat_history"), "share-atomic-chat"), 0);
    }

    // R12-1b 补充：ActivityReview 失败日志必须给出阶段+具体诊断码，且不记录正文。
    void activityReviewFailureDiagnostics()
    {
        ProactiveIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"private_writing":true}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("activity-diagnostics.db"));
        MemoryManager db(path);
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("诊断活动"), QString(), QStringLiteral("foreground"),
            QStringLiteral("chat/101"), QStringLiteral("run-diag-1"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        const qlonglong id = created.value(QStringLiteral("id")).toInteger();
        QVERIFY(makeInternalActivityDue(path, id, "diag-due-1"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("diag-clock-1")));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        const auto wake = review.wake(12, 60);
        QVERIFY2(wake.started, qPrintable(wake.reason));
        const int version = requested.last().at(1).toInt();
        const auto origin = requested.last().at(2).toJsonObject()
                                .value(QStringLiteral("origin")).toObject();
        const QString privateText = QStringLiteral("这句私聊正文不应出现在诊断日志里");
        {
            MessageCapture capture;
            review.receive(id, version,
                           QJsonObject{{QStringLiteral("status"), QStringLiteral("exploding")},
                                       {QStringLiteral("reflection"), privateText},
                                       {QStringLiteral("_origin"), origin}},
                           {});
            const auto failure = db.reviewFailure(QStringLiteral("activity"), ref);
            QCOMPARE(failure.value(QStringLiteral("last_reason")).toString(),
                     QStringLiteral("status_enum_invalid"));
            QCOMPARE(failure.value(QStringLiteral("last_stage")).toString(),
                     QStringLiteral("schema"));
            bool sawDiagnostic = false;
            for (const auto &line : capture.lines()) {
                if (line.message.contains(QStringLiteral("[Activity] review_failed"))
                    && line.message.contains(QStringLiteral("code=status_enum_invalid"))
                    && line.message.contains(QStringLiteral("stage=schema"))
                    && line.message.contains(QStringLiteral("status_enum=exploding")))
                    sawDiagnostic = true;
                QVERIFY2(!line.message.contains(privateText),
                         "诊断日志不能记录私聊正文");
            }
            QVERIFY(sawDiagnostic);
        }
        // 失败后恢复到期并重新领取，成功提交合法 completed。
        QVERIFY(db.resetReviewFailure(QStringLiteral("activity"), ref));
        QVERIFY(makeInternalActivityDue(path, id, "diag-due-2"));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("diag-clock-2")));
        const auto wakeAgain = review.wake(12, 60);
        QVERIFY2(wakeAgain.started, qPrintable(wakeAgain.reason));
        const int retryVersion = requested.last().at(1).toInt();
        const auto originAgain = requested.last().at(2).toJsonObject()
                                     .value(QStringLiteral("origin")).toObject();
        review.receive(id, retryVersion,
                       QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                   {QStringLiteral("reflection"), QStringLiteral("写完了")},
                                   {QStringLiteral("draft"), QStringLiteral("真实写下的成果")},
                                   {QStringLiteral("_origin"), originAgain}},
                       {});
        QCOMPARE(db.activityByRef(ref).value(QStringLiteral("status")).toString(),
                 QStringLiteral("completed"));
        QVERIFY(db.reviewFailure(QStringLiteral("activity"), ref).isEmpty());
    }

    // ===== R12-2c-3：调度/预算/失败生命周期回归（A24-A27） =====

    // A24：max_per_day 只约束 OpenWake 领取次数（不是 contact/提醒次数）；max_per_day=0 停
    // OpenWake；daily_limit=0 停后台自主认知但独立提醒仍可提交；OpenWake 与普通任务共享预算。
    void schedulingBudgetSemantics()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath(QStringLiteral("a24-budget.db"));
        {
            MemoryManager db(path);
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            QVERIFY(makeOpenWakeDue(path, QStringLiteral("a24-due")));
            // max_per_day=1：只能领一次 OpenWake（领取次数上限，不是 contact 次数上限）。
            QString reason;
            auto wake = db.claimOpenWake(12, 60, 1, 100, 4, &reason);
            QVERIFY2(!wake.isEmpty(), qPrintable(reason));
            QVERIFY(db.finishOpenWake(wake.value(QStringLiteral("id")).toInteger(),
                                      wake.value(QStringLiteral("version")).toInt(),
                                      QStringLiteral("rest"), QString(), QJsonObject{},
                                      30, nullptr));
            QVERIFY(makeOpenWakeDue(path, QStringLiteral("a24-due-2")));
            QVERIFY(db.claimOpenWake(12, 60, 1, 100, 4, &reason).isEmpty());
            QCOMPARE(reason, QStringLiteral("budget"));
            // 独立提醒链不受 OpenWake 每日上限影响，也不占用回顾预算。
            const qint64 reminder = insertPendingReminder(
                path, QStringLiteral("a24-reminder"), Channel::kPet,
                Channel::kDefaultSession, -10, QStringLiteral("该喝水了"));
            QVERIFY(reminder > 0);
            const auto reminderCommit = db.commitReminderDeliveryAction(reminder);
            QVERIFY2(!reminderCommit.contains(QStringLiteral("error")),
                     qPrintable(reminderCommit.value(QStringLiteral("error")).toString()));
        }
        {
            // max_per_day=0：OpenWake 停止领取；daily_limit=0：所有后台回顾停止，
            // 但独立提醒仍可提交（提醒链不经过回顾预算）。
            const QString path2 = dir.filePath(QStringLiteral("a24-disabled.db"));
            MemoryManager db(path2);
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            QVERIFY(makeOpenWakeDue(path2, QStringLiteral("a24-due-3")));
            QString reason;
            QVERIFY(db.claimOpenWake(12, 60, 0, 100, 4, &reason).isEmpty());
            QCOMPARE(reason, QStringLiteral("budget"));
            const qint64 reflectionTurn = seedReflectionSource(db, QStringLiteral("a24-reflect"));
            QVERIFY(reflectionTurn > 0);
            reason.clear();
            QVERIFY(db.claimPrivateReflection(0, 60, &reason).isEmpty());
            QCOMPARE(reason, QStringLiteral("daily_limit"));
            const qint64 reminder = insertPendingReminder(
                path2, QStringLiteral("a24-reminder-2"), Channel::kPet,
                Channel::kDefaultSession, -10, QStringLiteral("早点休息"));
            QVERIFY(reminder > 0);
            const auto reminderCommit = db.commitReminderDeliveryAction(reminder);
            QVERIFY2(!reminderCommit.contains(QStringLiteral("error")),
                     qPrintable(reminderCommit.value(QStringLiteral("error")).toString()));
        }
        {
            // OpenWake 与普通任务共享每日预算：先领普通回顾后，OpenWake 因共享额度被挡。
            const QString path3 = dir.filePath(QStringLiteral("a24-shared.db"));
            MemoryManager db(path3);
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            QVERIFY(makeOpenWakeDue(path3, QStringLiteral("a24-due-4")));
            const qint64 turn = seedReflectionSource(db, QStringLiteral("a24-shared-reflect"));
            QVERIFY(turn > 0);
            QVERIFY(!db.claimPrivateReflection(1, 60).isEmpty());
            QString reason;
            QVERIFY(db.claimOpenWake(1, 60, 12, 100, 4, &reason).isEmpty());
            // 共享每日预算已用尽（OpenWake 与普通任务共用 thought_review_log 预算；
            // claimOpenWake 将 daily_limit 归并为对外稳定原因 budget）。
            QCOMPARE(reason, QStringLiteral("budget"));
        }
    }

    // A25：N 次普通任务后的 OpenWake 优先级不因失败/重启清零；到期 OpenWake 不被普通任务饿死。
    void openWakeFairnessPersistsAcrossFailureAndRestart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath(QStringLiteral("a25-fairness.db"));
        MemoryManager db(path);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        QVERIFY(makeOpenWakeDue(path, QStringLiteral("a25-due")));
        QVERIFY(seedReflectionSource(db, QStringLiteral("a25-r1")) > 0);
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        QCOMPARE(db.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 1);
        QVERIFY(seedReflectionSource(db, QStringLiteral("a25-r2")) > 0);
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("a25-clock")));
        QVERIFY(!db.claimPrivateReflection(12, 60).isEmpty());
        QCOMPARE(db.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 2);
        // 失败（预算拦截）不重置公平计数；只按实际成功领取递增。
        QVERIFY(db.claimPrivateReflection(0, 60).isEmpty());
        QCOMPARE(db.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 2);
        // 重启不重置。
        MemoryManager reopened(path);
        QCOMPARE(reopened.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 2);
        // 到期 OpenWake 仍可领取（不被普通任务饿死），领取成功清零公平计数。
        QVERIFY(makeOpenWakeDue(path, QStringLiteral("a25-due-2")));
        QVERIFY(rewindReviewCooldown(path, QStringLiteral("a25-clock-2")));
        QString reason;
        const auto wake = reopened.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!wake.isEmpty(), qPrintable(reason));
        QCOMPARE(reopened.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 0);
    }

    // A26：每次真实回顾尝试独立 run_id；旧版本的迟到失败不污染新版本来源失败状态；
    // 用户插话 yield 不增加失败计数。
    void reviewAttemptRunIdAndLateFailureIsolation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath(QStringLiteral("a26-runid.db"));
        MemoryManager db(path);
        const qlonglong turn = seedReflectionSource(db, QStringLiteral("a26-r"));
        QVERIFY(turn > 0);
        const QString ref = QStringLiteral("reflection/%1").arg(turn);
        const auto first = db.claimPrivateReflection(12, 60);
        QVERIFY(!first.isEmpty());
        const int firstVersion = first.value(QStringLiteral("version")).toInt();
        QVERIFY(firstVersion > 0);
        QVERIFY(db.attachReviewAttemptRunId(QStringLiteral("reflection"), ref,
                                            QStringLiteral("run-a")));
        QString error;
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref, QStringLiteral("verify"),
                                       QStringLiteral("network_error"), true, &error,
                                       firstVersion));
        QVERIFY(deferredSeconds(path, turn, QStringLiteral("a26-defer")) > 0);
        QVERIFY(rewindReflectionAttempt(path, turn, QStringLiteral("a26-rewind")));
        const auto second = db.claimPrivateReflection(12, 60);
        QVERIFY(!second.isEmpty());
        const int secondVersion = second.value(QStringLiteral("version")).toInt();
        QVERIFY(secondVersion > firstVersion);
        QVERIFY(db.attachReviewAttemptRunId(QStringLiteral("reflection"), ref,
                                            QStringLiteral("run-b")));
        {
            RawDb raw(path, QStringLiteral("a26-runids"));
            QVERIFY(raw.open());
            QCOMPARE(raw.scalar(QStringLiteral(
                         "SELECT count(DISTINCT run_id) FROM thought_review_log "
                         "WHERE source_kind='reflection' AND source_ref='")
                         + ref + QStringLiteral("'")).toInt(),
                     2);
        }
        // 旧版本的迟到失败只审计：不改变当前失败行（此前 1 次真实失败不变）。
        const int failuresBefore =
            db.reviewFailure(QStringLiteral("reflection"), ref)
                .value(QStringLiteral("consecutive_failures")).toInt();
        QCOMPARE(failuresBefore, 1);
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref, QStringLiteral("verify"),
                                       QStringLiteral("network_error"), true, &error,
                                       firstVersion));
        QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                     .value(QStringLiteral("consecutive_failures")).toInt(),
                 failuresBefore);
        // 当前版本的失败才累计。
        QVERIFY(db.recordReviewFailure(QStringLiteral("reflection"), ref, QStringLiteral("verify"),
                                       QStringLiteral("network_error"), true, &error,
                                       secondVersion));
        QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                     .value(QStringLiteral("consecutive_failures")).toInt(),
                 failuresBefore + 1);
        // 用户插话主动让出：不增加失败计数、不退避。
        QVERIFY(rewindReflectionAttempt(path, turn, QStringLiteral("a26-rewind-2")));
        PrivateReview review(db);
        const auto wake = review.wake(12, 60);
        QVERIFY2(wake.started, qPrintable(wake.reason));
        review.yieldAttention(QStringLiteral("incoming_message"));
        QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                     .value(QStringLiteral("consecutive_failures")).toInt(),
                 failuresBefore + 1);
    }

    // A27：action_context_epoch 只在相关语境变化时 bump（主人私聊输入/成果撤回/放弃活动），
    // 无关群聊不 bump；模型决定的长 next_wake 不被缩短。
    void actionContextEpochScopedAndLongIntervalPreserved()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ProactiveIdentityGuard identity;
        QVERIFY(identity.enableContact());
        const QString path = dir.filePath(QStringLiteral("a27-epoch.db"));
        MemoryManager db(path);
        QCOMPARE(db.actionContextEpoch(), qint64(0));
        RequestMeta ownerMeta = concernTurnMeta(QStringLiteral("a27-owner"));
        ownerMeta.channel = Channel::kPet;
        ownerMeta.sessionId = Channel::kDefaultSession;
        QVERIFY(db.recordInput(QStringLiteral("我回来了"), ownerMeta, false) > 0);
        QCOMPARE(db.actionContextEpoch(), qint64(1));
        // 无关群聊输入不 bump。
        RequestMeta groupMeta = concernTurnMeta(QStringLiteral("a27-group"));
        groupMeta.channel = Channel::kQQGroup;
        groupMeta.sessionId = QStringLiteral("group-1");
        groupMeta.actorId = QStringLiteral("member-1");
        QVERIFY(db.recordInput(QStringLiteral("群里随便聊聊"), groupMeta, false) > 0);
        QCOMPARE(db.actionContextEpoch(), qint64(1));
        // 完成活动本身不 bump；分享撤回（成果失效）bump。
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("写雨声"), QStringLiteral("记下窗边雨声"),
            QStringLiteral("foreground"), QString(), QStringLiteral("a27-run-1"));
        QVERIFY(!created.isEmpty());
        const QString ref = created.value(QStringLiteral("ref")).toString();
        QString error;
        const auto finished = db.finishActivity(
            ref, created.value(QStringLiteral("version")).toInt(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写好了")},
                        {QStringLiteral("draft"), QStringLiteral("雨在窗上停了一停。")},
                        {QStringLiteral("share_with_owner"), true}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        QCOMPARE(db.actionContextEpoch(), qint64(1));
        QVERIFY(db.setActivityShareIntent(
            ref, finished.value(QStringLiteral("version")).toInt(),
            QStringLiteral("withdraw"), QString(), -1, &error));
        QCOMPARE(db.actionContextEpoch(), qint64(2));
        // 放弃活动（成果撤回）也 bump。
        const auto secondActivity = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理桌面"), QStringLiteral("记一下"),
            QStringLiteral("foreground"), QString(), QStringLiteral("a27-run-2"));
        QVERIFY(!secondActivity.isEmpty());
        const auto abandoned = db.applyActivityAction(
            secondActivity.value(QStringLiteral("ref")).toString(),
            secondActivity.value(QStringLiteral("version")).toInt(),
            QStringLiteral("abandon"), 0, QStringLiteral("test"), &error);
        QVERIFY2(!abandoned.isEmpty(), qPrintable(error));
        QCOMPARE(db.actionContextEpoch(), qint64(3));
        // 模型给出的长间隔（1440 分钟）不被缩短。
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        QVERIFY(makeOpenWakeDue(path, QStringLiteral("a27-due")));
        QString reason;
        const auto wake = db.claimOpenWake(12, 60, 12, 100, 4, &reason);
        QVERIFY2(!wake.isEmpty(), qPrintable(reason));
        const qint64 before = QDateTime::currentSecsSinceEpoch();
        QVERIFY(db.finishOpenWake(wake.value(QStringLiteral("id")).toInteger(),
                                  wake.value(QStringLiteral("version")).toInt(),
                                  QStringLiteral("rest"), QString(), QJsonObject{},
                                  1440, &error));
        const qint64 nextDue =
            db.openWakeState().value(QStringLiteral("next_due_at")).toInteger();
        QVERIFY2(nextDue - before >= 1430 * 60, qPrintable(QString::number(nextDue - before)));
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
