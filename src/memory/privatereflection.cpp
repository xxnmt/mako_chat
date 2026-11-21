#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include <QScopeGuard>
#include <QJsonDocument>
#include <QSqlQuery>

namespace {
// Keep eligibility identical at claim, completion and subsequent conversational read.
const QString sourceRows = QStringLiteral(
    " FROM chat_history h JOIN cognitive_source s ON s.event=h.cognitive_event "
    "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input ");
const QString eligible = QStringLiteral(
    " h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
    "AND h.turn_state IN ('handed_off','quiet') AND h.turn_kind='reply' ");

bool basisVisible(const QSqlDatabase &db,qint64 turn,const MemoryReadPolicy &policy)
{
    QSqlQuery q(db);
    q.prepare("SELECT h.channel,h.session_id,h.user_input,h.sensitive FROM reflection_basis b "
              "LEFT JOIN chat_history h ON h.id=b.source WHERE b.turn=?");
    q.addBindValue(turn);
    if (!q.exec()) return false;
    while(q.next())
        if (q.value(0).isNull() || q.value(3).toBool() || !policy.allows(q.value(0).toString(),q.value(1).toString(),q.value(2).toString())) return false;
    return true;
}
}

QJsonObject MemoryManager::claimPrivateReflection(int dailyLimit, int cooldownSeconds)
{
    if (dailyLimit <= 0 || !m_db.transaction()) return {};
    auto rollback = qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    q.prepare("SELECT h.id,h.channel,h.session_id,s.input,s.reported,coalesce(r.version,0),h.raw_reply,h.turn_state" + sourceRows +
              "LEFT JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND coalesce(h.thought,'')='' AND julianday(s.reported) BETWEEN julianday('now','-1 day') AND julianday('now') "
              "AND (r.turn_id IS NULL OR (r.state IN ('pending','deferred') AND r.eligible_at<=unixepoch())) "
              "ORDER BY h.id DESC LIMIT 24");
    if (!q.exec()) return {};
    const MemoryReadPolicy policy;
    while (q.next()) {
        const auto input = q.value(3).toString();
        if (!policy.allows(q.value(1).toString(), q.value(2).toString(), input)) continue;
        const auto id = q.value(0).toLongLong();
        const int version = q.value(5).toInt()+1;
        const auto reported = q.value(4).toString();
        const auto channel=q.value(1).toString(), session=q.value(2).toString();
        QStringList words;
        for (const auto &sentence : QJsonDocument::fromJson(q.value(6).toByteArray()).object()["sentences"].toArray())
            words.append(sentence.toObject()["zh_text"].toString());
        QString ownReply=words.join('\n');
        if (!policy.allows(q.value(1).toString(),q.value(2).toString(),ownReply)) ownReply.clear();
        const auto handoff=q.value(7).toString();
        q.finish();
        if (!chargePrivateReview(id,dailyLimit,cooldownSeconds)) return {};
        q.prepare("INSERT INTO private_reflection(turn_id,version,state,eligible_at,updated_at) "
                  "VALUES(?,?,'pending',unixepoch()+1800,unixepoch()) ON CONFLICT(turn_id) DO UPDATE SET "
                  "version=excluded.version,state='pending',eligible_at=excluded.eligible_at,updated_at=unixepoch() "
                  "WHERE private_reflection.version=excluded.version-1 AND private_reflection.state IN ('deferred','pending')");
        q.addBindValue(id);
        q.addBindValue(version);
        if (!q.exec() || q.numRowsAffected()!=1 || !m_db.commit()) return {};
        rollback.dismiss();
        QJsonArray actions;
        q.prepare("SELECT due,state,message FROM reminder WHERE turn=? ORDER BY due LIMIT 32");
        q.addBindValue(id);
        if (q.exec()) while (q.next()) {
            const auto message=q.value(2).toString();
            if (policy.allows(channel,session,message))
                actions.append(QJsonObject{{"due_at",QDateTime::fromSecsSinceEpoch(q.value(0).toLongLong()).toLocalTime().toString(Qt::ISODate)},
                    {"state",q.value(1).toString()},{"message",message}});
        }
        return {{"id",id},{"version",version},{"kind","reflection"},{"owner_statement",input.left(1000)},
                {"source_channel",channel},{"source_reminders",actions},
                {"reported_at",reported},{"excerpt_truncated",input.size()>1000},{"turn_outcome",handoff},{"own_reply_excerpt",ownReply.left(1200)}};
    }
    return {};
}

