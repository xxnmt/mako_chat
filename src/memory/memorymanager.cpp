#include "mattersemantics.h"
#include "memorymanager.h"
#include "actionpolicy.h"
#include "debuglog.h"
#include "configmanager.h"
#include "memoryreadpolicy.h"

#include <QStandardPaths>
#include <QScopeGuard>
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QThread>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDebug>
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

    QString thought;
    QJsonArray updates;
    if (meta.longTermMemoryEnabled && meta.persistTurn && !meta.sensitive
        && meta.turnKind == "reply" && !meta.subject.isEmpty()
        && (meta.channel == Channel::kPet || !meta.actorId.isEmpty())) {
        if (meta.subject == "user" && (meta.channel == Channel::kPet || meta.channel == Channel::kQQPrivate)) {
            const auto candidate = response["personal_interest"].toString().trimmed();
            if (!candidate.isEmpty() && candidate.size() <= 120 && !hasActiveThought(meta.subject))
                thought = candidate;
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
        query.prepare("UPDATE chat_history SET raw_reply=:raw_reply,thought=:thought,turn_state='handed_off' "
                      "WHERE id=:id AND cognitive_event=:event AND turn_state='received'");
        query.bindValue(":id", receivedTurn);
        query.bindValue(":event", meta.eventId);
    } else {
        query.prepare("INSERT INTO chat_history "
                      "(user_input, raw_reply, channel, session_id, speaker, sensitive, turn_kind, trigger_event_ids, subject, state_updates, thought, cognitive_event, timestamp) "
                      "VALUES (:user_input, :raw_reply, :channel, :session_id, :speaker, :sensitive, :turn_kind, :trigger_event_ids, :subject, :updates, :thought, :event, :timestamp)");
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
    query.bindValue(":thought", thought);
    query.bindValue(":raw_reply", JsonStr);
    if (!query.exec()) {
        qDebug()<<"[MemoryManager]:本轮对话保存失败:"<<query.lastError().text();
        return false;
    }

    if (query.numRowsAffected() != 1) return false;
    const qlonglong indexedTurn = receivedTurn > 0 ? receivedTurn : query.lastInsertId().toLongLong();
    if (!thought.isEmpty() && indexedTurn > 0) {
        // B2：念头/活动的持久化档案。source_turn 必须等于所在行 id；新插入时只有拿到
        // last_insert_rowid 才知道 id，所以插入成功后在同一连接内补写一次（恢复轮次直接
        // 用 receivedTurn）。这里覆盖该行 thought_result 是安全的：能在本轮创建活动的前
        // 提是 hasActiveThought()==false，即该行还没有活动档案。
        const auto nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        const QJsonObject result{
            {QStringLiteral("status"), QStringLiteral("active")},
            {QStringLiteral("goal_kind"), QStringLiteral("interest")},
            {QStringLiteral("source_turn"), indexedTurn},
            {QStringLiteral("created_at"), nowIso},
            {QStringLiteral("updated_at"), nowIso},
            {QStringLiteral("artifact_ref"), QString{}}};
        QSqlQuery init(m_db);
        init.prepare("UPDATE chat_history SET thought_result=? "
                     "WHERE id=? AND thought IS NOT NULL AND thought<>''");
        init.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        init.addBindValue(indexedTurn);
        if (!init.exec() || init.numRowsAffected() != 1)
            qWarning() << "[MemoryManager]thought_result 初始化失败:" << indexedTurn << init.lastError();
    }
    if (indexedTurn > 0 && !indexChatTurn(indexedTurn))
        qWarning() << "[MemoryIndex] incremental chat index failed:" << indexedTurn;
    else if (indexedTurn > 0 && !meta.sensitive)
        emit searchChunksChanged();
    const auto activityAction = response["activity_action"].toObject();
    if (!activityAction.isEmpty()) {
        const QString ref = activityAction["activity_ref"].toString();
        const qlonglong activityId = ref.startsWith(QStringLiteral("thought/"))
            ? ref.mid(QStringLiteral("thought/").size()).toLongLong()
            : 0;
        const QString action = activityAction["action"].toString();
        const int expectedVersion = activityAction["expected_version"].toInt(-1);
        if (meta.subject != "user" || meta.sensitive || !meta.longTermMemoryEnabled
            || (meta.channel!=Channel::kPet && meta.channel!=Channel::kQQPrivate)
            || activityId <= 0 || expectedVersion < 0
            || !applyThoughtAction(activityId, expectedVersion, action,
                                  activityAction["after_minutes"].toInt(-1), activityAction["reason"].toString())) {
            recordCognitiveAudit("activity_action_rejected", meta.runId, 0,
                {{"activity_ref", ref}, {"action", action}, {"expected_version", expectedVersion}});
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

bool MemoryManager::chargePrivateReview(qlonglong turn, int dailyLimit, int cooldownSeconds)
{
    QSqlQuery q(m_db);
    q.prepare(R"(
        INSERT INTO thought_review_log(turn_id,started_at)
        SELECT ?,unixepoch() WHERE (SELECT count(*) FROM thought_review_log
             WHERE date(started_at,'unixepoch','localtime')=date('now','localtime')) < ?
        AND coalesce((SELECT max(started_at) FROM thought_review_log),0)+?<=unixepoch()
    )");
    q.addBindValue(turn);
    q.addBindValue(dailyLimit);
    q.addBindValue(qMax(60,cooldownSeconds));
    return q.exec() && q.numRowsAffected()==1;
}

bool MemoryManager::claimThoughtReview(qlonglong id, int version, int dailyLimit, int cooldownSeconds)
{
    if (dailyLimit <= 0 || !m_db.transaction()) return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET review_after=unixepoch()+1800,thought_version=thought_version+1 "
              "WHERE id=? AND thought IS NOT NULL AND thought_version=? AND sensitive=0 AND subject='user' "
              "AND coalesce(json_extract(thought_result,'$.status'),'active')='active' "
              "AND coalesce(review_after,0)<=unixepoch()");
    q.addBindValue(id);
    q.addBindValue(version);
    if (q.exec() && q.numRowsAffected()==1 && chargePrivateReview(id,dailyLimit,cooldownSeconds)
        && m_db.commit()) return true;
    m_db.rollback();
    return false;
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
    for (const auto &key : {"reflection","goal","progress","next_step","draft","mood","paused_reason"}) {
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
    if (!result.value(QStringLiteral("draft")).toString().isEmpty())
        result[QStringLiteral("artifact_ref")] = QStringLiteral("thought/%1").arg(id);
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

QJsonObject MemoryManager::claimAttentionEvent()
{
    if (!m_db.isOpen() || !m_db.transaction())
        return {};
    QJsonObject event;
    {
        QSqlQuery select(m_db);
        if (!select.exec("SELECT id,kind,ref,payload,priority,created_at FROM attention_event "
                         "WHERE state='pending' ORDER BY priority,id LIMIT 1")
            || !select.next()) {
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

QJsonArray MemoryManager::memoryChunksForEmbedding(int limit) const
{
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
    if (!q.exec())
        return result;
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

qlonglong MemoryManager::recordProactiveMessage(const QString &channel, const QString &session, const QString &text,
                                                const QString &kind, const QString &ref)
{
    const QString body = text.trimmed();
    if (!m_db.isOpen() || body.isEmpty() || channel.isEmpty() || session.isEmpty()) return 0;
    // 同一投递意图（kind+ref）只写一条历史：渠道拒绝后补发成功时，历史里不会出现
    // 两条一模一样的"茉子主动说过的话"。
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
    // 主动消息也按"茉子说过的话"存档：sentences 与正常回复同形，另带 delivery 引用，
    // 供下次对话把这句话当成 prior_assistant_expression 回认。
    QJsonObject sentence{{QStringLiteral("zh_text"), body},
                         {QStringLiteral("ja_text"), QString{}},
                         {QStringLiteral("tags"), QJsonObject{}}};
    QJsonObject root{{QStringLiteral("sentences"), QJsonArray{sentence}},
                     {QStringLiteral("delivery"), QJsonObject{{QStringLiteral("kind"), kind},
                                                              {QStringLiteral("ref"), ref}}}};
    const QString rawReply = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!m_db.transaction()) return 0;
    QSqlQuery q(m_db);
    // user_input 用 SQL NULL：主动轮次没有用户发言，不能伪装成用户输入参与回顾/认识更新。
    // turn_state='handed_off' 与正常轮次一致；turn_kind='proactive' 让回顾领取与认识更新跳过它。
    q.prepare("INSERT INTO chat_history(user_input,raw_reply,channel,session_id,speaker,sensitive,"
              "turn_kind,subject,turn_state,cognitive_event,timestamp) "
              "VALUES(NULL,?,?,?,'mako',0,'proactive','user','handed_off',NULL,?)");
    q.addBindValue(rawReply);
    q.addBindValue(channel);
    q.addBindValue(session);
    q.addBindValue(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "[MemoryManager]主动消息写历史失败:" << q.lastError();
        m_db.rollback();
        return 0;
    }
    const qlonglong id = q.lastInsertId().toLongLong();
    // raw_events：direction='out'；发送者留空，避免给"茉子"造一条 QQ 身份映射。
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
    // 出站流水没有需要再消费的原始事件；标成已处理，避免被群聊切片器当成未读消息。
    event.processed = true;
    event.visibility = channel == Channel::kQQGroup ? QStringLiteral("channel")
                                                    : QStringLiteral("private");
    event.redactionStatus = QStringLiteral("none");
    // 原始流水失败时整次回滚，避免只留半条历史。
    if (!appendRawEvent(event) || !m_db.commit()) {
        qWarning() << "[MemoryManager]主动消息写原始事件失败:" << m_db.lastError();
        m_db.rollback();
        return 0;
    }
    // commit 后再建检索块：主动轮次没有用户块，assistant 块收录这里说的 zh_text。
    if (id > 0 && !indexChatTurn(id))
        qWarning() << "[MemoryIndex] proactive chat index failed:" << id;
    else if (id > 0)
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
                                                  const QString &session,int limit) const
{
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
    }
    return rows;
}

QJsonArray MemoryManager::recentSelfExpressions(const QString &subject,const QString &channel,
                                                const QString &session,int limit) const
{
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
    }
    return rows;
}

QJsonArray MemoryManager::recentInferences(const QString &subject,int limit) const
{
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
    }
    return rows;
}

QJsonArray MemoryManager::currentMotivations(int limit) const
{
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
        const auto channel=concerns.value(5).toString();
        const auto session=concerns.value(6).toString();
        const auto note=concerns.value(3).toString();
        if (!policy.allows(channel,session,note)) continue;
        const auto topic=concerns.value(2).toString();
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("concern/%1").arg(concerns.value(4).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("concern")},
                                {QStringLiteral("summary"),(topic.isEmpty()?note:topic).left(160)},
                                {QStringLiteral("version"),concerns.value(1).toInt()}});
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
        const auto channel=thoughts.value(3).toString();
        const auto session=thoughts.value(4).toString();
        const auto text=thoughts.value(1).toString();
        if (!policy.allows(channel,session,text)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("thought/%1").arg(thoughts.value(0).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("interest")},
                                {QStringLiteral("summary"),text.left(160)},
                                {QStringLiteral("version"),thoughts.value(2).toInt()}});
    }
    return rows;
}

QJsonArray MemoryManager::activeCommitments(const QString &channel,const QString &session,
                                            int limit) const
{
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
        const auto message=q.value(4).toString();
        if (!policy.allows(channel,session,message)) continue;
        rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("reminder/%1").arg(q.value(0).toLongLong())},
                                {QStringLiteral("kind"),QStringLiteral("reminder")},
                                {QStringLiteral("state"),q.value(1).toString()},
                                {QStringLiteral("due_at"),snapshotIsoFromEpoch(q.value(2).toLongLong())},
                                {QStringLiteral("expires_at"),snapshotIsoFromEpoch(q.value(3).toLongLong())},
                                {QStringLiteral("message"),message.left(80)}});
    }
    return rows;
}

