#include "memorymanager.h"
#include "configmanager.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QJsonDocument>

QJsonArray MemoryManager::historicalReports(const QString &subject, const QString &channel, const QString &session,
                                           const QString &before, const QStringList &terms, bool forRevision) const
{
    QSqlQuery q(m_db);
    q.prepare(R"(
        WITH candidates AS (
            SELECT h.id,coalesce(s.reported,h.timestamp) AS reported,user_input,
                (SELECT count(*) FROM json_each(:terms) WHERE instr(user_input,value)>0) matches
            FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event
            WHERE h.subject=:subject AND h.channel=:channel AND session_id=:session
              AND h.sensitive=0 AND h.turn_kind='reply' AND length(user_input)>0
              AND (:revision=0 OR s.id IS NOT NULL OR h.state_updates IS NOT NULL OR h.thought IS NOT NULL)
              AND coalesce(julianday(s.reported),julianday(h.timestamp,'utc'))<julianday(:before)
        ), ranked AS (
            SELECT *,row_number() OVER (ORDER BY matches DESC,id DESC) topic_rank,
                     row_number() OVER (ORDER BY id DESC) recent_rank FROM candidates
        )
        SELECT id,reported,user_input FROM ranked ORDER BY min(topic_rank,recent_rank),topic_rank LIMIT 6
    )");
    q.bindValue(":subject",subject);
    q.bindValue(":revision",int(forRevision));
    q.bindValue(":channel",channel);
    q.bindValue(":session",session);
    q.bindValue(":before",before);
    q.bindValue(":terms",QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(terms)).toJson(QJsonDocument::Compact)));
    QJsonArray reports;
    if (q.exec()) {
        while (q.next())
            reports.append(QJsonObject{{"turn",q.value(0).toLongLong()},{"reported_at",q.value(1).toString()},
                                       {"text",q.value(2).toString()}});
    }
    return reports;
}

bool MemoryManager::enqueueObservation(const RequestMeta &meta, const QString &input)
{
    if (!meta.persistTurn || !meta.longTermMemoryEnabled || meta.sensitive || meta.turnKind != "reply"
        || meta.eventId.isEmpty() || meta.subject.isEmpty() || input.trimmed().isEmpty()
        || (meta.channel != Channel::kPet && meta.actorId.isEmpty()))
        return false;

    const auto reported = meta.occurredAtUtc.isValid() ? meta.occurredAtUtc : meta.receivedAtUtc;
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO cognitive_source(event,run,subject,channel,session,input,reported) "
              "VALUES(?,?,?,?,?,?,?) ON CONFLICT(event) DO NOTHING");
    q.addBindValue(meta.eventId);
    q.addBindValue(meta.runId);
    q.addBindValue(meta.subject);
    q.addBindValue(meta.channel);
    q.addBindValue(meta.sessionId);
    q.addBindValue(input);
    q.addBindValue((reported.isValid() ? reported.toUTC() : QDateTime::currentDateTimeUtc()).toString(Qt::ISODateWithMs));
    if (q.exec()) {
        qInfo() << "[Cognition] queued run=" << meta.runId << "inserted=" << q.numRowsAffected();
        return true;
    }
    qWarning() << "[Cognition] enqueue_failed run=" << meta.runId;
    return false;
}

QJsonObject MemoryManager::pendingObservation(qint64 source) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT id,run,subject,input,reported,channel,session FROM cognitive_source s "
                "WHERE (:source=0 OR id=:source) AND outcome IS NULL AND due<=unixepoch() AND NOT EXISTS "
                "(SELECT 1 FROM cognitive_source p WHERE p.subject=s.subject AND p.outcome IS NULL AND p.id<s.id) "
                "ORDER BY id LIMIT 1");
    q.bindValue(":source",source);
    if (!q.exec() || !q.next())
        return {};
    return {{"source",q.value(0).toLongLong()},{"run",q.value(1).toString()},
            {"subject",q.value(2).toString()},{"input",q.value(3).toString()},
            {"reported_at",q.value(4).toString()},{"channel",q.value(5).toString()},
            {"session",q.value(6).toString()}};
}