bool MemoryManager::consultPrivateReflection(qint64 turn,int version,const QStringList &terms,QJsonArray &results) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE "+eligible+" AND h.id=? AND r.version=? AND r.state='pending'");
    q.addBindValue(turn); q.addBindValue(version);
    if (!q.exec() || !q.next()) return {};
    const QString channel=q.value(0).toString(), session=q.value(1).toString();
    const MemoryReadPolicy policy;
    if (!policy.allows(channel,session,q.value(2).toString())) return {};
    const auto rows=historicalReports("user",channel,session,QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),terms,true);
    results={};
    for (const auto &value : rows) {
        auto row=value.toObject();
        const auto text=row["text"].toString();
        if (row["turn"].toInteger()==turn || !policy.allows(channel,session,text)) continue;
        row["text"]=text.left(512);
        results.append(row);
    }
    return true;
}

bool MemoryManager::finishPrivateReflection(qlonglong turn, int version, const QString &choice, const QString &note,
                                            const QJsonObject &contact,const QJsonArray &consulted)
{
    if (choice != "rest" && choice != "consider") return false;
    if (choice == "consider" && (note.trimmed().isEmpty() || note.size()>240)) return false;
    if (!m_db.transaction()) return false;
    auto rollback = qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    q.prepare("SELECT h.channel,h.session_id,s.input" + sourceRows + "WHERE " + eligible + " AND h.id=?");
    q.addBindValue(turn);
    if (!q.exec() || !q.next()) return false;
    const MemoryReadPolicy policy;
    const auto channel = q.value(0).toString(), session = q.value(1).toString();
    if (!policy.allows(channel,session,q.value(2).toString()) || !policy.allows(channel,session,note)) return false;
    q.finish();
    for (const auto &value : consulted) {
        const auto row=value.toObject();
        q.prepare("SELECT user_input FROM chat_history WHERE id=? AND subject='user' AND sensitive=0 AND channel=? AND session_id=?");
        q.addBindValue(row["turn"].toInteger()); q.addBindValue(channel); q.addBindValue(session);
        if (!q.exec() || !q.next()) return false;
        const auto text=q.value(0).toString();
        if (text.left(512)!=row["text"].toString() || !policy.allows(channel,session,text)) return false;
        q.finish();
        q.prepare("INSERT INTO reflection_basis(turn,source) VALUES(?,?)");
        q.addBindValue(turn); q.addBindValue(row["turn"].toInteger());
        if (!q.exec()) return false;
    }
    q.prepare("UPDATE private_reflection SET state=?,note=?,updated_at=unixepoch() "
              "WHERE turn_id=? AND version=? AND state='pending'");
    q.addBindValue(choice);
    q.addBindValue(choice=="consider" ? note.trimmed() : QString{});
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected()!=1) return false;
    if (choice=="consider" && !contact.isEmpty() && ConfigManager::instance().getProactiveConfig().enabled) {
        const auto timing = contact["timing"].toString();
        const auto reason = contact["reason"].toString().trimmed();
        const auto message = contact["message"].toString().trimmed();
        if (timing!="private" && timing!="next_chat" && timing!="now") return false;
        if (reason.isEmpty() || reason.size()>240 || !policy.allows(channel,session,reason)) return false;
        if (timing=="now" && (message.isEmpty() || message.size()>300 || !policy.allows(channel,session,message))) return false;
        q.prepare("INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome) VALUES(?,?,?,?,?,?)");
        q.addBindValue(turn);
        q.addBindValue(version);
        q.addBindValue(timing);
        q.addBindValue(reason);
        q.addBindValue(timing=="now" ? message : QString());
        q.addBindValue(timing=="now" ? "proposed" : "retained");
        if (!q.exec()) return false;
    }
    if (!m_db.commit()) return false;
    rollback.dismiss();
    return true;
}

QString MemoryManager::claimReflectionContact(qlonglong turn, int version, const QString &channel, const QString &session)
{
    auto &cfg = ConfigManager::instance();
    const auto opts = cfg.getProactiveConfig();
    const auto memory = cfg.getMemoryPolicyConfig();
    if (!opts.enabled || !opts.privateWriting || !memory.longTermMemoryEnabled
        || !opts.targets.contains(qMakePair(channel,session))) return {};
    if (!(channel==Channel::kPet && session==Channel::kDefaultSession)
        && !(channel==Channel::kQQPrivate && cfg.isMasterQQ(session))) return {};
    if (!m_db.transaction()) return {};
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    q.prepare("SELECT c.message,h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id JOIN reflection_contact c ON c.turn_id=r.turn_id "
              "WHERE " + eligible + " AND c.turn_id=? AND c.version=? AND r.version=c.version "
              "AND r.state='consider' AND c.outcome='proposed'");
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return {};
    const auto message=q.value(0).toString(), sourceChannel=q.value(1).toString(), sourceSession=q.value(2).toString();
    const MemoryReadPolicy policy;
    if ((!memory.agent.ownerCrossChannelPrivate && (sourceChannel!=channel || sourceSession!=session))
        || !basisVisible(m_db,turn,policy)
        || !policy.allows(sourceChannel,sourceSession,q.value(3).toString()) || !policy.allows(channel,session,message)) return {};
    q.finish();
    q.prepare("UPDATE reflection_contact SET outcome='dispatching',channel=?,session=? "
              "WHERE turn_id=? AND version=? AND outcome='proposed'");
    q.addBindValue(channel);
    q.addBindValue(session);
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected()!=1 || !m_db.commit()) return {};
    rollback.dismiss();
    return message;
}