QJsonArray MemoryManager::currentActivities(int limit) const
{
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
    }
    return rows;
}

QJsonArray MemoryManager::internalWakes(int limit) const
{
    QJsonArray rows;
    if (!m_db.isOpen() || limit<=0) return rows;
    // 1) 最近一次空闲唤醒（idle）。
    {
        QSqlQuery wake(m_db);
        if (wake.exec("SELECT value FROM attention_state WHERE key='last_wake_at'") && wake.next()) {
            const auto value=wake.value(0).toString();
            if (!value.trimmed().isEmpty())
                rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("attention/last_wake_at")},
                                        {QStringLiteral("kind"),QStringLiteral("idle")},
                                        {QStringLiteral("at"),snapshotIsoText(value)}});
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
            const auto at=snapshotIsoFromEpoch(concerns.value(2).toLongLong());
            rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("concern/%1").arg(concerns.value(1).toLongLong())},
                                    {QStringLiteral("kind"),QStringLiteral("concern_review")},
                                    {QStringLiteral("at"),at},
                                    // 与回顾提示词里的 own_next_review_at 同义：她自己的再检查时间。
                                    {QStringLiteral("own_next_review_at"),at}});
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
            rows.append(QJsonObject{{QStringLiteral("ref"),QStringLiteral("thought/%1").arg(thoughts.value(0).toLongLong())},
                                    {QStringLiteral("kind"),QStringLiteral("scheduled")},
                                    {QStringLiteral("at"),snapshotIsoFromEpoch(thoughts.value(1).toLongLong())}});
        }
    }
    return rows;
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
    }
    return true;
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
    QSqlQuery pick(m_db);
    // 只领取 queued，或"被渠道明确拒绝且还有一次重试预算"的失败行；
    // dispatching（可能已交给网络）永远不在这里重领——它只能由 recoverStaleOutbox 冻结为 unknown。
    pick.prepare("SELECT o.id,o.action_id,o.action_key,o.channel,o.session_id,o.payload_json,o.attempts,"
                 "a.kind,a.state FROM action_outbox o JOIN cognitive_action a ON a.id=o.action_id "
                 "WHERE o.state='queued' OR (o.state='failed' AND o.attempts<2) "
                 "ORDER BY o.id LIMIT 1");
    if (!pick.exec() || !pick.next()) { m_db.rollback(); return {}; }
    const auto outboxId=pick.value(0).toLongLong();
    const auto actionId=pick.value(1).toLongLong();
    const auto actionKey=pick.value(2).toString();
    const auto channel=pick.value(3).toString();
    const auto session=pick.value(4).toString();
    const auto payloadJson=pick.value(5).toString();
    const auto attempts=pick.value(6).toInt();
    const auto kind=pick.value(7).toString();
    pick.finish();

    QSqlQuery claim(m_db);
    claim.prepare("UPDATE action_outbox SET state='dispatching',lease_owner=?,"
                  "lease_until=unixepoch()+?,dispatched_at=coalesce(dispatched_at,unixepoch()),"
                  "attempts=attempts+1 WHERE id=? AND (state='queued' "
                  "OR (state='failed' AND attempts<2))");
    claim.addBindValue(owner);
    claim.addBindValue(leaseSeconds);
    claim.addBindValue(outboxId);
    if (!claim.exec() || claim.numRowsAffected()!=1) { m_db.rollback(); return {}; }

    QSqlQuery touch(m_db);
    touch.prepare("UPDATE cognitive_action SET state='dispatching',updated_at=unixepoch() "
                  "WHERE id=? AND state NOT IN ('unknown','withheld')");
    touch.addBindValue(actionId);
    if (!touch.exec()) { m_db.rollback(); return {}; }

    if (!markOutboxDomainDispatching(m_db,actionKey,kind,channel,session)) {
        m_db.rollback();
        return {};
    }
    if (!m_db.commit()) return {};
    rollback.dismiss();
    qInfo()<<"[Outbox] claimed key="<<actionKey<<"kind="<<kind<<"attempt="<<(attempts+1);
    return QJsonObject{{QStringLiteral("outbox_id"),outboxId},
                       {QStringLiteral("action_id"),actionId},
                       {QStringLiteral("action_key"),actionKey},
                       {QStringLiteral("kind"),kind},
                       {QStringLiteral("channel"),channel},
                       {QStringLiteral("session"),session},
                       {QStringLiteral("payload_json"),payloadJson},
                       {QStringLiteral("payload"),QJsonDocument::fromJson(payloadJson.toUtf8()).object()},
                       {QStringLiteral("attempts"),attempts+1}};
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
                 "o.id,o.attempts FROM cognitive_action a "
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
    find.finish();

    // 终态保护：unknown/withheld 冻结，绝不因迟到的重复回执再变；已协议接受的只允许同结果重复。
    if (state==QStringLiteral("unknown") || state==QStringLiteral("withheld")) {
        m_db.rollback();
        rollback.dismiss();
        return true;
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
