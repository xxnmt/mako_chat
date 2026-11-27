#include "mattersemantics.h"
#include "memorymanager.h"
#include "actionorigin.h"
#include "actionpolicy.h"
#include "debuglog.h"
#include "configmanager.h"
#include "memoryreadpolicy.h"

#include <QStandardPaths>
#include <QScopeGuard>
#include <QDir>
#include <QCryptographicHash>
#include <QUuid>
#include <QElapsedTimer>
#include <QThread>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <QDebug>
#include <QDate>
#include <QTime>
#include "qqmessageparser.h"
#include "memorytextutils.h"
#include "memoryembedding.h"

#include "historyturn.h"

namespace {

RawEvent rawEventFromQuery(const QSqlQuery &query)
{
    RawEvent event;
    event.id = query.value("id").toLongLong();
    event.platform = query.value("platform").toString();
    event.channel = query.value("channel").toString();
    event.sessionId = query.value("session_id").toString();
    event.messageId = query.value("message_id").toString();
    event.direction = query.value("direction").toString();
    event.senderId = query.value("sender_id").toString();
    event.senderName = query.value("sender_name").toString();
    event.personId = query.value("person_id").toString();
    event.eventType = query.value("event_type").toString();
    event.content = query.value("content").toString();
    event.payloadJson = query.value("payload").toString();
    event.occurredAt = query.value("occurred_at").toDateTime();
    event.sensitive = query.value("sensitive").toInt() != 0;
    event.visibility = query.value("visibility").toString();
    event.processed = query.value("processed").toInt();
    return event;
}

QString rawEventColumns()
{
    return QStringLiteral(
        "id, platform, channel, session_id, message_id, direction, sender_id, "
        "sender_name, person_id, event_type, content, payload, occurred_at, sensitive, "
        "visibility, processed");
}

// R12-1b：成果正文的 SHA-256 前 16 位（十六进制小写）。只对已持久化的实际 draft 计算，
// 用于把分享意愿绑定到具体成果版本；不记录/输出正文。
QString activityContentHash(const QString &text)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(text.trimmed().toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}

// R12-1b：活动渠道是否属于主人私聊域（pet/qq_private）。空渠道表示"只知道是主人私聊
// 来源"（回顾上下文可能省略），仍按私聊域处理；群聊/其它渠道一律不是。
bool isOwnerPrivateActivityChannel(const QString &channel)
{
    return channel.isEmpty() || channel == Channel::kPet || channel == Channel::kQQPrivate;
}

} // namespace

MemoryManager::MemoryManager(const QString &dbFilePath,QObject *parent,
                             const QString &connectionName)
    : QObject{parent}, m_databasePath(dbFilePath)
{
    initDatabase(dbFilePath, connectionName);
}

MemoryManager::~MemoryManager()
{
    if(m_db.isOpen()){
        m_db.close();

        qDebug()<<"[MemoryManager]:Sqlite已关闭";
    }
}

bool MemoryManager::saveQATurn(const QString &userInput, const QString &rawReply, const QList<SentenceText> &sentences, const RequestMeta &meta, qint64 receivedTurn)
{
    if(!m_db.isOpen()){
        qDebug()<<"[MemoryManager]:数据库打开失败";
        return false;
    }

    QJsonObject rootObj;
    QJsonArray sentencesArray;
    for (const SentenceText &sentence : sentences) {
        QJsonObject sentenceObj;
        sentenceObj["zh_text"] = sentence.zhText;
        sentenceObj["ja_text"] = sentence.jaText;

        QJsonObject tagsObj;
        for (QMap<QString, QString>::const_iterator it=sentence.rawTags.constBegin();
             it!=sentence.rawTags.constEnd(); it++) {
            tagsObj[it.key()] = it.value();
        }
        sentenceObj["tags"] = tagsObj;
        sentencesArray.append(sentenceObj);
    }
    rootObj["sentences"]=sentencesArray;
    const auto response = QJsonDocument::fromJson(rawReply.toUtf8()).object();
    const auto action = response["conversation_action"].toString();
    if (action == "close" || action == "defer")
        rootObj["conversation_action"] = action;
    QString JsonStr = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);

    QString personalInterest;
    QJsonArray updates;
    if (meta.longTermMemoryEnabled && meta.persistTurn && !meta.sensitive
        && meta.turnKind == "reply" && !meta.subject.isEmpty()
        && (meta.channel == Channel::kPet || !meta.actorId.isEmpty())) {
        if (meta.subject == "user" && (meta.channel == Channel::kPet || meta.channel == Channel::kQQPrivate)) {
            // R12-1b：合法 personal_interest 不再写 chat_history.thought，改为下面在拿到
            // 本轮 id 后创建统一 internal 活动（私有写入门控 + 幂等 + 单 active）。
            const auto candidate = response["personal_interest"].toString().trimmed();
            if (!candidate.isEmpty() && candidate.size() <= 120)
                personalInterest = candidate;
        }
        const auto proposals = meta.eventId.isEmpty() ? response["state_updates"].toArray() : QJsonArray{};
        for (const auto &proposal : proposals) {
            const auto obj = proposal.toObject();
            const auto topic = obj["topic"].toString().trimmed();
            const auto quote = obj["quote"].toString().trimmed();
            if (topic.size() < 2 || topic.size() > 32 || quote.isEmpty() || quote.size() > 180
                || !userInput.contains(quote))
                continue;
            updates.append(QJsonObject{{"topic", topic}, {"quote", quote}});
            if (updates.size() == 4)
                break;
        }
    }
    QSqlQuery query(m_db);
    if (receivedTurn != 0) {
        if (sentences.isEmpty()) return false;
        // R12-1b：恢复轮次不再覆盖 thought（新代码不写 legacy thought 列）。
        query.prepare("UPDATE chat_history SET raw_reply=:raw_reply,turn_state='handed_off' "
                      "WHERE id=:id AND cognitive_event=:event AND turn_state='received'");
        query.bindValue(":id", receivedTurn);
        query.bindValue(":event", meta.eventId);
    } else {
        query.prepare("INSERT INTO chat_history "
                      "(user_input, raw_reply, channel, session_id, speaker, sensitive, turn_kind, trigger_event_ids, subject, state_updates, cognitive_event, timestamp) "
                      "VALUES (:user_input, :raw_reply, :channel, :session_id, :speaker, :sensitive, :turn_kind, :trigger_event_ids, :subject, :updates, :event, :timestamp)");
        query.bindValue(":event", meta.eventId);
        query.bindValue(":updates", updates.isEmpty() ? QVariant(QMetaType(QMetaType::QString))
            : QVariant(QString::fromUtf8(QJsonDocument(updates).toJson(QJsonDocument::Compact))));
        // 主动/感知轮次没有用户输入：绑定 SQL NULL，而不是空串
        query.bindValue(":user_input", userInput.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(userInput));
        query.bindValue(":channel", meta.channel);
        query.bindValue(":session_id", meta.sessionId);
        query.bindValue(":speaker", meta.speaker);
        query.bindValue(":sensitive", meta.sensitive ? 1 : 0);
        query.bindValue(":turn_kind", meta.turnKind.isEmpty() ? QStringLiteral("reply") : meta.turnKind);
        query.bindValue(":trigger_event_ids", meta.triggerEventIds.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(meta.triggerEventIds));
        query.bindValue(":subject", meta.subject.isEmpty() ? QStringLiteral("user") : meta.subject);
        const QDateTime logicalTime = meta.receivedAtUtc.isValid()
            ? meta.receivedAtUtc.toLocalTime() : QDateTime::currentDateTime();
        query.bindValue(":timestamp", logicalTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }
    query.bindValue(":raw_reply", JsonStr);
    if (!query.exec()) {
        qDebug()<<"[MemoryManager]:本轮对话保存失败:"<<query.lastError().text();
        return false;
    }

    if (query.numRowsAffected() != 1) return false;
    const qlonglong indexedTurn = receivedTurn > 0 ? receivedTurn : query.lastInsertId().toLongLong();
    if (indexedTurn > 0 && !indexChatTurn(indexedTurn))
        qWarning() << "[MemoryIndex] incremental chat index failed:" << indexedTurn;
    else if (indexedTurn > 0 && !meta.sensitive)
        emit searchChunksChanged();
    // R12-1b：前台 personal_interest 统一创建 internal 活动（不写 thought、不伪造聊天行）。
    // 创建发生在 chat 行落库之后：失败不会回滚这轮对话，也不会退回 legacy thought。
    if (!personalInterest.isEmpty() && indexedTurn > 0) {
        const auto proactive = ConfigManager::instance().getProactiveConfig();
        if (!proactive.privateWriting) {
            qInfo().noquote()
                << QStringLiteral("[Activity] foreground_create_skipped writing_disabled turn=%1")
                       .arg(indexedTurn);
        } else {
            const QString normalizedEvent = meta.eventId.trimmed();
            const QString runId = normalizedEvent.isEmpty()
                ? QStringLiteral("chat/%1").arg(indexedTurn) : normalizedEvent;
            const QString originRef = QStringLiteral("chat/%1").arg(indexedTurn);
            QString createError;
            const auto created = createActivity(
                QStringLiteral("owner"), meta.channel, meta.sessionId, personalInterest,
                QString(), QStringLiteral("foreground"), originRef, runId, &createError);
            if (created.isEmpty()) {
                if (createError == QStringLiteral("already_active")) {
                    // 单 active：已有活动（legacy/internal）时不重复创建，也绝不退回 legacy。
                    qInfo().noquote()
                        << QStringLiteral("[Activity] foreground_create_skipped already_active"
                                          " run_id=%1 turn=%2")
                               .arg(runId).arg(indexedTurn);
                } else {
                    qWarning().noquote()
                        << QStringLiteral("[Activity] foreground_create_failed error=%1"
                                          " run_id=%2 turn=%3")
                               .arg(createError, runId).arg(indexedTurn);
                    recordCognitiveAudit(
                        QStringLiteral("activity_create_failed"), meta.runId, indexedTurn,
                        QJsonObject{{QStringLiteral("origin_kind"), QStringLiteral("foreground")},
                                    {QStringLiteral("origin_ref"), originRef},
                                    {QStringLiteral("run_id"), runId},
                                    {QStringLiteral("error"), createError},
                                    {QStringLiteral("turn"), indexedTurn}});
                }
            } else {
                qInfo().noquote()
                    << QStringLiteral("[Activity] foreground_created ref=%1 version=%2"
                                      " run_id=%3 turn=%4")
                           .arg(created.value(QStringLiteral("ref")).toString())
                           .arg(created.value(QStringLiteral("version")).toInt())
                           .arg(runId)
                           .arg(indexedTurn);
            }
        }
    }
    // R12-1b：activity_action 统一走 ActivityRef（thought/<id> 或 activity/<id>）。
    // 门控（主体/隐私/长期记忆/渠道/私下写作）与版本校验都在写库前完成；非法引用只留审计，
    // 不写任何半状态。
    const auto activityAction = response["activity_action"].toObject();
    if (!activityAction.isEmpty()) {
        const QString refText = activityAction["activity_ref"].toString().trimmed();
        const QString action = activityAction["action"].toString().trimmed();
        const int expectedVersion = activityAction["expected_version"].toInt(-1);
        const bool writingEnabled = ConfigManager::instance().getProactiveConfig().privateWriting;
        QString rejectReason;
        ActivityRef parsed;
        QString parseError;
        if (meta.subject != "user" || meta.sensitive || !meta.longTermMemoryEnabled
            || (meta.channel != Channel::kPet && meta.channel != Channel::kQQPrivate))
            rejectReason = QStringLiteral("permission_denied");
        else if (!writingEnabled)
            rejectReason = QStringLiteral("writing_disabled");
        else if (!ActivityRef::parse(refText, &parsed, &parseError))
            rejectReason = QStringLiteral("invalid_ref");
        else if (expectedVersion < 0)
            rejectReason = QStringLiteral("invalid_version");
        if (rejectReason.isEmpty()) {
            QString actionError;
            const auto applied = applyActivityAction(
                refText, expectedVersion, action, activityAction["after_minutes"].toInt(-1),
                activityAction["reason"].toString(), &actionError);
            if (applied.isEmpty())
                rejectReason = actionError.isEmpty() ? QStringLiteral("rejected") : actionError;
            else
                qInfo().noquote()
                    << QStringLiteral("[Activity] action_applied ref=%1 action=%2 version=%3->%4")
                           .arg(refText, action)
                           .arg(expectedVersion)
                           .arg(applied.value(QStringLiteral("version")).toInt());
        }
        if (!rejectReason.isEmpty()) {
            recordCognitiveAudit(
                QStringLiteral("activity_action_rejected"), meta.runId, 0,
                QJsonObject{{QStringLiteral("activity_ref"), refText},
                            {QStringLiteral("action"), action},
                            {QStringLiteral("expected_version"), expectedVersion},
                            {QStringLiteral("reason"), rejectReason}});
        }
    }
    qDebug()<<"[MemoryManager]:成功保存本轮对话"<<"channel="<<meta.channel<<"session="<<meta.sessionId;
    return true;

}

bool MemoryManager::recordCognitiveAudit(const QString &kind, const QString &run, qint64 source, const QJsonObject &data)
{
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO cognitive_audit(kind,run,source,data) VALUES(?,?,?,?)");
    q.addBindValue(kind);
    q.addBindValue(run);
    q.addBindValue(source);
    q.addBindValue(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
    if (q.exec()) return true;
    qWarning() << "[Cognition] audit write failed" << q.lastError();
    return false;
}

qint64 MemoryManager::recordInput(const QString &input, const RequestMeta &meta, bool observe)
{
    // R12-2c-2：Action Context Fence 的触发点：相关的**主人私聊输入**（pet/qq_private，
    // subject=user，reply 轮）推进行动语境版本，使旧在途提案/已入 Outbox 未发送的主动动作
    // 失效。无关群聊/第三方/感知轮次不 bump（不能因群消息全局取消主人行动）。
    const bool ownerPrivateInput =
        !input.isEmpty()
        && (meta.channel == Channel::kPet || meta.channel == Channel::kQQPrivate)
        && meta.subject == QStringLiteral("user")
        && meta.turnKind == QStringLiteral("reply");
    if (ownerPrivateInput) bumpActionContextEpoch(QStringLiteral("owner_private_input"));
    if (!meta.persistTurn || meta.eventId.isEmpty() || input.isEmpty()) return 0;
    if (!m_db.transaction()) {
        qWarning() << "[Conversation] receipt transaction failed" << m_db.lastError();
        return 0;
    }
    // The history trigger can now see the source. A failing receipt rolls back both writes.
    if (observe) enqueueObservation(meta, input);
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO chat_history(user_input,raw_reply,channel,session_id,speaker,subject,"
              "sensitive,turn_kind,cognitive_event,turn_state) VALUES(?,'',?,?,?,?,?,?,?,'received')");
    q.addBindValue(input);
    q.addBindValue(meta.channel);
    q.addBindValue(meta.sessionId);
    q.addBindValue(meta.speaker);
    q.addBindValue(meta.subject);
    q.addBindValue(meta.sensitive);
    q.addBindValue(meta.turnKind);
    q.addBindValue(meta.eventId);
    if (q.exec()) {
        const auto id = q.lastInsertId().toLongLong();
        if (!meta.sensitive && !indexChatTurn(id))
            qWarning() << "[MemoryIndex] received input indexing failed:" << id;
        if (m_db.commit()) {
            if (!meta.sensitive) emit searchChunksChanged();
            return id;
        }
    }
    qWarning() << "[Conversation] record input failed" << q.lastError() << m_db.lastError();
    m_db.rollback();
    return 0;
}

bool MemoryManager::linkGroupedInput(qint64 previousTurn, qint64 followingTurn)
{
    QSqlQuery query(m_db);
    query.prepare("UPDATE chat_history SET turn_state='grouped',turn_detail=? WHERE id=? AND turn_state='received'");
    query.addBindValue(QString::number(followingTurn));
    query.addBindValue(previousTurn);
    return query.exec() && query.numRowsAffected() == 1;
}

bool MemoryManager::recordQuiet(qint64 turn, const QString &action)
{
    if (action != "close" && action != "defer") return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET turn_state='quiet',turn_detail=? WHERE id=? AND turn_state='received'");
    q.addBindValue(action);
    q.addBindValue(turn);
    return q.exec() && q.numRowsAffected() == 1;
}

bool MemoryManager::interruptTurn(qint64 turn, const QString &reason)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET turn_state='interrupted',turn_detail=? WHERE id=? AND turn_state='received'");
    q.addBindValue(reason);
    q.addBindValue(turn);
    return q.exec() && q.numRowsAffected() == 1;
}

QJsonObject MemoryManager::nextThought(bool dueOnly) const
{
    QSqlQuery q(m_db);
    q.prepare(R"(
        SELECT id,thought,thought_version,thought_result FROM chat_history
        WHERE thought IS NOT NULL AND thought<>'' AND subject='user' AND sensitive=0
          AND (:due=0 OR (coalesce(json_extract(thought_result,'$.status'),'active')='active'
               AND coalesce(review_after,0)<=unixepoch()))
        ORDER BY CASE WHEN :due=1 THEN coalesce(review_after,0) ELSE 0 END,
                 id DESC LIMIT 1
    )");
    q.bindValue(":due", int(dueOnly));
    if (!q.exec() || !q.next())
        return {};
    return {{"id",q.value(0).toLongLong()},{"interest",q.value(1).toString()},
            {"version",q.value(2).toInt()},
            {"last_review",QJsonDocument::fromJson(q.value(3).toByteArray()).object()}};
}

QJsonObject MemoryManager::activityById(qlonglong id) const
{
    if (id <= 0) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT id,thought,thought_version,thought_result FROM chat_history "
              "WHERE id=? AND thought IS NOT NULL AND thought<>'' AND sensitive=0");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return {};
    return {{"ref",QStringLiteral("thought/%1").arg(id)},
            {"id",q.value(0).toLongLong()},
            {"interest",q.value(1).toString()},
            {"version",q.value(2).toInt()},
            {"result",QJsonDocument::fromJson(q.value(3).toByteArray()).object()}};
}

bool MemoryManager::chargePrivateReview(qlonglong turn, int dailyLimit, int cooldownSeconds,
                                        QString *reason, QDateTime *nextEligibleAt,
                                        const QString &sourceKind, const QString &sourceRef,
                                        const QString &runId, int sourceDailyLimit)
{
    if (reason) reason->clear();
    if (nextEligibleAt) *nextEligibleAt = QDateTime{};
    const auto nextLocalDay = []() {
        return QDateTime(QDate::currentDate().addDays(1), QTime(0, 0)).toUTC();
    };
    if (dailyLimit <= 0) {
        if (reason) *reason = QStringLiteral("daily_limit");
        if (nextEligibleAt) *nextEligibleAt = nextLocalDay();
        return false;
    }
    // 独立来源预算（OpenWake）：仍受共享每日总量与冷却约束。取整时保证正预算至少有一次
    // 机会，但绝不突破共享 dailyLimit 或配置的 openWakeDailyLimit。
    const bool perSourceCap = sourceDailyLimit > 0 && !sourceKind.isEmpty();
    const int cooldown = qMax(60, cooldownSeconds);
    QString sql = QStringLiteral(
        "INSERT INTO thought_review_log(turn_id,started_at,source_kind,source_ref,run_id) "
        "SELECT ?,unixepoch(),?,?,? WHERE (SELECT count(*) FROM thought_review_log "
        "WHERE date(started_at,'unixepoch','localtime')=date('now','localtime')) < ? "
        "AND coalesce((SELECT max(started_at) FROM thought_review_log),0)+?<=unixepoch()");
    if (perSourceCap) {
        sql += QStringLiteral(
            " AND (SELECT count(*) FROM thought_review_log "
            "WHERE date(started_at,'unixepoch','localtime')=date('now','localtime') "
            "AND source_kind=?) < ?");
    }
    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(turn);
    q.addBindValue(sourceKind.isEmpty() ? QVariant() : QVariant(sourceKind));
    q.addBindValue(sourceRef.isEmpty() ? QVariant() : QVariant(sourceRef));
    q.addBindValue(runId.isEmpty() ? QVariant() : QVariant(runId));
    q.addBindValue(dailyLimit);
    q.addBindValue(cooldown);
    if (perSourceCap) {
        q.addBindValue(sourceKind);
        q.addBindValue(sourceDailyLimit);
    }
    if (!q.exec()) {
        if (reason) *reason = QStringLiteral("db_error");
        return false;
    }
    if (q.numRowsAffected() == 1) return true;
    // 未插入：再查一次当日已用次数、上次开始时间与该来源今日次数，把原因分开。
    // 只读诊断，不影响预算/冷却语义。
    QString diagSql = QStringLiteral(
        "SELECT (SELECT count(*) FROM thought_review_log "
        "WHERE date(started_at,'unixepoch','localtime')=date('now','localtime')),"
        "coalesce((SELECT max(started_at) FROM thought_review_log),0),unixepoch()");
    if (perSourceCap)
        diagSql += QStringLiteral(
            ",(SELECT count(*) FROM thought_review_log "
            "WHERE date(started_at,'unixepoch','localtime')=date('now','localtime') "
            "AND source_kind=?)");
    QSqlQuery diag(m_db);
    diag.prepare(diagSql);
    if (perSourceCap) diag.addBindValue(sourceKind);
    if (!diag.exec() || !diag.next()) {
        if (reason) *reason = QStringLiteral("db_error");
        return false;
    }
    const int used = diag.value(0).toInt();
    const qlonglong last = diag.value(1).toLongLong();
    const qlonglong now = diag.value(2).toLongLong();
    const int sourceUsed = perSourceCap ? diag.value(3).toInt() : 0;
    if (used >= dailyLimit) {
        if (reason) *reason = QStringLiteral("daily_limit");
        if (nextEligibleAt) *nextEligibleAt = nextLocalDay();
    } else if (perSourceCap && sourceUsed >= sourceDailyLimit) {
        if (reason) *reason = QStringLiteral("budget");
        if (nextEligibleAt) *nextEligibleAt = nextLocalDay();
    } else if (last > 0 && last + cooldown > now) {
        if (reason) *reason = QStringLiteral("cooldown");
        if (nextEligibleAt)
            *nextEligibleAt = QDateTime::fromSecsSinceEpoch(last + cooldown).toUTC();
    } else {
        if (reason) *reason = QStringLiteral("db_error");
    }
    return false;
}

bool MemoryManager::claimThoughtReview(qlonglong id, int version, int dailyLimit, int cooldownSeconds,
                                       QString *reason, QDateTime *nextEligibleAt)
{
    if (reason) reason->clear();
    if (nextEligibleAt) *nextEligibleAt = QDateTime{};
    if (dailyLimit <= 0) {
        if (reason) *reason = QStringLiteral("daily_limit");
        return false;
    }
    if (!m_db.transaction()) {
        if (reason) *reason = QStringLiteral("db_error");
        return false;
    }
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET review_after=unixepoch()+1800,thought_version=thought_version+1 "
              "WHERE id=? AND thought IS NOT NULL AND thought_version=? AND sensitive=0 AND subject='user' "
              "AND coalesce(json_extract(thought_result,'$.status'),'active')='active' "
              "AND coalesce(review_after,0)<=unixepoch()");
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec()) {
        if (reason) *reason = QStringLiteral("db_error");
        m_db.rollback();
        return false;
    }
    if (q.numRowsAffected() != 1) {
        // 版本/状态/到期条件不再满足（迟到回调或并发领取）。
        if (reason) *reason = QStringLiteral("cas_conflict");
        m_db.rollback();
        return false;
    }
    // R12-1a：暂停到期后的恢复尝试（重置连续失败计数，保留 last_reason）。
    if (!recoverSuspendedFailure(QStringLiteral("activity"), QStringLiteral("thought/%1").arg(id))) {
        if (reason) *reason = QStringLiteral("db_error");
        m_db.rollback();
        return false;
    }
    QString chargeReason;
    QDateTime next;
    if (!chargePrivateReview(id, dailyLimit, cooldownSeconds, &chargeReason, &next,
                             QStringLiteral("activity"), QStringLiteral("thought/%1").arg(id))) {
        if (reason) *reason = chargeReason.isEmpty() ? QStringLiteral("db_error") : chargeReason;
        if (nextEligibleAt) *nextEligibleAt = next;
        m_db.rollback();
        return false;
    }
    // 公平性：成功领取非 OpenWake 任务时，在同一事务内推进持久化公平计数。
    if (!bumpOpenWakeFairness()) {
        if (reason) *reason = QStringLiteral("db_error");
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        if (reason) *reason = QStringLiteral("db_error");
        m_db.rollback();
        return false;
    }
    return true;
}

bool MemoryManager::deferThoughtReview(qlonglong id, int version, const QString &reason)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET thought_result=json_set(coalesce(thought_result,'{}'),'$.deferred_reason',?),"
              "thought_version=thought_version+1,review_after=0 WHERE id=? AND thought_version=? AND thought IS NOT NULL");
    q.addBindValue(reason);
    q.addBindValue(id);
    q.addBindValue(version);
    return q.exec() && q.numRowsAffected()==1;
}

bool MemoryManager::finishThoughtReview(qlonglong id, int version, const QJsonObject &proposal)
{
    const auto status = proposal["status"].toString();
    if (status != "active" && status != "paused" && status != "completed"
        && status != "abandoned")
        return false;
    // A full draft is a replacement. Truncating it would silently lose earlier work.
    if (proposal["draft"].toString().trimmed().size() > 800)
        return false;
    QJsonObject result{{"status",status},{"deferred_reason",QJsonValue::Null},{"paused_reason",QJsonValue::Null}};
    // R12-1b 补充：completion_condition/activity_kind 是"纯思考类活动能否完成"的判据，
    // 必须在 active/paused 阶段就能持久化，后续完成时才能沿用已有条件。
    for (const auto &key : {"reflection","goal","progress","next_step","draft","mood",
                            "paused_reason","completion_condition","activity_kind"}) {
        const int limit = (QString(key) == "draft") ? 800 : 200;
        const QString value = proposal[key].toString().trimmed().left(limit);
        if (!value.isEmpty())
            result[key] = value;
    }
    if (!result.contains("reflection"))
        return false;
    // B2：每次回顾提交都刷新 updated_at；产生草稿时记录产物引用。
    // thought_result 用 json_patch 合并（而不是整行覆盖），用户/调用方已有字段不会被清掉；
    // artifact_ref 只在确有草稿时写入，纯"继续思考"的回顾不会伪造产物。
    result[QStringLiteral("updated_at")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!result.value(QStringLiteral("draft")).toString().isEmpty()) {
        result[QStringLiteral("artifact_ref")] = QStringLiteral("thought/%1").arg(id);
        // R12-1b：提交后版本 + 正文哈希，完成成果可被核验（旧记录缺这两个字段时只读兼容）。
        result[QStringLiteral("artifact_version")] = version + 1;
        result[QStringLiteral("artifact_hash")] =
            activityContentHash(result.value(QStringLiteral("draft")).toString());
    }
    if (status == QStringLiteral("completed"))
        result[QStringLiteral("completed_at")] = QDateTime::currentSecsSinceEpoch();
    const int hours = qBound(1, proposal["review_hours"].toInt(24), 168);
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET thought_result=json_patch(coalesce(thought_result,'{}'),?),thought_version=thought_version+1,"
              "review_after=unixepoch()+? WHERE id=? AND thought_version=? AND thought IS NOT NULL AND sensitive=0 "
              "AND coalesce(json_extract(thought_result,'$.status'),'active')='active'");
    q.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    q.addBindValue(hours*3600);
    q.addBindValue(id);
    q.addBindValue(version);
    return q.exec() && q.numRowsAffected()==1;
}

bool MemoryManager::applyThoughtAction(qlonglong id, int expectedVersion, const QString &action,
                                     int afterMinutes, const QString &reason)
{
    if (action == "reschedule") {
        if (afterMinutes<0 || afterMinutes>7*24*60 || reason.trimmed().isEmpty() || reason.size()>200)
            return false;
        QSqlQuery q(m_db);
        q.prepare("UPDATE chat_history SET review_after=unixepoch()+?,thought_version=thought_version+1,"
                  "thought_result=json_set(coalesce(thought_result,'{}'),'$.schedule_reason',?) "
                  "WHERE id=? AND thought_version=? AND thought IS NOT NULL AND sensitive=0 "
                  "AND coalesce(json_extract(thought_result,'$.status'),'active')='active'");
        q.addBindValue(afterMinutes*60);
        q.addBindValue(reason.trimmed());
        q.addBindValue(id);
        q.addBindValue(expectedVersion);
        return q.exec() && q.numRowsAffected()==1;
    }
    const QString status = action == "pause" ? QStringLiteral("paused")
        : action == "resume" ? QStringLiteral("active")
        : action == "abandon" ? QStringLiteral("abandoned")
        : QString{};
    if (status.isEmpty() || id <= 0 || expectedVersion < 0)
        return false;
    QJsonObject patch{{"status", status}};
    patch["paused_reason"] = QJsonValue::Null;
    if (action == "pause") patch["paused_reason"] = QStringLiteral("user_paused");
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET thought_result=json_patch(coalesce(thought_result,'{}'),?),"
              "thought_version=thought_version+1,review_after=0 "
              "WHERE id=? AND thought_version=? AND thought IS NOT NULL AND sensitive=0 "
              "AND CASE coalesce(json_extract(thought_result,'$.status'),'active') "
              "WHEN 'paused' THEN ? IN ('resume','abandon') WHEN 'active' THEN ? IN ('pause','abandon') ELSE 0 END");
    q.addBindValue(QString::fromUtf8(QJsonDocument(patch).toJson(QJsonDocument::Compact)));
    q.addBindValue(id);
    q.addBindValue(expectedVersion);
    q.addBindValue(action);
    q.addBindValue(action);
    if (!q.exec() || q.numRowsAffected()!=1) {
        qWarning() << "[MemoryManager]activity action rejected:" << id << expectedVersion << action;
        return false;
    }
    return true;
}

bool MemoryManager::hasActiveThought(const QString &subject) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
              "AND subject=? AND sensitive=0 AND coalesce(json_extract(thought_result,'$.status'),'active') "
              "IN ('active','paused') LIMIT 1");
    q.addBindValue(subject);
    return q.exec() && q.next();
}

bool MemoryManager::canShareThought(qlonglong id, int expectedVersion) const
{
    QSqlQuery q(m_db);
    // B2：没有实际产物（artifact_ref 为空）的活动不可分享——只写着"在思考"的阶段不打扰主人。
    // 版本/状态校验与 M2 的分享去重规则保持不变。
    q.prepare("SELECT coalesce(json_extract(thought_result,'$.share_outcome'),''),"
              "coalesce(json_extract(thought_result,'$.share_version'),-1) "
              "FROM chat_history WHERE id=? AND thought IS NOT NULL AND sensitive=0 AND thought_version=? "
              "AND coalesce(json_extract(thought_result,'$.status'),'active') IN ('active','completed') "
              "AND coalesce(json_extract(thought_result,'$.artifact_ref'),'')<>''");
    q.addBindValue(id);
    q.addBindValue(expectedVersion);
    if (!q.exec() || !q.next()) return false;
    // 分享去重只针对"同一版本（同一份草稿）"：新一轮回顾会产生新版本，新草稿仍可再分享。
    // 同一版本内：已送达（handed_off/protocol_accepted）或协议层"未知"不再重发；
    // 渠道明确拒绝时最多补齐一次，失败次数记在 delivery_ledger.attempts 上。
    const qlonglong shareVersion = q.value(1).toLongLong();
    if (shareVersion != expectedVersion) return true;
    const QString shareOutcome = q.value(0).toString();
    if (shareOutcome == QStringLiteral("handed_off")
        || shareOutcome == QStringLiteral("protocol_accepted")
        || shareOutcome == QStringLiteral("unknown"))
        return false;
    if (shareOutcome == QStringLiteral("rejected")) {
        QSqlQuery ledger(m_db);
        ledger.prepare("SELECT attempts FROM delivery_ledger WHERE kind='share' AND ref=?");
        ledger.addBindValue(QStringLiteral("thought:%1:%2").arg(id).arg(expectedVersion));
        if (!ledger.exec() || !ledger.next() || ledger.value(0).toInt() >= 2) return false;
    }
    return true;
}

bool MemoryManager::recordShareOutcome(qlonglong activityId, int version, const QString &outcome,
                                       const QString &channel, const QString &session)
{
    static const QSet<QString> states{
        QStringLiteral("handed_off"), QStringLiteral("protocol_accepted"), QStringLiteral("rejected"),
        QStringLiteral("unknown"), QStringLiteral("withheld"), QStringLiteral("expired")};
    if (activityId <= 0 || version < 0 || !states.contains(outcome)) return false;
    const bool retryable = outcome == QStringLiteral("rejected") || outcome == QStringLiteral("unknown");
    const QString ref = QStringLiteral("thought:%1:%2").arg(activityId).arg(version);
    const QString at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (!m_db.transaction()) return false;
    QSqlQuery q(m_db);
    // 先打补丁到 thought_result：分享结果跟随活动存档，重启后仍能判断是否已送达。
    // 这里不改 thought_version：回执只是投递元数据，不是一次新的活动修订。
    q.prepare("UPDATE chat_history SET thought_result=json_patch(coalesce(thought_result,'{}'),?) "
              "WHERE id=? AND thought IS NOT NULL AND sensitive=0");
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("share_outcome"), outcome},
        {QStringLiteral("share_version"), version},
        {QStringLiteral("share_channel"), channel},
        {QStringLiteral("share_at"), at}}).toJson(QJsonDocument::Compact)));
    q.addBindValue(activityId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "[MemoryManager]分享回执落库失败:" << activityId << version << outcome << q.lastError();
        m_db.rollback();
        return false;
    }
    // 投递流水：重复回执只累加失败尝试，不产生重复行；channel/session 留空时不覆盖旧值。
    q.prepare("INSERT INTO delivery_ledger(kind,ref,channel,session,state,attempts,updated_at) "
              "VALUES('share',?,?,?,?,?,CURRENT_TIMESTAMP) ON CONFLICT(kind,ref) DO UPDATE SET "
              "state=excluded.state,"
              "channel=coalesce(excluded.channel,delivery_ledger.channel),"
              "session=coalesce(excluded.session,delivery_ledger.session),"
              "attempts=delivery_ledger.attempts+excluded.attempts,"
              "updated_at=CURRENT_TIMESTAMP");
    q.addBindValue(ref);
    q.addBindValue(channel.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(channel));
    q.addBindValue(session.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(session));
    q.addBindValue(outcome);
    q.addBindValue(retryable ? 1 : 0);
    if (!q.exec() || !m_db.commit()) {
        qWarning() << "[MemoryManager]分享投递流水写入失败:" << ref << q.lastError();
        m_db.rollback();
        return false;
    }
    qInfo() << "[Share] outcome" << ref << outcome << channel << session;
    return true;
}

// ============== 普通回复的投递账本（kind='reply'，复用 delivery_ledger，无迁移） ==============
// 与 contact/share 共用同一张表：这里只提供通用的 claim/attempt/outcome/retry 读写，
// 不绑定具体渠道或场景；投递策略（何时补发、补几次）由调用方决定。
bool MemoryManager::recordDelivery(const QString &kind, const QString &ref,
                                   const QString &channel, const QString &session,
                                   const QString &text)
{
    if (!m_db.isOpen() || kind.isEmpty() || ref.isEmpty()) return false;
    QSqlQuery q(m_db);
    // 首次投递即 claim。同一 (kind,ref) 重复 claim（例如重启后重发同一句）
    // 只刷新 channel/session/text，attempts 保留，避免把"已经补发过几次"冲成 0。
    q.prepare("INSERT INTO delivery_ledger(kind,ref,channel,session,text,state,attempts,updated_at) "
              "VALUES(?,?,?,?,?,'claimed',0,CURRENT_TIMESTAMP) "
              "ON CONFLICT(kind,ref) DO UPDATE SET "
              "channel=excluded.channel,"
              "session=excluded.session,"
              "text=excluded.text,"
              "state='claimed',"
              "updated_at=CURRENT_TIMESTAMP");
    q.addBindValue(kind);
    q.addBindValue(ref);
    q.addBindValue(channel.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(channel));
    q.addBindValue(session.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(session));
    q.addBindValue(text.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(text));
    if (!q.exec()) {
        qWarning() << "[Delivery] claim 写入失败" << kind << ref << q.lastError();
        return false;
    }
    return true;
}

bool MemoryManager::recordDeliveryAttempt(const QString &kind, const QString &ref)
{
    if (!m_db.isOpen() || kind.isEmpty() || ref.isEmpty()) return false;
    QSqlQuery q(m_db);
    // 发送前先占一次尝试并把状态拉回 claimed：同一行不会被下一个 5s 补发周期
    // 同时选中；真正的结果仍由 sendResult 回执覆盖。
    q.prepare("UPDATE delivery_ledger SET attempts=attempts+1,state='claimed',"
              "updated_at=CURRENT_TIMESTAMP WHERE kind=? AND ref=?");
    q.addBindValue(kind);
    q.addBindValue(ref);
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "[Delivery] attempt 写入失败" << kind << ref << q.lastError();
        return false;
    }
    return true;
}

bool MemoryManager::recordDeliveryOutcome(const QString &kind, const QString &ref,
                                          const QString &outcome, const QString &detail)
{
    if (!m_db.isOpen() || kind.isEmpty() || ref.isEmpty() || outcome.isEmpty()) return false;
    QSqlQuery q(m_db);
    // 只落终态与诊断信息：attempts 只由发送方在发送前递增，回执不重复计数。
    q.prepare("UPDATE delivery_ledger SET state=?,last_error=?,updated_at=CURRENT_TIMESTAMP "
              "WHERE kind=? AND ref=?");
    q.addBindValue(outcome);
    q.addBindValue(detail.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(detail));
    q.addBindValue(kind);
    q.addBindValue(ref);
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "[Delivery] outcome 写入失败" << kind << ref << outcome << q.lastError();
        return false;
    }
    return true;
}

QJsonArray MemoryManager::retryableDeliveries(const QString &kind, int maxAttempts, int limit) const
{
    QJsonArray result;
    if (!m_db.isOpen() || kind.isEmpty() || maxAttempts <= 0 || limit <= 0) return result;
    QSqlQuery q(m_db);
    // 只取"渠道明确拒绝、且还没用完补发次数"的行；unknown（协议可能已送达）永不入选。
    q.prepare("SELECT kind,ref,channel,session,text,attempts,state FROM delivery_ledger "
              "WHERE kind=? AND state='rejected' AND attempts<? ORDER BY id LIMIT ?");
    q.addBindValue(kind);
    q.addBindValue(maxAttempts);
    q.addBindValue(limit);
    if (!q.exec()) {
        qWarning() << "[Delivery] retry 查询失败" << kind << q.lastError();
        return result;
    }
    while (q.next()) {
        result.append(QJsonObject{{QStringLiteral("kind"), q.value(0).toString()},
                                  {QStringLiteral("ref"), q.value(1).toString()},
                                  {QStringLiteral("channel"), q.value(2).toString()},
                                  {QStringLiteral("session"), q.value(3).toString()},
                                  {QStringLiteral("text"), q.value(4).toString()},
                                  {QStringLiteral("attempts"), q.value(5).toInt()},
                                  {QStringLiteral("state"), q.value(6).toString()}});
    }
    return result;
}

// ============== B1：统一注意力事件与当前活动状态（v25） ==============
qlonglong MemoryManager::enqueueAttentionEvent(const QString &kind, const QString &ref,
                                               const QJsonObject &payload, const QString &reason,
                                               int priority)
{
    if (!m_db.isOpen() || kind.isEmpty() || ref.isEmpty())
        return 0;
    const QString payloadJson = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QSqlQuery insert(m_db);
    // 同一 (kind,ref) 只保留一行：重复入队不重置已有状态，也不覆盖已有 payload/priority。
    insert.prepare("INSERT INTO attention_event(kind,ref,payload,priority,reason,created_at) "
                   "VALUES(?,?,?,?,?,unixepoch()) ON CONFLICT(kind,ref) DO NOTHING");
    insert.addBindValue(kind);
    insert.addBindValue(ref);
    insert.addBindValue(payload.isEmpty() ? QStringLiteral("{}") : payloadJson);
    insert.addBindValue(qBound(0, priority, 100));
    insert.addBindValue(reason.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(reason));
    if (!insert.exec()) {
        qWarning() << "[Attention] enqueue failed" << kind << ref << insert.lastError();
        return 0;
    }
    // 无论本次是插入还是冲突跳过，都回读该行 id：调用方需要它来落终态。
    QSqlQuery existing(m_db);
    existing.prepare("SELECT id FROM attention_event WHERE kind=? AND ref=?");
    existing.addBindValue(kind);
    existing.addBindValue(ref);
    if (!existing.exec() || !existing.next())
        return 0;
    return existing.value(0).toLongLong();
}

int MemoryManager::reconcileAttentionNotifications()
{
    if (!m_db.isOpen()) return -1;
    QSqlQuery update(m_db);
    // At startup there is no live message queue. Received turns are reconciled separately.
    // Activity notifications are audit projections of results that were already committed.
    if (!update.exec("UPDATE attention_event SET "
                     "state=CASE kind WHEN 'message' THEN 'abandoned' ELSE 'done' END,"
                     "done_at=unixepoch(),"
                     "reason=CASE kind WHEN 'message' THEN 'restart_notification' "
                     "ELSE 'activity_result_recorded' END "
                     "WHERE state IN ('pending','claimed') AND kind IN ('message','activity_result')"))
        return -1;
    return update.numRowsAffected();
}

QJsonObject MemoryManager::claimAttentionEvent(const QString &kind)
{
    if (!m_db.isOpen() || !m_db.transaction())
        return {};
    QJsonObject event;
    {
        QSqlQuery select(m_db);
        select.prepare("SELECT id,kind,ref,payload,priority,created_at FROM attention_event "
                       "WHERE state='pending' AND (?='' OR kind=?) ORDER BY priority,id LIMIT 1");
        select.addBindValue(kind.isNull() ? QStringLiteral("") : kind);
        select.addBindValue(kind.isNull() ? QStringLiteral("") : kind);
        if (!select.exec() || !select.next()) {
            m_db.rollback();
            return {};
        }
        event = QJsonObject{
            {QStringLiteral("id"), select.value(0).toLongLong()},
            {QStringLiteral("kind"), select.value(1).toString()},
            {QStringLiteral("ref"), select.value(2).toString()},
            {QStringLiteral("payload"), QJsonDocument::fromJson(select.value(3).toByteArray()).object()},
            {QStringLiteral("priority"), select.value(4).toInt()},
            {QStringLiteral("created_at"), select.value(5).toLongLong()}};
    }
    QSqlQuery claim(m_db);
    claim.prepare("UPDATE attention_event SET state='claimed',claimed_at=unixepoch() "
                  "WHERE id=? AND state='pending'");
    claim.addBindValue(event.value(QStringLiteral("id")).toInteger());
    if (!claim.exec() || claim.numRowsAffected() != 1 || !m_db.commit()) {
        m_db.rollback();
        return {};
    }
    return event;
}

bool MemoryManager::completeAttentionEvent(qlonglong id, const QString &state, const QString &reason)
{
    if (id <= 0 || (state != QStringLiteral("done") && state != QStringLiteral("abandoned")))
        return false;
    QSqlQuery q(m_db);
    // 允许 pending→done：message/due_wake 这类"直接观测"事件没有独占领取者；
    // claimed→done/abandoned 是 recovery/activity_result 这类独占处理者的正常终态。
    // 终态不可再次变更（同一事件重复 complete 返回 false）。
    q.prepare("UPDATE attention_event SET state=?,done_at=unixepoch(),"
              "reason=coalesce(nullif(?,''),reason) WHERE id=? AND state IN ('pending','claimed')");
    q.addBindValue(state);
    q.addBindValue(reason);
    q.addBindValue(id);
    return q.exec() && q.numRowsAffected() == 1;
}

QJsonObject MemoryManager::attentionBacklog() const
{
    QJsonObject pendingKinds;
    QJsonObject byState;
    qlonglong oldest = 0;
    if (!m_db.isOpen()) {
        return {{QStringLiteral("pending"), pendingKinds},
                {QStringLiteral("by_state"), byState},
                {QStringLiteral("oldest_age_seconds"), oldest}};
    }
    QSqlQuery states(m_db);
    if (states.exec("SELECT state,count(*) FROM attention_event GROUP BY state")) {
        while (states.next())
            byState.insert(states.value(0).toString(), states.value(1).toInt());
    }
    QSqlQuery pending(m_db);
    if (pending.exec("SELECT kind,count(*) FROM attention_event WHERE state='pending' GROUP BY kind")) {
        while (pending.next())
            pendingKinds.insert(pending.value(0).toString(), pending.value(1).toInt());
    }
    // 没有 pending 时 min(created_at) 为 NULL，整体表达式也是 NULL，此处归 0。
    QSqlQuery age(m_db);
    if (age.exec("SELECT unixepoch()-min(created_at) FROM attention_event WHERE state='pending'")
        && age.next() && !age.value(0).isNull())
        oldest = qMax<qlonglong>(0, age.value(0).toLongLong());
    return {{QStringLiteral("pending"), pendingKinds},
            {QStringLiteral("by_state"), byState},
            {QStringLiteral("oldest_age_seconds"), oldest}};
}

void MemoryManager::setAttentionState(const QString &key, const QString &value)
{
    if (!m_db.isOpen() || key.isEmpty())
        return;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO attention_state(key,value,updated_at) VALUES(?,?,unixepoch()) "
              "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at");
    q.addBindValue(key);
    q.addBindValue(value);
    if (!q.exec())
        qWarning() << "[Attention] state write failed" << key << q.lastError();
}

QString MemoryManager::attentionState(const QString &key) const
{
    if (!m_db.isOpen() || key.isEmpty())
        return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM attention_state WHERE key=?");
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return {};
    return q.value(0).toString();
}

// R12-2c-2：Action Context Fence 的持久化版本。沿用 attention_state 的键值存档，
// 不新增表：action_context_epoch=整数（缺失视为 0），action_context_updated_at=unix 秒，
// action_context_reason=最近一次 bump 的原因（只做审计，最长 80 字）。
// 该值跨重启可验证、绝不清零；提案落库时快照到 action_proposal.action_context_epoch。
qint64 MemoryManager::actionContextEpoch() const
{
    const QString value = attentionState(QStringLiteral("action_context_epoch")).trimmed();
    if (value.isEmpty()) return 0;
    bool ok = false;
    const qint64 epoch = value.toLongLong(&ok);
    return (ok && epoch >= 0) ? epoch : 0;
}

qint64 MemoryManager::bumpActionContextEpoch(const QString &reason)
{
    if (!m_db.isOpen()) return -1;
    if (!m_db.transaction()) return -1;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const auto upsertState = [this](const QString &key, const QString &value) -> bool {
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO attention_state(key,value,updated_at) VALUES(?,?,unixepoch()) "
                  "ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
                  "updated_at=excluded.updated_at");
        q.addBindValue(key);
        q.addBindValue(value);
        return q.exec();
    };
    // 单条 UPSERT 原子自增：并发/重启都不会读到中间值，也不会覆盖成 0。
    QSqlQuery increase(m_db);
    if (!increase.exec(QStringLiteral(
            "INSERT INTO attention_state(key,value,updated_at) "
            "VALUES('action_context_epoch','1',unixepoch()) "
            "ON CONFLICT(key) DO UPDATE SET "
            "value=CAST(coalesce(CAST(attention_state.value AS INTEGER),0)+1 AS TEXT),"
            "updated_at=excluded.updated_at"))) {
        qWarning() << "[Fence] bump failed" << increase.lastError();
        m_db.rollback();
        rollback.dismiss();
        return -1;
    }
    bool ok = false;
    qint64 epoch = -1;
    {
        QSqlQuery read(m_db);
        read.prepare("SELECT value FROM attention_state WHERE key='action_context_epoch'");
        if (read.exec() && read.next())
            epoch = read.value(0).toString().toLongLong(&ok);
    }
    if (!ok || epoch < 0) {
        m_db.rollback();
        rollback.dismiss();
        return -1;
    }
    const QString auditReason = reason.trimmed().left(80);
    const QString nowText = QString::number(QDateTime::currentSecsSinceEpoch());
    if (!upsertState(QStringLiteral("action_context_updated_at"), nowText)
        || !upsertState(QStringLiteral("action_context_reason"), auditReason)) {
        m_db.rollback();
        rollback.dismiss();
        return -1;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return -1;
    }
    rollback.dismiss();
    qInfo() << "[Fence] action_context_epoch=" << epoch
            << "reason=" << (auditReason.isEmpty() ? QStringLiteral("-") : auditReason);
    return epoch;
}

int MemoryManager::enqueueRecoveryEvents(int olderThanSeconds)
{
    if (!m_db.isOpen())
        return 0;
    // timestamp 是本地时区字段（recordInput/saveQATurn 都按本地时间写入），比较时
    // 先用 'localtime' 转到本地再回退 N 秒，与 decayEpisodicMemory 等既有写法一致。
    const int age = qMax(0, olderThanSeconds);
    QSqlQuery received(m_db);
    received.prepare(QStringLiteral(
        "SELECT id,channel,session_id FROM chat_history "
        "WHERE turn_state='received' AND julianday(timestamp)<=julianday('now','localtime','-%1 seconds')")
        .arg(age));
    if (!received.exec()) {
        qWarning() << "[Attention] recovery scan failed" << received.lastError();
        return 0;
    }
    QList<QVariantList> rows;
    while (received.next())
        rows.append(QVariantList{received.value(0), received.value(1), received.value(2)});
    int queued = 0;
    for (const auto &row : rows) {
        const qlonglong turn = row.at(0).toLongLong();
        if (turn <= 0)
            continue;
        const QString ref = QStringLiteral("turn:%1").arg(turn);
        // 幂等：该轮次已有 recovery 记录（无论状态）就不再入队，重复扫描返回 0。
        QSqlQuery seen(m_db);
        seen.prepare("SELECT 1 FROM attention_event WHERE kind='recovery' AND ref=?");
        seen.addBindValue(ref);
        if (seen.exec() && seen.next())
            continue;
        const QJsonObject payload{{QStringLiteral("turn"), turn},
                                  {QStringLiteral("channel"), row.at(1).toString()},
                                  {QStringLiteral("session"), row.at(2).toString()}};
        if (enqueueAttentionEvent(QStringLiteral("recovery"), ref, payload,
                                  QStringLiteral("interrupted_turn"), 10) > 0)
            ++queued;
    }
    return queued;
}

QJsonArray MemoryManager::structuredMemoryRows(const QStringList &kinds, const QString &subject,
                                              bool includeOthers, bool allEpisodic) const
{
    // Structured evidence is read from its authoritative table. It has no embedding
    // and retains its source reference when ranked alongside indexed messages.
    QJsonArray rows;
    auto collect = [&](const QString &kind, const QString &sql, const QVariantList &bindings = {}) {
        if (!kinds.isEmpty() && !kinds.contains(kind))
            return;
        QSqlQuery query(m_db);
        query.prepare(sql);
        for (const auto &binding : bindings)
            query.addBindValue(binding);
        if (!query.exec()) {
            qWarning() << "[MemoryRetrieval]" << kind << query.lastError();
            return;
        }
        while (query.next()) {
            // importance 只有情景记忆提供；其它结构化查询没有第 10 列，取值退化为 0。
            rows.append(QJsonObject{{"source_type", kind}, {"source_ref", kind + "/" + query.value(0).toString()},
                {"text", query.value(1).toString()}, {"channel", query.value(2).toString()},
                {"session", query.value(3).toString()}, {"speaker_id", query.value(4).toString()},
                {"speaker_role", query.value(4).isNull() ? "unknown" : "user"},
                {"speech_at", query.value(5).toString()}, {"event_time", query.value(6).toString()},
                {"recorded_at", query.value(7).toString()}, {"derived", query.value(8).toString()},
                {"importance", query.value(9).toDouble()}});
        }
    };
    collect("episode", "SELECT id,content,origin,session_id,subject,NULL,event_time,content_recorded_at,'memory',importance "
            "FROM episodic_memory WHERE status='active' AND (subject=? OR ?) AND (origin='pet' OR ?)",
            {subject, includeOthers, allEpisodic});
    if (includeOthers) {
        collect("interaction", "SELECT id,safe_summary,channel,session_id,NULL,ended_at,NULL,content_recorded_at,'summary' "
                "FROM interactions WHERE summary_status='ready' AND safe_summary IS NOT NULL");
        collect("summary", "SELECT id,summary_text,channel,session_id,NULL,NULL,NULL,updated_at,'summary' "
                "FROM long_term_summary WHERE is_dirty=0");
        collect("claim", "SELECT id,statement,source_channel,source_session_id,asserted_by_person_id,NULL,"
                "valid_time_start,updated_at,'claim' FROM memory_claims WHERE claim_status='active' AND privacy_level<3");
    }
    collect("source", "SELECT id,input,channel,session,subject,reported,NULL,reported,'original' "
            "FROM cognitive_source WHERE subject=? AND outcome IS NULL", {subject});
    collect("revision", "SELECT r.matter||'/'||r.version,r.quote,s.channel,s.session,s.subject,"
            "s.reported,r.event_time,r.recorded,r.kind FROM cognitive_source s "
            "JOIN matter_revision r ON r.source=s.id WHERE s.subject=? OR ?", {subject, includeOthers});
    collect("legacy", "SELECT h.id||'/'||j.key,json_extract(j.value,'$.quote'),h.channel,h.session_id,"
            "h.subject,h.timestamp,NULL,h.timestamp,'inference' FROM chat_history h,json_each(h.state_updates) j "
            "WHERE (h.subject=? OR ?) AND h.sensitive=0", {subject, includeOthers});
    return rows;
}

bool MemoryManager::rebuildSearchIndex(QString *error)
{
    QElapsedTimer rebuildTimer;
    rebuildTimer.start();
    auto fail = [&](const QString &message) {
        m_db.rollback();
        if (error) *error = message;
        qWarning() << "[MemoryIndex] rebuild failed:" << message;
        return false;
    };
    if (!m_db.transaction())
        return fail(m_db.lastError().text());
    QSqlQuery clear(m_db);
    for (const auto &table : {QStringLiteral("memory_embeddings"),
                              QStringLiteral("memory_chunk_sources"),
                              QStringLiteral("memory_chunks"),
                              QStringLiteral("memory_index_state")}) {
        if (!clear.exec(QStringLiteral("DELETE FROM ") + table))
            return fail(clear.lastError().text());
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO memory_chunks("
        "source_type,source_ref,chunk_kind,derived,text,content_hash,search_text,"
        "channel,session_id,speaker_id,speaker_name,speaker_role,"
        "speech_at,event_time,recorded_at,privacy,status,index_generation,created_at,updated_at)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'active',1,?,?)"));
    QSqlQuery idQuery(m_db);
    idQuery.prepare(QStringLiteral(
        "SELECT id FROM memory_chunks WHERE source_type=? AND source_ref=? "
        "AND chunk_kind=? AND content_hash=? AND index_generation=1"));
    QSqlQuery sourceInsert(m_db);
    sourceInsert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO memory_chunk_sources(chunk_id,source_table,source_id) VALUES(?,?,?)"));

    auto addChunk = [&](const QString &sourceType, const QString &sourceRef,
                        const QString &chunkKind, const QString &rawText,
                        const QString &channel, const QString &session,
                        const QString &speakerId, const QString &speakerName,
                        const QString &speakerRole, const QString &speechAt,
                        const QString &derived, const QString &privacy,
                        const QString &sourceTable, const QString &sourceId) -> bool {
        const QString text = rawText.trimmed();
        if (text.isEmpty())
            return true;
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(
            (sourceRef + QLatin1Char('\n') + text).toUtf8(),
            QCryptographicHash::Sha256).toHex());
        insert.addBindValue(sourceType);
        insert.addBindValue(sourceRef);
        insert.addBindValue(chunkKind);
        insert.addBindValue(derived);
        insert.addBindValue(text);
        insert.addBindValue(hash);
        insert.addBindValue(text);
        insert.addBindValue(channel);
        insert.addBindValue(session);
        insert.addBindValue(speakerId);
        insert.addBindValue(speakerName);
        insert.addBindValue(speakerRole);
        insert.addBindValue(speechAt);
        insert.addBindValue(QVariant(QMetaType(QMetaType::QString)));
        insert.addBindValue(now);
        insert.addBindValue(privacy);
        insert.addBindValue(now);
        insert.addBindValue(now);
        if (!insert.exec()) {
            if (error) *error = insert.lastError().text();
            return false;
        }
        qlonglong chunkId = insert.numRowsAffected() ? insert.lastInsertId().toLongLong() : 0;
        if (chunkId <= 0) {
            idQuery.addBindValue(sourceType);
            idQuery.addBindValue(sourceRef);
            idQuery.addBindValue(chunkKind);
            idQuery.addBindValue(hash);
            if (!idQuery.exec() || !idQuery.next()) {
                if (error) *error = idQuery.lastError().text();
                return false;
            }
            chunkId = idQuery.value(0).toLongLong();
            idQuery.finish();
        }
        sourceInsert.addBindValue(chunkId);
        sourceInsert.addBindValue(sourceTable);
        sourceInsert.addBindValue(sourceId);
        if (!sourceInsert.exec()) {
            if (error) *error = sourceInsert.lastError().text();
            return false;
        }
        return true;
    };

    QSqlQuery raw(m_db);
    if (!raw.exec(QStringLiteral(
            "SELECT rowid,channel,session_id,sender_id,sender_name,person_id,content,occurred_at "
            "FROM raw_events WHERE ") + MemoryReadPolicy::rawMessagePredicate())) {
        return fail(raw.lastError().text());
    }
    while (raw.next()) {
        const auto rowid = raw.value(0).toLongLong();
        const auto channel = raw.value(1).toString();
        const auto session = raw.value(2).toString();
        const auto senderId = raw.value(3).toString();
        const auto senderName = raw.value(4).toString();
        const auto personId = raw.value(5).toString();
        const bool owner = personId == QStringLiteral("owner");
        if (!addChunk(QStringLiteral("raw"), QStringLiteral("raw/%1").arg(rowid),
                      QStringLiteral("message"), raw.value(6).toString(),
                      channel, session,
                      personId.isEmpty() ? senderId : personId,
                      senderName.isEmpty() ? (owner ? QStringLiteral("主人") : QStringLiteral("群友"))
                                           : senderName,
                      owner ? QStringLiteral("user") : QStringLiteral("other"),
                      raw.value(7).toString(), QStringLiteral("original"),
                      channel == Channel::kQQGroup ? QStringLiteral("public") : QStringLiteral("private"),
                      QStringLiteral("raw_events"), QString::number(rowid)))
            return fail(error ? *error : QStringLiteral("raw chunk")) ;
    }

    QSqlQuery chat(m_db);
    if (!chat.exec(QStringLiteral(
            "SELECT id,channel,session_id,subject,speaker,user_input,raw_reply,timestamp "
            "FROM chat_history WHERE sensitive=0 AND (user_input IS NOT NULL OR raw_reply IS NOT NULL)"))) {
        return fail(chat.lastError().text());
    }
    while (chat.next()) {
        const auto id = chat.value(0).toLongLong();
        const auto channel = chat.value(1).toString();
        const auto session = chat.value(2).toString();
        const auto subject = chat.value(3).toString();
        const auto speaker = chat.value(4).toString();
        const auto userText = chat.value(5).toString();
        const auto rawReply = chat.value(6).toString();
        const auto timestamp = chat.value(7).toString();
        const auto privacy = channel == Channel::kQQGroup
            ? QStringLiteral("public") : QStringLiteral("private");
        if (!addChunk(QStringLiteral("chat"), QStringLiteral("chat/%1:user").arg(id),
                      QStringLiteral("turn_user"), userText, channel, session,
                      subject.isEmpty() ? QStringLiteral("user") : subject,
                      speaker.isEmpty() ? (subject == QStringLiteral("user")
                                              ? QStringLiteral("主人") : QStringLiteral("群友"))
                                        : speaker,
                      QStringLiteral("user"), timestamp, QStringLiteral("original"),
                      privacy, QStringLiteral("chat_history"), QString::number(id)))
            return fail(error ? *error : QStringLiteral("chat user chunk"));
        const auto root = QJsonDocument::fromJson(rawReply.toUtf8()).object();
        QStringList replyParts;
        for (const auto &value : root.value(QStringLiteral("sentences")).toArray()) {
            const auto zh = value.toObject().value(QStringLiteral("zh_text")).toString().trimmed();
            if (!zh.isEmpty()) replyParts.append(zh);
        }
        if (replyParts.isEmpty() && !rawReply.trimmed().startsWith(QLatin1Char('{')))
            replyParts.append(rawReply.trimmed());
        if (!addChunk(QStringLiteral("chat"), QStringLiteral("chat/%1:assistant").arg(id),
                      QStringLiteral("turn_assistant"), replyParts.join(QLatin1Char('\n')),
                      channel, session, QStringLiteral("mako"), QStringLiteral("茉子"),
                      QStringLiteral("assistant"), timestamp, QStringLiteral("assistant_expression"),
                      privacy, QStringLiteral("chat_history"), QString::number(id)))
            return fail(error ? *error : QStringLiteral("chat assistant chunk"));
    }
    QSqlQuery count(m_db);
    int chunkCount = 0;
    if (count.exec("SELECT count(*) FROM memory_chunks WHERE status='active'") && count.next())
        chunkCount = count.value(0).toInt();
    QSqlQuery state(m_db);
    state.prepare("INSERT OR REPLACE INTO memory_index_state(key,value) VALUES(?,?)");
    const QList<QPair<QString, QString>> stateValues{
        {QStringLiteral("status"), QStringLiteral("complete")},
        {QStringLiteral("last_rebuild"), now},
        {QStringLiteral("chunks"), QString::number(chunkCount)},
        {QStringLiteral("generation"), QStringLiteral("1")},
        {QStringLiteral("duration_ms"), QString::number(rebuildTimer.elapsed())}};
    for (const auto &entry : stateValues) {
        state.bindValue(0, entry.first);
        state.bindValue(1, entry.second);
        if (!state.exec())
            return fail(state.lastError().text());
    }
    if (!m_db.commit())
        return fail(m_db.lastError().text());
    return true;
}

int MemoryManager::memoryChunkCount() const
{
    QSqlQuery q(m_db);
    if (!q.exec("SELECT count(*) FROM memory_chunks WHERE status='active'") || !q.next())
        return 0;
    return q.value(0).toInt();
}

bool MemoryManager::clearSearchIndex(QString *error)
{
    if (!m_db.transaction()) {
        if (error) *error = m_db.lastError().text();
        return false;
    }
    for (const auto &table : {QStringLiteral("memory_embeddings"),
                              QStringLiteral("memory_chunk_sources"),
                              QStringLiteral("memory_chunks"),
                              QStringLiteral("memory_index_state")}) {
        QSqlQuery clear(m_db);
        if (!clear.exec(QStringLiteral("DELETE FROM ") + table)) {
            m_db.rollback();
            if (error) *error = clear.lastError().text();
            return false;
        }
    }
    if (!m_db.commit()) {
        if (error) *error = m_db.lastError().text();
        return false;
    }
    return true;
}

bool MemoryManager::backupDatabase(const QString &destination, QString *error) const
{
    if (destination.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("empty backup destination");
        return false;
    }
    QSqlQuery query(m_db);
    // VACUUM INTO 生成一致性副本；不依赖 -wal/-shm 文件同时复制。
    query.prepare("VACUUM INTO ?");
    query.addBindValue(destination);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return false;
    }
    return true;
}

QJsonObject MemoryManager::searchIndexStatus() const
{
    QJsonObject status;
    status.insert(QStringLiteral("chunks"), memoryChunkCount());
    QSqlQuery bySource(m_db);
    if (bySource.exec("SELECT source_type,count(*) FROM memory_chunks "
                      "WHERE status='active' GROUP BY source_type ORDER BY source_type")) {
        QJsonObject counts;
        while (bySource.next())
            counts.insert(bySource.value(0).toString(), bySource.value(1).toInt());
        status.insert(QStringLiteral("chunks_by_source"), counts);
    }
    QSqlQuery embeddings(m_db);
    if (embeddings.exec("SELECT count(*) FROM memory_embeddings") && embeddings.next())
        status.insert(QStringLiteral("embeddings"), embeddings.value(0).toInt());
    else
        status.insert(QStringLiteral("embeddings"), 0);
    QSqlQuery state(m_db);
    if (state.exec("SELECT key,value FROM memory_index_state")) {
        QJsonObject stateObject;
        while (state.next())
            stateObject.insert(state.value(0).toString(), state.value(1).toString());
        status.insert(QStringLiteral("state"), stateObject);
    }
    return status;
}

QJsonArray MemoryManager::searchMemoryChunks(const QStringList &terms, int limit,
                                             const QString &exactQuery) const
{
    QJsonArray result;
    QString sql = QStringLiteral(
        "SELECT id,source_type,source_ref,chunk_kind,derived,text,content_hash,channel,session_id,"
        "speaker_id,speaker_name,speaker_role,speech_at,event_time,recorded_at,privacy,status "
        "FROM memory_chunks WHERE status='active' AND index_generation=1");
    QStringList safe;
    for (const auto &term : terms) {
        const auto trimmed = term.trimmed();
        if (trimmed.isEmpty() || safe.contains(trimmed))
            continue;
        safe.append(trimmed);
        if (safe.size() >= 8)
            break;
    }
    const QString exact = exactQuery.trimmed();
    if (!exact.isEmpty() && !safe.contains(exact))
        safe.prepend(exact);
    if (!safe.isEmpty()) {
        QStringList clauses;
        for (int i = 0; i < safe.size(); ++i)
            clauses.append(QStringLiteral("text LIKE ?"));
        sql += QStringLiteral(" AND (") + clauses.join(QStringLiteral(" OR ")) + QStringLiteral(")");
    }
    if (!exact.isEmpty())
        sql += QStringLiteral(" ORDER BY CASE WHEN text LIKE ? THEN 0 ELSE 1 END,datetime(speech_at) DESC,id DESC LIMIT ?");
    else
        sql += QStringLiteral(" ORDER BY datetime(speech_at) DESC,id DESC LIMIT ?");
    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const auto &term : safe)
        q.addBindValue(QStringLiteral("%") + term + QStringLiteral("%"));
    if (!exact.isEmpty())
        q.addBindValue(QStringLiteral("%") + exact + QStringLiteral("%"));
    q.addBindValue(qMax(1, limit * 4));
    if (!q.exec())
        return result;
    while (q.next()) {
        result.append(QJsonObject{
            {"id",q.value(0).toLongLong()},{"source_type",q.value(1).toString()},
            {"source_ref",q.value(2).toString()},{"chunk_kind",q.value(3).toString()},
            {"derived",q.value(4).toString()},
            {"text",q.value(5).toString()},{"content_hash",q.value(6).toString()},
            {"channel",q.value(7).toString()},{"session",q.value(8).toString()},
            {"speaker_id",q.value(9).toString()},{"speaker",q.value(10).toString()},
            {"speaker_role",q.value(11).toString()},{"speech_at",q.value(12).toString()},
            {"event_time",q.value(13).toString()},{"recorded_at",q.value(14).toString()},
            {"privacy",q.value(15).toString()},{"status",q.value(16).toString()}});
    }
    return result;
}

QJsonArray MemoryManager::memoryChunksForEmbedding(int limit, QString *error) const
{
    if (error) error->clear();
    QJsonArray result;
    QSqlQuery q(m_db);
    QString sql = QStringLiteral(
        "SELECT id,source_type,source_ref,chunk_kind,derived,text,content_hash,"
        "channel,session_id,speaker_id,speaker_name,speaker_role,"
        "speech_at,event_time,recorded_at,privacy,status "
        "FROM memory_chunks WHERE status='active' AND index_generation=1 "
        "ORDER BY id");
    if (limit > 0)
        sql += QStringLiteral(" LIMIT ?");
    q.prepare(sql);
    if (limit > 0)
        q.addBindValue(limit);
    if (!q.exec()) {
        if (error) *error = q.lastError().text();
        return result;
    }
    while (q.next()) {
        result.append(QJsonObject{
            {"id",q.value(0).toLongLong()},{"source_type",q.value(1).toString()},
            {"source_ref",q.value(2).toString()},{"chunk_kind",q.value(3).toString()},
            {"derived",q.value(4).toString()},{"text",q.value(5).toString()},
            {"content_hash",q.value(6).toString()},{"channel",q.value(7).toString()},
            {"session",q.value(8).toString()},{"speaker_id",q.value(9).toString()},
            {"speaker",q.value(10).toString()},{"speaker_role",q.value(11).toString()},
            {"speech_at",q.value(12).toString()},{"event_time",q.value(13).toString()},
            {"recorded_at",q.value(14).toString()},{"privacy",q.value(15).toString()},
            {"status",q.value(16).toString()}});
    }
    return result;
}

QJsonArray MemoryManager::memoryChunksMissingEmbedding(int limit, const QString &model,
                                                      qlonglong afterId, QString *error) const
{
    QJsonArray result;
    if (model.isEmpty())
        return result;
    QSqlQuery q(m_db);
    q.prepare("SELECT c.id,c.source_type,c.source_ref,c.chunk_kind,c.derived,c.text,c.content_hash,"
              "c.channel,c.session_id,c.speaker_id,c.speaker_name,c.speaker_role,"
              "c.speech_at,c.event_time,c.recorded_at,c.privacy,c.status "
              "FROM memory_chunks c WHERE c.status='active' AND c.index_generation=1 "
              "AND c.id>? AND NOT EXISTS (SELECT 1 FROM memory_embeddings e WHERE e.chunk_id=c.id "
              "AND e.model_config=? AND e.content_hash=c.content_hash) "
              "ORDER BY c.id LIMIT ?");
    q.addBindValue(afterId);
    q.addBindValue(model);
    q.addBindValue(qMax(1, limit));
    if (!q.exec()) {
        if (error) *error = q.lastError().text();
        return result;
    }
    while (q.next()) {
        result.append(QJsonObject{
            {"id",q.value(0).toLongLong()},{"source_type",q.value(1).toString()},
            {"source_ref",q.value(2).toString()},{"chunk_kind",q.value(3).toString()},
            {"derived",q.value(4).toString()},{"text",q.value(5).toString()},
            {"content_hash",q.value(6).toString()},{"channel",q.value(7).toString()},
            {"session",q.value(8).toString()},{"speaker_id",q.value(9).toString()},
            {"speaker",q.value(10).toString()},{"speaker_role",q.value(11).toString()},
            {"speech_at",q.value(12).toString()},{"event_time",q.value(13).toString()},
            {"recorded_at",q.value(14).toString()},{"privacy",q.value(15).toString()},
            {"status",q.value(16).toString()}});
    }
    return result;
}

bool MemoryManager::storeChunkEmbedding(qlonglong chunkId, const QString &model, int dimension,
                                        const QByteArray &blob, const QString &contentHash)
{
    if (chunkId <= 0 || model.isEmpty() || dimension <= 0 || blob.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO memory_embeddings("
              "chunk_id,model_config,dimension,vector,content_hash,index_generation,created_at)"
              " SELECT ?,?,?,?,?,1,? FROM memory_chunks WHERE id=? AND status='active' "
              "AND index_generation=1 AND content_hash=?");
    q.addBindValue(chunkId);
    q.addBindValue(model);
    q.addBindValue(dimension);
    q.addBindValue(blob);
    q.addBindValue(contentHash);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    q.addBindValue(chunkId);
    q.addBindValue(contentHash);
    return q.exec();
}

QByteArray MemoryManager::chunkEmbedding(qlonglong chunkId, const QString &model) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT vector FROM memory_embeddings WHERE chunk_id=? AND model_config=? "
              "AND index_generation=1");
    q.addBindValue(chunkId);
    q.addBindValue(model);
    if (!q.exec() || !q.next())
        return {};
    return q.value(0).toByteArray();
}

bool MemoryManager::embedSearchChunks(MemoryEmbeddingProvider *provider, int limit,
                                      QString *error, const MemoryReadPolicy *policy)
{
    if (!provider) {
        if (error) *error = QStringLiteral("embedding provider is null");
        return false;
    }
    const int chunkBatch = qMax(1, limit);
    const MemoryReadPolicy readPolicy = policy ? *policy : MemoryReadPolicy();
    qlonglong afterId = 0;
    int eligibleCount = 0;
    int scanned = 0;
    QHash<QByteArray, QVector<float>> textVectors;
    while (true) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            if (error) *error = QStringLiteral("embedding interrupted");
            return false;
        }
        QString readError;
        const auto chunks = memoryChunksMissingEmbedding(chunkBatch, provider->modelId(),
                                                        afterId, &readError);
        if (!readError.isEmpty()) {
            if (error) *error = readError;
            return false;
        }
        if (chunks.isEmpty())
            break;

        scanned += chunks.size();
        afterId = chunks.last().toObject().value("id").toInteger();
        QJsonArray eligible;
        QStringList pendingTexts;
        QList<QByteArray> pendingKeys;
        QHash<QByteArray, int> pendingIndex;
        for (const auto &value : chunks) {
            const auto chunk = value.toObject();
            const QByteArray key = chunk.value(QStringLiteral("text")).toString().toUtf8();
            if (!readPolicy.allows(chunk.value("channel").toString(), chunk.value("session").toString(),
                                   chunk.value("text").toString()))
                continue;
            eligible.append(chunk);
            if (textVectors.contains(key) || pendingIndex.contains(key))
                continue;
            pendingIndex.insert(key, pendingTexts.size());
            pendingKeys.append(key);
            pendingTexts.append(QString::fromUtf8(key));
        }
        eligibleCount += eligible.size();
        const int providerBatch = 10;
        for (int offset = 0; offset < pendingTexts.size(); offset += providerBatch) {
            QVector<QVector<float>> batchVectors;
            QString embedError;
            if (!provider->embedBatch(pendingTexts.mid(offset, providerBatch),
                                      &batchVectors, &embedError)) {
                if (error) *error = embedError;
                return false;
            }
            for (int i = 0; i < batchVectors.size()
                            && offset + i < pendingKeys.size(); ++i) {
                textVectors.insert(pendingKeys.at(offset + i), batchVectors.at(i));
            }
        }

        for (const auto &value : eligible) {
            const auto chunk = value.toObject();
            const QByteArray key = chunk.value(QStringLiteral("text")).toString().toUtf8();
            if (!textVectors.contains(key)) {
                if (error) *error = QStringLiteral("embedding batch missing a chunk");
                return false;
            }
            const auto &vector = textVectors.value(key);
            const QByteArray blob(reinterpret_cast<const char *>(vector.constData()),
                                  vector.size() * int(sizeof(float)));
            if (!storeChunkEmbedding(chunk.value(QStringLiteral("id")).toInteger(),
                                     provider->modelId(), vector.size(), blob,
                                     chunk.value(QStringLiteral("content_hash")).toString())) {
                if (error) *error = QStringLiteral("store embedding failed");
                return false;
            }
        }
        if (chunks.size() < chunkBatch)
            break;
    }
    QSqlQuery state(m_db);
    // One statement gives both counters the same SQLite snapshot.
    state.prepare("INSERT OR REPLACE INTO memory_index_state(key,value) "
                  "SELECT 'chunks',count(*) FROM memory_chunks WHERE status='active' "
                  "UNION ALL SELECT 'embeddings',count(*) FROM memory_embeddings e "
                  "JOIN memory_chunks c ON c.id=e.chunk_id AND c.content_hash=e.content_hash "
                  "WHERE c.status='active' AND e.model_config=? "
                  "UNION ALL SELECT 'embedding_model',?");
    state.addBindValue(provider->modelId());
    state.addBindValue(provider->modelId());
    if (state.exec()) {
        if (scanned)
            qInfo() << "[MemoryIndex] embedding scan" << provider->modelId()
                    << "examined=" << scanned << "eligible=" << eligibleCount;
        return true;
    }
    if (error) *error = state.lastError().text();
    return false;
}

bool MemoryManager::upsertMemoryChunk(const MemoryChunkWrite &chunk)
{
    if (chunk.text.trimmed().isEmpty())
        return true;
    // 兼容极旧库/半迁移库：没有 v15 表时不阻断主写入路径。
    QSqlQuery table(m_db);
    if (!table.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='memory_chunks'")
        || !table.next())
        return true;
    table.finish();
    const QString text = chunk.text.trimmed();
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(
        (chunk.sourceRef + QLatin1Char('\n') + text).toUtf8(),
        QCryptographicHash::Sha256).toHex());
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery insert(m_db);
    insert.prepare("INSERT OR IGNORE INTO memory_chunks("
                   "source_type,source_ref,chunk_kind,derived,text,content_hash,search_text,"
                   "channel,session_id,speaker_id,speaker_name,speaker_role,"
                   "speech_at,event_time,recorded_at,privacy,status,index_generation,created_at,updated_at)"
                   "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'active',1,?,?)");
    insert.addBindValue(chunk.sourceType);
    insert.addBindValue(chunk.sourceRef);
    insert.addBindValue(chunk.chunkKind);
    insert.addBindValue(chunk.derived);
    insert.addBindValue(text);
    insert.addBindValue(hash);
    insert.addBindValue(text);
    insert.addBindValue(chunk.channel);
    insert.addBindValue(chunk.session);
    insert.addBindValue(chunk.speakerId);
    insert.addBindValue(chunk.speakerName);
    insert.addBindValue(chunk.speakerRole);
    insert.addBindValue(chunk.speechAt);
    insert.addBindValue(chunk.eventTime);
    insert.addBindValue(now);
    insert.addBindValue(chunk.privacy);
    insert.addBindValue(now);
    insert.addBindValue(now);
    if (!insert.exec())
        return false;
    qlonglong chunkId = insert.numRowsAffected() ? insert.lastInsertId().toLongLong() : 0;
    if (chunkId <= 0) {
        QSqlQuery find(m_db);
        find.prepare("SELECT id FROM memory_chunks WHERE source_type=? AND source_ref=? "
                     "AND chunk_kind=? AND content_hash=? AND index_generation=1");
        find.addBindValue(chunk.sourceType);
        find.addBindValue(chunk.sourceRef);
        find.addBindValue(chunk.chunkKind);
        find.addBindValue(hash);
        if (!find.exec() || !find.next())
            return false;
        chunkId = find.value(0).toLongLong();
    }
    QSqlQuery source(m_db);
    source.prepare("INSERT OR IGNORE INTO memory_chunk_sources(chunk_id,source_table,source_id)"
                   " VALUES(?,?,?)");
    source.addBindValue(chunkId);
    source.addBindValue(chunk.sourceTable);
    source.addBindValue(chunk.sourceId);
    return source.exec();
}

bool MemoryManager::indexRawEvent(qlonglong id)
{
    if (id <= 0)
        return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT channel,session_id,sender_id,sender_name,person_id,content,occurred_at,"
              "direction,event_type,sensitive,redaction_status FROM raw_events WHERE rowid=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return false;
    if (q.value(7).toString() != QStringLiteral("in")
        || q.value(8).toString() != QStringLiteral("message")
        || q.value(9).toInt() != 0
        || q.value(10).toString() != QStringLiteral("none")
        || q.value(5).toString().trimmed().isEmpty())
        return true;
    const QString channel = q.value(0).toString();
    const QString session = q.value(1).toString();
    const QString senderId = q.value(2).toString();
    const QString senderName = q.value(3).toString();
    const QString personId = q.value(4).toString();
    const bool owner = personId == QStringLiteral("owner");
    MemoryChunkWrite chunk;
    chunk.sourceType = QStringLiteral("raw");
    chunk.sourceRef = QStringLiteral("raw/%1").arg(id);
    chunk.chunkKind = QStringLiteral("message");
    chunk.derived = QStringLiteral("original");
    chunk.text = q.value(5).toString();
    chunk.channel = channel;
    chunk.session = session;
    chunk.speakerId = personId.isEmpty() ? senderId : personId;
    chunk.speakerName = senderName.isEmpty()
        ? (owner ? QStringLiteral("主人") : QStringLiteral("群友")) : senderName;
    chunk.speakerRole = owner ? QStringLiteral("user") : QStringLiteral("other");
    chunk.speechAt = q.value(6).toString();
    chunk.privacy = channel == Channel::kQQGroup
        ? QStringLiteral("public") : QStringLiteral("private");
    chunk.sourceTable = QStringLiteral("raw_events");
    chunk.sourceId = QString::number(id);
    q.finish();
    QSqlQuery remove(m_db);
    remove.prepare("DELETE FROM memory_chunks WHERE source_type='raw' AND source_ref=?");
    remove.addBindValue(QStringLiteral("raw/%1").arg(id));
    if (!remove.exec())
        return false;
    if (!upsertMemoryChunk(chunk)) return false;
    emit searchChunksChanged();
    return true;
}

bool MemoryManager::indexChatTurn(qlonglong id)
{
    if (id <= 0)
        return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT channel,session_id,subject,speaker,user_input,raw_reply,timestamp,sensitive,turn_kind "
              "FROM chat_history WHERE id=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next() || q.value(7).toInt() != 0)
        return false;
    const QString channel = q.value(0).toString();
    const QString session = q.value(1).toString();
    const QString subject = q.value(2).toString();
    const QString speaker = q.value(3).toString();
    const QString userText = q.value(4).toString();
    const QString rawReply = q.value(5).toString();
    const QString timestamp = q.value(6).toString();
    // 主动轮次（turn_kind='proactive'）没有用户输入：不造空的 turn_user 块，
    // 说话内容全部由下面的 assistant 块从 raw_reply.sentences[].zh_text 收录。
    const bool proactive = q.value(8).toString() == QStringLiteral("proactive");
    const QString privacy = channel == Channel::kQQGroup
        ? QStringLiteral("public") : QStringLiteral("private");
    q.finish();
    // Completing a received turn adds an assistant chunk. Keep the original
    // user chunk (and any vector already computed while the reply was pending).
    QSqlQuery remove(m_db);
    remove.prepare("DELETE FROM memory_chunks WHERE source_type='chat' AND source_ref LIKE ? "
                   "AND NOT (chunk_kind='turn_user' AND text=coalesce(?,''))");
    remove.addBindValue(QStringLiteral("chat/%1:%").arg(id));
    remove.addBindValue(userText.trimmed());
    if (!remove.exec())
        return false;
    if (!proactive) {
        MemoryChunkWrite user;
        user.sourceType = QStringLiteral("chat");
        user.sourceRef = QStringLiteral("chat/%1:user").arg(id);
        user.chunkKind = QStringLiteral("turn_user");
        user.derived = QStringLiteral("original");
        user.text = userText;
        user.channel = channel;
        user.session = session;
        user.speakerId = subject.isEmpty()
            ? QStringLiteral("user")
            : (subject == QStringLiteral("user") ? QStringLiteral("owner") : subject);
        user.speakerName = speaker.isEmpty()
            ? (subject == QStringLiteral("user") ? QStringLiteral("主人") : QStringLiteral("群友"))
            : speaker;
        user.speakerRole = QStringLiteral("user");
        user.speechAt = timestamp;
        user.privacy = privacy;
        user.sourceTable = QStringLiteral("chat_history");
        user.sourceId = QString::number(id);
        if (!upsertMemoryChunk(user))
            return false;
    }

    const auto root = QJsonDocument::fromJson(rawReply.toUtf8()).object();
    QStringList replyParts;
    for (const auto &value : root.value(QStringLiteral("sentences")).toArray()) {
        const auto zh = value.toObject().value(QStringLiteral("zh_text")).toString().trimmed();
        if (!zh.isEmpty()) replyParts.append(zh);
    }
    if (replyParts.isEmpty() && !rawReply.trimmed().startsWith(QLatin1Char('{')))
        replyParts.append(rawReply.trimmed());
    MemoryChunkWrite assistant;
    assistant.sourceType = QStringLiteral("chat");
    assistant.sourceRef = QStringLiteral("chat/%1:assistant").arg(id);
    assistant.chunkKind = QStringLiteral("turn_assistant");
    assistant.derived = QStringLiteral("assistant_expression");
    assistant.text = replyParts.join(QLatin1Char('\n'));
    assistant.channel = channel;
    assistant.session = session;
    assistant.speakerId = QStringLiteral("mako");
    assistant.speakerName = QStringLiteral("茉子");
    assistant.speakerRole = QStringLiteral("assistant");
    assistant.speechAt = timestamp;
    assistant.privacy = privacy;
    assistant.sourceTable = QStringLiteral("chat_history");
    assistant.sourceId = QString::number(id);
    return upsertMemoryChunk(assistant);
}

QJsonArray MemoryManager::matterEvidence(const RequestMeta &audience, const QString &matter) const{
    QJsonArray result;
    if (matter.isEmpty()) return result;
    const auto revisions = internalMatters(audience, {}, {matter});
    for (const auto &value : revisions) {
        auto row = value.toObject();
        row["source_kind"] = row.take("kind");
        row["effective_at"] = QJsonValue::Null;
        result.prepend(row);
    }
    return result;
}

QString MemoryManager::conversationPhase(const RequestMeta &audience) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT raw_reply,turn_state,turn_detail FROM chat_history WHERE channel=? AND session_id=? AND subject=? "
              "AND sensitive=0 AND turn_kind='reply' AND turn_state IN ('quiet','handed_off','legacy') ORDER BY id DESC LIMIT 1");
    q.addBindValue(audience.channel);
    q.addBindValue(audience.sessionId);
    q.addBindValue(audience.subject);
    if (!q.exec() || !q.next()) return {};
    if (q.value(1).toString() == "quiet") return q.value(2).toString();
    const auto reply = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
    if (reply["sentences"].toArray().isEmpty()) return {};
    return reply["conversation_action"].toString();
}

QJsonArray MemoryManager::currentUnderstanding(const RequestMeta &audience, const QString &input,
                                    QStringList *keys, const QStringList &only,
                                    bool includeAudiencePrivate) const
{
    QJsonArray result;
    if (keys) keys->clear();
    if ((!audience.longTermMemoryEnabled && audience.channel != Channel::kQQGroup)
        || audience.sensitive || audience.subject.isEmpty()
        || (audience.channel != Channel::kPet && audience.actorId.isEmpty()))
        return result;
    QSqlQuery query(m_db);
    if (!query.prepare(R"(
        WITH evidence AS (
            SELECT h.id,h.timestamp,json_extract(j.value,'$.topic') AS topic,
                   json_extract(j.value,'$.quote') AS quote,
                   (h.channel=? AND h.session_id=?) AS local,
                   'legacy:'||json_extract(j.value,'$.topic') AS matter,0 AS version,
                   NULL AS status,NULL AS kind,NULL AS understanding,NULL AS event_time,NULL AS recorded,h.channel,h.session_id,h.user_input AS original,NULL AS semantics
            FROM chat_history h,json_each(h.state_updates) j
            WHERE h.subject=? AND h.state_updates IS NOT NULL AND h.sensitive=0
              AND NOT EXISTS (SELECT 1 FROM cognitive_source WHERE event=h.cognitive_event)
            UNION ALL
            SELECT s.id,s.reported,r.topic,r.quote,(s.channel=? AND s.session=?),
                   r.matter,r.version,r.status,r.kind,r.understanding,r.event_time,r.recorded,s.channel,s.session,s.input,r.semantics
            FROM matter_revision r JOIN cognitive_source s ON s.id=r.source WHERE s.subject=?
        ), ranked AS (
            SELECT *,row_number() OVER (PARTITION BY matter ORDER BY version DESC,id DESC) AS pos,
                   max(local) OVER (PARTITION BY matter) AS mentioned_here,
                   first_value(quote) OVER origin AS initial_quote,
                   first_value(timestamp) OVER origin AS initial_reported,
                   first_value(event_time) OVER origin AS initial_event_time,
                   first_value(channel) OVER origin AS initial_channel,
                   first_value(session_id) OVER origin AS initial_session
            FROM evidence WINDOW origin AS (PARTITION BY matter ORDER BY version,id)
        ), self_ranked AS (
            SELECT matter,quote,version,status,id,timestamp,recorded,channel,session_id,original,
                   row_number() OVER (PARTITION BY matter ORDER BY version DESC,id DESC) AS spos
            FROM evidence WHERE kind='self_report'
        )
        SELECT r.topic,r.quote,r.timestamp,r.local,r.mentioned_here,r.version,r.status,r.kind,
               r.understanding,r.id,r.matter,r.event_time,r.recorded,
               sr.quote,sr.version,sr.status,sr.id,sr.timestamp,sr.recorded,
               r.channel,r.session_id,r.original,sr.channel,sr.session_id,sr.original,
               r.semantics,r.initial_quote,r.initial_reported,r.initial_event_time,r.initial_channel,r.initial_session
        FROM ranked r LEFT JOIN self_ranked sr ON sr.matter=r.matter AND sr.spos=1
        WHERE r.pos=1
          AND (? OR r.matter IN (SELECT value FROM json_each(?)))
        ORDER BY (instr(?,r.topic)>0) DESC,datetime(r.timestamp) DESC,r.id DESC,r.matter LIMIT 32
    )")) {
        qWarning() << "[MemoryManager]currentUnderstanding prepare failed:" << query.lastError();
        return result;
    }
    query.addBindValue(audience.channel);
    query.addBindValue(audience.sessionId);
    query.addBindValue(audience.subject);
    query.addBindValue(audience.channel);
    query.addBindValue(audience.sessionId);
    query.addBindValue(audience.subject);
    query.addBindValue(only.isEmpty());
    query.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(only)).toJson(QJsonDocument::Compact)));
    query.addBindValue(input);
    if (!query.exec()) {
        qWarning() << "[MemoryManager]currentUnderstanding query failed:" << query.lastError();
        return result;
    }

    auto &config = ConfigManager::instance();
    const auto rules = config.getMemoryPolicyConfig().privacyRules;
    QStringList selected;
    while (query.next()) {
        if (config.isBlindZone(query.value(19).toString(), query.value(20).toString())
            || config.isBlindZone(query.value(22).toString(), query.value(23).toString()))
            continue;
        bool restricted = false;
        for (int column : {0, 1, 8, 13, 21, 24, 25}) {
            const auto scan = SensitiveContentFilter::sanitize(query.value(column).toString(), {}, rules);
            if (scan.blocked || scan.redacted) { restricted = true; break; }
        }
        if (restricted) continue;
        const auto count = result.size();
        const auto topic = query.value(0).toString();
        if (audience.channel == Channel::kQQGroup && !query.value(3).toBool()
            && !includeAudiencePrivate) {

            if (query.value(4).toBool() || input.contains(topic)) {
                if (query.value(5).toInt() > 0)
                    result.append(QJsonObject{{"avoid_unprompted_followup", true}});
                else
                    result.append(QJsonObject{{"avoid_stale_followup", topic}});
            }
        } else if (query.value(5).toInt() > 0) {
            QJsonObject record{{"quote", query.value(1).toString()}, {"reported_at", query.value(2).toString()}};
            if (audience.channel != Channel::kQQGroup || includeAudiencePrivate) {
                record["topic"] = topic;
                record["understanding"] = query.value(8).toString();
                record["status"] = query.value(6).toString();
                record["source_kind"] = query.value(7).toString();
                record["revision"] = query.value(5).toLongLong();
                record["evidence_ref"] = QString("source/%1").arg(query.value(9).toLongLong());
                record["event_time_expression"] = query.value(11).toString();
                record["interpretation_recorded_at"] = query.value(12).toString();
                record["effective_at"] = QJsonValue::Null;
                auto semantics = QJsonDocument::fromJson(query.value(25).toByteArray()).object();
                if (semantics.isEmpty()) semantics = MatterSemantics::legacy(query.value(6).toString());
                record["semantics"] = semantics;
                bool originAllowed = !config.isBlindZone(query.value(29).toString(),query.value(30).toString());
                for (int column : {26,28}) {
                    const auto scan = SensitiveContentFilter::sanitize(query.value(column).toString(),{},rules);
                    if (scan.blocked || scan.redacted) originAllowed = false;
                }
                if (originAllowed)
                    record["origin"] = QJsonObject{{"quote",query.value(26).toString()},
                        {"reported_at",query.value(27).toString()},{"event_time_expression",query.value(28).toString()}};

                const auto selfQuote = query.value(13).toString();
                const auto selfVersion = query.value(14).toLongLong();
                if (selfVersion > 0 && !selfQuote.isEmpty()) {
                    record["self_report"] = QJsonObject{
                        {"quote", selfQuote},
                        {"version", selfVersion},
                        {"status", query.value(15).toString()},
                        {"reported_at", query.value(17).toString()},
                        {"evidence_ref", QString("source/%1").arg(query.value(16).toLongLong())},
                        {"source_kind", QStringLiteral("self_report")},
                        {"speaker_role", QStringLiteral("user")}};
                }
            }
            result.append(record);
        } else {
            result.append(QJsonObject{{"topic", topic}, {"self_report", query.value(1).toString()},
                                      {"reported_at", query.value(2).toString()}});
        }
        if (result.size() > count) {
            if (keys) keys->append(query.value(10).toString());
            selected << QString("%1/%2").arg(query.value(9).toLongLong()).arg(query.value(5).toLongLong());
        }
    }
    if (!audience.runId.isEmpty())
        qInfo() << "[Cognition] context run=" << audience.runId << "source/version=" << selected;

    return result;
}

QList<HistoryTurn> MemoryManager::getHistoryTurn(int N, const QString &channel, const QString &sessionId, qint64 beforeTurn)
{
    QList<HistoryTurn> historyTurnList;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]:数据库打开失败"<<m_db.lastError();
        return historyTurnList;
    }
    if(N<=0){
        return historyTurnList;
    }

    QSqlQuery query(m_db);
    QString sql = R"(
        SELECT id, user_input, raw_reply, timestamp, turn_kind, subject, speaker
        FROM (
            SELECT id, user_input, raw_reply, timestamp, turn_kind, subject, speaker
            FROM chat_history
            WHERE channel = :channel AND session_id = :session_id AND (:before=0 OR id<:before)
            ORDER BY id DESC
            LIMIT :limit
        ) ORDER BY id ASC;
    )";
    query.prepare(sql);
    query.bindValue(":limit",N);
    query.bindValue(":before",beforeTurn);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);
    if(!query.exec()){
        qDebug()<<"[MemoryManager]获取历史记录失败:"<<query.lastError();
    }
    while(query.next()){
        HistoryTurn turn;
        turn.id = query.value("id").toLongLong();
        turn.userInput = query.value("user_input").toString();
        turn.rawReply = query.value("raw_reply").toString();
        turn.timestamp = query.value("timestamp").toDateTime();
        turn.turnKind = query.value("turn_kind").toString();
        turn.subject = query.value("subject").toString();
        turn.speaker = query.value("speaker").toString();
        historyTurnList.append(turn);
    }
    qDebug()<<"[MemoryManager]获取历史记录成功";
    return historyTurnList;
}

QList<HistoryTurn> MemoryManager::getHistoryTurn(int offset, int limit){
    QList<HistoryTurn> historyList;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return historyList;
    }
    QSqlQuery query(m_db);
    query.prepare(R"(
        SELECT id, user_input, raw_reply, timestamp, turn_kind, subject, speaker,turn_state,turn_detail
        FROM chat_history
        ORDER BY id DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);
    if(!query.exec()){
        qDebug()<<"[MemoryManager]分页查询失败:"<<query.lastError();
        return historyList;
    }
    while (query.next()) {
        HistoryTurn turn;
        turn.id = query.value("id").toInt();
        turn.state = query.value("turn_state").toString();
        turn.detail = query.value("turn_detail").toString();
        turn.userInput = query.value("user_input").toString();
        turn.rawReply = query.value("raw_reply").toString();
        turn.timestamp = query.value("timestamp").toDateTime();
        turn.turnKind = query.value("turn_kind").toString();
        turn.subject = query.value("subject").toString();
        turn.speaker = query.value("speaker").toString();
        historyList.append(turn);
    }
    qDebug()<<"[MemoryManager]分页查询成功";
    return historyList;

}

qlonglong MemoryManager::getTotalHistoryCount()
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return 0;
    }
    QSqlQuery query(m_db);
    if (query.exec("SELECT COUNT(*) FROM chat_history")) {
        if (query.next()) {
                qDebug()<<"[MemoryManager]获取历史总记录数成功";
            return query.value(0).toInt();
        }
    }
    qDebug()<<"[MemoryManager]获取历史总记录数（0）成功";
    return 0;
}

bool MemoryManager::deleteTurnByID(int id)
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM chat_history WHERE id = :id");
    query.bindValue(":id", id);

    if (!query.exec()) {
        qDebug()<<"[MemoryManager]删除id:"<<id<<"的记录失败:"<<query.lastError();
        return false;
    }
    emit understandingChanged();
    qDebug()<<"[MemoryManager]删除id:"<<id<<"的记录成功";
    return true;
}

bool MemoryManager::clearAllHistory()
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    if (!m_db.transaction()) return false;
    query.prepare("DELETE FROM chat_history");
    if (!query.exec() || !query.exec("DELETE FROM cognitive_source") || !m_db.commit()) {
        m_db.rollback();
        qDebug()<<"[MemoryManager]清空记录失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]清空记录成功";
    emit understandingChanged();
    return true;
}

// R12-2c-3：事务内写"茉子主动说过的话"历史 + raw_events；不提交、不建索引。
// 幂等键与记录口径沿用原 recordProactiveMessage：同 channel/session 下 kind+ref 只写一条，
// 渠道拒绝后补发成功不会在历史里留下两条一样的主动表述。返回新轮次 id（0=失败）。
qlonglong MemoryManager::writeProactiveHistoryLocked(const QString &channel, const QString &session,
                                                     const QString &text, const QString &kind,
                                                     const QString &ref, bool *created)
{
    if (created) *created = false;
    const QString body = text.trimmed();
    if (!m_db.isOpen() || body.isEmpty() || channel.isEmpty() || session.isEmpty()) return 0;
    if (!kind.isEmpty() && !ref.isEmpty()) {
        QSqlQuery existing(m_db);
        existing.prepare("SELECT id FROM chat_history WHERE channel=? AND session_id=? "
                         "AND turn_kind='proactive' "
                         "AND json_extract(raw_reply,'$.delivery.kind')=? "
                         "AND json_extract(raw_reply,'$.delivery.ref')=? ORDER BY id LIMIT 1");
        existing.addBindValue(channel);
        existing.addBindValue(session);
        existing.addBindValue(kind);
        existing.addBindValue(ref);
        if (existing.exec() && existing.next()) return existing.value(0).toLongLong();
    }
    const QJsonObject sentence{{QStringLiteral("zh_text"), body},
                               {QStringLiteral("ja_text"), QString{}},
                               {QStringLiteral("tags"), QJsonObject{}}};
    const QJsonObject root{{QStringLiteral("sentences"), QJsonArray{sentence}},
                           {QStringLiteral("delivery"), QJsonObject{{QStringLiteral("kind"), kind},
                                                                    {QStringLiteral("ref"), ref}}}};
    const QString rawReply = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO chat_history(user_input,raw_reply,channel,session_id,speaker,sensitive,"
              "turn_kind,subject,turn_state,cognitive_event,timestamp) "
              "VALUES(NULL,?,?,?,'mako',0,'proactive','user','handed_off',NULL,?)");
    q.addBindValue(rawReply);
    q.addBindValue(channel);
    q.addBindValue(session);
    q.addBindValue(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "[MemoryManager]主动消息写历史失败:" << q.lastError();
        return 0;
    }
    const qlonglong id = q.lastInsertId().toLongLong();
    RawEventWrite event;
    event.platform = channel == Channel::kPet ? QStringLiteral("pet")
        : (channel == Channel::kQQPrivate || channel == Channel::kQQGroup) ? QStringLiteral("qq")
        : channel;
    event.channel = channel;
    event.sessionId = session;
    event.eventType = QStringLiteral("message");
    event.content = body;
    event.direction = QStringLiteral("out");
    event.senderName = QStringLiteral("茉子");
    event.occurredAt = QDateTime::currentDateTime();
    event.sensitive = false;
    event.processed = true;
    event.visibility = channel == Channel::kQQGroup ? QStringLiteral("channel")
                                                    : QStringLiteral("private");
    event.redactionStatus = QStringLiteral("none");
    if (!appendRawEvent(event)) {
        qWarning() << "[MemoryManager]主动消息写原始事件失败:" << m_db.lastError();
        return 0;
    }
    if (created) *created = true;
    return id;
}

qlonglong MemoryManager::recordProactiveMessage(const QString &channel, const QString &session, const QString &text,
                                                const QString &kind, const QString &ref)
{
    const QString body = text.trimmed();
    if (!m_db.isOpen() || body.isEmpty() || channel.isEmpty() || session.isEmpty()) return 0;
    if (!m_db.transaction()) return 0;
    bool created = false;
    const qlonglong id = writeProactiveHistoryLocked(channel, session, body, kind, ref, &created);
    if (id <= 0) {
        m_db.rollback();
        return 0;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    // commit 后再建检索块：主动轮次没有用户块，assistant 块收录这里说的 zh_text。
    if (created && !indexChatTurn(id))
        qWarning() << "[MemoryIndex] proactive chat index failed:" << id;
    else if (created)
        emit searchChunksChanged();
    qInfo() << "[Proactive] recorded" << kind << ref << channel << session << "turn=" << id;
    return id;
}


QList<HistoryTurn> MemoryManager::getUnsummarizedTurns(qlonglong &outLastEndId, QList<qlonglong> &outSourceIds, const QString &channel, const QString &sessionId, qint64 throughTurn)
{
    QList<HistoryTurn> turns;
    outSourceIds.clear();
    outLastEndId = -1;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return turns;
    }
    QSqlQuery query(m_db);
    // 摘要游标按 channel+session_id 双键取（与摘要分桶一致）
    QString sql = R"(
        SELECT id, user_input, raw_reply, turn_kind, subject, speaker
        FROM chat_history
        WHERE channel = :channel AND session_id = :session_id
          AND (:through=0 OR id<=:through)
          AND id > (SELECT COALESCE(MAX(covered_turn_end_id), 0) FROM long_term_summary
                    WHERE channel = :channel2 AND session_id = :session_id2)
        ORDER BY id ASC;
    )";
    query.prepare(sql);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);
    query.bindValue(":through", throughTurn);
    query.bindValue(":channel2", channel);
    query.bindValue(":session_id2", sessionId);
    if(query.exec()){
        while(query.next()){
            qlonglong id=query.value("id").toLongLong();
            HistoryTurn turn;
            turn.id = id;
            turn.userInput = query.value("user_input").toString();
            turn.rawReply = query.value("raw_reply").toString();
            turn.turnKind = query.value("turn_kind").toString();
            turn.subject = query.value("subject").toString();
            turn.speaker = query.value("speaker").toString();

            turns.append(turn);
            outSourceIds.append(id);
            outLastEndId = id;
        }
    }
    else{
        qDebug()<<"[MemoryManager]获取未摘要历史失败"<<query.lastError();
    }
    qDebug()<<"[MemoryManager]获取未摘要历史成功";
    return turns;
}

// ============== v4: 角色档案 CRUD ==============

bool MemoryManager::upsertCharacterProfile(const QString &subject, const QString &key, const QString &value)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    // key 归一化：同 subject 内用编辑距离匹配已有 key，避免语义重复
    QString normalizedKey = normalizeProfileKey(subject, key);
    // 先尝试查询已有记录（同 subject + normalizedKey）
    query.prepare("SELECT id, value FROM character_profile WHERE subject=? AND key=?");
    query.addBindValue(subject);
    query.addBindValue(normalizedKey);
    if (query.exec() && query.next()) {
        int existingId = query.value(0).toInt();
        QString existingValue = query.value(1).toString();
        // 合并 value：去重合并
        QString mergedValue = mergeProfileValue(normalizedKey, existingValue, value);
        query.prepare("UPDATE character_profile SET value=?, updated_at=? WHERE id=?");
        query.addBindValue(mergedValue);
        query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        query.addBindValue(existingId);
        if (!query.exec()) {
            qDebug() << "[MemoryManager]更新角色档案失败:" << query.lastError();
            return false;
        }
        qDebug() << "[MemoryManager]角色档案已更新:" << subject << normalizedKey << mergedValue;
        return true;
    } else {
        // 新增
        query.prepare("INSERT OR REPLACE INTO character_profile (subject, key, value, updated_at) VALUES (?, ?, ?, ?)");
        query.addBindValue(subject);
        query.addBindValue(normalizedKey);
        query.addBindValue(value);
        query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        if (!query.exec()) {
            qDebug() << "[MemoryManager]新增角色档案失败:" << query.lastError();
            return false;
        }
        qDebug() << "[MemoryManager]角色档案已新增:" << subject << normalizedKey
                 << "valueLen=" << value.size();
        return true;
    }
}

QList<CharacterProfile> MemoryManager::getCharacterProfiles(const QString &subject)
{
    QList<CharacterProfile> profiles;
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return profiles;
    }
    QSqlQuery query(m_db);
    if (subject.isEmpty()) {
        query.prepare("SELECT id, subject, key, value, updated_at FROM character_profile ORDER BY subject, key");
    } else {
        query.prepare("SELECT id, subject, key, value, updated_at FROM character_profile WHERE subject=? ORDER BY key");
        query.addBindValue(subject);
    }
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询角色档案失败:" << query.lastError();
        return profiles;
    }
    while (query.next()) {
        CharacterProfile p;
        p.id = query.value(0).toLongLong();
        p.subject = query.value(1).toString();
        p.key = query.value(2).toString();
        p.value = query.value(3).toString();
        p.updatedAt = query.value(4).toDateTime();
        profiles.append(p);
    }
    qDebug() << "[MemoryManager]获取角色档案成功，共" << profiles.size() << "条";
    return profiles;
}

bool MemoryManager::deleteCharacterProfile(qlonglong id)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM character_profile WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]删除角色档案失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]角色档案已删除 id=" << id;
    return true;
}

// ============== v4: 情景记忆 CRUD ==============

bool MemoryManager::addEpisodicMemory(const QString &content, const QString &type, double importance,
                                      const QDateTime &eventTime, const QString &sourceIds,
                                      const QString &origin, const QString &visibility, const QString &subject,
                                      const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO episodic_memory "
                  "(content, event_time, importance, type, status, last_accessed, last_decay_at, source_ids, origin, visibility, subject, session_id) "
                  "VALUES (?, ?, ?, ?, 'active', ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(content);
    QDateTime et = eventTime.isValid() ? eventTime : QDateTime::currentDateTime();
    query.addBindValue(et.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(importance);
    query.addBindValue(type.isEmpty() ? "event" : type);
    query.addBindValue(et.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(et.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(sourceIds);
    query.addBindValue(origin.isEmpty() ? QStringLiteral("pet") : origin);
    query.addBindValue(visibility.isEmpty() ? QStringLiteral("private") : visibility);
    query.addBindValue(subject.isEmpty() ? QStringLiteral("user") : subject);
    query.addBindValue(sessionId.isEmpty() ? QStringLiteral("default") : sessionId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]新增情景记忆失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]情景记忆已新增: chars=" << content.size()
             << "type=" << type << "importance=" << importance
             << "origin=" << origin << "visibility=" << visibility
             << "subject=" << subject;
    if (debugFullLogsEnabled()) {
        qDebug().noquote() << "[MemoryManager][debug] episodic:" << content;
    }
    return true;
}

QList<EpisodicMemory> MemoryManager::getActiveEpisodicMemories(double minImportance, int limit,
                                                              const QString &origin, const QString &visibility,
                                                              const QString &sessionId, bool touch)
{
    QList<EpisodicMemory> memories;
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return memories;
    }
    QSqlQuery query(m_db);
    QString sql = "SELECT id, content, event_time, importance, type, status, last_accessed, source_ids, origin, visibility, subject, session_id "
                  "FROM episodic_memory WHERE status='active' AND importance >= :min";
    if (!origin.isEmpty()) {
        sql += " AND origin = :origin";
    }
    if (!visibility.isEmpty()) {
        sql += " AND visibility = :visibility";
    }
    if (!sessionId.isEmpty()) {
        sql += " AND session_id = :session_id";
    }
    sql += " ORDER BY importance DESC, event_time DESC LIMIT :limit";
    query.prepare(sql);
    query.bindValue(":min", minImportance);
    query.bindValue(":limit", limit);
    if (!origin.isEmpty()) {
        query.bindValue(":origin", origin);
    }
    if (!visibility.isEmpty()) {
        query.bindValue(":visibility", visibility);
    }
    if (!sessionId.isEmpty()) {
        query.bindValue(":session_id", sessionId);
    }
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询情景记忆失败:" << query.lastError();
        return memories;
    }
    while (query.next()) {
        EpisodicMemory m;
        m.id = query.value(0).toLongLong();
        m.content = query.value(1).toString();
        m.eventTime = query.value(2).toDateTime();
        m.importance = query.value(3).toDouble();
        m.type = query.value(4).toString();
        m.status = query.value(5).toString();
        m.lastAccessed = query.value(6).toDateTime();
        m.sourceIds = query.value(7).toString();
        m.origin = query.value(8).toString();
        m.visibility = query.value(9).toString();
        m.subject = query.value(10).toString();
        m.sessionId = query.value(11).toString();
        memories.append(m);
    }

    // 更新 last_accessed
    if (touch && !memories.isEmpty()) {
        QStringList ids;
        for (const auto &m : memories) ids << QString::number(m.id);
        QSqlQuery upd(m_db);
        upd.prepare(QString("UPDATE episodic_memory SET last_accessed=? WHERE id IN (%1)")
                        .arg(ids.join(",")));
        upd.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        upd.exec();
    }

    qDebug() << "[MemoryManager]获取情景记忆成功，共" << memories.size() << "条";
    return memories;
}

bool MemoryManager::deleteEpisodicMemory(qlonglong id)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM episodic_memory WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]删除情景记忆失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]情景记忆已删除 id=" << id;
    return true;
}

bool MemoryManager::updateEpisodicMemoryStatus(qlonglong id, const QString &status)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE episodic_memory SET status=? WHERE id=?");
    query.addBindValue(status);
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]更新情景记忆状态失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]情景记忆状态已更新 id=" << id << "status=" << status;
    return true;
}

int MemoryManager::decayEpisodicMemory()
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return -1;
    }
    // 衰减规则：
    // - importance >= 0.8 的里程碑/承诺不衰减
    // - 其余按 -0.05/天 衰减
    // - importance < 0.1 删除
    m_db.transaction();
    try {
        QSqlQuery query(m_db);
        QString decaySql = R"(
            UPDATE episodic_memory
            SET importance = MAX(0, importance - 0.05 * (julianday('now', 'localtime') - julianday(last_decay_at))),
                last_decay_at = datetime('now', 'localtime')
            WHERE importance < 0.8
              AND (julianday('now', 'localtime') - julianday(last_decay_at)) >= 0.0416
        )";
        if (!query.exec(decaySql)) throw query.lastError();
        int updatedRows = query.numRowsAffected();

        QString cleanSql = "DELETE FROM episodic_memory WHERE importance < 0.1 AND status='active'";
        if (!query.exec(cleanSql)) throw query.lastError();
        int deletedRows = query.numRowsAffected();

        m_db.commit();
        if (updatedRows > 0 || deletedRows > 0) {
            qDebug()<<QString("[MemoryManager]情景记忆衰减完成:更新%1条，删除%2条。")
                            .arg(updatedRows).arg(deletedRows);
        } else {
            qDebug()<<"[MemoryManager]情景记忆保持最新，无需衰减。";
        }
        return deletedRows;
    } catch (const QSqlError &err) {
        m_db.rollback();
        qDebug()<<"[MemoryManager]情景记忆衰减失败:"<<err.text();
        return -1;
    }
}

QString MemoryManager::normalizeProfileKey(const QString &subject, const QString &rawKey)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT DISTINCT key FROM character_profile WHERE subject=?");
    query.addBindValue(subject);
    if(!query.exec()){
        qDebug()<<"[MemoryManager]归一化查询失败，保留原key:"<<rawKey;
        return rawKey;
    }
    QStringList existingKeys;
    while(query.next()){
        existingKeys<<query.value(0).toString();
    }
    //全匹配
    if(existingKeys.contains(rawKey)){
        return rawKey;
    }
    //编辑距离模糊匹配（同 subject 内）
    int threshold=getEditDistanceThreshold(rawKey);
    QString bestMatch;
    int minDist=threshold+1;
    for (const QString &existingKey : existingKeys) {
        int dist =levenshteinDistance(rawKey, existingKey);
        if (dist <= threshold && dist < minDist) {
            minDist = dist;
            bestMatch = existingKey;
        }
    }
    if (!bestMatch.isEmpty()) {
        qDebug()<<"[MemoryManager]key归一化["<<subject<<"]:"<<rawKey<<"->"<<bestMatch<<"编辑距离="<<minDist;
        return bestMatch;
    }
    qDebug()<<"[MemoryManager]新key维度["<<subject<<"]:"<<rawKey;
    return rawKey;
}

QString MemoryManager::mergeProfileValue(const QString &key, const QString &oldVal, const QString &newVal)
{
    //同key的值value的合并逻辑

    if (newVal.isEmpty()) return oldVal;
    if (newVal.length() >= oldVal.length()) return newVal;
    return oldVal;
}

int MemoryManager::levenshteinDistance(const QString &s1, const QString &s2)
{
    int n=s1.length();
    int m=s2.length();
    if(m==0)return m;
    if(n==0)return n;

    QVector<QVector<int>>dp(n+1,QVector<int>(m+1));

    for (int i = 0; i <= n; i++) dp[i][0] = i;
    for (int j = 0; j <= m; j++) dp[0][j] = j;

    for(int i=1;i<=n;i++){
        for(int j =1;j<=m;j++) {
            int cost=(s1[i-1]==s2[j-1]?0:1);
            dp[i][j]=qMin(qMin(dp[i-1][j]+1,dp[i][j-1]+1),dp[i-1][j-1]+cost);
        }
    }
    return dp[n][m];
}

int MemoryManager::getEditDistanceThreshold(const QString &value)
{
    int len = value.length();
    if (len <= 4) return 1;
    if (len <= 8) return 2;
    if (len <= 12) return 3;
    return 4;
}

bool MemoryManager::addLongTermSummary(const QString &summaryText, qlonglong coveredEndId, const QString &sourceIdsJson, const QString &channel, const QString &sessionId)
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    QString sql = R"(
        INSERT INTO long_term_summary (summary_text, covered_turn_end_id, source_ids, is_dirty, channel, session_id)
        VALUES (:text, :end_id, :source_ids, 0, :channel, :session_id)
    )";

    query.prepare(sql);
    query.bindValue(":text", summaryText);
    query.bindValue(":end_id", coveredEndId);
    query.bindValue(":source_ids", sourceIdsJson);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);

    if (!query.exec()) {
        qDebug()<<"[MemoryManager]添加记忆摘要失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]添加记忆摘要成功";
    return true;
}

QList<LongTermSummary> MemoryManager::getLatestSummaries(int limit, const QString &channel, const QString &sessionId)
{
    QList<LongTermSummary> summaries;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]:数据库打开失败"<<m_db.lastError();
        return summaries;
    }
    if(limit<=0){
        return summaries;
    }

    QSqlQuery query(m_db);
    QString sql = R"(
        SELECT * FROM (
            SELECT * FROM long_term_summary
            WHERE is_dirty = 0 AND channel = :channel AND session_id = :session_id
            ORDER BY id DESC
            LIMIT :limit
        ) ORDER BY id ASC
    )";

    query.prepare(sql);
    query.bindValue(":limit", limit);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);

    if (query.exec()) {
        while (query.next()) {
            LongTermSummary s;
            s.id = query.value("id").toLongLong();
            s.summaryText = query.value("summary_text").toString();
            s.coveredTurnEndId = query.value("covered_turn_end_id").toLongLong();
            s.sourceIds = query.value("source_ids").toString();
            s.isDirty = query.value("is_dirty").toBool();
            s.createdAt = query.value("created_at").toDateTime();
            s.updatedAt = query.value("updated_at").toDateTime();
            summaries.append(s);
        }
    }
    else{
        if (!query.exec()) {
            qDebug()<<"[MemoryManager]获取记忆摘要失败:"<<query.lastError();
            return summaries;
        }
    }
    qDebug()<<"[MemoryManager]获取记忆摘要成功";
    return summaries;
}

bool MemoryManager::deleteLongTermSummary(qlonglong id)
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM long_term_summary WHERE id = :id");
    query.bindValue(":id", id);
    if (!query.exec()) {
        qDebug()<<"[MemoryManager]删除记忆摘要失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]删除记忆摘要成功";
    return true;
}


// ============== 关系状态 CRUD ==============

bool MemoryManager::upsertRelationshipState(const QString &dimension, double delta, const QString &subject)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    // 查询当前值（subject 隔离：'user' 即主人）
    query.prepare("SELECT value FROM relationship_state WHERE subject=? AND dimension=?");
    query.addBindValue(subject);
    query.addBindValue(dimension);
    if (!query.exec() || !query.next()) {
        // 维度不存在，新建（clamp 到 [0, 100]）
        double initVal = qBound(0.0, 30.0 + delta, 100.0);
        query.prepare("INSERT OR REPLACE INTO relationship_state (subject, dimension, value, updated_at) VALUES (?, ?, ?, ?)");
        query.addBindValue(subject);
        query.addBindValue(dimension);
        query.addBindValue(initVal);
        query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        if (!query.exec()) {
            qDebug() << "[MemoryManager]新增关系状态失败:" << query.lastError();
            return false;
        }
        qDebug() << "[MemoryManager]关系状态已新增:" << subject << dimension << "=" << initVal;
        return true;
    }
    double oldVal = query.value(0).toDouble();
    double newVal = qBound(0.0, oldVal + delta, 100.0);
    query.prepare("UPDATE relationship_state SET value=?, updated_at=? WHERE subject=? AND dimension=?");
    query.addBindValue(newVal);
    query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(subject);
    query.addBindValue(dimension);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]更新关系状态失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]关系状态已更新:" << subject << dimension << oldVal << "->" << newVal << "(delta=" << delta << ")";
    return true;
}

QList<RelationshipState> MemoryManager::getRelationshipStates(const QString &subject)
{
    QList<RelationshipState> states;
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return states;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT dimension, value, updated_at FROM relationship_state WHERE subject=? ORDER BY dimension");
    query.addBindValue(subject);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询关系状态失败:" << query.lastError();
        return states;
    }
    while (query.next()) {
        RelationshipState s;
        s.subject = subject;
        s.dimension = query.value(0).toString();
        s.value = query.value(1).toDouble();
        s.updatedAt = query.value(2).toDateTime();
        states.append(s);
    }
    qDebug() << "[MemoryManager]获取关系状态成功，subject=" << subject << "共" << states.size() << "条";
    return states;
}

bool MemoryManager::initRelationshipState()
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM relationship_state");
    query.exec();
    int count = 0;
    if (query.next()) count = query.value(0).toInt();
    if (count > 0) {
        qDebug() << "[MemoryManager]关系状态已存在，无需初始化";
        return true;
    }
    query.prepare("INSERT OR IGNORE INTO relationship_state (subject, dimension, value) VALUES ('user', 'intimacy', 30.0)");
    bool ok1 = query.exec();
    query.prepare("INSERT OR IGNORE INTO relationship_state (subject, dimension, value) VALUES ('user', 'trust', 30.0)");
    bool ok2 = query.exec();
    qDebug() << "[MemoryManager]关系状态初始化:" << (ok1 && ok2 ? "成功" : "失败");
    return ok1 && ok2;
}


// 幂等加列：预检列存在性 + duplicate column 容忍，应对重复/并发启动（三重防御）
static bool ensureColumn(QSqlQuery &query, const QString &table, const QString &column, const QString &alterDdl)
{
    // 1. 预检：列已存在则跳过
    if (query.exec(QString("PRAGMA table_info(%1)").arg(table))) {
        while (query.next()) {
            if (query.value(1).toString() == column) {
                return true;
            }
        }
    }
    // 2. 执行加列
    if (query.exec(alterDdl)) {
        qDebug() << "[MemoryManager]已加列:" << table << "." << column;
        return true;
    }
    // 3. 容忍：并发窗口内另一连接已完成加列
    const QString err = query.lastError().text();
    if (err.contains("duplicate column", Qt::CaseInsensitive)) {
        qDebug() << "[MemoryManager]列已被其他连接添加(并发忽略):" << column;
        return true;
    }
    qDebug() << "[MemoryManager]加列失败:" << alterDdl << "错误:" << err;
    return false;
}

bool MemoryManager::appendRawEvent(const QString &platform, const QString &channel,
                                   const QString &sessionId, const QString &eventType,
                                   const QString &content, const QString &messageId,
                                   const QString &direction, const QString &senderId,
                                   const QString &senderName, const QString &payloadJson,
                                   const QDateTime &occurredAt, bool sensitive,
                                   const QString &visibility)
{
    RawEventWrite event;
    event.platform = platform;
    event.channel = channel;
    event.sessionId = sessionId;
    event.eventType = eventType;
    event.content = content;
    event.messageId = messageId;
    event.direction = direction;
    event.senderId = senderId;
    event.senderName = senderName;
    event.payloadJson = payloadJson;
    event.occurredAt = occurredAt;
    event.sensitive = sensitive;
    event.visibility = visibility;
    return appendRawEvent(event);
}

bool MemoryManager::appendRawEvent(const RawEventWrite &event)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    const QString groupId = (event.channel == Channel::kQQGroup)
                                ? event.sessionId
                                : QString();
    QString personId = event.personId;
    if (personId.isEmpty() && !event.senderId.isEmpty()) {
        personId = resolvePersonId(event.platform, event.senderId, groupId);
        ensurePersonIdentity(event.platform, event.senderId, groupId,
                             event.senderName, event.occurredAt);
    }

    QSqlQuery query(m_db);
    query.prepare("INSERT OR IGNORE INTO raw_events "
                  "(platform, channel, session_id, message_id, direction, sender_id, sender_name, "
                  " person_id, event_type, content, payload, occurred_at, sensitive, visibility, processed, "
                  " redaction_status, redaction_rules, policy_version) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(event.platform);
    query.addBindValue(event.channel);
    query.addBindValue(event.sessionId);
    query.addBindValue(event.messageId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.messageId));
    query.addBindValue(event.direction.isEmpty()
                           ? QStringLiteral("in")
                           : event.direction);
    query.addBindValue(event.senderId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.senderId));
    query.addBindValue(event.senderName.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.senderName));
    query.addBindValue(personId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(personId));
    query.addBindValue(event.eventType);
    query.addBindValue(event.contentNull
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.content));
    query.addBindValue(event.payloadNull || event.payloadJson.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.payloadJson));
    query.addBindValue((event.occurredAt.isValid()
                            ? event.occurredAt
                            : QDateTime::currentDateTime())
                           .toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(event.sensitive ? 1 : 0);
    query.addBindValue(event.visibility.isEmpty()
                           ? QStringLiteral("private")
                           : event.visibility);
    query.addBindValue(event.processed ? 1 : 0);
    query.addBindValue(event.redactionStatus.isEmpty()
                           ? QStringLiteral("none")
                           : event.redactionStatus);
    query.addBindValue(event.redactionRules.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.redactionRules));
    query.addBindValue(event.policyVersion.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.policyVersion));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]写入raw_events失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]raw_events写入: platform=" << event.platform
             << "channel=" << event.channel << "session=" << event.sessionId
             << "type=" << event.eventType << "affected=" << query.numRowsAffected();
    const qlonglong rawId = query.lastInsertId().toLongLong();
    if (rawId > 0 && query.numRowsAffected() > 0 && !indexRawEvent(rawId))
        qWarning() << "[MemoryIndex] incremental raw index failed:" << rawId;
    if (debugFullLogsEnabled()) {
        qDebug().noquote() << "[MemoryManager][debug] raw content:" << event.content
                           << "payload:" << event.payloadJson;
    }
    return true;
}

bool MemoryManager::appendPrivacyBlockedEvent(const QString &platform,
                                              const QString &channel,
                                              const QString &sessionId,
                                              const QString &messageId,
                                              const QString &senderId,
                                              const QString &senderName,
                                              const QDateTime &occurredAt,
                                              const QStringList &ruleIds,
                                              const QString &policyVersion)
{
    QJsonArray rulesArray;
    for (const QString &ruleId : ruleIds) {
        rulesArray.append(ruleId);
    }
    const QString rulesJson = QString::fromUtf8(
        QJsonDocument(rulesArray).toJson(QJsonDocument::Compact));

    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }

    // 同一 message_id 之前可能已以 clean 状态入库；命中 blocked 后必须清空旧原文。
    if (!messageId.isEmpty()) {
        QSqlQuery update(m_db);
        update.prepare("UPDATE raw_events SET event_type='privacy_blocked', "
                       "content=NULL, payload=NULL, sensitive=1, "
                       "redaction_status='blocked', redaction_rules=?, "
                       "policy_version=?, processed=1 "
                       "WHERE channel=? AND message_id=?");
        update.addBindValue(rulesJson);
        update.addBindValue(policyVersion);
        update.addBindValue(channel);
        update.addBindValue(messageId);
        if (!update.exec()) {
            qDebug() << "[MemoryManager]清理旧 raw_events 失败:" << update.lastError();
            return false;
        }
        if (update.numRowsAffected() > 0) {
            qDebug() << "[MemoryManager]raw_events 已转为 privacy_blocked:"
                     << channel << sessionId;
            return true;
        }
    }

    RawEventWrite event;
    event.platform = platform;
    event.channel = channel;
    event.sessionId = sessionId;
    event.eventType = QStringLiteral("privacy_blocked");
    event.messageId = messageId;
    event.senderId = senderId;
    event.senderName = senderName;
    event.occurredAt = occurredAt;
    event.sensitive = true;
    event.visibility = QStringLiteral("private");
    event.redactionStatus = QStringLiteral("blocked");
    event.redactionRules = rulesJson;
    event.policyVersion = policyVersion;
    event.contentNull = true;
    event.payloadNull = true;
    event.processed = true;
    return appendRawEvent(event);
}

QList<RawEvent> MemoryManager::getUnprocessedGroupEvents(const QString &sessionId, int limit)
{
    QList<RawEvent> events;
    if (!m_db.isOpen() && !m_db.open()) {
        return events;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM raw_events "
                                 "WHERE channel='qq_group' AND session_id=? "
                                 "AND processed=0 AND event_type='message' "
                                 "ORDER BY id ASC LIMIT ?")
                      .arg(rawEventColumns()));
    query.addBindValue(sessionId);
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询未处理群事件失败:" << query.lastError();
        return events;
    }
    while (query.next()) {
        events.append(rawEventFromQuery(query));
    }
    return events;
}

QList<QString> MemoryManager::getPendingGroupSessions()
{
    QList<QString> sessions;
    if (!m_db.isOpen() && !m_db.open()) {
        return sessions;
    }
    QSqlQuery query(m_db);
    if (!query.exec("SELECT DISTINCT session_id FROM raw_events "
                    "WHERE channel='qq_group' AND processed=0 "
                    "AND event_type='message'")) {
        qDebug() << "[MemoryManager]查询待消费群会话失败:" << query.lastError();
        return sessions;
    }
    while (query.next()) {
        sessions.append(query.value(0).toString());
    }
    return sessions;
}

int MemoryManager::countUnprocessedGroupEvents(const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return 0;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM raw_events "
                  "WHERE channel='qq_group' AND session_id=? "
                  "AND processed=0 AND event_type='message'");
    query.addBindValue(sessionId);
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

QDateTime MemoryManager::latestUnprocessedGroupEventTime(const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return QDateTime();
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT MAX(occurred_at) FROM raw_events "
                  "WHERE channel='qq_group' AND session_id=? "
                  "AND processed=0 AND event_type='message'");
    query.addBindValue(sessionId);
    if (query.exec() && query.next()) {
        return query.value(0).toDateTime();
    }
    return QDateTime();
}

qlonglong MemoryManager::createInteractionBatch(const InteractionDraft &draft,
                                                const QList<qlonglong> &eventIds)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return -1;
    }
    if (eventIds.isEmpty()) {
        return -1;
    }

    QJsonArray sourceIds;
    for (qlonglong id : eventIds) {
        sourceIds.append(static_cast<double>(id));
    }
    const QString sourceIdsJson = QString::fromUtf8(
        QJsonDocument(sourceIds).toJson(QJsonDocument::Compact));

    m_db.transaction();
    QSqlQuery insert(m_db);
    insert.prepare("INSERT INTO interactions "
                   "(channel, session_id, started_at, ended_at, source_event_ids, "
                   " segmentation_method, confidence, summary_status, summary_attempts) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', 0)");
    insert.addBindValue(draft.channel);
    insert.addBindValue(draft.sessionId);
    insert.addBindValue((draft.startedAt.isValid() ? draft.startedAt
                                                   : QDateTime::currentDateTime())
                            .toString("yyyy-MM-dd HH:mm:ss"));
    insert.addBindValue((draft.endedAt.isValid() ? draft.endedAt
                                                 : QDateTime::currentDateTime())
                            .toString("yyyy-MM-dd HH:mm:ss"));
    insert.addBindValue(sourceIdsJson);
    insert.addBindValue(draft.segmentationMethod);
    insert.addBindValue(draft.confidence);
    if (!insert.exec()) {
        qDebug() << "[MemoryManager]创建 interaction 失败:" << insert.lastError();
        m_db.rollback();
        return -1;
    }
    const qlonglong interactionId = insert.lastInsertId().toLongLong();

    for (const auto &participant : draft.participants) {
        QString personId = draft.participantPersonIds.value(participant.first);
        if (personId.isEmpty()) {
            personId = resolvePersonId(QStringLiteral("qq"), participant.first,
                                       draft.channel == Channel::kQQGroup
                                           ? draft.sessionId
                                           : QString());
        }
        QSqlQuery p(m_db);
        p.prepare("INSERT OR IGNORE INTO interaction_participants "
                  "(interaction_id, user_id, display_name_at_time, person_id) "
                  "VALUES (?, ?, ?, ?)");
        p.addBindValue(interactionId);
        p.addBindValue(participant.first);
        p.addBindValue(participant.second);
        p.addBindValue(personId);
        if (!p.exec()) {
            qDebug() << "[MemoryManager]写入 interaction participant 失败:" << p.lastError();
            m_db.rollback();
            return -1;
        }
    }

    QStringList placeholders;
    for (int i = 0; i < eventIds.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery mark(m_db);
    mark.prepare(QStringLiteral("UPDATE raw_events SET processed=1 WHERE id IN (%1)")
                     .arg(placeholders.join(',')));
    for (qlonglong id : eventIds) {
        mark.addBindValue(id);
    }
    if (!mark.exec()) {
        qDebug() << "[MemoryManager]标记 raw_events processed 失败:" << mark.lastError();
        m_db.rollback();
        return -1;
    }

    const qlonglong lastEventId = eventIds.last();
    QSqlQuery cursor(m_db);
    cursor.prepare("INSERT OR IGNORE INTO memory_cursor "
                   "(channel, session_id, last_event_id, last_turn_id, updated_at) "
                   "VALUES (?, ?, 0, 0, datetime('now','localtime'))");
    cursor.addBindValue(draft.channel);
    cursor.addBindValue(draft.sessionId);
    if (!cursor.exec()) {
        qDebug() << "[MemoryManager]初始化 memory_cursor 失败:" << cursor.lastError();
        m_db.rollback();
        return -1;
    }
    QSqlQuery updateCursor(m_db);
    updateCursor.prepare("UPDATE memory_cursor SET last_event_id=?, "
                         "updated_at=datetime('now','localtime') "
                         "WHERE channel=? AND session_id=?");
    updateCursor.addBindValue(lastEventId);
    updateCursor.addBindValue(draft.channel);
    updateCursor.addBindValue(draft.sessionId);
    if (!updateCursor.exec()) {
        qDebug() << "[MemoryManager]推进 memory_cursor 失败:" << updateCursor.lastError();
        m_db.rollback();
        return -1;
    }

    if (!m_db.commit()) {
        qDebug() << "[MemoryManager]提交 interaction 事务失败:" << m_db.lastError();
        m_db.rollback();
        return -1;
    }
    return interactionId;
}

QList<RawEvent> MemoryManager::getRawEventsByIds(const QList<qlonglong> &ids)
{
    QList<RawEvent> events;
    if (ids.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return events;
    }
    QStringList placeholders;
    for (int i = 0; i < ids.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM raw_events WHERE id IN (%2) ORDER BY id ASC")
                      .arg(rawEventColumns(), placeholders.join(',')));
    for (qlonglong id : ids) {
        query.addBindValue(id);
    }
    if (!query.exec()) {
        qDebug() << "[MemoryManager]按 id 查询 raw_events 失败:" << query.lastError();
        return events;
    }
    while (query.next()) {
        events.append(rawEventFromQuery(query));
    }
    return events;
}

QList<qlonglong> MemoryManager::getPendingInteractionIds(int limit)
{
    QList<qlonglong> ids;
    if (!m_db.isOpen() && !m_db.open()) {
        return ids;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id FROM interactions "
                  "WHERE summary_status='pending' AND summary_attempts < 3 "
                  "ORDER BY id ASC LIMIT ?");
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询待摘要 interaction 失败:" << query.lastError();
        return ids;
    }
    while (query.next()) {
        ids.append(query.value(0).toLongLong());
    }
    return ids;
}

QList<RawEvent> MemoryManager::getInteractionSourceEvents(qlonglong interactionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return {};
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT source_event_ids FROM interactions WHERE id=?");
    query.addBindValue(interactionId);
    if (!query.exec() || !query.next()) {
        return {};
    }
    const QJsonArray ids = QJsonDocument::fromJson(
        query.value(0).toString().toUtf8()).array();
    QList<qlonglong> eventIds;
    for (const QJsonValue &value : ids) {
        eventIds.append(static_cast<qlonglong>(value.toDouble()));
    }
    return getRawEventsByIds(eventIds);
}

bool MemoryManager::saveInteractionSummary(qlonglong id, const QString &summary,
                                           const QString &status)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE interactions SET safe_summary=?, summary_status=?, "
                  "summary_attempts=CASE WHEN ?='ready' THEN 0 ELSE summary_attempts END "
                  "WHERE id=?");
    query.addBindValue(summary.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(summary));
    query.addBindValue(status);
    query.addBindValue(status);
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]保存 interaction summary 失败:" << query.lastError();
        return false;
    }
    return true;
}

bool MemoryManager::saveInteractionSummaryWithClaims(
    qlonglong id,
    const QString &summary,
    const QString &summaryFormat,
    const QString &segmentsJson,
    const QList<ClaimDraft> &claims,
    const QString &status)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return false;
    }

    QSqlQuery sourceQuery(m_db);
    sourceQuery.prepare("SELECT source_event_ids, channel, session_id "
                        "FROM interactions WHERE id=?");
    sourceQuery.addBindValue(id);
    if (!sourceQuery.exec() || !sourceQuery.next()) {
        qDebug() << "[MemoryManager]读取 interaction claim 来源失败:" << sourceQuery.lastError();
        return false;
    }
    const QString sourceEventIds = sourceQuery.value(0).toString();
    const QString sourceChannel = sourceQuery.value(1).toString();
    const QString sourceSession = sourceQuery.value(2).toString();

    m_db.transaction();
    QSqlQuery update(m_db);
    update.prepare("UPDATE interactions SET safe_summary=?, summary_status=?, "
                   "summary_format=?, summary_segments=?, "
                   "summary_attempts=CASE WHEN ?='ready' THEN 0 ELSE summary_attempts END "
                   "WHERE id=?");
    update.addBindValue(summary.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(summary));
    update.addBindValue(status);
    update.addBindValue(summaryFormat.isEmpty()
                           ? QStringLiteral("legacy")
                           : summaryFormat);
    update.addBindValue(segmentsJson.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(segmentsJson));
    update.addBindValue(status);
    update.addBindValue(id);
    if (!update.exec()) {
        qDebug() << "[MemoryManager]保存 interaction summary/segments 失败:" << update.lastError();
        m_db.rollback();
        return false;
    }

    for (const ClaimDraft &claim : claims) {
        const QString statement = claim.statement.trimmed();
        if (statement.isEmpty()) {
            continue;
        }
        QString asserted = claim.assertedByPersonId.isEmpty()
                               ? QStringLiteral("unknown")
                               : claim.assertedByPersonId;
        QString about = claim.aboutPersonId.isEmpty()
                            ? QStringLiteral("unknown")
                            : claim.aboutPersonId;
        QString owner = claim.privacyOwnerPersonId.isEmpty()
                            ? QStringLiteral("unknown")
                            : claim.privacyOwnerPersonId;
        QString claimStatus = QStringLiteral("quarantine");
        if (asserted != QStringLiteral("unknown")
            && about != QStringLiteral("unknown")
            && owner != QStringLiteral("unknown")
            && asserted == about
            && about == owner
            && sourceChannel == QStringLiteral("qq_group")
            && claim.privacyLevel <= 2) {
            // 保守激活：只允许群内公开的自述事实。
            claimStatus = QStringLiteral("active");
        }
        QSqlQuery insert(m_db);
        insert.prepare("INSERT INTO memory_claims "
                       "(interaction_id, asserted_by_person_id, about_person_id, "
                       " privacy_owner_person_id, counterpart_person_id, statement, "
                       " privacy_level, source_event_ids, source_channel, "
                       " source_session_id, valid_time_start, valid_time_end, claim_status) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        insert.addBindValue(id);
        insert.addBindValue(asserted);
        insert.addBindValue(about);
        insert.addBindValue(owner);
        insert.addBindValue(claim.counterpartPersonId.isEmpty()
                                ? QVariant(QMetaType(QMetaType::QString))
                                : QVariant(claim.counterpartPersonId));
        insert.addBindValue(statement);
        insert.addBindValue(claim.privacyLevel);
        insert.addBindValue(sourceEventIds);
        insert.addBindValue(sourceChannel);
        insert.addBindValue(sourceSession);
        insert.addBindValue(QVariant(QMetaType(QMetaType::QString)));
        insert.addBindValue(QVariant(QMetaType(QMetaType::QString)));
        insert.addBindValue(claimStatus);
        if (!insert.exec()) {
            qDebug() << "[MemoryManager]写入 memory_claim 失败:" << insert.lastError();
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        qDebug() << "[MemoryManager]提交 summary/claims 事务失败:" << m_db.lastError();
        m_db.rollback();
        return false;
    }
    return true;
}

QList<ClaimDraft> MemoryManager::getClaimsForInteraction(qlonglong interactionId)
{
    QList<ClaimDraft> claims;
    if (!m_db.isOpen() && !m_db.open()) {
        return claims;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT asserted_by_person_id, about_person_id, "
                  "privacy_owner_person_id, counterpart_person_id, statement, "
                  "privacy_level, claim_status FROM memory_claims "
                  "WHERE interaction_id=? ORDER BY id ASC");
    query.addBindValue(interactionId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询 claims 失败:" << query.lastError();
        return claims;
    }
    while (query.next()) {
        ClaimDraft claim;
        claim.assertedByPersonId = query.value(0).toString();
        claim.aboutPersonId = query.value(1).toString();
        claim.privacyOwnerPersonId = query.value(2).toString();
        claim.counterpartPersonId = query.value(3).toString();
        claim.statement = query.value(4).toString();
        claim.privacyLevel = query.value(5).toInt();
        claim.claimStatus = query.value(6).toString();
        claims.append(claim);
    }
    return claims;
}

QList<MemoryClaim> MemoryManager::getActiveClaims(int limit)
{
    QList<MemoryClaim> claims;
    if (!m_db.isOpen() && !m_db.open()) {
        return claims;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id, interaction_id, asserted_by_person_id, "
                  "about_person_id, privacy_owner_person_id, counterpart_person_id, "
                  "statement, privacy_level, source_channel, source_session_id, "
                  "claim_status, valid_time_start, valid_time_end "
                  "FROM memory_claims WHERE claim_status='active' "
                  "ORDER BY id DESC LIMIT ?");
    query.addBindValue(limit < 0 ? -1 : qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询 active claims 失败:" << query.lastError();
        return claims;
    }
    while (query.next()) {
        MemoryClaim claim;
        claim.id = query.value(0).toLongLong();
        claim.interactionId = query.value(1).toLongLong();
        claim.assertedByPersonId = query.value(2).toString();
        claim.aboutPersonId = query.value(3).toString();
        claim.privacyOwnerPersonId = query.value(4).toString();
        claim.counterpartPersonId = query.value(5).toString();
        claim.statement = query.value(6).toString();
        claim.privacyLevel = query.value(7).toInt();
        claim.sourceChannel = query.value(8).toString();
        claim.sourceSessionId = query.value(9).toString();
        claim.claimStatus = query.value(10).toString();
        claim.validTimeStart = query.value(11).toDateTime();
        claim.validTimeEnd = query.value(12).toDateTime();
        claims.append(claim);
    }
    return claims;
}

bool MemoryManager::markInteractionSummaryFailed(qlonglong id)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE interactions SET "
                  "summary_attempts=summary_attempts+1, "
                  "summary_status=CASE WHEN summary_attempts+1 >= 3 "
                  "THEN 'failed' ELSE 'pending' END "
                  "WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]标记 interaction summary 失败失败:" << query.lastError();
        return false;
    }
    return true;
}

QList<InteractionSummary> MemoryManager::getRecentInteractionSummaries(
    const QString &channel, const QString &sessionId, int limit)
{
    QList<InteractionSummary> summaries;
    if (!m_db.isOpen() && !m_db.open()) {
        return summaries;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id, channel, session_id, safe_summary, ended_at, "
                  "summary_format, summary_segments "
                  "FROM interactions "
                  "WHERE summary_status='ready' AND channel=? AND session_id=? "
                  "ORDER BY ended_at DESC LIMIT ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询 interaction summary 失败:" << query.lastError();
        return summaries;
    }
    while (query.next()) {
        InteractionSummary summary;
        summary.id = query.value(0).toLongLong();
        summary.channel = query.value(1).toString();
        summary.sessionId = query.value(2).toString();
        summary.summaryText = query.value(3).toString();
        summary.endedAt = query.value(4).toDateTime();
        summary.summaryFormat = query.value(5).toString();
        summary.summarySegments = query.value(6).toString();
        summaries.append(summary);
    }
    return summaries;
}

QList<InteractionSummary> MemoryManager::getInteractionSummariesInRange(
    const QString &channel,
    const QString &sessionId,
    const QDateTime &from,
    const QDateTime &to,
    int limit)
{
    QList<InteractionSummary> summaries;
    if (!from.isValid() || !to.isValid() || from >= to
        || (!m_db.isOpen() && !m_db.open())) {
        return summaries;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id, channel, session_id, safe_summary, ended_at, "
                  "summary_format, summary_segments "
                  "FROM interactions "
                  "WHERE summary_status='ready' AND channel=? AND session_id=? "
                  "AND ended_at >= ? AND ended_at < ? "
                  "ORDER BY ended_at DESC LIMIT ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(from.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(to.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]按时间查询 interaction summary 失败:"
                 << query.lastError();
        return summaries;
    }
    while (query.next()) {
        InteractionSummary summary;
        summary.id = query.value(0).toLongLong();
        summary.channel = query.value(1).toString();
        summary.sessionId = query.value(2).toString();
        summary.summaryText = query.value(3).toString();
        summary.endedAt = query.value(4).toDateTime();
        summary.summaryFormat = query.value(5).toString();
        summary.summarySegments = query.value(6).toString();
        summaries.append(summary);
    }
    return summaries;
}

QList<InteractionSummary> MemoryManager::getInteractionSummariesOverlapping(
    const QString &channel,
    const QString &sessionId,
    const QDateTime &from,
    const QDateTime &to,
    int limit)
{
    QList<InteractionSummary> summaries;
    if (!from.isValid() || !to.isValid() || from >= to
        || (!m_db.isOpen() && !m_db.open())) {
        return summaries;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id, channel, session_id, safe_summary, ended_at, "
                  "summary_format, summary_segments "
                  "FROM interactions "
                  "WHERE summary_status='ready' AND channel=? AND session_id=? "
                  "AND started_at < ? AND ended_at >= ? "
                  "ORDER BY ended_at ASC LIMIT ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(to.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(from.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]按重叠时间查询 interaction summary 失败:"
                 << query.lastError();
        return summaries;
    }
    while (query.next()) {
        InteractionSummary summary;
        summary.id = query.value(0).toLongLong();
        summary.channel = query.value(1).toString();
        summary.sessionId = query.value(2).toString();
        summary.summaryText = query.value(3).toString();
        summary.endedAt = query.value(4).toDateTime();
        summary.summaryFormat = query.value(5).toString();
        summary.summarySegments = query.value(6).toString();
        summaries.append(summary);
    }
    return summaries;
}

int MemoryManager::countInteractionsInRange(const QString &channel,
                                            const QString &sessionId,
                                            const QDateTime &from,
                                            const QDateTime &to)
{
    if (!from.isValid() || !to.isValid() || from >= to
        || (!m_db.isOpen() && !m_db.open())) {
        return 0;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM interactions "
                  "WHERE channel=? AND session_id=? "
                  "AND started_at < ? AND ended_at >= ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(to.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(from.toString("yyyy-MM-dd HH:mm:ss"));
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QString MemoryManager::getGroupDisplayName(const QString &groupId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return {};
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT payload FROM raw_events "
                  "WHERE session_id=? AND payload IS NOT NULL "
                  "ORDER BY id DESC LIMIT 20");
    query.addBindValue(groupId);
    if (!query.exec()) {
        return {};
    }
    while (query.next()) {
        const QJsonObject payload = QJsonDocument::fromJson(
            query.value(0).toString().toUtf8()).object();
        const QString name = payload.value(QStringLiteral("group_name"))
                                 .toString().trimmed();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return {};
}

QList<qlonglong> MemoryManager::getFailedInteractionsWithUnsafeParticipantNames(
    int limit)
{
    QList<qlonglong> result;
    if (!m_db.isOpen() && !m_db.open()) {
        return result;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT i.id, p.display_name_at_time "
                  "FROM interactions i "
                  "JOIN interaction_participants p "
                  "ON p.interaction_id=i.id "
                  "WHERE i.summary_status='failed' "
                  "ORDER BY i.id DESC LIMIT ?");
    query.addBindValue(qMax(1, limit) * 8);
    if (!query.exec()) {
        return result;
    }
    QSet<qlonglong> seen;
    while (query.next() && result.size() < qMax(1, limit)) {
        const qlonglong id = query.value(0).toLongLong();
        if (seen.contains(id)) {
            continue;
        }
        const QString rawName = query.value(1).toString();
        if (rawName.isEmpty()) {
            continue;
        }
        const QString safeName = MemoryTextUtils::sanitizeIdentityText(
            rawName, QStringLiteral("某位成员"));
        if (safeName != rawName) {
            seen.insert(id);
            result.append(id);
        }
    }
    return result;
}

bool MemoryManager::resetInteractionSummaryForRetry(qlonglong interactionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE interactions SET summary_status='pending', "
                  "summary_attempts=0 WHERE id=? AND summary_status='failed'");
    query.addBindValue(interactionId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]重置 interaction summary 失败:"
                 << query.lastError();
        return false;
    }
    return query.numRowsAffected() > 0;
}

QStringList MemoryManager::getRecentGroupIds(const QStringList &allowedGroupIds,
                                             int maxGroups,
                                             const QDateTime &since, const QDateTime &until)
{
    QStringList groups;
    if (allowedGroupIds.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return groups;
    }

    QStringList placeholders;
    for (int i = 0; i < allowedGroupIds.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }

    QString sql = QStringLiteral(
        "SELECT session_id, MAX(occurred_at) AS latest FROM raw_events "
        "WHERE channel='qq_group' AND ") + MemoryReadPolicy::rawMessagePredicate()
        + QStringLiteral(" AND session_id IN (%1)").arg(placeholders.join(','));
    if (since.isValid()) {
        sql += QStringLiteral(" AND occurred_at >= ?");
    }
    if (until.isValid())
        sql += QStringLiteral(" AND occurred_at < ?");
    sql += QStringLiteral(" GROUP BY session_id ORDER BY latest DESC,session_id LIMIT ?");

    QSqlQuery query(m_db);
    query.prepare(sql);
    for (const QString &groupId : allowedGroupIds) {
        query.addBindValue(groupId);
    }
    if (since.isValid()) {
        query.addBindValue(since.toString("yyyy-MM-dd HH:mm:ss"));
    }
    if (until.isValid())
        query.addBindValue(until.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(qMax(1, maxGroups));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询近期群组失败:" << query.lastError();
        return groups;
    }
    while (query.next()) {
        groups.append(query.value(0).toString());
    }
    return groups;
}

RecentContextBundle MemoryManager::searchRecentGroupContext(
    const RecentContextQuery &query)
{
    RecentContextBundle bundle;
    bundle.mode = query.mode;
    if (!m_db.isOpen() && !m_db.open()) {
        return bundle;
    }
    if (query.allowedGroupIds.isEmpty()) {
        return bundle;
    }

    const QStringList groups = getRecentGroupIds(
        query.allowedGroupIds, query.allowedGroupIds.size(), query.since, query.until);
    const MemoryReadPolicy policy;
    int includedGroups = 0;
    QList<RecentContextItem> candidates;
    for (const QString &groupId : groups) {
        if (!policy.allowsSession(Channel::kQQGroup, groupId))
            continue;
        const auto before = candidates.size();
        QString sql = QStringLiteral(
            "SELECT id, session_id, message_id, sender_id, sender_name, "
            "person_id, content, payload, occurred_at "
            "FROM raw_events "
            "WHERE channel='qq_group' AND session_id=? "
            "AND ") + MemoryReadPolicy::rawMessagePredicate();
        if (query.since.isValid()) {
            sql += QStringLiteral(" AND occurred_at >= ?");
        }
        if (query.until.isValid()) {
            sql += QStringLiteral(" AND occurred_at < ?");
        }
        sql += QStringLiteral(" ORDER BY occurred_at DESC, id DESC");

        QSqlQuery eventQuery(m_db);
        eventQuery.prepare(sql);
        eventQuery.addBindValue(groupId);
        if (query.since.isValid()) {
            eventQuery.addBindValue(query.since.toString("yyyy-MM-dd HH:mm:ss"));
        }
        if (query.until.isValid()) {
            eventQuery.addBindValue(query.until.toString("yyyy-MM-dd HH:mm:ss"));
        }
        if (!eventQuery.exec()) {
            qDebug() << "[MemoryManager]查询近期群消息失败:" << eventQuery.lastError();
            continue;
        }
        while (eventQuery.next()) {
            if (!policy.allows(Channel::kQQGroup, groupId, eventQuery.value(6).toString()))
                continue;
            RecentContextItem item;
            item.rawEventId = eventQuery.value(0).toLongLong();
            item.sourceGroupId = eventQuery.value(1).toString();
            item.messageId = eventQuery.value(2).toString();
            item.senderId = eventQuery.value(3).toString();
            item.senderDisplayName = eventQuery.value(4).toString();
            item.personId = eventQuery.value(5).toString();
            if (item.personId.isEmpty()) {
                item.personId = query.viewerIds.contains(item.senderId)
                                    ? QStringLiteral("owner")
                                    : QString();
            }
            item.isViewer = !query.viewerPersonId.isEmpty()
                                && item.personId == query.viewerPersonId;
            if (query.viewerPersonId.isEmpty())
                item.isViewer = query.viewerIds.contains(item.senderId);
            if (query.onlyViewer && !item.isViewer)
                continue;
            if (item.senderDisplayName.isEmpty()
                || QRegularExpression(QStringLiteral("^\\d{5,}$"))
                       .match(item.senderDisplayName).hasMatch()) {
                item.senderDisplayName = QStringLiteral("群友");
            }
            item.sanitizedText = eventQuery.value(6).toString();
            item.sanitizedText.replace(
                QRegularExpression(QStringLiteral("@\\d{5,}")),
                QStringLiteral("@某位成员"));
            item.sanitizedText.replace(
                QRegularExpression(QStringLiteral("\\bqq:\\d{5,}\\b"),
                                    QRegularExpression::CaseInsensitiveOption),
                QStringLiteral("某位成员"));
            const QString payload = eventQuery.value(7).toString();
            if (!payload.isEmpty()) {
                const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
                if (doc.isObject()) {
                    item.replyToMessageId =
                        QQMessageParser::parse(doc.object()).replyToMessageId;
                }
            }
            item.occurredAt = eventQuery.value(8).toDateTime();
            candidates.append(item);
            if (candidates.size() - before >= qMax(1, query.perGroupCandidateLimit))
                break;
        }
        if (candidates.size() > before && ++includedGroups >= qMax(1, query.maxGroups))
            break;
    }

    bundle = RecentContextPlanner::build(candidates, query);
    return bundle;
}

bool MemoryManager::ensurePerson(const QString &personId,
                                 const QString &role,
                                 const QString &canonicalName)
{
    if (personId.isEmpty()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("INSERT OR IGNORE INTO persons "
                  "(person_id, role, canonical_name) VALUES (?, ?, ?)");
    query.addBindValue(personId);
    query.addBindValue(role);
    query.addBindValue(canonicalName.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(canonicalName));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]创建 person 失败:" << query.lastError();
        return false;
    }
    if (!canonicalName.isEmpty()) {
        QSqlQuery update(m_db);
        update.prepare("UPDATE persons SET canonical_name=?, updated_at=datetime('now','localtime') "
                       "WHERE person_id=? AND (canonical_name IS NULL OR canonical_name='')");
        update.addBindValue(canonicalName);
        update.addBindValue(personId);
        update.exec();
    }
    return true;
}

QString MemoryManager::resolvePersonId(const QString &platform,
                                       const QString &platformUserId,
                                       const QString &groupId)
{
    Q_UNUSED(groupId);
    if (platformUserId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return {};
    }
    if (platform == QStringLiteral("qq") && m_masterUserIds.contains(platformUserId)) {
        ensurePerson(QStringLiteral("owner"), QStringLiteral("owner"),
                     QStringLiteral("欧尼酱"));
        return QStringLiteral("owner");
    }
    const QString personId = QStringLiteral("%1:%2").arg(platform, platformUserId);
    ensurePerson(personId, QStringLiteral("member"), QString());
    return personId;
}

bool MemoryManager::ensurePersonIdentity(const QString &platform,
                                         const QString &platformUserId,
                                         const QString &groupId,
                                         const QString &displayName,
                                         const QDateTime &occurredAt,
                                         const QString &source)
{
    if (platformUserId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return false;
    }
    const QString personId = resolvePersonId(platform, platformUserId, groupId);
    if (personId.isEmpty()) {
        return false;
    }
    const QString when = (occurredAt.isValid() ? occurredAt
                                                : QDateTime::currentDateTime())
                             .toString("yyyy-MM-dd HH:mm:ss");
    const QString group = groupId.isEmpty() ? QStringLiteral("") : groupId;

    QSqlQuery active(m_db);
    active.prepare("SELECT id, display_name FROM person_identities "
                   "WHERE person_id=? AND platform=? AND platform_user_id=? "
                   "AND group_id=? AND valid_to IS NULL "
                   "ORDER BY id DESC LIMIT 1");
    active.addBindValue(personId);
    active.addBindValue(platform);
    active.addBindValue(platformUserId);
    active.addBindValue(group);
    if (!active.exec()) {
        qDebug() << "[MemoryManager]查询 person identity 失败:" << active.lastError();
        return false;
    }

    if (active.next()) {
        const qlonglong id = active.value(0).toLongLong();
        const QString currentName = active.value(1).toString();
        if (currentName == displayName || displayName.isEmpty()) {
            QSqlQuery touch(m_db);
            touch.prepare("UPDATE person_identities SET last_verified_at=? WHERE id=?");
            touch.addBindValue(when);
            touch.addBindValue(id);
            return touch.exec();
        }
        QSqlQuery close(m_db);
        close.prepare("UPDATE person_identities SET valid_to=? WHERE id=?");
        close.addBindValue(when);
        close.addBindValue(id);
        if (!close.exec()) {
            qDebug() << "[MemoryManager]关闭旧 person identity 失败:" << close.lastError();
            return false;
        }
    }

    QSqlQuery insert(m_db);
    insert.prepare("INSERT INTO person_identities "
                   "(person_id, platform, platform_user_id, group_id, display_name, "
                   " valid_from, source, last_verified_at) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    insert.addBindValue(personId);
    insert.addBindValue(platform);
    insert.addBindValue(platformUserId);
    insert.addBindValue(group);
    insert.addBindValue(displayName);
    insert.addBindValue(when);
    insert.addBindValue(source);
    insert.addBindValue(when);
    if (!insert.exec()) {
        qDebug() << "[MemoryManager]写入 person identity 失败:" << insert.lastError();
        return false;
    }
    return true;
}

QStringList MemoryManager::getPersonAliases(const QString &personId,
                                            const QString &groupId,
                                            int limit)
{
    QStringList aliases;
    if (personId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return aliases;
    }
    QSqlQuery query(m_db);
    if (groupId.isEmpty()) {
        query.prepare("SELECT display_name, MAX(id) AS latest FROM person_identities "
                      "WHERE person_id=? AND display_name IS NOT NULL AND display_name<>'' "
                      "GROUP BY display_name ORDER BY latest DESC LIMIT ?");
        query.addBindValue(personId);
    } else {
        query.prepare("SELECT display_name, MAX(id) AS latest FROM person_identities "
                      "WHERE person_id=? AND group_id=? "
                      "AND display_name IS NOT NULL AND display_name<>'' "
                      "GROUP BY display_name ORDER BY latest DESC LIMIT ?");
        query.addBindValue(personId);
        query.addBindValue(groupId);
    }
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询 person aliases 失败:" << query.lastError();
        return aliases;
    }
    while (query.next()) {
        const QString name = query.value(0).toString();
        if (!name.isEmpty()
            && !QRegularExpression(QStringLiteral("^\\d{5,}$")).match(name).hasMatch()) {
            aliases.append(name);
        }
    }
    return aliases;
}

bool MemoryManager::replaceGroupMembershipSnapshot(
    const QString &groupId,
    const QList<QPair<QString, QString>> &members,
    const QDateTime &verifiedAt)
{
    if (groupId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return false;
    }
    const QString when = (verifiedAt.isValid() ? verifiedAt
                                                : QDateTime::currentDateTime())
                             .toString("yyyy-MM-dd HH:mm:ss");
    m_db.transaction();

    QStringList memberIds;
    for (const auto &member : members) {
        const QString userId = member.first;
        if (userId.isEmpty()) {
            continue;
        }
        memberIds << userId;
        const QString personId = resolvePersonId(
            QStringLiteral("qq"), userId, groupId);
        if (!ensurePersonIdentity(QStringLiteral("qq"), userId, groupId,
                                  member.second, verifiedAt)) {
            m_db.rollback();
            return false;
        }
        QSqlQuery open(m_db);
        open.prepare("SELECT id FROM group_membership_periods "
                     "WHERE group_id=? AND user_id=? AND valid_to IS NULL "
                     "ORDER BY id DESC LIMIT 1");
        open.addBindValue(groupId);
        open.addBindValue(userId);
        if (!open.exec()) {
            m_db.rollback();
            return false;
        }
        if (open.next()) {
            QSqlQuery touch(m_db);
            touch.prepare("UPDATE group_membership_periods "
                          "SET person_id=?, last_verified_at=? WHERE id=?");
            touch.addBindValue(personId);
            touch.addBindValue(when);
            touch.addBindValue(open.value(0).toLongLong());
            if (!touch.exec()) {
                m_db.rollback();
                return false;
            }
        } else {
            QSqlQuery insert(m_db);
            insert.prepare("INSERT INTO group_membership_periods "
                           "(group_id, user_id, person_id, valid_from, "
                           " source, last_verified_at) "
                           "VALUES (?, ?, ?, ?, 'snapshot', ?)");
            insert.addBindValue(groupId);
            insert.addBindValue(userId);
            insert.addBindValue(personId);
            insert.addBindValue(when);
            insert.addBindValue(when);
            if (!insert.exec()) {
                qDebug() << "[MemoryManager]写入 membership period 失败:"
                         << insert.lastError();
                m_db.rollback();
                return false;
            }
        }
    }

    if (memberIds.isEmpty()) {
        QSqlQuery closeAll(m_db);
        closeAll.prepare("UPDATE group_membership_periods SET valid_to=? "
                         "WHERE group_id=? AND valid_to IS NULL");
        closeAll.addBindValue(when);
        closeAll.addBindValue(groupId);
        if (!closeAll.exec()) {
            m_db.rollback();
            return false;
        }
    } else {
        QStringList placeholders;
        for (int i = 0; i < memberIds.size(); ++i) {
            placeholders.append(QStringLiteral("?"));
        }
        QSqlQuery closeMissing(m_db);
        closeMissing.prepare(QStringLiteral(
            "UPDATE group_membership_periods SET valid_to=? "
            "WHERE group_id=? AND valid_to IS NULL "
            "AND user_id NOT IN (%1)").arg(placeholders.join(',')));
        closeMissing.addBindValue(when);
        closeMissing.addBindValue(groupId);
        for (const QString &userId : memberIds) {
            closeMissing.addBindValue(userId);
        }
        if (!closeMissing.exec()) {
            m_db.rollback();
            return false;
        }
    }

    QSqlQuery snapshot(m_db);
    snapshot.prepare("INSERT OR REPLACE INTO group_membership_snapshots "
                     "(group_id, last_verified_at, status, last_error) "
                     "VALUES (?, ?, 'ok', NULL)");
    snapshot.addBindValue(groupId);
    snapshot.addBindValue(when);
    if (!snapshot.exec()) {
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return false;
    }
    return true;
}

bool MemoryManager::markGroupMembershipSnapshotFailed(
    const QString &groupId,
    const QString &error,
    const QDateTime &verifiedAt)
{
    if (groupId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return false;
    }
    const QString when = (verifiedAt.isValid() ? verifiedAt
                                                : QDateTime::currentDateTime())
                             .toString("yyyy-MM-dd HH:mm:ss");
    QSqlQuery snapshot(m_db);
    snapshot.prepare("INSERT OR REPLACE INTO group_membership_snapshots "
                     "(group_id, last_verified_at, status, last_error) "
                     "VALUES (?, ?, 'failed', ?)");
    snapshot.addBindValue(groupId);
    snapshot.addBindValue(when);
    snapshot.addBindValue(error);
    if (!snapshot.exec()) {
        qDebug() << "[MemoryManager]标记 membership 快照失败失败:"
                 << snapshot.lastError();
        return false;
    }
    return true;
}

bool MemoryManager::hasValidGroupMembershipSnapshot(
    const QString &groupId, int maxAgeMinutes, const QDateTime &now)
{
    if (groupId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT last_verified_at FROM group_membership_snapshots "
                  "WHERE group_id=? AND status='ok'");
    query.addBindValue(groupId);
    if (!query.exec() || !query.next()) {
        return false;
    }
    const QDateTime verified = query.value(0).toDateTime();
    if (!verified.isValid()) {
        return false;
    }
    const QDateTime current = now.isValid() ? now : QDateTime::currentDateTime();
    return verified.secsTo(current) <= qMax(0, maxAgeMinutes) * 60;
}

QStringList MemoryManager::getOpenGroupMemberIds(const QString &groupId)
{
    QStringList ids;
    if (groupId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return ids;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT user_id FROM group_membership_periods "
                  "WHERE group_id=? AND valid_to IS NULL ORDER BY user_id");
    query.addBindValue(groupId);
    if (!query.exec()) {
        return ids;
    }
    while (query.next()) {
        ids.append(query.value(0).toString());
    }
    return ids;
}

QStringList MemoryManager::getOpenGroupsForUser(const QString &userId, int limit)
{
    QStringList groups;
    if (userId.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return groups;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT DISTINCT group_id FROM group_membership_periods "
                  "WHERE user_id=? AND valid_to IS NULL "
                  "ORDER BY group_id DESC LIMIT ?");
    query.addBindValue(userId);
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        return groups;
    }
    while (query.next()) {
        groups.append(query.value(0).toString());
    }
    return groups;
}

bool MemoryManager::openGroupMembershipPeriod(const QString &groupId,
                                              const QString &userId,
                                              const QString &displayName,
                                              const QString &source,
                                              const QDateTime &at)
{
    if (groupId.isEmpty() || userId.isEmpty()
        || (!m_db.isOpen() && !m_db.open())) {
        return false;
    }
    const QString when = (at.isValid() ? at : QDateTime::currentDateTime())
                             .toString("yyyy-MM-dd HH:mm:ss");
    const QString personId = resolvePersonId(
        QStringLiteral("qq"), userId, groupId);
    if (!ensurePersonIdentity(QStringLiteral("qq"), userId, groupId,
                              displayName, at, source.isEmpty()
                                                    ? QStringLiteral("group_increase")
                                                    : source)) {
        return false;
    }
    QSqlQuery open(m_db);
    open.prepare("SELECT id FROM group_membership_periods "
                 "WHERE group_id=? AND user_id=? AND valid_to IS NULL "
                 "ORDER BY id DESC LIMIT 1");
    open.addBindValue(groupId);
    open.addBindValue(userId);
    if (!open.exec()) {
        return false;
    }
    if (open.next()) {
        QSqlQuery update(m_db);
        update.prepare("UPDATE group_membership_periods "
                       "SET person_id=?, last_verified_at=? WHERE id=?");
        update.addBindValue(personId);
        update.addBindValue(when);
        update.addBindValue(open.value(0).toLongLong());
        return update.exec();
    }
    QSqlQuery insert(m_db);
    insert.prepare("INSERT INTO group_membership_periods "
                   "(group_id, user_id, person_id, valid_from, source, "
                   " last_verified_at) VALUES (?, ?, ?, ?, ?, ?)");
    insert.addBindValue(groupId);
    insert.addBindValue(userId);
    insert.addBindValue(personId);
    insert.addBindValue(when);
    insert.addBindValue(source.isEmpty() ? QStringLiteral("group_increase")
                                          : source);
    insert.addBindValue(when);
    if (!insert.exec()) {
        qDebug() << "[MemoryManager]打开 membership period 失败:" << insert.lastError();
        return false;
    }
    return true;
}

bool MemoryManager::closeGroupMembershipPeriod(const QString &groupId,
                                               const QString &userId,
                                               const QDateTime &at)
{
    if (groupId.isEmpty() || userId.isEmpty()
        || (!m_db.isOpen() && !m_db.open())) {
        return false;
    }
    const QString when = (at.isValid() ? at : QDateTime::currentDateTime())
                             .toString("yyyy-MM-dd HH:mm:ss");
    QSqlQuery query(m_db);
    query.prepare("UPDATE group_membership_periods SET valid_to=? "
                  "WHERE group_id=? AND user_id=? AND valid_to IS NULL");
    query.addBindValue(when);
    query.addBindValue(groupId);
    query.addBindValue(userId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]关闭 membership period 失败:" << query.lastError();
        return false;
    }
    return true;
}

void MemoryManager::updateMasterAccounts(const QStringList &masterUserIds)
{
    m_masterUserIds.clear();
    for (const QString &id : masterUserIds) {
        if (!id.isEmpty()) {
            m_masterUserIds.insert(id);
        }
    }
}

void MemoryManager::seedIdentity(const QStringList &masterUserIds)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return;
    }
    updateMasterAccounts(masterUserIds);
    ensurePerson(QStringLiteral("owner"), QStringLiteral("owner"),
                 QStringLiteral("欧尼酱"));

    m_db.transaction();
    auto fail = [this](const QSqlError &error, const QString &what) {
        qDebug() << "[MemoryManager]seedIdentity 失败:" << what << error;
        m_db.rollback();
    };

    if (!m_masterUserIds.isEmpty()) {
        QStringList placeholders;
        for (int i = 0; i < m_masterUserIds.size(); ++i) {
            placeholders.append(QStringLiteral("?"));
        }
        const QString inClause = placeholders.join(',');
        QSqlQuery ownerRaw(m_db);
        ownerRaw.prepare(QStringLiteral(
            "UPDATE raw_events SET person_id='owner' "
            "WHERE platform='qq' AND sender_id IN (%1)").arg(inClause));
        for (const QString &id : m_masterUserIds) {
            ownerRaw.addBindValue(id);
        }
        if (!ownerRaw.exec()) {
            fail(ownerRaw.lastError(), "owner raw_events");
            return;
        }
        QSqlQuery ownerParticipants(m_db);
        ownerParticipants.prepare(QStringLiteral(
            "UPDATE interaction_participants SET person_id='owner' "
            "WHERE user_id IN (%1)").arg(inClause));
        for (const QString &id : m_masterUserIds) {
            ownerParticipants.addBindValue(id);
        }
        if (!ownerParticipants.exec()) {
            fail(ownerParticipants.lastError(), "owner participants");
            return;
        }
    }

    QSqlQuery rawRows(m_db);
    if (!rawRows.exec("SELECT DISTINCT platform, sender_id, sender_name, session_id, "
                      "channel, occurred_at FROM raw_events "
                      "WHERE sender_id IS NOT NULL AND sender_id<>'' "
                      "ORDER BY occurred_at ASC")) {
        fail(rawRows.lastError(), "raw rows");
        return;
    }
    while (rawRows.next()) {
        const QString platform = rawRows.value(0).toString();
        const QString senderId = rawRows.value(1).toString();
        const QString senderName = rawRows.value(2).toString();
        const QString sessionId = rawRows.value(3).toString();
        const QString channel = rawRows.value(4).toString();
        const QString groupId = (channel == Channel::kQQGroup) ? sessionId : QString();
        if (!ensurePersonIdentity(platform, senderId, groupId, senderName,
                                  rawRows.value(5).toDateTime())) {
            fail(QSqlError(), "identity raw");
            return;
        }
        const QString personId = resolvePersonId(platform, senderId, groupId);
        QSqlQuery updateRaw(m_db);
        updateRaw.prepare("UPDATE raw_events SET person_id=? "
                          "WHERE platform=? AND sender_id=? AND person_id IS NULL");
        updateRaw.addBindValue(personId);
        updateRaw.addBindValue(platform);
        updateRaw.addBindValue(senderId);
        updateRaw.exec();
    }

    QSqlQuery participantRows(m_db);
    if (!participantRows.exec(
            "SELECT p.user_id, p.display_name_at_time, i.session_id, i.channel, i.ended_at "
            "FROM interaction_participants p "
            "JOIN interactions i ON i.id=p.interaction_id "
            "ORDER BY i.ended_at ASC, i.id ASC")) {
        fail(participantRows.lastError(), "participant rows");
        return;
    }
    while (participantRows.next()) {
        const QString userId = participantRows.value(0).toString();
        const QString displayName = participantRows.value(1).toString();
        const QString sessionId = participantRows.value(2).toString();
        const QString channel = participantRows.value(3).toString();
        const QDateTime endedAt = participantRows.value(4).toDateTime();
        const QString groupId = (channel == Channel::kQQGroup) ? sessionId : QString();
        if (!ensurePersonIdentity(QStringLiteral("qq"), userId, groupId,
                                  displayName, endedAt)) {
            fail(QSqlError(), "identity participant");
            return;
        }
        QSqlQuery updateParticipant(m_db);
        updateParticipant.prepare("UPDATE interaction_participants SET person_id=? "
                                  "WHERE user_id=? AND person_id IS NULL");
        updateParticipant.addBindValue(resolvePersonId(QStringLiteral("qq"), userId));
        updateParticipant.addBindValue(userId);
        updateParticipant.exec();
    }

    if (!m_db.commit()) {
        qDebug() << "[MemoryManager]seedIdentity 提交失败:" << m_db.lastError();
        m_db.rollback();
    }
}

QStringList MemoryManager::getKnownDisplayNamesForUserIds(
    const QStringList &userIds, int limit)
{
    QStringList names;
    if (userIds.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return names;
    }
    QStringList placeholders;
    for (int i = 0; i < userIds.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT display_name_at_time, MAX(interaction_id) AS latest "
        "FROM interaction_participants "
        "WHERE user_id IN (%1) "
        "AND display_name_at_time IS NOT NULL "
        "AND display_name_at_time <> '' "
        "GROUP BY display_name_at_time "
        "ORDER BY latest DESC LIMIT ?").arg(placeholders.join(',')));
    for (const QString &userId : userIds) {
        query.addBindValue(userId);
    }
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询已知昵称失败:" << query.lastError();
        return names;
    }
    while (query.next()) {
        const QString name = query.value(0).toString();
        if (!name.isEmpty()
            && !QRegularExpression(QStringLiteral("^\\d{5,}$")).match(name).hasMatch()) {
            names.append(name);
        }
    }
    return names;
}

bool MemoryManager::updateMemoryCursor(const QString &channel, const QString &sessionId,
                                       qlonglong lastEventId, qlonglong lastTurnId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO memory_cursor "
                  "(channel, session_id, last_event_id, last_turn_id, updated_at) "
                  "VALUES (?, ?, ?, ?, datetime('now', 'localtime'))");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(lastEventId);
    query.addBindValue(lastTurnId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]更新memory_cursor失败:" << query.lastError();
        return false;
    }
    return true;
}

qlonglong MemoryManager::getMemoryCursorEventId(const QString &channel, const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return 0;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COALESCE(last_event_id, 0) FROM memory_cursor "
                  "WHERE channel = ? AND session_id = ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    if (query.exec() && query.next()) {
        return query.value(0).toLongLong();
    }
    return 0;
}

void MemoryManager::initDatabase(const QString &dbFilePath, const QString &connectionName)
{
    QDir dbDir = QFileInfo(dbFilePath).dir();
    if (!dbDir.exists()) {
        if (!dbDir.mkpath(".")) {
            qCritical() << "[MemoryManager] 无法创建数据库目录:" << dbDir.absolutePath();
            return;
        }
    }



    const QString connName = connectionName.isEmpty()
        ? QStringLiteral("memory_db_connection") : connectionName;
    if (QSqlDatabase::contains(connName)) {
        // 复用连接时不自动 open（旧文件可能已不存在），改由下方 setDatabaseName 后统一打开
        m_db = QSqlDatabase::database(connName, false);
        if (m_db.isOpen()) {
            m_db.close();
        }
    }
    else
    {
        m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
    }
    m_db.setDatabaseName(dbFilePath);


    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return;
    }

    qDebug()<<"[MemoryManager]数据库连接成功:"<<m_db;

    QSqlQuery query(m_db);
    int currentVersion = 0;
    if (query.exec("PRAGMA user_version")) {
        if (query.next()) {
            currentVersion = query.value(0).toInt();
        }
    }
    qDebug()<<"[MemoryManager]当前数据库版本:"<<currentVersion;
    if (!m_db.transaction()) return;

    try {
        if (currentVersion<1) {
            qDebug()<<"[MemoryManager]正在初始化数据库表结构:";

            // 1. 对话历史表
            QString createChatHistorySql = R"(
                CREATE TABLE IF NOT EXISTS chat_history (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp DATETIME DEFAULT (datetime('now', 'localtime')),
                    user_input TEXT NOT NULL,
                    raw_reply TEXT NOT NULL
                );
            )";
            if (!query.exec(createChatHistorySql)) {
                qDebug()<<"[MemoryManager]创建chat_history异常:"<<query.lastError();
            }

            // 2. 长期摘要表
            QString createSummarySql = R"(
                CREATE TABLE IF NOT EXISTS long_term_summary (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    summary_text TEXT NOT NULL,
                    covered_turn_end_id INTEGER NOT NULL,
                    source_ids TEXT NOT NULL,
                    is_dirty INTEGER DEFAULT 0,
                    created_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime'))
                );
            )";
            if (!query.exec(createSummarySql)) {
                qDebug()<<"[MemoryManager]创建long_term_summary异常:"<<query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_summary_covered_end ON long_term_summary(covered_turn_end_id);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_summary_is_dirty ON long_term_summary(is_dirty);");

            // 3. 角色档案表（取代已废弃的 user_profile）
            QString createCharProfileSql = R"(
                CREATE TABLE IF NOT EXISTS character_profile (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    subject TEXT NOT NULL,
                    key TEXT NOT NULL,
                    value TEXT NOT NULL,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    UNIQUE(subject, key)
                );
            )";
            if (!query.exec(createCharProfileSql)) {
                qDebug()<<"[MemoryManager]创建character_profile异常:"<<query.lastError();
            }

            // 4. 情景记忆表
            QString createEpisodicSql = R"(
                CREATE TABLE IF NOT EXISTS episodic_memory (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    content TEXT NOT NULL,
                    event_time DATETIME DEFAULT (datetime('now', 'localtime')),
                    importance REAL DEFAULT 0.5,
                    type TEXT DEFAULT 'event',
                    status TEXT DEFAULT 'active',
                    last_accessed DATETIME DEFAULT (datetime('now', 'localtime')),
                    last_decay_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    source_ids TEXT
                );
            )";
            if (!query.exec(createEpisodicSql)) {
                qDebug()<<"[MemoryManager]创建episodic_memory异常:"<<query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_importance ON episodic_memory(importance);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_type ON episodic_memory(type);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_status ON episodic_memory(status);");

            // 5. 关系状态表（亲密度/信任度等关系维度）
            QString createRelStateSql = R"(
                CREATE TABLE IF NOT EXISTS relationship_state (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    dimension TEXT NOT NULL UNIQUE,
                    value REAL NOT NULL,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime'))
                );
            )";
            if (!query.exec(createRelStateSql)) {
                qDebug()<<"[MemoryManager]创建relationship_state异常:"<<query.lastError();
            }
            // 首次创建时初始化默认关系维度
            query.prepare("SELECT COUNT(*) FROM relationship_state");
            query.exec();
            int relCount = 0;
            if (query.next()) relCount = query.value(0).toInt();
            if (relCount == 0) {
                query.prepare("INSERT OR IGNORE INTO relationship_state (dimension, value) VALUES ('intimacy', 30.0)");
                query.exec();
                query.prepare("INSERT OR IGNORE INTO relationship_state (dimension, value) VALUES ('trust', 30.0)");
                query.exec();
                qDebug()<<"[MemoryManager]关系状态已初始化: intimacy=30, trust=30";
            }

            if (!query.exec("PRAGMA user_version = 1")) {
                qDebug()<<"[MemoryManager]设置数据库版本异常:"<<query.lastError();
            }
            currentVersion = 1;
        }

        // ============== v2: 多渠道记忆扩展（Phase 0，见 docs/HUMANIZATION_PLAN.md 8.5） ==============
        if (currentVersion < 2) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v2(多渠道记忆扩展):";

            // 1. chat_history 渠道分桶列
            ensureColumn(query, "chat_history", "channel",
                         "ALTER TABLE chat_history ADD COLUMN channel TEXT NOT NULL DEFAULT 'pet'");
            ensureColumn(query, "chat_history", "session_id",
                         "ALTER TABLE chat_history ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'");
            ensureColumn(query, "chat_history", "speaker",
                         "ALTER TABLE chat_history ADD COLUMN speaker TEXT");
            ensureColumn(query, "chat_history", "sensitive",
                         "ALTER TABLE chat_history ADD COLUMN sensitive INTEGER NOT NULL DEFAULT 0");
            query.exec("CREATE INDEX IF NOT EXISTS idx_history_session ON chat_history(channel, session_id);");

            // 2. long_term_summary 渠道分桶列（摘要与未摘要游标按双键，审查修订 A1）
            ensureColumn(query, "long_term_summary", "channel",
                         "ALTER TABLE long_term_summary ADD COLUMN channel TEXT NOT NULL DEFAULT 'pet'");
            ensureColumn(query, "long_term_summary", "session_id",
                         "ALTER TABLE long_term_summary ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'");

            // 3. episodic_memory 渠道溯源列
            ensureColumn(query, "episodic_memory", "origin",
                         "ALTER TABLE episodic_memory ADD COLUMN origin TEXT NOT NULL DEFAULT 'pet'");

            // 4. relationship_state 重建：旧结构 dimension 单列 UNIQUE → 新结构 UNIQUE(subject, dimension)
            //    存量行复制为 subject='user'（'user' 即主人，E2 决策：永久保留不迁移）
            QString createRelV2Sql = R"(
                CREATE TABLE IF NOT EXISTS relationship_state_v2 (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    subject TEXT NOT NULL DEFAULT 'user',
                    dimension TEXT NOT NULL,
                    value REAL NOT NULL,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    UNIQUE(subject, dimension)
                );
            )";
            if (!query.exec(createRelV2Sql)) {
                qDebug() << "[MemoryManager]创建relationship_state_v2异常:" << query.lastError();
            }
            if (!query.exec("INSERT OR IGNORE INTO relationship_state_v2 (subject, dimension, value) "
                            "SELECT 'user', dimension, value FROM relationship_state")) {
                qDebug() << "[MemoryManager]关系状态数据迁移异常:" << query.lastError();
            }
            if (!query.exec("DROP TABLE relationship_state")) {
                qDebug() << "[MemoryManager]删除旧relationship_state表异常:" << query.lastError();
            }
            if (!query.exec("ALTER TABLE relationship_state_v2 RENAME TO relationship_state")) {
                qDebug() << "[MemoryManager]relationship_state重命名异常:" << query.lastError();
            }

            // 5. raw_messages 群聊原始消息表（Phase 0 只建不写，Phase 1 接数据源）
            QString createRawSql = R"(
                CREATE TABLE IF NOT EXISTS raw_messages (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    speaker TEXT,
                    content TEXT NOT NULL,
                    timestamp DATETIME DEFAULT (datetime('now', 'localtime')),
                    sensitive INTEGER NOT NULL DEFAULT 0,
                    consolidated INTEGER NOT NULL DEFAULT 0
                );
            )";
            if (!query.exec(createRawSql)) {
                qDebug() << "[MemoryManager]创建raw_messages异常:" << query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_session ON raw_messages(session_id, timestamp);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_consolidated ON raw_messages(consolidated);");

            if (!query.exec("PRAGMA user_version = 2")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 2;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v3: 记忆库调整（HUMANIZATION_PLAN 10.4） ==============
        if (currentVersion < 3) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v3(记忆库调整):";

            // 1. chat_history 重建：user_input 放开 NOT NULL + 轮次字段
            query.exec("DROP TABLE IF EXISTS chat_history_v3");
            QString createChatV3 = R"(
                CREATE TABLE chat_history_v3 (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp DATETIME DEFAULT (datetime('now', 'localtime')),
                    user_input TEXT,
                    raw_reply TEXT NOT NULL,
                    channel TEXT NOT NULL DEFAULT 'pet',
                    session_id TEXT NOT NULL DEFAULT 'default',
                    speaker TEXT,
                    sensitive INTEGER NOT NULL DEFAULT 0,
                    turn_kind TEXT NOT NULL DEFAULT 'reply',
                    trigger_event_ids TEXT,
                    subject TEXT NOT NULL DEFAULT 'user'
                );
            )";
            if (!query.exec(createChatV3)) throw query.lastError();
            if (!query.exec("INSERT INTO chat_history_v3 "
                            "(id, timestamp, user_input, raw_reply, channel, session_id, speaker, sensitive) "
                            "SELECT id, timestamp, user_input, raw_reply, channel, session_id, speaker, sensitive "
                            "FROM chat_history")) throw query.lastError();
            if (!query.exec("DROP TABLE chat_history")) throw query.lastError();
            if (!query.exec("ALTER TABLE chat_history_v3 RENAME TO chat_history")) throw query.lastError();
            query.exec("CREATE INDEX IF NOT EXISTS idx_history_session ON chat_history(channel, session_id);");

            // 2. raw_messages → raw_events（全量事件流水）
            bool hasLegacyRaw = false;
            if (query.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='raw_messages'")
                && query.next()) {
                hasLegacyRaw = true;
            }
            QString createRawEvents = R"(
                CREATE TABLE IF NOT EXISTS raw_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    platform TEXT NOT NULL,
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    message_id TEXT,
                    direction TEXT NOT NULL DEFAULT 'in',
                    sender_id TEXT,
                    sender_name TEXT,
                    event_type TEXT NOT NULL,
                    content TEXT,
                    payload TEXT,
                    occurred_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    sensitive INTEGER NOT NULL DEFAULT 0,
                    visibility TEXT NOT NULL DEFAULT 'private',
                    processed INTEGER NOT NULL DEFAULT 0,
                    UNIQUE(channel, message_id)
                );
            )";
            if (!query.exec(createRawEvents)) throw query.lastError();
            if (hasLegacyRaw) {
                if (!query.exec("INSERT OR IGNORE INTO raw_events "
                                "(platform, channel, session_id, sender_name, event_type, content, "
                                " occurred_at, sensitive, visibility, processed, direction) "
                                "SELECT CASE WHEN channel LIKE 'qq%' THEN 'qq' ELSE 'pet' END, "
                                "       channel, session_id, speaker, 'message', content, "
                                "       timestamp, sensitive, 'private', consolidated, 'in' "
                                "FROM raw_messages")) throw query.lastError();
                if (!query.exec("DROP TABLE raw_messages")) throw query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_events_session ON raw_events(channel, session_id, occurred_at);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_events_processed ON raw_events(processed);");

            // 3. episodic_memory 可见性/主体
            if (!ensureColumn(query, "episodic_memory", "visibility",
                              "ALTER TABLE episodic_memory ADD COLUMN visibility TEXT NOT NULL DEFAULT 'private'")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "episodic_memory", "subject",
                              "ALTER TABLE episodic_memory ADD COLUMN subject TEXT NOT NULL DEFAULT 'user'")) {
                throw query.lastError();
            }

            // 4. 蒸馏游标（每 channel+session 一条）
            QString createCursor = R"(
                CREATE TABLE IF NOT EXISTS memory_cursor (
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    last_event_id INTEGER NOT NULL DEFAULT 0,
                    last_turn_id INTEGER NOT NULL DEFAULT 0,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    PRIMARY KEY(channel, session_id)
                );
            )";
            if (!query.exec(createCursor)) throw query.lastError();

            if (!query.exec("PRAGMA user_version = 3")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 3;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v4: episodic 会话隔离（C3 三重隔离） ==============
        if (currentVersion < 4) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v4(episodic 会话隔离):";
            if (!ensureColumn(query, "episodic_memory", "session_id",
                              "ALTER TABLE episodic_memory ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'")) {
                throw query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_origin_session "
                       "ON episodic_memory(origin, session_id);");
            if (!query.exec("PRAGMA user_version = 4")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 4;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v6: interactions（PR-2） ==============
        if (currentVersion < 6) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v6(interactions):";
            const QString createInteractions = R"(
                CREATE TABLE IF NOT EXISTS interactions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    started_at DATETIME NOT NULL,
                    ended_at DATETIME NOT NULL,
                    source_event_ids TEXT NOT NULL,
                    segmentation_method TEXT NOT NULL,
                    confidence REAL NOT NULL DEFAULT 0.0,
                    safe_summary TEXT,
                    summary_status TEXT NOT NULL DEFAULT 'pending',
                    summary_attempts INTEGER NOT NULL DEFAULT 0,
                    created_at DATETIME DEFAULT (datetime('now','localtime'))
                );
            )";
            if (!query.exec(createInteractions)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interactions_session_time "
                            "ON interactions(channel, session_id, started_at)")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interactions_summary_status "
                            "ON interactions(summary_status, summary_attempts)")) {
                throw query.lastError();
            }

            const QString createParticipants = R"(
                CREATE TABLE IF NOT EXISTS interaction_participants (
                    interaction_id INTEGER NOT NULL,
                    user_id TEXT NOT NULL,
                    display_name_at_time TEXT,
                    participation_kind TEXT NOT NULL DEFAULT 'message',
                    PRIMARY KEY(interaction_id, user_id),
                    FOREIGN KEY(interaction_id) REFERENCES interactions(id) ON DELETE CASCADE
                );
            )";
            if (!query.exec(createParticipants)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interaction_participants_user "
                            "ON interaction_participants(user_id, interaction_id)")) {
                throw query.lastError();
            }

        }

        // ============== v5: raw event 脱敏审计（PR-1a） ==============
        if (currentVersion < 5) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v5(raw event 脱敏审计):";
            if (!ensureColumn(query, "raw_events", "redaction_status",
                              "ALTER TABLE raw_events ADD COLUMN redaction_status TEXT NOT NULL DEFAULT 'none'")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "raw_events", "redaction_rules",
                              "ALTER TABLE raw_events ADD COLUMN redaction_rules TEXT")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "raw_events", "policy_version",
                              "ALTER TABLE raw_events ADD COLUMN policy_version TEXT")) {
                throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version = 5")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 5;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        if (currentVersion < 6) {
            if (!query.exec("PRAGMA user_version = 6")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 6;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v7: person / person_identity（PR-3.0） ==============
        if (currentVersion < 7) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v7(person):";
            const QString createPersons = R"(
                CREATE TABLE IF NOT EXISTS persons (
                    person_id TEXT PRIMARY KEY,
                    role TEXT NOT NULL DEFAULT 'member',
                    canonical_name TEXT,
                    created_at DATETIME DEFAULT (datetime('now','localtime')),
                    updated_at DATETIME DEFAULT (datetime('now','localtime'))
                );
            )";
            if (!query.exec(createPersons)) {
                throw query.lastError();
            }
            const QString createIdentities = R"(
                CREATE TABLE IF NOT EXISTS person_identities (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    person_id TEXT NOT NULL,
                    platform TEXT NOT NULL,
                    platform_user_id TEXT NOT NULL,
                    group_id TEXT NOT NULL DEFAULT '',
                    display_name TEXT,
                    valid_from DATETIME NOT NULL,
                    valid_to DATETIME,
                    source TEXT NOT NULL DEFAULT 'message',
                    last_verified_at DATETIME,
                    FOREIGN KEY(person_id) REFERENCES persons(person_id)
                );
            )";
            if (!query.exec(createIdentities)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_person_identities_person "
                            "ON person_identities(person_id, valid_to)")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_person_identities_lookup "
                            "ON person_identities(platform, platform_user_id, group_id)")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "raw_events", "person_id",
                              "ALTER TABLE raw_events ADD COLUMN person_id TEXT")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "interaction_participants", "person_id",
                              "ALTER TABLE interaction_participants ADD COLUMN person_id TEXT")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_raw_events_person "
                            "ON raw_events(person_id, occurred_at)")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interaction_participants_person "
                            "ON interaction_participants(person_id, interaction_id)")) {
                throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version = 7")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 7;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v8: summary person refs + claims（PR-3.1） ==============
        if (currentVersion < 8) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v8(summary/claims):";
            if (!ensureColumn(query, "interactions", "summary_format",
                              "ALTER TABLE interactions ADD COLUMN "
                              "summary_format TEXT NOT NULL DEFAULT 'legacy'")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "interactions", "summary_segments",
                              "ALTER TABLE interactions ADD COLUMN summary_segments TEXT")) {
                throw query.lastError();
            }
            const QString createClaims = R"(
                CREATE TABLE IF NOT EXISTS memory_claims (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    interaction_id INTEGER NOT NULL,
                    asserted_by_person_id TEXT NOT NULL DEFAULT 'unknown',
                    about_person_id TEXT NOT NULL DEFAULT 'unknown',
                    privacy_owner_person_id TEXT NOT NULL DEFAULT 'unknown',
                    counterpart_person_id TEXT,
                    statement TEXT NOT NULL,
                    privacy_level INTEGER NOT NULL DEFAULT 1,
                    source_event_ids TEXT NOT NULL,
                    source_channel TEXT NOT NULL,
                    source_session_id TEXT NOT NULL,
                    valid_time_start DATETIME,
                    valid_time_end DATETIME,
                    claim_status TEXT NOT NULL DEFAULT 'quarantine',
                    created_at DATETIME DEFAULT (datetime('now','localtime')),
                    updated_at DATETIME DEFAULT (datetime('now','localtime')),
                    FOREIGN KEY(interaction_id) REFERENCES interactions(id) ON DELETE CASCADE
                );
            )";
            if (!query.exec(createClaims)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_memory_claims_interaction "
                            "ON memory_claims(interaction_id, claim_status)")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_memory_claims_persons "
                            "ON memory_claims(privacy_owner_person_id, about_person_id)")) {
                throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version = 8")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 8;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v9: 群成员关系快照（PR-3.2a） ==============
        if (currentVersion < 9) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v9(membership):";
            const QString createPeriods = R"(
                CREATE TABLE IF NOT EXISTS group_membership_periods (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    group_id TEXT NOT NULL,
                    user_id TEXT NOT NULL,
                    person_id TEXT,
                    valid_from DATETIME NOT NULL,
                    valid_to DATETIME,
                    source TEXT NOT NULL,
                    last_verified_at DATETIME,
                    UNIQUE(group_id, user_id, valid_from)
                );
            )";
            if (!query.exec(createPeriods)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_membership_group_user "
                            "ON group_membership_periods(group_id, user_id, valid_to)")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_membership_person "
                            "ON group_membership_periods(person_id, group_id)")) {
                throw query.lastError();
            }
            const QString createSnapshots = R"(
                CREATE TABLE IF NOT EXISTS group_membership_snapshots (
                    group_id TEXT PRIMARY KEY,
                    last_verified_at DATETIME,
                    status TEXT NOT NULL,
                    last_error TEXT
                );
            )";
            if (!query.exec(createSnapshots)) {
                throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version = 9")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 9;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        if (currentVersion < 10) {
            if (!query.exec("ALTER TABLE chat_history ADD COLUMN state_updates TEXT")
                || !query.exec("CREATE INDEX idx_state_subject ON chat_history(subject,id) WHERE state_updates IS NOT NULL")
                || !query.exec("ALTER TABLE chat_history ADD COLUMN thought TEXT")
                || !query.exec("ALTER TABLE chat_history ADD COLUMN thought_version INTEGER NOT NULL DEFAULT 0")
                || !query.exec("ALTER TABLE chat_history ADD COLUMN thought_result TEXT")
                || !query.exec("ALTER TABLE chat_history ADD COLUMN review_after INTEGER")
                || !query.exec("CREATE TABLE thought_review_log(turn_id INTEGER,started_at INTEGER NOT NULL)")
                || !query.exec("CREATE INDEX idx_thought_review_time ON thought_review_log(started_at)")
                || !query.exec("PRAGMA user_version=10"))
                throw query.lastError();
            currentVersion = 10;
        }
        if (currentVersion < 11) {
            const QStringList statements = {
                "ALTER TABLE chat_history ADD COLUMN cognitive_event TEXT",
                "CREATE TABLE cognitive_source(id INTEGER PRIMARY KEY AUTOINCREMENT,event TEXT NOT NULL UNIQUE,"
                "run TEXT,subject TEXT NOT NULL,channel TEXT NOT NULL,session TEXT NOT NULL,input TEXT NOT NULL,"
                "reported TEXT NOT NULL,outcome TEXT,last_error TEXT,rejections INTEGER NOT NULL DEFAULT 0,attempts INTEGER NOT NULL DEFAULT 0,due INTEGER NOT NULL DEFAULT 0)",
                "CREATE INDEX cognitive_pending ON cognitive_source(subject,id) WHERE outcome IS NULL",
                "CREATE TABLE matter_revision(matter TEXT NOT NULL,version INTEGER NOT NULL,source INTEGER NOT NULL,"
                "topic TEXT NOT NULL,status TEXT NOT NULL,quote TEXT NOT NULL,kind TEXT NOT NULL,event_time TEXT,understanding TEXT NOT NULL,"
                "recorded TEXT DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(matter,version),UNIQUE(matter,source))",
                "CREATE INDEX matter_source ON matter_revision(source)",
                "CREATE TRIGGER forget_turn AFTER DELETE ON chat_history BEGIN "
                "DELETE FROM cognitive_source WHERE event=OLD.cognitive_event; END",
                "CREATE TRIGGER forget_observation BEFORE DELETE ON cognitive_source BEGIN "
                "DELETE FROM matter_revision WHERE (matter,version) IN (SELECT r.matter,r.version FROM matter_revision r JOIN matter_revision d ON d.matter=r.matter AND r.version>=d.version WHERE d.source=OLD.id); END",
                "PRAGMA user_version=11"
            };
            for (const QString &statement : statements)
                if (!query.exec(statement)) throw query.lastError();
            currentVersion = 11;
        }
        if (currentVersion < 12) {
            // Revisit the affected events and all later revisions which used their matters.
            // A source may update several matters, so invalidation is transitive.
            const auto invalidate = [](const QString &seed) {
                const QString closure = "WITH RECURSIVE affected(id) AS (" + seed +
                    " UNION SELECT later.source FROM affected a JOIN matter_revision prior ON prior.source=a.id "
                    "JOIN matter_revision later ON later.matter=prior.matter AND later.version>prior.version) "
                    "SELECT id FROM affected";
                return "UPDATE cognitive_source SET outcome=NULL,due=0,rejections=0 WHERE id IN (" + closure + "); "
                    "DELETE FROM observation_history WHERE source IN (SELECT id FROM cognitive_source WHERE outcome IS NULL); "
                    "DELETE FROM matter_revision WHERE source IN (SELECT id FROM cognitive_source WHERE outcome IS NULL); ";
            };
            const QStringList statements = {
                "CREATE TABLE observation_history(source INTEGER NOT NULL,turn INTEGER NOT NULL,PRIMARY KEY(source,turn))",
                "CREATE INDEX observation_history_turn ON observation_history(turn)",
                "CREATE TRIGGER reset_history_support AFTER UPDATE OF outcome ON cognitive_source "
                "WHEN NEW.outcome IS NULL AND OLD.outcome IS NOT NULL BEGIN "
                "DELETE FROM observation_history WHERE source=NEW.id; END",
                "CREATE TRIGGER retry_missing_history AFTER INSERT ON chat_history WHEN NEW.cognitive_event IS NOT NULL BEGIN "
                "UPDATE cognitive_source SET outcome=NULL,due=0 WHERE outcome='needs_evidence' "
                "AND subject=NEW.subject AND channel=NEW.channel AND session=NEW.session_id "
                "AND julianday(reported)>(SELECT julianday(reported) FROM cognitive_source WHERE event=NEW.cognitive_event); END",
                "DROP TRIGGER forget_observation",
                "CREATE TRIGGER forget_observation BEFORE DELETE ON cognitive_source BEGIN " +
                    invalidate("SELECT OLD.id") + "END",
                "CREATE TRIGGER forget_evidence BEFORE DELETE ON chat_history BEGIN " +
                    invalidate("SELECT source FROM observation_history WHERE turn=OLD.id") + "END",
                "UPDATE cognitive_source SET outcome=NULL,due=0,rejections=0 "
                "WHERE outcome IN ('needs_evidence','review_required') AND length(input)>0",
                "PRAGMA user_version=12"
            };
            for (const auto &statement : statements)
                if (!query.exec(statement)) throw query.lastError();
            currentVersion = 12;
        }
        if (currentVersion < 13) {
            const QStringList statements = {
                "ALTER TABLE chat_history ADD COLUMN turn_state TEXT NOT NULL DEFAULT 'legacy'",
                "ALTER TABLE chat_history ADD COLUMN turn_detail TEXT",
                "CREATE UNIQUE INDEX foreground_event ON chat_history(cognitive_event) WHERE turn_state<>'legacy'",
                "PRAGMA user_version=13"
            };
            for (const auto &statement : statements)
                if (!query.exec(statement)) throw query.lastError();
            currentVersion = 13;
        }
        if (currentVersion < 14) {
            const QStringList statements = {
                "CREATE TABLE cognitive_audit(id INTEGER PRIMARY KEY,recorded TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,kind TEXT NOT NULL,run TEXT,source INTEGER,data TEXT NOT NULL)",
                "INSERT INTO cognitive_audit(kind,run,source,data) SELECT 'legacy_response_failure',run,id,json_object('error',last_error,'outcome',outcome,'attempts',attempts,'rejections',rejections) FROM cognitive_source WHERE last_error='empty_content'",
                "UPDATE cognitive_source SET outcome=NULL,due=0,rejections=0 WHERE outcome='review_required' AND last_error='empty_content' AND length(input)>0",
                "CREATE TRIGGER forget_cognitive_audit AFTER DELETE ON cognitive_source BEGIN DELETE FROM cognitive_audit WHERE source=OLD.id; END",
                "CREATE TRIGGER forget_answer_audit AFTER DELETE ON chat_history BEGIN DELETE FROM cognitive_audit WHERE json_extract(data,'$.event_id')=OLD.cognitive_event; END",
                "PRAGMA user_version=14"
            };
            for (const auto &statement : statements)
                if (!query.exec(statement)) throw query.lastError();
            currentVersion = 14;
        }
        if (currentVersion < 15) {
            const QStringList statements = {
                "CREATE TABLE IF NOT EXISTS memory_chunks("
                "id INTEGER PRIMARY KEY,"
                "source_type TEXT NOT NULL,"
                "source_ref TEXT NOT NULL,"
                "chunk_kind TEXT NOT NULL,"
                "derived TEXT,"
                "text TEXT NOT NULL,"
                "content_hash TEXT NOT NULL,"
                "search_text TEXT NOT NULL,"
                "channel TEXT,"
                "session_id TEXT,"
                "speaker_id TEXT,"
                "speaker_name TEXT,"
                "speaker_role TEXT,"
                "speech_at TEXT,"
                "event_time TEXT,"
                "recorded_at TEXT,"
                "privacy TEXT,"
                "status TEXT NOT NULL DEFAULT 'active',"
                "index_generation INTEGER NOT NULL DEFAULT 1,"
                "created_at TEXT NOT NULL,"
                "updated_at TEXT NOT NULL,"
                "UNIQUE(source_type,source_ref,chunk_kind,content_hash,index_generation))",
                "CREATE INDEX IF NOT EXISTS memory_chunks_active ON memory_chunks(status,index_generation,id)",
                "CREATE INDEX IF NOT EXISTS memory_chunks_source ON memory_chunks(source_type,source_ref)",
                "CREATE TABLE IF NOT EXISTS memory_chunk_sources("
                "chunk_id INTEGER NOT NULL,"
                "source_table TEXT NOT NULL,"
                "source_id TEXT NOT NULL,"
                "range_start INTEGER,"
                "range_end INTEGER,"
                "PRIMARY KEY(chunk_id,source_table,source_id))",
                "CREATE INDEX IF NOT EXISTS memory_chunk_sources_source ON memory_chunk_sources(source_table,source_id)",
                "CREATE TABLE IF NOT EXISTS memory_embeddings("
                "chunk_id INTEGER NOT NULL,"
                "model_config TEXT NOT NULL,"
                "dimension INTEGER NOT NULL,"
                "vector BLOB NOT NULL,"
                "content_hash TEXT NOT NULL,"
                "index_generation INTEGER NOT NULL,"
                "created_at TEXT NOT NULL,"
                "PRIMARY KEY(chunk_id,model_config))",
                "CREATE TABLE IF NOT EXISTS memory_index_state("
                "key TEXT PRIMARY KEY,value TEXT)",
                "CREATE TRIGGER IF NOT EXISTS forget_chunk_parts AFTER DELETE ON memory_chunks BEGIN "
                "DELETE FROM memory_chunk_sources WHERE chunk_id=OLD.id; "
                "DELETE FROM memory_embeddings WHERE chunk_id=OLD.id; END"
            };
            for (const auto &statement : statements)
                if (!query.exec(statement)) throw query.lastError();
            auto hasTable = [&](const QString &name) {
                QSqlQuery check(m_db);
                check.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
                check.addBindValue(name);
                return check.exec() && check.next();
            };
            if (hasTable(QStringLiteral("raw_events"))
                && !query.exec("CREATE TRIGGER IF NOT EXISTS forget_raw_chunks AFTER DELETE ON raw_events BEGIN "
                               "DELETE FROM memory_chunks WHERE source_type='raw' AND source_ref='raw/'||OLD.rowid; END"))
                throw query.lastError();
            if (hasTable(QStringLiteral("chat_history"))
                && !query.exec("CREATE TRIGGER IF NOT EXISTS forget_chat_chunks AFTER DELETE ON chat_history BEGIN "
                               "DELETE FROM memory_chunks WHERE source_type='chat' AND source_ref LIKE 'chat/'||OLD.id||':%'; END"))
                throw query.lastError();
            if (!query.exec("PRAGMA user_version=15"))
                throw query.lastError();
            currentVersion = 15;
        }
        if (currentVersion < 16) {
            // Older rows have unknown recording times. Leave them NULL.
            for (const QString table : {"interactions", "episodic_memory", "raw_events"}) {
                QSqlQuery columns(m_db);
                if (!columns.exec("PRAGMA table_info(" + table + ")")) throw columns.lastError();
                bool exists = false, stamped = false;
                while (columns.next()) {
                    exists = true;
                    stamped |= columns.value(1).toString() == "content_recorded_at";
                }
                if (!exists) continue;
                if (!stamped && !query.exec("ALTER TABLE " + table + " ADD COLUMN content_recorded_at TEXT"))
                    throw query.lastError();
                const QString fields = table == "interactions" ? "safe_summary,summary_segments"
                    : table == "raw_events" ? "content" : "content,event_time";
                const QString stamp = " ON " + table + " BEGIN UPDATE " + table
                    + " SET content_recorded_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=NEW.id; END";
                if (!query.exec("CREATE TRIGGER IF NOT EXISTS stamp_" + table + "_insert AFTER INSERT" + stamp)
                    || !query.exec("CREATE TRIGGER IF NOT EXISTS stamp_" + table + "_update AFTER UPDATE OF "
                                   + fields + stamp)) throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version=16")) throw query.lastError();
            currentVersion = 16;
        }
        if (currentVersion < 17) {
            QSqlQuery columns(m_db);
            if (!columns.exec("PRAGMA table_info(matter_revision)")) throw columns.lastError();
            bool hasSemantics = false;
            while (columns.next()) hasSemantics |= columns.value(1).toString() == "semantics";
            if (!hasSemantics && !query.exec("ALTER TABLE matter_revision ADD COLUMN semantics TEXT"))
                throw query.lastError();
            if (!query.exec("PRAGMA user_version=17")) throw query.lastError();
            currentVersion = 17;
        }
        if (currentVersion < 18) {
            if (!query.exec("CREATE TABLE IF NOT EXISTS private_reflection(turn_id INTEGER PRIMARY KEY,"
                            "version INTEGER NOT NULL,state TEXT NOT NULL,eligible_at INTEGER NOT NULL,"
                            "note TEXT,updated_at INTEGER NOT NULL)")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_private_reflection AFTER DELETE ON chat_history "
                               "BEGIN DELETE FROM private_reflection WHERE turn_id=OLD.id; END")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_reflection_source AFTER DELETE ON cognitive_source "
                               "BEGIN DELETE FROM private_reflection WHERE turn_id IN "
                               "(SELECT id FROM chat_history WHERE cognitive_event=OLD.event); END")
                || !query.exec("PRAGMA user_version=18")) throw query.lastError();
            currentVersion = 18;
        }
        if (currentVersion < 19) {
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_contact(turn_id INTEGER PRIMARY KEY,"
                            "version INTEGER NOT NULL,timing TEXT NOT NULL,reason TEXT,message TEXT,"
                            "outcome TEXT NOT NULL,channel TEXT,session TEXT)")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_reflection_contact AFTER DELETE ON private_reflection "
                               "BEGIN DELETE FROM reflection_contact WHERE turn_id=OLD.turn_id; END")
                || !query.exec("PRAGMA user_version=19")) throw query.lastError();
            currentVersion = 19;
        }
        if (currentVersion < 20) {
            if (!query.exec("CREATE TABLE IF NOT EXISTS reminder(id INTEGER PRIMARY KEY AUTOINCREMENT,turn INTEGER NOT NULL,"
                            "channel TEXT NOT NULL,session TEXT NOT NULL,due INTEGER NOT NULL,expires INTEGER NOT NULL,"
                            "message TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'pending',UNIQUE(turn,due))")
                || !query.exec("CREATE INDEX IF NOT EXISTS reminder_due ON reminder(state,channel,session,due)")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_reminder AFTER DELETE ON chat_history BEGIN DELETE FROM reminder WHERE turn=OLD.id; END")
                || !query.exec("PRAGMA user_version=20")) throw query.lastError();
            currentVersion=20;
        }
        if (currentVersion<21) {
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_basis(turn INTEGER,source INTEGER,PRIMARY KEY(turn,source))")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_reflection_basis AFTER DELETE ON chat_history BEGIN "
                               "DELETE FROM private_reflection WHERE turn_id IN (SELECT turn FROM reflection_basis WHERE source=OLD.id); "
                               "DELETE FROM reflection_basis WHERE source=OLD.id OR turn=OLD.id; END")
                || !query.exec("PRAGMA user_version=21")) throw query.lastError();
            currentVersion=21;
        }
        if (currentVersion<22) {
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_change(turn INTEGER PRIMARY KEY,prior_turn INTEGER UNIQUE NOT NULL,"
                            "action TEXT NOT NULL CHECK(action IN ('revise','resolve')))")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_reflection_change AFTER DELETE ON private_reflection BEGIN "
                               "DELETE FROM reflection_change WHERE turn=OLD.turn_id OR prior_turn=OLD.turn_id; END")
                || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_reflection_evidence AFTER DELETE ON cognitive_source BEGIN "
                               "DELETE FROM private_reflection WHERE turn_id IN (SELECT b.turn FROM reflection_basis b "
                               "JOIN chat_history h ON h.id=b.source WHERE h.cognitive_event=OLD.event); END")
                || !query.exec("PRAGMA user_version=22")) throw query.lastError();
            currentVersion=22;
        }
        if (currentVersion<23) {
            // 关注生命周期字段（v23）。旧版本回退重放（例如把 user_version 调低再启动）时
            // private_reflection 里可能已经有这些列，故按列名幂等补列；
            // 新库从 v0 一路迁移时 private_reflection 已在 v18 建好，这里同样只是补列。
            QSqlQuery columns(m_db);
            if (!columns.exec("PRAGMA table_info(private_reflection)")) throw columns.lastError();
            QSet<QString> present;
            while (columns.next()) present.insert(columns.value(1).toString());
            const QList<QPair<QString,QString>> alters{
                {QStringLiteral("topic"),QStringLiteral("TEXT")},
                {QStringLiteral("watch"),QStringLiteral("TEXT")},
                {QStringLiteral("root_turn"),QStringLiteral("INTEGER")},
                {QStringLiteral("next_check_at"),QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
                {QStringLiteral("resolved_by"),QStringLiteral("INTEGER")},
                {QStringLiteral("resolved_kind"),QStringLiteral("TEXT")},
                {QStringLiteral("matter"),QStringLiteral("TEXT")},
                {QStringLiteral("claimed_by"),QStringLiteral("TEXT")}};
            for (const auto &alter : alters) {
                if (present.contains(alter.first)) continue;
                if (!query.exec(QStringLiteral("ALTER TABLE private_reflection ADD COLUMN ")
                                + alter.first + QStringLiteral(" ") + alter.second))
                    throw query.lastError();
            }
            // 既有行补根轮次与下一次检查时间：关注身份默认等于来源轮次，12 小时后再看。
            if (!query.exec("UPDATE private_reflection SET root_turn=turn_id WHERE root_turn IS NULL")
                || !query.exec("UPDATE private_reflection SET next_check_at=updated_at+43200 WHERE next_check_at=0")
                || !query.exec("PRAGMA user_version=23")) throw query.lastError();
            currentVersion=23;
        }
        if (currentVersion<24) {
            // v24：主动联系/分享的"执行反馈"（B4）。
            // 1) reflection_contact 增加投递状态：attempts 只统计被拒/未知的失败尝试，
            //    last_outcome 做终态快照（outcome 列仍是当前状态机状态），last_attempt_at 供重试排序。
            //    旧库回退重放（把 user_version 调低再启动）时这些列可能已存在，先按列名幂等补列。
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_contact(turn_id INTEGER PRIMARY KEY,"
                            "version INTEGER NOT NULL,timing TEXT NOT NULL,reason TEXT,message TEXT,"
                            "outcome TEXT NOT NULL,channel TEXT,session TEXT)"))
                throw query.lastError();
            QSqlQuery contactColumns(m_db);
            if (!contactColumns.exec("PRAGMA table_info(reflection_contact)")) throw contactColumns.lastError();
            QSet<QString> contactPresent;
            while (contactColumns.next()) contactPresent.insert(contactColumns.value(1).toString());
            const QList<QPair<QString,QString>> contactAlters{
                {QStringLiteral("attempts"),QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
                {QStringLiteral("last_outcome"),QStringLiteral("TEXT")},
                {QStringLiteral("last_attempt_at"),QStringLiteral("INTEGER")}};
            for (const auto &alter : contactAlters) {
                if (contactPresent.contains(alter.first)) continue;
                if (!query.exec(QStringLiteral("ALTER TABLE reflection_contact ADD COLUMN ")
                                + alter.first + QStringLiteral(" ") + alter.second))
                    throw query.lastError();
            }
            // 存量行回填：老库的 outcome 就是最近一次结果；没有 attempts 历史，按 0 起算。
            if (!query.exec("UPDATE reflection_contact SET last_outcome=outcome WHERE last_outcome IS NULL"))
                throw query.lastError();
            // 2) 统一投递流水：提醒/主动联系/草稿分享共用一张表，按 (kind,ref) 幂等。
            //    state 取值：claimed|handed_off|protocol_accepted|rejected|unknown|withheld|expired。
            if (!query.exec("CREATE TABLE IF NOT EXISTS delivery_ledger("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "kind TEXT NOT NULL,"
                            "ref TEXT NOT NULL,"
                            "channel TEXT, session TEXT,"
                            "text TEXT,"
                            "state TEXT NOT NULL,"
                            "attempts INTEGER NOT NULL DEFAULT 0,"
                            "last_error TEXT,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "UNIQUE(kind,ref))")
                || !query.exec("CREATE INDEX IF NOT EXISTS delivery_ledger_state "
                               "ON delivery_ledger(state,kind)")
                || !query.exec("PRAGMA user_version=24")) throw query.lastError();
            currentVersion=24;
        }
        if (currentVersion<25) {
            // v25：统一注意力事件（B1）。
            // attention_event 是通用队列：kind=pending 状态机 + (kind,ref) 幂等键；
            // attention_state 存"当前活动/切换原因/最近唤醒"等当下状态。
            // created_at/updated_at 由写入方显式给 unixepoch()，不依赖 DEFAULT 表达式，
            // 与旧库时区约定一致。旧测试会把 user_version 压回更小值重放迁移，故此块
            // 全部用 IF NOT EXISTS，保证可重放。
            if (!query.exec("CREATE TABLE IF NOT EXISTS attention_event("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "kind TEXT NOT NULL,"
                            "ref TEXT NOT NULL,"
                            "payload TEXT NOT NULL DEFAULT '{}',"
                            "state TEXT NOT NULL DEFAULT 'pending',"
                            "priority INTEGER NOT NULL DEFAULT 50,"
                            "reason TEXT,"
                            "created_at INTEGER NOT NULL DEFAULT 0,"
                            "claimed_at INTEGER,"
                            "done_at INTEGER,"
                            "UNIQUE(kind,ref))")
                || !query.exec("CREATE INDEX IF NOT EXISTS attention_event_pending "
                               "ON attention_event(state,priority,id)")
                || !query.exec("CREATE TABLE IF NOT EXISTS attention_state("
                               "key TEXT PRIMARY KEY,"
                               "value TEXT,"
                               "updated_at INTEGER NOT NULL DEFAULT 0)")
                || !query.exec("PRAGMA user_version=25")) throw query.lastError();
            currentVersion=25;
        }
        if (currentVersion<26) {
            // 到期回顾的决策流水：来源回顾用 reflection_change 新建修订链，
            // 到期回顾是原地更新同一行，旧想法会被覆盖，因此单独留旧/新快照供审计。
            if (!query.exec("CREATE TABLE IF NOT EXISTS concern_revision("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "turn INTEGER NOT NULL,"
                            "version INTEGER NOT NULL,"
                            "action TEXT NOT NULL,"
                            "note_before TEXT,"
                            "note_after TEXT,"
                            "topic TEXT,"
                            "watch TEXT,"
                            "next_check_at INTEGER NOT NULL DEFAULT 0,"
                            "contact_timing TEXT,"
                            "recorded TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)")
                || !query.exec("CREATE INDEX IF NOT EXISTS concern_revision_turn ON concern_revision(turn)")
                || !query.exec("PRAGMA user_version=26")) throw query.lastError();
            currentVersion=26;
        }
        if (currentVersion<27) {
            // v27：主动联系的"可核验行动依据"字段。basis_type/basis_refs 与联系意图同一行落库，
            // 供投递前审计与事后追溯（例如"是凭哪条提醒/哪次发言/哪个活动结果决定现在联系"）。
            // 旧测试会把 user_version 压回更小值重放迁移，故先按列名幂等补列再升级版本号。
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_contact(turn_id INTEGER PRIMARY KEY,"
                            "version INTEGER NOT NULL,timing TEXT NOT NULL,reason TEXT,message TEXT,"
                            "outcome TEXT NOT NULL,channel TEXT,session TEXT)"))
                throw query.lastError();
            QSqlQuery basisColumns(m_db);
            if (!basisColumns.exec("PRAGMA table_info(reflection_contact)")) throw basisColumns.lastError();
            QSet<QString> basisPresent;
            while (basisColumns.next()) basisPresent.insert(basisColumns.value(1).toString());
            for (const auto &column : {QStringLiteral("basis_type"), QStringLiteral("basis_refs")}) {
                if (basisPresent.contains(column)) continue;
                if (!query.exec(QStringLiteral("ALTER TABLE reflection_contact ADD COLUMN ")
                                + column + QStringLiteral(" TEXT")))
                    throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version=27")) throw query.lastError();
            currentVersion=27;
        }
        if (currentVersion<28) {
            // v28：contact now 的依据门禁定稿。
            // 1) 时间委托改由提醒链独占执行，回顾不再能以 explicit_time_delegation 依据"现在联系"；
            //    旧库里的 now 联系若没有可核验依据（basis_type/basis_refs 为空），在新授权模型下
            //    不可投递：统一置 withheld 并留审计，避免旧行被投递层直接领走。
            // 2) reflection_contact 原先没有 updated_at；v28 起用它记录状态机最近变更时间
            //    （basis_expired / dispatch_abandoned 等审计）。旧测试会把 user_version 压回
            //    更小值重放迁移，故先按列名幂等补列，再执行数据整理并升级版本号。
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_contact(turn_id INTEGER PRIMARY KEY,"
                            "version INTEGER NOT NULL,timing TEXT NOT NULL,reason TEXT,message TEXT,"
                            "outcome TEXT NOT NULL,channel TEXT,session TEXT)"))
                throw query.lastError();
            QSqlQuery contactColumns28(m_db);
            if (!contactColumns28.exec("PRAGMA table_info(reflection_contact)"))
                throw contactColumns28.lastError();
            QSet<QString> contactPresent28;
            while (contactColumns28.next()) contactPresent28.insert(contactColumns28.value(1).toString());
            if (!contactPresent28.contains(QStringLiteral("updated_at"))) {
                if (!query.exec(QStringLiteral("ALTER TABLE reflection_contact ADD COLUMN updated_at INTEGER")))
                    throw query.lastError();
            }
            if (!query.exec("UPDATE reflection_contact SET outcome='withheld',"
                            "last_outcome='missing_basis_at_v28',updated_at=unixepoch() "
                            "WHERE timing='now' AND outcome IN ('proposed','rejected') "
                            "AND (basis_type IS NULL OR basis_type='' "
                            "OR basis_refs IS NULL OR basis_refs='')"))
                throw query.lastError();
            if (!query.exec("PRAGMA user_version=28")) throw query.lastError();
            currentVersion=28;
        }
        if (currentVersion<29) {
            // v29：主动外发的唯一提交入口（切片 B）。
            // cognitive_action 是"一次主动动作"的本地唯一记录（action_key UNIQUE 幂等）；
            // action_outbox 是它的发送队列（1:1，action_id/action_key 都 UNIQUE）。
            // 两者与领域状态在同一个 SQLite 事务里写入；发送只由 OutboxDispatcher 做，
            // 事务内不碰网络。旧测试会把 user_version 压回更小值重放迁移，故全部 IF NOT EXISTS。
            if (!query.exec("CREATE TABLE IF NOT EXISTS cognitive_action ("
                            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "action_key TEXT NOT NULL UNIQUE,"
                            "kind TEXT NOT NULL,"
                            "run_id TEXT, trigger_ref TEXT,"
                            "subject TEXT NOT NULL DEFAULT 'user',"
                            "channel TEXT NOT NULL, session_id TEXT NOT NULL,"
                            "motivation_type TEXT, motivation_ref TEXT,"
                            "payload_json TEXT NOT NULL,"
                            "claims_json TEXT, context_refs_json TEXT, snapshot_hash TEXT, expected_json TEXT,"
                            "state TEXT NOT NULL,"
                            "policy_reason TEXT, semantic_status TEXT,"
                            "attempts INTEGER NOT NULL DEFAULT 0,"
                            "last_outcome TEXT,"
                            "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
                            "updated_at INTEGER NOT NULL DEFAULT (unixepoch()))")
                || !query.exec("CREATE TABLE IF NOT EXISTS action_outbox ("
                               "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               "action_id INTEGER NOT NULL UNIQUE,"
                               "action_key TEXT NOT NULL UNIQUE,"
                               "channel TEXT NOT NULL, session_id TEXT NOT NULL,"
                               "payload_json TEXT NOT NULL,"
                               "state TEXT NOT NULL,"
                               "lease_owner TEXT, lease_until INTEGER,"
                               "attempts INTEGER NOT NULL DEFAULT 0,"
                               "dispatched_at INTEGER, completed_at INTEGER,"
                               "last_error TEXT,"
                               "FOREIGN KEY(action_id) REFERENCES cognitive_action(id))")
                || !query.exec("CREATE INDEX IF NOT EXISTS action_outbox_state ON action_outbox(state,id)")
                || !query.exec("PRAGMA user_version=29")) throw query.lastError();
            currentVersion=29;
        }
        if (currentVersion<30) {
            // v30（报告切片 C）：动机与事实分离。
            // reflection_contact 增加三列：
            //   motivation_json —— motivation{type,ref,summary}，说明"她为什么想开口"；
            //   claims_json     —— claims[{id,type,text,refs}]，消息里的事实/承诺/执行结果声称；
            //   semantic_status —— 语义核验结论（entailed/repaired/withheld/not_required/retained），
            //                      使"最终为什么保留/压下这条联系"可审计。
            // 旧 basis 路径继续用 basis_type/basis_refs，不受影响；旧测试会把 user_version
            // 压回更小值重放迁移，故先按列名幂等补列再升级版本号。
            if (!query.exec("CREATE TABLE IF NOT EXISTS reflection_contact(turn_id INTEGER PRIMARY KEY,"
                            "version INTEGER NOT NULL,timing TEXT NOT NULL,reason TEXT,message TEXT,"
                            "outcome TEXT NOT NULL,channel TEXT,session TEXT)"))
                throw query.lastError();
            QSqlQuery contactColumns30(m_db);
            if (!contactColumns30.exec("PRAGMA table_info(reflection_contact)"))
                throw contactColumns30.lastError();
            QSet<QString> contactPresent30;
            while (contactColumns30.next()) contactPresent30.insert(contactColumns30.value(1).toString());
            const QList<QPair<QString,QString>> contactAlters30{
                {QStringLiteral("motivation_json"),QStringLiteral("TEXT")},
                {QStringLiteral("claims_json"),QStringLiteral("TEXT")},
                {QStringLiteral("semantic_status"),QStringLiteral("TEXT")}};
            for (const auto &alter : contactAlters30) {
                if (contactPresent30.contains(alter.first)) continue;
                if (!query.exec(QStringLiteral("ALTER TABLE reflection_contact ADD COLUMN ")
                                + alter.first + QStringLiteral(" ") + alter.second))
                    throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version=30")) throw query.lastError();
            currentVersion=30;
        }

        if (currentVersion<31) {
            // v31（R12-1a）：OpenWake 调度状态/租约、内部活动骨架、持久化回顾失败退避。
            // 全部 IF NOT EXISTS / 判列幂等，旧测试把 user_version 压回更小值重放迁移也安全；
            // 本块不改写任何既有事实行，也不触碰 action/outbox 的存量提交。
            const QStringList v31Statements = {
                "CREATE TABLE IF NOT EXISTS open_wake_state("
                "scope_key TEXT PRIMARY KEY,"
                "next_due_at INTEGER NOT NULL,"
                "last_started_at INTEGER,"
                "last_finished_at INTEGER,"
                "last_outcome TEXT,"
                "last_wake_id INTEGER,"
                "consecutive_failures INTEGER NOT NULL DEFAULT 0,"
                "non_open_since_due INTEGER NOT NULL DEFAULT 0,"
                "suspended_until INTEGER,"
                "version INTEGER NOT NULL DEFAULT 0,"
                "updated_at INTEGER NOT NULL)",
                "CREATE TABLE IF NOT EXISTS open_wake("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "scope_key TEXT NOT NULL,"
                "run_id TEXT NOT NULL UNIQUE,"
                "version INTEGER NOT NULL DEFAULT 1,"
                "state TEXT NOT NULL,"
                "trigger_reason TEXT NOT NULL,"
                "created_at INTEGER NOT NULL,"
                "started_at INTEGER,"
                "finished_at INTEGER,"
                "lease_until INTEGER,"
                "current_context_epoch INTEGER,"
                "snapshot_hash TEXT,"
                "decision TEXT,"
                "note TEXT,"
                "intent_json TEXT,"
                "next_due_at INTEGER,"
                "error_code TEXT)",
                "CREATE UNIQUE INDEX IF NOT EXISTS open_wake_running_unique "
                "ON open_wake(scope_key) WHERE state='running'",
                "CREATE INDEX IF NOT EXISTS open_wake_scope_state ON open_wake(scope_key,state,id)",
                "CREATE TABLE IF NOT EXISTS internal_activity("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "scope_key TEXT NOT NULL,"
                "origin_kind TEXT NOT NULL,"
                "origin_ref TEXT,"
                "run_id TEXT,"
                "subject TEXT NOT NULL DEFAULT 'user',"
                "channel TEXT NOT NULL,"
                "session_id TEXT NOT NULL,"
                "interest TEXT NOT NULL,"
                "status TEXT NOT NULL DEFAULT 'active',"
                "version INTEGER NOT NULL DEFAULT 0,"
                "result_json TEXT NOT NULL DEFAULT '{}',"
                "artifact_ref TEXT,"
                "review_after INTEGER NOT NULL DEFAULT 0,"
                "created_at INTEGER NOT NULL,"
                "updated_at INTEGER NOT NULL)",
                "CREATE TABLE IF NOT EXISTS review_failure("
                "source_kind TEXT NOT NULL,"
                "source_ref TEXT NOT NULL,"
                "consecutive_failures INTEGER NOT NULL DEFAULT 0,"
                "next_retry_at INTEGER,"
                "suspended_until INTEGER,"
                "last_stage TEXT,"
                "last_reason TEXT,"
                "content_hash TEXT,"
                "last_attempt_at INTEGER,"
                "updated_at INTEGER NOT NULL,"
                "PRIMARY KEY(source_kind,source_ref))"
            };
            for (const auto &statement : v31Statements)
                if (!query.exec(statement)) throw query.lastError();
            // 迁移重放场景里 thought_review_log 可能因为跳过 v10 而不存在，先幂等补表再补列；
            // 旧行的 source_* 保持 NULL，现有"按整表当日行数"计预算的逻辑不变。
            if (!query.exec("CREATE TABLE IF NOT EXISTS thought_review_log("
                            "turn_id INTEGER,started_at INTEGER NOT NULL)"))
                throw query.lastError();
            if (!ensureColumn(query, "thought_review_log", "source_kind",
                              "ALTER TABLE thought_review_log ADD COLUMN source_kind TEXT")
                || !ensureColumn(query, "thought_review_log", "source_ref",
                                 "ALTER TABLE thought_review_log ADD COLUMN source_ref TEXT")
                || !ensureColumn(query, "thought_review_log", "run_id",
                                 "ALTER TABLE thought_review_log ADD COLUMN run_id TEXT"))
                throw query.lastError();
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_thought_review_time "
                            "ON thought_review_log(started_at)"))
                throw query.lastError();
            if (!query.exec("PRAGMA user_version=31")) throw query.lastError();
            currentVersion=31;
            qDebug()<<"[MemoryManager]数据库结构升级完成,版本:"<<currentVersion;
        }

        if (currentVersion<32) {
            // v32（R12-2a）：通用 ActionProposal 持久化。
            // 只新增独立表/索引，不读写任何旧表（chat_history/cognitive_action/action_outbox
            // 存量行原样保留）；全部 IF NOT EXISTS，旧测试把 user_version 压回更小值重放
            // 迁移或重复启动都安全。state 机与 action_key 唯一约束在本表内自洽：
            //   proposed → verified → committed（发送留给 R12-2b）
            //            ↘ withheld / expired / unknown
            // action_key 由 ActionOrigin 生成，唯一约束是幂等的最后防线。
            const QStringList v32Statements = {
                "CREATE TABLE IF NOT EXISTS action_proposal("
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
                "updated_at INTEGER NOT NULL)",
                "CREATE INDEX IF NOT EXISTS action_proposal_state_expires "
                "ON action_proposal(state,expires_at)",
                "CREATE INDEX IF NOT EXISTS action_proposal_origin "
                "ON action_proposal(origin_kind,origin_ref)"
            };
            for (const auto &statement : v32Statements)
                if (!query.exec(statement)) throw query.lastError();
            if (!query.exec("PRAGMA user_version=32")) throw query.lastError();
            currentVersion=32;
            qDebug()<<"[MemoryManager]数据库结构升级完成,版本:"<<currentVersion;
        }

        if (currentVersion<33) {
            // v33（R12-2c-2）：ActionProposal 的 verified 内容绑定 / 历史投影与取消审计 /
            // Action Context Fence 快照。原因：
            //   * v32 只记录"核验通过"这一状态，没有持久化"到底核验了哪份内容"。模型核验
            //     与真正提交之间如果 payload/claims 被改写（内存/DB 篡改或串线），提交层无法
            //     证明"核验的是 A、发送的是 B"；因此新增 verified_payload_hash（与 verifier
            //     输入的 payload_digest 同一规范化算法）、verified_claims_hash、
            //     verified_request_id、verified_at，核验通过时在 proposed→verified 的同一事务写入；
            //   * 提案在提交后仍需在 claimOutbox/真正发送前被取消（主人插话使语境变化、
            //     通道关闭、成果撤回、过期等）。已提交提案不能回退 state（回执闭环依赖
            //     committed），因此用 cancel_reason 记录取消审计；history_projected 预留给
            //     c3 回执历史投影幂等标记（本片只建列并保持 0，不实现 c3 闭环）；
            //   * action_context_epoch 记录"提案生成时的行动语境版本"快照，来源方案未包含，
            //     但 fence 必须持久化在提案上才能跨重启比较，故一并加入（不是来源身份里的
            //     context_epoch）。
            // 幂等：全部 ensureColumn + IF NOT EXISTS；旧测试把 user_version 压回更小值重放
            // 迁移也安全。旧行新列保持 NULL/0，不改写任何既有提案/动作/Outbox。
            if (!ensureColumn(query, "action_proposal", "verified_payload_hash",
                              "ALTER TABLE action_proposal ADD COLUMN verified_payload_hash TEXT")
                || !ensureColumn(query, "action_proposal", "verified_claims_hash",
                                 "ALTER TABLE action_proposal ADD COLUMN verified_claims_hash TEXT")
                || !ensureColumn(query, "action_proposal", "verified_request_id",
                                 "ALTER TABLE action_proposal ADD COLUMN verified_request_id TEXT")
                || !ensureColumn(query, "action_proposal", "verified_at",
                                 "ALTER TABLE action_proposal ADD COLUMN verified_at INTEGER")
                || !ensureColumn(query, "action_proposal", "history_projected",
                                 "ALTER TABLE action_proposal ADD COLUMN history_projected "
                                 "INTEGER NOT NULL DEFAULT 0")
                || !ensureColumn(query, "action_proposal", "cancel_reason",
                                 "ALTER TABLE action_proposal ADD COLUMN cancel_reason TEXT")
                || !ensureColumn(query, "action_proposal", "action_context_epoch",
                                 "ALTER TABLE action_proposal ADD COLUMN action_context_epoch "
                                 "INTEGER NOT NULL DEFAULT 0"))
                throw query.lastError();
            if (!query.exec("CREATE INDEX IF NOT EXISTS action_proposal_verified_at "
                            "ON action_proposal(verified_at)")
                || !query.exec("PRAGMA user_version=33")) throw query.lastError();
            currentVersion=33;
            qDebug()<<"[MemoryManager]数据库结构升级完成,版本:"<<currentVersion;
        }

        // 关注了结的独立流水（v22 的补充表）。与 reflection_change（回顾链修订）分开：
        // 前者由正常对话写回，后者由私下回顾写，共用 turn 主键会互相覆盖。
        // 用 IF NOT EXISTS 幂等补表，不改变结构版本号，旧库升级与新库初始化走同一句。
        if (!query.exec("CREATE TABLE IF NOT EXISTS concern_resolution(id INTEGER PRIMARY KEY AUTOINCREMENT,"
                        "turn INTEGER NOT NULL,prior_turn INTEGER NOT NULL UNIQUE,"
                        "action TEXT NOT NULL,kind TEXT NOT NULL,"
                        "recorded TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)")
            || !query.exec("CREATE INDEX IF NOT EXISTS concern_resolution_turn ON concern_resolution(turn)")
            || !query.exec("CREATE TRIGGER IF NOT EXISTS forget_concern_resolution AFTER DELETE ON private_reflection "
                           "BEGIN DELETE FROM concern_resolution WHERE prior_turn=OLD.turn_id; END"))
            throw query.lastError();
        if (!m_db.commit()) throw m_db.lastError();
        qDebug()<<"[MemoryManager]数据库初始化完成,结构版本为:"<<currentVersion;
    }
    catch(const QSqlError &error){
        m_db.rollback();
        qDebug()<<"[MemoryManager]数据库更新失败:"<<error;
    }
}

// ============================ R12-1a：回顾失败持久化 / OpenWake ============================
// 这一片只做内存/SQLite 层：
//   * review_failure 记录四类来源（reflection|concern|activity|open_wake）的连续失败、
//     退避与暂停；计数/退避/恢复/终止规则集中在这里，调用方只负责传错误分类与日志；
//   * OpenWake 的调度状态、租约、每日独立预算和公平计数都持久化在 SQLite；
//     领取/提交/延后/取消/恢复都是带 version CAS 的单一事务；
//   * internal_activity 本片只读：不创建 OpenWake 活动、不迁移存量 thought 活动。
namespace {

const QString kReviewKindReflection = QStringLiteral("reflection");
const QString kReviewKindConcern = QStringLiteral("concern");
const QString kReviewKindActivity = QStringLiteral("activity");
const QString kReviewKindOpenWake = QStringLiteral("open_wake");

bool reviewKindValid(const QString &kind)
{
    return kind == kReviewKindReflection || kind == kReviewKindConcern
        || kind == kReviewKindActivity || kind == kReviewKindOpenWake;
}

// 内容摘要：SHA-256 取前 16 个 hex 字符。只用于"来源是否实质修订"的比较，不承载安全含义。
QString reviewHash16(const QString &payload)
{
    return QString::fromLatin1(QCryptographicHash::hash(payload.toUtf8(),
                                                        QCryptographicHash::Sha256)
                                   .toHex()
                                   .left(16));
}

QString reviewIsoFromEpoch(qlonglong seconds)
{
    return seconds > 0
        ? QDateTime::fromSecsSinceEpoch(seconds).toUTC().toString(Qt::ISODateWithMs)
        : QString{};
}

// source_ref 约定（与 recentOpenWakeNotes 的 ref 一致）：
//   reflection/<turn>、concern/<turn>、thought/<id>、wake/<id>:<version>。
// 也接受裸数字 id（open_wake 除外：open_wake 需要行版本做 CAS）。
bool parseReviewSourceRef(const QString &kind, const QString &ref, qlonglong *id, int *version)
{
    if (!id) return false;
    *id = 0;
    if (version) *version = 0;
    const QString trimmed = ref.trimmed();
    if (trimmed.isEmpty()) return false;
    const int slash = trimmed.indexOf(QLatin1Char('/'));
    const QString idText = slash >= 0 ? trimmed.mid(slash + 1).section(QLatin1Char(':'), 0, 0)
                                      : trimmed;
    bool ok = false;
    const qlonglong parsed = idText.toLongLong(&ok);
    if (!ok || parsed <= 0) return false;
    *id = parsed;
    if (version) {
        const QString versionText = slash >= 0
            ? trimmed.mid(slash + 1).section(QLatin1Char(':'), 1, 1) : QString{};
        if (!versionText.isEmpty()) {
            bool versionOk = false;
            const int parsedVersion = versionText.toInt(&versionOk);
            if (!versionOk || parsedVersion <= 0) return false;
            *version = parsedVersion;
        }
    }
    return true;
}

struct ReviewSourceSnapshot
{
    bool present = false;
    bool internalActivity = false;    // R12-1b：activity/<id> 指向 internal_activity（否则 chat_history）
    QString state;                    // 源状态机当前状态
    int version = 0;                  // 与 CAS 相关的行版本
    QString contentHash;              // 用于检测来源实质修订
    QString scopeKey;                 // open_wake 的调度 scope
    qlonglong scheduleFailures = 0;   // open_wake_state.consecutive_failures（跨 wake 的权威计数）
};

// 只读来源快照：内容字段按来源定义拼接后取哈希。
//   reflection/concern = note/topic/watch；
//   activity = legacy(thought/status/goal/progress/draft) 或
//              internal(interest/status/result_json 关键字段)；
//   open_wake = note/intent。
ReviewSourceSnapshot reviewSourceSnapshot(const QSqlDatabase &db, const QString &kind,
                                          const QString &ref, qlonglong id, int refVersion,
                                          QString *error)
{
    ReviewSourceSnapshot snapshot;
    if (error) error->clear();
    QSqlQuery q(db);
    if (kind == kReviewKindReflection || kind == kReviewKindConcern) {
        q.prepare(QStringLiteral(
            "SELECT coalesce(note,''),coalesce(topic,''),coalesce(watch,''),state,version "
            "FROM private_reflection WHERE turn_id=?"));
        q.addBindValue(id);
        if (!q.exec()) {
            if (error) *error = QStringLiteral("db_error");
            return snapshot;
        }
        if (!q.next()) return snapshot;
        snapshot.present = true;
        snapshot.state = q.value(3).toString();
        snapshot.version = q.value(4).toInt();
        snapshot.contentHash = reviewHash16(q.value(0).toString() + QChar(31)
                                            + q.value(1).toString() + QChar(31)
                                            + q.value(2).toString());
        return snapshot;
    }
    if (kind == kReviewKindActivity) {
        // R12-1b：失败引用支持新活动 activity/<id>；旧 thought/<id> 行为完全保持。
        ActivityRef activityRef;
        if (!ActivityRef::parse(ref, &activityRef, nullptr)) return snapshot;
        if (activityRef.kind == ActivityRef::InternalActivity) {
            q.prepare(QStringLiteral(
                "SELECT interest,coalesce(result_json,'{}'),version,status "
                "FROM internal_activity WHERE id=?"));
            q.addBindValue(id);
            if (!q.exec()) {
                if (error) *error = QStringLiteral("db_error");
                return snapshot;
            }
            if (!q.next()) return snapshot;
            snapshot.present = true;
            snapshot.internalActivity = true;
            snapshot.version = q.value(2).toInt();
            snapshot.state = q.value(3).toString();
            const auto result = QJsonDocument::fromJson(q.value(1).toByteArray()).object();
            snapshot.contentHash = reviewHash16(
                q.value(0).toString() + QChar(31)
                + snapshot.state + QChar(31)
                + result.value(QStringLiteral("goal")).toString() + QChar(31)
                + result.value(QStringLiteral("progress")).toString() + QChar(31)
                + result.value(QStringLiteral("next_step")).toString() + QChar(31)
                + result.value(QStringLiteral("draft")).toString() + QChar(31)
                + result.value(QStringLiteral("reflection")).toString());
            return snapshot;
        }
        q.prepare(QStringLiteral(
            "SELECT thought,coalesce(thought_result,'{}'),thought_version FROM chat_history "
            "WHERE id=? AND thought IS NOT NULL AND thought<>'' AND sensitive=0"));
        q.addBindValue(id);
        if (!q.exec()) {
            if (error) *error = QStringLiteral("db_error");
            return snapshot;
        }
        if (!q.next()) return snapshot;
        snapshot.present = true;
        snapshot.version = q.value(2).toInt();
        const auto result = QJsonDocument::fromJson(q.value(1).toByteArray()).object();
        snapshot.state = result.value(QStringLiteral("status")).toString(QStringLiteral("active"));
        snapshot.contentHash = reviewHash16(
            q.value(0).toString() + QChar(31)
            + result.value(QStringLiteral("status")).toString() + QChar(31)
            + result.value(QStringLiteral("goal")).toString() + QChar(31)
            + result.value(QStringLiteral("progress")).toString() + QChar(31)
            + result.value(QStringLiteral("draft")).toString());
        return snapshot;
    }
    // open_wake
    q.prepare(QStringLiteral(
        "SELECT state,coalesce(note,''),coalesce(intent_json,''),version,scope_key "
        "FROM open_wake WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec()) {
        if (error) *error = QStringLiteral("db_error");
        return snapshot;
    }
    if (!q.next()) return snapshot;
    snapshot.present = true;
    snapshot.state = q.value(0).toString();
    snapshot.version = q.value(3).toInt();
    snapshot.scopeKey = q.value(4).toString();
    snapshot.contentHash = reviewHash16(q.value(1).toString() + QChar(31) + q.value(2).toString());
    Q_UNUSED(refVersion);
    QSqlQuery schedule(db);
    schedule.prepare(QStringLiteral(
        "SELECT consecutive_failures FROM open_wake_state WHERE scope_key=?"));
    schedule.addBindValue(snapshot.scopeKey);
    if (schedule.exec() && schedule.next())
        snapshot.scheduleFailures = schedule.value(0).toLongLong();
    return snapshot;
}

// 失败可否写回源状态机：只有"已领取"（pending/running/active）的源才可写；
// 源已删除/resolved/dropped/abandoned 时清遗留失败行并报可审计错误码。
QString reviewClaimabilityError(const QString &kind, const ReviewSourceSnapshot &snapshot,
                                int refVersion)
{
    if (!snapshot.present) return QStringLiteral("source_missing");
    if (kind == kReviewKindReflection || kind == kReviewKindConcern)
        return snapshot.state == QStringLiteral("pending") ? QString{}
                                                           : QStringLiteral("source_inactive");
    if (kind == kReviewKindActivity)
        return snapshot.state == QStringLiteral("active") ? QString{}
                                                          : QStringLiteral("source_inactive");
    // open_wake：ref 带版本时必须与 running 行版本一致；否则是迟到回执，不写回状态。
    if (refVersion > 0 && snapshot.version != refVersion)
        return QStringLiteral("cas_conflict");
    return snapshot.state == QStringLiteral("running") ? QString{}
                                                       : QStringLiteral("source_inactive");
}

// R12-1a：关注保留期截止（epoch 秒）= 源 reported + retentionDays。reported 用 SQLite
// strftime 解析，兼容 ISO 'T' 与 'YYYY-MM-DD HH:MM:SS' 两种存量格式；解析失败时
// deadlineEpoch=0，不触发终止（保留原有领取过滤作为兜底）。
struct ConcernRetentionDeadline
{
    bool found = false;
    QString reported;
    qint64 deadlineEpoch = 0;
};

ConcernRetentionDeadline concernRetentionDeadline(const QSqlDatabase &db, qlonglong turn,
                                                  int retentionDays)
{
    ConcernRetentionDeadline deadline;
    if (!db.isOpen() || turn <= 0) return deadline;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT coalesce(s.reported,h.timestamp),"
        "CAST(strftime('%s',coalesce(s.reported,h.timestamp)) AS INTEGER) "
        "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
        "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
        "AND s.input=h.user_input WHERE h.id=?"));
    q.addBindValue(turn);
    if (!q.exec() || !q.next()) return deadline;
    deadline.found = true;
    deadline.reported = q.value(0).toString();
    const qint64 reportedEpoch = q.value(1).toLongLong();
    if (reportedEpoch > 0)
        deadline.deadlineEpoch = reportedEpoch + qint64(qMax(1, retentionDays)) * 86400;
    return deadline;
}

QString retentionReportedText(const ConcernRetentionDeadline &deadline)
{
    const QString reported = deadline.reported.trimmed();
    return reported.isEmpty() ? QStringLiteral("-") : reported;
}

// recordReviewFailure 内（已有事务）使用：终止一条已经越过保留期的关注失败重试。
// 源当前是领取后的 pending：回到 consider、next_check_at=0，保留 note/topic/watch 与记忆
// 事实；同时清掉该来源的 review_failure 行。返回 false 表示 SQL/影响行数异常（调用方回滚）。
bool terminatePendingConcernRetry(const QSqlDatabase &db, qlonglong turn, int expectedVersion,
                                  const QString &ref, qlonglong now)
{
    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM review_failure WHERE source_kind='concern' AND source_ref=?"));
    del.addBindValue(ref);
    if (!del.exec()) return false;
    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE private_reflection SET state='consider',claimed_by=NULL,next_check_at=0,"
        "updated_at=? WHERE turn_id=? AND version=? AND state='pending'"));
    update.addBindValue(now);
    update.addBindValue(turn);
    update.addBindValue(expectedVersion);
    if (!update.exec()) return false;
    // 必须精确命中一行：并发/迟到回执不能把已变化的关注再写坏。
    return update.numRowsAffected() == 1;
}

} // namespace

QString MemoryManager::classifyReviewError(const QString &error) const
{
    const QString text = error.trimmed().toLower();
    if (text.isEmpty()) return QStringLiteral("unknown");
    if (text.contains(QStringLiteral("network")) || text.contains(QStringLiteral("timeout"))
        || text.contains(QStringLiteral("connection")) || text.contains(QStringLiteral("unreachable"))
        || text.contains(QStringLiteral("socket")) || text.contains(QStringLiteral("eof")))
        return QStringLiteral("network_error");
    if (text.contains(QStringLiteral("truncat")) || text.contains(QStringLiteral("empty_content"))
        || text.contains(QStringLiteral("too_long")))
        return QStringLiteral("truncated");
    if (text.contains(QStringLiteral("json")) || text.contains(QStringLiteral("parse")))
        return QStringLiteral("json_parse");
    if (text.contains(QStringLiteral("schema")) || text.contains(QStringLiteral("missing_field")))
        return QStringLiteral("schema");
    if (text.contains(QStringLiteral("cas")) || text.contains(QStringLiteral("conflict"))
        || text.contains(QStringLiteral("stale")) || text.contains(QStringLiteral("version")))
        return QStringLiteral("cas_conflict");
    if (text.contains(QStringLiteral("permission")) || text.contains(QStringLiteral("policy"))
        || text.contains(QStringLiteral("denied")) || text.contains(QStringLiteral("forbidden"))
        || text.contains(QStringLiteral("withheld")))
        return QStringLiteral("permission_denied");
    return QStringLiteral("db_error");
}

QJsonObject MemoryManager::reviewFailure(const QString &sourceKind,
                                         const QString &sourceRef) const
{
    QJsonObject result;
    if (!m_db.isOpen()) return result;
    const QString kind = sourceKind.trimmed();
    const QString ref = sourceRef.trimmed();
    if (!reviewKindValid(kind) || ref.isEmpty()) return result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT consecutive_failures,next_retry_at,suspended_until,last_stage,last_reason,"
        "content_hash,last_attempt_at,updated_at FROM review_failure "
        "WHERE source_kind=? AND source_ref=?"));
    q.addBindValue(kind);
    q.addBindValue(ref);
    if (!q.exec() || !q.next()) return result;
    const qlonglong nextRetry = q.value(1).toLongLong();
    const qlonglong suspended = q.value(2).toLongLong();
    result = QJsonObject{{QStringLiteral("source_kind"), kind},
                         {QStringLiteral("source_ref"), ref},
                         {QStringLiteral("consecutive_failures"), q.value(0).toInt()},
                         {QStringLiteral("next_retry_at"), nextRetry},
                         {QStringLiteral("suspended_until"), suspended},
                         {QStringLiteral("last_stage"), q.value(3).toString()},
                         {QStringLiteral("last_reason"), q.value(4).toString()},
                         {QStringLiteral("content_hash"), q.value(5).toString()},
                         {QStringLiteral("last_attempt_at"), q.value(6).toLongLong()},
                         {QStringLiteral("updated_at"), q.value(7).toLongLong()}};
    result[QStringLiteral("next_retry_iso")] = reviewIsoFromEpoch(nextRetry);
    result[QStringLiteral("suspended_iso")] = reviewIsoFromEpoch(suspended);
    return result;
}

bool MemoryManager::recordReviewFailure(const QString &sourceKind, const QString &sourceRef,
                                        const QString &stage, const QString &reason,
                                        bool countsAsFailure, QString *error,
                                        int expectedVersion)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    const QString kind = sourceKind.trimmed();
    const QString ref = sourceRef.trimmed();
    if (!reviewKindValid(kind) || ref.isEmpty()) return fail(QStringLiteral("invalid_source"));
    qlonglong sourceId = 0;
    int refVersion = 0;
    if (!parseReviewSourceRef(kind, ref, &sourceId, &refVersion))
        return fail(QStringLiteral("invalid_source"));
    // R12-1b：activity 来源必须能区分 thought/<id> 与 activity/<id>，裸 id/未知前缀一律拒绝。
    if (kind == kReviewKindActivity) {
        ActivityRef activityRef;
        if (!ActivityRef::parse(ref, &activityRef, nullptr))
            return fail(QStringLiteral("invalid_source"));
    }
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QString snapshotError;
    const ReviewSourceSnapshot snapshot = reviewSourceSnapshot(m_db, kind, ref, sourceId,
                                                               refVersion, &snapshotError);
    if (!snapshotError.isEmpty()) return fail(QStringLiteral("db_error"));
    // R12-2c-3：迟到失败隔离。每次真实回顾尝试都有独立 run_id/版本；若源当前版本已前进，
    // 说明这份失败来自旧尝试，不得推进新版本来源的失败计数/退避状态。只审计、不写库。
    if (countsAsFailure && expectedVersion > 0 && kind != kReviewKindOpenWake
        && snapshot.present && snapshot.version != expectedVersion) {
        m_db.rollback();
        rollback.dismiss();
        qInfo().noquote()
            << QStringLiteral("[Review] stale_failure_ignored kind=%1 ref=%2 expected=%3"
                              " current=%4 reason=%5")
                   .arg(kind, ref)
                   .arg(expectedVersion)
                   .arg(snapshot.version)
                   .arg(reason);
        return true;
    }
    // 源已删除 / 已 resolved / drop / 已不是可写状态：失败记录不能静默残留，
    // 清掉遗留行并返回明确错误码，由调用方据此写可审计日志。
    const QString nonWritable = countsAsFailure
        ? reviewClaimabilityError(kind, snapshot, refVersion) : QString{};
    if (!snapshot.present || !nonWritable.isEmpty()) {
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral(
            "DELETE FROM review_failure WHERE source_kind=? AND source_ref=?"));
        del.addBindValue(kind);
        del.addBindValue(ref);
        if (!del.exec()) return fail(QStringLiteral("db_error"));
        if (!m_db.commit()) return fail(QStringLiteral("db_error"));
        rollback.dismiss();
        const QString code = snapshot.present ? nonWritable : QStringLiteral("source_missing");
        qWarning() << "[Review] failure_dropped kind=" << kind << "ref=" << ref
                   << "reason=" << code;
        return fail(code);
    }
    QSqlQuery existing(m_db);
    existing.prepare(QStringLiteral(
        "SELECT consecutive_failures,next_retry_at,suspended_until,content_hash "
        "FROM review_failure WHERE source_kind=? AND source_ref=?"));
    existing.addBindValue(kind);
    existing.addBindValue(ref);
    if (!existing.exec()) return fail(QStringLiteral("db_error"));
    int existingFailures = 0;
    qlonglong existingNext = 0;
    qlonglong existingSuspended = 0;
    QString existingHash;
    const bool hadRow = existing.next();
    if (hadRow) {
        existingFailures = existing.value(0).toInt();
        if (!existing.value(1).isNull()) existingNext = existing.value(1).toLongLong();
        if (!existing.value(2).isNull()) existingSuspended = existing.value(2).toLongLong();
        existingHash = existing.value(3).toString();
    }
    existing.finish();
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    int baseFailures = hadRow ? existingFailures : 0;
    if (kind == kReviewKindOpenWake)
        baseFailures = qMax(baseFailures, int(snapshot.scheduleFailures));
    // 来源实质修订检测：内容哈希变化说明"她在意的事已经变了"，旧失败链不再成立，
    // 计数重置为 0 后再计本次。普通领取 bump 的 version 不参与判断（正常重试必须累计）。
    const bool revisionReset = hadRow && countsAsFailure && kind != kReviewKindOpenWake
        && !existingHash.isEmpty() && !snapshot.contentHash.isEmpty()
        && existingHash != snapshot.contentHash;
    if (revisionReset) {
        baseFailures = 0;
        qDebug() << "[Review] failure_revision_reset kind=" << kind << "ref=" << ref;
    }
    int failures = existingFailures;
    qlonglong nextRetry = existingNext;
    qlonglong suspended = existingSuspended;
    QString contentHash = existingHash;
    if (countsAsFailure) {
        failures = baseFailures + 1;
        const auto options = ConfigManager::instance().getProactiveConfig();
        const int maxFailures = qMax(1, options.maxReviewFailures);
        if (failures >= maxFailures) {
            // 达到上限：暂停 24h，next_retry 与 suspended_until 对齐。
            suspended = now + 86400;
            nextRetry = suspended;
        } else {
            const QList<int> &backoff = options.reviewBackoffSeconds;
            const int index = qMin(failures - 1,
                                   backoff.isEmpty() ? 0 : int(backoff.size()) - 1);
            const int seconds = backoff.isEmpty() ? 120 : backoff.at(index);
            nextRetry = now + seconds;
            suspended = 0;
        }
        contentHash = snapshot.contentHash;
    }
    // R12-1a：关注保留期终止定时重试。失败会把 next_retry_at 写回 next_check_at；如果
    // 该重试已越过 reported+retention（或当前时间已越过），就不再保留定时重试资格：
    // 单事务清 failure 行 + next_check_at=0，state 保持 consider，note/topic/fact 不动。
    // 后续合法新证据仍可按 matchedConcerns 命中/更新。返回 false + error=retry_expired
    // 作为可审计结果（不是普通认知失败，不保留失败行）。
    if (countsAsFailure && kind == kReviewKindConcern) {
        const int retentionDays = qBound(
            1, ConfigManager::instance().getProactiveConfig().concernRetentionDays, 365);
        const auto deadline = concernRetentionDeadline(m_db, sourceId, retentionDays);
        // 同时检查本次计划的重试时间与既有失败行（可能已被上一次失败排到截止之后）。
        const bool retryBeyondDeadline = deadline.deadlineEpoch > 0
            && (now >= deadline.deadlineEpoch
                || nextRetry >= deadline.deadlineEpoch
                || existingNext >= deadline.deadlineEpoch
                || existingSuspended >= deadline.deadlineEpoch);
        if (retryBeyondDeadline) {
            if (!terminatePendingConcernRetry(m_db, sourceId, snapshot.version, ref, now))
                return fail(QStringLiteral("db_error"));
            if (!m_db.commit()) return fail(QStringLiteral("db_error"));
            rollback.dismiss();
            qInfo().noquote()
                << QStringLiteral("[Review] retry_expired kind=concern ref=%1 reported=%2"
                                  " retention_days=%3")
                       .arg(ref, retentionReportedText(deadline))
                       .arg(retentionDays);
            return fail(QStringLiteral("retry_expired"));
        }
    }
    QSqlQuery upsert(m_db);
    upsert.prepare(QStringLiteral(
        "INSERT INTO review_failure(source_kind,source_ref,consecutive_failures,next_retry_at,"
        "suspended_until,last_stage,last_reason,content_hash,last_attempt_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(source_kind,source_ref) DO UPDATE SET "
        "consecutive_failures=excluded.consecutive_failures,"
        "next_retry_at=excluded.next_retry_at,"
        "suspended_until=excluded.suspended_until,"
        "last_stage=excluded.last_stage,last_reason=excluded.last_reason,"
        "content_hash=excluded.content_hash,last_attempt_at=excluded.last_attempt_at,"
        "updated_at=excluded.updated_at"));
    upsert.addBindValue(kind);
    upsert.addBindValue(ref);
    upsert.addBindValue(failures);
    upsert.addBindValue(nextRetry > 0 ? QVariant(nextRetry) : QVariant());
    upsert.addBindValue(suspended > 0 ? QVariant(suspended) : QVariant());
    upsert.addBindValue(stage.trimmed().left(120));
    upsert.addBindValue(reason.trimmed().left(240));
    upsert.addBindValue(contentHash.isEmpty() ? QVariant() : QVariant(contentHash));
    upsert.addBindValue(now);
    upsert.addBindValue(now);
    if (!upsert.exec()) return fail(QStringLiteral("db_error"));
    if (countsAsFailure) {
        // 失败行与源状态机同事务写回；每步影响行数必须为 1，否则整体失败回滚，
        // 绝不出现"失败计数更新了、源却没被延后"的半写状态。
        QSqlQuery source(m_db);
        int affected = 0;
        if (kind == kReviewKindReflection) {
            source.prepare(QStringLiteral(
                "UPDATE private_reflection SET state='deferred',claimed_by=NULL,"
                "eligible_at=?,updated_at=? WHERE turn_id=? AND version=? AND state='pending'"));
            source.addBindValue(nextRetry);
            source.addBindValue(now);
            source.addBindValue(sourceId);
            source.addBindValue(snapshot.version);
            if (!source.exec()) return fail(QStringLiteral("db_error"));
            affected = source.numRowsAffected();
        } else if (kind == kReviewKindConcern) {
            source.prepare(QStringLiteral(
                "UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                "next_check_at=?,eligible_at=?,updated_at=? "
                "WHERE turn_id=? AND version=? AND state='pending'"));
            source.addBindValue(nextRetry);
            source.addBindValue(nextRetry);
            source.addBindValue(now);
            source.addBindValue(sourceId);
            source.addBindValue(snapshot.version);
            if (!source.exec()) return fail(QStringLiteral("db_error"));
            affected = source.numRowsAffected();
        } else if (kind == kReviewKindActivity) {
            if (snapshot.internalActivity) {
                // R12-1b：新活动失败退避写回 internal_activity；版本 +1、状态不变，
                // review_after 与 legacy 共用同一退避序列（不允许新活动绕开）。
                source.prepare(QStringLiteral(
                    "UPDATE internal_activity SET review_after=?,"
                    "result_json=json_set(coalesce(result_json,'{}'),'$.deferred_reason',?),"
                    "version=version+1,updated_at=? "
                    "WHERE id=? AND version=? AND status='active'"));
                source.addBindValue(nextRetry);
                source.addBindValue(reason.trimmed().left(240));
                source.addBindValue(now);
                source.addBindValue(sourceId);
                source.addBindValue(snapshot.version);
            } else {
                source.prepare(QStringLiteral(
                    "UPDATE chat_history SET review_after=?,"
                    "thought_result=json_set(coalesce(thought_result,'{}'),'$.deferred_reason',?),"
                    "thought_version=thought_version+1 "
                    "WHERE id=? AND thought_version=? AND thought IS NOT NULL AND sensitive=0 "
                    "AND coalesce(json_extract(thought_result,'$.status'),'active')='active'"));
                source.addBindValue(nextRetry);
                source.addBindValue(reason.trimmed().left(240));
                source.addBindValue(sourceId);
                source.addBindValue(snapshot.version);
            }
            if (!source.exec()) return fail(QStringLiteral("db_error"));
            affected = source.numRowsAffected();
        } else {
            source.prepare(QStringLiteral(
                "UPDATE open_wake SET state='deferred',lease_until=NULL,next_due_at=?,"
                "finished_at=?,error_code=? WHERE id=? AND version=? AND state='running'"));
            source.addBindValue(nextRetry);
            source.addBindValue(now);
            source.addBindValue(reason.trimmed().left(120));
            source.addBindValue(sourceId);
            source.addBindValue(snapshot.version);
            if (!source.exec()) return fail(QStringLiteral("db_error"));
            affected = source.numRowsAffected();
            if (affected == 1) {
                // 调度状态一并推到 next_retry，避免 deferred 行立刻又被领取。
                QSqlQuery scheduling(m_db);
                scheduling.prepare(QStringLiteral(
                    "UPDATE open_wake_state SET next_due_at=?,suspended_until=?,"
                    "consecutive_failures=?,version=version+1,updated_at=? WHERE scope_key=?"));
                scheduling.addBindValue(nextRetry);
                scheduling.addBindValue(suspended > 0 ? QVariant(suspended) : QVariant());
                scheduling.addBindValue(failures);
                scheduling.addBindValue(now);
                scheduling.addBindValue(snapshot.scopeKey);
                if (!scheduling.exec()) return fail(QStringLiteral("db_error"));
                if (scheduling.numRowsAffected() != 1)
                    return fail(QStringLiteral("cas_conflict"));
            }
        }
        if (affected != 1) return fail(QStringLiteral("cas_conflict"));
    }
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    qDebug() << "[Review] failure kind=" << kind << "ref=" << ref
             << "stage=" << stage << "reason=" << reason
             << "counts=" << countsAsFailure << "failures=" << failures
             << "next_retry=" << nextRetry << "suspended=" << suspended;
    return true;
}

bool MemoryManager::clearReviewFailure(const QString &sourceKind, const QString &sourceRef)
{
    if (!m_db.isOpen()) return false;
    const QString kind = sourceKind.trimmed();
    const QString ref = sourceRef.trimmed();
    if (!reviewKindValid(kind) || ref.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM review_failure WHERE source_kind=? AND source_ref=?"));
    q.addBindValue(kind);
    q.addBindValue(ref);
    return q.exec();
}

bool MemoryManager::resetReviewFailure(const QString &sourceKind, const QString &sourceRef)
{
    if (!m_db.isOpen()) return false;
    const QString kind = sourceKind.trimmed();
    const QString ref = sourceRef.trimmed();
    if (!reviewKindValid(kind) || ref.isEmpty()) return false;
    qlonglong sourceId = 0;
    int refVersion = 0;
    if (!parseReviewSourceRef(kind, ref, &sourceId, &refVersion)) return false;
    if (!m_db.transaction()) return false;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM review_failure WHERE source_kind=? AND source_ref=?"));
    q.addBindValue(kind);
    q.addBindValue(ref);
    if (!q.exec()) return false;
    if (kind == kReviewKindReflection) {
        q.prepare(QStringLiteral(
            "UPDATE private_reflection SET eligible_at=unixepoch(),updated_at=unixepoch() "
            "WHERE turn_id=?"));
        q.addBindValue(sourceId);
    } else if (kind == kReviewKindConcern) {
        q.prepare(QStringLiteral(
            "UPDATE private_reflection SET eligible_at=unixepoch(),next_check_at=unixepoch(),"
            "updated_at=unixepoch() WHERE turn_id=?"));
        q.addBindValue(sourceId);
    } else if (kind == kReviewKindActivity) {
        // R12-1b：activity 来源可能是 legacy(thought/<id>) 或 internal(activity/<id>)；
        // 重置都要让源立即可领取，但不能写错存储。
        ActivityRef activityRef;
        if (!ActivityRef::parse(ref, &activityRef, nullptr)) return false;
        if (activityRef.kind == ActivityRef::InternalActivity) {
            q.prepare(QStringLiteral(
                "UPDATE internal_activity SET review_after=0,updated_at=unixepoch() WHERE id=?"));
        } else {
            q.prepare(QStringLiteral("UPDATE chat_history SET review_after=0 WHERE id=?"));
        }
        q.addBindValue(sourceId);
    } else {
        q.prepare(QStringLiteral(
            "UPDATE open_wake SET next_due_at=unixepoch() WHERE id=? AND state IN ('running','deferred')"));
        q.addBindValue(sourceId);
        if (!q.exec()) return false;
        q.prepare(QStringLiteral(
            "UPDATE open_wake_state SET next_due_at=unixepoch(),suspended_until=NULL,"
            "consecutive_failures=0,version=version+1,updated_at=unixepoch() "
            "WHERE scope_key=(SELECT scope_key FROM open_wake WHERE id=?)"));
        q.addBindValue(sourceId);
    }
    if (!q.exec()) return false;
    if (!m_db.commit()) return false;
    rollback.dismiss();
    return true;
}

int MemoryManager::expireConcernScheduledRetries(int retentionDays)
{
    if (!m_db.isOpen()) return -1;
    const int days = qBound(1, retentionDays, 365);
    const qint64 retentionSeconds = qint64(days) * 86400;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // 与 claimDueConcern 相同的来源资格口径；只扫描 state='consider' 且仍有定时检查
    // （next_check_at>0）的关注。两类越过保留期的情况都终止：
    //   1) 当前时间已越过 reported+retention；
    //   2) 失败行把 next_retry_at / suspended_until 排到了 reported+retention 之后
    //      （即使还没到期，也没有再等的意义）。
    QSqlQuery scan(m_db);
    scan.prepare(QStringLiteral(
        "SELECT r.turn_id,coalesce(s.reported,h.timestamp),r.version "
        "FROM chat_history h JOIN cognitive_source s ON s.event=h.cognitive_event "
        "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
        "AND s.input=h.user_input "
        "JOIN private_reflection r ON r.turn_id=h.id "
        "WHERE h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
        "AND h.turn_state IN ('handed_off','quiet') AND h.turn_kind='reply' "
        "AND r.state='consider' AND r.next_check_at>0 "
        "AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id) "
        "AND (julianday(coalesce(s.reported,h.timestamp)) < julianday('now', ?) "
        "  OR EXISTS (SELECT 1 FROM review_failure f WHERE f.source_kind='concern' "
        "     AND f.source_ref='concern/' || r.turn_id "
        "     AND ((f.next_retry_at IS NOT NULL AND f.next_retry_at > "
        "           CAST(strftime('%s',coalesce(s.reported,h.timestamp)) AS INTEGER) + ?) "
        "       OR (f.suspended_until IS NOT NULL AND f.suspended_until > "
        "           CAST(strftime('%s',coalesce(s.reported,h.timestamp)) AS INTEGER) + ?)))) "
        "ORDER BY r.next_check_at,r.turn_id LIMIT 64"));
    scan.addBindValue(QStringLiteral("-%1 day").arg(days));
    scan.addBindValue(retentionSeconds);
    scan.addBindValue(retentionSeconds);
    if (!scan.exec()) {
        qWarning() << "[Review] retry_expired_scan_failed" << scan.lastError();
        return -1;
    }
    struct Expired
    {
        qlonglong turn = 0;
        QString reported;
        int version = 0;
    };
    QList<Expired> rows;
    while (scan.next()) {
        Expired row;
        row.turn = scan.value(0).toLongLong();
        row.reported = scan.value(1).toString();
        row.version = scan.value(2).toInt();
        rows.append(row);
    }
    scan.finish();
    if (rows.isEmpty()) return 0;
    if (!m_db.transaction()) return -1;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    int expired = 0;
    for (const auto &row : rows) {
        // 单事务：先清失败行，再把定时检查终止为"只在新证据时再看"。
        // 只改 next_check_at/updated_at，state 保持 consider，note/topic/watch 与事实不动。
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral(
            "DELETE FROM review_failure WHERE source_kind='concern' AND source_ref=?"));
        del.addBindValue(QStringLiteral("concern/%1").arg(row.turn));
        if (!del.exec()) return -1;
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral(
            "UPDATE private_reflection SET next_check_at=0,updated_at=? "
            "WHERE turn_id=? AND version=? AND state='consider' AND next_check_at>0"));
        update.addBindValue(now);
        update.addBindValue(row.turn);
        update.addBindValue(row.version);
        if (!update.exec()) return -1;
        // 并发变化（已被别的路径领取/修订）：跳过，不写坏；下次扫描再判断。
        if (update.numRowsAffected() != 1) continue;
        ++expired;
        const QString reported = row.reported.trimmed();
        qInfo().noquote()
            << QStringLiteral("[Review] retry_expired kind=concern ref=concern/%1 reported=%2"
                              " retention_days=%3")
                   .arg(row.turn)
                   .arg(reported.isEmpty() ? QStringLiteral("-") : reported)
                   .arg(days);
    }
    if (!m_db.commit()) return -1;
    rollback.dismiss();
    if (expired > 0)
        qInfo() << "[Review] retry_expired_count kind=concern count=" << expired;
    return expired;
}

QJsonObject MemoryManager::reviewBudgetUsage() const
{
    QJsonObject result;
    if (!m_db.isOpen()) return result;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT coalesce(source_kind,'legacy'),count(*) FROM thought_review_log "
            "WHERE date(started_at,'unixepoch','localtime')=date('now','localtime') "
            "GROUP BY coalesce(source_kind,'legacy')")))
        return result;
    QJsonObject bySource;
    int total = 0;
    while (q.next()) {
        const int count = q.value(1).toInt();
        bySource.insert(q.value(0).toString(), count);
        total += count;
    }
    result[QStringLiteral("date")] = QDate::currentDate().toString(Qt::ISODate);
    result[QStringLiteral("total")] = total;
    result[QStringLiteral("by_source")] = bySource;
    result[QStringLiteral("open_wake")] = bySource.value(QStringLiteral("open_wake")).toInt();
    return result;
}

bool MemoryManager::bumpOpenWakeFairness()
{
    QSqlQuery q(m_db);
    // 只按"成功领取的任务"计数：调用方只在 claim 事务里、成功后调用。
    if (!q.exec(QStringLiteral(
            "UPDATE open_wake_state SET non_open_since_due=non_open_since_due+1,"
            "updated_at=unixepoch() WHERE next_due_at<=unixepoch() "
            "AND (suspended_until IS NULL OR suspended_until<=unixepoch())"))) {
        // 迁移未完成（测试/旧库回退重放）时公平计数表可能不存在：不阻断领取主路径。
        qWarning() << "[OpenWake] fairness bump failed" << q.lastError();
        return true;
    }
    return true;
}

bool MemoryManager::recoverSuspendedFailure(const QString &sourceKind, const QString &sourceRef)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE review_failure SET consecutive_failures=0,suspended_until=NULL,"
        "next_retry_at=unixepoch(),updated_at=unixepoch() "
        "WHERE source_kind=? AND source_ref=? AND suspended_until IS NOT NULL "
        "AND suspended_until<=unixepoch()"));
    q.addBindValue(sourceKind);
    q.addBindValue(sourceRef);
    if (!q.exec()) {
        // 迁移未完成（测试/旧库回退重放）时失败表可能不存在：不阻断领取主路径。
        qWarning() << "[Review] suspended recovery failed" << sourceKind << sourceRef
                   << q.lastError();
        return true;
    }
    if (q.numRowsAffected() > 0)
        qInfo() << "[Review] suspended_recovered kind=" << sourceKind << "ref=" << sourceRef;
    return true;
}

QJsonObject MemoryManager::openWakeState(const QString &scopeKey) const
{
    QJsonObject result;
    if (!m_db.isOpen() || scopeKey.trimmed().isEmpty()) return result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT next_due_at,last_started_at,last_finished_at,last_outcome,last_wake_id,"
        "consecutive_failures,non_open_since_due,suspended_until,version,updated_at "
        "FROM open_wake_state WHERE scope_key=?"));
    q.addBindValue(scopeKey);
    if (!q.exec() || !q.next()) return result;
    result = QJsonObject{{QStringLiteral("scope_key"), scopeKey},
                         {QStringLiteral("next_due_at"), q.value(0).toLongLong()},
                         {QStringLiteral("last_started_at"), q.value(1).toLongLong()},
                         {QStringLiteral("last_finished_at"), q.value(2).toLongLong()},
                         {QStringLiteral("last_outcome"), q.value(3).toString()},
                         {QStringLiteral("last_wake_id"), q.value(4).toLongLong()},
                         {QStringLiteral("consecutive_failures"), q.value(5).toInt()},
                         {QStringLiteral("non_open_since_due"), q.value(6).toInt()},
                         {QStringLiteral("suspended_until"), q.value(7).toLongLong()},
                         {QStringLiteral("version"), q.value(8).toInt()},
                         {QStringLiteral("updated_at"), q.value(9).toLongLong()}};
    result[QStringLiteral("next_due_iso")] = reviewIsoFromEpoch(q.value(0).toLongLong());
    result[QStringLiteral("suspended_iso")] = reviewIsoFromEpoch(q.value(7).toLongLong());
    return result;
}

bool MemoryManager::ensureOpenWakeScheduled(const QString &scopeKey, int initialDelaySeconds,
                                            QString *error)
{
    if (error) error->clear();
    if (!m_db.isOpen()) {
        if (error) *error = QStringLiteral("db_error");
        return false;
    }
    const QString scope = scopeKey.trimmed();
    if (scope.isEmpty()) {
        if (error) *error = QStringLiteral("invalid_scope");
        return false;
    }
    const int delay = qBound(0, initialDelaySeconds, 7 * 24 * 3600);
    // 幂等：已存在调度行时保持原 due，不因重复启动把下一次唤醒推迟/提前。
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO open_wake_state(scope_key,next_due_at,updated_at) "
        "VALUES(?,unixepoch()+?,unixepoch()) ON CONFLICT(scope_key) DO NOTHING"));
    q.addBindValue(scope);
    q.addBindValue(delay);
    if (!q.exec()) {
        if (error) *error = QStringLiteral("db_error");
        return false;
    }
    return true;
}

QJsonObject MemoryManager::claimOpenWake(int dailyLimit, int cooldownSeconds,
                                         int openWakeDailyLimit, int sharedBudgetPercent,
                                         int maxConsecutiveFailures, QString *reason,
                                         QDateTime *nextEligibleAt, const QString &scopeKey)
{
    if (reason) reason->clear();
    if (nextEligibleAt) *nextEligibleAt = QDateTime{};
    const auto fail = [reason](const QString &code) {
        if (reason) *reason = code;
        return QJsonObject{};
    };
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    const QString scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner")
                                                       : scopeKey.trimmed();
    // 独立每日预算：min(openWakeDailyLimit, dailyLimit*sharedBudgetPercent/100)。
    // 取整至少 1（正预算不能因整除被永久饿死），但绝不突破共享 dailyLimit。
    int cap = 0;
    if (dailyLimit > 0 && openWakeDailyLimit > 0) {
        const int share = qMax(1, dailyLimit * qMax(0, sharedBudgetPercent) / 100);
        cap = qMin(openWakeDailyLimit, qMin(dailyLimit, share));
    }
    if (cap <= 0) return fail(QStringLiteral("budget"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT next_due_at,consecutive_failures,suspended_until,version FROM open_wake_state "
        "WHERE scope_key=?"));
    q.addBindValue(scope);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (!q.next()) return fail(QStringLiteral("no_candidate"));
    const qlonglong dueAt = q.value(0).toLongLong();
    const int scheduleFailures = q.value(1).toInt();
    const qlonglong suspendedUntil = q.value(2).isNull() ? 0 : q.value(2).toLongLong();
    const qint64 contextEpoch = qint64(q.value(3).toInt()) + 1;
    q.finish();
    // 同一 scope 只允许一个 running：租约仍有效 → busy；租约已过期 → 就地标记 interrupted，
    // 保证唯一索引不会挡住本次领取（等价于一次轻量 recover；重启恢复仍由 recoverOpenWakeLeases 负责）。
    QSqlQuery running(m_db);
    running.prepare(QStringLiteral(
        "SELECT id,lease_until FROM open_wake WHERE scope_key=? AND state='running' "
        "ORDER BY id DESC LIMIT 1"));
    running.addBindValue(scope);
    if (!running.exec()) return fail(QStringLiteral("db_error"));
    if (running.next()) {
        const qlonglong staleWakeId = running.value(0).toLongLong();
        const qlonglong lease = running.value(1).isNull() ? 0 : running.value(1).toLongLong();
        running.finish();
        if (lease > now) {
            if (nextEligibleAt)
                *nextEligibleAt = QDateTime::fromSecsSinceEpoch(lease).toUTC();
            return fail(QStringLiteral("busy"));
        }
        QSqlQuery interrupt(m_db);
        interrupt.prepare(QStringLiteral(
            "UPDATE open_wake SET state='interrupted',lease_until=NULL,finished_at=?,"
            "error_code='lease_expired',next_due_at=? WHERE id=? AND state='running'"));
        interrupt.addBindValue(now);
        interrupt.addBindValue(now);
        interrupt.addBindValue(staleWakeId);
        if (!interrupt.exec()) return fail(QStringLiteral("db_error"));
        qWarning() << "[OpenWake] stale_running_interrupted id=" << staleWakeId
                   << "scope=" << scope;
    } else {
        running.finish();
    }
    if (suspendedUntil > now) {
        if (nextEligibleAt)
            *nextEligibleAt = QDateTime::fromSecsSinceEpoch(suspendedUntil).toUTC();
        return fail(QStringLiteral("suspended"));
    }
    // 暂停到期：允许一次恢复尝试，连续失败清零（last_outcome 等审计字段保留）。
    const bool recovering = suspendedUntil > 0 && suspendedUntil <= now;
    const int effectiveFailures = recovering ? 0 : scheduleFailures;
    if (recovering)
        qInfo() << "[OpenWake] suspended_recovered scope=" << scope
                << "failures=" << scheduleFailures;
    if (maxConsecutiveFailures > 0 && effectiveFailures >= maxConsecutiveFailures)
        return fail(QStringLiteral("suspended"));
    if (dueAt > now) {
        if (nextEligibleAt)
            *nextEligibleAt = QDateTime::fromSecsSinceEpoch(dueAt).toUTC();
        return fail(QStringLiteral("not_due"));
    }
    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString chargeReason;
    QDateTime chargeNext;
    if (!chargePrivateReview(0, dailyLimit, cooldownSeconds, &chargeReason, &chargeNext,
                             QStringLiteral("open_wake"), QString(), runId, cap)) {
        if (nextEligibleAt) *nextEligibleAt = chargeNext;
        if (chargeReason == QStringLiteral("cooldown")) return fail(QStringLiteral("cooldown"));
        if (chargeReason == QStringLiteral("db_error")) return fail(QStringLiteral("db_error"));
        return fail(QStringLiteral("budget"));
    }
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO open_wake(scope_key,run_id,version,state,trigger_reason,created_at,"
        "started_at,lease_until,next_due_at,current_context_epoch) "
        "VALUES(?,?,1,'running','due',?,?,?,?,?)"));
    insert.addBindValue(scope);
    insert.addBindValue(runId);
    insert.addBindValue(now);
    insert.addBindValue(now);
    insert.addBindValue(now + 1800);
    insert.addBindValue(dueAt);
    insert.addBindValue(contextEpoch);
    if (!insert.exec()) return fail(QStringLiteral("db_error"));
    const qlonglong wakeId = insert.lastInsertId().toLongLong();
    QSqlQuery schedule(m_db);
    schedule.prepare(QStringLiteral(
        "UPDATE open_wake_state SET last_started_at=?,version=version+1,updated_at=?,"
        "non_open_since_due=0,last_wake_id=?,consecutive_failures=?,suspended_until=NULL "
        "WHERE scope_key=?"));
    schedule.addBindValue(now);
    schedule.addBindValue(now);
    schedule.addBindValue(wakeId);
    schedule.addBindValue(effectiveFailures);
    schedule.addBindValue(scope);
    if (!schedule.exec()) return fail(QStringLiteral("db_error"));
    if (schedule.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    qInfo() << "[OpenWake] claimed id=" << wakeId << "scope=" << scope
            << "run=" << runId << "cap=" << cap << "failures=" << effectiveFailures;
    return QJsonObject{{QStringLiteral("id"), wakeId},
                       {QStringLiteral("version"), 1},
                       {QStringLiteral("run_id"), runId},
                       {QStringLiteral("scope_key"), scope},
                       {QStringLiteral("next_due_at"), dueAt},
                       {QStringLiteral("current_context_epoch"), contextEpoch},
                       {QStringLiteral("state"), QStringLiteral("running")}};
}

bool MemoryManager::finishOpenWake(qlonglong id, int version, const QString &decision,
                                   const QString &note, const QJsonObject &intent,
                                   int nextWakeMinutes, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (decision != QStringLiteral("rest") && decision != QStringLiteral("reflect")
        && decision != QStringLiteral("contact"))
        return fail(QStringLiteral("invalid_decision"));
    const QString trimmedNote = note.trimmed();
    if (trimmedNote.size() > 240) return fail(QStringLiteral("invalid_note"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT scope_key FROM open_wake WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (!q.next()) return fail(QStringLiteral("cas_conflict"));
    const QString scope = q.value(0).toString();
    q.finish();
    // nextWakeMinutes 由调用方按分钟给出；内存层夹紧 5..1440（0 按最小值 5 处理）。
    const int minutes = qBound(5, nextWakeMinutes, 1440);
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    const qlonglong nextDue = now + qint64(minutes) * 60;
    const QString intentJson =
        QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    q.prepare(QStringLiteral(
        "UPDATE open_wake SET state='committed',decision=?,note=?,intent_json=?,"
        "finished_at=?,lease_until=NULL,next_due_at=? "
        "WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(decision);
    q.addBindValue(trimmedNote);
    q.addBindValue(intentJson);
    q.addBindValue(now);
    q.addBindValue(nextDue);
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (q.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    q.prepare(QStringLiteral(
        "UPDATE open_wake_state SET next_due_at=?,last_finished_at=?,last_outcome=?,"
        "version=version+1,updated_at=?,suspended_until=NULL,consecutive_failures=0 "
        "WHERE scope_key=?"));
    q.addBindValue(nextDue);
    q.addBindValue(now);
    q.addBindValue(decision);
    q.addBindValue(now);
    q.addBindValue(scope);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (q.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    // 成功提交（rest 与 reflect 都算成功）时清掉该 wake 的失败记录。
    q.prepare(QStringLiteral(
        "DELETE FROM review_failure WHERE source_kind='open_wake' AND source_ref IN (?,?)"));
    q.addBindValue(QStringLiteral("wake/%1:%2").arg(id).arg(version));
    q.addBindValue(QStringLiteral("wake/%1").arg(id));
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    qInfo() << "[OpenWake] finished id=" << id << "decision=" << decision
            << "next_due=" << nextDue;
    return true;
}

bool MemoryManager::deferOpenWake(qlonglong id, int version, int retrySeconds,
                                  const QString &reason)
{
    if (!m_db.isOpen()) return false;
    const int retry = qBound(0, retrySeconds, 7 * 24 * 3600);
    if (!m_db.transaction()) return false;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT scope_key FROM open_wake WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return false;
    const QString scope = q.value(0).toString();
    q.finish();
    q.prepare(QStringLiteral(
        "UPDATE open_wake SET state='deferred',next_due_at=?,finished_at=?,lease_until=NULL,"
        "error_code=? WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(now + retry);
    q.addBindValue(now);
    q.addBindValue(reason.trimmed().left(120));
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected() != 1) return false;
    // 主动让出/用户插话不增加失败计数，只把调度推到（可立即）再试。
    q.prepare(QStringLiteral(
        "UPDATE open_wake_state SET next_due_at=?,version=version+1,updated_at=? "
        "WHERE scope_key=?"));
    q.addBindValue(now + retry);
    q.addBindValue(now);
    q.addBindValue(scope);
    if (!q.exec() || q.numRowsAffected() != 1) return false;
    if (!m_db.commit()) return false;
    rollback.dismiss();
    qInfo() << "[OpenWake] deferred id=" << id << "retry_seconds=" << retry
            << "reason=" << reason;
    return true;
}

bool MemoryManager::cancelOpenWake(qlonglong id, int version, const QString &reason)
{
    if (!m_db.isOpen()) return false;
    if (!m_db.transaction()) return false;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT scope_key FROM open_wake WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return false;
    const QString scope = q.value(0).toString();
    q.finish();
    // 取消是终态：不增加失败计数，也不改下一次到期（由调度方决定后续节奏）。
    q.prepare(QStringLiteral(
        "UPDATE open_wake SET state='cancelled',finished_at=?,lease_until=NULL,error_code=? "
        "WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(now);
    q.addBindValue(reason.trimmed().left(120));
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected() != 1) return false;
    q.prepare(QStringLiteral(
        "UPDATE open_wake_state SET last_finished_at=?,last_outcome='cancelled',"
        "version=version+1,updated_at=? WHERE scope_key=?"));
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(scope);
    if (!q.exec() || q.numRowsAffected() != 1) return false;
    if (!m_db.commit()) return false;
    rollback.dismiss();
    qInfo() << "[OpenWake] cancelled id=" << id << "reason=" << reason;
    return true;
}

int MemoryManager::recoverOpenWakeLeases(int staleSeconds)
{
    if (staleSeconds < 0) staleSeconds = 0;
    if (!m_db.isOpen()) return 0;
    if (!m_db.transaction()) return 0;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    const QString staleCondition = QStringLiteral(
        "state='running' AND (lease_until IS NULL OR lease_until < unixepoch()-%1)")
        .arg(staleSeconds);
    QSqlQuery scopes(m_db);
    if (!scopes.exec(QStringLiteral("SELECT DISTINCT scope_key FROM open_wake WHERE ")
                     + staleCondition))
        return 0;
    QStringList scopeKeys;
    while (scopes.next()) scopeKeys.append(scopes.value(0).toString());
    scopes.finish();
    if (scopeKeys.isEmpty())
        return 0;
    QSqlQuery q(m_db);
    // 重启恢复只做一次：running → interrupted，下一次到期拨到 now，由调度方重新领取。
    q.prepare(QStringLiteral(
        "UPDATE open_wake SET state='interrupted',lease_until=NULL,finished_at=?,"
        "error_code='lease_expired',next_due_at=? WHERE ") + staleCondition);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) return 0;
    const int recovered = q.numRowsAffected();
    for (const QString &scope : scopeKeys) {
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral(
            "UPDATE open_wake_state SET next_due_at=?,version=version+1,updated_at=? "
            "WHERE scope_key=?"));
        update.addBindValue(now);
        update.addBindValue(now);
        update.addBindValue(scope);
        if (!update.exec()) return 0;
    }
    if (!m_db.commit()) return 0;
    rollback.dismiss();
    if (recovered > 0)
        qInfo() << "[OpenWake] leases_recovered=" << recovered;
    return recovered;
}

QJsonArray MemoryManager::recentOpenWakeNotes(int limit, const QString &scopeKey) const
{
    QJsonArray result;
    if (!m_db.isOpen() || limit <= 0) return result;
    const QString scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner")
                                                       : scopeKey.trimmed();
    const int capped = qMin(limit, 20);
    QSqlQuery q(m_db);
    // R12-1a：按 scope_key 过滤，默认只读 owner 私聊域；其它 scope（群聊/第三方/其它会话）
    // 的内部记录不会被带进前台或回顾快照，也不扩大现有读取范围。
    q.prepare(QStringLiteral(
        "SELECT id,version,decision,note,finished_at FROM open_wake "
        "WHERE scope_key=? AND state='committed' AND decision IN ('rest','reflect') "
        "AND note IS NOT NULL AND trim(note)<>'' "
        "ORDER BY coalesce(finished_at,0) DESC,id DESC LIMIT ?"));
    q.addBindValue(scope);
    q.addBindValue(capped);
    if (!q.exec()) return result;
    while (q.next()) {
        const qlonglong id = q.value(0).toLongLong();
        const int version = q.value(1).toInt();
        const qlonglong finishedAt = q.value(4).toLongLong();
        result.append(QJsonObject{
            {QStringLiteral("ref"), QStringLiteral("wake/%1:%2").arg(id).arg(version)},
            {QStringLiteral("version"), version},
            {QStringLiteral("note"), q.value(3).toString()},
            {QStringLiteral("decision"), q.value(2).toString()},
            {QStringLiteral("at"), reviewIsoFromEpoch(finishedAt)},
            {QStringLiteral("finished_at"), finishedAt}});
    }
    return result;
}

bool MemoryManager::hasActiveInternalActivity() const
{
    if (!m_db.isOpen()) return false;
    // legacy：chat_history 的 active/paused 念头。
    QSqlQuery legacy(m_db);
    if (legacy.exec(QStringLiteral(
            "SELECT 1 FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
            "AND sensitive=0 AND coalesce(json_extract(thought_result,'$.status'),'active') "
            "IN ('active','paused') LIMIT 1"))
        && legacy.next())
        return true;
    // internal：v31 骨架表（本片只读）。
    QSqlQuery internal(m_db);
    if (internal.exec(QStringLiteral(
            "SELECT 1 FROM internal_activity WHERE status IN ('active','paused') LIMIT 1"))
        && internal.next())
        return true;
    return false;
}

// ============================ R12-1b：统一活动接口 ============================
// 这一片把"活动"从两处存储统一成一条访问/生命周期契约：
//   * legacy：chat_history 的 thought/thought_result（ref=thought/<id>）是 R12-1b 之前
//     唯一的活动存储；这里只做读取适配、复用旧 CAS/预算/投递语义，不迁移、不复制聊天行；
//   * internal：v31 的 internal_activity（ref=activity/<id>）是新运行时使用的存储；
//     新建活动不写 chat_history/检索索引，不伪造完成状态，不建 Outbox/ledger。
// 两张表可以有相同的整数 id，必须通过 ActivityRef 带前缀解析，裸 id 不是身份。
namespace {

// epoch 秒 -> UTC ISO；0 表示"没有时间"（不输出空字符串以外的伪造时间）。
QString activityIsoFromEpoch(qlonglong seconds)
{
    return seconds > 0
        ? QDateTime::fromSecsSinceEpoch(seconds).toUTC().toString(Qt::ISODateWithMs)
        : QString{};
}

// 兼容 thought_result.updated_at（UTC ISO）与 chat_history.timestamp（本地 datetime 文本）。
qlonglong activityEpochFromText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return 0;
    QDateTime value = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!value.isValid()) value = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!value.isValid())
        value = QDateTime::fromString(trimmed, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    return value.isValid() ? value.toSecsSinceEpoch() : 0;
}

// 统一视图的文本字段：只保留非空值，并按字段上限截断。
void insertActivityText(QJsonObject *row, const QString &key, const QJsonValue &value,
                        int limit = 200)
{
    if (!row) return;
    const QString text = value.toString().trimmed();
    if (text.isEmpty()) return;
    row->insert(key, text.left(limit));
}

// legacy 行（chat_history thought）的统一表示。SELECT 必须包含：
// id,thought,thought_version,thought_result,coalesce(review_after,0) AS review_after,
// channel,session_id,subject,timestamp。
QJsonObject legacyActivityRecord(const QSqlQuery &query)
{
    const qlonglong id = query.value(QStringLiteral("id")).toLongLong();
    const QJsonObject stored = QJsonDocument::fromJson(
        query.value(QStringLiteral("thought_result")).toString().toUtf8()).object();
    QJsonObject row{
        {QStringLiteral("ref"), QStringLiteral("thought/%1").arg(id)},
        {QStringLiteral("origin"), QStringLiteral("legacy")},
        {QStringLiteral("kind"), QStringLiteral("legacy")},
        {QStringLiteral("id"), id},
        {QStringLiteral("status"),
         stored.value(QStringLiteral("status")).toString(QStringLiteral("active"))},
        {QStringLiteral("version"), query.value(QStringLiteral("thought_version")).toInt()},
        {QStringLiteral("interest"), query.value(QStringLiteral("thought")).toString()},
        {QStringLiteral("review_after"), query.value(QStringLiteral("review_after")).toLongLong()},
        {QStringLiteral("channel"), query.value(QStringLiteral("channel")).toString()},
        {QStringLiteral("session"), query.value(QStringLiteral("session_id")).toString()},
        {QStringLiteral("subject"), query.value(QStringLiteral("subject")).toString()}};
    for (const auto &key : {QStringLiteral("goal"), QStringLiteral("progress"),
                            QStringLiteral("next_step"), QStringLiteral("reflection"),
                            QStringLiteral("mood"), QStringLiteral("paused_reason"),
                            QStringLiteral("deferred_reason"),
                            QStringLiteral("completion_condition"),
                            QStringLiteral("activity_kind")})
        insertActivityText(&row, key, stored.value(key), 240);
    insertActivityText(&row, QStringLiteral("draft"), stored.value(QStringLiteral("draft")), 800);
    insertActivityText(&row, QStringLiteral("artifact_ref"),
                       stored.value(QStringLiteral("artifact_ref")), 200);
    // R12-1b：成果版本/哈希（新写入必有；旧 completed 只读兼容时按行版本兜底）。
    if (stored.value(QStringLiteral("artifact_version")).isDouble())
        row.insert(QStringLiteral("artifact_version"),
                   stored.value(QStringLiteral("artifact_version")).toInt());
    insertActivityText(&row, QStringLiteral("artifact_hash"),
                       stored.value(QStringLiteral("artifact_hash")), 64);
    if (stored.contains(QStringLiteral("completed_at"))) {
        const QJsonValue completedAt = stored.value(QStringLiteral("completed_at"));
        if (completedAt.isDouble())
            row.insert(QStringLiteral("completed_at"),
                       activityIsoFromEpoch(completedAt.toInteger()));
        else
            insertActivityText(&row, QStringLiteral("completed_at"), completedAt, 64);
    }
    // 旧分享结果字段 share_outcome/share_version 映射为统一的 share_intent（只读，
    // 只表示"曾经/想要分享"，不代表已经发送）。
    QJsonObject shareIntent;
    const QString shareOutcome = stored.value(QStringLiteral("share_outcome")).toString();
    const int shareVersion = stored.value(QStringLiteral("share_version")).toInt(-1);
    if (!shareOutcome.isEmpty() || shareVersion >= 0) {
        shareIntent.insert(QStringLiteral("requested"), !shareOutcome.isEmpty());
        if (!shareOutcome.isEmpty())
            shareIntent.insert(QStringLiteral("outcome"), shareOutcome);
        if (shareVersion >= 0)
            shareIntent.insert(QStringLiteral("version"), shareVersion);
        const QString shareDraft = stored.value(QStringLiteral("draft")).toString().trimmed();
        if (!shareDraft.isEmpty())
            shareIntent.insert(QStringLiteral("draft"), shareDraft.left(800));
        row.insert(QStringLiteral("share_intent"), shareIntent);
    }
    const qlonglong updated =
        activityEpochFromText(stored.value(QStringLiteral("updated_at")).toString());
    const qlonglong created =
        activityEpochFromText(stored.value(QStringLiteral("created_at")).toString());
    const QString timestamp = query.value(QStringLiteral("timestamp")).toString();
    if (updated > 0) {
        row.insert(QStringLiteral("updated_at"), activityIsoFromEpoch(updated));
    } else if (!timestamp.trimmed().isEmpty()) {
        row.insert(QStringLiteral("updated_at"), timestamp);
    }
    if (created > 0) row.insert(QStringLiteral("created_at"), activityIsoFromEpoch(created));
    return row;
}

// internal 行（internal_activity）的统一表示。SELECT * 以便按列名取值。
QJsonObject internalActivityRecord(const QSqlQuery &query)
{
    const qlonglong id = query.value(QStringLiteral("id")).toLongLong();
    const QJsonObject stored = QJsonDocument::fromJson(
        query.value(QStringLiteral("result_json")).toString().toUtf8()).object();
    QJsonObject row{
        {QStringLiteral("ref"), QStringLiteral("activity/%1").arg(id)},
        {QStringLiteral("origin"), QStringLiteral("internal")},
        {QStringLiteral("kind"), QStringLiteral("internal")},
        {QStringLiteral("id"), id},
        {QStringLiteral("scope_key"), query.value(QStringLiteral("scope_key")).toString()},
        {QStringLiteral("origin_kind"), query.value(QStringLiteral("origin_kind")).toString()},
        {QStringLiteral("subject"), query.value(QStringLiteral("subject")).toString()},
        {QStringLiteral("channel"), query.value(QStringLiteral("channel")).toString()},
        {QStringLiteral("session"), query.value(QStringLiteral("session_id")).toString()},
        {QStringLiteral("interest"), query.value(QStringLiteral("interest")).toString()},
        {QStringLiteral("status"), query.value(QStringLiteral("status")).toString()},
        {QStringLiteral("version"), query.value(QStringLiteral("version")).toInt()},
        {QStringLiteral("review_after"), query.value(QStringLiteral("review_after")).toLongLong()}};
    const QString originRef = query.value(QStringLiteral("origin_ref")).toString();
    if (!originRef.isEmpty()) row.insert(QStringLiteral("origin_ref"), originRef);
    const QString runId = query.value(QStringLiteral("run_id")).toString();
    if (!runId.isEmpty()) row.insert(QStringLiteral("run_id"), runId);
    for (const auto &key : {QStringLiteral("goal"), QStringLiteral("progress"),
                            QStringLiteral("next_step"), QStringLiteral("reflection"),
                            QStringLiteral("mood"), QStringLiteral("paused_reason"),
                            QStringLiteral("deferred_reason"),
                            QStringLiteral("completion_condition"),
                            QStringLiteral("activity_kind")})
        insertActivityText(&row, key, stored.value(key), 240);
    insertActivityText(&row, QStringLiteral("draft"), stored.value(QStringLiteral("draft")), 800);
    const QString artifactRef = query.value(QStringLiteral("artifact_ref")).toString();
    if (!artifactRef.trimmed().isEmpty())
        insertActivityText(&row, QStringLiteral("artifact_ref"), artifactRef, 200);
    else
        insertActivityText(&row, QStringLiteral("artifact_ref"),
                           stored.value(QStringLiteral("artifact_ref")), 200);
    // R12-1b：成果版本/哈希/completed_at 来自同一份 result_json，用于核验与完成目录。
    if (stored.value(QStringLiteral("artifact_version")).isDouble())
        row.insert(QStringLiteral("artifact_version"),
                   stored.value(QStringLiteral("artifact_version")).toInt());
    insertActivityText(&row, QStringLiteral("artifact_hash"),
                       stored.value(QStringLiteral("artifact_hash")), 64);
    if (stored.contains(QStringLiteral("completed_at"))) {
        const QJsonValue completedAt = stored.value(QStringLiteral("completed_at"));
        if (completedAt.isDouble())
            row.insert(QStringLiteral("completed_at"),
                       activityIsoFromEpoch(completedAt.toInteger()));
        else
            insertActivityText(&row, QStringLiteral("completed_at"), completedAt, 64);
    }
    const QJsonValue shareIntent = stored.value(QStringLiteral("share_intent"));
    if (shareIntent.isObject())
        row.insert(QStringLiteral("share_intent"), shareIntent.toObject());
    if (stored.value(QStringLiteral("share_offered")).toBool())
        row.insert(QStringLiteral("share_offered"), true);
    const qlonglong created = query.value(QStringLiteral("created_at")).toLongLong();
    const qlonglong updated = query.value(QStringLiteral("updated_at")).toLongLong();
    if (created > 0) row.insert(QStringLiteral("created_at"), activityIsoFromEpoch(created));
    if (updated > 0) row.insert(QStringLiteral("updated_at"), activityIsoFromEpoch(updated));
    return row;
}

// 跨存储的 active/paused 检查。excludeRef 非空时排除自身（resume 用）：
// legacy 只属于 owner scope，internal 按 scope_key 隔离，不同 scope 互不影响。
// 排除自身只作用于与 excludeRef 同存储的那一行，另一个存储的活动仍然阻止 resume。
bool hasActiveActivityInStore(const QSqlDatabase &db, const QString &scopeKey,
                              const QString &excludeRef, QString *error)
{
    if (error) error->clear();
    if (scopeKey == QStringLiteral("owner")) {
        QSqlQuery legacy(db);
        if (excludeRef.startsWith(QStringLiteral("thought/"))) {
            // legacy resume：排除自身，但仍检查同一存储里是否还有别的 active/paused。
            legacy.prepare(QStringLiteral(
                "SELECT 1 FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
                "AND subject='user' AND sensitive=0 AND ('thought/'||id)<>? "
                "AND coalesce(json_extract(thought_result,'$.status'),'active') "
                "IN ('active','paused') LIMIT 1"));
            legacy.addBindValue(excludeRef);
            if (!legacy.exec()) {
                if (error) *error = QStringLiteral("db_error");
                return false;
            }
        } else if (!legacy.exec(QStringLiteral(
                       "SELECT 1 FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
                       "AND subject='user' AND sensitive=0 "
                       "AND coalesce(json_extract(thought_result,'$.status'),'active') "
                       "IN ('active','paused') LIMIT 1"))) {
            if (error) *error = QStringLiteral("db_error");
            return false;
        }
        if (legacy.next()) return true;
    }
    QSqlQuery internal(db);
    if (excludeRef.isEmpty()) {
        internal.prepare(QStringLiteral(
            "SELECT 1 FROM internal_activity WHERE scope_key=? "
            "AND status IN ('active','paused') LIMIT 1"));
        internal.addBindValue(scopeKey);
    } else {
        internal.prepare(QStringLiteral(
            "SELECT 1 FROM internal_activity WHERE scope_key=? "
            "AND status IN ('active','paused') AND ('activity/'||id)<>? LIMIT 1"));
        internal.addBindValue(scopeKey);
        internal.addBindValue(excludeRef);
    }
    if (!internal.exec()) {
        if (error) *error = QStringLiteral("db_error");
        return false;
    }
    return internal.next();
}

// 单行读取 internal_activity（调用方已确保数据库打开且 id>0）。
bool loadInternalActivity(const QSqlDatabase &db, qlonglong id, QJsonObject *view,
                          QString *error)
{
    if (error) error->clear();
    if (view) *view = QJsonObject{};
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT * FROM internal_activity WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec()) {
        if (error) *error = QStringLiteral("db_error");
        return false;
    }
    if (!q.next()) return false;
    if (view) *view = internalActivityRecord(q);
    return true;
}

QString normalizedActivityScope(const QString &scopeKey)
{
    const QString trimmed = scopeKey.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("owner") : trimmed;
}

// ===== R12-1b 补充：成果真实性 / 模型可见投影 / 完成成果目录 =====
// 这些函数只读管理视图（activityByRef 的统一行形状）做确定性判断，不写库。

// 成果是否真实存在且可核验：artifact_ref 必须指回活动自身，且 result_json 里有非空 draft；
// 旧 completed 允许缺 artifact_version/hash（只读兼容），新写入必须带两者。
bool activityArtifactPresent(const QJsonObject &raw)
{
    const QString ref = raw.value(QStringLiteral("ref")).toString();
    const QString artifactRef = raw.value(QStringLiteral("artifact_ref")).toString().trimmed();
    const QString draft = raw.value(QStringLiteral("draft")).toString().trimmed();
    return !ref.isEmpty() && !artifactRef.isEmpty() && !draft.isEmpty()
        && artifactRef == ref;
}

// completed 的确定性归属：文字创作（需要实际 draft）或纯思考（需要 completion_condition）。
// 未显式声明时：有正文按文字创作，否则有完成条件按纯思考，两者都无仍按文字创作处理
// （要求实际成果，绝不自动把"只有想法"的回顾改成完成）。
struct ActivityCompletionPlan
{
    bool valid = false;
    QString error;
    QString kind;                    // writing / thought
    QString draft;                   // 本次或已持久化的实际成果正文
    QString completionCondition;     // 纯思考的完成条件
    QString reflection;              // 本次或已有的认识
};

QString normalizedActivityKind(const QString &value)
{
    return value.trimmed().toLower();
}

bool isUnsupportedActivityKind(const QString &kind)
{
    static const QSet<QString> unsupported{
        QStringLiteral("external"), QStringLiteral("browser"), QStringLiteral("painting"),
        QStringLiteral("execution"), QStringLiteral("tool"), QStringLiteral("contact"),
        QStringLiteral("share")};
    return unsupported.contains(kind);
}

ActivityCompletionPlan planActivityCompletion(const QJsonObject &stored,
                                             const QJsonObject &proposal)
{
    ActivityCompletionPlan plan;
    // 当前实现没有任何执行工具：外部行为/执行结果只能写 intent/reflection，
    // 不能作为 completed 或产物的依据。
    static const QStringList externalKeys{
        QStringLiteral("external_action"), QStringLiteral("external_result"),
        QStringLiteral("execution_result"), QStringLiteral("executed")};
    for (const auto &key : externalKeys) {
        if (proposal.contains(key)) {
            plan.error = QStringLiteral("unsupported_external_activity");
            return plan;
        }
    }
    QString kind = normalizedActivityKind(proposal.value(QStringLiteral("activity_kind")).toString());
    if (!kind.isEmpty()
        && kind != QStringLiteral("writing") && kind != QStringLiteral("thought")) {
        plan.error = isUnsupportedActivityKind(kind)
            ? QStringLiteral("unsupported_external_activity")
            : QStringLiteral("invalid_activity_kind");
        return plan;
    }
    if (kind.isEmpty()) {
        const QString storedKind =
            normalizedActivityKind(stored.value(QStringLiteral("activity_kind")).toString());
        if (!storedKind.isEmpty()
            && storedKind != QStringLiteral("writing") && storedKind != QStringLiteral("thought")) {
            plan.error = isUnsupportedActivityKind(storedKind)
                ? QStringLiteral("unsupported_external_activity")
                : QStringLiteral("invalid_activity_kind");
            return plan;
        }
        kind = storedKind;
    }
    // 引用已有成果正文：本次没有重写全文时沿用最近一次已持久化的 draft（不要求每次重写）。
    plan.draft = proposal.value(QStringLiteral("draft")).toString().trimmed();
    if (plan.draft.isEmpty())
        plan.draft = stored.value(QStringLiteral("draft")).toString().trimmed();
    plan.completionCondition =
        proposal.value(QStringLiteral("completion_condition")).toString().trimmed();
    if (plan.completionCondition.isEmpty())
        plan.completionCondition =
            stored.value(QStringLiteral("completion_condition")).toString().trimmed();
    plan.reflection = proposal.value(QStringLiteral("reflection")).toString().trimmed();
    if (plan.reflection.isEmpty())
        plan.reflection = stored.value(QStringLiteral("reflection")).toString().trimmed();
    if (kind.isEmpty()) {
        if (!plan.draft.isEmpty())
            kind = QStringLiteral("writing");
        else if (!plan.completionCondition.isEmpty())
            kind = QStringLiteral("thought");
        else
            kind = QStringLiteral("writing");
    }
    plan.kind = kind;
    if (kind == QStringLiteral("writing")) {
        if (plan.draft.isEmpty()) {
            plan.error = QStringLiteral("missing_artifact");
            return plan;
        }
    } else {
        if (plan.completionCondition.isEmpty()) {
            plan.error = QStringLiteral("missing_completion_condition");
            return plan;
        }
        if (plan.reflection.isEmpty()) {
            plan.error = QStringLiteral("missing_reflection");
            return plan;
        }
    }
    plan.valid = true;
    return plan;
}

// 分享意愿的嵌套字段过滤：state/版本/哈希等服务端字段保留，request/正文类文本
// 必须与活动其它字段走同一套读取策略（盲区 + 隐私规则），不可见就移除。
void filterShareIntentForModel(QJsonObject *intent, const MemoryReadPolicy &policy,
                               const QString &channel, const QString &session)
{
    if (!intent || intent->isEmpty()) return;
    QJsonObject filtered;
    for (const auto &key : {QStringLiteral("state"), QStringLiteral("artifact_ref"),
                            QStringLiteral("artifact_version"), QStringLiteral("content_hash"),
                            QStringLiteral("requested_at"), QStringLiteral("withdrawn_at"),
                            QStringLiteral("version")}) {
        if (intent->contains(key)) filtered.insert(key, intent->value(key));
    }
    const QString request = intent->value(QStringLiteral("request")).toString().trimmed();
    if (!request.isEmpty() && policy.allows(channel, session, request))
        filtered.insert(QStringLiteral("request"), request.left(120));
    // 旧 legacy 映射可能把 draft 塞进 share_intent；只保留通过策略的文本。
    const QString draft = intent->value(QStringLiteral("draft")).toString().trimmed();
    if (!draft.isEmpty() && policy.allows(channel, session, draft))
        filtered.insert(QStringLiteral("draft"), draft.left(800));
    *intent = filtered;
}

// 统一行（activityByRef 形状）→ 模型可见投影。调用方已确认活动本身可见；
// 文本字段逐项按读取策略过滤，嵌套 share_intent 同样过滤；completed 不返回过时 next_step。
QJsonObject projectActivityForModel(const QJsonObject &raw, const QString &channel,
                                    const QString &session, const MemoryReadPolicy &policy)
{
    QJsonObject row;
    for (const auto &key : {QStringLiteral("ref"), QStringLiteral("origin"),
                            QStringLiteral("kind"), QStringLiteral("id"),
                            QStringLiteral("status"), QStringLiteral("version"),
                            QStringLiteral("channel"), QStringLiteral("session"),
                            QStringLiteral("scope_key"), QStringLiteral("origin_kind"),
                            QStringLiteral("origin_ref"), QStringLiteral("run_id"),
                            QStringLiteral("review_after"), QStringLiteral("created_at"),
                            QStringLiteral("updated_at")}) {
        if (raw.contains(key)) row.insert(key, raw.value(key));
    }
    const QString interest = raw.value(QStringLiteral("interest")).toString().trimmed();
    if (!interest.isEmpty()) {
        if (!policy.allows(channel, session, interest)) return {};
        row.insert(QStringLiteral("interest"), interest.left(160));
    }
    for (const auto &key : {QStringLiteral("goal"), QStringLiteral("progress"),
                            QStringLiteral("next_step"), QStringLiteral("reflection"),
                            QStringLiteral("mood"), QStringLiteral("paused_reason"),
                            QStringLiteral("deferred_reason"),
                            QStringLiteral("completion_condition")}) {
        const QString text = raw.value(key).toString().trimmed();
        if (text.isEmpty()) continue;
        if (!policy.allows(channel, session, text)) continue;
        row.insert(key, text.left(key == QStringLiteral("completion_condition") ? 200 : 120));
    }
    const QString draft = raw.value(QStringLiteral("draft")).toString().trimmed();
    bool draftVisible = false;
    if (!draft.isEmpty() && policy.allows(channel, session, draft)) {
        row.insert(QStringLiteral("draft"), draft.left(800));
        draftVisible = true;
    }
    const QString status = raw.value(QStringLiteral("status")).toString();
    // completed/abandoned 的 next_step 是过时调度信息，不能进入模型视图。
    if (status == QStringLiteral("completed") || status == QStringLiteral("abandoned"))
        row.remove(QStringLiteral("next_step"));
    // 只有可核验且成果正文本身对本视图可见时才暴露 artifact_ref/版本/哈希；
    // 悬空引用或不可见正文一律不出现（与嵌套字段同一可见性）。
    if (draftVisible && activityArtifactPresent(raw)) {
        row.insert(QStringLiteral("artifact_ref"),
                   raw.value(QStringLiteral("artifact_ref")).toString().trimmed().left(200));
        const int artifactVersion = raw.value(QStringLiteral("artifact_version")).toInt(0);
        row.insert(QStringLiteral("artifact_version"),
                   artifactVersion > 0 ? artifactVersion
                                       : raw.value(QStringLiteral("version")).toInt());
        const QString artifactHash =
            raw.value(QStringLiteral("artifact_hash")).toString().trimmed();
        if (!artifactHash.isEmpty()) row.insert(QStringLiteral("artifact_hash"), artifactHash.left(64));
    }
    if (raw.contains(QStringLiteral("completed_at")))
        row.insert(QStringLiteral("completed_at"), raw.value(QStringLiteral("completed_at")));
    if (raw.value(QStringLiteral("share_intent")).isObject()) {
        QJsonObject intent = raw.value(QStringLiteral("share_intent")).toObject();
        filterShareIntentForModel(&intent, policy, channel, session);
        if (!intent.isEmpty()) row.insert(QStringLiteral("share_intent"), intent);
    }
    if (raw.value(QStringLiteral("share_offered")).toBool())
        row.insert(QStringLiteral("share_offered"), true);
    return row;
}

// 完成成果目录的一行：不包含过时 next_step，也不包含未持久化的正文。
QJsonObject recentActivityResultRow(const QJsonObject &raw, const MemoryReadPolicy &policy)
{
    const QString channel = raw.value(QStringLiteral("channel")).toString();
    const QString session = raw.value(QStringLiteral("session")).toString();
    const QString draft = raw.value(QStringLiteral("draft")).toString().trimmed();
    const bool artifactVisible =
        !draft.isEmpty() && policy.allows(channel, session, draft);
    QJsonObject row{
        {QStringLiteral("ref"), raw.value(QStringLiteral("ref"))},
        {QStringLiteral("version"), raw.value(QStringLiteral("version"))},
        {QStringLiteral("status"), QStringLiteral("completed")}};
    // 成果正文/引用按同一可见性处理：正文不可见时也不暴露 artifact_ref/版本/哈希。
    if (artifactVisible) {
        row.insert(QStringLiteral("artifact_ref"),
                   raw.value(QStringLiteral("artifact_ref")).toString().trimmed());
        const int artifactVersion = raw.value(QStringLiteral("artifact_version")).toInt(0);
        row.insert(QStringLiteral("artifact_version"),
                   artifactVersion > 0 ? artifactVersion
                                       : raw.value(QStringLiteral("version")).toInt());
        const QString artifactHash =
            raw.value(QStringLiteral("artifact_hash")).toString().trimmed();
        if (!artifactHash.isEmpty())
            row.insert(QStringLiteral("artifact_hash"), artifactHash.left(64));
    }
    // title=活动本身（兴趣），goal=意图，summary=最近认识；全部按读取策略过滤。
    const QString title = raw.value(QStringLiteral("interest")).toString().trimmed();
    if (!title.isEmpty() && policy.allows(channel, session, title))
        row.insert(QStringLiteral("title"), title.left(160));
    const QString goal = raw.value(QStringLiteral("goal")).toString().trimmed();
    if (!goal.isEmpty() && policy.allows(channel, session, goal))
        row.insert(QStringLiteral("goal"), goal.left(120));
    QString summary = raw.value(QStringLiteral("reflection")).toString().trimmed();
    if (summary.isEmpty())
        summary = raw.value(QStringLiteral("progress")).toString().trimmed();
    if (!summary.isEmpty() && policy.allows(channel, session, summary))
        row.insert(QStringLiteral("summary"), summary.left(120));
    const QString completedAt = raw.value(QStringLiteral("completed_at")).toString().trimmed();
    if (!completedAt.isEmpty())
        row.insert(QStringLiteral("completed_at"), completedAt);
    return row;
}

qlonglong activityCompletedEpoch(const QJsonObject &raw)
{
    const QString text = raw.value(QStringLiteral("completed_at")).toString().trimmed();
    qlonglong epoch = activityEpochFromText(text);
    if (epoch > 0) return epoch;
    epoch = activityEpochFromText(raw.value(QStringLiteral("updated_at")).toString());
    return epoch;
}

} // namespace

// 统一活动列表：legacy/internal 合并后按 updated/created 倒序。
// 只返回 active/paused：completed 的过时 next_step 绝不出现在"当前活动"里。
// R12-1b：queried/eligible 只做快照可观测性（与其它域口径一致，不改变返回内容）。
QJsonArray MemoryManager::currentActivitiesUnified(int limit,int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || limit <= 0) return rows;
    const MemoryReadPolicy policy;
    QList<QPair<qint64, QJsonObject>> combined;
    // legacy：字段与 currentActivities 一致，只额外标 origin=legacy；旧接口保持不变。
    QSqlQuery legacy(m_db);
    legacy.prepare(QStringLiteral(
        "SELECT id,thought,thought_version,coalesce(review_after,0) AS review_after,"
        "thought_result,channel,session_id,subject,timestamp "
        "FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
        "AND sensitive=0 AND subject='user' "
        "AND coalesce(json_extract(thought_result,'$.status'),'active') IN ('active','paused') "
        "ORDER BY id DESC LIMIT ?"));
    legacy.addBindValue(limit);
    if (legacy.exec()) {
        while (legacy.next()) {
            if (queried) ++*queried;
            QJsonObject row = legacyActivityRecord(legacy);
            const QString channel = row.value(QStringLiteral("channel")).toString();
            const QString session = row.value(QStringLiteral("session")).toString();
            for (const auto &key : {QStringLiteral("interest"), QStringLiteral("goal"),
                                    QStringLiteral("progress"), QStringLiteral("next_step"),
                                    QStringLiteral("reflection"), QStringLiteral("artifact_ref")}) {
                if (!row.contains(key)) continue;
                const QString text = row.value(key).toString();
                if (text.isEmpty() || !policy.allows(channel, session, text)) {
                    row.remove(key);
                    continue;
                }
                row.insert(key, text.left(key == QStringLiteral("interest") ? 160 : 120));
            }
            if (row.contains(QStringLiteral("draft"))) {
                const QString draft = row.value(QStringLiteral("draft")).toString();
                if (!policy.allows(channel, session, draft))
                    row.remove(QStringLiteral("draft"));
            }
            combined.append({activityEpochFromText(
                                 row.value(QStringLiteral("updated_at")).toString()), row});
        }
    }
    // internal：本片起可写，ref=activity/<id>，origin=internal；只读 owner 私聊域，
    // 其它 scope（群聊/第三方/其它会话）的活动不进入统一列表，避免快照/前台扩权。
    QSqlQuery internal(m_db);
    internal.prepare(QStringLiteral(
        "SELECT * FROM internal_activity WHERE scope_key='owner' "
        "AND status IN ('active','paused') ORDER BY id DESC LIMIT ?"));
    internal.addBindValue(limit);
    if (internal.exec()) {
        while (internal.next()) {
            if (queried) ++*queried;
            QJsonObject row = internalActivityRecord(internal);
            const QString channel = row.value(QStringLiteral("channel")).toString();
            const QString session = row.value(QStringLiteral("session")).toString();
            const QString interest = row.value(QStringLiteral("interest")).toString();
            if (interest.isEmpty() || !policy.allows(channel, session, interest)) continue;
            for (const auto &key : {QStringLiteral("goal"), QStringLiteral("progress"),
                                    QStringLiteral("next_step"), QStringLiteral("reflection"),
                                    QStringLiteral("artifact_ref")}) {
                if (!row.contains(key)) continue;
                const QString text = row.value(key).toString();
                if (text.isEmpty() || !policy.allows(channel, session, text)) {
                    row.remove(key);
                    continue;
                }
                row.insert(key, text.left(120));
            }
            row.insert(QStringLiteral("interest"), interest.left(160));
            if (row.contains(QStringLiteral("draft"))) {
                const QString draft = row.value(QStringLiteral("draft")).toString();
                if (!policy.allows(channel, session, draft))
                    row.remove(QStringLiteral("draft"));
            }
            combined.append({activityEpochFromText(
                                 row.value(QStringLiteral("updated_at")).toString()), row});
        }
    }
    std::sort(combined.begin(), combined.end(),
              [](const QPair<qint64, QJsonObject> &left,
                 const QPair<qint64, QJsonObject> &right) {
                  if (left.first != right.first) return left.first > right.first;
                  return left.second.value(QStringLiteral("ref")).toString()
                      > right.second.value(QStringLiteral("ref")).toString();
              });
    for (const auto &entry : combined) {
        if (rows.size() >= limit) break;
        rows.append(entry.second);
    }
    if (eligible) *eligible = int(rows.size());
    return rows;
}

// 新建 internal 活动（"产生意愿并开始安排"，不代表完成/产物）。
// 事务内：幂等键 -> 两存储单 active -> 插入；任何一步失败整体回滚，绝不半写。
QJsonObject MemoryManager::createActivity(const QString &scopeKey, const QString &channel,
                                          const QString &session, const QString &interest,
                                          const QString &goal, const QString &originKind,
                                          const QString &originRef, const QString &runId,
                                          QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return QJsonObject{};
    };
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    const QString scope = normalizedActivityScope(scopeKey);
    const QString trimmedChannel = channel.trimmed();
    const QString trimmedSession = session.trimmed();
    const QString trimmedInterest = interest.trimmed();
    const QString trimmedGoal = goal.trimmed();
    const QString trimmedOriginKind = originKind.trimmed();
    const QString trimmedOriginRef = originRef.trimmed();
    const QString trimmedRunId = runId.trimmed();
    // 新建活动只允许主人私聊域的 foreground/open_wake；legacy 迁移/导入不能走新写接口。
    if (scope != QStringLiteral("owner")) return fail(QStringLiteral("invalid_scope"));
    if (trimmedChannel.isEmpty() || trimmedSession.isEmpty() || trimmedInterest.isEmpty())
        return fail(QStringLiteral("invalid_argument"));
    if (trimmedOriginKind != QStringLiteral("foreground")
        && trimmedOriginKind != QStringLiteral("open_wake"))
        return fail(QStringLiteral("invalid_origin_kind"));
    if (trimmedRunId.isEmpty()) return fail(QStringLiteral("missing_run_id"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    // 幂等：同一 (origin_kind, run_id) 只对应一个活动（R12-1b：前台同一 meta.eventId
    // 可能因重复提交产生不同 chat 行，origin_ref 会变，但 runId 不变，仍不得重复建活动）；
    // 重复调用返回已有行。
    QSqlQuery existing(m_db);
    existing.prepare(QStringLiteral(
        "SELECT * FROM internal_activity WHERE origin_kind=? AND run_id=? ORDER BY id LIMIT 1"));
    existing.addBindValue(trimmedOriginKind);
    existing.addBindValue(trimmedRunId);
    if (!existing.exec()) return fail(QStringLiteral("db_error"));
    if (existing.next()) {
        QJsonObject row = internalActivityRecord(existing);
        existing.finish();
        if (!m_db.commit()) return fail(QStringLiteral("db_error"));
        rollback.dismiss();
        return row;
    }
    existing.finish();
    // 单 active/paused 跨存储统一：任一存储已有当前活动就拒绝，且不写任何行。
    QString activeError;
    if (hasActiveActivityInStore(m_db, scope, QString{}, &activeError))
        return fail(QStringLiteral("already_active"));
    if (!activeError.isEmpty()) return fail(QStringLiteral("db_error"));
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    QJsonObject result{{QStringLiteral("goal"), trimmedGoal},
                       {QStringLiteral("next_step"), QString{}}};
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO internal_activity(scope_key,origin_kind,origin_ref,run_id,subject,channel,"
        "session_id,interest,status,version,result_json,artifact_ref,review_after,"
        "created_at,updated_at) "
        "VALUES(?,?,?,?,'user',?,?,?,'active',1,?,NULL,?,?,?)"));
    insert.addBindValue(scope);
    insert.addBindValue(trimmedOriginKind);
    insert.addBindValue(trimmedOriginRef.isEmpty() ? QVariant() : QVariant(trimmedOriginRef));
    insert.addBindValue(trimmedRunId);
    insert.addBindValue(trimmedChannel);
    insert.addBindValue(trimmedSession);
    insert.addBindValue(trimmedInterest);
    insert.addBindValue(QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Compact)));
    insert.addBindValue(now + 1800);
    insert.addBindValue(now);
    insert.addBindValue(now);
    if (!insert.exec()) return fail(QStringLiteral("db_error"));
    if (insert.numRowsAffected() != 1) return fail(QStringLiteral("db_error"));
    const qlonglong id = insert.lastInsertId().toLongLong();
    if (id <= 0) return fail(QStringLiteral("db_error"));
    QJsonObject view;
    QString loadError;
    if (!loadInternalActivity(m_db, id, &view, &loadError))
        return fail(loadError.isEmpty() ? QStringLiteral("db_error") : loadError);
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    return view;
}

// R12-1b：OpenWake 的 start_activity 提交——同一事务先建 internal 活动，再完成 wake/调度。
// 任一步失败整体回滚：既不会留下"只有活动、wake 仍 running"，也不会"wake 已完成、活动没建"。
bool MemoryManager::finishOpenWakeWithActivity(qlonglong id, int version,
                                               const QString &decision, const QString &note,
                                               const QJsonObject &intent, int nextWakeMinutes,
                                               const QJsonObject &activityProposal,
                                               QJsonObject *result, QString *error)
{
    if (result) *result = QJsonObject{};
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (decision != QStringLiteral("start_activity")) return fail(QStringLiteral("invalid_decision"));
    // 私下写入门控：OpenWake 仍可 rest/reflect，但只有 private_writing=true 才允许创建活动。
    if (!ConfigManager::instance().getProactiveConfig().privateWriting)
        return fail(QStringLiteral("writing_disabled"));
    const QString trimmedNote = note.trimmed();
    if (trimmedNote.size() > 240) return fail(QStringLiteral("invalid_note"));
    // 活动提案：只代表"产生意愿并开始安排"，不写完成状态/产物。
    const QString interest = activityProposal.value(QStringLiteral("interest")).toString().trimmed();
    const QString goal = activityProposal.value(QStringLiteral("goal")).toString().trimmed();
    if (interest.isEmpty() || interest.size() > 120) return fail(QStringLiteral("invalid_interest"));
    if (goal.size() > 200) return fail(QStringLiteral("invalid_goal"));
    if (activityProposal.contains(QStringLiteral("contact"))
        || activityProposal.contains(QStringLiteral("artifact"))
        || activityProposal.contains(QStringLiteral("status"))
        || activityProposal.contains(QStringLiteral("draft")))
        return fail(QStringLiteral("schema"));
    int reviewMinutes = 30;
    if (activityProposal.contains(QStringLiteral("initial_review_after_minutes"))) {
        const auto value = activityProposal.value(QStringLiteral("initial_review_after_minutes"));
        if (!value.isDouble()) return fail(QStringLiteral("invalid_initial_review"));
        reviewMinutes = value.toInt();
        if (reviewMinutes < 5 || reviewMinutes > 1440)
            return fail(QStringLiteral("invalid_initial_review"));
    }
    QString channel = activityProposal.value(QStringLiteral("channel")).toString().trimmed();
    QString session = activityProposal.value(QStringLiteral("session")).toString().trimmed();
    if (channel.isEmpty()) channel = QString(Channel::kPet);
    if (session.isEmpty()) session = QString(Channel::kDefaultSession);
    if (channel != Channel::kPet && channel != Channel::kQQPrivate)
        return fail(QStringLiteral("invalid_channel"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    // 1) 读取并 CAS 校验 open_wake 仍处于本次领取的 running/version。
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT scope_key,run_id,state,version FROM open_wake WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (!q.next()) return fail(QStringLiteral("cas_conflict"));
    const QString scope = normalizedActivityScope(q.value(0).toString());
    const QString runId = q.value(1).toString().trimmed();
    const QString state = q.value(2).toString();
    const int currentVersion = q.value(3).toInt();
    q.finish();
    if (currentVersion != version || state != QStringLiteral("running"))
        return fail(QStringLiteral("cas_conflict"));
    // 新建 internal 活动只允许主人私聊域；scope/run_id 必须自洽。
    if (scope != QStringLiteral("owner")) return fail(QStringLiteral("invalid_scope"));
    if (runId.isEmpty()) return fail(QStringLiteral("missing_run_id"));
    // 2) 单 active 跨存储：legacy/internal 任一当前活动都让整次提交失败（wake 不半写）。
    QString activeError;
    if (hasActiveActivityInStore(m_db, scope, QString{}, &activeError))
        return fail(QStringLiteral("already_active"));
    if (!activeError.isEmpty()) return fail(QStringLiteral("db_error"));
    // 3) 创建 internal 活动（origin_ref=wake/<id>，run_id 复用 open_wake.run_id）。
    const qlonglong reviewAfter = now + qlonglong(reviewMinutes) * 60;
    const QString initialJson = QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("goal"), goal},
        {QStringLiteral("next_step"), QString{}}}).toJson(QJsonDocument::Compact));
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO internal_activity(scope_key,origin_kind,origin_ref,run_id,subject,channel,"
        "session_id,interest,status,version,result_json,artifact_ref,review_after,"
        "created_at,updated_at) "
        "VALUES('owner','open_wake',?,?,'user',?,?,?,'active',1,?,NULL,?,?,?)"));
    insert.addBindValue(QStringLiteral("wake/%1").arg(id));
    insert.addBindValue(runId);
    insert.addBindValue(channel);
    insert.addBindValue(session);
    insert.addBindValue(interest);
    insert.addBindValue(initialJson);
    insert.addBindValue(reviewAfter);
    insert.addBindValue(now);
    insert.addBindValue(now);
    if (!insert.exec()) return fail(QStringLiteral("db_error"));
    if (insert.numRowsAffected() != 1) return fail(QStringLiteral("db_error"));
    const qlonglong activityId = insert.lastInsertId().toLongLong();
    if (activityId <= 0) return fail(QStringLiteral("db_error"));
    // 4) CAS 完成 open_wake：decision=start_activity，note 可空，安排下一次唤醒。
    const int minutes = qBound(5, nextWakeMinutes, 1440);
    const qlonglong nextDue = now + qlonglong(minutes) * 60;
    const QString intentJson =
        QString::fromUtf8(QJsonDocument(intent).toJson(QJsonDocument::Compact));
    q.prepare(QStringLiteral(
        "UPDATE open_wake SET state='committed',decision=?,note=?,intent_json=?,"
        "finished_at=?,lease_until=NULL,next_due_at=? "
        "WHERE id=? AND version=? AND state='running'"));
    q.addBindValue(decision);
    q.addBindValue(trimmedNote);
    q.addBindValue(intentJson);
    q.addBindValue(now);
    q.addBindValue(nextDue);
    q.addBindValue(id);
    q.addBindValue(version);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (q.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    // 5) 调度状态同事务推进；start_activity 与 rest/reflect 一样算成功。
    q.prepare(QStringLiteral(
        "UPDATE open_wake_state SET next_due_at=?,last_finished_at=?,last_outcome=?,"
        "version=version+1,updated_at=?,suspended_until=NULL,consecutive_failures=0 "
        "WHERE scope_key=?"));
    q.addBindValue(nextDue);
    q.addBindValue(now);
    q.addBindValue(decision);
    q.addBindValue(now);
    q.addBindValue(scope);
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    if (q.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    q.prepare(QStringLiteral(
        "DELETE FROM review_failure WHERE source_kind='open_wake' AND source_ref IN (?,?)"));
    q.addBindValue(QStringLiteral("wake/%1:%2").arg(id).arg(version));
    q.addBindValue(QStringLiteral("wake/%1").arg(id));
    if (!q.exec()) return fail(QStringLiteral("db_error"));
    QJsonObject view;
    QString loadError;
    if (!loadInternalActivity(m_db, activityId, &view, &loadError))
        return fail(loadError.isEmpty() ? QStringLiteral("db_error") : loadError);
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    if (result) {
        *result = view;
        result->insert(QStringLiteral("decision"), decision);
        result->insert(QStringLiteral("next_due_at"), nextDue);
        result->insert(QStringLiteral("initial_review_after_minutes"), reviewMinutes);
    }
    qInfo().noquote()
        << QStringLiteral("[OpenWake] activity_created wake=%1 activity=%2 review_after=%3")
               .arg(id)
               .arg(view.value(QStringLiteral("ref")).toString(),
                    reviewIsoFromEpoch(reviewAfter));
    return true;
}

// 统一读取：legacy 适配 thought_result，internal 读 internal_activity；不存在返回 {}。
QJsonObject MemoryManager::activityByRef(const QString &ref) const
{
    if (!m_db.isOpen()) return {};
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return {};
    QSqlQuery q(m_db);
    if (parsed.kind == ActivityRef::LegacyThought) {
        q.prepare(QStringLiteral(
            "SELECT id,thought,thought_version,coalesce(review_after,0) AS review_after,"
            "thought_result,channel,session_id,subject,timestamp FROM chat_history "
            "WHERE id=? AND thought IS NOT NULL AND thought<>'' AND sensitive=0 "
            "AND subject='user'"));
        q.addBindValue(parsed.id);
        if (!q.exec() || !q.next()) return {};
        return legacyActivityRecord(q);
    }
    QJsonObject view;
    QString error;
    if (!loadInternalActivity(m_db, parsed.id, &view, &error)) return {};
    return view;
}

// ===== R12-1b 补充：模型可见活动投影 =====
// activityByRef 是管理读取；模型入口（capabilityManifest/自己的念头、ActivityReview context、
// 快照 current/recent、未来行动核验）一律走这里。投影只读，不改变任何存储。
QJsonObject MemoryManager::activityForModel(const QString &ref, const QString &channel,
                                            const QString &session, const QString &subject,
                                            bool longTermMemoryEnabled) const
{
    if (!m_db.isOpen()) return {};
    // 长记忆关闭时不提供任何活动信息；参数与服务端配置必须同时开启。
    if (!longTermMemoryEnabled) return {};
    if (!ConfigManager::instance().getMemoryPolicyConfig().longTermMemoryEnabled) return {};
    // 活动只属于主人私聊域；群聊/第三方一律不可见。
    if (subject != QStringLiteral("user")) return {};
    if (!isOwnerPrivateActivityChannel(channel)) return {};
    if (ConfigManager::instance().isBlindZone(channel, session)) return {};
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return {};
    const QJsonObject raw = activityByRef(ref);
    if (raw.isEmpty()) return {};
    if (raw.value(QStringLiteral("subject")).toString() != QStringLiteral("user")) return {};
    if (parsed.kind == ActivityRef::InternalActivity
        && raw.value(QStringLiteral("scope_key")).toString() != QStringLiteral("owner"))
        return {};
    // 活动自身的来源渠道/会话也必须属于私聊域且不在盲区（群聊来源的活动不进入投影）。
    const QString rowChannel = raw.value(QStringLiteral("channel")).toString();
    const QString rowSession = raw.value(QStringLiteral("session")).toString();
    if (!isOwnerPrivateActivityChannel(rowChannel)) return {};
    if (ConfigManager::instance().isBlindZone(rowChannel, rowSession)) return {};
    MemoryReadPolicy policy;
    if (!policy.allowsSession(rowChannel, rowSession)) return {};
    return projectActivityForModel(raw, rowChannel, rowSession, policy);
}

QJsonArray MemoryManager::currentActivitiesForModel(int limit, const QString &channel,
                                                    const QString &session,
                                                    const QString &subject,
                                                    bool longTermMemoryEnabled) const
{
    QJsonArray rows;
    if (limit <= 0) return rows;
    const int capped = qMin(limit, 8);
    const QJsonArray rawRows = currentActivitiesUnified(capped);
    for (const auto &value : rawRows) {
        if (rows.size() >= capped) break;
        const QString ref = value.toObject().value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) continue;
        const QJsonObject projected =
            activityForModel(ref, channel, session, subject, longTermMemoryEnabled);
        if (projected.isEmpty()) continue;
        rows.append(projected);
    }
    return rows;
}

QJsonArray MemoryManager::recentActivityResults(const QString &scopeKey, int limit) const
{
    QJsonArray rows;
    if (!m_db.isOpen() || limit <= 0) return rows;
    const int capped = qMin(limit, 8);
    const QString scope = normalizedActivityScope(scopeKey);
    const MemoryReadPolicy policy;
    struct Entry {
        qlonglong completedAt = 0;
        qlonglong tie = 0;
        QJsonObject row;
    };
    QList<Entry> entries;
    // legacy 完成目录：completed 且 artifact_ref/draft 真实存在；旧记录缺版本/哈希时兜底。
    if (scope == QStringLiteral("owner")) {
        QSqlQuery legacy(m_db);
        legacy.prepare(QStringLiteral(
            "SELECT id,thought,thought_version,coalesce(review_after,0) AS review_after,"
            "thought_result,channel,session_id,subject,timestamp FROM chat_history "
            "WHERE thought IS NOT NULL AND thought<>'' AND subject='user' AND sensitive=0 "
            "AND coalesce(json_extract(thought_result,'$.status'),'active')='completed' "
            "ORDER BY coalesce(json_extract(thought_result,'$.completed_at'),"
            "json_extract(thought_result,'$.artifact_version'),id) DESC, id DESC "
            "LIMIT ?"));
        legacy.addBindValue(capped);
        if (legacy.exec()) {
            while (legacy.next()) {
                const QJsonObject raw = legacyActivityRecord(legacy);
                if (!activityArtifactPresent(raw)) continue;
                entries.append({activityCompletedEpoch(raw),
                                raw.value(QStringLiteral("version")).toInteger(),
                                recentActivityResultRow(raw, policy)});
            }
        }
    }
    // internal 完成目录：同一 scope 内按完成时间倒序；无真实成果的 completed 不进入目录。
    QSqlQuery internal(m_db);
    internal.prepare(QStringLiteral(
        "SELECT * FROM internal_activity WHERE scope_key=? AND status='completed' "
        "ORDER BY coalesce(json_extract(result_json,'$.completed_at'),updated_at) DESC, id DESC "
        "LIMIT ?"));
    internal.addBindValue(scope);
    internal.addBindValue(capped);
    if (internal.exec()) {
        while (internal.next()) {
            const QJsonObject raw = internalActivityRecord(internal);
            if (!activityArtifactPresent(raw)) continue;
            entries.append({activityCompletedEpoch(raw),
                            raw.value(QStringLiteral("version")).toInteger(),
                            recentActivityResultRow(raw, policy)});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        if (left.completedAt != right.completedAt) return left.completedAt > right.completedAt;
        if (left.tie != right.tie) return left.tie > right.tie;
        return left.row.value(QStringLiteral("ref")).toString()
            > right.row.value(QStringLiteral("ref")).toString();
    });
    for (const auto &entry : entries) {
        if (rows.size() >= capped) break;
        rows.append(entry.row);
    }
    return rows;
}

QJsonArray MemoryManager::recentActivityResultsForModel(int limit, const QString &channel,
                                                       const QString &session,
                                                       const QString &subject,
                                                       bool longTermMemoryEnabled) const
{
    QJsonArray rows;
    if (limit <= 0) return rows;
    if (!longTermMemoryEnabled) return rows;
    if (!ConfigManager::instance().getMemoryPolicyConfig().longTermMemoryEnabled) return rows;
    if (subject != QStringLiteral("user")) return rows;
    if (!isOwnerPrivateActivityChannel(channel)) return rows;
    if (ConfigManager::instance().isBlindZone(channel, session)) return rows;
    const int capped = qMin(limit, 8);
    // 完成目录本身已按读取策略过滤文本；这里再逐条复核活动投影（scope/渠道/盲区/敏感）。
    for (const auto &value : recentActivityResults(QStringLiteral("owner"), capped)) {
        if (rows.size() >= capped) break;
        const QJsonObject row = value.toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) continue;
        const QJsonObject projected =
            activityForModel(ref, channel, session, subject, longTermMemoryEnabled);
        if (projected.isEmpty()) continue;
        rows.append(row);
    }
    return rows;
}

bool MemoryManager::hasActiveActivity(const QString &scopeKey) const
{
    if (!m_db.isOpen()) return false;
    QString error;
    const bool active = hasActiveActivityInStore(m_db, normalizedActivityScope(scopeKey),
                                                 QString{}, &error);
    if (!error.isEmpty()) qWarning() << "[Activity] active query failed" << error;
    return active;
}

// 两存储中最早到期（active + review_after<=now）的候选。legacy 只属于 owner；
// internal 要求 scope 匹配且 channel 是 pet/qq_private（与私人回顾一致）。
// R12-1b 补充：候选必须通过模型可见投影；不可见候选在查询层跳过，不阻塞其它合法活动。
QJsonObject MemoryManager::nextDueActivity(const QString &scopeKey) const
{
    if (!m_db.isOpen()) return {};
    const QString scope = normalizedActivityScope(scopeKey);
    // 长期记忆关闭时活动回顾整体不可用：没有任何合法候选。
    if (!ConfigManager::instance().getMemoryPolicyConfig().longTermMemoryEnabled) return {};
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    struct Candidate {
        qlonglong due = -1;
        qlonglong id = 0;
        QString ref;
    };
    QList<Candidate> candidates;
    if (scope == QStringLiteral("owner")) {
        QSqlQuery legacy(m_db);
        legacy.prepare(QStringLiteral(
            "SELECT id,coalesce(review_after,0) FROM chat_history "
            "WHERE thought IS NOT NULL AND thought<>'' AND subject='user' AND sensitive=0 "
            "AND coalesce(json_extract(thought_result,'$.status'),'active')='active' "
            "AND coalesce(review_after,0)<=? "
            "ORDER BY coalesce(review_after,0),id LIMIT 8"));
        legacy.addBindValue(now);
        if (legacy.exec()) {
            while (legacy.next()) {
                const qlonglong id = legacy.value(0).toLongLong();
                candidates.append({legacy.value(1).toLongLong(), id,
                                   QStringLiteral("thought/%1").arg(id)});
            }
        }
    }
    QSqlQuery internal(m_db);
    internal.prepare(QStringLiteral(
        "SELECT id,review_after FROM internal_activity WHERE scope_key=? AND status='active' "
        "AND channel IN ('pet','qq_private') AND subject='user' AND review_after<=? "
        "ORDER BY review_after,id LIMIT 8"));
    internal.addBindValue(scope);
    internal.addBindValue(now);
    if (internal.exec()) {
        while (internal.next()) {
            const qlonglong id = internal.value(0).toLongLong();
            candidates.append({internal.value(1).toLongLong(), id,
                               QStringLiteral("activity/%1").arg(id)});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        if (left.due != right.due) return left.due < right.due;
        if (left.id != right.id) return left.id < right.id;
        return left.ref < right.ref;
    });
    for (const auto &candidate : candidates) {
        ActivityRef parsed;
        if (!ActivityRef::parse(candidate.ref, &parsed)) continue;
        const QJsonObject raw = activityByRef(candidate.ref);
        if (raw.isEmpty()) continue;
        // 投影不可见的活动不进入 ActivityReview 队列。
        const QJsonObject projected = activityForModel(
            candidate.ref, raw.value(QStringLiteral("channel")).toString(),
            raw.value(QStringLiteral("session")).toString(), QStringLiteral("user"), true);
        if (projected.isEmpty()) continue;
        return QJsonObject{{QStringLiteral("ref"), candidate.ref},
                           {QStringLiteral("kind"), parsed.originKind()},
                           {QStringLiteral("origin"), parsed.originKind()},
                           {QStringLiteral("version"),
                            projected.value(QStringLiteral("version")).toInt()},
                           {QStringLiteral("review_after"), candidate.due},
                           {QStringLiteral("activity"), projected}};
    }
    return {};
}

// 统一领取。legacy 复用 claimThoughtReview（预算/冷却/失败恢复都在原实现里）；
// internal 在单事务内 version CAS + 同一份共享预算/冷却 + 公平计数，失败整体回滚。
QJsonObject MemoryManager::claimActivity(const QString &ref, int expectedVersion, int dailyLimit,
                                         int cooldownSeconds, QString *reason)
{
    if (reason) reason->clear();
    const auto fail = [reason](const QString &code) {
        if (reason) *reason = code;
        return QJsonObject{};
    };
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return fail(QStringLiteral("invalid_ref"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (parsed.kind == ActivityRef::LegacyThought) {
        // R12-1b 补充：提交前复核模型可见投影，不可见活动不能被领取。
        const QJsonObject before = activityByRef(parsed.ref());
        if (before.isEmpty()) return fail(QStringLiteral("invalid_ref"));
        if (activityForModel(parsed.ref(), before.value(QStringLiteral("channel")).toString(),
                             before.value(QStringLiteral("session")).toString(),
                             QStringLiteral("user"), true).isEmpty())
            return fail(QStringLiteral("not_visible"));
        QString claimReason;
        QDateTime next;
        if (!claimThoughtReview(parsed.id, expectedVersion, dailyLimit, cooldownSeconds,
                                &claimReason, &next))
            return fail(claimReason.isEmpty() ? QStringLiteral("db_error") : claimReason);
        const QJsonObject view = activityByRef(parsed.ref());
        return QJsonObject{{QStringLiteral("ref"), parsed.ref()},
                           {QStringLiteral("kind"), QStringLiteral("legacy")},
                           {QStringLiteral("origin_kind"), QStringLiteral("legacy")},
                           {QStringLiteral("id"), parsed.id},
                           {QStringLiteral("version"), expectedVersion + 1},
                           {QStringLiteral("activity"), view}};
    }
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery current(m_db);
    current.prepare(QStringLiteral("SELECT * FROM internal_activity WHERE id=?"));
    current.addBindValue(parsed.id);
    if (!current.exec()) return fail(QStringLiteral("db_error"));
    if (!current.next()) return fail(QStringLiteral("cas_conflict"));
    const int currentVersion = current.value(QStringLiteral("version")).toInt();
    const QString currentStatus = current.value(QStringLiteral("status")).toString();
    const qlonglong currentDue = current.value(QStringLiteral("review_after")).toLongLong();
    const QString runId = current.value(QStringLiteral("run_id")).toString();
    const QString scopeKey = current.value(QStringLiteral("scope_key")).toString();
    const QString rowChannel = current.value(QStringLiteral("channel")).toString();
    const QString rowSession = current.value(QStringLiteral("session_id")).toString();
    current.finish();
    if (currentVersion != expectedVersion || currentStatus != QStringLiteral("active"))
        return fail(QStringLiteral("cas_conflict"));
    if (currentDue > now) return fail(QStringLiteral("not_due"));
    // 提交前再次复核投影/来源/状态/版本：不可见活动不进入在途回顾。
    if (scopeKey != QStringLiteral("owner")
        || activityForModel(parsed.ref(), rowChannel, rowSession,
                            QStringLiteral("user"), true).isEmpty())
        return fail(QStringLiteral("not_visible"));
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET review_after=unixepoch()+1800,version=version+1,"
        "updated_at=unixepoch() WHERE id=? AND version=? AND status='active' "
        "AND review_after<=unixepoch()"));
    update.addBindValue(parsed.id);
    update.addBindValue(expectedVersion);
    if (!update.exec()) return fail(QStringLiteral("db_error"));
    if (update.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    // R12-1a 失败暂停到期后的恢复（与 legacy 领取同一套 review_failure 语义）。
    if (!recoverSuspendedFailure(QStringLiteral("activity"), parsed.ref()))
        return fail(QStringLiteral("db_error"));
    QString chargeReason;
    QDateTime nextEligibleAt;
    if (!chargePrivateReview(parsed.id, dailyLimit, cooldownSeconds, &chargeReason,
                             &nextEligibleAt, QStringLiteral("activity"), parsed.ref(), runId, 0))
        return fail(chargeReason.isEmpty() ? QStringLiteral("db_error") : chargeReason);
    if (!bumpOpenWakeFairness()) return fail(QStringLiteral("db_error"));
    QJsonObject view;
    QString loadError;
    if (!loadInternalActivity(m_db, parsed.id, &view, &loadError))
        return fail(QStringLiteral("db_error"));
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    return QJsonObject{{QStringLiteral("ref"), parsed.ref()},
                       {QStringLiteral("kind"), QStringLiteral("internal")},
                       {QStringLiteral("origin_kind"), view.value(QStringLiteral("origin_kind"))},
                       {QStringLiteral("id"), parsed.id},
                       {QStringLiteral("version"), view.value(QStringLiteral("version"))},
                       {QStringLiteral("scope_key"), scopeKey},
                       {QStringLiteral("run_id"), runId},
                       {QStringLiteral("activity"), view}};
}

// 统一提交。internal 单事务 CAS：状态、正文 patch、artifact_ref/版本/哈希、review_after、
// version、share_intent 一次性写入；失败（并发/版本过期/缺真实成果/分享请求非法）整体回滚。
// R12-1b 补充：completed 必须能指向真实成果——文字创作要求已持久化 draft（可沿用），
// 纯思考要求 completion_condition + reflection；外部行为/未执行结果不能写 completed。
QJsonObject MemoryManager::finishActivity(const QString &ref, int version,
                                          const QJsonObject &proposal, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return QJsonObject{};
    };
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return fail(QStringLiteral("invalid_ref"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    // 状态字段先做类型/枚举诊断，避免把缺失/类型错误混成同一个 schema 失败。
    if (!proposal.contains(QStringLiteral("status"))) return fail(QStringLiteral("status_missing"));
    if (!proposal.value(QStringLiteral("status")).isString())
        return fail(QStringLiteral("status_type_invalid"));
    const QString status = proposal.value(QStringLiteral("status")).toString();
    static const QStringList allowedStatuses{QStringLiteral("active"), QStringLiteral("paused"),
                                             QStringLiteral("completed"),
                                             QStringLiteral("abandoned")};
    if (!allowedStatuses.contains(status)) return fail(QStringLiteral("status_enum_invalid"));
    // 分享请求只接受布尔或简短请求文本；不合法时整体拒绝（不与完成状态半写）。
    bool shareRequested = false;
    QString shareRequestText;
    if (proposal.contains(QStringLiteral("share_with_owner"))) {
        const QJsonValue value = proposal.value(QStringLiteral("share_with_owner"));
        if (!value.isBool()) return fail(QStringLiteral("share_intent_invalid"));
        shareRequested = value.toBool();
    }
    if (proposal.contains(QStringLiteral("share_request"))) {
        const QJsonValue value = proposal.value(QStringLiteral("share_request"));
        if (value.isBool()) {
            shareRequested = shareRequested || value.toBool();
        } else if (value.isString()) {
            const QString text = value.toString().trimmed();
            if (text.size() > 120) return fail(QStringLiteral("share_intent_invalid"));
            if (!text.isEmpty()) {
                shareRequested = true;
                shareRequestText = text;
            }
        } else {
            return fail(QStringLiteral("share_intent_invalid"));
        }
    }
    if (shareRequested && status != QStringLiteral("completed"))
        return fail(QStringLiteral("share_intent_invalid"));

    if (parsed.kind == ActivityRef::LegacyThought) {
        // legacy 活动：只做适配与确定性校验，旧分享链（canShareThought → Outbox）保持不变。
        const QJsonObject before = activityByRef(parsed.ref());
        if (before.isEmpty()) return fail(QStringLiteral("invalid_or_stale_proposal"));
        if (activityForModel(parsed.ref(), before.value(QStringLiteral("channel")).toString(),
                             before.value(QStringLiteral("session")).toString(),
                             QStringLiteral("user"), true).isEmpty())
            return fail(QStringLiteral("not_visible"));
        if (status == QStringLiteral("completed")) {
            const ActivityCompletionPlan plan = planActivityCompletion(before, proposal);
            if (!plan.valid) return fail(plan.error);
        }
        if (!finishThoughtReview(parsed.id, version, proposal))
            return fail(QStringLiteral("invalid_or_stale_proposal"));
        const QJsonObject view = activityByRef(parsed.ref());
        QJsonObject result{{QStringLiteral("ref"), parsed.ref()},
                           {QStringLiteral("status"), view.value(QStringLiteral("status"))},
                           {QStringLiteral("version"), version + 1}};
        for (const auto &key : {QStringLiteral("artifact_ref"), QStringLiteral("artifact_version"),
                                QStringLiteral("artifact_hash"),
                                QStringLiteral("completed_at")}) {
            if (view.contains(key)) result.insert(key, view.value(key));
        }
        qInfo().noquote()
            << QStringLiteral("[Activity] finished ref=%1 status=%2 version=%3 artifact=%4")
                   .arg(parsed.ref(), status)
                   .arg(version + 1)
                   .arg(view.value(QStringLiteral("artifact_ref")).toString().isEmpty()
                            ? QStringLiteral("-") : view.value(QStringLiteral("artifact_ref")).toString());
        return result;
    }

    // internal 活动：draft 是"完整最新稿"，截断会静默丢稿，超长直接拒绝。
    // legacy 仍交给 finishThoughtReview 的旧校验（保持 invalid_or_stale_proposal 兼容）。
    const QString draft = proposal.value(QStringLiteral("draft")).toString().trimmed();
    if (draft.size() > 800) return fail(QStringLiteral("draft_too_long"));
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    const int hours = qBound(1, proposal.value(QStringLiteral("review_hours")).toInt(24), 168);
    const qlonglong reviewAfter =
        (status == QStringLiteral("completed") || status == QStringLiteral("abandoned"))
        ? 0 : now + qlonglong(hours) * 3600;
    const bool completed = status == QStringLiteral("completed");
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery current(m_db);
    current.prepare(QStringLiteral("SELECT * FROM internal_activity WHERE id=?"));
    current.addBindValue(parsed.id);
    if (!current.exec()) return fail(QStringLiteral("db_error"));
    if (!current.next()) return fail(QStringLiteral("commit_conflict"));
    const int currentVersion = current.value(QStringLiteral("version")).toInt();
    const QString currentStatus = current.value(QStringLiteral("status")).toString();
    const QString rowChannel = current.value(QStringLiteral("channel")).toString();
    const QString rowSession = current.value(QStringLiteral("session_id")).toString();
    const QJsonObject stored = QJsonDocument::fromJson(
        current.value(QStringLiteral("result_json")).toString().toUtf8()).object();
    current.finish();
    if (currentVersion != version
        || (currentStatus != QStringLiteral("active")
            && currentStatus != QStringLiteral("paused")))
        return fail(QStringLiteral("commit_conflict"));
    // 提交前再次复核模型可见投影：不可见活动不能借提交写回状态/成果。
    if (activityForModel(parsed.ref(), rowChannel, rowSession, QStringLiteral("user"), true)
            .isEmpty())
        return fail(QStringLiteral("not_visible"));
    ActivityCompletionPlan plan;
    if (completed) {
        plan = planActivityCompletion(stored, proposal);
        if (!plan.valid) return fail(plan.error);
    }
    QJsonObject patch{{QStringLiteral("deferred_reason"), QJsonValue::Null},
                      {QStringLiteral("paused_reason"), QJsonValue::Null}};
    for (const auto &key : {QStringLiteral("reflection"), QStringLiteral("goal"),
                            QStringLiteral("progress"), QStringLiteral("next_step"),
                            QStringLiteral("mood"), QStringLiteral("paused_reason")})
        insertActivityText(&patch, key, proposal.value(key), 200);
    // 创作中的实际草稿也必须在 active/paused 阶段持久化；否则后续只能引用“已存在成果”
    // 完成时会误判 missing_artifact（draft 被静默丢掉）。
    insertActivityText(&patch, QStringLiteral("draft"),
                       proposal.value(QStringLiteral("draft")), 800);
    // completion_condition/activity_kind 在 active/paused 阶段也要持久化，后续完成时沿用。
    if (proposal.value(QStringLiteral("completion_condition")).isString())
        insertActivityText(&patch, QStringLiteral("completion_condition"),
                           proposal.value(QStringLiteral("completion_condition")), 200);
    const QString requestedKind =
        normalizedActivityKind(proposal.value(QStringLiteral("activity_kind")).toString());
    if (!requestedKind.isEmpty()) {
        if (requestedKind != QStringLiteral("writing")
            && requestedKind != QStringLiteral("thought"))
            return fail(isUnsupportedActivityKind(requestedKind)
                            ? QStringLiteral("unsupported_external_activity")
                            : QStringLiteral("invalid_activity_kind"));
        patch.insert(QStringLiteral("activity_kind"), requestedKind);
    }
    const int newVersion = currentVersion + 1;
    bool setArtifact = false;
    bool clearArtifact = false;
    QString artifactRef;
    int artifactVersion = 0;
    QString artifactHash;
    if (completed) {
        patch.insert(QStringLiteral("completed_at"), now);
        if (plan.kind == QStringLiteral("writing")) {
            // 实际成果正文：本次新稿或已持久化的完整稿；服务端写入权威产物引用/版本/哈希。
            setArtifact = true;
            artifactRef = parsed.ref();
            artifactVersion = newVersion;
            artifactHash = activityContentHash(plan.draft);
            patch.insert(QStringLiteral("draft"), plan.draft.left(800));
            patch.insert(QStringLiteral("activity_kind"), QStringLiteral("writing"));
            patch.insert(QStringLiteral("artifact_ref"), artifactRef);
            patch.insert(QStringLiteral("artifact_version"), artifactVersion);
            patch.insert(QStringLiteral("artifact_hash"), artifactHash);
            if (!plan.completionCondition.isEmpty())
                patch.insert(QStringLiteral("completion_condition"),
                             plan.completionCondition.left(200));
        } else {
            // 纯思考：不写产物，只保留完成条件与认识；不声称外部执行结果。
            clearArtifact = true;
            patch.insert(QStringLiteral("activity_kind"), QStringLiteral("thought"));
            patch.insert(QStringLiteral("completion_condition"),
                         plan.completionCondition.left(200));
            patch.insert(QStringLiteral("artifact_ref"), QJsonValue::Null);
            patch.insert(QStringLiteral("artifact_version"), QJsonValue::Null);
            patch.insert(QStringLiteral("artifact_hash"), QJsonValue::Null);
        }
    }
    if (shareRequested) {
        // 分享意愿与完成状态同一事务：只能绑定服务端实际成果 ref/版本/哈希，
        // 不接受模型传入的任意 draft；旧 intent 保留到有界历史。
        if (!setArtifact) return fail(QStringLiteral("missing_artifact"));
        const QJsonObject previousIntent = stored.value(QStringLiteral("share_intent")).toObject();
        const int previousShareVersion =
            previousIntent.value(QStringLiteral("version")).toInt(0);
        QJsonArray history =
            previousIntent.value(QStringLiteral("history")).toArray();
        if (previousShareVersion > 0) {
            QJsonObject previous = previousIntent;
            previous.remove(QStringLiteral("history"));
            history.append(previous);
            while (history.size() > 4) history.removeFirst();
        }
        QJsonObject intent{
            {QStringLiteral("state"), QStringLiteral("saved")},
            {QStringLiteral("artifact_ref"), artifactRef},
            {QStringLiteral("artifact_version"), artifactVersion},
            {QStringLiteral("content_hash"), artifactHash},
            {QStringLiteral("requested_at"), now},
            {QStringLiteral("version"), previousShareVersion + 1}};
        if (!shareRequestText.isEmpty())
            intent.insert(QStringLiteral("request"), shareRequestText.left(120));
        if (!history.isEmpty()) intent.insert(QStringLiteral("history"), history);
        patch.insert(QStringLiteral("share_intent"), intent);
        patch.insert(QStringLiteral("share_offered"), true);
    }
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET status=?,"
        "result_json=json_patch(coalesce(result_json,'{}'),?),"
        "version=version+1,review_after=?,updated_at=?,"
        "artifact_ref=CASE WHEN ?=1 THEN ? WHEN ?=1 THEN NULL ELSE artifact_ref END "
        "WHERE id=? AND version=? AND status IN ('active','paused')"));
    update.addBindValue(status);
    update.addBindValue(QString::fromUtf8(QJsonDocument(patch).toJson(QJsonDocument::Compact)));
    update.addBindValue(reviewAfter);
    update.addBindValue(now);
    update.addBindValue(setArtifact ? 1 : 0);
    update.addBindValue(artifactRef);
    update.addBindValue(clearArtifact ? 1 : 0);
    update.addBindValue(parsed.id);
    update.addBindValue(version);
    if (!update.exec()) return fail(QStringLiteral("db_error"));
    if (update.numRowsAffected() != 1) return fail(QStringLiteral("commit_conflict"));
    QJsonObject result{{QStringLiteral("ref"), parsed.ref()},
                       {QStringLiteral("status"), status},
                       {QStringLiteral("version"), newVersion}};
    if (setArtifact) {
        result.insert(QStringLiteral("artifact_ref"), artifactRef);
        result.insert(QStringLiteral("artifact_version"), artifactVersion);
        result.insert(QStringLiteral("artifact_hash"), artifactHash);
    }
    if (completed) result.insert(QStringLiteral("completed_at"), now);
    if (shareRequested) result.insert(QStringLiteral("share_intent"), patch.value(QStringLiteral("share_intent")));
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    qInfo().noquote()
        << QStringLiteral("[Activity] finished ref=%1 status=%2 version=%3 artifact=%4"
                          " artifact_version=%5 share_intent=%6")
               .arg(parsed.ref(), status)
               .arg(newVersion)
               .arg(setArtifact ? artifactRef : QStringLiteral("-"))
               .arg(setArtifact ? artifactVersion : 0)
               .arg(shareRequested ? QStringLiteral("saved") : QStringLiteral("-"));
    return result;
}

// 统一延后：不改 lifecycle，只推下一次回顾时间（0=立即可再领取）。
bool MemoryManager::deferActivity(const QString &ref, int version, int retrySeconds,
                                  const QString &reason)
{
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return false;
    if (!m_db.isOpen()) return false;
    if (parsed.kind == ActivityRef::LegacyThought)
        return deferThoughtReview(parsed.id, version, reason);
    const int retry = qBound(0, retrySeconds, 7 * 24 * 3600);
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET review_after=unixepoch()+?,version=version+1,"
        "updated_at=unixepoch(),"
        "result_json=json_set(coalesce(result_json,'{}'),'$.deferred_reason',?) "
        "WHERE id=? AND version=? AND status IN ('active','paused')"));
    update.addBindValue(retry);
    update.addBindValue(reason.trimmed().left(240));
    update.addBindValue(parsed.id);
    update.addBindValue(version);
    return update.exec() && update.numRowsAffected() == 1;
}

// 统一动作。legacy 复用 applyThoughtAction（完成必须走 finishActivity 带产物）；
// internal 做显式状态机 + version CAS，resume 先做跨存储 active/paused 检查。
QJsonObject MemoryManager::applyActivityAction(const QString &ref, int expectedVersion,
                                               const QString &action, int afterMinutes,
                                               const QString &reason, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return QJsonObject{};
    };
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return fail(QStringLiteral("invalid_ref"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    const QString trimmedAction = action.trimmed();
    if (trimmedAction != QStringLiteral("pause") && trimmedAction != QStringLiteral("resume")
        && trimmedAction != QStringLiteral("abandon") && trimmedAction != QStringLiteral("complete")
        && trimmedAction != QStringLiteral("reschedule"))
        return fail(QStringLiteral("invalid_action"));
    // 动作提交前复核模型可见投影：不可见活动不能被前台/回顾借动作改状态。
    const QJsonObject visibilityView = activityByRef(parsed.ref());
    if (visibilityView.isEmpty()) return fail(QStringLiteral("invalid_ref"));
    if (activityForModel(parsed.ref(), visibilityView.value(QStringLiteral("channel")).toString(),
                         visibilityView.value(QStringLiteral("session")).toString(),
                         QStringLiteral("user"), true).isEmpty())
        return fail(QStringLiteral("not_visible"));
    if (parsed.kind == ActivityRef::LegacyThought) {
        // legacy 的 applyThoughtAction 只支持 pause/resume/abandon/reschedule；
        // "complete" 必须带产物走 finishActivity，不能在这里伪造完成。
        if (trimmedAction == QStringLiteral("complete"))
            return fail(QStringLiteral("unsupported_action"));
        // R12-1b：resume 与 internal 走同一跨存储单 active 检查（排除自身后仍有当前活动
        // 就拒绝，绝不把双开写回来）。
        if (trimmedAction == QStringLiteral("resume")) {
            QString activeError;
            if (hasActiveActivityInStore(m_db, QStringLiteral("owner"), parsed.ref(),
                                         &activeError))
                return fail(QStringLiteral("already_active"));
            if (!activeError.isEmpty()) return fail(QStringLiteral("db_error"));
        }
        if (!applyThoughtAction(parsed.id, expectedVersion, trimmedAction, afterMinutes, reason))
            return fail(QStringLiteral("invalid_or_stale"));
        const QJsonObject view = activityByRef(parsed.ref());
        return QJsonObject{{QStringLiteral("ref"), parsed.ref()},
                           {QStringLiteral("origin"), QStringLiteral("legacy")},
                           {QStringLiteral("status"), view.value(QStringLiteral("status"))},
                           {QStringLiteral("version"), view.value(QStringLiteral("version"))}};
    }
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery current(m_db);
    current.prepare(QStringLiteral("SELECT * FROM internal_activity WHERE id=?"));
    current.addBindValue(parsed.id);
    if (!current.exec()) return fail(QStringLiteral("db_error"));
    if (!current.next()) return fail(QStringLiteral("cas_conflict"));
    const int currentVersion = current.value(QStringLiteral("version")).toInt();
    const QString currentStatus = current.value(QStringLiteral("status")).toString();
    const QString scopeKey = current.value(QStringLiteral("scope_key")).toString();
    const qlonglong currentReviewAfter = current.value(QStringLiteral("review_after")).toLongLong();
    const QJsonObject stored = QJsonDocument::fromJson(
        current.value(QStringLiteral("result_json")).toString().toUtf8()).object();
    current.finish();
    if (currentVersion != expectedVersion) return fail(QStringLiteral("cas_conflict"));
    const bool currentlyOpen = currentStatus == QStringLiteral("active")
        || currentStatus == QStringLiteral("paused");
    if (!currentlyOpen) return fail(QStringLiteral("invalid_transition"));
    QString newStatus = currentStatus;
    qlonglong newReviewAfter = currentReviewAfter;
    bool setArtifact = false;
    bool clearArtifact = false;
    QString artifactRef;
    int artifactVersion = 0;
    QString artifactHash;
    const QString trimmedReason = reason.trimmed().left(200);
    QJsonObject patch{{QStringLiteral("last_action"), trimmedAction}};
    if (trimmedAction == QStringLiteral("pause")) {
        if (currentStatus != QStringLiteral("active")) return fail(QStringLiteral("invalid_transition"));
        newStatus = QStringLiteral("paused");
        patch.insert(QStringLiteral("paused_reason"),
                     trimmedReason.isEmpty() ? QStringLiteral("user_paused") : trimmedReason);
    } else if (trimmedAction == QStringLiteral("resume")) {
        if (currentStatus != QStringLiteral("paused")) return fail(QStringLiteral("invalid_transition"));
        QString activeError;
        // 跨存储检查（排除自身）：历史数据里若遗留多个 active，resume 不能把双开写回来。
        if (hasActiveActivityInStore(m_db, scopeKey, parsed.ref(), &activeError))
            return fail(QStringLiteral("already_active"));
        if (!activeError.isEmpty()) return fail(QStringLiteral("db_error"));
        if (afterMinutes > 10080) return fail(QStringLiteral("invalid_argument"));
        newStatus = QStringLiteral("active");
        newReviewAfter = now + (afterMinutes > 0 ? qlonglong(afterMinutes) * 60 : 1800);
        patch.insert(QStringLiteral("paused_reason"), QJsonValue::Null);
        if (!trimmedReason.isEmpty())
            patch.insert(QStringLiteral("reason"), trimmedReason);
    } else if (trimmedAction == QStringLiteral("abandon")) {
        newStatus = QStringLiteral("abandoned");
        newReviewAfter = 0;
        if (!trimmedReason.isEmpty())
            patch.insert(QStringLiteral("reason"), trimmedReason);
    } else if (trimmedAction == QStringLiteral("complete")) {
        // 与 finishActivity 使用同一套成果真实性规则：文字创作要有实际 draft，
        // 纯思考要有 completion_condition + reflection；仅有想法/进度不能完成。
        QJsonObject storedView = stored;
        storedView.insert(QStringLiteral("ref"), parsed.ref());
        const ActivityCompletionPlan plan = planActivityCompletion(storedView, QJsonObject{});
        if (!plan.valid) return fail(plan.error);
        newStatus = QStringLiteral("completed");
        newReviewAfter = 0;
        if (!trimmedReason.isEmpty())
            patch.insert(QStringLiteral("reason"), trimmedReason);
        patch.insert(QStringLiteral("completed_at"), now);
        if (plan.kind == QStringLiteral("writing")) {
            setArtifact = true;
            artifactRef = parsed.ref();
            artifactVersion = expectedVersion + 1;
            artifactHash = activityContentHash(plan.draft);
            patch.insert(QStringLiteral("draft"), plan.draft.left(800));
            patch.insert(QStringLiteral("activity_kind"), QStringLiteral("writing"));
            patch.insert(QStringLiteral("artifact_ref"), artifactRef);
            patch.insert(QStringLiteral("artifact_version"), artifactVersion);
            patch.insert(QStringLiteral("artifact_hash"), artifactHash);
            if (!plan.completionCondition.isEmpty())
                patch.insert(QStringLiteral("completion_condition"),
                             plan.completionCondition.left(200));
        } else {
            clearArtifact = true;
            patch.insert(QStringLiteral("activity_kind"), QStringLiteral("thought"));
            patch.insert(QStringLiteral("completion_condition"),
                         plan.completionCondition.left(200));
            patch.insert(QStringLiteral("artifact_ref"), QJsonValue::Null);
            patch.insert(QStringLiteral("artifact_version"), QJsonValue::Null);
            patch.insert(QStringLiteral("artifact_hash"), QJsonValue::Null);
        }
    } else { // reschedule
        if (afterMinutes < 0 || afterMinutes > 10080)
            return fail(QStringLiteral("invalid_argument"));
        newReviewAfter = now + qlonglong(afterMinutes) * 60;
        if (!trimmedReason.isEmpty())
            patch.insert(QStringLiteral("schedule_reason"), trimmedReason);
    }
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET status=?,review_after=?,"
        "result_json=json_patch(coalesce(result_json,'{}'),?),version=version+1,updated_at=?,"
        "artifact_ref=CASE WHEN ?=1 THEN ? WHEN ?=1 THEN NULL ELSE artifact_ref END "
        "WHERE id=? AND version=? AND status IN ('active','paused')"));
    update.addBindValue(newStatus);
    update.addBindValue(newReviewAfter);
    update.addBindValue(QString::fromUtf8(QJsonDocument(patch).toJson(QJsonDocument::Compact)));
    update.addBindValue(now);
    update.addBindValue(setArtifact ? 1 : 0);
    update.addBindValue(artifactRef);
    update.addBindValue(clearArtifact ? 1 : 0);
    update.addBindValue(parsed.id);
    update.addBindValue(expectedVersion);
    if (!update.exec()) return fail(QStringLiteral("db_error"));
    if (update.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    // R12-2c-3：成果撤回（放弃活动）是行动语境变化：旧在途分享/联系提案按 fence epoch 失效，
    // 历史活动/成果行保留不删除。完成/暂停/resume 等生命周期常规推进不额外 bump。
    if (newStatus == QStringLiteral("abandoned"))
        bumpActionContextEpoch(QStringLiteral("activity_abandoned"));
    QJsonObject result{{QStringLiteral("ref"), parsed.ref()},
                       {QStringLiteral("origin"), QStringLiteral("internal")},
                       {QStringLiteral("scope_key"), scopeKey},
                       {QStringLiteral("status"), newStatus},
                       {QStringLiteral("version"), expectedVersion + 1}};
    if (setArtifact) {
        result.insert(QStringLiteral("artifact_ref"), artifactRef);
        result.insert(QStringLiteral("artifact_version"), artifactVersion);
        result.insert(QStringLiteral("artifact_hash"), artifactHash);
    }
    return result;
}

// 分享意愿：只写 internal_activity.result_json.share_intent（不发送、不建账本/outbox/聊天行）。
// 旧签名保留兼容：等价于 action=share、自动递增 share 版本、request 最多 120 字。
bool MemoryManager::setActivityShareIntent(const QString &ref, int version,
                                           const QString &draft, QString *error)
{
    return setActivityShareIntent(ref, version, QStringLiteral("share"), draft, -1, error);
}

// R12-1b 补充：显式分享版本语义（修改/撤回/重新分享）。只接受当前已完成成果的
// artifact_ref/artifact_version/artifact_hash 匹配（重新计算正文哈希核对），
// 不接受任意 draft 冒充成果，也不允许无成果分享；withdraw 保留历史并置 withdrawn。
bool MemoryManager::setActivityShareIntent(const QString &ref, int expectedVersion,
                                           const QString &action, const QString &request,
                                           int shareVersion, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return fail(QStringLiteral("invalid_ref"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (parsed.kind == ActivityRef::LegacyThought)
        return fail(QStringLiteral("unsupported_legacy"));
    const QString trimmedAction = action.trimmed();
    if (trimmedAction != QStringLiteral("share") && trimmedAction != QStringLiteral("modify")
        && trimmedAction != QStringLiteral("withdraw"))
        return fail(QStringLiteral("invalid_action"));
    const QString trimmedRequest = request.trimmed();
    if (trimmedRequest.size() > 120) return fail(QStringLiteral("invalid_request"));
    if (trimmedAction != QStringLiteral("withdraw") && trimmedRequest.isEmpty())
        return fail(QStringLiteral("invalid_request"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery current(m_db);
    current.prepare(QStringLiteral("SELECT * FROM internal_activity WHERE id=?"));
    current.addBindValue(parsed.id);
    if (!current.exec()) return fail(QStringLiteral("db_error"));
    if (!current.next()) return fail(QStringLiteral("missing_artifact"));
    const int currentVersion = current.value(QStringLiteral("version")).toInt();
    const QString currentStatus = current.value(QStringLiteral("status")).toString();
    const QJsonObject stored = QJsonDocument::fromJson(
        current.value(QStringLiteral("result_json")).toString().toUtf8()).object();
    current.finish();
    if (currentVersion != expectedVersion) return fail(QStringLiteral("cas_conflict"));
    // 没有完成、没有真实成果的活动不能产生分享意愿。
    if (currentStatus != QStringLiteral("completed")) return fail(QStringLiteral("missing_artifact"));
    const QString storedDraft = stored.value(QStringLiteral("draft")).toString().trimmed();
    const QString storedArtifactRef =
        stored.value(QStringLiteral("artifact_ref")).toString().trimmed();
    const int storedArtifactVersion =
        stored.value(QStringLiteral("artifact_version")).toInt(0);
    const QString storedHash = stored.value(QStringLiteral("artifact_hash")).toString().trimmed();
    if (storedDraft.isEmpty() || storedArtifactRef != parsed.ref()
        || storedArtifactVersion <= 0 || storedHash.size() != 16)
        return fail(QStringLiteral("missing_artifact"));
    // 只能绑定当前成果版本：正文哈希必须与持久化哈希一致，旧版本/任意 draft 不能冒充。
    if (activityContentHash(storedDraft) != storedHash)
        return fail(QStringLiteral("artifact_mismatch"));
    const QJsonObject previousIntent = stored.value(QStringLiteral("share_intent")).toObject();
    const int previousShareVersion = previousIntent.value(QStringLiteral("version")).toInt(0);
    if (trimmedAction == QStringLiteral("modify")
        && previousIntent.value(QStringLiteral("state")).toString() != QStringLiteral("saved"))
        return fail(QStringLiteral("invalid_transition"));
    const int newShareVersion =
        shareVersion > 0 ? shareVersion : previousShareVersion + 1;
    if (newShareVersion <= previousShareVersion)
        return fail(QStringLiteral("share_version_conflict"));
    QJsonArray history = previousIntent.value(QStringLiteral("history")).toArray();
    if (previousShareVersion > 0) {
        QJsonObject previous = previousIntent;
        previous.remove(QStringLiteral("history"));
        history.append(previous);
        while (history.size() > 4) history.removeFirst();
    }
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    QJsonObject intent{
        {QStringLiteral("state"), trimmedAction == QStringLiteral("withdraw")
             ? QStringLiteral("withdrawn") : QStringLiteral("saved")},
        {QStringLiteral("artifact_ref"), storedArtifactRef},
        {QStringLiteral("artifact_version"), storedArtifactVersion},
        {QStringLiteral("content_hash"), storedHash},
        {QStringLiteral("version"), newShareVersion},
        {QStringLiteral("requested_at"), now}};
    if (trimmedAction == QStringLiteral("withdraw"))
        intent.insert(QStringLiteral("withdrawn_at"), now);
    else
        intent.insert(QStringLiteral("request"), trimmedRequest.left(120));
    if (!history.isEmpty()) intent.insert(QStringLiteral("history"), history);
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET result_json=json_set(coalesce(result_json,'{}'),"
        "'$.share_intent',json(?),'$.share_offered',json(?)),updated_at=unixepoch() "
        "WHERE id=? AND version=? AND status='completed'"));
    update.addBindValue(QString::fromUtf8(
        QJsonDocument(intent).toJson(QJsonDocument::Compact)));
    update.addBindValue(trimmedAction == QStringLiteral("withdraw") ? QStringLiteral("false")
                                                                   : QStringLiteral("true"));
    update.addBindValue(parsed.id);
    update.addBindValue(expectedVersion);
    if (!update.exec()) return fail(QStringLiteral("db_error"));
    if (update.numRowsAffected() != 1) return fail(QStringLiteral("cas_conflict"));
    if (!m_db.commit()) return fail(QStringLiteral("db_error"));
    rollback.dismiss();
    qInfo().noquote()
        << QStringLiteral("[Activity] share_intent action=%1 ref=%2 share_version=%3"
                          " artifact_version=%4")
               .arg(trimmedAction, parsed.ref())
               .arg(newShareVersion)
               .arg(storedArtifactVersion);
    // R12-2c-2：成果撤回是 Action Context Fence 的触发点——相关分享语境已变，
    // 已在途/已入 Outbox 未发送的旧分享提案按 epoch 不一致失效（历史成果保留不删除）。
    if (trimmedAction == QStringLiteral("withdraw"))
        bumpActionContextEpoch(QStringLiteral("share_withdrawn"));
    return true;
}

// 启动恢复诊断：只读、幂等。报告跨存储 active/paused 冲突与最早到期列表，
// 不删除活动、不伪造完成、不发消息。
QJsonArray MemoryManager::recoverActivities(const QString &scopeKey)
{
    QJsonArray result;
    if (!m_db.isOpen()) return result;
    const QString scope = normalizedActivityScope(scopeKey);
    const qlonglong now = QDateTime::currentSecsSinceEpoch();
    QJsonArray legacyCurrent;
    if (scope == QStringLiteral("owner")) {
        QSqlQuery legacy(m_db);
        legacy.prepare(QStringLiteral(
            "SELECT id FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
            "AND subject='user' AND sensitive=0 "
            "AND coalesce(json_extract(thought_result,'$.status'),'active') "
            "IN ('active','paused') ORDER BY id"));
        if (legacy.exec()) while (legacy.next())
            legacyCurrent.append(QStringLiteral("thought/%1").arg(legacy.value(0).toLongLong()));
    }
    QJsonArray internalCurrent;
    QSqlQuery internal(m_db);
    internal.prepare(QStringLiteral(
        "SELECT id,version,status,review_after FROM internal_activity "
        "WHERE scope_key=? AND status IN ('active','paused') ORDER BY id"));
    internal.addBindValue(scope);
    if (internal.exec()) while (internal.next())
        internalCurrent.append(QStringLiteral("activity/%1").arg(internal.value(0).toLongLong()));
    if (!legacyCurrent.isEmpty()) {
        result.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("current")},
                                  {QStringLiteral("origin"), QStringLiteral("legacy")},
                                  {QStringLiteral("scope_key"), scope},
                                  {QStringLiteral("count"), legacyCurrent.size()},
                                  {QStringLiteral("refs"), legacyCurrent}});
    }
    if (!internalCurrent.isEmpty()) {
        result.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("current")},
                                  {QStringLiteral("origin"), QStringLiteral("internal")},
                                  {QStringLiteral("scope_key"), scope},
                                  {QStringLiteral("count"), internalCurrent.size()},
                                  {QStringLiteral("refs"), internalCurrent}});
    }
    if (!legacyCurrent.isEmpty() && !internalCurrent.isEmpty()) {
        // 历史并发可能写出双开：只报告，不自动删除/合并，交给调用方按可审计流程处理。
        result.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("cross_store_conflict")},
                                  {QStringLiteral("scope_key"), scope},
                                  {QStringLiteral("legacy_refs"), legacyCurrent},
                                  {QStringLiteral("internal_refs"), internalCurrent},
                                  {QStringLiteral("action"), QStringLiteral("none")}});
    }
    // legacy 到期列表（只在 owner 域）。
    if (scope == QStringLiteral("owner")) {
        QSqlQuery dueLegacy(m_db);
        dueLegacy.prepare(QStringLiteral(
            "SELECT id,thought_version,coalesce(review_after,0) FROM chat_history "
            "WHERE thought IS NOT NULL AND thought<>'' AND subject='user' AND sensitive=0 "
            "AND coalesce(json_extract(thought_result,'$.status'),'active')='active' "
            "AND coalesce(review_after,0)<=? ORDER BY coalesce(review_after,0),id"));
        dueLegacy.addBindValue(now);
        if (dueLegacy.exec()) while (dueLegacy.next()) {
            const qlonglong id = dueLegacy.value(0).toLongLong();
            result.append(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("due")},
                {QStringLiteral("ref"), QStringLiteral("thought/%1").arg(id)},
                {QStringLiteral("origin"), QStringLiteral("legacy")},
                {QStringLiteral("version"), dueLegacy.value(1).toInt()},
                {QStringLiteral("review_after"), dueLegacy.value(2).toLongLong()}});
        }
    }
    QSqlQuery dueInternal(m_db);
    dueInternal.prepare(QStringLiteral(
        "SELECT id,version,review_after FROM internal_activity "
        "WHERE scope_key=? AND status='active' AND channel IN ('pet','qq_private') "
        "AND review_after<=? ORDER BY review_after,id"));
    dueInternal.addBindValue(scope);
    dueInternal.addBindValue(now);
    if (dueInternal.exec()) while (dueInternal.next()) {
        const qlonglong id = dueInternal.value(0).toLongLong();
        result.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("due")},
                                  {QStringLiteral("ref"), QStringLiteral("activity/%1").arg(id)},
                                  {QStringLiteral("origin"), QStringLiteral("internal")},
                                  {QStringLiteral("version"), dueInternal.value(1).toInt()},
                                  {QStringLiteral("review_after"), dueInternal.value(2).toLongLong()}});
    }
    if (result.isEmpty()) {
        result.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("clean")},
                                  {QStringLiteral("scope_key"), scope}});
    }
    return result;
}

// ============================ 认知快照只读查询 ============================
// 这一组方法只读：不加表、不改迁移、不写库。所有文本都先过 MemoryReadPolicy
// （盲区 + 敏感内容规则），敏感行（sensitive=1）直接排除；时间统一输出 UTC ISO。
namespace {

QString snapshotIsoFromEpoch(qint64 seconds)
{
    return QDateTime::fromSecsSinceEpoch(seconds).toUTC().toString(Qt::ISODateWithMs);
}

// 库里的时间有两种：chat_history.timestamp 是本地 datetime 字符串，
// cognitive_source.reported / attention_state.value 是 UTC ISO。两种都归一成 UTC。
QDateTime snapshotStoredTimeUtc(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return {};
    QDateTime value = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!value.isValid()) value = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!value.isValid())
        value = QDateTime::fromString(trimmed, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    return value.isValid() ? value.toUTC() : QDateTime{};
}

QString snapshotIsoText(const QString &stored)
{
    const QDateTime value = snapshotStoredTimeUtc(stored);
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : stored.trimmed();
}

// 她说过的话：raw_reply 是 {sentences:[{zh_text,...}]}；老数据可能是纯文本。
QString snapshotExpressionText(const QString &rawReply)
{
    const QString raw = rawReply.trimmed();
    if (raw.isEmpty()) return {};
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
    if (document.isObject()) {
        QStringList parts;
        for (const auto &value : document.object().value(QStringLiteral("sentences")).toArray()) {
            const auto text = value.toObject().value(QStringLiteral("zh_text")).toString().trimmed();
            if (!text.isEmpty()) parts.append(text);
        }
        if (!parts.isEmpty()) return parts.join(QLatin1Char('\n'));
    }
    return raw.startsWith(QLatin1Char('{')) ? QString{} : raw;
}

} // namespace

QJsonArray MemoryManager::recentOwnerObservations(const QString &subject,const QString &channel,
                                                  const QString &session,int limit,
                                                  int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || subject.isEmpty() || limit<=0) return rows;
    const MemoryReadPolicy policy;
    const bool hasChannel=!channel.isEmpty();
    const bool hasSession=hasChannel && !session.isEmpty();
    QString sql = QStringLiteral(
        "SELECT h.id,coalesce(s.reported,h.timestamp),h.user_input,h.subject,h.channel,h.session_id "
        "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
        "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input "
        "WHERE h.subject=? AND h.sensitive=0 AND h.turn_kind='reply' "
        "AND h.user_input IS NOT NULL AND h.user_input<>''");
    if (hasSession)
        sql += QStringLiteral(" AND h.channel=? AND h.session_id=?");
    else if (hasChannel)
        sql += QStringLiteral(" AND h.channel=?");
    else
        // 回顾上下文可能只知道“这是主人私聊来源”：按主人私聊域读取，绝不扩到群聊。
        sql += QStringLiteral(" AND h.channel IN ('pet','qq_private')");
    sql += QStringLiteral(" ORDER BY h.id DESC LIMIT ?");
    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(subject);
    if (hasChannel) q.addBindValue(channel);
    if (hasSession) q.addBindValue(session);
    q.addBindValue(limit);
    if (!q.exec()) return rows;
    while (q.next()) {
        if (queried) ++*queried;
        const auto text=q.value(2).toString();
        const auto rowChannel=q.value(4).toString();
        const auto rowSession=q.value(5).toString();
        if (!policy.allows(rowChannel,rowSession,text)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("chat/%1").arg(q.value(0).toLongLong())},
                                {QStringLiteral("subject"),q.value(3).toString()},
                                {QStringLiteral("channel"),rowChannel},
                                {QStringLiteral("session"),rowSession},
                                {QStringLiteral("occurred_at"),snapshotIsoText(q.value(1).toString())},
                                {QStringLiteral("text"),text.left(512)},
                                {QStringLiteral("sensitive"),false}});
        if (eligible) ++*eligible;
    }
    return rows;
}

QJsonArray MemoryManager::recentSelfExpressions(const QString &subject,const QString &channel,
                                                const QString &session,int limit,
                                                int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || subject.isEmpty() || limit<=0) return rows;
    const MemoryReadPolicy policy;
    const bool hasChannel=!channel.isEmpty();
    const bool hasSession=hasChannel && !session.isEmpty();
    QString sql = QStringLiteral(
        "SELECT h.id,h.turn_kind,h.raw_reply,coalesce(s.reported,h.timestamp),h.channel,h.session_id "
        "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
        "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input "
        "WHERE h.subject=? AND h.sensitive=0 AND h.turn_kind IN ('reply','proactive') "
        "AND h.raw_reply IS NOT NULL AND h.raw_reply<>''");
    if (hasSession)
        sql += QStringLiteral(" AND h.channel=? AND h.session_id=?");
    else if (hasChannel)
        sql += QStringLiteral(" AND h.channel=?");
    else
        sql += QStringLiteral(" AND h.channel IN ('pet','qq_private')");
    sql += QStringLiteral(" ORDER BY h.id DESC LIMIT ?");
    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(subject);
    if (hasChannel) q.addBindValue(channel);
    if (hasSession) q.addBindValue(session);
    q.addBindValue(limit);
    if (!q.exec()) return rows;
    while (q.next()) {
        if (queried) ++*queried;
        const auto rowChannel=q.value(4).toString();
        const auto rowSession=q.value(5).toString();
        const auto text=snapshotExpressionText(q.value(2).toString());
        if (text.isEmpty() || !policy.allows(rowChannel,rowSession,text)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("chat/%1").arg(q.value(0).toLongLong())},
                                {QStringLiteral("kind"),q.value(1).toString()},
                                {QStringLiteral("channel"),rowChannel},
                                {QStringLiteral("session"),rowSession},
                                {QStringLiteral("occurred_at"),snapshotIsoText(q.value(3).toString())},
                                {QStringLiteral("text"),text.left(512)},
                                {QStringLiteral("sensitive"),false}});
        if (eligible) ++*eligible;
    }
    return rows;
}

QJsonArray MemoryManager::recentInferences(const QString &subject,int limit,
                                           int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || subject.isEmpty() || limit<=0) return rows;
    const MemoryReadPolicy policy;
    // 1) 她自己留下的回顾/关注想法（note）。
    QSqlQuery notes(m_db);
    notes.prepare("SELECT r.turn_id,r.note,coalesce(s.reported,h.timestamp),h.channel,h.session_id "
                  "FROM private_reflection r JOIN chat_history h ON h.id=r.turn_id "
                  "LEFT JOIN cognitive_source s ON s.event=h.cognitive_event AND s.channel=h.channel "
                  "AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input "
                  "WHERE h.subject=? AND h.sensitive=0 AND coalesce(r.note,'')<>'' "
                  "ORDER BY r.updated_at DESC,r.turn_id DESC LIMIT ?");
    notes.addBindValue(subject);
    notes.addBindValue(limit);
    if (notes.exec()) while (notes.next()) {
        if (queried) ++*queried;
        const auto channel=notes.value(3).toString();
        const auto session=notes.value(4).toString();
        const auto text=notes.value(1).toString();
        if (!policy.allows(channel,session,text)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("reflection/%1").arg(notes.value(0).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("reflection")},
                                {QStringLiteral("channel"),channel},
                                {QStringLiteral("session"),session},
                                {QStringLiteral("occurred_at"),snapshotIsoText(notes.value(2).toString())},
                                {QStringLiteral("text"),text.left(240)},
                                {QStringLiteral("sensitive"),false}});
        if (eligible) ++*eligible;
    }
    // 2) 每个事项只取最新一版 understanding（她是如何理解这件事的）。
    if (rows.size()>=limit) return rows;
    QSqlQuery revisions(m_db);
    revisions.prepare("SELECT r.matter,r.version,r.understanding,s.reported,s.channel,s.session "
                      "FROM matter_revision r JOIN cognitive_source s ON s.id=r.source WHERE s.subject=? "
                      "ORDER BY r.matter,r.version DESC");
    revisions.addBindValue(subject);
    if (!revisions.exec()) return rows;
    QSet<QString> seen;
    while (revisions.next() && rows.size()<limit) {
        if (queried) ++*queried;
        const auto matter=revisions.value(0).toString();
        if (seen.contains(matter)) continue;
        seen.insert(matter);
        const auto channel=revisions.value(4).toString();
        const auto session=revisions.value(5).toString();
        const auto text=revisions.value(2).toString();
        if (!policy.allows(channel,session,text)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("revision/%1/%2")
                                     .arg(matter).arg(revisions.value(1).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("matter_revision")},
                                {QStringLiteral("channel"),channel},
                                {QStringLiteral("session"),session},
                                {QStringLiteral("occurred_at"),snapshotIsoText(revisions.value(3).toString())},
                                {QStringLiteral("text"),text.left(240)},
                                {QStringLiteral("sensitive"),false}});
        if (eligible) ++*eligible;
    }
    return rows;
}

QJsonArray MemoryManager::currentMotivations(int limit,int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || limit<=0) return rows;
    const MemoryReadPolicy policy;
    // open concern：仍 open 且未被回顾链修订掉的 head。
    QSqlQuery concerns(m_db);
    concerns.prepare("SELECT r.turn_id,r.version,coalesce(r.topic,''),coalesce(r.note,''),"
                     "coalesce(r.root_turn,r.turn_id),h.channel,h.session_id "
                     "FROM private_reflection r JOIN chat_history h ON h.id=r.turn_id "
                     "WHERE h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
                     "AND r.state='consider' AND NOT EXISTS "
                     "(SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id) "
                     "AND coalesce(r.note,'')<>'' ORDER BY r.updated_at DESC,r.turn_id DESC LIMIT ?");
    concerns.addBindValue(limit);
    if (concerns.exec()) while (concerns.next() && rows.size()<limit) {
        if (queried) ++*queried;
        const auto channel=concerns.value(5).toString();
        const auto session=concerns.value(6).toString();
        const auto note=concerns.value(3).toString();
        if (!policy.allows(channel,session,note)) continue;
        const auto topic=concerns.value(2).toString();
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("concern/%1").arg(concerns.value(4).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("concern")},
                                {QStringLiteral("summary"),(topic.isEmpty()?note:topic).left(160)},
                                {QStringLiteral("version"),concerns.value(1).toInt()}});
        if (eligible) ++*eligible;
    }
    if (rows.size()>=limit) return rows;
    // active thought：她自己想继续的兴趣/写作，属于自发动机。
    QSqlQuery thoughts(m_db);
    thoughts.prepare("SELECT id,thought,thought_version,h.channel,h.session_id FROM chat_history h "
                     "WHERE h.thought IS NOT NULL AND h.thought<>'' AND h.sensitive=0 AND h.subject='user' "
                     "AND coalesce(json_extract(h.thought_result,'$.status'),'active') IN ('active','paused') "
                     "ORDER BY h.id DESC LIMIT ?");
    thoughts.addBindValue(limit);
    if (thoughts.exec()) while (thoughts.next() && rows.size()<limit) {
        if (queried) ++*queried;
        const auto channel=thoughts.value(3).toString();
        const auto session=thoughts.value(4).toString();
        const auto text=thoughts.value(1).toString();
        if (!policy.allows(channel,session,text)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("thought/%1").arg(thoughts.value(0).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("interest")},
                                {QStringLiteral("summary"),text.left(160)},
                                {QStringLiteral("version"),thoughts.value(2).toInt()}});
        if (eligible) ++*eligible;
    }
    return rows;
}

QJsonArray MemoryManager::activeCommitments(const QString &channel,const QString &session,
                                            int limit,int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || channel.isEmpty() || session.isEmpty() || limit<=0) return rows;
    const MemoryReadPolicy policy;
    // 主人真实委托：只取目标 channel/session 匹配、未过期且仍 pending 的提醒。
    QSqlQuery q(m_db);
    q.prepare("SELECT m.id,m.state,m.due,m.expires,m.message FROM reminder m "
              "JOIN chat_history h ON h.id=m.turn "
              "WHERE m.state='pending' AND m.channel=? AND m.session=? AND m.expires>unixepoch() "
              "AND h.subject='user' AND h.sensitive=0 ORDER BY m.due,m.id LIMIT ?");
    q.addBindValue(channel);
    q.addBindValue(session);
    q.addBindValue(limit);
    if (!q.exec()) return rows;
    while (q.next()) {
        if (queried) ++*queried;
        const auto message=q.value(4).toString();
        if (!policy.allows(channel,session,message)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("reminder/%1").arg(q.value(0).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("reminder")},
                                {QStringLiteral("state"),q.value(1).toString()},
                                {QStringLiteral("due_at"),snapshotIsoFromEpoch(q.value(2).toLongLong())},
                                {QStringLiteral("expires_at"),snapshotIsoFromEpoch(q.value(3).toLongLong())},
                                {QStringLiteral("message"),message.left(80)}});
        if (eligible) ++*eligible;
    }
    return rows;
}

QJsonArray MemoryManager::currentActivities(int limit,int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || limit<=0) return rows;
    const MemoryReadPolicy policy;
    QSqlQuery q(m_db);
    q.prepare("SELECT id,coalesce(json_extract(thought_result,'$.status'),'active'),thought_version,"
              "coalesce(json_extract(thought_result,'$.artifact_ref'),''),"
              "coalesce(json_extract(thought_result,'$.progress'),''),"
              "coalesce(json_extract(thought_result,'$.goal'),''),"
              "coalesce(json_extract(thought_result,'$.next_step'),''),h.channel,h.session_id "
              "FROM chat_history h WHERE h.thought IS NOT NULL AND h.thought<>'' "
              "AND h.sensitive=0 AND h.subject='user' "
              "AND coalesce(json_extract(h.thought_result,'$.status'),'active') IN ('active','paused') "
              "ORDER BY h.id DESC LIMIT ?");
    q.addBindValue(limit);
    if (!q.exec()) return rows;
    while (q.next()) {
        if (queried) ++*queried;
        const auto channel=q.value(7).toString();
        const auto session=q.value(8).toString();
        QJsonObject row{{QStringLiteral("ref"),QStringLiteral("thought/%1").arg(q.value(0).toLongLong())},
                        {QStringLiteral("status"),q.value(1).toString()},
                        {QStringLiteral("version"),q.value(2).toInt()},
                        {QStringLiteral("artifact_ref"),QString()},
                        {QStringLiteral("progress"),QString()}};
        for (const auto &pair : {qMakePair(QStringLiteral("artifact_ref"),q.value(3).toString()),
                                 qMakePair(QStringLiteral("progress"),q.value(4).toString()),
                                 qMakePair(QStringLiteral("goal"),q.value(5).toString()),
                                 qMakePair(QStringLiteral("next_step"),q.value(6).toString())}) {
            const auto text=pair.second.trimmed();
            if (text.isEmpty() || !policy.allows(channel,session,text)) continue;
            row.insert(pair.first,text.left(120));
        }
        rows.append(row);
        if (eligible) ++*eligible;
    }
    return rows;
}

QJsonArray MemoryManager::internalWakes(int limit,int *queried,int *eligible) const
{
    if (queried) *queried = 0;
    if (eligible) *eligible = 0;
    QJsonArray rows;
    if (!m_db.isOpen() || limit<=0) return rows;
    // 1) 最近一次空闲唤醒（idle）。
    {
        QSqlQuery wake(m_db);
        if (wake.exec("SELECT value FROM attention_state WHERE key='last_wake_at'") && wake.next()) {
            if (queried) ++*queried;
            const auto value=wake.value(0).toString();
            if (!value.trimmed().isEmpty()) {
                rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("attention/last_wake_at")},
                                        {QStringLiteral("kind"),QStringLiteral("idle")},
                                        {QStringLiteral("at"),snapshotIsoText(value)}});
                if (eligible) ++*eligible;
            }
        }
    }
    if (rows.size()>=limit) return rows;
    // 2) 未了结关注自己排的“再看一眼”时间。它不是主人约定，只能作为内部唤醒。
    {
        QSqlQuery concerns(m_db);
        concerns.prepare("SELECT r.turn_id,coalesce(r.root_turn,r.turn_id),r.next_check_at "
                         "FROM private_reflection r JOIN chat_history h ON h.id=r.turn_id "
                         "WHERE h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
                         "AND r.state IN ('consider','pending') AND r.next_check_at>0 "
                         "ORDER BY r.next_check_at LIMIT ?");
        concerns.addBindValue(int(limit-rows.size()));
        if (concerns.exec()) while (concerns.next() && rows.size()<limit) {
            if (queried) ++*queried;
            const auto at=snapshotIsoFromEpoch(concerns.value(2).toLongLong());
            rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("concern/%1").arg(concerns.value(1).toLongLong())},
                                    {QStringLiteral("kind"),QStringLiteral("concern_review")},
                                    {QStringLiteral("at"),at},
                                    // 与回顾提示词里的 own_next_review_at 同义：她自己的再检查时间。
                                    {QStringLiteral("own_next_review_at"),at}});
            if (eligible) ++*eligible;
        }
    }
    if (rows.size()>=limit) return rows;
    // 3) 已排期的活动回顾（scheduled）。
    {
        QSqlQuery thoughts(m_db);
        thoughts.prepare("SELECT id,review_after FROM chat_history "
                         "WHERE thought IS NOT NULL AND thought<>'' AND sensitive=0 AND subject='user' "
                         "AND coalesce(json_extract(thought_result,'$.status'),'active')='active' "
                         "AND coalesce(review_after,0)>0 ORDER BY review_after LIMIT ?");
        thoughts.addBindValue(int(limit-rows.size()));
        if (thoughts.exec()) while (thoughts.next() && rows.size()<limit) {
            if (queried) ++*queried;
            rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("thought/%1").arg(thoughts.value(0).toLongLong())},
                                    {QStringLiteral("kind"),QStringLiteral("scheduled")},
                                    {QStringLiteral("at"),snapshotIsoFromEpoch(thoughts.value(1).toLongLong())}});
            if (eligible) ++*eligible;
        }
    }
    return rows;
}

QJsonObject MemoryManager::reviewInteractionTimes(const QString &channel,const QString &session,
                                                  const QString &subject) const
{
    // R12-1a：区分“当前会话最近交互”与“跨渠道最近交互”，并保留 R12-0 的旧字段别名，
    // 供回顾 prompt 的 time_context 向后兼容。只读，不新造状态、不扩大读取范围。
    QJsonObject result{
        {QStringLiteral("last_owner_interaction_at"),QString()},
        {QStringLiteral("last_confirmed_outbound_at"),QString()},
        {QStringLiteral("last_owner_session_at"),QString()},
        {QStringLiteral("last_owner_cross_channel_at"),QString()},
        {QStringLiteral("last_confirmed_outbound_session_at"),QString()},
        {QStringLiteral("last_confirmed_outbound_cross_channel_at"),QString()}};
    if (!m_db.isOpen()) return result;

    const bool hasChannel = !channel.isEmpty();
    const bool hasSession = hasChannel && !session.isEmpty();
    // 当前会话范围：有渠道就按渠道（+会话）；只知道“这是主人私聊来源”时退化为主人私聊域。
    const QString sessionScopeSuffix = hasSession
        ? QStringLiteral(" AND h.channel=? AND h.session_id=?")
        : hasChannel ? QStringLiteral(" AND h.channel=?")
                     : QStringLiteral(" AND h.channel IN ('pet','qq_private')");
    // 跨渠道范围：只在主人私聊域（pet/qq_private）内跨会话读取，绝不扩到群聊。
    const bool ownerSubject = subject == QStringLiteral("user");
    const bool ownerPrivateChannel = !hasChannel
        || channel == Channel::kPet || channel == Channel::kQQPrivate;
    // 新字段暴露条件：主人 + 非群聊来源；群聊/第三方不读私人跨渠道时间。
    const bool crossAllowed = ownerSubject && ownerPrivateChannel;
    const QString crossScopeSuffix = QStringLiteral(" AND h.channel IN ('pet','qq_private')");

    const auto firstTime = [this](const QString &sql, const QVariantList &binds) {
        QSqlQuery q(m_db);
        q.prepare(sql);
        for (const auto &value : binds) q.addBindValue(value);
        if (q.exec() && q.next()) return snapshotIsoText(q.value(0).toString());
        return QString{};
    };

    // 主人最近一次真实输入（subject=user、turn_kind='reply'，reported 优先、其次落库时间）。
    const QString ownerSql = QStringLiteral(
        "SELECT coalesce(s.reported,h.timestamp) FROM chat_history h "
        "LEFT JOIN cognitive_source s ON s.event=h.cognitive_event AND s.channel=h.channel "
        "AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input "
        "WHERE h.subject=? AND h.sensitive=0 AND h.turn_kind='reply' "
        "AND h.user_input IS NOT NULL AND h.user_input<>''");
    QVariantList sessionBinds{QVariant(subject)};
    if (hasChannel) sessionBinds.append(QVariant(channel));
    if (hasSession) sessionBinds.append(QVariant(session));
    const QString sessionOwner = firstTime(
        ownerSql + sessionScopeSuffix + QStringLiteral(" ORDER BY h.id DESC LIMIT 1"),
        sessionBinds);

    // 最近一条已落库的成功表达：沿用现有落库语义（turn_state='handed_off'），
    // 普通 reply 与 proactive 都只证明“她确实说过/发出过”，不代表主人已经读过。
    const QString outboundSql = QStringLiteral(
        "SELECT coalesce(s.reported,h.timestamp) FROM chat_history h "
        "LEFT JOIN cognitive_source s ON s.event=h.cognitive_event AND s.channel=h.channel "
        "AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input "
        "WHERE h.subject=? AND h.sensitive=0 AND h.turn_state='handed_off' "
        "AND h.turn_kind IN ('reply','proactive') "
        "AND h.raw_reply IS NOT NULL AND h.raw_reply<>''");
    const QString sessionOutbound = firstTime(
        outboundSql + sessionScopeSuffix + QStringLiteral(" ORDER BY h.id DESC LIMIT 1"),
        sessionBinds);

    // 跨渠道查询只在需要时执行：无来源渠道（旧别名语义需要）或主人私聊域（新字段）。
    QString crossOwner;
    QString crossOutbound;
    if (!hasChannel || crossAllowed) {
        crossOwner = firstTime(
            ownerSql + crossScopeSuffix + QStringLiteral(" ORDER BY h.id DESC LIMIT 1"),
            QVariantList{QVariant(subject)});
        crossOutbound = firstTime(
            outboundSql + crossScopeSuffix + QStringLiteral(" ORDER BY h.id DESC LIMIT 1"),
            QVariantList{QVariant(subject)});
    }

    result[QStringLiteral("last_owner_session_at")] = sessionOwner;
    result[QStringLiteral("last_confirmed_outbound_session_at")] = sessionOutbound;
    // 跨渠道时间只对“主人 + 非群聊来源”返回：群聊/第三方不扩大私人资料权限。
    if (crossAllowed) {
        result[QStringLiteral("last_owner_cross_channel_at")] = crossOwner;
        result[QStringLiteral("last_confirmed_outbound_cross_channel_at")] = crossOutbound;
    }
    // R12-0 旧字段别名：有明确来源渠道/会话时等价于当前会话时间；没有渠道时保持旧语义
    // （主人私聊域最近一次），既有回顾 prompt 与测试无需同步改语义。
    result[QStringLiteral("last_owner_interaction_at")] =
        hasChannel ? sessionOwner : crossOwner;
    result[QStringLiteral("last_confirmed_outbound_at")] =
        hasChannel ? sessionOutbound : crossOutbound;
    return result;
}

QString MemoryManager::snapshotSubjectId(const RequestMeta &meta) const
{
    // 主人（含桌宠/QQ 私聊/群里的主人）统一归一为 'user'；第三方取 actor，
    // 没有 actor 时沿用既有 subject 约定（qq:<uin> 等），保证与库里的行对得上。
    if (meta.subject.isEmpty() || meta.subject == QStringLiteral("user"))
        return QStringLiteral("user");
    if (!meta.actorId.trimmed().isEmpty()) return meta.actorId.trimmed();
    return meta.subject;
}

// ============================ 切片 B：主动动作提交 + Outbox ============================
// 说明：整条链路的唯一不变式是
//   1) 领域状态 CAS、cognitive_action、action_outbox 在同一个 SQLite 事务里落库；
//   2) 事务内只读写数据库，绝不调用 QQ/网络/UI；
//   3) 真正的发送只发生在 OutboxDispatcher（普通 QTimer 轮询），发送失败/回执再 settle；
//   4) action_key 是本地唯一键：同一动作重复提交幂等，outbox 永远只有一行。
// 状态机：
//   action : queued → dispatching → handed_off(含 protocol_accepted) | rejected | unknown | withheld
//            rejected(attempts<2) 可被 outbox 重新领取一次；unknown/withheld 冻结。
//   outbox : queued → dispatching → dispatched | failed(终态) | unknown
//            领取时 attempts+1（attempts 语义=已提交的发送次数，最多 2 次）；
//            settle rejected 且 attempts<2 → 回 queued（受控一次重试）。
namespace {

// R12-2c-2：以下三个函数定义在后面的 R12-2b 提交片区（同一匿名命名空间），
// 发送前复验需要复用同一套错误码映射/硬拒绝分类/安静时段口径，先前置声明避免分叉。
bool actionProposalHardReject(const QString &code);
QString actionProposalErrorCode(const QString &reason);
bool actionQuietNow(const ProactiveConfig &options);

// R12-2b：新来源动作在 cognitive_action.kind 上的稳定名称。
// 与 legacy 的 SpontaneousContact / ActivityShare 分开，settle/恢复分支据此把回执写回
// open_wake_state / internal_activity，而不会误走 reflection_contact / thought_result 旧路径。
const QString kActionKindOpenWakeContact = QStringLiteral("OpenWakeContact");
const QString kActionKindInternalShare = QStringLiteral("InternalActivityShare");

// 与 claimReflectionContact 完全一致的来源/资格口径（privatereflection.cpp 内是文件静态的，
// 这里按同一语义复制一份，保证"提交"和旧"领取"对同一个联系意图的判断不会分叉）。
const QString kActionContactSourceRows = QStringLiteral(
    " FROM chat_history h JOIN cognitive_source s ON s.event=h.cognitive_event "
    "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input ");
const QString kActionContactEligible = QStringLiteral(
    " h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
    "AND h.turn_state IN ('handed_off','quiet') AND h.turn_kind='reply' ");

QJsonObject actionError(const QString &code,const QString &detail = {})
{
    QJsonObject result{{QStringLiteral("error"),code}};
    if (!detail.isEmpty()) result[QStringLiteral("detail")] = detail;
    return result;
}

bool actionVisibleSession(const QString &channel,const QString &session,
                          const QString &viewerChannel,const QString &viewerSession)
{
    if (channel==viewerChannel && session==viewerSession) return true;
    auto &cfg=ConfigManager::instance();
    return cfg.getMemoryPolicyConfig().agent.ownerCrossChannelPrivate &&
           ((channel==Channel::kPet && session==Channel::kDefaultSession) ||
            (channel==Channel::kQQPrivate && cfg.isMasterQQ(session)));
}

bool actionBasisVisible(const QSqlDatabase &db,qlonglong turn,const MemoryReadPolicy &policy,
                        const QString &viewerChannel,const QString &viewerSession)
{
    QSqlQuery q(db);
    q.prepare("SELECT h.channel,h.session_id,h.user_input,h.sensitive FROM reflection_basis b "
              "LEFT JOIN chat_history h ON h.id=b.source WHERE b.turn=?");
    q.addBindValue(turn);
    if (!q.exec()) return false;
    while(q.next()) {
        if (q.value(0).isNull() || q.value(3).toBool()
            || !actionVisibleSession(q.value(0).toString(),q.value(1).toString(),
                                     viewerChannel,viewerSession)
            || !policy.allows(q.value(0).toString(),q.value(1).toString(),q.value(2).toString()))
            return false;
    }
    return true;
}

// 统一投递流水：只做可观测性，失败不阻断主状态机。与 privatereflection.cpp 的 noteDelivery 同义。
void actionNoteDelivery(const QSqlDatabase &db,const QString &kind,const QString &ref,
                        const QString &channel,const QString &session,const QString &text,
                        const QString &state,int attemptsDelta)
{
    if (kind.isEmpty() || ref.isEmpty() || state.isEmpty()) return;
    QSqlQuery q(db);
    q.prepare("INSERT INTO delivery_ledger(kind,ref,channel,session,text,state,attempts,updated_at) "
              "VALUES(?,?,?,?,?,?,?,CURRENT_TIMESTAMP) ON CONFLICT(kind,ref) DO UPDATE SET "
              "state=excluded.state,"
              "channel=coalesce(excluded.channel,delivery_ledger.channel),"
              "session=coalesce(excluded.session,delivery_ledger.session),"
              "text=coalesce(excluded.text,delivery_ledger.text),"
              "attempts=delivery_ledger.attempts+excluded.attempts,"
              "updated_at=CURRENT_TIMESTAMP");
    q.addBindValue(kind);
    q.addBindValue(ref);
    q.addBindValue(channel.isEmpty() ? QVariant() : QVariant(channel));
    q.addBindValue(session.isEmpty() ? QVariant() : QVariant(session));
    q.addBindValue(text.isEmpty() ? QVariant() : QVariant(text));
    q.addBindValue(state);
    q.addBindValue(attemptsDelta);
    if (!q.exec()) qWarning()<<"[Outbox] ledger 写入失败"<<kind<<ref<<state<<q.lastError();
}

// R12-2c-2：提交事务里的账本写入必须可判定。SQLite 的 UPSERT 影响 1 行；
// 影响行数不是 1 时提交方必须整体回滚，绝不能出现"动作/Outbox 写了、额度账本没写"。
bool actionNoteDeliveryChecked(const QSqlDatabase &db,const QString &kind,const QString &ref,
                               const QString &channel,const QString &session,const QString &text,
                               const QString &state,int attemptsDelta)
{
    if (kind.isEmpty() || ref.isEmpty() || state.isEmpty()) return false;
    QSqlQuery q(db);
    q.prepare("INSERT INTO delivery_ledger(kind,ref,channel,session,text,state,attempts,updated_at) "
              "VALUES(?,?,?,?,?,?,?,CURRENT_TIMESTAMP) ON CONFLICT(kind,ref) DO UPDATE SET "
              "state=excluded.state,"
              "channel=coalesce(excluded.channel,delivery_ledger.channel),"
              "session=coalesce(excluded.session,delivery_ledger.session),"
              "text=coalesce(excluded.text,delivery_ledger.text),"
              "attempts=delivery_ledger.attempts+excluded.attempts,"
              "updated_at=CURRENT_TIMESTAMP");
    q.addBindValue(kind);
    q.addBindValue(ref);
    q.addBindValue(channel.isEmpty() ? QVariant() : QVariant(channel));
    q.addBindValue(session.isEmpty() ? QVariant() : QVariant(session));
    q.addBindValue(text.isEmpty() ? QVariant() : QVariant(text));
    q.addBindValue(state);
    q.addBindValue(attemptsDelta);
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "[Outbox] ledger 提交写入失败" << kind << ref << state << q.lastError();
        return false;
    }
    return true;
}

// 幂等查询：动作已存在时返回其当前状态，调用方据此不重复建 outbox。
QJsonObject existingAction(const QSqlDatabase &db,const QString &actionKey)
{
    QSqlQuery q(db);
    q.prepare("SELECT id,kind,state,channel,session_id,payload_json,attempts,last_outcome "
              "FROM cognitive_action WHERE action_key=?");
    q.addBindValue(actionKey);
    if (!q.exec() || !q.next()) return {};
    QJsonObject result{{QStringLiteral("action_key"),actionKey},
                       {QStringLiteral("action_id"),q.value(0).toLongLong()},
                       {QStringLiteral("kind"),q.value(1).toString()},
                       {QStringLiteral("state"),q.value(2).toString()},
                       {QStringLiteral("channel"),q.value(3).toString()},
                       {QStringLiteral("session"),q.value(4).toString()},
                       {QStringLiteral("attempts"),q.value(6).toInt()},
                       {QStringLiteral("idempotent"),true}};
    if (!q.value(7).isNull()) result[QStringLiteral("last_outcome")] = q.value(7).toString();
    return result;
}

// 提醒的可达性与 claimDueReminder 的 allowedReminder 同口径。
bool actionReminderAllowed(const QString &channel,const QString &session)
{
    auto &cfg=ConfigManager::instance();
    const auto opts=cfg.getProactiveConfig();
    return opts.enabled && opts.targets.contains(qMakePair(channel,session))
        && cfg.getMemoryPolicyConfig().longTermMemoryEnabled && !cfg.isBlindZone(channel,session)
        && ((channel==Channel::kPet && session==Channel::kDefaultSession)
            || (channel==Channel::kQQPrivate && cfg.isMasterQQ(session)));
}

// 目标选择：优先 pet/default（本地必定可达），否则第一个可用的 QQ 私聊主人目标。
// 与旧 dispatchContact/draftReadyToShare 的"按配置顺序、宠物优先"一致；QQ 是否在线
// 由 AppController 在提交前门控，提交本身不依赖网络状态。
struct ActionTarget { QString channel; QString session; bool found=false; };

ActionTarget actionChooseTarget(const QList<QPair<QString,QString>> &targets,
                                const QString &draftLikeText,bool requireShareable)
{
    auto &cfg=ConfigManager::instance();
    ActionTarget target;
    ActionTarget qqFallback;
    MemoryReadPolicy policy;
    for (const auto &pair : targets) {
        const QString channel=pair.first, session=pair.second;
        const bool pet=channel==Channel::kPet && session==Channel::kDefaultSession;
        const bool qq=channel==Channel::kQQPrivate && cfg.isMasterQQ(session);
        if (!pet && !qq) continue;
        if (requireShareable && cfg.isBlindZone(channel,session)) continue;
        if (!policy.allows(channel,session,draftLikeText)) continue;
        if (pet) { target.channel=channel; target.session=session; target.found=true; return target; }
        if (!qqFallback.found) { qqFallback.channel=channel; qqFallback.session=session; qqFallback.found=true; }
    }
    return qqFallback;
}

// claimOutbox 的事务内动作：把可重试的领域行从 rejected 拉回 dispatching，
// 使崩溃恢复（recoverStaleOutbox）与旧 recoverStaleContacts 看到一致的状态。
bool markOutboxDomainDispatching(const QSqlDatabase &db,const QString &actionKey,
                                 const QString &kind,const QString &channel,const QString &session)
{
    const auto parts=actionKey.split(QLatin1Char(':'));
    QSqlQuery q(db);
    if (kind==QStringLiteral("SpontaneousContact")) {
        if (parts.size()<4) return true;
        const auto turn=parts[1].toLongLong();
        const auto version=parts[2].toInt();
        q.prepare("UPDATE reflection_contact SET outcome='dispatching',last_attempt_at=unixepoch(),"
                  "updated_at=unixepoch() WHERE turn_id=? AND version=? AND outcome='rejected'");
        q.addBindValue(turn);
        q.addBindValue(version);
        if (!q.exec()) return false;
        actionNoteDelivery(db,QStringLiteral("contact"),
                           QStringLiteral("reflection:%1:%2").arg(turn).arg(version),
                           channel,session,QString{},QStringLiteral("claimed"),0);
    } else if (kind==QStringLiteral("ReminderDelivery")) {
        if (parts.size()<2) return true;
        q.prepare("UPDATE reminder SET state='dispatching' WHERE id=? AND state='rejected'");
        q.addBindValue(parts[1].toLongLong());
        if (!q.exec()) return false;
        actionNoteDelivery(db,QStringLiteral("reminder"),
                           QStringLiteral("reminder:%1").arg(parts[1].toLongLong()),
                           channel,session,QString{},QStringLiteral("claimed"),0);
    } else if (kind==QStringLiteral("ActivityShare")) {
        if (parts.size()<3) return true;
        const QString ref=QStringLiteral("thought:%1:%2").arg(parts[1]).arg(parts[2]);
        q.prepare("UPDATE delivery_ledger SET state='claimed',updated_at=CURRENT_TIMESTAMP "
                  "WHERE kind='share' AND ref=? AND state='rejected'");
        q.addBindValue(ref);
        if (!q.exec()) return false;
    } else if (kind==kActionKindOpenWakeContact) {
        // R12-2b：新来源 contact 的领域行就是 delivery_ledger(kind=contact, ref=action_key)，
        // 领取时只把"被渠道拒绝后的受控重试"拉回 claimed；action_key 已存在的幂等判断在提交层。
        actionNoteDelivery(db,QStringLiteral("contact"),actionKey,channel,session,
                           QString{},QStringLiteral("claimed"),0);
    } else if (kind==kActionKindInternalShare) {
        actionNoteDelivery(db,QStringLiteral("share"),actionKey,channel,session,
                           QString{},QStringLiteral("claimed"),0);
    }
    return true;
}

// settle 的事务内动作：把结果同时写回领域行（contact / reminder / share）。
// 领域行可能已被旧恢复逻辑推成终态，此时"影响 0 行"不算失败，只有 SQL 错误才失败。
bool settleOutboxDomain(const QSqlDatabase &db,const QString &actionKey,const QString &kind,
                        const QString &channel,const QString &session,const QString &outcome)
{
    const auto parts=actionKey.split(QLatin1Char(':'));
    const bool failed = outcome==QStringLiteral("rejected") || outcome==QStringLiteral("unknown");
    const bool terminalReceipt = outcome==QStringLiteral("rejected")
        || outcome==QStringLiteral("unknown") || outcome==QStringLiteral("protocol_accepted");
    QSqlQuery q(db);
    if (kind==QStringLiteral("SpontaneousContact")) {
        if (parts.size()<4) return true;
        const auto turn=parts[1].toLongLong();
        const auto version=parts[2].toInt();
        q.prepare("UPDATE reflection_contact SET outcome=?,last_outcome=?,attempts=attempts+?,"
                  "last_attempt_at=unixepoch(),updated_at=unixepoch() "
                  "WHERE turn_id=? AND version=? AND (outcome IN ('dispatching','rejected') "
                  "OR (outcome='handed_off' AND ?))");
        q.addBindValue(outcome);
        q.addBindValue(outcome);
        q.addBindValue(failed ? 1 : 0);
        q.addBindValue(turn);
        q.addBindValue(version);
        q.addBindValue(terminalReceipt ? 1 : 0);
        if (!q.exec()) return false;
        actionNoteDelivery(db,QStringLiteral("contact"),
                           QStringLiteral("reflection:%1:%2").arg(turn).arg(version),
                           QString{},QString{},QString{},outcome,failed ? 1 : 0);
    } else if (kind==QStringLiteral("ReminderDelivery")) {
        if (parts.size()<2) return true;
        const auto id=parts[1].toLongLong();
        q.prepare("UPDATE reminder SET state=? WHERE id=? AND (state IN ('dispatching','rejected','pending') "
                  "OR (state='handed_off' AND ?))");
        q.addBindValue(outcome);
        q.addBindValue(id);
        q.addBindValue(terminalReceipt ? 1 : 0);
        if (!q.exec()) return false;
        actionNoteDelivery(db,QStringLiteral("reminder"),QStringLiteral("reminder:%1").arg(id),
                           QString{},QString{},QString{},outcome,failed ? 1 : 0);
    } else if (kind==QStringLiteral("ActivityShare")) {
        if (parts.size()<3) return true;
        const auto activityId=parts[1].toLongLong();
        const auto version=parts[2].toInt();
        // 分享结果跟随活动存档（与 recordShareOutcome 的 thought_result 补丁同形），
        // 重启后仍能判断同一版本是否已送达/未知。
        q.prepare("UPDATE chat_history SET thought_result=json_patch(coalesce(thought_result,'{}'),?) "
                  "WHERE id=? AND thought IS NOT NULL AND sensitive=0");
        q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("share_outcome"),outcome},
            {QStringLiteral("share_version"),version},
            {QStringLiteral("share_channel"),channel},
            {QStringLiteral("share_at"),QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}})
            .toJson(QJsonDocument::Compact)));
        q.addBindValue(activityId);
        if (!q.exec()) return false;
        actionNoteDelivery(db,QStringLiteral("share"),
                           QStringLiteral("thought:%1:%2").arg(activityId).arg(version),
                           channel,session,QString{},outcome,failed ? 1 : 0);
    } else if (kind==kActionKindOpenWakeContact) {
        // R12-2b：新来源 contact 的 delivery_ledger.ref 就是 action_key（wake:<id>:<v>:contact）。
        // 成功回执额外把 open_wake_state.last_outcome 更新成 contact_<outcome>，供调度/审计读取；
        // unknown/rejected/withheld 只落账本终态，不写成"她说过"。
        actionNoteDelivery(db,QStringLiteral("contact"),actionKey,channel,session,
                           QString{},outcome,failed ? 1 : 0);
        if (outcome==QStringLiteral("handed_off")
            || outcome==QStringLiteral("protocol_accepted")) {
            if (parts.size()>=2) {
                QSqlQuery scope(db);
                scope.prepare(QStringLiteral("SELECT scope_key FROM open_wake WHERE id=?"));
                scope.addBindValue(parts[1].toLongLong());
                if (scope.exec() && scope.next()) {
                    // R12-2c-3：只有回执对应"最新一次 wake"才允许写 last_outcome；
                    // 旧 wake 的迟到回执不得覆盖新 wake 的状态。last_wake_id NULL/0 时保守跳过。
                    QSqlQuery stateUpdate(db);
                    stateUpdate.prepare(QStringLiteral(
                        "UPDATE open_wake_state SET last_outcome=?,updated_at=unixepoch() "
                        "WHERE scope_key=? AND last_wake_id=?"));
                    stateUpdate.addBindValue(QStringLiteral("contact_%1").arg(outcome));
                    stateUpdate.addBindValue(scope.value(0).toString());
                    stateUpdate.addBindValue(parts[1].toLongLong());
                    if (!stateUpdate.exec()) return false;
                }
            }
        }
    } else if (kind==kActionKindInternalShare) {
        // R12-2b：internal share 的账本 ref=action_key（activity:<id>:<artifact_version>:share）。
        // 成功回执只 patch result_json 的分享结果，**不改变活动 status/version**（完成状态保留）。
        actionNoteDelivery(db,QStringLiteral("share"),actionKey,channel,session,
                           QString{},outcome,failed ? 1 : 0);
        if (outcome==QStringLiteral("handed_off")
            || outcome==QStringLiteral("protocol_accepted")) {
            if (parts.size()>=3) {
                const qlonglong activityId = parts[1].toLongLong();
                const int shareVersion = parts[2].toInt();
                // R12-2c-3：回执绑定原成果 artifact_ref/version/hash；旧成果版本的迟到回执
                // 只审计、不覆盖当前版本的分享结果（条件判断用当前 share_intent 的成果版本）。
                QJsonObject actionPayload;
                {
                    QSqlQuery payload(db);
                    payload.prepare(QStringLiteral(
                        "SELECT payload_json FROM cognitive_action WHERE action_key=?"));
                    payload.addBindValue(actionKey);
                    if (payload.exec() && payload.next())
                        actionPayload = QJsonDocument::fromJson(
                            payload.value(0).toString().toUtf8()).object();
                }
                QJsonObject patch{
                    {QStringLiteral("share_outcome"),outcome},
                    {QStringLiteral("share_version"),shareVersion},
                    {QStringLiteral("share_channel"),channel},
                    {QStringLiteral("share_at"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
                const QString artifactRef =
                    actionPayload.value(QStringLiteral("artifact_ref")).toString();
                const int artifactVersion =
                    actionPayload.value(QStringLiteral("artifact_version")).toInt();
                const QString artifactHash =
                    actionPayload.value(QStringLiteral("artifact_hash")).toString();
                if (!artifactRef.isEmpty())
                    patch.insert(QStringLiteral("share_artifact_ref"), artifactRef);
                if (artifactVersion > 0)
                    patch.insert(QStringLiteral("share_artifact_version"), artifactVersion);
                if (!artifactHash.isEmpty())
                    patch.insert(QStringLiteral("share_artifact_hash"), artifactHash);
                QSqlQuery patchQuery(db);
                patchQuery.prepare(QStringLiteral(
                    "UPDATE internal_activity SET result_json="
                    "json_patch(coalesce(result_json,'{}'),?),updated_at=unixepoch() "
                    "WHERE id=? AND status='completed' "
                    "AND json_extract(result_json,'$.share_intent.artifact_version')=?"));
                patchQuery.addBindValue(QString::fromUtf8(
                    QJsonDocument(patch).toJson(QJsonDocument::Compact)));
                patchQuery.addBindValue(activityId);
                patchQuery.addBindValue(shareVersion);
                if (!patchQuery.exec()) return false;
            }
        }
    }
    return true;
}

// R12-2c-2：事务内取消一条 outbox 动作（永久失效，绝不发送）。幂等：
// outbox=failed、action=withheld、领域账本/领域行=withheld，提案写 cancel_reason
// （未提交的置 expired；已 committed 保持 state 供 c3 回执闭环），不删除任何历史事实/成果。
// 返回 false 表示任一写步骤失败，调用方必须整体回滚（不能留下半取消状态）。
bool cancelOutboxLocked(const QSqlDatabase &db,const QString &actionKey,const QString &kind,
                        const QString &channel,const QString &session,const QString &reason)
{
    // 已交付/已冻结（handed_off/unknown/withheld）的动作绝不回退：只写提案取消审计，
    // 不改动作/Outbox/账本（"已实际交付动作不回退"）。
    QString currentActionState;
    {
        QSqlQuery current(db);
        current.prepare("SELECT state FROM cognitive_action WHERE action_key=?");
        current.addBindValue(actionKey);
        if (current.exec() && current.next()) currentActionState = current.value(0).toString();
    }
    if (currentActionState == QStringLiteral("handed_off")
        || currentActionState == QStringLiteral("unknown")
        || currentActionState == QStringLiteral("withheld")) {
        QSqlQuery proposalAudit(db);
        proposalAudit.prepare("UPDATE action_proposal SET "
                              "state=CASE WHEN state IN ('proposed','verified') "
                              "THEN 'expired' ELSE state END,"
                              "cancel_reason=?,updated_at=unixepoch() WHERE action_key=?");
        proposalAudit.addBindValue(reason.left(120));
        proposalAudit.addBindValue(actionKey);
        if (!proposalAudit.exec()) {
            qWarning()<<"[Outbox] cancel proposal 失败"<<actionKey<<proposalAudit.lastError();
            return false;
        }
        return true;
    }
    bool ok = true;
    QSqlQuery action(db);
    action.prepare("UPDATE cognitive_action SET state='withheld',last_outcome='withheld',"
                   "updated_at=unixepoch() WHERE action_key=? "
                   "AND state NOT IN ('handed_off','unknown','withheld')");
    action.addBindValue(actionKey);
    if (!action.exec()) {
        qWarning()<<"[Outbox] cancel action 失败"<<actionKey<<action.lastError();
        ok = false;
    }
    QSqlQuery outbox(db);
    outbox.prepare("UPDATE action_outbox SET state='failed',last_error=?,lease_owner=NULL,"
                   "lease_until=NULL,completed_at=coalesce(completed_at,unixepoch()) "
                   "WHERE action_key=? AND state NOT IN ('dispatched','unknown')");
    outbox.addBindValue(reason.left(120));
    outbox.addBindValue(actionKey);
    if (!outbox.exec()) {
        qWarning()<<"[Outbox] cancel outbox 失败"<<actionKey<<outbox.lastError();
        ok = false;
    }
    // 领域行/账本统一落 withheld（contact/reminder/share），不会出现"没发却记为已发"。
    if (!settleOutboxDomain(db,actionKey,kind,channel,session,QStringLiteral("withheld"))) {
        qWarning()<<"[Outbox] cancel domain 失败"<<actionKey<<kind;
        ok = false;
    }
    QSqlQuery proposal(db);
    proposal.prepare("UPDATE action_proposal SET "
                     "state=CASE WHEN state IN ('proposed','verified') THEN 'expired' ELSE state END,"
                     "cancel_reason=?,updated_at=unixepoch() WHERE action_key=?");
    proposal.addBindValue(reason.left(120));
    proposal.addBindValue(actionKey);
    if (!proposal.exec()) {
        qWarning()<<"[Outbox] cancel proposal 失败"<<actionKey<<proposal.lastError();
        ok = false;
    }
    return ok;
}

// R12-2c-2：发送前最终复验的返回结构。
// permanent=true 表示授权/内容/语境已不可恢复（取消终态、不重领）；
// permanent=false 只用于安静时段/每日额度/目标暂时不可达这类可恢复阻塞（保留 queued）。
struct OutboxSendBlock
{
    bool blocked = false;
    bool permanent = false;
    QString reason;
};

// R12-2c-2：发送前最终复验（只读，不发网络、不写库）。
// claimOutbox（领取时）与 ActionOutboxDispatcher/AppController sender（最后一个可取消边界）
// 共用同一判定，避免"只在某个入口检查一次"。新来源（wake:/activity:）做完整复验：
//   action/outbox 状态一致；提案 committed/未取消/未过期；Action Context Fence epoch 未变；
//   verified 内容指纹与权威 payload/claims 一致；target 与动作/Outbox 完全绑定且仍是主人
//   私聊渠道；来源版本/runId/scope/target、成果 artifact_ref/version/hash/share_intent、
//   开关/权限/覆盖范围（ActionPolicy 确定性复核）；安静时段与共享额度。
// legacy（reminder/reflection/thought）保留既有领域有效期语义，只做状态一致性复验。
OutboxSendBlock outboxSendGuard(MemoryManager &memory,const QSqlDatabase &db,
                                const QString &actionKey, bool atChannel = false)
{
    OutboxSendBlock block;
    const auto stop = [&block](bool permanent,const QString &reason) {
        block.blocked = true;
        block.permanent = permanent;
        block.reason = reason;
        return block;
    };
    // 1) action/outbox 状态一致：终态行绝不重领/重发（"withheld 对应 failed" 也在此排除）。
    QString kind, channel, session, actionState, outboxState;
    QString actionPayloadJson, outboxPayloadJson;
    int outboxAttempts = 0;
    {
        QSqlQuery row(db);
        row.prepare(QStringLiteral(
            "SELECT a.kind,a.state,a.channel,a.session_id,a.payload_json,"
            "o.state,o.channel,o.session_id,o.payload_json,o.attempts "
            "FROM cognitive_action a JOIN action_outbox o ON o.action_id=a.id "
            "WHERE a.action_key=?"));
        row.addBindValue(actionKey);
        if (!row.exec() || !row.next()) return stop(true, QStringLiteral("action_missing"));
        kind = row.value(0).toString();
        actionState = row.value(1).toString();
        channel = row.value(2).toString();
        session = row.value(3).toString();
        actionPayloadJson = row.value(4).toString();
        outboxState = row.value(5).toString();
        const QString outboxChannel = row.value(6).toString();
        const QString outboxSession = row.value(7).toString();
        outboxPayloadJson = row.value(8).toString();
        outboxAttempts = row.value(9).toInt();
        if (outboxChannel != channel || outboxSession != session
            || outboxChannel.trimmed().isEmpty() || outboxSession.trimmed().isEmpty())
            return stop(true, QStringLiteral("target_mismatch"));
    }
    if (actionState == QStringLiteral("handed_off") || actionState == QStringLiteral("unknown")
        || actionState == QStringLiteral("withheld"))
        return stop(true, QStringLiteral("already_terminal"));
    if (outboxState == QStringLiteral("unknown") || outboxState == QStringLiteral("dispatched"))
        return stop(true, QStringLiteral("outbox_terminal"));
    if (outboxState == QStringLiteral("failed") && outboxAttempts >= 2)
        return stop(true, QStringLiteral("outbox_terminal"));
    if (outboxState != QStringLiteral("queued") && outboxState != QStringLiteral("dispatching")
        && outboxState != QStringLiteral("failed"))
        return stop(true, QStringLiteral("outbox_invalid_state"));
    // 新来源才做完整 fence/绑定/策略复核；legacy 的领域有效期在各自 commit/settle 校验，
    // 这里只保证 failed 行不会被当作可发送行重领。
    const bool newSource = actionKey.startsWith(QStringLiteral("wake:"))
        || actionKey.startsWith(QStringLiteral("activity:"));
    if (!newSource) {
        if (outboxState == QStringLiteral("failed")
            && (actionState != QStringLiteral("rejected") || outboxAttempts >= 2))
            return stop(true, QStringLiteral("outbox_terminal"));
        if (!atChannel) return block;
        if (!actionReminderAllowed(channel, session))
            return stop(true, QStringLiteral("legacy_target_not_allowed"));
        const auto parts = actionKey.split(QLatin1Char(':'));
        const auto id = parts.value(1).toLongLong();
        const auto version = parts.value(2).toInt();
        if (actionPayloadJson != outboxPayloadJson && kind != QStringLiteral("SpontaneousContact"))
            return stop(true, QStringLiteral("legacy_payload_mismatch"));
        const auto outgoing = QJsonDocument::fromJson(outboxPayloadJson.toUtf8()).object();
        QSqlQuery domain(db);
        if (kind == QStringLiteral("ReminderDelivery")) {
            domain.prepare("SELECT 1 FROM reminder WHERE id=? AND state='dispatching' "
                           "AND due<=unixepoch() AND expires>unixepoch() AND channel=? AND session=? AND message=?");
            domain.addBindValue(id);
            domain.addBindValue(channel);
            domain.addBindValue(session);
            domain.addBindValue(outgoing.value(QStringLiteral("message")).toString());
        } else if (kind == QStringLiteral("SpontaneousContact")) {
            domain.prepare("SELECT 1 FROM reflection_contact c JOIN private_reflection r "
                           "ON c.turn_id=r.turn_id WHERE c.turn_id=? AND c.version=? "
                           "AND c.outcome='dispatching' AND r.state='consider' AND r.version=? AND c.message=?");
            domain.addBindValue(id);
            domain.addBindValue(version);
            domain.addBindValue(version);
            const auto sentences = outgoing.value(QStringLiteral("sentences")).toArray();
            if (sentences.size() != 1)
                return stop(true, QStringLiteral("legacy_payload_mismatch"));
            domain.addBindValue(sentences.first().toObject()
                                    .value(QStringLiteral("zh_text")).toString());
        } else if (kind == QStringLiteral("ActivityShare")) {
            if (!memory.canShareThought(id, version))
                return stop(true, QStringLiteral("legacy_activity_not_shareable"));
            domain.prepare("SELECT 1 FROM chat_history WHERE id=? AND "
                           "json_extract(thought_result,'$.draft')=?");
            domain.addBindValue(id);
            domain.addBindValue(outgoing.value(QStringLiteral("draft")).toString());
        } else {
            return stop(true, QStringLiteral("unsupported_legacy_action"));
        }
        if (!domain.exec() || !domain.next())
            return stop(true, QStringLiteral("legacy_action_no_longer_valid"));
        return block;
    }
    // 2) 提案状态 / 取消审计 / 有效期。
    const QJsonObject proposal = memory.actionProposal(actionKey);
    if (proposal.isEmpty()) return stop(true, QStringLiteral("proposal_missing"));
    const QString proposalState = proposal.value(QStringLiteral("state")).toString();
    if (proposalState != QStringLiteral("committed"))
        return stop(true, proposalState == QStringLiteral("expired")
                              ? QStringLiteral("expired")
                              : QStringLiteral("proposal_not_committed"));
    if (!proposal.value(QStringLiteral("cancel_reason")).toString().trimmed().isEmpty())
        return stop(true, QStringLiteral("proposal_cancelled"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 expiresAt = proposal.value(QStringLiteral("expires_at")).toInteger();
    if (expiresAt > 0 && expiresAt <= now) return stop(true, QStringLiteral("expired"));
    // 3) Action Context Fence：提交后、发送前语境必须仍是提案生成时那一版。
    const qint64 snapshotEpoch = proposal.value(QStringLiteral("action_context_epoch")).toInteger();
    if (snapshotEpoch != memory.actionContextEpoch())
        return stop(true, QStringLiteral("context_epoch_changed"));
    // 4) verified 内容指纹必须与权威 payload/claims 完全一致（防"核验 A、发送 B"）。
    const QJsonObject payload = proposal.value(QStringLiteral("payload")).toObject();
    const QJsonArray claims = proposal.value(QStringLiteral("claims")).toArray();
    const QString verifiedPayloadHash =
        proposal.value(QStringLiteral("verified_payload_hash")).toString();
    const QString verifiedClaimsHash =
        proposal.value(QStringLiteral("verified_claims_hash")).toString();
    if (verifiedPayloadHash.isEmpty() || verifiedClaimsHash.isEmpty()
        || proposal.value(QStringLiteral("verified_request_id")).toString().trimmed().isEmpty())
        return stop(true, QStringLiteral("verified_binding_missing"));
    if (ActionPolicy::canonicalProposalPayloadDigest(payload, claims) != verifiedPayloadHash
        || ActionPolicy(&memory).proposalEvidenceDigest(claims) != verifiedClaimsHash)
        return stop(true, QStringLiteral("verified_content_mismatch"));
    // 5) target 三方绑定：提案 target = 动作行 = Outbox，且仍是允许的主人私聊渠道。
    const QString targetChannel = proposal.value(QStringLiteral("target_channel")).toString();
    const QString targetSession = proposal.value(QStringLiteral("target_session")).toString();
    const QString proposalKind = proposal.value(QStringLiteral("kind")).toString();
    if (targetChannel != channel || targetSession != session)
        return stop(true, QStringLiteral("target_mismatch"));
    auto &cfg = ConfigManager::instance();
    const ProactiveConfig opts = cfg.getProactiveConfig();
    const bool petTarget =
        targetChannel == Channel::kPet && targetSession == Channel::kDefaultSession;
    const bool qqTarget = targetChannel == Channel::kQQPrivate && cfg.isMasterQQ(targetSession);
    if (!petTarget && !qqTarget) return stop(true, QStringLiteral("target_not_owner_private"));
    // 权限/开关先于"目标暂时不可达"判定：主动联系/长期记忆被关闭是授权撤回（永久失效），
    // 不能因为 targets 同时被清空就降级成可恢复阻塞。
    if (!opts.enabled || !cfg.getMemoryPolicyConfig().longTermMemoryEnabled)
        return stop(true, QStringLiteral("proactive_disabled"));
    if (!opts.targets.contains(qMakePair(targetChannel, targetSession)))
        return stop(false, QStringLiteral("target_unreachable"));
    // 动作/Outbox payload 与提案内容绑定（防篡改/串线）。
    const QJsonObject actionPayload = QJsonDocument::fromJson(actionPayloadJson.toUtf8()).object();
    const QJsonObject outboxPayload = QJsonDocument::fromJson(outboxPayloadJson.toUtf8()).object();
    const QString expectedActionKind = proposalKind == QStringLiteral("contact")
        ? kActionKindOpenWakeContact : kActionKindInternalShare;
    if (kind != expectedActionKind
        || actionPayload.value(QStringLiteral("origin_ref")).toString()
               != proposal.value(QStringLiteral("origin_ref")).toString()
        || actionPayload.value(QStringLiteral("origin_version")).toInt()
               != proposal.value(QStringLiteral("origin_version")).toInt())
        return stop(true, QStringLiteral("payload_mismatch"));
    const QString expectedText = proposalKind == QStringLiteral("contact")
        ? payload.value(QStringLiteral("message")).toString().trimmed()
        : payload.value(QStringLiteral("draft")).toString().trimmed();
    const auto payloadText = [](const QJsonObject &value) {
        const QString message = value.value(QStringLiteral("message")).toString().trimmed();
        return message.isEmpty() ? value.value(QStringLiteral("draft")).toString().trimmed()
                                 : message;
    };
    const QString actionText = payloadText(actionPayload);
    const QString outboxText = payloadText(outboxPayload);
    if (expectedText.isEmpty() || actionText != expectedText || outboxText != expectedText)
        return stop(true, QStringLiteral("payload_mismatch"));
    // 6) allow_contact：OpenWake contact 的独立授权撤回（enabled/长期记忆已在上面判过）。
    if (proposalKind == QStringLiteral("contact")
        && proposal.value(QStringLiteral("origin_kind")).toString() == QStringLiteral("wake")
        && !cfg.openWakeAllowContact())
        return stop(true, QStringLiteral("allow_contact_disabled"));
    // 7) 来源真实身份/版本/runId/scope/epoch/target/成果/coverage 统一复核（确定性、无网络）。
    ActionOrigin origin;
    QString originError;
    if (!ActionOrigin::fromJson(proposal.value(QStringLiteral("origin")).toObject(),
                                &origin, &originError)
        || !origin.isValid())
        return stop(true, QStringLiteral("invalid_origin"));
    ActionPolicy policy(&memory);
    const QJsonObject validationInput{
        {QStringLiteral("origin"), proposal.value(QStringLiteral("origin"))},
        {QStringLiteral("kind"), proposalKind},
        {QStringLiteral("action_key"), actionKey},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), targetChannel},
                     {QStringLiteral("session_id"), targetSession}}},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("claims"), claims},
        {QStringLiteral("expires_at"), expiresAt}};
    QString policyError;
    const QJsonObject validation = policy.validateActionProposal(validationInput, &policyError);
    if (!validation.value(QStringLiteral("allowed")).toBool()) {
        const QString reason = validation.value(QStringLiteral("reason")).toString(policyError);
        const QString code = actionProposalErrorCode(reason);
        // 目标类失败可恢复（等通道/配置恢复）；来源失效/成果不符/开关关闭永久失效。
        if (code == QStringLiteral("target_unreachable"))
            return stop(false, code);
        return stop(true, code);
    }
    // 8) 安静时段/共享额度：可恢复阻塞，保留 queued 等下一拍（不占发送 attempts）。
    if (proposalKind == QStringLiteral("contact")) {
        if (actionQuietNow(opts)) return stop(false, QStringLiteral("quiet"));
        if (opts.contactDailyLimit > 0) {
            QSqlQuery used(db);
            // 排除本动作自己的账本行，否则"自己占的额度"会把已提交动作永久挡死。
            used.prepare(QStringLiteral(
                "SELECT count(*) FROM delivery_ledger WHERE kind='contact' "
                "AND date(created_at,'localtime')=date('now','localtime') "
                "AND state<>'withheld' AND ref<>?"));
            used.addBindValue(actionKey);
            if (used.exec() && used.next()
                && used.value(0).toInt() >= opts.contactDailyLimit)
                return stop(false, QStringLiteral("limit"));
        }
    }
    return block;
}

// 恢复（进程崩溃遗留）时的领域回写：at-most-once，只标"不确定"，不增加失败尝试计数。
void recoverOutboxDomain(const QSqlDatabase &db,const QString &actionKey,const QString &kind)
{
    const auto parts=actionKey.split(QLatin1Char(':'));
    QSqlQuery q(db);
    if (kind==QStringLiteral("SpontaneousContact")) {
        if (parts.size()<4) return;
        q.prepare("UPDATE reflection_contact SET outcome='unknown',last_outcome='dispatch_abandoned',"
                  "updated_at=unixepoch() WHERE turn_id=? AND version=? AND outcome='dispatching'");
        q.addBindValue(parts[1].toLongLong());
        q.addBindValue(parts[2].toInt());
        if (!q.exec()) qWarning()<<"[Outbox] 恢复 contact 失败"<<q.lastError();
        return;
    }
    if (kind==QStringLiteral("ReminderDelivery")) {
        if (parts.size()<2) return;
        q.prepare("UPDATE reminder SET state='unknown' WHERE id=? AND state='dispatching'");
        q.addBindValue(parts[1].toLongLong());
        if (!q.exec()) qWarning()<<"[Outbox] 恢复 reminder 失败"<<q.lastError();
        return;
    }
    if (kind==QStringLiteral("ActivityShare")) {
        if (parts.size()<3) return;
        q.prepare("UPDATE delivery_ledger SET state='unknown',last_error='dispatch_abandoned',"
                  "updated_at=CURRENT_TIMESTAMP WHERE kind='share' AND ref=? AND state='claimed'");
        q.addBindValue(QStringLiteral("thought:%1:%2").arg(parts[1]).arg(parts[2]));
        if (!q.exec()) qWarning()<<"[Outbox] 恢复 share 失败"<<q.lastError();
        return;
    }
    if (kind==kActionKindOpenWakeContact) {
        // R12-2b：进程崩溃遗留的新来源 contact 一律冻结 unknown（at-most-once，绝不重发）。
        q.prepare("UPDATE delivery_ledger SET state='unknown',last_error='dispatch_abandoned',"
                  "updated_at=CURRENT_TIMESTAMP WHERE kind='contact' AND ref=? "
                  "AND state NOT IN ('unknown','withheld')");
        q.addBindValue(actionKey);
        if (!q.exec()) {
            qWarning()<<"[Outbox] 恢复 open_wake contact 失败"<<q.lastError();
            return;
        }
        if (parts.size()>=2) {
            QSqlQuery scope(db);
            scope.prepare(QStringLiteral("SELECT scope_key FROM open_wake WHERE id=?"));
            scope.addBindValue(parts[1].toLongLong());
            if (scope.exec() && scope.next()) {
                QSqlQuery stateUpdate(db);
                // R12-2c-3：旧 wake 的恢复也不能覆盖新 wake 的 last_outcome。
                stateUpdate.prepare(QStringLiteral(
                    "UPDATE open_wake_state SET last_outcome='contact_unknown',"
                    "updated_at=unixepoch() WHERE scope_key=? AND last_wake_id=?"));
                stateUpdate.addBindValue(scope.value(0).toString());
                stateUpdate.addBindValue(parts[1].toLongLong());
                if (!stateUpdate.exec())
                    qWarning()<<"[Outbox] 恢复 open_wake_state 失败"<<stateUpdate.lastError();
            }
        }
        return;
    }
    if (kind==kActionKindInternalShare) {
        q.prepare("UPDATE delivery_ledger SET state='unknown',last_error='dispatch_abandoned',"
                  "updated_at=CURRENT_TIMESTAMP WHERE kind='share' AND ref=? "
                  "AND state NOT IN ('unknown','withheld')");
        q.addBindValue(actionKey);
        if (!q.exec()) {
            qWarning()<<"[Outbox] 恢复 internal share 失败"<<q.lastError();
            return;
        }
        if (parts.size()>=3) {
            // R12-2c-3：只对仍指向同一成果版本的当前 share_intent 冻结为 unknown；
            // 旧版本意图已被新成果替换时不得改写新版本的分享结果。
            QSqlQuery patch(db);
            patch.prepare(QStringLiteral(
                "UPDATE internal_activity SET result_json="
                "json_patch(coalesce(result_json,'{}'),?),updated_at=unixepoch() "
                "WHERE id=? AND status='completed' "
                "AND json_extract(result_json,'$.share_intent.artifact_version')=?"));
            patch.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("share_outcome"),QStringLiteral("unknown")},
                {QStringLiteral("share_version"),parts[2].toInt()},
                {QStringLiteral("share_at"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}})
                .toJson(QJsonDocument::Compact)));
            patch.addBindValue(parts[1].toLongLong());
            patch.addBindValue(parts[2].toInt());
            if (!patch.exec())
                qWarning()<<"[Outbox] 恢复 internal share 结果失败"<<patch.lastError();
        }
    }
}

QJsonObject actionCommittedResult(const QString &actionKey,qlonglong actionId,
                                  const QString &kind,const QString &channel,const QString &session)
{
    return QJsonObject{{QStringLiteral("action_key"),actionKey},
                       {QStringLiteral("action_id"),actionId},
                       {QStringLiteral("kind"),kind},
                       {QStringLiteral("state"),QStringLiteral("queued")},
                       {QStringLiteral("channel"),channel},
                       {QStringLiteral("session"),session}};
}

// 在事务内插入 action + outbox 两行。payloadJson 由调用方按 kind 构造。
// 切片 C：主动联系可以把动机/claims 摘要带进 cognitive_action（审计用），
// outbox 仍只携带真正要发送的文本（outboxPayloadJson 为空时沿用 payloadJson）。
bool insertActionWithOutbox(const QSqlDatabase &db,const QString &actionKey,const QString &kind,
                            const QString &triggerRef,const QString &channel,const QString &session,
                            const QString &payloadJson,const QString &policyReason,
                            qlonglong *actionId,
                            const QString &claimsJson = QString(),
                            const QString &motivationType = QString(),
                            const QString &motivationRef = QString(),
                            const QString &semanticStatus = QString(),
                            const QString &contextRefsJson = QString(),
                            const QString &outboxPayloadJson = QString())
{
    QSqlQuery action(db);
    action.prepare("INSERT INTO cognitive_action(action_key,kind,trigger_ref,subject,channel,session_id,"
                   "motivation_type,motivation_ref,payload_json,claims_json,context_refs_json,"
                   "state,policy_reason,semantic_status,attempts,created_at,updated_at) "
                   "VALUES(?,?,?,'user',?,?,?,?,?,?,?,'queued',?,?,0,unixepoch(),unixepoch())");
    action.addBindValue(actionKey);
    action.addBindValue(kind);
    action.addBindValue(triggerRef.isEmpty() ? QVariant() : QVariant(triggerRef));
    action.addBindValue(channel);
    action.addBindValue(session);
    action.addBindValue(motivationType.isEmpty() ? QVariant() : QVariant(motivationType));
    action.addBindValue(motivationRef.isEmpty() ? QVariant() : QVariant(motivationRef));
    action.addBindValue(payloadJson);
    action.addBindValue(claimsJson.isEmpty() ? QVariant() : QVariant(claimsJson));
    action.addBindValue(contextRefsJson.isEmpty() ? QVariant() : QVariant(contextRefsJson));
    action.addBindValue(policyReason.isEmpty() ? QVariant() : QVariant(policyReason));
    action.addBindValue(semanticStatus.isEmpty() ? QVariant() : QVariant(semanticStatus));
    if (!action.exec()) {
        qWarning()<<"[Outbox] action 写入失败"<<actionKey<<action.lastError();
        return false;
    }
    const qlonglong id=action.lastInsertId().toLongLong();
    QSqlQuery outbox(db);
    outbox.prepare("INSERT INTO action_outbox(action_id,action_key,channel,session_id,payload_json,"
                   "state,attempts) VALUES(?,?,?,?,?,'queued',0)");
    outbox.addBindValue(id);
    outbox.addBindValue(actionKey);
    outbox.addBindValue(channel);
    outbox.addBindValue(session);
    outbox.addBindValue(outboxPayloadJson.isEmpty() ? payloadJson : outboxPayloadJson);
    if (!outbox.exec()) {
        qWarning()<<"[Outbox] outbox 写入失败"<<actionKey<<outbox.lastError();
        return false;
    }
    if (actionId) *actionId=id;
    return true;
}

} // namespace

QJsonObject MemoryManager::commitReflectionContactAction(qlonglong turn,int version,
                                                         const QString &policyReason)
{
    if (!m_db.isOpen() || turn<=0 || version<=0)
        return actionError(QStringLiteral("invalid_args"));
    const QString actionKey=QStringLiteral("reflection:%1:%2:contact").arg(turn).arg(version);
    // 幂等：同一联系意图只允许一次本地提交，重复调用返回已有 state（不重复建 outbox）。
    const auto existing=existingAction(m_db,actionKey);
    if (!existing.isEmpty()) return existing;

    auto &cfg=ConfigManager::instance();
    const auto opts=cfg.getProactiveConfig();
    const auto memory=cfg.getMemoryPolicyConfig();
    if (!opts.enabled || !memory.longTermMemoryEnabled)
        return actionError(QStringLiteral("proactive_disabled"));
    if (!m_db.transaction()) return actionError(QStringLiteral("transaction_failed"));
    auto rollback=qScopeGuard([this]{m_db.rollback();});

    // 每日"现在联系"投递上限：与 claimReflectionContact 一致，按 delivery_ledger 的 contact 行计数。
    if (opts.contactDailyLimit > 0) {
        QSqlQuery used(m_db);
        if (used.exec("SELECT count(*) FROM delivery_ledger WHERE kind='contact' "
                      "AND date(created_at,'localtime')=date('now','localtime') "
                      "AND state<>'withheld'")
            && used.next() && used.value(0).toInt() >= opts.contactDailyLimit)
            return actionError(QStringLiteral("daily_limit"));
    }

    // 域校验：完全复用 claimReflectionContact 的领取条件（来源轮次/关注状态/版本/重试预算）。
    QSqlQuery q(m_db);
    q.prepare("SELECT c.message,h.channel,h.session_id,s.input"+kActionContactSourceRows+
              "JOIN private_reflection r ON r.turn_id=h.id JOIN reflection_contact c ON c.turn_id=r.turn_id "
              "WHERE "+kActionContactEligible+" AND c.turn_id=? AND c.version=? AND r.version=c.version "
              "AND r.state='consider' AND (c.outcome='proposed' "
              "OR (c.outcome='rejected' AND c.attempts<2)) AND NOT EXISTS "
              "(SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)");
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return actionError(QStringLiteral("not_claimable"));
    const auto message=q.value(0).toString();
    const auto sourceChannel=q.value(1).toString();
    const auto sourceSession=q.value(2).toString();
    const auto sourceInput=q.value(3).toString();
    q.finish();

    // 目标选择 + 可见性/敏感内容校验（与领取时同一套 MemoryReadPolicy 口径）。
    ActionTarget target;
    {
        auto &config=ConfigManager::instance();
        const auto policy=MemoryReadPolicy();
        ActionTarget qqFallback;
        for (const auto &pair : opts.targets) {
            const QString channel=pair.first, session=pair.second;
            const bool pet=channel==Channel::kPet && session==Channel::kDefaultSession;
            const bool qq=channel==Channel::kQQPrivate && config.isMasterQQ(session);
            if (!pet && !qq) continue;
            if (!policy.allows(channel,session,message)) continue;
            if ((!memory.agent.ownerCrossChannelPrivate
                 && (sourceChannel!=channel || sourceSession!=session))
                || !actionBasisVisible(m_db,turn,policy,channel,session)
                || !policy.allows(sourceChannel,sourceSession,sourceInput)) continue;
            if (pet) { target=ActionTarget{channel,session,true}; break; }
            if (!qqFallback.found) qqFallback=ActionTarget{channel,session,true};
        }
        if (!target.found) target=qqFallback;
    }
    if (!target.found) return actionError(QStringLiteral("no_target"));

    // 持久化依据重验：提出联系到真正提交之间可能隔了几分钟，只有"此刻仍站得住"的
    // now 联系才允许进入 outbox；否则不写任何行，由调用方把该意图落 withheld。
    // 切片 C：新契约（claims/motivation）以 claims 为主要事实来源，旧 basis 仅作兼容路径。
    QSqlQuery persisted(m_db);
    persisted.prepare("SELECT c.timing,coalesce(c.basis_type,''),coalesce(c.basis_refs,''),"
                      "coalesce(c.claims_json,''),coalesce(c.motivation_json,''),"
                      "coalesce(c.semantic_status,'') "
                      "FROM reflection_contact c WHERE c.turn_id=? AND c.version=?");
    persisted.addBindValue(turn);
    persisted.addBindValue(version);
    if (!persisted.exec() || !persisted.next())
        return actionError(QStringLiteral("missing_contact"));
    const auto contactTiming=persisted.value(0).toString();
    const auto basisType=persisted.value(1).toString();
    const auto basisRefsText=persisted.value(2).toString();
    const auto claimsText=persisted.value(3).toString();
    const auto motivationText=persisted.value(4).toString();
    const auto semanticStatus=persisted.value(5).toString();
    persisted.finish();
    const bool newContract=!claimsText.isEmpty() || !motivationText.isEmpty();
    if (newContract) {
        // 切片 C：提交前用同一套确定性规则再核验一次 claims（不调用语义验证器；
        // 语义结论已由 LLMService 写进 reflection_contact.semantic_status）。
        const ActionPolicy policy(this);
        const auto claims=QJsonDocument::fromJson(claimsText.toUtf8()).array();
        for (const auto &value : claims) {
            const auto validation=policy.validateClaimDeterministic(sourceChannel,sourceSession,
                                                                    turn,value.toObject());
            if (validation.result!=ClaimValidation::Entailed)
                return actionError(QStringLiteral("claims_invalid"),validation.reason);
        }
        if (semanticStatus==QStringLiteral("withheld")
            || semanticStatus==QStringLiteral("repair_needed")
            || semanticStatus==QStringLiteral("unverified"))
            return actionError(QStringLiteral("semantic_withheld"),semanticStatus);
    }
    if (contactTiming==QStringLiteral("now") && !newContract) {
        QString rejection;
        QJsonArray refs;
        if (basisType.isEmpty() || basisRefsText.isEmpty()) {
            rejection=QStringLiteral("missing_basis");
        } else {
            refs=QJsonDocument::fromJson(basisRefsText.toUtf8()).array();
            if (refs.isEmpty()) rejection=QStringLiteral("missing_basis");
        }
        if (rejection.isEmpty()) {
            const QJsonObject contact{
                {QStringLiteral("timing"),contactTiming},
                {QStringLiteral("basis"),QJsonObject{{QStringLiteral("type"),basisType},
                                                     {QStringLiteral("refs"),refs}}}};
            rejection=validateContactBasis(sourceChannel,sourceSession,turn,contact);
        }
        if (!rejection.isEmpty()) return actionError(QStringLiteral("basis_invalid"),rejection);
    } else if (contactTiming==QStringLiteral("now")) {
        // 新契约也走一遍确定性联系策略（motivation/claims/quiet/目标/重复），
        // 但不要因为"缺 verifier"在这里二次发起语义请求。
        const auto claims=QJsonDocument::fromJson(claimsText.toUtf8()).array();
        const auto motivation=QJsonDocument::fromJson(motivationText.toUtf8()).object();
        QJsonObject contact{{QStringLiteral("timing"),contactTiming},
                            {QStringLiteral("motivation"),motivation},
                            {QStringLiteral("claims"),claims}};
        const ActionPolicy policy(this);
        const auto deterministic=policy.evaluateSpontaneousContactDeterministic(
            sourceChannel,sourceSession,turn,contact);
        if (!deterministic.value(QStringLiteral("allowed")).toBool())
            return actionError(QStringLiteral("policy_suppressed"),
                               deterministic.value(QStringLiteral("reason")).toString());
    }

    // 域状态 CAS：proposed（或 rejected 且 attempts<2）→ dispatching。与 action/outbox 同事务。
    QSqlQuery claim(m_db);
    claim.prepare("UPDATE reflection_contact SET outcome='dispatching',channel=?,session=?,"
                  "last_attempt_at=unixepoch(),updated_at=unixepoch() "
                  "WHERE turn_id=? AND version=? AND (outcome='proposed' "
                  "OR (outcome='rejected' AND attempts<2))");
    claim.addBindValue(target.channel);
    claim.addBindValue(target.session);
    claim.addBindValue(turn);
    claim.addBindValue(version);
    if (!claim.exec() || claim.numRowsAffected()!=1)
        return actionError(QStringLiteral("claim_race"));

    // outbox 只带发送文本；cognitive_action 另带动机/claims 摘要供审计。
    const QString outboxPayloadJson=QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("sentences"),QJsonArray{QJsonObject{{QStringLiteral("zh_text"),message}}}},
        {QStringLiteral("channel"),target.channel},
        {QStringLiteral("session"),target.session}}).toJson(QJsonDocument::Compact));
    QJsonObject actionPayload=QJsonDocument::fromJson(outboxPayloadJson.toUtf8()).object();
    QJsonObject motivation;
    QJsonArray claims;
    if (newContract) {
        motivation=QJsonDocument::fromJson(motivationText.toUtf8()).object();
        claims=QJsonDocument::fromJson(claimsText.toUtf8()).array();
        if (!motivation.isEmpty()) actionPayload[QStringLiteral("motivation")]=motivation;
        if (!claims.isEmpty()) actionPayload[QStringLiteral("claims")]=claims;
    }
    const QString payloadJson=QString::fromUtf8(
        QJsonDocument(actionPayload).toJson(QJsonDocument::Compact));
    QString contextRefsJson;
    {
        QJsonArray refs;
        for (const auto &claim : claims)
            for (const auto &ref : claim.toObject().value(QStringLiteral("refs")).toArray())
                refs.append(ref);
        const auto motivationRef=motivation.value(QStringLiteral("ref")).toString();
        if (!motivationRef.isEmpty()) refs.append(motivationRef);
        if (!refs.isEmpty()) contextRefsJson=QString::fromUtf8(
            QJsonDocument(refs).toJson(QJsonDocument::Compact));
    }
    qlonglong actionId=0;
    if (!insertActionWithOutbox(m_db,actionKey,QStringLiteral("SpontaneousContact"),
                                QStringLiteral("reflection:%1:%2").arg(turn).arg(version),
                                target.channel,target.session,payloadJson,policyReason,&actionId,
                                claimsText,
                                motivation.value(QStringLiteral("type")).toString(),
                                motivation.value(QStringLiteral("ref")).toString(),
                                semanticStatus,contextRefsJson,outboxPayloadJson))
        return actionError(QStringLiteral("insert_failed"));
    actionNoteDelivery(m_db,QStringLiteral("contact"),
                       QStringLiteral("reflection:%1:%2").arg(turn).arg(version),
                       target.channel,target.session,message,QStringLiteral("claimed"),0);
    if (!m_db.commit()) return actionError(QStringLiteral("commit_failed"));
    rollback.dismiss();
    qInfo()<<"[Outbox] contact committed turn="<<turn<<"version="<<version
           <<"channel="<<target.channel<<"key="<<actionKey;
    return actionCommittedResult(actionKey,actionId,QStringLiteral("SpontaneousContact"),
                                 target.channel,target.session);
}

QJsonObject MemoryManager::commitReminderDeliveryAction(qlonglong reminderId)
{
    if (!m_db.isOpen() || reminderId<=0) return actionError(QStringLiteral("invalid_args"));
    const QString actionKey=QStringLiteral("reminder:%1:delivery").arg(reminderId);
    const auto existing=existingAction(m_db,actionKey);
    if (!existing.isEmpty()) return existing;

    auto &cfg=ConfigManager::instance();
    const auto opts=cfg.getProactiveConfig();
    if (!opts.enabled || !cfg.getMemoryPolicyConfig().longTermMemoryEnabled)
        return actionError(QStringLiteral("proactive_disabled"));
    if (!m_db.transaction()) return actionError(QStringLiteral("transaction_failed"));
    auto rollback=qScopeGuard([this]{m_db.rollback();});

    QSqlQuery q(m_db);
    q.prepare("SELECT channel,session,message,state,due,expires FROM reminder WHERE id=?");
    q.addBindValue(reminderId);
    if (!q.exec() || !q.next()) return actionError(QStringLiteral("reminder_missing"));
    const auto channel=q.value(0).toString(), session=q.value(1).toString();
    const auto message=q.value(2).toString(), state=q.value(3).toString();
    const auto due=q.value(4).toLongLong(), expires=q.value(5).toLongLong();
    q.finish();
    // 与 claimDueReminder 等价的领域校验：只有 pending 且已到期、未过期才允许提交。
    if (state!=QStringLiteral("pending")) return actionError(QStringLiteral("not_pending"));
    const qint64 now=QDateTime::currentSecsSinceEpoch();
    if (due>now) return actionError(QStringLiteral("not_due"));
    if (expires<=now) return actionError(QStringLiteral("expired"));
    if (!actionReminderAllowed(channel,session)) return actionError(QStringLiteral("target_not_allowed"));
    if (!MemoryReadPolicy().allows(channel,session,message))
        return actionError(QStringLiteral("blocked_message"));

    QSqlQuery claim(m_db);
    claim.prepare("UPDATE reminder SET state='dispatching' WHERE id=? AND state='pending' "
                  "AND due<=unixepoch() AND expires>unixepoch()");
    claim.addBindValue(reminderId);
    if (!claim.exec() || claim.numRowsAffected()!=1)
        return actionError(QStringLiteral("claim_race"));

    const QString payloadJson=QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("message"),message},
        {QStringLiteral("channel"),channel},
        {QStringLiteral("session"),session},
        {QStringLiteral("reminder_id"),reminderId}}).toJson(QJsonDocument::Compact));
    qlonglong actionId=0;
    if (!insertActionWithOutbox(m_db,actionKey,QStringLiteral("ReminderDelivery"),
                                QStringLiteral("reminder:%1").arg(reminderId),
                                channel,session,payloadJson,QString{},&actionId))
        return actionError(QStringLiteral("insert_failed"));
    actionNoteDelivery(m_db,QStringLiteral("reminder"),QStringLiteral("reminder:%1").arg(reminderId),
                       channel,session,message,QStringLiteral("claimed"),0);
    if (!m_db.commit()) return actionError(QStringLiteral("commit_failed"));
    rollback.dismiss();
    qInfo()<<"[Outbox] reminder committed id="<<reminderId<<"channel="<<channel;
    return actionCommittedResult(actionKey,actionId,QStringLiteral("ReminderDelivery"),channel,session);
}

QJsonObject MemoryManager::commitActivityShareAction(qlonglong activityId,int version,
                                                     const QString &draft)
{
    if (!m_db.isOpen() || activityId<=0 || version<0)
        return actionError(QStringLiteral("invalid_args"));
    if (draft.trimmed().isEmpty()) return actionError(QStringLiteral("empty_draft"));
    const QString actionKey=QStringLiteral("thought:%1:%2:share").arg(activityId).arg(version);
    const auto existing=existingAction(m_db,actionKey);
    if (!existing.isEmpty()) return existing;

    auto &cfg=ConfigManager::instance();
    const auto opts=cfg.getProactiveConfig();
    if (!opts.enabled || !cfg.getMemoryPolicyConfig().longTermMemoryEnabled)
        return actionError(QStringLiteral("proactive_disabled"));
    if (!m_db.transaction()) return actionError(QStringLiteral("transaction_failed"));
    auto rollback=qScopeGuard([this]{m_db.rollback();});

    // 分享资格：completed/版本匹配/有产物/同版本未送达（含 unknown 视为可能已送达）。
    if (!canShareThought(activityId,version))
        return actionError(QStringLiteral("not_shareable"));
    const QString ledgerRef=QStringLiteral("thought:%1:%2").arg(activityId).arg(version);
    {
        QSqlQuery shared(m_db);
        shared.prepare("SELECT 1 FROM delivery_ledger WHERE kind='share' AND ref=? "
                       "AND state IN ('handed_off','protocol_accepted','unknown') LIMIT 1");
        shared.addBindValue(ledgerRef);
        if (shared.exec() && shared.next()) return actionError(QStringLiteral("already_shared"));
    }

    const ActionTarget target=actionChooseTarget(opts.targets,draft,true);
    if (!target.found) return actionError(QStringLiteral("no_target"));

    QSqlQuery ledger(m_db);
    ledger.prepare("INSERT INTO delivery_ledger(kind,ref,channel,session,text,state,attempts,updated_at) "
                   "VALUES('share',?,?,?,?,'claimed',0,CURRENT_TIMESTAMP) "
                   "ON CONFLICT(kind,ref) DO UPDATE SET state='claimed',"
                   "channel=excluded.channel,session=excluded.session,text=excluded.text,"
                   "updated_at=CURRENT_TIMESTAMP");
    ledger.addBindValue(ledgerRef);
    ledger.addBindValue(target.channel);
    ledger.addBindValue(target.session);
    ledger.addBindValue(draft);
    if (!ledger.exec()) return actionError(QStringLiteral("ledger_failed"));

    const QString payloadJson=QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("draft"),draft},
        {QStringLiteral("channel"),target.channel},
        {QStringLiteral("session"),target.session}}).toJson(QJsonDocument::Compact));
    qlonglong actionId=0;
    if (!insertActionWithOutbox(m_db,actionKey,QStringLiteral("ActivityShare"),ledgerRef,
                                target.channel,target.session,payloadJson,QString{},&actionId))
        return actionError(QStringLiteral("insert_failed"));
    if (!m_db.commit()) return actionError(QStringLiteral("commit_failed"));
    rollback.dismiss();
    qInfo()<<"[Outbox] share committed activity="<<activityId<<"version="<<version
           <<"channel="<<target.channel;
    return actionCommittedResult(actionKey,actionId,QStringLiteral("ActivityShare"),
                                 target.channel,target.session);
}

QJsonObject MemoryManager::claimOutbox(const QString &owner,int leaseSeconds)
{
    if (!m_db.isOpen() || owner.isEmpty()) return {};
    if (leaseSeconds<=0) leaseSeconds=60;
    if (!m_db.transaction()) return {};
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    // 只领取 queued，或"被渠道明确拒绝且还有一次重试预算"的失败行；
    // dispatching（可能已交给网络）永远不在这里重领——它只能由 recoverStaleOutbox 冻结为 unknown。
    // R12-2c-2：显式排除 action 终态（handed_off/unknown/withheld）。withheld 分支会把
    // outbox 置 failed 且 attempts 可能仍 <2，以前会被这条 attempts<2 重新领取并再发一次；
    // 现在由 a.state 判断排除（重点：withheld 对应 failed 不得重领，不能只看 attempts）。
    QSqlQuery pick(m_db);
    pick.prepare(QStringLiteral(
        "SELECT o.id,o.action_id,o.action_key,o.channel,o.session_id,o.payload_json,"
        "o.attempts,a.kind FROM action_outbox o JOIN cognitive_action a ON a.id=o.action_id "
        "WHERE (o.state='queued' OR (o.state='failed' AND o.attempts<2 AND a.state='rejected')) "
        "AND a.state IN ('queued','rejected') ORDER BY o.id LIMIT 8"));
    if (!pick.exec()) {
        m_db.rollback();
        return {};
    }
    struct Candidate {
        qlonglong outboxId = 0;
        qlonglong actionId = 0;
        QString actionKey;
        QString channel;
        QString session;
        QString payloadJson;
        int attempts = 0;
        QString kind;
    };
    QList<Candidate> candidates;
    while (pick.next()) {
        candidates.append(Candidate{pick.value(0).toLongLong(), pick.value(1).toLongLong(),
                                    pick.value(2).toString(), pick.value(3).toString(),
                                    pick.value(4).toString(), pick.value(5).toString(),
                                    pick.value(6).toInt(), pick.value(7).toString()});
    }
    pick.finish();
    if (candidates.isEmpty()) {
        m_db.rollback();
        return {};
    }
    // 领取时复验：永久失效当场取消（不发送、不重领）；可恢复阻塞保留 queued 等下一拍。
    bool cancelledAny = false;
    for (const auto &candidate : candidates) {
        const OutboxSendBlock guard = outboxSendGuard(*this, m_db, candidate.actionKey);
        if (guard.blocked) {
            if (guard.permanent) {
                // 终态（可能被另一个连接/回执并发推进）只跳过，不回退动作/账本。
                if (guard.reason == QStringLiteral("already_terminal")
                    || guard.reason == QStringLiteral("outbox_terminal"))
                    continue;
                if (!cancelOutboxLocked(m_db, candidate.actionKey, candidate.kind,
                                        candidate.channel, candidate.session, guard.reason)) {
                    m_db.rollback();
                    return {};
                }
                cancelledAny = true;
                qInfo() << "[Outbox] cancelled_at_claim key=" << candidate.actionKey
                        << "reason=" << guard.reason;
            }
            continue; // 可恢复：本次不领，行仍 queued；不消耗 attempts
        }
        QSqlQuery claim(m_db);
        claim.prepare("UPDATE action_outbox SET state='dispatching',lease_owner=?,"
                      "lease_until=unixepoch()+?,dispatched_at=coalesce(dispatched_at,unixepoch()),"
                      "attempts=attempts+1 WHERE id=? AND (state='queued' "
                      "OR (state='failed' AND attempts<2))");
        claim.addBindValue(owner);
        claim.addBindValue(leaseSeconds);
        claim.addBindValue(candidate.outboxId);
        if (!claim.exec() || claim.numRowsAffected()!=1) {
            m_db.rollback();
            return {};
        }
        QSqlQuery touch(m_db);
        touch.prepare("UPDATE cognitive_action SET state='dispatching',updated_at=unixepoch() "
                      "WHERE id=? AND state NOT IN ('unknown','withheld')");
        touch.addBindValue(candidate.actionId);
        if (!touch.exec()) {
            m_db.rollback();
            return {};
        }
        if (!markOutboxDomainDispatching(m_db,candidate.actionKey,candidate.kind,
                                         candidate.channel,candidate.session)) {
            m_db.rollback();
            return {};
        }
        if (!m_db.commit()) return {};
        rollback.dismiss();
        qInfo()<<"[Outbox] claimed key="<<candidate.actionKey<<"kind="<<candidate.kind
               <<"attempt="<<(candidate.attempts+1);
        return QJsonObject{{QStringLiteral("outbox_id"),candidate.outboxId},
                           {QStringLiteral("action_id"),candidate.actionId},
                           {QStringLiteral("action_key"),candidate.actionKey},
                           {QStringLiteral("kind"),candidate.kind},
                           {QStringLiteral("channel"),candidate.channel},
                           {QStringLiteral("session"),candidate.session},
                           {QStringLiteral("payload_json"),candidate.payloadJson},
                           {QStringLiteral("payload"),
                            QJsonDocument::fromJson(candidate.payloadJson.toUtf8()).object()},
                           {QStringLiteral("attempts"),candidate.attempts+1}};
    }
    // 只做了取消（或全部可恢复阻塞）：提交取消结果后返回空，下一拍再试。
    if (!m_db.commit()) return {};
    rollback.dismiss();
    if (cancelledAny) qInfo() << "[Outbox] claim skipped: invalid rows cancelled";
    return {};
}

QJsonObject MemoryManager::validateOutboxActionBeforeSend(const QString &actionKey, bool atChannel)
{
    const QString key = actionKey.trimmed();
    QJsonObject result{{QStringLiteral("ok"),false},
                       {QStringLiteral("permanent"),false},
                       {QStringLiteral("reason"),QStringLiteral("db_error")}};
    if (!m_db.isOpen() || key.isEmpty()) return result;
    const OutboxSendBlock block = outboxSendGuard(*this, m_db, key, atChannel);
    result[QStringLiteral("ok")] = !block.blocked;
    result[QStringLiteral("permanent")] = block.permanent;
    result[QStringLiteral("reason")] = block.reason;
    // 附带只读审计字段（供 dispatcher/日志分类；不作为发送依据）。
    QSqlQuery row(m_db);
    row.prepare(QStringLiteral(
        "SELECT a.state,o.state,a.channel,a.session_id FROM cognitive_action a "
        "JOIN action_outbox o ON o.action_id=a.id WHERE a.action_key=?"));
    row.addBindValue(key);
    if (row.exec() && row.next()) {
        result[QStringLiteral("action_state")] = row.value(0).toString();
        result[QStringLiteral("outbox_state")] = row.value(1).toString();
        result[QStringLiteral("channel")] = row.value(2).toString();
        result[QStringLiteral("session")] = row.value(3).toString();
    }
    return result;
}

bool MemoryManager::deferOutboxAction(const QString &actionKey,const QString &reason)
{
    const QString key = actionKey.trimmed();
    if (!m_db.isOpen() || key.isEmpty()) return false;
    if (!key.startsWith(QStringLiteral("wake:")) && !key.startsWith(QStringLiteral("activity:")))
        return false; // 仅新来源动作使用可恢复回队；legacy 保留既有 rejected 重试语义
    if (!m_db.transaction()) return false;
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery outbox(m_db);
    // 不算一次发送：attempts 回退，租约清空，状态回 queued。
    outbox.prepare("UPDATE action_outbox SET state='queued',lease_owner=NULL,lease_until=NULL,"
                   "last_error=?,attempts=CASE WHEN attempts>0 THEN attempts-1 ELSE 0 END "
                   "WHERE action_key=? AND state='dispatching'");
    outbox.addBindValue(reason.left(120));
    outbox.addBindValue(key);
    if (!outbox.exec() || outbox.numRowsAffected()!=1) {
        m_db.rollback();
        rollback.dismiss();
        return false;
    }
    QSqlQuery action(m_db);
    action.prepare("UPDATE cognitive_action SET state='queued',updated_at=unixepoch() "
                   "WHERE action_key=? AND state='dispatching'");
    action.addBindValue(key);
    if (!action.exec()) {
        m_db.rollback();
        rollback.dismiss();
        return false;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return false;
    }
    rollback.dismiss();
    qInfo() << "[Outbox] deferred key=" << key << "reason=" << reason;
    return true;
}

bool MemoryManager::cancelOutboxAction(const QString &actionKey,const QString &reason)
{
    const QString key = actionKey.trimmed();
    const QString why = reason.trimmed().left(120);
    if (!m_db.isOpen() || key.isEmpty() || why.isEmpty()) return false;
    if (!m_db.transaction()) return false;
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QString kind, channel, session;
    {
        QSqlQuery row(m_db);
        row.prepare("SELECT a.kind,a.channel,a.session_id FROM cognitive_action a "
                    "JOIN action_outbox o ON o.action_id=a.id WHERE a.action_key=?");
        row.addBindValue(key);
        if (!row.exec() || !row.next()) {
            m_db.rollback();
            rollback.dismiss();
            return false;
        }
        kind = row.value(0).toString();
        channel = row.value(1).toString();
        session = row.value(2).toString();
    }
    if (!cancelOutboxLocked(m_db, key, kind, channel, session, why)) {
        m_db.rollback();
        rollback.dismiss();
        return false;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return false;
    }
    rollback.dismiss();
    qInfo() << "[Outbox] cancelled key=" << key << "reason=" << why;
    return true;
}

bool MemoryManager::settleOutboxAction(const QString &actionKey,const QString &outcome,
                                       const QString &detail)
{
    static const QSet<QString> outcomes{
        QStringLiteral("protocol_accepted"), QStringLiteral("handed_off"),
        QStringLiteral("rejected"), QStringLiteral("unknown"), QStringLiteral("withheld")};
    if (!m_db.isOpen() || actionKey.isEmpty() || !outcomes.contains(outcome)) return false;
    if (!m_db.transaction()) return false;
    auto rollback=qScopeGuard([this]{m_db.rollback();});

    QSqlQuery find(m_db);
    find.prepare("SELECT a.id,a.kind,a.channel,a.session_id,a.state,coalesce(a.last_outcome,''),"
                 "o.id,o.attempts,o.channel,o.session_id FROM cognitive_action a "
                 "JOIN action_outbox o ON o.action_id=a.id WHERE a.action_key=?");
    find.addBindValue(actionKey);
    if (!find.exec() || !find.next()) return false;
    const auto actionId=find.value(0).toLongLong();
    const auto kind=find.value(1).toString();
    const auto channel=find.value(2).toString();
    const auto session=find.value(3).toString();
    const auto state=find.value(4).toString();
    const auto lastOutcome=find.value(5).toString();
    const auto outboxAttempts=find.value(7).toInt();
    const auto outboxChannel=find.value(8).toString();
    const auto outboxSession=find.value(9).toString();
    find.finish();

    // 终态保护：unknown/withheld 冻结，绝不因迟到的重复回执再变；已协议接受的只允许同结果重复。
    if (state==QStringLiteral("unknown") || state==QStringLiteral("withheld")) {
        m_db.rollback();
        rollback.dismiss();
        return true;
    }
    // R12-2c-3：渠道匹配。Outbox 路由必须与动作行完全一致，否则回执无法证明属于这条动作；
    // 对不存在/路由串线的回执不落账本、不触发历史投影。
    if (channel.trimmed().isEmpty() || session.trimmed().isEmpty()
        || outboxChannel != channel || outboxSession != session) {
        m_db.rollback();
        rollback.dismiss();
        qWarning() << "[Outbox] receipt_channel_mismatch key=" << actionKey
                   << "action=" << channel << session
                   << "outbox=" << outboxChannel << outboxSession;
        return false;
    }
    // R12-2c-3：新来源回执只允许来自已提交（committed）的提案；未提交/未知动作不写成功状态。
    // 状态必须仍是可领取/已领取（dispatching）或已交付（handed_off），其余状态不可接受。
    const bool newSource = actionKey.startsWith(QStringLiteral("wake:"))
        || actionKey.startsWith(QStringLiteral("activity:"));
    if (newSource && state != QStringLiteral("handed_off")) {
        const QJsonObject proposal = actionProposal(actionKey);
        if (proposal.value(QStringLiteral("state")).toString() != QStringLiteral("committed")
            || proposal.value(QStringLiteral("committed_action_key")).toString() != actionKey) {
            m_db.rollback();
            rollback.dismiss();
            qWarning() << "[Outbox] receipt_proposal_not_committed key=" << actionKey
                       << "state=" << proposal.value(QStringLiteral("state")).toString();
            return false;
        }
        if (state != QStringLiteral("dispatching")) {
            m_db.rollback();
            rollback.dismiss();
            qWarning() << "[Outbox] receipt_state_not_acceptable key=" << actionKey
                       << "state=" << state;
            return false;
        }
    }
    if (state==QStringLiteral("handed_off")) {
        // 已协议接受是终态：重复的 accepted/handed_off 回执不再重复累加，也不再触发订阅方回写历史。
        if (outcome==QStringLiteral("protocol_accepted")
            && lastOutcome==QStringLiteral("protocol_accepted")) {
            m_db.rollback();
            rollback.dismiss();
            return true;
        }
        if (outcome!=QStringLiteral("protocol_accepted")
            && (outcome==lastOutcome || lastOutcome==QStringLiteral("protocol_accepted"))) {
            m_db.rollback();
            rollback.dismiss();
            return true;
        }
    }

    const bool accepted = outcome==QStringLiteral("protocol_accepted")
        || outcome==QStringLiteral("handed_off");
    const bool unknown = outcome==QStringLiteral("unknown");
    const bool withheld = outcome==QStringLiteral("withheld");
    const bool failed = outcome==QStringLiteral("rejected") || unknown;

    QString outboxState;
    if (accepted) outboxState=QStringLiteral("dispatched");
    else if (unknown) outboxState=QStringLiteral("unknown");
    else if (withheld) outboxState=QStringLiteral("failed");
    else outboxState = outboxAttempts<2 ? QStringLiteral("queued") : QStringLiteral("failed");
    // action.state 只收敛到四个对外语义：handed_off（含协议接受）/rejected/unknown/withheld。
    const QString actionState = accepted ? QStringLiteral("handed_off") : outcome;

    QSqlQuery action(m_db);
    action.prepare("UPDATE cognitive_action SET state=?,last_outcome=?,attempts=attempts+1,"
                   "updated_at=unixepoch() WHERE id=?");
    action.addBindValue(actionState);
    action.addBindValue(outcome);
    action.addBindValue(actionId);
    if (!action.exec()) return false;

    QSqlQuery outbox(m_db);
    outbox.prepare("UPDATE action_outbox SET state=?,last_error=?,lease_owner=NULL,lease_until=NULL,"
                   "completed_at=CASE WHEN ? IN ('dispatched','unknown','failed') "
                   "THEN unixepoch() ELSE completed_at END WHERE action_id=?");
    outbox.addBindValue(outboxState);
    outbox.addBindValue(detail.isEmpty() ? QVariant() : QVariant(detail));
    outbox.addBindValue(outboxState);
    outbox.addBindValue(actionId);
    if (!outbox.exec()) return false;

    if (!settleOutboxDomain(m_db,actionKey,kind,channel,session,outcome)) return false;
    if (!m_db.commit()) return false;
    rollback.dismiss();
    qInfo()<<"[Outbox] settled key="<<actionKey<<"outcome="<<outcome
           <<"action="<<actionState<<"outbox="<<outboxState;
    return true;
}

int MemoryManager::recoverStaleOutbox(int staleSeconds)
{
    if (!m_db.isOpen()) return 0;
    if (staleSeconds<0) staleSeconds=0;
    if (!m_db.transaction()) return 0;
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery stale(m_db);
    // 领取租约过期（且已过期 staleSeconds 以上）仍停在 dispatching：进程崩溃遗留，
    // 无法确认消息是否已经交给渠道 → at-most-once 冻结为 unknown，绝不重发。
    stale.prepare("SELECT o.id,a.action_key,a.kind FROM action_outbox o "
                  "JOIN cognitive_action a ON a.id=o.action_id "
                  "WHERE o.state='dispatching' AND coalesce(o.lease_until,0)<unixepoch()-?");
    stale.addBindValue(staleSeconds);
    if (!stale.exec()) return 0;
    struct StaleRow { qlonglong outboxId; QString actionKey; QString kind; };
    QList<StaleRow> pending;
    while (stale.next())
        pending.append(StaleRow{stale.value(0).toLongLong(),
                                stale.value(1).toString(),stale.value(2).toString()});
    stale.finish();
    int recovered=0;
    for (const auto &row : pending) {
        QSqlQuery freeze(m_db);
        freeze.prepare("UPDATE action_outbox SET state='unknown',completed_at=unixepoch(),"
                       "lease_owner=NULL,lease_until=NULL,last_error='dispatch_abandoned' "
                       "WHERE id=? AND state='dispatching'");
        freeze.addBindValue(row.outboxId);
        if (!freeze.exec() || freeze.numRowsAffected()!=1) continue;
        QSqlQuery action(m_db);
        action.prepare("UPDATE cognitive_action SET state='unknown',last_outcome='unknown',"
                       "updated_at=unixepoch() WHERE action_key=? AND state NOT IN "
                       "('handed_off','unknown','withheld')");
        action.addBindValue(row.actionKey);
        if (!action.exec()) return 0;
        // 领域行同步冻结（contact→unknown / reminder→unknown / share ledger→unknown）。
        recoverOutboxDomain(m_db,row.actionKey,row.kind);
        ++recovered;
    }
    if (!m_db.commit()) return 0;
    rollback.dismiss();
    if (recovered>0) qInfo()<<"[Outbox] stale dispatching frozen as unknown count="<<recovered;
    return recovered;
}

// ============================ R12-2a：ActionProposal 持久化（v32） ============================
// 这一片只做"提案"的内存/SQLite 层：
//   * 提案只记录准备做什么，不发送、不建 action_outbox、不写 cognitive_action；
//   * origin/target 从 ActionOrigin 派生（action_key 由 origin+kind 决定），调用方不能伪造；
//   * action_key UNIQUE 是幂等的最后防线；payload 一旦落库不会被同 key 覆盖成新版本；
//   * state 机：proposed → verified → committed（发送留给 R12-2b）；
//                       ↘ withheld / expired / unknown（终态）。
namespace {

const QString kProposalKindContact = QStringLiteral("contact");
const QString kProposalKindActivityShare = QStringLiteral("activity_share");

bool proposalStateValid(const QString &state)
{
    return state == QStringLiteral("proposed") || state == QStringLiteral("verified")
        || state == QStringLiteral("committed") || state == QStringLiteral("withheld")
        || state == QStringLiteral("expired") || state == QStringLiteral("unknown");
}

// committed 只能由 markProposalCommitted 写入（需要同时落 committed_action_key）。
bool proposalTransitionAllowed(const QString &from, const QString &to)
{
    if (from == QStringLiteral("proposed")) {
        return to == QStringLiteral("verified") || to == QStringLiteral("withheld")
            || to == QStringLiteral("expired") || to == QStringLiteral("unknown");
    }
    if (from == QStringLiteral("verified")) {
        return to == QStringLiteral("withheld") || to == QStringLiteral("expired")
            || to == QStringLiteral("unknown");
    }
    return false;
}

// semantic_status 是服务端核验结论的封闭枚举；空串表示"尚未产生结论"（NULL）。
bool proposalSemanticStatusValid(const QString &status)
{
    if (status.isEmpty()) return true;
    static const QSet<QString> allowed{
        QStringLiteral("unverified"), QStringLiteral("entailed"),
        QStringLiteral("entailed_coverage"), QStringLiteral("repair_needed"),
        QStringLiteral("contradicted"), QStringLiteral("insufficient"),
        QStringLiteral("withheld"), QStringLiteral("not_required"),
        QStringLiteral("unknown")};
    return allowed.contains(status);
}

const QString kProposalColumns = QStringLiteral(
    "id,action_key,kind,origin_kind,origin_ref,origin_version,run_id,scope,context_epoch,"
    "target_channel,target_session,payload_json,claims_json,semantic_status,state,expires_at,"
    "committed_action_key,error_code,verified_payload_hash,verified_claims_hash,"
    "verified_request_id,verified_at,history_projected,cancel_reason,action_context_epoch,"
    "created_at,updated_at");

// 行 → JSON：JSON 列解析回对象/数组；origin 同时给出结构化的 origin 对象与扁平字段。
QJsonObject proposalRowJson(const QSqlQuery &query)
{
    const QString originKind = query.value(QStringLiteral("origin_kind")).toString();
    const QString originRef = query.value(QStringLiteral("origin_ref")).toString();
    QJsonObject originJson{{QStringLiteral("kind"), originKind},
                           {QStringLiteral("ref"), originRef},
                           {QStringLiteral("version"),
                            query.value(QStringLiteral("origin_version")).toInt()},
                           {QStringLiteral("run_id"), query.value(QStringLiteral("run_id")).toString()},
                           {QStringLiteral("scope"), query.value(QStringLiteral("scope")).toString()},
                           {QStringLiteral("context_epoch"),
                            query.value(QStringLiteral("context_epoch")).toLongLong()},
                           {QStringLiteral("target_channel"),
                            query.value(QStringLiteral("target_channel")).toString()},
                           {QStringLiteral("target_session"),
                            query.value(QStringLiteral("target_session")).toString()}};
    ActionOrigin::Kind originKindEnum = ActionOrigin::Reflection;
    if (ActionOrigin::kindFromName(originKind, &originKindEnum)) {
        qlonglong originId = 0;
        if (ActionOrigin::idFromRef(originKindEnum, originRef, &originId))
            originJson.insert(QStringLiteral("id"), originId);
    }
    const QJsonObject payload = QJsonDocument::fromJson(
        query.value(QStringLiteral("payload_json")).toString().toUtf8()).object();
    const QJsonArray claims = QJsonDocument::fromJson(
        query.value(QStringLiteral("claims_json")).toString().toUtf8()).array();
    const QString channel = query.value(QStringLiteral("target_channel")).toString();
    const QString session = query.value(QStringLiteral("target_session")).toString();
    return QJsonObject{
        {QStringLiteral("id"), query.value(QStringLiteral("id")).toLongLong()},
        {QStringLiteral("action_key"), query.value(QStringLiteral("action_key")).toString()},
        {QStringLiteral("kind"), query.value(QStringLiteral("kind")).toString()},
        {QStringLiteral("origin"), originJson},
        {QStringLiteral("origin_kind"), originKind},
        {QStringLiteral("origin_ref"), originRef},
        {QStringLiteral("origin_version"),
         query.value(QStringLiteral("origin_version")).toInt()},
        {QStringLiteral("run_id"), query.value(QStringLiteral("run_id")).toString()},
        {QStringLiteral("scope"), query.value(QStringLiteral("scope")).toString()},
        {QStringLiteral("context_epoch"),
         query.value(QStringLiteral("context_epoch")).toLongLong()},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), channel},
                     {QStringLiteral("session_id"), session}}},
        {QStringLiteral("target_channel"), channel},
        {QStringLiteral("target_session"), session},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("claims"), claims},
        {QStringLiteral("semantic_status"),
         query.value(QStringLiteral("semantic_status")).toString()},
        {QStringLiteral("state"), query.value(QStringLiteral("state")).toString()},
        {QStringLiteral("expires_at"),
         query.value(QStringLiteral("expires_at")).toLongLong()},
        {QStringLiteral("committed_action_key"),
         query.value(QStringLiteral("committed_action_key")).toString()},
        {QStringLiteral("error_code"), query.value(QStringLiteral("error_code")).toString()},
        // R12-2c-2：verified 内容绑定 / 取消审计 / fence 快照（旧行缺省为 NULL/0）。
        {QStringLiteral("verified_payload_hash"),
         query.value(QStringLiteral("verified_payload_hash")).toString()},
        {QStringLiteral("verified_claims_hash"),
         query.value(QStringLiteral("verified_claims_hash")).toString()},
        {QStringLiteral("verified_request_id"),
         query.value(QStringLiteral("verified_request_id")).toString()},
        {QStringLiteral("verified_at"),
         query.value(QStringLiteral("verified_at")).toLongLong()},
        // 预留给 c3 回执历史投影的幂等标记；本片只建列并保持 0。
        {QStringLiteral("history_projected"),
         query.value(QStringLiteral("history_projected")).toInt()},
        {QStringLiteral("cancel_reason"),
         query.value(QStringLiteral("cancel_reason")).toString()},
        {QStringLiteral("action_context_epoch"),
         query.value(QStringLiteral("action_context_epoch")).toLongLong()},
        {QStringLiteral("created_at"),
         query.value(QStringLiteral("created_at")).toLongLong()},
        {QStringLiteral("updated_at"),
         query.value(QStringLiteral("updated_at")).toLongLong()}};
}

QString canonicalJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString canonicalJson(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace

QJsonObject MemoryManager::persistActionProposal(const QJsonObject &proposal, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return QJsonObject{{QStringLiteral("error"), reason}};
    };
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (proposal.isEmpty()) return fail(QStringLiteral("invalid_proposal"));
    // 1) 来源身份：严格解析，target 必须由服务端给出。
    ActionOrigin origin;
    QString originError;
    if (!ActionOrigin::fromJson(proposal.value(QStringLiteral("origin")).toObject(),
                                &origin, &originError))
        return fail(originError.isEmpty() ? QStringLiteral("invalid_origin") : originError);
    if (!origin.hasTarget()) return fail(QStringLiteral("invalid_target"));
    // 2) action_key 由 origin+kind 派生，调用方给出的必须一致（不能自造成新版本）。
    const QString kind = proposal.value(QStringLiteral("kind")).toString().trimmed();
    if (kind != kProposalKindContact && kind != kProposalKindActivityShare)
        return fail(QStringLiteral("invalid_kind"));
    const QString actionKey = origin.actionKey(kind);
    if (actionKey.isEmpty()) return fail(QStringLiteral("origin_kind_not_allowed"));
    const QString suppliedKey = proposal.value(QStringLiteral("action_key")).toString().trimmed();
    if (!suppliedKey.isEmpty() && suppliedKey != actionKey)
        return fail(QStringLiteral("action_key_mismatch"));
    // 3) target 从 origin 生成：调用方若给出必须与 origin 一致。
    const QJsonObject target = proposal.value(QStringLiteral("target")).toObject();
    if (!target.isEmpty()) {
        const QString channel = target.value(QStringLiteral("channel")).toString().trimmed();
        QString session = target.value(QStringLiteral("session_id")).toString().trimmed();
        if (session.isEmpty()) session = target.value(QStringLiteral("session")).toString().trimmed();
        if ((!channel.isEmpty() && channel != origin.targetChannel)
            || (!session.isEmpty() && session != origin.targetSession))
            return fail(QStringLiteral("target_mismatch"));
    }
    // 4) payload/claims：contact 必须有 message，share 必须有成果 draft（只记录提案，不发送）。
    const QJsonValue payloadValue = proposal.value(QStringLiteral("payload"));
    if (!payloadValue.isObject()) return fail(QStringLiteral("invalid_payload"));
    const QJsonObject payload = payloadValue.toObject();
    const QJsonValue claimsValue = proposal.value(QStringLiteral("claims"));
    if (!claimsValue.isUndefined() && !claimsValue.isArray())
        return fail(QStringLiteral("invalid_claims"));
    const QJsonArray claims = claimsValue.toArray();
    if (kind == kProposalKindContact
        && payload.value(QStringLiteral("message")).toString().trimmed().isEmpty())
        return fail(QStringLiteral("missing_message"));
    if (kind == kProposalKindActivityShare
        && payload.value(QStringLiteral("draft")).toString().trimmed().isEmpty())
        return fail(QStringLiteral("missing_artifact_draft"));
    // 5) semantic_status 只能服务端核验后写入，持久化入口不接受调用方伪造。
    if (!proposal.value(QStringLiteral("semantic_status")).toString().trimmed().isEmpty())
        return fail(QStringLiteral("semantic_status_forbidden"));
    const qint64 expiresAt = proposal.value(QStringLiteral("expires_at")).toInteger(0);
    if (expiresAt < 0) return fail(QStringLiteral("invalid_expires_at"));
    const QString payloadJson = canonicalJson(payload);
    const QString claimsJson = canonicalJson(claims);
    // 6) 幂等预检：同 key 已存在时，只有内容完全一致才返回既有行；否则 conflict。
    const auto compareExisting = [&](const QJsonObject &existing) -> bool {
        return existing.value(QStringLiteral("kind")).toString() == kind
            && existing.value(QStringLiteral("origin_kind")).toString() == origin.kindName()
            && existing.value(QStringLiteral("origin_ref")).toString() == origin.ref
            && existing.value(QStringLiteral("origin_version")).toInt() == origin.version
            && existing.value(QStringLiteral("run_id")).toString() == origin.runId
            && existing.value(QStringLiteral("scope")).toString() == origin.scope
            && existing.value(QStringLiteral("context_epoch")).toInteger() == origin.contextEpoch
            && existing.value(QStringLiteral("target_channel")).toString() == origin.targetChannel
            && existing.value(QStringLiteral("target_session")).toString() == origin.targetSession
            && canonicalJson(existing.value(QStringLiteral("payload")).toObject()) == payloadJson
            && canonicalJson(existing.value(QStringLiteral("claims")).toArray()) == claimsJson
            && existing.value(QStringLiteral("expires_at")).toInteger() == expiresAt;
    };
    const QJsonObject existing = actionProposal(actionKey);
    if (!existing.isEmpty()) {
        if (!compareExisting(existing)) {
            if (error) *error = QStringLiteral("conflict");
            QJsonObject conflict = existing;
            conflict.insert(QStringLiteral("error"), QStringLiteral("conflict"));
            return conflict;
        }
        QJsonObject idempotent = existing;
        idempotent.insert(QStringLiteral("idempotent"), true);
        return idempotent;
    }
    // 7) 单事务写入；唯一约束兜底并发下的重复 key。
    // R12-2c-2：落库时快照当前 Action Context Fence epoch。后续 verified 绑定、提交、
    // claimOutbox 与实际发送前都和这个快照比较；同 key 幂等重放不覆盖首次快照。
    const qint64 fenceEpoch = actionContextEpoch();
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO action_proposal(action_key,kind,origin_kind,origin_ref,origin_version,"
        "run_id,scope,context_epoch,target_channel,target_session,payload_json,claims_json,"
        "semantic_status,state,expires_at,action_context_epoch,created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,NULL,'proposed',?,?,unixepoch(),unixepoch())"));
    insert.addBindValue(actionKey);
    insert.addBindValue(kind);
    insert.addBindValue(origin.kindName());
    insert.addBindValue(origin.ref);
    insert.addBindValue(origin.version);
    insert.addBindValue(origin.runId.isEmpty() ? QVariant() : QVariant(origin.runId));
    insert.addBindValue(origin.scope);
    insert.addBindValue(origin.contextEpoch);
    insert.addBindValue(origin.targetChannel);
    insert.addBindValue(origin.targetSession);
    insert.addBindValue(payloadJson);
    insert.addBindValue(claimsJson);
    insert.addBindValue(expiresAt);
    insert.addBindValue(fenceEpoch);
    if (!insert.exec()) {
        m_db.rollback();
        rollback.dismiss();
        // 并发写入同 key：返回 conflict（不覆盖先落库的 payload）。
        const QJsonObject raced = actionProposal(actionKey);
        if (!raced.isEmpty()) {
            if (error) *error = QStringLiteral("conflict");
            QJsonObject conflict = raced;
            conflict.insert(QStringLiteral("error"), QStringLiteral("conflict"));
            return conflict;
        }
        return fail(QStringLiteral("db_error"));
    }
    if (insert.numRowsAffected() != 1) {
        m_db.rollback();
        return fail(QStringLiteral("db_error"));
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return fail(QStringLiteral("db_error"));
    }
    rollback.dismiss();
    const QJsonObject stored = actionProposal(actionKey);
    if (stored.isEmpty()) return fail(QStringLiteral("db_error"));
    qInfo() << "[Proposal] persisted key=" << actionKey << "kind=" << kind
            << "state=proposed";
    return stored;
}

QJsonObject MemoryManager::actionProposal(const QString &actionKey) const
{
    const QString trimmed = actionKey.trimmed();
    if (trimmed.isEmpty() || !m_db.isOpen()) return {};
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT ") + kProposalColumns
                  + QStringLiteral(" FROM action_proposal WHERE action_key=?"));
    query.addBindValue(trimmed);
    if (!query.exec() || !query.next()) return {};
    return proposalRowJson(query);
}

bool MemoryManager::updateActionProposalState(const QString &actionKey, const QString &fromState,
                                              const QString &toState,
                                              const QString &semanticStatus,
                                              const QString &errorCode, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return false;
    };
    const QString key = actionKey.trimmed();
    const QString from = fromState.trimmed();
    const QString to = toState.trimmed();
    const QString semantic = semanticStatus.trimmed();
    const QString code = errorCode.trimmed();
    if (key.isEmpty()) return fail(QStringLiteral("invalid_action_key"));
    if (!proposalStateValid(from) || !proposalStateValid(to))
        return fail(QStringLiteral("invalid_state"));
    // committed 必须走 markProposalCommitted（需要 committed_action_key）。
    if (to == QStringLiteral("committed")) return fail(QStringLiteral("invalid_state"));
    // R12-2c-2：verified 必须走 markActionProposalVerified（需要内容指纹/request_id/fence）。
    if (to == QStringLiteral("verified")) return fail(QStringLiteral("verified_binding_required"));
    if (!proposalTransitionAllowed(from, to)) return fail(QStringLiteral("invalid_transition"));
    if (!proposalSemanticStatusValid(semantic))
        return fail(QStringLiteral("invalid_semantic_status"));
    if (code.size() > 120) return fail(QStringLiteral("invalid_error_code"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE action_proposal SET state=?,semantic_status=?,error_code=?,updated_at=unixepoch() "
        "WHERE action_key=? AND state=?"));
    update.addBindValue(to);
    update.addBindValue(semantic.isEmpty() ? QVariant() : QVariant(semantic));
    update.addBindValue(code.isEmpty() ? QVariant() : QVariant(code));
    update.addBindValue(key);
    update.addBindValue(from);
    if (!update.exec()) {
        m_db.rollback();
        rollback.dismiss();
        return fail(QStringLiteral("db_error"));
    }
    if (update.numRowsAffected() != 1) {
        m_db.rollback();
        rollback.dismiss();
        const QJsonObject existing = actionProposal(key);
        return fail(existing.isEmpty() ? QStringLiteral("not_found")
                                       : QStringLiteral("cas_conflict"));
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return fail(QStringLiteral("db_error"));
    }
    rollback.dismiss();
    qInfo() << "[Proposal] state" << from << "->" << to << "key=" << key
            << "semantic=" << semantic;
    return true;
}

// R12-2c-final-1：proposed→verified 的服务端内容绑定入口。核验（语义网络请求）已经在
// LLMService 完成；这里只做服务端权威的"内容绑定落库"，不发网络请求、不发送消息。
// 关键不变式：verified 行必须携带"到底核验了哪份内容"的 binding（origin/target/两级
// epoch/payload/claims/证据集合/request_id）；binding 由构建 verifier 请求的一方生成，
// 事务内与权威提案行逐项比较后才 CAS——绝不把"当前 DB 内容"当成"verifier 核验过的内容"。
bool MemoryManager::markActionProposalVerified(const QString &actionKey,
                                               const VerificationBinding &binding,
                                               QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return false;
    };
    const QString key = actionKey.trimmed();
    if (key.isEmpty()) return fail(QStringLiteral("invalid_action_key"));
    if (binding.actionKey != key) return fail(QStringLiteral("verification_binding_mismatch"));
    QString bindingError;
    if (!binding.isValid(&bindingError))
        return fail(bindingError.isEmpty() ? QStringLiteral("invalid_verification_binding")
                                           : bindingError);
    // verified 终态只能来自服务端映射：withheld binding 不能提升提案。
    if (binding.state != QStringLiteral("verified"))
        return fail(QStringLiteral("verification_state_not_verified"));
    const QString semantic = binding.semanticStatus.trimmed();
    if (semantic.isEmpty() || semantic == QStringLiteral("withheld")
        || !proposalSemanticStatusValid(semantic))
        return fail(QStringLiteral("invalid_semantic_status"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const auto failRollback = [this, &rollback, &fail](const QString &reason) {
        m_db.rollback();
        rollback.dismiss();
        return fail(reason);
    };
    QJsonObject row;
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral("SELECT ") + kProposalColumns
                      + QStringLiteral(" FROM action_proposal WHERE action_key=?"));
        query.addBindValue(key);
        if (!query.exec() || !query.next()) return failRollback(QStringLiteral("not_found"));
        row = proposalRowJson(query);
    }
    const QString state = row.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("committed") || state == QStringLiteral("verified")) {
        // 幂等只对"同一份 binding"成立：已 verified/committed 的行若不是这份 binding 的
        // 产物（内容/request_id 不同），说明是另一份核验结果，绝不覆盖首次绑定。
        const bool sameBinding =
            row.value(QStringLiteral("verified_payload_hash")).toString() == binding.payloadDigest
            && row.value(QStringLiteral("verified_claims_hash")).toString() == binding.evidenceDigest
            && row.value(QStringLiteral("verified_request_id")).toString() == binding.requestId;
        m_db.rollback();
        rollback.dismiss();
        if (sameBinding) return true;
        return fail(QStringLiteral("already_verified_with_different_binding"));
    }
    if (state != QStringLiteral("proposed")) return failRollback(QStringLiteral("invalid_state"));
    // 1) binding 与权威提案行的身份字段必须完全一致（同 key 被替换/串线在这里拒绝）。
    const auto sameText = [&row](const char *field, const QString &expected) {
        return row.value(QString::fromLatin1(field)).toString() == expected;
    };
    const bool identityMatches =
        sameText("kind", binding.kind)
        && sameText("origin_kind", binding.originKind)
        && sameText("origin_ref", binding.originRef)
        && row.value(QStringLiteral("origin_version")).toInt() == binding.originVersion
        && sameText("run_id", binding.runId)
        && sameText("scope", binding.scope)
        && row.value(QStringLiteral("context_epoch")).toInteger() == binding.originContextEpoch
        && sameText("target_channel", binding.targetChannel)
        && sameText("target_session", binding.targetSession)
        && row.value(QStringLiteral("action_context_epoch")).toInteger()
               == binding.actionContextEpoch;
    if (!identityMatches) {
        qInfo().noquote()
            << QStringLiteral("[Proposal] verify_rejected key=%1 reason=binding_mismatch"
                              " request=%2")
                   .arg(key, binding.requestId.left(8));
        return failRollback(QStringLiteral("verification_binding_mismatch"));
    }
    // 2) 库内权威 payload/claims 的规范摘要必须等于核验时摘要。
    const QString kind = row.value(QStringLiteral("kind")).toString();
    const QJsonObject payload = row.value(QStringLiteral("payload")).toObject();
    const QJsonArray claims = row.value(QStringLiteral("claims")).toArray();
    if (ActionPolicy::canonicalProposalPayloadDigest(payload, claims) != binding.payloadDigest
        || ActionPolicy::canonicalProposalClaimsDigest(claims) != binding.claimsDigest) {
        qInfo().noquote()
            << QStringLiteral("[Proposal] verify_rejected key=%1 reason=payload_digest_mismatch"
                              " request=%2")
                   .arg(key, binding.requestId.left(8));
        return failRollback(QStringLiteral("verification_binding_mismatch"));
    }
    if (kind == QStringLiteral("contact")
        && payload.value(QStringLiteral("message")).toString().trimmed().isEmpty())
        return failRollback(QStringLiteral("missing_message"));
    if (kind == QStringLiteral("activity_share")
        && payload.value(QStringLiteral("draft")).toString().trimmed().isEmpty())
        return failRollback(QStringLiteral("missing_artifact_draft"));
    // 3) 证据集合摘要：用权威 claims 与当前库内证据（服务端唯一选择函数）重算，
    //    与 verifier 实际输入集合不一致说明"核验过的证据"已不成立 → fail closed。
    {
        ActionPolicy policy(this);
        QJsonArray sources;
        if (!policy.proposalEvidence(claims, &sources))
            return failRollback(QStringLiteral("verification_evidence_unavailable"));
        const QString evidenceDigest = ActionPolicy::canonicalEvidenceDigest(claims, sources);
        if (evidenceDigest != binding.evidenceDigest) {
            qInfo().noquote()
                << QStringLiteral("[Proposal] verify_rejected key=%1 reason=evidence_mismatch"
                                  " request=%2")
                       .arg(key, binding.requestId.left(8));
            return failRollback(QStringLiteral("verification_evidence_mismatch"));
        }
    }
    // 4) Action Context Fence：提案落库快照 = binding 快照 = 当前语境版本。
    //    主人私聊插话/成果撤回等已推进 epoch → 这份"旧在途核验"不能再变成 verified。
    const qint64 currentEpoch = actionContextEpoch();
    const qint64 snapshotEpoch = row.value(QStringLiteral("action_context_epoch")).toInteger();
    if (currentEpoch != snapshotEpoch || binding.actionContextEpoch != currentEpoch) {
        QSqlQuery expire(m_db);
        expire.prepare(QStringLiteral(
            "UPDATE action_proposal SET state='expired',cancel_reason=?,updated_at=unixepoch() "
            "WHERE action_key=? AND state='proposed'"));
        expire.addBindValue(QStringLiteral("context_epoch_changed"));
        expire.addBindValue(key);
        if (!expire.exec()) return failRollback(QStringLiteral("db_error"));
        if (!m_db.commit()) return failRollback(QStringLiteral("db_error"));
        rollback.dismiss();
        qInfo() << "[Proposal] expired key=" << key
                << "reason=context_epoch_changed snapshot=" << snapshotEpoch
                << "current=" << currentEpoch;
        return fail(QStringLiteral("context_epoch_changed"));
    }
    // 5) 不在这里判 expires_at：过期是提交层/Drain 的职责（历史行为保持不变：
    //    verified 但已过期的提案由 commit 落 expired，而不是拒绝写 verified 绑定）。
    // Store the digest of claims and their evidence in the existing verified_claims_hash.
    // Pre-existing claims-only digests fail closed for actions still awaiting delivery.
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE action_proposal SET state='verified',semantic_status=?,"
        "verified_payload_hash=?,verified_claims_hash=?,verified_request_id=?,"
        "verified_at=?,error_code=NULL,updated_at=unixepoch() "
        "WHERE action_key=? AND state='proposed'"));
    update.addBindValue(semantic);
    update.addBindValue(binding.payloadDigest);
    update.addBindValue(binding.evidenceDigest);
    update.addBindValue(binding.requestId);
    update.addBindValue(QDateTime::currentSecsSinceEpoch());
    update.addBindValue(key);
    if (!update.exec()) return failRollback(QStringLiteral("db_error"));
    if (update.numRowsAffected() != 1)
        return failRollback(QStringLiteral("cas_conflict"));
    if (!m_db.commit()) return failRollback(QStringLiteral("db_error"));
    rollback.dismiss();
    qInfo().noquote()
        << QStringLiteral("[Proposal] verified key=%1 semantic=%2 request=%3"
                          " payload=%4 evidence=%5")
               .arg(key, semantic, binding.requestId.left(8), binding.payloadDigest,
                    binding.evidenceDigest);
    return true;
}

// R12-2c-final-1：旧入口（兼容既有测试/管理路径，生产核验链不再调用）。
// 它从库内行推导一份 binding（内容指纹来自库内内容），所以**不能**证明 verifier 实际
// 核验的就是这份内容；真正的生产路径由 PrivateReview 传入 LLMService 生成的
// VerificationBinding 走上面的重载。
// R12-2c-2：取消审计（幂等）。未提交提案回 expired；已 committed 的保持状态（回执闭环依赖
// committed），只写 cancel_reason，供 claimOutbox/发送前复验识别"已取消、不得发送"。
bool MemoryManager::cancelActionProposal(const QString &actionKey, const QString &reason)
{
    const QString key = actionKey.trimmed();
    const QString why = reason.trimmed().left(120);
    if (!m_db.isOpen() || key.isEmpty() || why.isEmpty()) return false;
    if (!m_db.transaction()) return false;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE action_proposal SET "
        "state=CASE WHEN state IN ('proposed','verified') THEN 'expired' ELSE state END,"
        "cancel_reason=?,updated_at=unixepoch() WHERE action_key=?"));
    update.addBindValue(why);
    update.addBindValue(key);
    if (!update.exec()) {
        m_db.rollback();
        rollback.dismiss();
        return false;
    }
    if (update.numRowsAffected() != 1) {
        m_db.rollback();
        rollback.dismiss();
        return false;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return false;
    }
    rollback.dismiss();
    qInfo() << "[Proposal] cancelled key=" << key << "reason=" << why;
    return true;
}

int MemoryManager::expireActionProposals(qint64 now)
{
    if (!m_db.isOpen()) return 0;
    const qint64 when = now > 0 ? now : QDateTime::currentSecsSinceEpoch();
    if (!m_db.transaction()) return 0;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery update(m_db);
    // 只处理明确设置了过期时间且已到期的提案；expires_at=0 表示"不过期"。
    // 审计：cancel_reason 记录 expired（若还没有其它取消原因），便于 claim/发送前诊断。
    update.prepare(QStringLiteral(
        "UPDATE action_proposal SET state='expired',"
        "cancel_reason=coalesce(cancel_reason,'expired'),updated_at=unixepoch() "
        "WHERE state IN ('proposed','verified') AND expires_at>0 AND expires_at<=?"));
    update.addBindValue(when);
    if (!update.exec()) {
        m_db.rollback();
        rollback.dismiss();
        return 0;
    }
    const int expired = update.numRowsAffected();
    if (!m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    rollback.dismiss();
    if (expired > 0) qInfo() << "[Proposal] expired count=" << expired;
    return expired;
}

bool MemoryManager::markProposalCommitted(const QString &actionKey,
                                          const QString &committedActionKey, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return false;
    };
    const QString key = actionKey.trimmed();
    const QString committed = committedActionKey.trimmed();
    if (key.isEmpty()) return fail(QStringLiteral("invalid_action_key"));
    if (committed.isEmpty()) return fail(QStringLiteral("invalid_committed_action_key"));
    if (committed.size() > 200) return fail(QStringLiteral("invalid_committed_action_key"));
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE action_proposal SET state='committed',committed_action_key=?,error_code=NULL,"
        "updated_at=unixepoch() WHERE action_key=? AND state='verified'"));
    update.addBindValue(committed);
    update.addBindValue(key);
    if (!update.exec()) {
        m_db.rollback();
        rollback.dismiss();
        return fail(QStringLiteral("db_error"));
    }
    if (update.numRowsAffected() != 1) {
        m_db.rollback();
        rollback.dismiss();
        const QJsonObject existing = actionProposal(key);
        return fail(existing.isEmpty() ? QStringLiteral("not_found")
                                       : QStringLiteral("cas_conflict"));
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return fail(QStringLiteral("db_error"));
    }
    rollback.dismiss();
    qInfo() << "[Proposal] committed key=" << key
            << "committed_action_key=" << committed;
    return true;
}



// ============================ R12-2b：verified 提案提交（复用车/箱/回执） ============================
// 这一片把 R12-2a 的 verified ActionProposal 接到既有 cognitive_action + action_outbox +
// delivery_ledger 唯一提交路径上：仍然只入 Outbox（发送只由 ActionOutboxDispatcher 做），
// 不写 reflection_contact/private_reflection，不伪造 chat_history/reflection。
// 提交前再次用 ActionPolicy::validateActionProposal 复核来源/版本/runId/scope/epoch/target/
// 成果新鲜度/开关/权限/coverage，并复核安静时段、可达目标与共享 contact 每日额度。
namespace {

// 不可恢复（策略/来源/成果/重复）失败 → 提案落 withheld；可恢复（安静/额度/不可达/DB）失败
// → 保留 verified，交给下一次 drain。见 memorymanager.h 的错误码约定。
bool actionProposalHardReject(const QString &code)
{
    static const QSet<QString> hard{
        QStringLiteral("allow_contact_disabled"),
        QStringLiteral("stale_origin"),
        QStringLiteral("artifact_mismatch"),
        QStringLiteral("duplicate"),
        QStringLiteral("policy_rejected"),
        QStringLiteral("origin_kind_not_allowed"),
        QStringLiteral("invalid_kind"),
        QStringLiteral("not_verified"),
        QStringLiteral("invalid_state")};
    return hard.contains(code);
}

// ActionPolicy 的细粒度原因 → R12-2b 对外错误码（提交层只暴露稳定错误码）。
QString actionProposalErrorCode(const QString &reason)
{
    const QString trimmed = reason.trimmed();
    if (trimmed == QStringLiteral("open_wake_contact_disabled"))
        return QStringLiteral("allow_contact_disabled");
    if (trimmed.startsWith(QStringLiteral("origin_")))
        return QStringLiteral("stale_origin");
    if (trimmed.contains(QStringLiteral("artifact")))
        return QStringLiteral("artifact_mismatch");
    if (trimmed.contains(QStringLiteral("target")))
        return QStringLiteral("target_unreachable");
    return QStringLiteral("policy_rejected");
}

// 安静时段：与既有主动联系执行端口径一致（start==end 视为不设安静时段）。
bool actionQuietNow(const ProactiveConfig &options)
{
    const QTime now = QTime::currentTime();
    const QTime start = QTime::fromString(options.quietStart, QStringLiteral("HH:mm"));
    const QTime end = QTime::fromString(options.quietEnd, QStringLiteral("HH:mm"));
    if (!start.isValid() || !end.isValid() || start == end) return false;
    return start < end ? (now >= start && now < end) : (now >= start || now < end);
}

} // namespace

QJsonObject MemoryManager::commitVerifiedActionProposal(const QString &actionKey, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &code, const QString &detail = QString()) {
        if (error) *error = code;
        QJsonObject result{{QStringLiteral("error"), code}};
        if (!detail.isEmpty()) result[QStringLiteral("detail")] = detail;
        return result;
    };
    const QString key = actionKey.trimmed();
    if (!m_db.isOpen()) return fail(QStringLiteral("db_error"));
    if (key.isEmpty()) return fail(QStringLiteral("invalid_action_key"));
    const QJsonObject proposal = actionProposal(key);
    if (proposal.isEmpty()) return fail(QStringLiteral("not_found"));
    const QString kind = proposal.value(QStringLiteral("kind")).toString();
    const QString originKind = proposal.value(QStringLiteral("origin_kind")).toString();
    if (kind == QStringLiteral("contact")) {
        // 本片新来源只接 wake（reflection/concern 仍走 legacy commitReflectionContact 语义）。
        if (originKind != QStringLiteral("wake"))
            return fail(QStringLiteral("origin_kind_not_allowed"));
    } else if (kind == QStringLiteral("activity_share")) {
        if (originKind != QStringLiteral("thought") && originKind != QStringLiteral("activity"))
            return fail(QStringLiteral("origin_kind_not_allowed"));
    } else {
        return fail(QStringLiteral("invalid_kind"));
    }
    // 幂等：已经有动作行时不重复建 outbox；若提案停在 verified，补一次 committed 回写。
    // 同 key 不同 kind/来源版本/目标/正文/claims 视为 conflict/duplicate（不能静默复用
    // 另一条主动动作；R12-2c-2 起逐项核对，不再只比 origin_ref/version）。
    const QString expectedActionKind = kind == QStringLiteral("contact")
        ? kActionKindOpenWakeContact : kActionKindInternalShare;
    const QJsonObject existing = existingAction(m_db, key);
    if (!existing.isEmpty()) {
        QJsonObject existingPayload;
        {
            QSqlQuery row(m_db);
            row.prepare(QStringLiteral(
                "SELECT kind,payload_json FROM cognitive_action WHERE action_key=?"));
            row.addBindValue(key);
            if (row.exec() && row.next()) {
                if (row.value(0).toString() != expectedActionKind) {
                    if (error) *error = QStringLiteral("duplicate");
                    return QJsonObject{{QStringLiteral("error"), QStringLiteral("duplicate")}};
                }
                existingPayload = QJsonDocument::fromJson(
                    row.value(1).toString().toUtf8()).object();
            }
        }
        bool sameAction = !existingPayload.isEmpty()
            && existingPayload.value(QStringLiteral("origin_ref")).toString()
                   == proposal.value(QStringLiteral("origin_ref")).toString()
            && existingPayload.value(QStringLiteral("origin_version")).toInt()
                   == proposal.value(QStringLiteral("origin_version")).toInt()
            && existing.value(QStringLiteral("channel")).toString()
                   == proposal.value(QStringLiteral("target_channel")).toString()
            && existing.value(QStringLiteral("session")).toString()
                   == proposal.value(QStringLiteral("target_session")).toString();
        if (sameAction) {
            const QJsonObject proposalPayload = proposal.value(QStringLiteral("payload")).toObject();
            const QJsonArray proposalClaims = proposal.value(QStringLiteral("claims")).toArray();
            const QString proposalText = kind == QStringLiteral("contact")
                ? proposalPayload.value(QStringLiteral("message")).toString().trimmed()
                : proposalPayload.value(QStringLiteral("draft")).toString().trimmed();
            const QString actionText =
                !existingPayload.value(QStringLiteral("message")).toString().trimmed().isEmpty()
                ? existingPayload.value(QStringLiteral("message")).toString().trimmed()
                : existingPayload.value(QStringLiteral("draft")).toString().trimmed();
            sameAction = !proposalText.isEmpty() && actionText == proposalText
                && canonicalJson(existingPayload.value(QStringLiteral("claims")).toArray())
                       == canonicalJson(proposalClaims);
        }
        if (!sameAction) {
            if (error) *error = QStringLiteral("duplicate");
            return QJsonObject{{QStringLiteral("error"), QStringLiteral("duplicate")}};
        }
        if (proposal.value(QStringLiteral("state")).toString() == QStringLiteral("verified")) {
            QString markError;
            markProposalCommitted(key, key, &markError);
        }
        QJsonObject result = existing;
        result[QStringLiteral("proposal_state")] = QStringLiteral("committed");
        return result;
    }
    if (proposal.value(QStringLiteral("state")).toString() != QStringLiteral("verified")) {
        return fail(proposal.value(QStringLiteral("state")).toString() == QStringLiteral("committed")
                        ? QStringLiteral("invalid_state")
                        : QStringLiteral("not_verified"));
    }
    // R12-2c-2：Action Context Fence。提案落库后主人私聊插话/成果撤回已经推进 epoch，
    // 这份旧核验不能再提交（历史事实/成果保留，只是不发送旧动作）。
    if (proposal.value(QStringLiteral("action_context_epoch")).toInteger() != actionContextEpoch()) {
        cancelActionProposal(key, QStringLiteral("context_epoch_changed"));
        return fail(QStringLiteral("stale_origin"), QStringLiteral("context_epoch_changed"));
    }
    // R12-2c-2：verified 行必须带内容绑定；v32 迁移遗留的旧 verified 行没有指纹，
    // 一律 fail closed（withheld，不发送），等重新核验后产生新的 verified 行。
    const QString verifiedPayloadHash =
        proposal.value(QStringLiteral("verified_payload_hash")).toString();
    const QString verifiedClaimsHash =
        proposal.value(QStringLiteral("verified_claims_hash")).toString();
    if (verifiedPayloadHash.isEmpty() || verifiedClaimsHash.isEmpty()
        || proposal.value(QStringLiteral("verified_request_id")).toString().trimmed().isEmpty()) {
        updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                  QStringLiteral("withheld"),
                                  QStringLiteral("verified_binding_missing"));
        return fail(QStringLiteral("policy_rejected"),
                    QStringLiteral("verified_binding_missing"));
    }
    const qint64 expiresAt = proposal.value(QStringLiteral("expires_at")).toInteger();
    if (expiresAt > 0 && expiresAt <= QDateTime::currentSecsSinceEpoch()) {
        cancelActionProposal(key, QStringLiteral("expired"));
        return fail(QStringLiteral("stale_origin"), QStringLiteral("expired"));
    }
    auto &cfg = ConfigManager::instance();
    const ProactiveConfig opts = cfg.getProactiveConfig();
    if (!opts.enabled || !cfg.getMemoryPolicyConfig().longTermMemoryEnabled) {
        updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                  QStringLiteral("withheld"), QStringLiteral("proactive_disabled"));
        return fail(QStringLiteral("policy_rejected"), QStringLiteral("proactive_disabled"));
    }
    const QString targetChannel = proposal.value(QStringLiteral("target_channel")).toString();
    const QString targetSession = proposal.value(QStringLiteral("target_session")).toString();
    // 统一核验：来源真实身份（wake/成果）、版本/runId/scope/epoch、target 可见性、claims 用途、
    // 成果 hash/版本、coverage（claims=[] 也跑确定性 coverage）。
    ActionPolicy policy(this);
    const QJsonObject validationInput{
        {QStringLiteral("origin"), proposal.value(QStringLiteral("origin"))},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("action_key"), key},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), targetChannel},
                     {QStringLiteral("session_id"), targetSession}}},
        {QStringLiteral("payload"), proposal.value(QStringLiteral("payload"))},
        {QStringLiteral("claims"), proposal.value(QStringLiteral("claims"))},
        {QStringLiteral("expires_at"), expiresAt}};
    QString policyError;
    const QJsonObject validation = policy.validateActionProposal(validationInput, &policyError);
    if (!validation.value(QStringLiteral("allowed")).toBool()) {
        const QString reason = validation.value(QStringLiteral("reason")).toString(policyError);
        const QString code = actionProposalErrorCode(reason);
        if (actionProposalHardReject(code)) {
            updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                      QStringLiteral("withheld"), code);
        }
        return fail(code, reason);
    }
    // 目标可达性 / 权限（提交层粗门控；真实渠道可达性仍由建连状态与 dispatcher sender 决定）。
    if (!actionReminderAllowed(targetChannel, targetSession))
        return fail(QStringLiteral("target_unreachable"));
    // 安静时段：只影响 contact；轮询/恢复稍后再试，提案保持 verified 不丢。
    if (kind == QStringLiteral("contact") && actionQuietNow(opts))
        return fail(QStringLiteral("quiet"));
    // 共享 contact 每日额度：所有来源统一统计 delivery_ledger(kind=contact) 的当日非 withheld 行。
    if (kind == QStringLiteral("contact") && opts.contactDailyLimit > 0) {
        QSqlQuery used(m_db);
        if (used.exec(QStringLiteral(
                "SELECT count(*) FROM delivery_ledger WHERE kind='contact' "
                "AND date(created_at,'localtime')=date('now','localtime') "
                "AND state<>'withheld'"))
            && used.next() && used.value(0).toInt() >= opts.contactDailyLimit)
            return fail(QStringLiteral("limit"));
    }

    // 服务端组装最终 payload：只接受提案行的权威来源字段，模型原文元数据不进入动作行。
    const QJsonObject origin = proposal.value(QStringLiteral("origin")).toObject();
    const QJsonObject payloadIn = proposal.value(QStringLiteral("payload")).toObject();
    const QJsonArray claims = proposal.value(QStringLiteral("claims")).toArray();
    const QString semanticStatus = proposal.value(QStringLiteral("semantic_status")).toString();
    QJsonObject payload;
    QJsonObject outboxPayload;
    QString actionKind;
    QString ledgerKind;
    QString text;
    QString triggerRef;
    QString motivationType;
    QString motivationRef;
    QString contextRefsJson;
    qlonglong artifactId = 0;
    if (kind == QStringLiteral("contact")) {
        const QString message = payloadIn.value(QStringLiteral("message")).toString().trimmed();
        if (message.isEmpty())
            return fail(QStringLiteral("policy_rejected"), QStringLiteral("missing_message"));
        actionKind = kActionKindOpenWakeContact;
        ledgerKind = QStringLiteral("contact");
        text = message;
        const QString wakeRef = origin.value(QStringLiteral("ref")).toString();
        triggerRef = QStringLiteral("%1:%2").arg(wakeRef)
                         .arg(origin.value(QStringLiteral("version")).toInt());
        const QJsonObject motivation = payloadIn.value(QStringLiteral("motivation")).toObject();
        motivationType = motivation.value(QStringLiteral("type")).toString();
        motivationRef = motivation.value(QStringLiteral("ref")).toString();
        QJsonArray refs;
        for (const auto &value : claims)
            for (const auto &ref : value.toObject().value(QStringLiteral("refs")).toArray())
                refs.append(ref);
        if (refs.isEmpty() && !motivationRef.isEmpty()) refs.append(motivationRef);
        if (!refs.isEmpty())
            contextRefsJson = QString::fromUtf8(QJsonDocument(refs).toJson(QJsonDocument::Compact));
        payload = QJsonObject{
            {QStringLiteral("action_key"), key},
            {QStringLiteral("kind"), QStringLiteral("contact")},
            {QStringLiteral("origin_kind"), originKind},
            {QStringLiteral("origin_ref"), wakeRef},
            {QStringLiteral("origin_version"), origin.value(QStringLiteral("version"))},
            {QStringLiteral("run_id"), origin.value(QStringLiteral("run_id"))},
            {QStringLiteral("scope"), origin.value(QStringLiteral("scope"))},
            {QStringLiteral("context_epoch"), origin.value(QStringLiteral("context_epoch"))},
            {QStringLiteral("channel"), targetChannel},
            {QStringLiteral("session"), targetSession},
            {QStringLiteral("sentences"),
             QJsonArray{QJsonObject{{QStringLiteral("zh_text"), message}}}},
            {QStringLiteral("message"), message},
            {QStringLiteral("contact_draft"), message},
            {QStringLiteral("reason"), payloadIn.value(QStringLiteral("reason"))},
            {QStringLiteral("motivation"), motivation},
            {QStringLiteral("core_claim_ids"), payloadIn.value(QStringLiteral("core_claim_ids"))},
            {QStringLiteral("claims"), claims}};
        outboxPayload = QJsonObject{
            {QStringLiteral("sentences"),
             QJsonArray{QJsonObject{{QStringLiteral("zh_text"), message}}}},
            {QStringLiteral("channel"), targetChannel},
            {QStringLiteral("session"), targetSession},
            {QStringLiteral("message"), message}};
    } else {
        const QString draft = payloadIn.value(QStringLiteral("draft")).toString().trimmed();
        if (draft.isEmpty())
            return fail(QStringLiteral("policy_rejected"), QStringLiteral("missing_artifact_draft"));
        artifactId = origin.value(QStringLiteral("id")).toInteger();
        const QJsonObject view = activityByRef(origin.value(QStringLiteral("ref")).toString());
        if (view.isEmpty()) {
            updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                      QStringLiteral("withheld"),
                                      QStringLiteral("artifact_mismatch"));
            return fail(QStringLiteral("artifact_mismatch"), QStringLiteral("artifact_not_found"));
        }
        const QString artifactRef = view.value(QStringLiteral("artifact_ref")).toString().trimmed();
        int artifactVersion = view.value(QStringLiteral("artifact_version")).toInt(0);
        if (artifactVersion <= 0) artifactVersion = view.value(QStringLiteral("version")).toInt(0);
        const QString artifactHash = view.value(QStringLiteral("artifact_hash")).toString().trimmed();
        const QString storedDraft = view.value(QStringLiteral("draft")).toString().trimmed();
        if (view.value(QStringLiteral("status")).toString() != QStringLiteral("completed")
            || artifactRef.isEmpty()
            || artifactRef != view.value(QStringLiteral("ref")).toString()
            || storedDraft.isEmpty()
            || artifactVersion != origin.value(QStringLiteral("version")).toInt()) {
            updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                      QStringLiteral("withheld"),
                                      QStringLiteral("artifact_mismatch"));
            return fail(QStringLiteral("artifact_mismatch"), QStringLiteral("version_or_status"));
        }
        // 正文只能来自服务端已持久化成果；提交时按哈希再核对一次（防撤回/换稿）。
        if (!artifactHash.isEmpty() && activityContentHash(draft) != artifactHash) {
            updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                      QStringLiteral("withheld"),
                                      QStringLiteral("artifact_mismatch"));
            return fail(QStringLiteral("artifact_mismatch"), QStringLiteral("draft_hash"));
        }
        // internal 活动必须绑定同一份 share_intent（版本/哈希一致；撤回的不再发送）。
        if (originKind == QStringLiteral("activity")) {
            const QJsonObject intent = view.value(QStringLiteral("share_intent")).toObject();
            if (intent.isEmpty()) {
                updateActionProposalState(key, QStringLiteral("verified"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("artifact_mismatch"));
                return fail(QStringLiteral("artifact_mismatch"),
                            QStringLiteral("share_intent_missing"));
            }
            if (intent.value(QStringLiteral("state")).toString() == QStringLiteral("withdrawn")) {
                updateActionProposalState(key, QStringLiteral("verified"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("artifact_mismatch"));
                return fail(QStringLiteral("artifact_mismatch"), QStringLiteral("share_withdrawn"));
            }
            if (intent.value(QStringLiteral("artifact_version")).toInt() != artifactVersion) {
                updateActionProposalState(key, QStringLiteral("verified"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("artifact_mismatch"));
                return fail(QStringLiteral("artifact_mismatch"),
                            QStringLiteral("share_version_mismatch"));
            }
            const QString intentHash = intent.value(QStringLiteral("content_hash")).toString();
            if (!intentHash.isEmpty() && !artifactHash.isEmpty() && intentHash != artifactHash) {
                updateActionProposalState(key, QStringLiteral("verified"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("artifact_mismatch"));
                return fail(QStringLiteral("artifact_mismatch"), QStringLiteral("share_hash"));
            }
        }
        actionKind = kActionKindInternalShare;
        ledgerKind = QStringLiteral("share");
        text = draft;
        triggerRef = QStringLiteral("%1:%2").arg(origin.value(QStringLiteral("ref")).toString())
                         .arg(artifactVersion);
        payload = QJsonObject{
            {QStringLiteral("action_key"), key},
            {QStringLiteral("kind"), QStringLiteral("share")},
            {QStringLiteral("origin_kind"), originKind},
            {QStringLiteral("origin_ref"), origin.value(QStringLiteral("ref"))},
            {QStringLiteral("origin_version"), origin.value(QStringLiteral("version"))},
            {QStringLiteral("activity_ref"), view.value(QStringLiteral("ref"))},
            {QStringLiteral("artifact_ref"), artifactRef},
            {QStringLiteral("artifact_version"), artifactVersion},
            {QStringLiteral("artifact_hash"), artifactHash},
            {QStringLiteral("run_id"), origin.value(QStringLiteral("run_id"))},
            {QStringLiteral("scope"), origin.value(QStringLiteral("scope"))},
            {QStringLiteral("context_epoch"), origin.value(QStringLiteral("context_epoch"))},
            {QStringLiteral("channel"), targetChannel},
            {QStringLiteral("session"), targetSession},
            {QStringLiteral("sentences"),
             QJsonArray{QJsonObject{{QStringLiteral("zh_text"), draft}}}},
            {QStringLiteral("draft"), draft},
            {QStringLiteral("claims"), claims}};
        outboxPayload = QJsonObject{
            {QStringLiteral("sentences"),
             QJsonArray{QJsonObject{{QStringLiteral("zh_text"), draft}}}},
            {QStringLiteral("channel"), targetChannel},
            {QStringLiteral("session"), targetSession},
            {QStringLiteral("draft"), draft}};
    }

    // R12-2c-2/R12-2c-final-1：即将提交的 payload/claims 必须与"核验时的内容指纹"完全一致。
    // 两列分别保存 payloadDigest 和 evidenceDigest。内容在核验后
    // 被改写（含 claims 集合变化）就拒绝，绝不发送"核验的是 A、发送的是 B"。
    // 这里按提交内容重算；进入事务后再用权威提案行复核一次（双重复验）。
    const QString commitClaimsDigest = policy.proposalEvidenceDigest(claims);
    if (ActionPolicy::canonicalProposalPayloadDigest(payload, claims) != verifiedPayloadHash
        || commitClaimsDigest != verifiedClaimsHash) {
        updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                  QStringLiteral("withheld"),
                                  QStringLiteral("verified_content_mismatch"));
        return fail(QStringLiteral("policy_rejected"),
                    QStringLiteral("verified_content_mismatch"));
    }

    // 单事务：动作 + Outbox + 账本 + 提案 CAS 一起落库（失败整体回滚）。
    if (!m_db.transaction()) return fail(QStringLiteral("db_error"));
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    const auto rollbackThenFail = [this, &rollback, &fail](const QString &code,
                                                           const QString &detail) {
        m_db.rollback();
        rollback.dismiss();
        return fail(code, detail);
    };
    // 并发兜底：进入事务后再查一次动作行（唯一约束仍是最后防线）。
    const QJsonObject raced = existingAction(m_db, key);
    if (!raced.isEmpty()) {
        m_db.rollback();
        rollback.dismiss();
        QString markError;
        markProposalCommitted(key, key, &markError);
        QJsonObject result = raced;
        result[QStringLiteral("proposal_state")] = QStringLiteral("committed");
        return result;
    }
    // R12-2c-2：事务内重新读取权威提案行并逐项复验来源/版本/runId/scope/epoch/target/expires/
    // verified 绑定与即将提交的内容；任何不一致都不提交（窗口内被改写 → conflict/stale）。
    const QJsonObject fresh = actionProposal(key);
    if (fresh.isEmpty()) return rollbackThenFail(QStringLiteral("db_error"),
                                                 QStringLiteral("proposal_disappeared"));
    const auto sameField = [&fresh, &proposal](const char *field) {
        const QString name = QString::fromLatin1(field);
        return canonicalJson(QJsonObject{{QStringLiteral("v"), fresh.value(name)}})
            == canonicalJson(QJsonObject{{QStringLiteral("v"), proposal.value(name)}});
    };
    if (fresh.value(QStringLiteral("state")).toString() != QStringLiteral("verified")
        || fresh.value(QStringLiteral("kind")).toString() != kind
        || fresh.value(QStringLiteral("origin_kind")).toString() != originKind
        || !sameField("origin_ref") || !sameField("origin_version") || !sameField("run_id")
        || !sameField("scope") || !sameField("context_epoch")
        || fresh.value(QStringLiteral("target_channel")).toString() != targetChannel
        || fresh.value(QStringLiteral("target_session")).toString() != targetSession
        || fresh.value(QStringLiteral("expires_at")).toInteger() != expiresAt
        || fresh.value(QStringLiteral("verified_payload_hash")).toString() != verifiedPayloadHash
        || fresh.value(QStringLiteral("verified_claims_hash")).toString() != verifiedClaimsHash
        || fresh.value(QStringLiteral("verified_request_id")).toString()
               != proposal.value(QStringLiteral("verified_request_id")).toString()
        || canonicalJson(fresh.value(QStringLiteral("payload")).toObject())
               != canonicalJson(payloadIn)
        || canonicalJson(fresh.value(QStringLiteral("claims")).toArray())
               != canonicalJson(claims))
        return rollbackThenFail(QStringLiteral("cas_conflict"),
                                QStringLiteral("proposal_changed"));
    // fence/expires 在事务内再判一次（并发插话/过期不发送）。
    if (fresh.value(QStringLiteral("action_context_epoch")).toInteger() != actionContextEpoch()) {
        m_db.rollback();
        rollback.dismiss();
        cancelActionProposal(key, QStringLiteral("context_epoch_changed"));
        return fail(QStringLiteral("stale_origin"), QStringLiteral("context_epoch_changed"));
    }
    if (fresh.value(QStringLiteral("expires_at")).toInteger() > 0
        && fresh.value(QStringLiteral("expires_at")).toInteger()
               <= QDateTime::currentSecsSinceEpoch()) {
        m_db.rollback();
        rollback.dismiss();
        cancelActionProposal(key, QStringLiteral("expired"));
        return fail(QStringLiteral("stale_origin"), QStringLiteral("expired"));
    }
    if (ActionPolicy::canonicalProposalPayloadDigest(
            fresh.value(QStringLiteral("payload")).toObject(),
            fresh.value(QStringLiteral("claims")).toArray()) != verifiedPayloadHash
        || policy.proposalEvidenceDigest(
               fresh.value(QStringLiteral("claims")).toArray()) != verifiedClaimsHash) {
        m_db.rollback();
        rollback.dismiss();
        updateActionProposalState(key, QStringLiteral("verified"), QStringLiteral("withheld"),
                                  QStringLiteral("withheld"),
                                  QStringLiteral("verified_content_mismatch"));
        return fail(QStringLiteral("policy_rejected"),
                    QStringLiteral("verified_content_mismatch"));
    }
    // 事务内再做一次统一策略复核（来源版本/runId/scope/epoch/target/成果/权限/coverage），
    // 覆盖 phase1 读取之后、事务开始之前的窗口；确定性、无网络。
    {
        ActionPolicy txPolicy(this);
        const QString txTargetChannel =
            fresh.value(QStringLiteral("target_channel")).toString();
        const QString txTargetSession =
            fresh.value(QStringLiteral("target_session")).toString();
        const QJsonObject txInput{
            {QStringLiteral("origin"), fresh.value(QStringLiteral("origin"))},
            {QStringLiteral("kind"), fresh.value(QStringLiteral("kind"))},
            {QStringLiteral("action_key"), key},
            {QStringLiteral("target"),
             QJsonObject{{QStringLiteral("channel"), txTargetChannel},
                         {QStringLiteral("session_id"), txTargetSession}}},
            {QStringLiteral("payload"), fresh.value(QStringLiteral("payload"))},
            {QStringLiteral("claims"), fresh.value(QStringLiteral("claims"))},
            {QStringLiteral("expires_at"), fresh.value(QStringLiteral("expires_at"))}};
        QString txError;
        const QJsonObject txValidation = txPolicy.validateActionProposal(txInput, &txError);
        if (!txValidation.value(QStringLiteral("allowed")).toBool()) {
            const QString reason = txValidation.value(QStringLiteral("reason")).toString(txError);
            const QString code = actionProposalErrorCode(reason);
            m_db.rollback();
            rollback.dismiss();
            if (actionProposalHardReject(code)) {
                updateActionProposalState(key, QStringLiteral("verified"),
                                          QStringLiteral("withheld"),
                                          QStringLiteral("withheld"), code);
            }
            return fail(code, reason);
        }
    }
    // 共享 contact 每日额度：事务内统计（额度占用与 action/outbox 同一写事务），
    // 任何一步失败整体回滚；并发两个提交不会因"先读后写"而双占额度（SQLite 单写者）。
    if (kind == QStringLiteral("contact") && opts.contactDailyLimit > 0) {
        QSqlQuery usedNow(m_db);
        usedNow.prepare(QStringLiteral(
            "SELECT count(*) FROM delivery_ledger WHERE kind='contact' "
            "AND date(created_at,'localtime')=date('now','localtime') "
            "AND state<>'withheld'"));
        if (!usedNow.exec() || !usedNow.next())
            return rollbackThenFail(QStringLiteral("db_error"), QStringLiteral("quota_check"));
        if (usedNow.value(0).toInt() >= opts.contactDailyLimit)
            return rollbackThenFail(QStringLiteral("limit"),
                                    QStringLiteral("contact_daily_limit"));
    }
    const QString payloadJson = canonicalJson(payload);
    const QString outboxPayloadJson = canonicalJson(outboxPayload);
    qlonglong actionId = 0;
    if (!insertActionWithOutbox(m_db, key, actionKind, triggerRef, targetChannel, targetSession,
                                payloadJson,
                                QStringLiteral("proposal:%1").arg(semanticStatus),
                                &actionId, canonicalJson(claims), motivationType, motivationRef,
                                semanticStatus, contextRefsJson, outboxPayloadJson))
        return fail(QStringLiteral("db_error"), QStringLiteral("insert_failed"));
    {
        // 来源 runId/动机只做审计可观测（发送路由仍来自 target）。
        QSqlQuery touch(m_db);
        touch.prepare(QStringLiteral(
            "UPDATE cognitive_action SET run_id=?,motivation_type=?,motivation_ref=? WHERE id=?"));
        const QString runId = origin.value(QStringLiteral("run_id")).toString();
        touch.addBindValue(runId.isEmpty() ? QVariant() : QVariant(runId));
        touch.addBindValue(motivationType.isEmpty() ? QVariant() : QVariant(motivationType));
        touch.addBindValue(motivationRef.isEmpty() ? QVariant() : QVariant(motivationRef));
        touch.addBindValue(actionId);
        if (!touch.exec()) return fail(QStringLiteral("db_error"), QStringLiteral("action_touch"));
    }
    // 账本：kind=contact/share，ref=action_key（与 settle/回执/共享额度同一口径）。
    // R12-2c-2：必须检查影响行数为 1，失败整体回滚（不能出现"动作入 Outbox、额度没占"）。
    if (!actionNoteDeliveryChecked(m_db, ledgerKind, key, targetChannel, targetSession, text,
                                   QStringLiteral("claimed"), 0))
        return fail(QStringLiteral("db_error"), QStringLiteral("ledger_failed"));
    {
        QSqlQuery proposalUpdate(m_db);
        proposalUpdate.prepare(QStringLiteral(
            "UPDATE action_proposal SET state='committed',committed_action_key=?,error_code=NULL,"
            "updated_at=unixepoch() WHERE action_key=? AND state='verified'"));
        proposalUpdate.addBindValue(key);
        proposalUpdate.addBindValue(key);
        if (!proposalUpdate.exec() || proposalUpdate.numRowsAffected() != 1) {
            m_db.rollback();
            rollback.dismiss();
            return fail(QStringLiteral("db_error"), QStringLiteral("proposal_cas"));
        }
    }
    if (!m_db.commit()) return fail(QStringLiteral("db_error"), QStringLiteral("commit_failed"));
    rollback.dismiss();
    qInfo().noquote()
        << QStringLiteral("[Proposal] committed key=%1 kind=%2 channel=%3 session=%4"
                          " artifact=%5")
               .arg(key, actionKind, targetChannel, targetSession)
               .arg(artifactId > 0 ? QString::number(artifactId) : QStringLiteral("-"));
    QJsonObject result = actionCommittedResult(key, actionId, actionKind, targetChannel, targetSession);
    result[QStringLiteral("proposal_state")] = QStringLiteral("committed");
    result[QStringLiteral("kind")] = kind;
    return result;
}

QJsonObject MemoryManager::commitVerifiedOpenWakeContact(const QString &actionKey, QString *error)
{
    if (error) error->clear();
    const QJsonObject proposal = actionProposal(actionKey.trimmed());
    if (proposal.isEmpty()) {
        if (error) *error = QStringLiteral("not_found");
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("not_found")}};
    }
    if (proposal.value(QStringLiteral("kind")).toString() != QStringLiteral("contact")
        || proposal.value(QStringLiteral("origin_kind")).toString() != QStringLiteral("wake")) {
        if (error) *error = QStringLiteral("origin_kind_not_allowed");
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("origin_kind_not_allowed")}};
    }
    return commitVerifiedActionProposal(actionKey, error);
}

QJsonObject MemoryManager::commitVerifiedInternalActivityShare(const QString &actionKey,
                                                               QString *error)
{
    if (error) error->clear();
    const QJsonObject proposal = actionProposal(actionKey.trimmed());
    if (proposal.isEmpty()) {
        if (error) *error = QStringLiteral("not_found");
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("not_found")}};
    }
    if (proposal.value(QStringLiteral("kind")).toString() != QStringLiteral("activity_share")
        || proposal.value(QStringLiteral("origin_kind")).toString() != QStringLiteral("activity")) {
        if (error) *error = QStringLiteral("origin_kind_not_allowed");
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("origin_kind_not_allowed")}};
    }
    return commitVerifiedActionProposal(actionKey, error);
}

QJsonObject MemoryManager::drainVerifiedActionProposals(int limit)
{
    QJsonObject result{{QStringLiteral("scanned"), 0},
                       {QStringLiteral("committed"), 0},
                       {QStringLiteral("withheld"), 0},
                       {QStringLiteral("failed"), 0},
                       {QStringLiteral("results"), QJsonArray{}}};
    if (!m_db.isOpen() || limit <= 0) return result;
    // 顺手把已过期的提案落终态：过期的 verified 不应被反复尝试。
    expireActionProposals();
    QStringList keys;
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT action_key FROM action_proposal WHERE state='verified' ORDER BY id LIMIT ?"));
        query.addBindValue(qBound(1, limit, 32));
        if (!query.exec()) return result;
        while (query.next()) keys.append(query.value(0).toString());
    }
    QJsonArray results;
    int committed = 0, withheld = 0, failed = 0;
    for (const auto &key : keys) {
        QString commitError;
        const QJsonObject outcome = commitVerifiedActionProposal(key, &commitError);
        QJsonObject row{{QStringLiteral("action_key"), key}};
        if (outcome.contains(QStringLiteral("error"))) {
            row[QStringLiteral("error")] = commitError;
            const QJsonObject after = actionProposal(key);
            const QString state = after.value(QStringLiteral("state")).toString();
            row[QStringLiteral("state")] = state;
            if (state == QStringLiteral("withheld")) ++withheld;
            else ++failed;
        } else {
            ++committed;
            row[QStringLiteral("state")] = QStringLiteral("committed");
        }
        results.append(row);
    }
    result[QStringLiteral("scanned")] = keys.size();
    result[QStringLiteral("committed")] = committed;
    result[QStringLiteral("withheld")] = withheld;
    result[QStringLiteral("failed")] = failed;
    result[QStringLiteral("results")] = results;
    if (committed > 0 || withheld > 0 || failed > 0)
        qInfo() << "[Proposal] drain scanned=" << keys.size() << "committed=" << committed
                << "withheld=" << withheld << "failed=" << failed;
    return result;
}

QJsonObject MemoryManager::chooseProactiveTarget(const QString &draftLikeText,
                                                 bool requireShareable) const
{
    const ActionTarget target = actionChooseTarget(
        ConfigManager::instance().getProactiveConfig().targets, draftLikeText, requireShareable);
    return QJsonObject{{QStringLiteral("found"), target.found},
                       {QStringLiteral("channel"), target.channel},
                       {QStringLiteral("session"), target.session}};
}

namespace {

// R12-2c-3：回执历史投影的稳定口径（与 dispatcher 的 proactiveHistoryEligible 一致）。
bool receiptHistoryEligible(const QString &outcome, const QString &channel)
{
    if (outcome == QStringLiteral("protocol_accepted")) return true;
    // pet 没有协议层回执，handed_off 表示气泡已显示；其它渠道的 handed_off 只是入队。
    if (outcome == QStringLiteral("handed_off") && channel == Channel::kPet) return true;
    // unknown 只是"可能已送达但无法确认"，不能写成"茉子说过"；rejected/withheld 明确未发。
    return false;
}

// 回执历史的正文只来自服务端落库的动作 payload（不依赖 Qt/dispatcher 内存 payload）。
QString receiptHistoryText(const QJsonObject &payload)
{
    QString text = payload.value(QStringLiteral("message")).toString().trimmed();
    if (text.isEmpty()) text = payload.value(QStringLiteral("draft")).toString().trimmed();
    if (text.isEmpty()) {
        for (const auto &value : payload.value(QStringLiteral("sentences")).toArray()) {
            const QString line = value.toObject().value(QStringLiteral("zh_text")).toString().trimmed();
            if (!line.isEmpty()) {
                text = line;
                break;
            }
        }
    }
    return text;
}

// 新来源历史回认引用（与旧约定一致）：wake/<id>:<version>、activity/<id>:<artifact_version>。
QString receiptHistoryRef(const QString &actionKey)
{
    return actionKey.section(QLatin1Char(':'), 0, 2);
}

} // namespace

bool MemoryManager::recordActionReceipt(const QString &actionKey, const QString &outcome,
                                        const QString &detail)
{
    Q_UNUSED(detail);
    // 只处理 R12-2b 新来源；legacy reflection:/reminder:/thought: 回执仍由既有
    // settleOutboxAction 的领域回写处理（历史仍由 actionSettled 的旧路径写），避免双重投影。
    static const QSet<QString> outcomes{
        QStringLiteral("protocol_accepted"), QStringLiteral("handed_off"),
        QStringLiteral("rejected"), QStringLiteral("unknown"), QStringLiteral("withheld")};
    const QString key = actionKey.trimmed();
    if (!m_db.isOpen() || key.isEmpty() || !outcomes.contains(outcome)) return false;
    const bool wakeContact = key.startsWith(QStringLiteral("wake:"));
    const bool internalShare = key.startsWith(QStringLiteral("activity:"));
    if (!wakeContact && !internalShare) return true;
    const QString ledgerKind = wakeContact ? QStringLiteral("contact") : QStringLiteral("share");
    const QString expectedKind = wakeContact ? kActionKindOpenWakeContact
                                             : kActionKindInternalShare;

    if (!m_db.transaction()) return false;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    // 审计只需回滚（本事务尚未写库）并记录稳定错误码，不写 ledger/历史。
    const auto audit = [&](const QString &reason) {
        m_db.rollback();
        rollback.dismiss();
        qWarning().noquote()
            << QStringLiteral("[Receipt] audit key=%1 outcome=%2 reason=%3")
                   .arg(key, outcome, reason);
        return false;
    };
    const auto ignoreReceipt = [&](const QString &reason) {
        m_db.rollback();
        rollback.dismiss();
        qInfo().noquote()
            << QStringLiteral("[Receipt] ignored key=%1 outcome=%2 reason=%3")
                   .arg(key, outcome, reason);
        return true;
    };

    // 1) 动作行/Outbox/提案都从 DB 读；不存在/未提交/路由不匹配都不写任何账本或历史。
    struct ReceiptRow
    {
        qlonglong actionId = 0;
        QString kind;
        QString actionState;
        QString lastOutcome;
        QString channel;
        QString session;
        QString payloadJson;
        QString outboxState;
        QString outboxChannel;
        QString outboxSession;
        QString proposalState;
        QString committedActionKey;
        int historyProjected = 0;
    };
    ReceiptRow row;
    {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT a.id,a.kind,a.state,coalesce(a.last_outcome,''),a.channel,a.session_id,"
            "a.payload_json,o.state,o.channel,o.session_id,"
            "coalesce(p.state,''),coalesce(p.committed_action_key,''),"
            "coalesce(p.history_projected,0) "
            "FROM cognitive_action a JOIN action_outbox o ON o.action_id=a.id "
            "LEFT JOIN action_proposal p ON p.action_key=a.action_key "
            "WHERE a.action_key=?"));
        query.addBindValue(key);
        if (!query.exec()) return false;
        if (!query.next()) return audit(QStringLiteral("action_missing"));
        row.actionId = query.value(0).toLongLong();
        row.kind = query.value(1).toString();
        row.actionState = query.value(2).toString();
        row.lastOutcome = query.value(3).toString();
        row.channel = query.value(4).toString();
        row.session = query.value(5).toString();
        row.payloadJson = query.value(6).toString();
        row.outboxState = query.value(7).toString();
        row.outboxChannel = query.value(8).toString();
        row.outboxSession = query.value(9).toString();
        row.proposalState = query.value(10).toString();
        row.committedActionKey = query.value(11).toString();
        row.historyProjected = query.value(12).toInt();
    }
    // 渠道/身份匹配：action kind 必须与 key 前缀一致，Outbox 路由必须与动作行完全一致。
    if (row.kind != expectedKind) return audit(QStringLiteral("kind_mismatch"));
    if (row.outboxChannel != row.channel || row.outboxSession != row.session
        || row.channel.trimmed().isEmpty() || row.session.trimmed().isEmpty())
        return audit(QStringLiteral("channel_mismatch"));
    // 未提交动作（提案缺失/不是 committed）不写账本/历史；重复回执也走同一校验。
    if (row.proposalState != QStringLiteral("committed")
        || row.committedActionKey != key)
        return audit(QStringLiteral("proposal_not_committed"));
    // 账本已有渠道与动作不一致说明串线/篡改：只审计，不改写任何一方。
    {
        QSqlQuery ledger(m_db);
        ledger.prepare(QStringLiteral(
            "SELECT channel FROM delivery_ledger WHERE kind=? AND ref=?"));
        ledger.addBindValue(ledgerKind);
        ledger.addBindValue(key);
        if (ledger.exec() && ledger.next()) {
            const QString ledgerChannel = ledger.value(0).toString();
            if (!ledgerChannel.isEmpty() && ledgerChannel != row.channel)
                return audit(QStringLiteral("ledger_channel_mismatch"));
        }
    }
    // 2) 权威回执以 settle 落库的 action.last_outcome 为准；调用方 outcome 只用于一致性观测。
    if (row.lastOutcome.isEmpty()) return audit(QStringLiteral("receipt_not_settled"));
    const QString dbOutcome = row.lastOutcome;
    const bool dbSuccess = dbOutcome == QStringLiteral("handed_off")
        || dbOutcome == QStringLiteral("protocol_accepted");
    const bool expectedSuccess = outcome == QStringLiteral("handed_off")
        || outcome == QStringLiteral("protocol_accepted");
    if (dbSuccess != expectedSuccess) {
        qInfo().noquote()
            << QStringLiteral("[Receipt] outcome_mismatch key=%1 caller=%2 db=%3")
                   .arg(key, outcome, dbOutcome);
    }
    // unknown/withheld/rejected：绝不写成功历史，也不把已投影的回执回退成未投影。
    if (!dbSuccess) {
        if (!m_db.commit()) return false;
        rollback.dismiss();
        qInfo().noquote()
            << QStringLiteral("[Receipt] key=%1 outcome=%2 history=not_eligible")
                   .arg(key, dbOutcome);
        return true;
    }
    // 成功回执必须已收敛到 handed_off（协议接受/本地显示），否则状态不可接受。
    if (row.actionState != QStringLiteral("handed_off"))
        return ignoreReceipt(QStringLiteral("state_not_acceptable"));

    const QJsonObject payload =
        QJsonDocument::fromJson(row.payloadJson.toUtf8()).object();
    const QString historyKind = wakeContact ? QStringLiteral("contact") : QStringLiteral("share");
    const QString historyRef = receiptHistoryRef(key);
    qlonglong historyTurn = 0;
    bool projectedNow = false;

    // 3) 领域行幂等补齐（settle 已写；对账/旧行补 c3 的成果绑定与最新 wake 保护）。
    if (wakeContact) {
        const auto parts = key.split(QLatin1Char(':'));
        if (parts.size() >= 3) {
            const qlonglong wakeId = parts[1].toLongLong();
            QSqlQuery scope(m_db);
            scope.prepare(QStringLiteral("SELECT scope_key FROM open_wake WHERE id=?"));
            scope.addBindValue(wakeId);
            if (scope.exec() && scope.next()) {
                // 只允许最新一次 wake 的回执改写 last_outcome；旧 wake 的迟到回执不覆盖新状态。
                QSqlQuery stateUpdate(m_db);
                stateUpdate.prepare(QStringLiteral(
                    "UPDATE open_wake_state SET last_outcome=?,updated_at=unixepoch() "
                    "WHERE scope_key=? AND last_wake_id=?"));
                stateUpdate.addBindValue(QStringLiteral("contact_%1").arg(dbOutcome));
                stateUpdate.addBindValue(scope.value(0).toString());
                stateUpdate.addBindValue(wakeId);
                if (!stateUpdate.exec()) return false;
            }
        }
    } else {
        const auto parts = key.split(QLatin1Char(':'));
        if (parts.size() >= 3) {
            const qlonglong activityId = parts[1].toLongLong();
            const int shareVersion = parts[2].toInt();
            // 回执绑定原成果：artifact_ref/version/hash 来自动作 payload；只改仍指向同一成果版本的
            // 当前 share_intent，旧版本迟到回执不会覆盖新版本分享结果。
            QJsonObject patch{
                {QStringLiteral("share_outcome"), dbOutcome},
                {QStringLiteral("share_version"), shareVersion},
                {QStringLiteral("share_channel"), row.channel},
                {QStringLiteral("share_at"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
            const QString artifactRef = payload.value(QStringLiteral("artifact_ref")).toString();
            const int artifactVersion = payload.value(QStringLiteral("artifact_version")).toInt();
            const QString artifactHash = payload.value(QStringLiteral("artifact_hash")).toString();
            if (!artifactRef.isEmpty()) patch.insert(QStringLiteral("share_artifact_ref"), artifactRef);
            if (artifactVersion > 0) patch.insert(QStringLiteral("share_artifact_version"), artifactVersion);
            if (!artifactHash.isEmpty()) patch.insert(QStringLiteral("share_artifact_hash"), artifactHash);
            QSqlQuery patchQuery(m_db);
            patchQuery.prepare(QStringLiteral(
                "UPDATE internal_activity SET result_json=json_patch(coalesce(result_json,'{}'),?),"
                "updated_at=unixepoch() "
                "WHERE id=? AND status='completed' "
                "AND json_extract(result_json,'$.share_intent.artifact_version')=?"));
            patchQuery.addBindValue(QString::fromUtf8(
                QJsonDocument(patch).toJson(QJsonDocument::Compact)));
            patchQuery.addBindValue(activityId);
            patchQuery.addBindValue(shareVersion);
            if (!patchQuery.exec()) return false;
        }
    }

    // 4) 历史投影：以 history_projected 做持久化幂等标记，与回执同事务。
    //    成功且可投影 → 写主动历史 + 置 1；不可投影（网络 handed_off 等）→ 置 2 审计；
    //    写历史失败整体回滚（receipt 未落库前不会出现"回执已写、历史缺失"的不可恢复窗口）。
    const bool eligibleNow = receiptHistoryEligible(dbOutcome, row.channel);
    if (row.historyProjected == 0 || (row.historyProjected == 2 && eligibleNow)) {
        if (eligibleNow) {
            const QString text = receiptHistoryText(payload);
            if (!text.isEmpty()) {
                bool created = false;
                historyTurn = writeProactiveHistoryLocked(row.channel, row.session, text,
                                                          historyKind, historyRef, &created);
                if (historyTurn <= 0) return false;
                projectedNow = true;
                Q_UNUSED(created);
            }
        }
        QSqlQuery mark(m_db);
        mark.prepare(QStringLiteral(
            "UPDATE action_proposal SET history_projected=?,updated_at=unixepoch() "
            "WHERE action_key=? AND history_projected IN (0,2)"));
        mark.addBindValue(projectedNow ? 1 : 2);
        mark.addBindValue(key);
        if (!mark.exec()) return false;
    }
    if (!m_db.commit()) return false;
    rollback.dismiss();
    if (historyTurn > 0) {
        if (!indexChatTurn(historyTurn))
            qWarning() << "[MemoryIndex] receipt proactive index failed:" << historyTurn;
        else
            emit searchChunksChanged();
    }
    qInfo().noquote()
        << QStringLiteral("[Receipt] key=%1 outcome=%2 history=%3 history_projected=%4")
               .arg(key, dbOutcome,
                    projectedNow ? QStringLiteral("projected")
                                 : (row.historyProjected == 0
                                        ? QStringLiteral("not_eligible")
                                        : QStringLiteral("already_projected")))
               .arg(projectedNow ? 1 : row.historyProjected);
    return true;
}

int MemoryManager::reconcileActionReceipts()
{
    if (!m_db.isOpen()) return 0;
    // R12-2c-3：按"状态标记"扫描所有已成功回执但历史未投影的新来源动作。投影后
    // history_projected 变 1/2，因此不会固定卡在最早 64 条；这里再做有界多批，
    // 单次对账最多推进 8*64 条，重启/定时可继续补齐。已投影行永不回退。
    int processed = 0;
    for (int batch = 0; batch < 8; ++batch) {
        QStringList keys;
        {
            QSqlQuery query(m_db);
            query.prepare(QStringLiteral(
                "SELECT a.action_key,coalesce(a.last_outcome,'') "
                "FROM action_proposal p JOIN cognitive_action a "
                "ON a.action_key=p.committed_action_key "
                "WHERE p.state='committed' AND p.history_projected=0 "
                "AND a.state='handed_off' "
                "AND (a.last_outcome='protocol_accepted' "
                "  OR (a.last_outcome='handed_off' AND a.channel='pet')) "
                "ORDER BY a.id LIMIT 64"));
            if (!query.exec()) return processed;
            while (query.next()) {
                const QString key = query.value(0).toString();
                const QString outcome = query.value(1).toString();
                if (!key.isEmpty()) keys.append(key + QLatin1Char('\n') + outcome);
            }
        }
        if (keys.isEmpty()) break;
        int batchProcessed = 0;
        for (const QString &packed : keys) {
            const int split = packed.indexOf(QLatin1Char('\n'));
            const QString key = split > 0 ? packed.left(split) : packed;
            const QString outcome = split > 0 ? packed.mid(split + 1) : QString{};
            if (recordActionReceipt(key, outcome)) {
                ++processed;
                ++batchProcessed;
            } else {
                qWarning() << "[Receipt] reconcile audit key=" << key;
            }
        }
        if (batchProcessed == 0) break;
    }
    // A receipt transaction can commit just before the process exits without indexing the turn.
    // Recreate the missing assistant chunk without adding any new history.
    QList<qint64> missingChunks;
    {
        QSqlQuery query(m_db);
        if (query.exec("SELECT h.id FROM chat_history h WHERE h.turn_kind='proactive' "
                       "AND h.sensitive=0 AND h.turn_state='handed_off' "
                       "AND NOT EXISTS (SELECT 1 FROM memory_chunks c WHERE c.source_type='chat' "
                       "AND c.source_ref='chat/'||h.id||':assistant' AND c.status='active') "
                       "ORDER BY h.id DESC LIMIT 64")) {
            while (query.next()) missingChunks.append(query.value(0).toLongLong());
        }
    }
    int reindexed = 0;
    for (const auto turn : missingChunks)
        if (indexChatTurn(turn)) ++reindexed;
    if (reindexed > 0) {
        qInfo() << "[Receipt] proactive_chunks_recovered=" << reindexed;
        emit searchChunksChanged();
    }
    if (processed > 0) qInfo() << "[Receipt] reconciled=" << processed;
    return processed;
}

// ============================ R12-2c-3：internal activity share 崩溃恢复 ============================
// 只恢复"已完成活动 + 仍有效的 saved 分享意愿 + 成果 ref/版本/哈希仍匹配 + 未撤回 +
// 尚无同成果版本提案 + 尚无成功/未知分享回执"的意图；绝不把所有历史成果自动加入分享队列。
// 领取代价是 share_intent.recovery_state 标记（无新 schema），非 force 时按 retryAfterSeconds
// 节流，避免每个 tick 重复发起 verifier；重启 force=true 时立即重试（在途核验已随进程消失）。
QJsonArray MemoryManager::claimPendingActivityShareIntents(int limit, bool force,
                                                           int retryAfterSeconds)
{
    QJsonArray result;
    if (!m_db.isOpen() || limit <= 0) return result;
    const int capped = qBound(1, limit, 8);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const int retryAfter = qBound(60, retryAfterSeconds, 3600);
    if (!m_db.transaction()) return result;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    struct Candidate
    {
        qlonglong id = 0;
        int version = 0;
        QString channel;
        QString session;
        QString resultJson;
    };
    QList<Candidate> rows;
    // R12-2c-3：有界游标扫描。cursor 持久化在 attention_state（无新 schema），每次从上次
    // 位置之后取 64 行；不足 64 行说明扫到尾部，下一轮从头开始。避免大量历史 completed
    // 活动把新产生的分享意愿永久挡在首批之外。
    const QString cursorKey = QStringLiteral("activity_share_recovery_cursor");
    qlonglong cursor = attentionState(cursorKey).toLongLong();
    const auto loadRows = [this, &rows](qlonglong afterId) -> bool {
        rows.clear();
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "SELECT id,version,channel,session_id,result_json FROM internal_activity "
            "WHERE scope_key='owner' AND status='completed' AND subject='user' "
            "AND channel IN ('pet','qq_private') AND id>? ORDER BY id LIMIT 64"));
        query.addBindValue(afterId);
        if (!query.exec()) return false;
        while (query.next()) {
            rows.append(Candidate{query.value(0).toLongLong(), query.value(1).toInt(),
                                  query.value(2).toString(), query.value(3).toString(),
                                  query.value(4).toString()});
        }
        return true;
    };
    if (!loadRows(cursor)) return result;
    if (rows.isEmpty() && cursor > 0) {
        cursor = 0;
        if (!loadRows(cursor)) return result;
    }
    // 只改 result_json.share_intent 的恢复标记，不改活动 status/version；失败保留原意图。
    const auto markIntent = [this](qlonglong id, const QString &state, const QString &reason,
                                   qint64 at) -> bool {
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral(
            "UPDATE internal_activity SET result_json=json_patch(coalesce(result_json,'{}'),?),"
            "updated_at=unixepoch() WHERE id=? AND status='completed'"));
        update.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("share_intent"),
             QJsonObject{{QStringLiteral("recovery_state"), state},
                         {QStringLiteral("recovery_reason"), reason},
                         {QStringLiteral("recovery_at"), at}}}})
            .toJson(QJsonDocument::Compact)));
        update.addBindValue(id);
        return update.exec();
    };
    qlonglong lastScannedId = cursor;
    for (const auto &candidate : rows) {
        if (result.size() >= capped) break;
        lastScannedId = candidate.id;
        const QJsonObject stored =
            QJsonDocument::fromJson(candidate.resultJson.toUtf8()).object();
        const QJsonObject intent = stored.value(QStringLiteral("share_intent")).toObject();
        if (intent.isEmpty()) continue;
        // 只处理仍为 saved 的意图；withdrawn / 其它历史状态不自动恢复。
        if (intent.value(QStringLiteral("state")).toString() != QStringLiteral("saved"))
            continue;
        const QString recoveryState = intent.value(QStringLiteral("recovery_state")).toString();
        if (recoveryState == QStringLiteral("withheld")) continue;
        const QString ref = QStringLiteral("activity/%1").arg(candidate.id);
        const int intentVersion = intent.value(QStringLiteral("artifact_version")).toInt();
        const QString intentHash = intent.value(QStringLiteral("content_hash")).toString();
        const int rowArtifactVersion = stored.value(QStringLiteral("artifact_version")).toInt();
        const QString rowArtifactHash = stored.value(QStringLiteral("artifact_hash")).toString();
        const QString rowArtifactRef = stored.value(QStringLiteral("artifact_ref")).toString();
        const QString draft = stored.value(QStringLiteral("draft")).toString().trimmed();
        const QString shareOutcome = stored.value(QStringLiteral("share_outcome")).toString();
        const auto withhold = [&](const QString &reason) {
            if (!markIntent(candidate.id, QStringLiteral("withheld"), reason, now)) return false;
            qInfo().noquote()
                << QStringLiteral("[Activity] share_intent_invalidated ref=%1 artifact_version=%2"
                                  " reason=%3")
                       .arg(ref)
                       .arg(intentVersion)
                       .arg(reason);
            return true;
        };
        // 成果已修改/撤回/缺失：旧意图失效（审计），不发送旧成果、不改变完成状态。
        if (intentVersion <= 0) {
            if (!withhold(QStringLiteral("missing_artifact_version"))) return QJsonArray{};
            continue;
        }
        if (rowArtifactRef != ref || rowArtifactVersion != intentVersion) {
            if (!withhold(QStringLiteral("artifact_version_changed"))) return QJsonArray{};
            continue;
        }
        if (draft.isEmpty()) {
            if (!withhold(QStringLiteral("missing_artifact"))) return QJsonArray{};
            continue;
        }
        if (intentHash.isEmpty() || (!rowArtifactHash.isEmpty() && rowArtifactHash != intentHash)
            || activityContentHash(draft) != intentHash) {
            if (!withhold(QStringLiteral("artifact_content_mismatch"))) return QJsonArray{};
            continue;
        }
        // 已有成功/未知/被撤销的分享回执：不自动重发（at-most-once / 已交付）。
        if (!shareOutcome.isEmpty()) continue;
        // A persisted requested marker reserves the one verifier call for this artifact version.
        // After a restart or an expired request, a missing result is ambiguous. Preserve the
        // activity and audit the withheld share instead of issuing a second verifier call.
        const qint64 recoveryAt = intent.value(QStringLiteral("recovery_at")).toInteger();
        QString proposalState;
        {
            QSqlQuery existing(m_db);
            existing.prepare(QStringLiteral(
                "SELECT state FROM action_proposal WHERE kind='activity_share' "
                "AND origin_kind='activity' AND origin_ref=? AND origin_version=? "
                "ORDER BY id LIMIT 1"));
            existing.addBindValue(ref);
            existing.addBindValue(intentVersion);
            if (!existing.exec()) return QJsonArray{};
            if (existing.next()) proposalState = existing.value(0).toString();
        }
        if (!proposalState.isEmpty() && proposalState != QStringLiteral("proposed")) continue;
        if (recoveryState == QStringLiteral("requested") || !proposalState.isEmpty()) {
            if (!force && recoveryAt > 0 && now - recoveryAt < retryAfter) continue;
            if (!withhold(QStringLiteral("verification_outcome_unknown"))) return QJsonArray{};
            if (!proposalState.isEmpty()) {
                QSqlQuery cancel(m_db);
                cancel.prepare("UPDATE action_proposal SET state='withheld',semantic_status='withheld',"
                               "error_code='verification_outcome_unknown',updated_at=unixepoch() "
                               "WHERE kind='activity_share' AND origin_kind='activity' "
                               "AND origin_ref=? AND origin_version=? AND state='proposed'");
                cancel.addBindValue(ref);
                cancel.addBindValue(intentVersion);
                if (!cancel.exec()) return QJsonArray{};
            }
            continue;
        }
        if (!markIntent(candidate.id, QStringLiteral("requested"), QString(), now))
            return QJsonArray{};
        result.append(QJsonObject{
            {QStringLiteral("ref"), ref},
            {QStringLiteral("activity_id"), candidate.id},
            {QStringLiteral("activity_version"), candidate.version},
            {QStringLiteral("channel"), candidate.channel},
            {QStringLiteral("session"), candidate.session},
            {QStringLiteral("artifact_ref"), rowArtifactRef},
            {QStringLiteral("artifact_version"), intentVersion},
            {QStringLiteral("artifact_hash"), intentHash},
            {QStringLiteral("draft"), draft},
            {QStringLiteral("intent"), intent}});
    }
    // 游标推进：
    //   * 因 result 达上限提前停止 → 从最后处理到的行之后继续（不能跳到整批末尾，否则饿死）；
    //   * 本批满 64 行且已扫完 → 从最后一行继续；
    //   * 扫描不足 64 行 → 已到尾，下一轮从头（覆盖新增活动）。
    qlonglong nextCursor = 0;
    if (result.size() >= capped && lastScannedId > cursor) nextCursor = lastScannedId;
    else if (rows.size() >= 64) nextCursor = rows.last().id;
    setAttentionState(cursorKey, QString::number(nextCursor));
    if (!m_db.commit()) return QJsonArray{};
    rollback.dismiss();
    if (!result.isEmpty()) qInfo() << "[Activity] share_intents_claimed=" << result.size();
    return result;
}

bool MemoryManager::markActivityShareIntentWithheld(const QString &ref, int artifactVersion,
                                                    const QString &reason)
{
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return false;
    if (parsed.kind != ActivityRef::InternalActivity) return false;
    if (!m_db.isOpen() || artifactVersion <= 0) return false;
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET result_json=json_patch(coalesce(result_json,'{}'),?),"
        "updated_at=unixepoch() "
        "WHERE id=? AND status='completed' "
        "AND json_extract(result_json,'$.share_intent.artifact_version')=?"));
    update.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("share_intent"),
         QJsonObject{{QStringLiteral("recovery_state"), QStringLiteral("withheld")},
                     {QStringLiteral("recovery_reason"), reason.trimmed().left(120)},
                     {QStringLiteral("recovery_at"), QDateTime::currentSecsSinceEpoch()}}}})
        .toJson(QJsonDocument::Compact)));
    update.addBindValue(parsed.id);
    update.addBindValue(artifactVersion);
    if (!update.exec() || update.numRowsAffected() != 1) return false;
    qInfo() << "[Activity] share_intent_withheld ref=" << ref
            << "artifact_version=" << artifactVersion << "reason=" << reason;
    return true;
}

bool MemoryManager::markActivityShareIntentRequested(const QString &ref, int artifactVersion)
{
    ActivityRef parsed;
    if (!ActivityRef::parse(ref, &parsed)) return false;
    if (parsed.kind != ActivityRef::InternalActivity) return false;
    if (!m_db.isOpen() || artifactVersion <= 0) return false;
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE internal_activity SET result_json=json_patch(coalesce(result_json,'{}'),?),"
        "updated_at=unixepoch() "
        "WHERE id=? AND status='completed' "
        "AND json_extract(result_json,'$.share_intent.state')='saved' "
        "AND coalesce(json_extract(result_json,'$.share_intent.recovery_state'),'') "
        "NOT IN ('requested','withheld') "
        "AND json_extract(result_json,'$.share_intent.artifact_version')=?"));
    update.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("share_intent"),
         QJsonObject{{QStringLiteral("recovery_state"), QStringLiteral("requested")},
                     {QStringLiteral("recovery_reason"), QString{}},
                     {QStringLiteral("recovery_at"), QDateTime::currentSecsSinceEpoch()}}}})
        .toJson(QJsonDocument::Compact)));
    update.addBindValue(parsed.id);
    update.addBindValue(artifactVersion);
    return update.exec() && update.numRowsAffected() == 1;
}

// R12-2c-3：每次真实回顾尝试独立 run_id。领取时 thought_review_log 行已落库，runId 在
// PrivateReview 生成后回填最近一条仍为空的领取日志，避免旧尝试的迟到失败污染新版本状态。
bool MemoryManager::attachReviewAttemptRunId(const QString &sourceKind, const QString &sourceRef,
                                             const QString &runId)
{
    const QString kind = sourceKind.trimmed();
    const QString ref = sourceRef.trimmed();
    const QString run = runId.trimmed();
    if (!m_db.isOpen() || !reviewKindValid(kind) || ref.isEmpty() || run.isEmpty())
        return false;
    // thought_review_log 没有独立 id 列，使用 SQLite 隐式 rowid 定位最近尝试。
    const qlonglong turn = ref.section(QLatin1Char('/'), 1, 1).toLongLong();
    qlonglong logRow = 0;
    QSqlQuery find(m_db);
    if (!find.exec(QStringLiteral(
            "SELECT rowid FROM thought_review_log WHERE turn_id=%1 "
            "AND (run_id IS NULL OR run_id='') ORDER BY rowid DESC LIMIT 1").arg(turn)))
        return false;
    if (find.next()) logRow = find.value(0).toLongLong();
    if (logRow <= 0) {
        QSqlQuery latest(m_db);
        if (!latest.exec(QStringLiteral(
                "SELECT rowid FROM thought_review_log WHERE run_id IS NULL OR run_id='' "
                "ORDER BY rowid DESC LIMIT 1")))
            return false;
        if (latest.next()) logRow = latest.value(0).toLongLong();
    }
    if (logRow <= 0) return false;
    QString escaped = run;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery update(m_db);
    if (!update.exec(QStringLiteral("UPDATE thought_review_log SET run_id='%1' WHERE rowid=%2")
                         .arg(escaped).arg(logRow)))
        return false;
    return update.numRowsAffected() == 1;
}