bool MemoryManager::recordContactOutcome(qlonglong turn, int version, const QString &outcome)
{
    if (outcome!="handed_off" && outcome!="rejected" && outcome!="withheld"
        && outcome!="unknown" && outcome!="protocol_accepted") return false;
    QSqlQuery q(m_db);
    q.prepare("UPDATE reflection_contact SET outcome=? WHERE turn_id=? AND version=? AND (outcome=? OR (outcome='handed_off' AND ?))");
    q.addBindValue(outcome);
    q.addBindValue(turn);
    q.addBindValue(version);
    q.addBindValue(outcome=="withheld" ? "proposed" : "dispatching");
    q.addBindValue(outcome=="unknown" || outcome=="rejected" || outcome=="protocol_accepted");
    return q.exec() && q.numRowsAffected()==1;
}

bool MemoryManager::deferPrivateReflection(qlonglong turn, int version)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE private_reflection SET state='deferred',eligible_at=unixepoch(),updated_at=unixepoch() "
              "WHERE turn_id=? AND version=? AND state='pending'");
    q.addBindValue(turn);
    q.addBindValue(version);
    return q.exec() && q.numRowsAffected()==1;
}

QJsonObject MemoryManager::recentPrivateReflection(const RequestMeta &viewer, const QJsonArray &rankedTurns) const
{
    if (viewer.subject!="user" || viewer.sensitive || !viewer.longTermMemoryEnabled
        || (viewer.channel!="pet" && viewer.channel!="qq_private")) return {};
    const MemoryReadPolicy policy;
    if (!policy.allowsSession(viewer.channel,viewer.sessionId)) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT r.turn_id,r.note,h.channel,h.session_id,s.input,c.timing,c.outcome,c.message,c.channel,c.session" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id LEFT JOIN reflection_contact c ON c.turn_id=r.turn_id "
              "LEFT JOIN json_each(?) hit ON hit.value=h.id WHERE " + eligible +
              " AND r.state='consider' AND (? OR (h.channel=? AND h.session_id=?)) "
              "ORDER BY hit.key IS NULL,hit.key,r.updated_at DESC,r.turn_id DESC LIMIT 12");
    q.addBindValue(QString::fromUtf8(QJsonDocument(rankedTurns).toJson(QJsonDocument::Compact)));
    q.addBindValue(ConfigManager::instance().getMemoryPolicyConfig().agent.ownerCrossChannelPrivate);
    q.addBindValue(viewer.channel);
    q.addBindValue(viewer.sessionId);
    if (!q.exec()) return {};
    while (q.next()) {
        const auto channel=q.value(2).toString(), session=q.value(3).toString();
        const auto note=q.value(1).toString();
        if (!basisVisible(m_db,q.value(0).toLongLong(),policy)) continue;
        if (!policy.allows(channel,session,q.value(4).toString()) || !policy.allows(channel,session,note)) continue;
        QJsonObject record {{"ref",QString("reflection/%1").arg(q.value(0).toLongLong())},{"source_turn",q.value(0).toLongLong()},{"own_reflection",note}};
        if (!q.value(5).isNull()) {
            record["contact_timing"]=q.value(5).toString();
            record["contact_outcome"]=q.value(6).toString();
            if ((q.value(6).toString()=="handed_off" || q.value(6).toString()=="protocol_accepted") && q.value(8).toString()==viewer.channel
                && q.value(9).toString()==viewer.sessionId && policy.allows(channel,session,q.value(7).toString()))
                record["handed_off_message"]=q.value(7).toString();
        }
        return record;
    }
    return {};
}

namespace {
bool allowedReminder(const QString &channel,const QString &session)
{
    auto &cfg=ConfigManager::instance();
    const auto opts=cfg.getProactiveConfig();
    return opts.enabled && opts.targets.contains(qMakePair(channel,session))
        && cfg.getMemoryPolicyConfig().longTermMemoryEnabled && !cfg.isBlindZone(channel,session)
        && ((channel==Channel::kPet && session==Channel::kDefaultSession)
            || (channel==Channel::kQQPrivate && cfg.isMasterQQ(session)));
}
}

