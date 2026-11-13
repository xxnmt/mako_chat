#include "memorymanager.h"
#include "debuglog.h"

#include <QStandardPaths>
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
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
    : QObject{parent}
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
                      "(user_input, raw_reply, channel, session_id, speaker, sensitive, turn_kind, trigger_event_ids, subject, state_updates, thought, cognitive_event) "
                      "VALUES (:user_input, :raw_reply, :channel, :session_id, :speaker, :sensitive, :turn_kind, :trigger_event_ids, :subject, :updates, :thought, :event)");
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
    }
    query.bindValue(":thought", thought);
    query.bindValue(":raw_reply", JsonStr);
    if (!query.exec()) {
        qDebug()<<"[MemoryManager]:本轮对话保存失败:"<<query.lastError().text();
        return false;
    }

    if (query.numRowsAffected() != 1) return false;
    const qlonglong indexedTurn = receivedTurn > 0 ? receivedTurn : query.lastInsertId().toLongLong();
    if (indexedTurn > 0 && !indexChatTurn(indexedTurn))
        qWarning() << "[MemoryIndex] incremental chat index failed:" << indexedTurn;
    const auto activityAction = response["activity_action"].toObject();
    if (!activityAction.isEmpty()) {
        const QString ref = activityAction["activity_ref"].toString();
        const qlonglong activityId = ref.startsWith(QStringLiteral("thought/"))
            ? ref.mid(QStringLiteral("thought/").size()).toLongLong()
            : 0;
        const QString action = activityAction["action"].toString();
        const int expectedVersion = activityAction["expected_version"].toInt(-1);
        if (activityId <= 0 || expectedVersion < 0
            || !applyThoughtAction(activityId, expectedVersion, action)) {
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
        if (m_db.commit()) return id;
    }
    qWarning() << "[Conversation] record input failed" << q.lastError() << m_db.lastError();
    m_db.rollback();
    return 0;
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

bool MemoryManager::claimThoughtReview(qlonglong id, int dailyLimit, int cooldownSeconds)
{
    if (dailyLimit <= 0 || !m_db.transaction())
        return false;
    QSqlQuery q(m_db);
    q.prepare(R"(
        INSERT INTO thought_review_log(turn_id,started_at)
        SELECT id,unixepoch() FROM chat_history WHERE id=? AND thought IS NOT NULL
        AND coalesce(review_after,0)<=unixepoch()
        AND (SELECT count(*) FROM thought_review_log
             WHERE date(started_at,'unixepoch','localtime')=date('now','localtime')) < ?
        AND coalesce((SELECT max(started_at) FROM thought_review_log),0)+?<=unixepoch()
    )");
    q.addBindValue(id);
    q.addBindValue(dailyLimit);
    q.addBindValue(qMax(60,cooldownSeconds));
    if (q.exec() && q.numRowsAffected()==1) {
        q.prepare("UPDATE chat_history SET review_after=unixepoch()+1800 WHERE id=?");
        q.addBindValue(id);
        if (q.exec() && m_db.commit())
            return true;
    }
    m_db.rollback();
    return false;
}

bool MemoryManager::finishThoughtReview(qlonglong id, int version, const QJsonObject &proposal)
{
    const auto status = proposal["status"].toString();
    if (status != "active" && status != "paused" && status != "completed"
        && status != "abandoned")
        return false;
    QJsonObject result{{"status",status}};
    for (const auto &key : {"reflection","goal","progress","next_step","draft","mood","paused_reason"}) {
        const int limit = (QString(key) == "draft") ? 800 : 200;
        const QString value = proposal[key].toString().trimmed().left(limit);
        if (!value.isEmpty())
            result[key] = value;
    }
    if (!result.contains("reflection"))
        return false;
    const int hours = qBound(1, proposal["review_hours"].toInt(24), 168);
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET thought_result=json_patch(coalesce(thought_result,'{}'),?),thought_version=thought_version+1,"
              "review_after=unixepoch()+? WHERE id=? AND thought_version=? AND thought IS NOT NULL");
    q.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    q.addBindValue(hours*3600);
    q.addBindValue(id);
    q.addBindValue(version);
    return q.exec() && q.numRowsAffected()==1;
}

