#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include "recentcontext.h"
#include <QScopeGuard>
#include <QJsonDocument>
#include <QSqlQuery>
#include <algorithm>
#include <QSet>

namespace {
// Keep eligibility identical at claim, completion and subsequent conversational read.
const QString sourceRows = QStringLiteral(
    " FROM chat_history h JOIN cognitive_source s ON s.event=h.cognitive_event "
    "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input ");
const QString eligible = QStringLiteral(
    " h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
    "AND h.turn_state IN ('handed_off','quiet') AND h.turn_kind='reply' ");

bool visibleSession(const QString &channel,const QString &session,const QString &viewerChannel,const QString &viewerSession)
{
    if (channel==viewerChannel && session==viewerSession) return true;
    auto &cfg=ConfigManager::instance();
    return cfg.getMemoryPolicyConfig().agent.ownerCrossChannelPrivate &&
           ((channel==Channel::kPet && session==Channel::kDefaultSession) ||
            (channel==Channel::kQQPrivate && cfg.isMasterQQ(session)));
}

bool basisVisible(const QSqlDatabase &db,qint64 turn,const MemoryReadPolicy &policy,
                  const QString &viewerChannel,const QString &viewerSession)
{
    QSqlQuery q(db);
    q.prepare("SELECT h.channel,h.session_id,h.user_input,h.sensitive FROM reflection_basis b "
              "LEFT JOIN chat_history h ON h.id=b.source WHERE b.turn=?");
    q.addBindValue(turn);
    if (!q.exec()) return false;
    while(q.next())
        if (q.value(0).isNull() || q.value(3).toBool() || !visibleSession(q.value(0).toString(),q.value(1).toString(),viewerChannel,viewerSession) || !policy.allows(q.value(0).toString(),q.value(1).toString(),q.value(2).toString())) return false;
    return true;
}

// 一条"关注"对模型可见的字段。前台注入与后台 prior 使用同一构造，避免两处形状不一致。
QJsonObject buildConcernRecord(qlonglong turn,const QString &note,int version,const QString &reportedAt,
                               const QString &sourceChannel,const QString &sourceSession,
                               const QVariant &timing,const QVariant &outcome,const QVariant &message,
                               const QVariant &contactReason,const QVariant &contactChannel,
                               const QVariant &contactSession,const QString &viewerChannel,
                               const QString &viewerSession,const MemoryReadPolicy &policy,
                               bool includeSource)
{
    QJsonObject record{{"ref",QStringLiteral("reflection/%1").arg(turn)},
                       {"source_turn",turn},
                       {"own_reflection",note},
                       {"version",version},
                       {"reported_at",reportedAt}};
    if (includeSource) {
        record["source_channel"]=sourceChannel;
        record["source_session"]=sourceSession;
    }
    if (!timing.isNull()) {
        if (policy.allows(sourceChannel,sourceSession,contactReason.toString()))
            record["contact_reason"]=contactReason.toString();
        record["contact_timing"]=timing.toString();
        record["contact_outcome"]=outcome.toString();
        if ((outcome.toString()==QStringLiteral("handed_off")
             || outcome.toString()==QStringLiteral("protocol_accepted"))
            && contactChannel.toString()==viewerChannel && contactSession.toString()==viewerSession
            && policy.allows(sourceChannel,sourceSession,message.toString()))
            record["handed_off_message"]=message.toString();
    }
    return record;
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
                {"source_channel",channel},{"source_session",session},{"source_reminders",actions},
                {"reported_at",reported},{"excerpt_truncated",input.size()>1000},{"turn_outcome",handoff},{"own_reply_excerpt",ownReply.left(1200)}};
    }
    return {};
}