QJsonArray MemoryManager::matterHeads(const QString &subject, const QStringList &terms) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT matter,version,topic,status,quote,kind,reported,event_time,understanding,initial_quote,initial_reported FROM "
              "(SELECT r.*,s.reported,first_value(quote) OVER history AS initial_quote,"
              "first_value(reported) OVER history AS initial_reported,row_number() OVER(PARTITION BY matter ORDER BY version DESC) pos "
              "FROM matter_revision r JOIN cognitive_source s ON s.id=r.source WHERE s.subject=? "
              "WINDOW history AS (PARTITION BY matter ORDER BY version)) "
              "WHERE pos=1 ORDER BY (SELECT count(*) FROM json_each(?) WHERE "
              "instr(topic,value)>0 OR instr(understanding,value)>0 OR instr(initial_quote,value)>0) DESC,"
              "source DESC,matter LIMIT 48");
    q.addBindValue(subject);
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(terms)).toJson(QJsonDocument::Compact)));
    QJsonArray heads;
    if (q.exec()) {
        while (q.next())
            heads.append(QJsonObject{{"matter",q.value(0).toString()},{"version",q.value(1).toLongLong()},
                {"topic",q.value(2).toString()},{"status",q.value(3).toString()},{"quote",q.value(4).toString()},
                {"kind",q.value(5).toString()},{"reported_at",q.value(6).toString()},{"event_time",q.value(7).toString()},{"understanding",q.value(8).toString()},
                {"initial_quote",q.value(9).toString()},{"initial_reported_at",q.value(10).toString()}});
    }
    return heads;
}

QJsonArray MemoryManager::internalMatters(const RequestMeta &meta, const QStringList &terms,
                                          const QStringList &historyOf) const
{
    auto &config = ConfigManager::instance();
    const auto policy = config.getMemoryPolicyConfig();
    QJsonArray result;
    if (meta.sensitive || meta.subject.isEmpty() || !policy.longTermMemoryEnabled ||
        config.isBlindZone(meta.channel,meta.sessionId) ||
        (meta.channel != Channel::kPet && meta.actorId.isEmpty())) return result;

    QSqlQuery q(m_db);
    q.prepare(R"(
        WITH revisions AS (
            SELECT r.*,s.reported,s.channel,s.session,s.input,
                   row_number() OVER(PARTITION BY matter ORDER BY version DESC) pos
            FROM matter_revision r JOIN cognitive_source s ON s.id=r.source
            WHERE s.subject=:subject
              AND (:heads OR matter IN (SELECT value FROM json_each(:matters)))
        )
        SELECT matter,version,topic,quote,kind,status,understanding,event_time,reported,channel,session,input
        FROM revisions WHERE pos<=:depth
        ORDER BY (SELECT count(*) FROM json_each(:terms)
            WHERE instr(topic,value)>0 OR instr(quote,value)>0 OR instr(understanding,value)>0) DESC,
            version DESC,matter LIMIT 32
    )");
    q.bindValue(":subject",meta.subject);
    q.bindValue(":heads",historyOf.isEmpty());
    q.bindValue(":depth",historyOf.isEmpty() ? 1 : 8);
    q.bindValue(":matters",QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(historyOf)).toJson(QJsonDocument::Compact)));
    q.bindValue(":terms",QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(terms)).toJson(QJsonDocument::Compact)));
    if (!q.exec()) return result;
    QSet<QString> restricted;
    while (q.next()) {
        if (config.isBlindZone(q.value(9).toString(),q.value(10).toString())) {
            restricted.insert(q.value(0).toString());
            continue;
        }
        QJsonObject row{{"matter",q.value(0).toString()},{"version",q.value(1).toLongLong()},
            {"topic",q.value(2).toString()},{"quote",q.value(3).toString()},
            {"kind",q.value(4).toString()},{"status",q.value(5).toString()},
            {"understanding",q.value(6).toString()},{"event_time_expression",q.value(7).toString()},
            {"reported_at",q.value(8).toString()},{"original_report",q.value(11).toString()}};
        bool allowed = true;
        for (const auto &key : {"topic","quote","understanding","original_report","event_time_expression"}) {
            const auto scan = SensitiveContentFilter::sanitize(row[key].toString(),{},policy.privacyRules);
            if (scan.blocked || scan.redacted) { allowed = false; break; }
        }
        if (allowed) result.append(row);
        else restricted.insert(row["matter"].toString());
    }
    for (qsizetype i=result.size(); i>0; --i)
        if (restricted.contains(result.at(i-1).toObject()["matter"].toString())) result.removeAt(i-1);
    return result;
}

void MemoryManager::deferObservation(qint64 source, const QString &reason, bool rejected)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE cognitive_source SET attempts=attempts+1,last_error=?,"
              "rejections=rejections+?,outcome=CASE WHEN ? AND rejections>=2 THEN 'review_required' END,"
              "due=unixepoch()+min(3600,30*(1<<min(attempts,7))) "
              "WHERE id=? AND outcome IS NULL");
    q.addBindValue(reason);
    q.addBindValue(int(rejected));
    q.addBindValue(int(rejected));
    q.addBindValue(source);
    if (!q.exec()) qWarning() << "[Cognition] retry_schedule_failed source=" << source;
}