bool MemoryManager::applyThoughtAction(qlonglong id, int expectedVersion, const QString &action)
{
    const QString status = action == "pause" ? QStringLiteral("paused")
        : action == "resume" ? QStringLiteral("active")
        : action == "abandon" ? QStringLiteral("abandoned")
        : QString{};
    if (status.isEmpty() || id <= 0 || expectedVersion < 0)
        return false;
    QJsonObject patch{{"status", status}};
    if (action == "pause") patch["paused_reason"] = QStringLiteral("user_paused");
    QSqlQuery q(m_db);
    q.prepare("UPDATE chat_history SET thought_result=json_patch(coalesce(thought_result,'{}'),?),"
              "thought_version=thought_version+1,review_after=0 "
              "WHERE id=? AND thought_version=? AND thought IS NOT NULL");
    q.addBindValue(QString::fromUtf8(QJsonDocument(patch).toJson(QJsonDocument::Compact)));
    q.addBindValue(id);
    q.addBindValue(expectedVersion);
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
    q.prepare("SELECT 1 FROM chat_history WHERE id=? AND thought IS NOT NULL AND thought_version=? "
              "AND coalesce(json_extract(thought_result,'$.status'),'active') IN ('active','completed')");
    q.addBindValue(id);
    q.addBindValue(expectedVersion);
    return q.exec() && q.next();
}