QJsonObject MemoryManager::reflectionHistory(qint64 turn,int version,const QJsonArray &rankedTurns,
                                             const QStringList &terms) const
{
    QSqlQuery q(m_db);
    q.prepare("SELECT h.channel,h.session_id,s.input,s.reported" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE "+eligible+
              " AND h.id=? AND r.version=? AND r.state='pending'");
    q.addBindValue(turn);
    q.addBindValue(version);
    const MemoryReadPolicy policy;
    if (!q.exec() || !q.next() || !policy.allows(q.value(0).toString(),q.value(1).toString(),q.value(2).toString()))
        return {};
    RequestMeta viewer;
    viewer.channel=q.value(0).toString();
    viewer.sessionId=q.value(1).toString();
    viewer.subject=QStringLiteral("user");
    viewer.sensitive=false;
    viewer.longTermMemoryEnabled=true;
    const auto reported=q.value(3).toString();
    const auto sourceInput=q.value(2).toString();
    q.finish();
    q.prepare("SELECT h.id,h.channel,h.session_id,h.user_input,s.reported FROM json_each(?) hit "
              "JOIN chat_history h ON h.id=hit.value JOIN cognitive_source s ON s.event=h.cognitive_event "
              "WHERE "+eligible+" AND s.input=h.user_input AND h.id<>? AND julianday(s.reported)<=julianday(?) ORDER BY hit.key LIMIT 6");
    q.addBindValue(QString::fromUtf8(QJsonDocument(rankedTurns).toJson(QJsonDocument::Compact)));
    q.addBindValue(turn);
    q.addBindValue(reported);
    if (!q.exec()) return {};
    QJsonArray rows,ids;
    while (q.next()) {
        const auto channel=q.value(1).toString(),session=q.value(2).toString(),text=q.value(3).toString();
        if (!visibleSession(channel,session,viewer.channel,viewer.sessionId) || !policy.allows(channel,session,text)) continue;
        ids.append(q.value(0).toLongLong());
        rows.append(QJsonObject{{"turn",ids.last()},{"channel",channel},{"text",text.left(512)},{"reported_at",q.value(4).toString()}});
    }
    // 旧关注的发现顺序：先看"与本次来源发言相关、且仍然开放"的关注（不再要求检索命中），
    // 再退回旧的"命中轮次上恰好挂着回顾"兜底。两处共用同一份关心，避免前台/后台各算一遍。
    QStringList queryTerms=terms;
    if (queryTerms.isEmpty())
        queryTerms=RecentContextPlanner::extractTerms(sourceInput.left(100));
    auto matched=matchedConcerns(viewer,queryTerms,sourceInput,turn,2,reported);
    QJsonObject result{{"history",rows}};
    if (!matched.isEmpty()) {
        result["prior"]=matched.first();
        result["prior_reason"]=QStringLiteral("match");
        result["concern_matches"]=matched.size();
    } else {
        const auto prior=recentPrivateReflection(viewer,ids,true);
        if (!prior.isEmpty()) {
            result["prior"]=prior;
            result["prior_reason"]=QStringLiteral("ranked");
        } else {
            result["prior_reason"]=QStringLiteral("none");
        }
    }
    return result;
}