bool MemoryManager::commitObservation(qint64 source, const QJsonObject &proposal, QString *failure,
                                      const QJsonArray &historyEvidence, QJsonObject *conflict)
{
    if (conflict) *conflict = {};
    if (failure) *failure = QStringLiteral("invalid_proposal");
    const QString outcome = proposal["outcome"].toString();
    const auto changes = proposal["changes"].toArray();
    if ((outcome != "updated" && outcome != "no_change" && outcome != "needs_evidence")
        || !proposal["changes"].isArray() || (outcome == "updated") != !changes.isEmpty()
        || changes.size() > 8)
        return false;

    if (!m_db.transaction()) {
        if (failure) *failure = QStringLiteral("storage_error");
        return false;
    }
    auto abort = [this, failure](const QString &reason) {
        if (failure) *failure = reason;
        m_db.rollback();
        return false;
    };
    QSqlQuery q(m_db);
    q.prepare("SELECT input,subject FROM cognitive_source WHERE id=? AND outcome IS NULL");
    q.addBindValue(source);
    if (!q.exec()) return abort("storage_error");
    if (!q.next()) return abort("source_unavailable");
    const QString input = q.value(0).toString();
    const QString subject = q.value(1).toString();
    q.finish();
    static const QSet<QString> statuses{"confirmed","tentative","disputed","unconfirmed","cancelled","ended"};
    static const QSet<QString> kinds{"self_report","reported","inference"};
    for (qsizetype i=0; i<changes.size(); ++i) {
        const auto change = changes[i].toObject();
        QString matter = change["matter"].toString();
        const qint64 version = change["version"].toInteger(-1);
        const QString topic = change["topic"].toString().trimmed();
        const QString quote = change["quote"].toString().trimmed();
        const QString status = change["status"].toString();
        const QString kind = change["kind"].toString();
        const QString time = change["event_time"].toString();
        const auto understanding = change["understanding"].toString().trimmed();
        if (understanding.isEmpty() || understanding.size()>512 || !change["matter"].isString() || version < 0 || topic.isEmpty() || topic.size()>96
            || quote.isEmpty() || quote.size()>512 || !input.contains(quote)
            || !statuses.contains(status) || !kinds.contains(kind) || time.size()>128
            || (!time.isEmpty() && !input.contains(time)))
            return abort("invalid_change");
        if (matter.isEmpty()) {
            if (version != 0) return abort("invalid_change");
            matter = QStringLiteral("m%1.%2").arg(source).arg(i);
        } else {
            q.prepare("SELECT version,s.subject,s.id FROM matter_revision r JOIN cognitive_source s ON s.id=r.source "
                      "WHERE r.matter=? ORDER BY version DESC LIMIT 1");
            q.addBindValue(matter);
            if (!q.exec()) return abort("storage_error");
            const bool found = q.next();
            QString mismatch;
            if (!found) mismatch = "missing_matter";
            else if (q.value(1).toString()!=subject) mismatch = "subject_mismatch";
            else if (q.value(0).toLongLong()!=version) mismatch = "version_mismatch";
            else if (q.value(2).toLongLong()>=source) mismatch = "source_order";
            if (!mismatch.isEmpty()) {
                if (conflict) *conflict = {{"change_index",i},{"matter",matter},{"expected_version",version},
                    {"actual_version",found ? QJsonValue(q.value(0).toLongLong()) : QJsonValue::Null},
                    {"reason",mismatch}};
                return abort("base_mismatch");
            }
            q.finish();
        }
        q.prepare("INSERT INTO matter_revision(matter,version,source,topic,status,quote,kind,event_time,understanding) "
                  "VALUES(?,?,?,?,?,?,?,?,?)");
        for (const QVariant &value : QVariantList{matter,source,source,topic,status,quote,kind,time,understanding})
            q.addBindValue(value);
        if (!q.exec()) return abort("storage_error");
    }
    for (const auto &item : historyEvidence) {
        const auto report = item.toObject();
        q.prepare("INSERT INTO observation_history(source,turn) SELECT ?,id FROM chat_history "
                  "WHERE id=? AND subject=? AND sensitive=0 AND instr(user_input,?)=1");
        q.addBindValue(source);
        q.addBindValue(report["turn"].toInteger());
        q.addBindValue(subject);
        q.addBindValue(report["text"].toString());
        if (!q.exec()) return abort("storage_error");
        if (q.numRowsAffected()!=1) return abort("evidence_changed");
    }
    q.prepare("UPDATE cognitive_source SET outcome=?,last_error=NULL,input=CASE WHEN ?='no_change' "
              "AND NOT EXISTS(SELECT 1 FROM observation_history WHERE source=cognitive_source.id) THEN '' ELSE input END WHERE id=? AND outcome IS NULL");
    q.addBindValue(outcome);
    q.addBindValue(outcome);
    q.addBindValue(source);
    if (!q.exec() || q.numRowsAffected()!=1 || !m_db.commit()) return abort("storage_error");
    if (outcome == "updated") emit understandingChanged();
    return true;
}