QJsonArray MemoryManager::pendingReminders(const RequestMeta &v) const
{
    QJsonArray result;
    if (v.subject!="user" || v.sensitive || !v.longTermMemoryEnabled || !allowedReminder(v.channel,v.sessionId)) return result;
    QSqlQuery q(m_db);
    q.prepare("SELECT id,due,message FROM reminder WHERE channel=? AND session=? AND state='pending' AND expires>=unixepoch() ORDER BY due LIMIT 32");
    q.addBindValue(v.channel);
    q.addBindValue(v.sessionId);
    if (q.exec()) while(q.next()) {
        if (MemoryReadPolicy().allows(v.channel,v.sessionId,q.value(2).toString()))
            result.append(QJsonObject{{"id",q.value(0).toLongLong()},{"at",QDateTime::fromSecsSinceEpoch(q.value(1).toLongLong()).toLocalTime().toString(Qt::ISODate)},{"message",q.value(2).toString()}});
    }
    return result;
}

bool MemoryManager::applyReminders(qint64 turn,const RequestMeta &v,const QJsonObject &plan)
{
    if (!turn || v.subject!="user" || v.sensitive || !v.longTermMemoryEnabled || !allowedReminder(v.channel,v.sessionId)) return false;
    const auto add=plan["add"].toArray(), cancel=plan["cancel"].toArray();
    if ((add.isEmpty() && cancel.isEmpty()) || add.size()+cancel.size()>32 || !m_db.transaction()) return false;
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM chat_history WHERE id=? AND cognitive_event=? AND subject='user' AND sensitive=0 AND channel=? AND session_id=? AND turn_state='received'");
    q.addBindValue(turn); q.addBindValue(v.eventId); q.addBindValue(v.channel); q.addBindValue(v.sessionId);
    if (!q.exec() || !q.next()) return false;
    q.finish();
    for (const auto &id : cancel) {
        q.prepare("UPDATE reminder SET state='cancelled' WHERE id=? AND channel=? AND session=? AND state='pending'");
        q.addBindValue(id.toInteger());
        q.addBindValue(v.channel);
        q.addBindValue(v.sessionId);
        if (!q.exec() || q.numRowsAffected()!=1) return false;
    }
    for (const auto &item : add) {
        const auto row=item.toObject();
        const auto due=QDateTime::fromString(row["at"].toString(),Qt::ISODate);
        const auto end=QDateTime::fromString(row["until"].toString(),Qt::ISODate);
        const auto message=row["message"].toString().trimmed();
        if (!due.isValid() || !end.isValid() || due<=QDateTime::currentDateTime() || end<=due
            || message.isEmpty() || message.size()>500 || !MemoryReadPolicy().allows(v.channel,v.sessionId,message)) return false;
        q.prepare("INSERT INTO reminder(turn,channel,session,due,expires,message) VALUES(?,?,?,?,?,?)");
        q.addBindValue(turn); q.addBindValue(v.channel); q.addBindValue(v.sessionId);
        q.addBindValue(due.toSecsSinceEpoch()); q.addBindValue(end.toSecsSinceEpoch()); q.addBindValue(message);
        if (!q.exec()) return false;
    }
    if (!m_db.commit()) return false;
    rollback.dismiss();
    return true;
}

QJsonObject MemoryManager::claimDueReminder(const QString &channel,const QString &session)
{
    if (!allowedReminder(channel,session) || !m_db.transaction()) return {};
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    if (!q.exec("UPDATE reminder SET state='expired' WHERE state='pending' AND expires<unixepoch()")) return {};
    q.prepare("SELECT id,message FROM reminder WHERE state='pending' AND channel=? AND session=? AND due<=unixepoch() ORDER BY due,id LIMIT 1");
    q.addBindValue(channel); q.addBindValue(session);
    if (!q.exec()) return {};
    if (!q.next()) {q.finish(); if (m_db.commit()) rollback.dismiss(); return {};}
    const auto id=q.value(0).toLongLong();
    const auto message=q.value(1).toString();
    q.finish();
    const bool allowed=MemoryReadPolicy().allows(channel,session,message);
    q.prepare("UPDATE reminder SET state=? WHERE id=? AND state='pending'");
    q.addBindValue(allowed ? "dispatching" : "withheld"); q.addBindValue(id);
    if (!q.exec() || q.numRowsAffected()!=1 || !m_db.commit()) return {};
    rollback.dismiss();
    return allowed ? QJsonObject{{"id",id},{"message",message}} : QJsonObject{};
}

void MemoryManager::reminderOutcome(qint64 id,const QString &state)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE reminder SET state=? WHERE id=? AND (state='dispatching' OR (state='handed_off' AND ?))");
    q.addBindValue(state); q.addBindValue(id); q.addBindValue(state!="handed_off");
    if (!q.exec()) qWarning()<<"[Reminder] outcome write failed"<<id;
}