QJsonArray MemoryManager::matchedConcerns(const RequestMeta &viewer,const QStringList &terms,
                                          const QString &queryText,qint64 excludeTurn,int limit,
                                          const QString &beforeReported) const
{
    QJsonArray result;
    if (viewer.subject!="user" || viewer.sensitive || !viewer.longTermMemoryEnabled
        || (viewer.channel!=Channel::kPet && viewer.channel!=Channel::kQQPrivate)) return result;
    const MemoryReadPolicy policy;
    if (!policy.allowsSession(viewer.channel,viewer.sessionId)) return result;
    QStringList clean;
    for (const auto &term : terms) {
        const auto value=term.trimmed().toLower();
        if (value.isEmpty() || value.size()>16 || clean.contains(value)) continue;
        clean.append(value);
        if (clean.size()>=12) break;
    }
    if (limit<=0 || (queryText.isEmpty() && clean.isEmpty())) return result;
    QSqlQuery q(m_db);
    // beforeReported 只用于"回顾某一轮时，prior 必须早于该轮"；前台注入不需要这个上界。
    // 不能把空串绑成占位参数：空 QString 会被绑成 SQL NULL，`?=''` 恒为假，会把候选全过滤掉。
    QString sql = "SELECT r.turn_id,r.note,r.version,h.channel,h.session_id,s.input,s.reported,"
                  "c.timing,c.outcome,c.reason,c.message,c.channel,c.session" + sourceRows +
                  "JOIN private_reflection r ON r.turn_id=h.id "
                  "LEFT JOIN reflection_contact c ON c.turn_id=r.turn_id WHERE " + eligible +
                  " AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)"
                  " AND h.id<>?";
    if (!beforeReported.isEmpty()) sql += " AND julianday(s.reported)<=julianday(?)";
    sql += " ORDER BY r.updated_at DESC,r.turn_id DESC LIMIT 48";
    q.prepare(sql);
    q.addBindValue(excludeTurn);
    if (!beforeReported.isEmpty()) q.addBindValue(beforeReported);
    if (!q.exec()) return result;
    struct Scored { QJsonObject record; double score; };
    QList<Scored> scored;
    while (q.next()) {
        const auto concernTurn=q.value(0).toLongLong();
        const auto note=q.value(1).toString();
        const auto channel=q.value(3).toString(),session=q.value(4).toString(),input=q.value(5).toString();
        if (!visibleSession(channel,session,viewer.channel,viewer.sessionId)) continue;
        if (!basisVisible(m_db,concernTurn,policy,viewer.channel,viewer.sessionId)) continue;
        if (!policy.allows(channel,session,input) || !policy.allows(channel,session,note)) continue;
        int hits3=0,hits2=0,noteHits=0,quoteHits=0;
        // 用关注 note 的连续片段去匹配"整条当前输入"，而不是只依赖旧抽词的尾部 16 个词，
        // 这样长消息中段的回答（例如"没有荷包蛋呀"）也能命中旧的疑问。
        // 收紧要则：命中一个 3 字连续片段，或至少两个不同的 2 字片段，避免"你/好/的"这类常见片段误命中。
        if (!queryText.isEmpty() && note.size()>=2) {
            QSet<QString> grams3;
            for (int i=0;i+3<=note.size();++i) grams3.insert(note.mid(i,3));
            for (const auto &gram : grams3)
                if (queryText.contains(gram,Qt::CaseInsensitive)) ++hits3;
            QSet<QString> grams2;
            for (int i=0;i+2<=note.size();++i) grams2.insert(note.mid(i,2));
            for (const auto &gram : grams2)
                if (queryText.contains(gram,Qt::CaseInsensitive)) ++hits2;
            noteHits=hits3*2+hits2;
        }
        for (const auto &term : clean) {
            if (note.contains(term,Qt::CaseInsensitive)) ++noteHits;
            if (input.contains(term,Qt::CaseInsensitive)) ++quoteHits;
        }
        // 只接受"关注文本与当前输入有连续重叠"的匹配：一个 3 字片段，或两个不同的 2 字片段。
        // 不接受"仅抽词里一个常见词多处同时出现"的弱匹配，避免误了结一条真实关注。
        if (queryText.isEmpty() || (hits3<1 && hits2<2)) continue;
        const double score=hits3*2.0+hits2*0.5+quoteHits*0.5;
        auto record=buildConcernRecord(concernTurn,note,q.value(2).toInt(),q.value(6).toString(),
                                       channel,session,q.value(7),q.value(8),q.value(10),q.value(9),
                                       q.value(11),q.value(12),viewer.channel,viewer.sessionId,policy,false);
        record["match_score"]=score;
        QJsonArray hitTerms;
        for (const auto &term : clean)
            if (note.contains(term,Qt::CaseInsensitive) || input.contains(term,Qt::CaseInsensitive))
                hitTerms.append(term);
        record["matched_terms"]=hitTerms;
        scored.append({record,score});
    }
    std::stable_sort(scored.begin(),scored.end(),
                     [](const Scored &a,const Scored &b){return a.score>b.score;});
    for (int i=0;i<scored.size() && result.size()<limit;++i) result.append(scored.at(i).record);
    return result;
}

QJsonObject MemoryManager::concernByHead(qint64 headTurn) const
{
    if (headTurn<=0) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT r.turn_id,r.note,r.version,h.channel,h.session_id,s.input,s.reported,"
              "c.timing,c.outcome,c.reason,c.message,c.channel,c.session" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id "
              "LEFT JOIN reflection_contact c ON c.turn_id=r.turn_id WHERE " + eligible +
              " AND h.id=? AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)");
    q.addBindValue(headTurn);
    if (!q.exec() || !q.next()) return {};
    const MemoryReadPolicy policy;
    const auto channel=q.value(3).toString(),session=q.value(4).toString();
    return buildConcernRecord(q.value(0).toLongLong(),q.value(1).toString(),q.value(2).toInt(),
                              q.value(6).toString(),channel,session,q.value(7),q.value(8),q.value(10),
                              q.value(9),q.value(11),q.value(12),channel,session,policy,true);
}