QJsonArray MemoryManager::personalEvidence(const QString &subject, const QStringList &accounts,
                                           const QStringList &terms, bool includeOthers,
                                           const QString &currentChannel, const QString &currentSession,
                                           bool excludeCurrent, bool includeUnmatched,
                                           bool includeAllEpisodic, bool requireTermMatch) const
{
    // raw_events already reflects the inbound retention decision. Historical short-only
    // chat rows lack this provenance and must not be promoted during migration.
    QSqlQuery query(m_db);
    query.prepare(R"(
        WITH evidence AS (
            SELECT content AS text,occurred_at AS time,channel,session_id AS session,
                   sender_id IN (SELECT value FROM json_each(:accounts)) AS own,
                   sender_id AS speaker,'raw/'||rowid AS ref
            FROM raw_events WHERE direction='in' AND event_type='message' AND sensitive=0
              AND redaction_status='none' AND content IS NOT NULL
              AND (:all_people OR sender_id IN (SELECT value FROM json_each(:accounts)))
            UNION ALL
            SELECT content,event_time,origin,session_id,subject=:subject,subject,'episode/'||rowid FROM episodic_memory
            WHERE (subject=:subject OR :all_people) AND (origin='pet' OR :all_episodic) AND status='active'
            UNION ALL
            SELECT safe_summary,ended_at,channel,session_id,0,'summary','interaction/'||rowid FROM interactions
            WHERE :all_people AND summary_status='ready' AND safe_summary IS NOT NULL
            UNION ALL
            SELECT summary_text,created_at,channel,session_id,0,'summary','summary/'||rowid FROM long_term_summary
            WHERE :all_people AND is_dirty=0
            UNION ALL
            SELECT statement,valid_time_start,source_channel,source_session_id,0,'claim','claim/'||rowid
            FROM memory_claims WHERE :all_people AND claim_status='active' AND privacy_level<3
            UNION ALL
            SELECT input,reported,channel,session,1,subject,'source/'||id FROM cognitive_source
            WHERE subject=:subject AND outcome IS NULL
            UNION ALL
            SELECT r.quote,s.reported,s.channel,s.session,s.subject=:subject,s.subject,'revision/'||r.matter||'/'||r.version
            FROM cognitive_source s JOIN matter_revision r ON r.source=s.id
            WHERE (s.subject=:subject OR :all_people)
            UNION ALL
            SELECT json_extract(j.value,'$.quote'),h.timestamp,h.channel,h.session_id,h.subject=:subject,h.subject,'legacy/'||h.id||'/'||j.key
            FROM chat_history h,json_each(h.state_updates) j
            WHERE (h.subject=:subject OR :all_people) AND h.state_updates IS NOT NULL AND h.sensitive=0
        ), candidates AS (
            SELECT text,time,channel,session,own,speaker,min(ref) AS ref,
                (SELECT count(*) FROM json_each(:terms) WHERE instr(text,value)>0) AS matches
            FROM evidence WHERE text <> ''
              AND (:exclude_current=0 OR NOT (channel=:current_channel AND session=:current_session))
            GROUP BY text,time,channel,session,own,speaker
        ), ranked AS (
            SELECT *,
                row_number() OVER (ORDER BY matches DESC,own DESC,datetime(time) DESC,channel,session,speaker,text) AS topic_rank,
                row_number() OVER (PARTITION BY own ORDER BY datetime(time) DESC,channel,session,speaker,text) AS timeline_rank
            FROM candidates WHERE (own OR matches>0 OR :include_unmatched)
              AND (:require_match=0 OR matches>0)
        )
        SELECT substr(text,1,360),time,channel,session,own,speaker,ref FROM ranked
        ORDER BY CASE WHEN own THEN min(topic_rank,timeline_rank) ELSE topic_rank END,
                 topic_rank LIMIT 12
    )");
    auto json = [](const QStringList &strings) {
        return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(strings)).toJson(QJsonDocument::Compact));
    };
    query.bindValue(":subject", subject);
    query.bindValue(":accounts", json(accounts));
    query.bindValue(":all_people", int(includeOthers));
    query.bindValue(":current_channel", currentChannel.isNull() ? QStringLiteral("") : currentChannel);
    query.bindValue(":current_session", currentSession.isNull() ? QStringLiteral("") : currentSession);
    query.bindValue(":exclude_current", int(excludeCurrent));
    query.bindValue(":include_unmatched", int(includeUnmatched));
    query.bindValue(":all_episodic", int(includeAllEpisodic));
    query.bindValue(":require_match", int(requireTermMatch));
    query.bindValue(":terms", json(terms));
    QJsonArray evidence;
    if (query.exec()) {
        while (query.next())
            evidence.append(QJsonObject{{"text",query.value(0).toString()},
                                        {"time",query.value(1).toString()},
                                        {"reference",query.value(6).toString()},
                                        {"channel",query.value(2).toString()},
                                        {"session",query.value(3).toString()},
                                        {"own",query.value(4).toBool()}, {"speaker",query.value(5).toString()}});
    }
    return evidence;
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
        qlonglong chunkId = insert.lastInsertId().toLongLong();
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
            "FROM raw_events WHERE direction='in' AND event_type='message' AND sensitive=0 "
            "AND redaction_status='none' AND content IS NOT NULL AND content<>''"))) {
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
    q.prepare("SELECT id,source_type,source_ref,chunk_kind,derived,text,content_hash,"
              "channel,session_id,speaker_id,speaker_name,speaker_role,"
              "speech_at,event_time,recorded_at,privacy,status "
              "FROM memory_chunks WHERE status='active' AND index_generation=1 "
              "ORDER BY id LIMIT ?");
    q.addBindValue(qMax(1, limit));
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

QJsonArray MemoryManager::memoryChunksMissingEmbedding(int limit, const QString &model) const
{
    QJsonArray result;
    if (model.isEmpty())
        return result;
    QSqlQuery q(m_db);
    q.prepare("SELECT c.id,c.source_type,c.source_ref,c.chunk_kind,c.derived,c.text,c.content_hash,"
              "c.channel,c.session_id,c.speaker_id,c.speaker_name,c.speaker_role,"
              "c.speech_at,c.event_time,c.recorded_at,c.privacy,c.status "
              "FROM memory_chunks c WHERE c.status='active' AND c.index_generation=1 "
              "AND NOT EXISTS (SELECT 1 FROM memory_embeddings e WHERE e.chunk_id=c.id "
              "AND e.model_config=? AND e.content_hash=c.content_hash) "
              "ORDER BY c.id LIMIT ?");
    q.addBindValue(model);
    q.addBindValue(qMax(1, limit));
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

bool MemoryManager::storeChunkEmbedding(qlonglong chunkId, const QString &model, int dimension,
                                        const QByteArray &blob, const QString &contentHash)
{
    if (chunkId <= 0 || model.isEmpty() || dimension <= 0 || blob.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare("INSERT OR REPLACE INTO memory_embeddings("
              "chunk_id,model_config,dimension,vector,content_hash,index_generation,created_at)"
              " VALUES(?,?,?,?,?,1,?)");
    q.addBindValue(chunkId);
    q.addBindValue(model);
    q.addBindValue(dimension);
    q.addBindValue(blob);
    q.addBindValue(contentHash);
    q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
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
                                      QString *error)
{
    if (!provider) {
        if (error) *error = QStringLiteral("embedding provider is null");
        return false;
    }
    const int chunkBatch = qMax(1, limit);
    int embedded = 0;
    QHash<QByteArray, QVector<float>> textVectors;
    while (true) {
        const auto chunks = memoryChunksMissingEmbedding(chunkBatch, provider->modelId());
        if (chunks.isEmpty())
            break;

        QStringList pendingTexts;
        QList<QByteArray> pendingKeys;
        QHash<QByteArray, int> pendingIndex;
        for (const auto &value : chunks) {
            const auto chunk = value.toObject();
            const QByteArray key = chunk.value(QStringLiteral("text")).toString().toUtf8();
            if (textVectors.contains(key) || pendingIndex.contains(key))
                continue;
            pendingIndex.insert(key, pendingTexts.size());
            pendingKeys.append(key);
            pendingTexts.append(QString::fromUtf8(key));
        }
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

        for (const auto &value : chunks) {
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
            ++embedded;
        }
        if (chunks.size() < chunkBatch)
            break;
    }
    QSqlQuery state(m_db);
    state.prepare("INSERT OR REPLACE INTO memory_index_state(key,value) VALUES(?,?)");
    state.bindValue(0, QStringLiteral("embeddings"));
    state.bindValue(1, QString::number(embedded));
    state.exec();
    return true;
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
    qlonglong chunkId = insert.lastInsertId().toLongLong();
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
    QSqlQuery remove(m_db);
    remove.prepare("DELETE FROM memory_chunks WHERE source_type='raw' AND source_ref=?");
    remove.addBindValue(QStringLiteral("raw/%1").arg(id));
    if (!remove.exec())
        return false;
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
    return upsertMemoryChunk(chunk);
}

bool MemoryManager::indexChatTurn(qlonglong id)
{
    if (id <= 0)
        return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT channel,session_id,subject,speaker,user_input,raw_reply,timestamp,sensitive "
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
    const QString privacy = channel == Channel::kQQGroup
        ? QStringLiteral("public") : QStringLiteral("private");
    QSqlQuery remove(m_db);
    remove.prepare("DELETE FROM memory_chunks WHERE source_type='chat' AND source_ref LIKE ?");
    remove.addBindValue(QStringLiteral("chat/%1:%").arg(id));
    if (!remove.exec())
        return false;
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
    if (audience.sensitive || !audience.longTermMemoryEnabled) return result;
    QSqlQuery q(m_db);
    q.prepare("SELECT s.id,s.reported,r.quote,r.kind,r.event_time,r.recorded,r.status,r.understanding "
              "FROM matter_revision r JOIN cognitive_source s ON s.id=r.source "
              "WHERE r.matter=? AND s.subject=? AND (? OR (s.channel=? AND s.session=?)) "
              "ORDER BY r.version DESC LIMIT 8");
    q.addBindValue(matter);
    q.addBindValue(audience.subject);
    q.addBindValue(audience.channel != Channel::kQQGroup);
    q.addBindValue(audience.channel);
    q.addBindValue(audience.sessionId);
    if (!q.exec()) return result;
    while (q.next()) result.prepend(QJsonObject{
        {"evidence_ref",QString("source/%1").arg(q.value(0).toLongLong())},
        {"reported_at",q.value(1).toString()},{"quote",q.value(2).toString()},
        {"source_kind",q.value(3).toString()},{"event_time_expression",q.value(4).toString()},
        {"interpretation_recorded_at",q.value(5).toString()},{"effective_at",QJsonValue::Null},
        {"status",q.value(6).toString()},{"understanding",q.value(7).toString()}});
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
                   NULL AS status,NULL AS kind,NULL AS understanding,NULL AS event_time,NULL AS recorded
            FROM chat_history h,json_each(h.state_updates) j
            WHERE h.subject=? AND h.state_updates IS NOT NULL AND h.sensitive=0
              AND NOT EXISTS (SELECT 1 FROM cognitive_source WHERE event=h.cognitive_event)
            UNION ALL
            SELECT s.id,s.reported,r.topic,r.quote,(s.channel=? AND s.session=?),
                   r.matter,r.version,r.status,r.kind,r.understanding,r.event_time,r.recorded
            FROM matter_revision r JOIN cognitive_source s ON s.id=r.source WHERE s.subject=?
        ), ranked AS (
            SELECT *,row_number() OVER (PARTITION BY matter ORDER BY version DESC,id DESC) AS pos,
                   max(local) OVER (PARTITION BY matter) AS mentioned_here FROM evidence
        ), self_ranked AS (
            SELECT matter,quote,version,status,id,timestamp,recorded,
                   row_number() OVER (PARTITION BY matter ORDER BY version DESC,id DESC) AS spos
            FROM evidence WHERE kind='self_report'
        )
        SELECT r.topic,r.quote,r.timestamp,r.local,r.mentioned_here,r.version,r.status,r.kind,
               r.understanding,r.id,r.matter,r.event_time,r.recorded,
               sr.quote,sr.version,sr.status,sr.id,sr.timestamp,sr.recorded
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

    QStringList selected;
    while (query.next()) {
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

QList<HistoryTurn> MemoryManager::searchHistoryTurns(const QString &channel, const QString &sessionId,
                                                   const QStringList &terms, int limit,
                                                   const QDateTime &from, const QDateTime &to)
{
    QList<HistoryTurn> result;
    if (!m_db.open() || limit <= 0)
        return result;
    QString sql = QStringLiteral(
        "SELECT id,user_input,raw_reply,timestamp,turn_kind,subject,speaker "
        "FROM chat_history WHERE channel=? AND session_id=? AND sensitive=0");
    QStringList safeTerms;
    for (const auto &term : terms) {
        const auto trimmed = term.trimmed();
        if (trimmed.isEmpty() || safeTerms.contains(trimmed))
            continue;
        safeTerms.append(trimmed);
        if (safeTerms.size() >= 8)
            break;
    }
    if (!safeTerms.isEmpty()) {
        QStringList clauses;
        for (int i = 0; i < safeTerms.size(); ++i)
            clauses.append(QStringLiteral("(user_input LIKE ? OR raw_reply LIKE ?)"));
        sql += QStringLiteral(" AND (") + clauses.join(QStringLiteral(" OR ")) + QStringLiteral(")");
    }
    if (from.isValid())
        sql += QStringLiteral(" AND timestamp>=?");
    if (to.isValid())
        sql += QStringLiteral(" AND timestamp<?");
    sql += QStringLiteral(" ORDER BY id DESC LIMIT ?");
    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(channel);
    q.addBindValue(sessionId);
    for (const auto &term : safeTerms) {
        const auto pattern = QStringLiteral("%") + term + QStringLiteral("%");
        q.addBindValue(pattern);
        q.addBindValue(pattern);
    }
    if (from.isValid())
        q.addBindValue(from);
    if (to.isValid())
        q.addBindValue(to);
    q.addBindValue(qMax(1, limit));
    if (!q.exec())
        return result;
    while (q.next()) {
        HistoryTurn turn;
        turn.id = q.value(0).toLongLong();
        turn.userInput = q.value(1).toString();
        turn.rawReply = q.value(2).toString();
        turn.timestamp = q.value(3).toDateTime();
        turn.turnKind = q.value(4).toString();
        turn.subject = q.value(5).toString();
        turn.speaker = q.value(6).toString();
        result.prepend(turn);
    }
    return result;
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
                                                              const QString &sessionId)
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
    if (!memories.isEmpty()) {
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
    if (rawId > 0 && !indexRawEvent(rawId))
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
    query.addBindValue(qMax(1, limit));
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
                  "AND started_at < ? AND ended_at > ? "
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
                  "AND started_at < ? AND ended_at > ?");
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
                                             const QDateTime &since)
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
        "WHERE channel='qq_group' AND event_type='message' "
        "AND content IS NOT NULL "
        "AND COALESCE(redaction_status, 'none') <> 'blocked' "
        "AND session_id IN (%1)").arg(placeholders.join(','));
    if (since.isValid()) {
        sql += QStringLiteral(" AND occurred_at >= ?");
    }
    sql += QStringLiteral(" GROUP BY session_id ORDER BY latest DESC LIMIT ?");

    QSqlQuery query(m_db);
    query.prepare(sql);
    for (const QString &groupId : allowedGroupIds) {
        query.addBindValue(groupId);
    }
    if (since.isValid()) {
        query.addBindValue(since.toString("yyyy-MM-dd HH:mm:ss"));
    }
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
        query.allowedGroupIds, query.maxGroups, query.since);
    QList<RecentContextItem> candidates;
    for (const QString &groupId : groups) {
        QString sql = QStringLiteral(
            "SELECT id, session_id, message_id, sender_id, sender_name, "
            "person_id, content, payload, occurred_at "
            "FROM raw_events "
            "WHERE channel='qq_group' AND session_id=? "
            "AND event_type='message' AND content IS NOT NULL "
            "AND COALESCE(redaction_status, 'none') <> 'blocked'");
        if (query.since.isValid()) {
            sql += QStringLiteral(" AND occurred_at >= ?");
        }
        if (query.until.isValid()) {
            sql += QStringLiteral(" AND occurred_at < ?");
        }
        if (query.onlyViewer && !query.viewerPersonId.isEmpty()) {
            sql += QStringLiteral(" AND person_id=?");
        }
        sql += QStringLiteral(" ORDER BY occurred_at DESC, id DESC LIMIT ?");

        QSqlQuery eventQuery(m_db);
        eventQuery.prepare(sql);
        eventQuery.addBindValue(groupId);
        if (query.since.isValid()) {
            eventQuery.addBindValue(query.since.toString("yyyy-MM-dd HH:mm:ss"));
        }
        if (query.until.isValid()) {
            eventQuery.addBindValue(query.until.toString("yyyy-MM-dd HH:mm:ss"));
        }
        if (query.onlyViewer && !query.viewerPersonId.isEmpty()) {
            eventQuery.addBindValue(query.viewerPersonId);
        }
        eventQuery.addBindValue(qMax(1, query.perGroupCandidateLimit));
        if (!eventQuery.exec()) {
            qDebug() << "[MemoryManager]查询近期群消息失败:" << eventQuery.lastError();
            continue;
        }
        while (eventQuery.next()) {
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
            if (item.isViewer) {
                // 旧数据兼容：没有 person_id 时仍按 viewer 平台 ID 识别。
                item.isViewer = true;
            } else if (query.viewerPersonId.isEmpty()) {
                item.isViewer = query.viewerIds.contains(item.senderId);
            }
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
        }
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

void MemoryManager::seedIdentity(const QStringList &masterUserIds)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return;
    }
    m_masterUserIds.clear();
    for (const QString &id : masterUserIds) {
        if (!id.isEmpty()) {
            m_masterUserIds.insert(id);
        }
    }
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
        if (!m_db.commit()) throw m_db.lastError();
        qDebug()<<"[MemoryManager]数据库初始化完成,结构版本为:"<<currentVersion;
    }
    catch(const QSqlError &error){
        m_db.rollback();
        qDebug()<<"[MemoryManager]数据库更新失败:"<<error;
    }
}
