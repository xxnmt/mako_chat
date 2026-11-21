#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include <QScopeGuard>
#include <QSqlQuery>

namespace {
// Keep eligibility identical at claim, completion and subsequent conversational read.
const QString sourceRows = QStringLiteral(
    " FROM chat_history h JOIN cognitive_source s ON s.event=h.cognitive_event "
    "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input ");
const QString eligible = QStringLiteral(
    " h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
    "AND h.turn_state IN ('handed_off','quiet') AND h.turn_kind='reply' ");
}

QJsonObject MemoryManager::claimPrivateReflection(int dailyLimit, int cooldownSeconds)
{
    if (dailyLimit <= 0 || !m_db.transaction()) return {};
    auto rollback = qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    q.prepare("SELECT h.id,h.channel,h.session_id,s.input,s.reported,coalesce(r.version,0)" + sourceRows +
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
        return {{"id",id},{"version",version},{"kind","reflection"},{"owner_statement",input.left(1000)},
                {"reported_at",reported},{"excerpt_truncated",input.size()>1000}};
    }
    return {};
}

bool MemoryManager::finishPrivateReflection(qlonglong turn, int version, const QString &choice, const QString &note,
                                            const QJsonObject &contact)
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

QJsonObject MemoryManager::recentPrivateReflection(const RequestMeta &viewer) const
{
    if (viewer.subject!="user" || viewer.sensitive || !viewer.longTermMemoryEnabled
        || (viewer.channel!="pet" && viewer.channel!="qq_private")) return {};
    const MemoryReadPolicy policy;
    if (!policy.allowsSession(viewer.channel,viewer.sessionId)) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT r.turn_id,r.note,h.channel,h.session_id,s.input,c.timing,c.outcome,c.message,c.channel,c.session" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id LEFT JOIN reflection_contact c ON c.turn_id=r.turn_id WHERE " + eligible +
              " AND r.state='consider' AND (? OR (h.channel=? AND h.session_id=?)) "
              "ORDER BY r.updated_at DESC,r.turn_id DESC LIMIT 12");
    q.addBindValue(ConfigManager::instance().getMemoryPolicyConfig().agent.ownerCrossChannelPrivate);
    q.addBindValue(viewer.channel);
    q.addBindValue(viewer.sessionId);
    if (!q.exec()) return {};
    while (q.next()) {
        const auto channel=q.value(2).toString(), session=q.value(3).toString();
        const auto note=q.value(1).toString();
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