bool MemoryManager::resolveConcern(qlonglong headTurn,qlonglong byTurn,const QString &kind,
                                   const QString &evidenceRef)
{
    static const QSet<QString> kinds{QStringLiteral("answer"),QStringLiteral("contradiction"),
                                     QStringLiteral("cancelled"),QStringLiteral("dropped")};
    if (headTurn<=0 || byTurn<=0 || !kinds.contains(kind) || evidenceRef.trimmed().isEmpty()) return false;
    if (!m_db.transaction()) return false;
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    q.prepare("SELECT r.version,h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND h.id=? AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)");
    q.addBindValue(headTurn);
    if (!q.exec() || !q.next()) return false;
    const int version=q.value(0).toInt();
    const auto headChannel=q.value(1).toString(),headSession=q.value(2).toString(),headInput=q.value(3).toString();
    q.finish();
    q.prepare("SELECT channel,session_id,user_input,subject,sensitive FROM chat_history WHERE id=?");
    q.addBindValue(byTurn);
    if (!q.exec() || !q.next()) return false;
    const auto byChannel=q.value(0).toString(),bySession=q.value(1).toString();
    if (q.value(3).toString()!=QStringLiteral("user") || q.value(4).toBool()) return false;
    q.finish();
    const MemoryReadPolicy policy;
    if (!visibleSession(headChannel,headSession,byChannel,bySession)
        || !basisVisible(m_db,headTurn,policy,byChannel,bySession)
        || !policy.allows(headChannel,headSession,headInput)) return false;
    if (evidenceRef!=QStringLiteral("current_input")) {
        bool valid=false;
        if (evidenceRef.startsWith(QStringLiteral("chat/"))) {
            const auto refTurn=evidenceRef.mid(5).section(':',0,0).toLongLong();
            q.prepare("SELECT channel,session_id,user_input FROM chat_history WHERE id=? AND subject='user' AND sensitive=0");
            q.addBindValue(refTurn);
            if (q.exec() && q.next())
                valid=visibleSession(q.value(0).toString(),q.value(1).toString(),byChannel,bySession)
                      && policy.allows(q.value(0).toString(),q.value(1).toString(),q.value(2).toString());
        }
        if (!valid) return false;
    }
    // 独立的了结流水：与回顾链的 reflection_change 分开，避免同一轮次的两种修订语义共用主键。
    q.prepare("INSERT INTO concern_resolution(turn,prior_turn,action,kind) VALUES(?,?,'resolve',?)");
    q.addBindValue(byTurn);
    q.addBindValue(headTurn);
    q.addBindValue(kind);
    if (!q.exec() || q.numRowsAffected()!=1) return false;
    q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) SELECT ?,source FROM reflection_basis WHERE turn=?");
    q.addBindValue(byTurn);
    q.addBindValue(headTurn);
    if (!q.exec()) return false;
    q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) VALUES(?,?)");
    q.addBindValue(byTurn);
    q.addBindValue(headTurn);
    if (!q.exec()) return false;
    q.prepare("UPDATE private_reflection SET state='resolved',updated_at=unixepoch() "
              "WHERE turn_id=? AND version=? AND state='consider'");
    q.addBindValue(headTurn);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected()!=1) return false;
    if (!m_db.commit()) return false;
    rollback.dismiss();
    return true;
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
                                            const QJsonObject &contact,const QJsonArray &consulted,
                                            const QJsonObject &prior,const QString &priorAction)
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
    if (!priorAction.isEmpty() && (prior.isEmpty() ||
        (priorAction!="resolve" && !(priorAction=="revise" && choice=="consider")))) return false;
    if (!prior.isEmpty()) {
        const auto previous=prior["source_turn"].toInteger();
        const auto current=concernByHead(previous);
        if (previous<=0 || previous==turn || current.isEmpty()
            || current["ref"].toString()!=prior["ref"].toString()
            || current["version"].toInteger()!=prior["version"].toInteger()
            || !basisVisible(m_db,previous,policy,channel,session)
            || !visibleSession(current["source_channel"].toString(),current["source_session"].toString(),
                               channel,session)) return false;
        // The new note may rely on any evidence that supported the previous note.
        q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) SELECT ?,source FROM reflection_basis WHERE turn=?");
        q.addBindValue(turn);
        q.addBindValue(previous);
        if (!q.exec()) return false;
        q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) VALUES(?,?)");
        q.addBindValue(turn);
        q.addBindValue(previous);
        if (!q.exec()) return false;
        if (!priorAction.isEmpty()) {
            q.prepare("INSERT INTO reflection_change(turn,prior_turn,action) VALUES(?,?,?)");
            q.addBindValue(turn);
            q.addBindValue(previous);
            q.addBindValue(priorAction);
            if (!q.exec()) return false;
        }
    }
    for (const auto &value : consulted) {
        const auto row=value.toObject();
        q.prepare("SELECT user_input,channel,session_id FROM chat_history WHERE id=? AND subject='user' AND sensitive=0");
        q.addBindValue(row["turn"].toInteger());
        if (!q.exec() || !q.next()) return false;
        const auto text=q.value(0).toString();
        if (row["turn"].toInteger()==turn || !visibleSession(q.value(1).toString(),q.value(2).toString(),channel,session) ||
            text.left(512)!=row["text"].toString() || !policy.allows(q.value(1).toString(),q.value(2).toString(),text)) return false;
        q.finish();
        q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) VALUES(?,?)");
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
              "AND r.state='consider' AND c.outcome='proposed' AND NOT EXISTS "
              "(SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)");
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return {};
    const auto message=q.value(0).toString(), sourceChannel=q.value(1).toString(), sourceSession=q.value(2).toString();
    const MemoryReadPolicy policy;
    if ((!memory.agent.ownerCrossChannelPrivate && (sourceChannel!=channel || sourceSession!=session))
        || !basisVisible(m_db,turn,policy,channel,session)
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

QJsonObject MemoryManager::recentPrivateReflection(const RequestMeta &viewer, const QJsonArray &rankedTurns,bool requireRanked) const
{
    if (viewer.subject!="user" || viewer.sensitive || !viewer.longTermMemoryEnabled
        || (viewer.channel!="pet" && viewer.channel!="qq_private")) return {};
    const MemoryReadPolicy policy;
    if (!policy.allowsSession(viewer.channel,viewer.sessionId)) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT r.turn_id,r.note,h.channel,h.session_id,s.input,c.timing,c.outcome,c.message,c.channel,c.session,r.version,c.reason,s.reported" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id LEFT JOIN reflection_contact c ON c.turn_id=r.turn_id "
              "LEFT JOIN json_each(?) hit ON hit.value=h.id WHERE " + eligible +
              " AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id) "
              " AND (?=0 OR hit.key IS NOT NULL) AND (? OR (h.channel=? AND h.session_id=?)) "
              "ORDER BY hit.key IS NULL,hit.key,r.updated_at DESC,r.turn_id DESC LIMIT 12");
    q.addBindValue(QString::fromUtf8(QJsonDocument(rankedTurns).toJson(QJsonDocument::Compact)));
    q.addBindValue(int(requireRanked));
    q.addBindValue(ConfigManager::instance().getMemoryPolicyConfig().agent.ownerCrossChannelPrivate);
    q.addBindValue(viewer.channel);
    q.addBindValue(viewer.sessionId);
    if (!q.exec()) return {};
    while (q.next()) {
        const auto channel=q.value(2).toString(), session=q.value(3).toString();
        const auto note=q.value(1).toString();
        if (!visibleSession(channel,session,viewer.channel,viewer.sessionId) || !basisVisible(m_db,q.value(0).toLongLong(),policy,viewer.channel,viewer.sessionId)) continue;
        if (!policy.allows(channel,session,q.value(4).toString()) || !policy.allows(channel,session,note)) continue;
        QJsonObject record {{"ref",QString("reflection/%1").arg(q.value(0).toLongLong())},{"source_turn",q.value(0).toLongLong()},{"own_reflection",note},{"version",q.value(10).toInt()},{"reported_at",q.value(12).toString()}};
        if (!q.value(5).isNull()) {
            if (policy.allows(channel,session,q.value(11).toString())) record["contact_reason"]=q.value(11).toString();
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
