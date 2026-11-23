#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include "recentcontext.h"
#include <QScopeGuard>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QHash>
#include <algorithm>
#include <cmath>
#include <QSet>

namespace {
// Keep eligibility identical at claim, completion and subsequent conversational read.
const QString sourceRows = QStringLiteral(
    " FROM chat_history h JOIN cognitive_source s ON s.event=h.cognitive_event "
    "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject AND s.input=h.user_input ");
const QString eligible = QStringLiteral(
    " h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
    "AND h.turn_state IN ('handed_off','quiet') AND h.turn_kind='reply' ");

// v23 才有的关注列。迁移失败或旧库回退重放时可能读不到，此时降级为空值而不是整条查询失败。
bool concernColumnsReady(const QSqlDatabase &db)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(private_reflection)"))) return false;
    while (q.next())
        if (q.value(1).toString()==QStringLiteral("topic")) return true;
    return false;
}

QString isoFromEpoch(qint64 secs)
{
    return QDateTime::fromSecsSinceEpoch(secs).toUTC().toString(Qt::ISODateWithMs);
}

// 关注到期时间：0 表示不按时间再检查（只在新证据或显式唤醒时再看），落库为 0 让到期领取过滤掉。
qint64 nextCheckAfter(int minutes)
{
    return minutes>0 ? minutes*60 : 0;
}

bool visibleSession(const QString &channel,const QString &session,const QString &viewerChannel,const QString &viewerSession)
{
    if (channel==viewerChannel && session==viewerSession) return true;
    auto &cfg=ConfigManager::instance();
    return cfg.getMemoryPolicyConfig().agent.ownerCrossChannelPrivate &&
           ((channel==Channel::kPet && session==Channel::kDefaultSession) ||
            (channel==Channel::kQQPrivate && cfg.isMasterQQ(session)));
}

// 统一投递流水（delivery_ledger）的幂等写入：同一 kind+ref 只有一行，
// 重复回执只累加失败尝试并刷新状态。流水只是可观测性，失败不得阻断主状态机。
void noteDelivery(const QSqlDatabase &db,const QString &kind,const QString &ref,
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
    if (!q.exec()) qWarning()<<"[Delivery] ledger 写入失败"<<kind<<ref<<state<<q.lastError();
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
                               const QString &topic,const QString &watch,
                               const QVariant &rootTurn,const QVariant &nextCheckAt,
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
    if (!topic.isEmpty()) record["topic"]=topic;
    if (!watch.isEmpty()) record["watch"]=watch;
    const auto root=rootTurn.toLongLong();
    if (root>0) {
        // 关注可能被后续回顾修订：当前行（head）用 reflection/<turn> 精确定位，
        // 稳定身份 root_ref 另给 "concern/<根轮次>"。前台写回按 head 解析，
        // 因此 ref 保持旧约定，避免跨层解析出现两种格式。
        record["root_ref"]=QStringLiteral("concern/%1").arg(root);
        record["root_turn"]=root;
    }
    const auto checkAt=nextCheckAt.toLongLong();
    if (checkAt>0) record["next_check_at"]=isoFromEpoch(checkAt);
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
        q.prepare("INSERT INTO private_reflection(turn_id,version,state,eligible_at,updated_at,claimed_by,root_turn) "
                  "VALUES(?,?,'pending',unixepoch()+1800,unixepoch(),'source',?) ON CONFLICT(turn_id) DO UPDATE SET "
                  "version=excluded.version,state='pending',eligible_at=excluded.eligible_at,"
                  "claimed_by=excluded.claimed_by,updated_at=unixepoch() "
                  "WHERE private_reflection.version=excluded.version-1 AND private_reflection.state IN ('deferred','pending')");
        q.addBindValue(id);
        q.addBindValue(version);
        q.addBindValue(id);
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
    // 旧实现的"抽词 + note 连续片段"会被几乎每条 note 都含有的称呼（如"欧尼酱"）带偏；
    // 现在只按片段命中的文档频率（DF）打分，terms 不再参与门槛，保留签名兼容既有调用。
    Q_UNUSED(terms);
    QJsonArray result;
    if (viewer.subject!="user" || viewer.sensitive || !viewer.longTermMemoryEnabled
        || (viewer.channel!=Channel::kPet && viewer.channel!=Channel::kQQPrivate)) return result;
    const MemoryReadPolicy policy;
    if (!policy.allowsSession(viewer.channel,viewer.sessionId)) return result;
    if (limit<=0 || queryText.trimmed().isEmpty()) return result;
    // v23 的结构化描述列；读不到时用 NULL 占位，保持列下标稳定。
    const QString extra=concernColumnsReady(m_db)
        ? QStringLiteral("r.topic,r.watch,r.root_turn,r.next_check_at")
        : QStringLiteral("NULL,NULL,NULL,NULL");
    QSqlQuery q(m_db);
    // beforeReported 只用于"回顾某一轮时，prior 必须早于该轮"；前台注入不需要这个上界。
    // 不能把空串绑成占位参数：空 QString 会被绑成 SQL NULL，`?=''` 恒为假，会把候选全过滤掉。
    QString sql = "SELECT r.turn_id,r.note,r.version,h.channel,h.session_id,s.input,s.reported,"
                  "c.timing,c.outcome,c.reason,c.message,c.channel,c.session," + extra + sourceRows +
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
    // 第一遍：只收集通过可见性/敏感/policy/basisVisible/beforeReported 的候选，保留原始行字段。
    struct Candidate {
        qlonglong turn; int version;
        QString note,topic,watch,channel,session,text;
        QString reportedAt;
        QVariant timing,outcome,message,contactReason,contactChannel,contactSession;
        QVariant rootTurn,nextCheckAt;
    };
    QList<Candidate> candidates;
    while (q.next()) {
        Candidate c;
        c.turn=q.value(0).toLongLong();
        c.note=q.value(1).toString();
        c.version=q.value(2).toInt();
        c.channel=q.value(3).toString();
        c.session=q.value(4).toString();
        c.reportedAt=q.value(6).toString();
        c.timing=q.value(7);
        c.outcome=q.value(8);
        c.contactReason=q.value(9);
        c.message=q.value(10);
        c.contactChannel=q.value(11);
        c.contactSession=q.value(12);
        c.topic=q.value(13).toString();
        c.watch=q.value(14).toString();
        c.rootTurn=q.value(15);
        c.nextCheckAt=q.value(16);
        if (!visibleSession(c.channel,c.session,viewer.channel,viewer.sessionId)) continue;
        if (!basisVisible(m_db,c.turn,policy,viewer.channel,viewer.sessionId)) continue;
        if (!policy.allows(c.channel,c.session,q.value(5).toString())
            || !policy.allows(c.channel,c.session,c.note)) continue;
        // note 几乎都带称呼；结构化描述（topic/watch）也拼进来，让"在意的事"参与匹配。
        // 用换行分隔，避免把两个字段的边界当作连续片段。
        c.text=c.note;
        if (!c.topic.isEmpty()) c.text+='\n'+c.topic;
        if (!c.watch.isEmpty()) c.text+='\n'+c.watch;
        candidates.append(c);
    }
    const int corpus=candidates.size();
    if (corpus<=0) return result;
    // 第二遍：语料内统计每个 2/3 字片段的文档频率（同一文档内去重）。
    QHash<QString,int> df;
    QList<QSet<QString>> grams3(corpus),grams2(corpus);
    for (int i=0;i<corpus;++i) {
        const auto &text=candidates.at(i).text;
        QSet<QString> three,two;
        for (int p=0;p+3<=text.size();++p) three.insert(text.mid(p,3));
        for (int p=0;p+2<=text.size();++p) two.insert(text.mid(p,2));
        for (const auto &gram : three) ++df[gram];
        for (const auto &gram : two) ++df[gram];
        grams3[i]=three;
        grams2[i]=two;
    }
    // 片段权重：命中 queryText 且不是"语料里过半文档都含"的常见片段才计数。
    // 返回分数对 DF 取对数：越少见的片段越有区分度；2 字片段权重减半。
    const auto gramWeight=[&](const QString &gram,bool three) {
        if (!queryText.contains(gram,Qt::CaseInsensitive)) return 0.0;
        const int count=df.value(gram);
        if (corpus>1 && count>1 && count*2>corpus) return 0.0;
        const double base=std::log((corpus+1.0)/(count+1.0))+0.5;
        return three ? base : base*0.5;
    };
    struct Scored { QJsonObject record; double score; };
    QList<Scored> scored;
    for (int i=0;i<corpus;++i) {
        int hits3=0,hits2=0;
        double score=0.0;
        QJsonArray matchedTerms;
        for (const auto &gram : grams3.at(i)) {
            const double weight=gramWeight(gram,true);
            if (weight<=0.0) continue;
            ++hits3;
            score+=weight;
            if (matchedTerms.size()<12) matchedTerms.append(gram);
        }
        for (const auto &gram : grams2.at(i)) {
            const double weight=gramWeight(gram,false);
            if (weight<=0.0) continue;
            ++hits2;
            score+=weight;
            if (matchedTerms.size()<12) matchedTerms.append(gram);
        }
        // 门槛：一个 3 字片段，或至少两个不同的 2 字片段。避免常见称呼/虚词把无关关注带出来。
        if (hits3<1 && hits2<2) continue;
        const auto &c=candidates.at(i);
        auto record=buildConcernRecord(c.turn,c.note,c.version,c.reportedAt,c.topic,c.watch,
                                       c.rootTurn,c.nextCheckAt,c.channel,c.session,
                                       c.timing,c.outcome,c.message,c.contactReason,
                                       c.contactChannel,c.contactSession,
                                       viewer.channel,viewer.sessionId,policy,false);
        record["match_score"]=score;
        record["matched_terms"]=matchedTerms;
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
    const QString extra=concernColumnsReady(m_db)
        ? QStringLiteral("r.topic,r.watch,r.root_turn,r.next_check_at")
        : QStringLiteral("NULL,NULL,NULL,NULL");
    QSqlQuery q(m_db);
    q.prepare("SELECT r.turn_id,r.note,r.version,h.channel,h.session_id,s.input,s.reported,"
              "c.timing,c.outcome,c.reason,c.message,c.channel,c.session," + extra + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id "
              "LEFT JOIN reflection_contact c ON c.turn_id=r.turn_id WHERE " + eligible +
              " AND h.id=? AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)");
    q.addBindValue(headTurn);
    if (!q.exec() || !q.next()) return {};
    const MemoryReadPolicy policy;
    const auto channel=q.value(3).toString(),session=q.value(4).toString();
    return buildConcernRecord(q.value(0).toLongLong(),q.value(1).toString(),q.value(2).toInt(),
                              q.value(6).toString(),q.value(13).toString(),q.value(14).toString(),
                              q.value(15),q.value(16),
                              channel,session,q.value(7),q.value(8),q.value(10),
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
    q.prepare("UPDATE private_reflection SET state='resolved',resolved_by=?,resolved_kind=?,updated_at=unixepoch() "
              "WHERE turn_id=? AND version=? AND state='consider'");
    q.addBindValue(byTurn);
    q.addBindValue(kind);
    q.addBindValue(headTurn);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected()!=1) return false;
    if (!m_db.commit()) return false;
    rollback.dismiss();
    return true;
}

QJsonObject MemoryManager::claimDueConcern(int dailyLimit,int cooldownSeconds)
{
    if (dailyLimit<=0 || !m_db.transaction()) return {};
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    // 保留期只约束"按时间到期"的回顾；新证据命中走 matchedConcerns，不受这里影响。
    const int retentionDays=qBound(1,
        ConfigManager::instance().getProactiveConfig().concernRetentionDays,365);
    QSqlQuery q(m_db);
    // 到期但已超出保留期的关注不再领取；单独记一条日志，便于解释"到期却没被唤醒"。
    {
        QSqlQuery expired(m_db);
        expired.prepare(QStringLiteral(
            "SELECT r.turn_id") + sourceRows +
            "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
            " AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)"
            " AND r.next_check_at>0 AND r.next_check_at<=unixepoch()"
            " AND julianday(s.reported)<julianday('now','-" + QString::number(retentionDays) + " day')"
            " ORDER BY r.next_check_at,r.turn_id LIMIT 24");
        if (expired.exec())
            while (expired.next())
                qInfo()<<"[Concern] expired_by_retention turn="<<expired.value(0).toLongLong();
    }
    // 只领"按时间到期"的开放关注；被回顾链修订过的旧行不再作为 head。
    q.prepare("SELECT r.turn_id,r.version,r.note,h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)"
              " AND r.next_check_at>0 AND r.next_check_at<=unixepoch()"
              " AND julianday(s.reported)>=julianday('now','-" + QString::number(retentionDays) + " day')"
              " ORDER BY r.next_check_at,r.turn_id LIMIT 24");
    if (!q.exec()) return {};
    struct Due {
        qlonglong turn; int version;
        QString note,channel,session,input;
    };
    QList<Due> due;
    while (q.next()) {
        const auto channel=q.value(3).toString();
        // 到期回顾只处理主人自己的私聊来源；其余渠道的关注不进入私下回顾。
        if (channel!=Channel::kPet && channel!=Channel::kQQPrivate) continue;
        due.append({q.value(0).toLongLong(),q.value(1).toInt(),q.value(2).toString(),
                    channel,q.value(4).toString(),q.value(5).toString()});
    }
    q.finish();
    const MemoryReadPolicy policy;
    for (const auto &item : due) {
        if (!policy.allows(item.channel,item.session,item.input)
            || !policy.allows(item.channel,item.session,item.note)) continue;
        // 到期回顾与新来源回顾共用同一份每日预算与冷却；预算用尽时本轮不再领取。
        if (!chargePrivateReview(item.turn,dailyLimit,cooldownSeconds)) return {};
        const int nextVersion=item.version+1;
        q.prepare("UPDATE private_reflection SET state='pending',claimed_by='due',version=?,"
                  "eligible_at=unixepoch()+1800 WHERE turn_id=? AND version=? AND state='consider'");
        q.addBindValue(nextVersion);
        q.addBindValue(item.turn);
        q.addBindValue(item.version);
        // 必须精确命中一行：并发/迟到的领取不能把已变化的关注再取走。
        if (!q.exec() || q.numRowsAffected()!=1 || !m_db.commit()) return {};
        rollback.dismiss();
        qInfo()<<"[Concern] due claimed turn="<<item.turn<<"version="<<nextVersion;
        return concernReviewContext(item.turn,nextVersion);
    }
    return {};
}

QJsonObject MemoryManager::concernReviewContext(qint64 turn,int version) const
{
    if (turn<=0 || version<=0) return {};
    QSqlQuery q(m_db);
    q.prepare("SELECT r.note,r.version,r.topic,r.watch,r.root_turn,r.matter,r.next_check_at,"
              "h.channel,h.session_id,s.input,s.reported" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND h.id=? AND r.version=? AND r.state='pending'");
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return {};
    const auto note=q.value(0).toString();
    const auto topic=q.value(2).toString();
    const auto watch=q.value(3).toString();
    const auto root=q.value(4).toLongLong();
    const auto matterName=q.value(5).toString();
    const auto nextCheck=q.value(6).toLongLong();
    const auto channel=q.value(7).toString();
    const auto session=q.value(8).toString();
    const auto input=q.value(9).toString();
    const auto reported=q.value(10).toString();
    const MemoryReadPolicy policy;
    if (!policy.allows(channel,session,input) || !policy.allows(channel,session,note)) return {};
    q.finish();
    // 之前留下的联系记录：让模型知道已经留过什么话，避免重复打扰。
    QJsonArray contacts;
    QSqlQuery contactQuery(m_db);
    contactQuery.prepare("SELECT timing,outcome,reason,message,channel,session FROM reflection_contact "
                         "WHERE turn_id=? ORDER BY rowid");
    contactQuery.addBindValue(turn);
    if (contactQuery.exec()) while (contactQuery.next()) {
        const auto contactChannel=contactQuery.value(4).toString();
        const auto contactSession=contactQuery.value(5).toString();
        const auto scopeChannel=contactChannel.isEmpty() ? channel : contactChannel;
        const auto scopeSession=contactSession.isEmpty() ? session : contactSession;
        QJsonObject contact{{"timing",contactQuery.value(0).toString()},
                            {"outcome",contactQuery.value(1).toString()},
                            {"channel",contactChannel},{"session",contactSession}};
        const auto reason=contactQuery.value(2).toString();
        if (!reason.isEmpty() && policy.allows(scopeChannel,scopeSession,reason)) contact["reason"]=reason;
        const auto message=contactQuery.value(3).toString();
        if (!message.isEmpty() && policy.allows(scopeChannel,scopeSession,message)) contact["message"]=message;
        contacts.append(contact);
    }
    // 关注来源之后主人说过的话：到期回顾不带新消息，必须从这里判断"事情有没有下落"。
    QJsonArray since;
    QSqlQuery sinceQuery(m_db);
    sinceQuery.prepare("SELECT h.id,h.channel,h.session_id,h.user_input,"
                       "coalesce(s.reported,h.timestamp) FROM chat_history h "
                       "LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
                       "AND s.channel=h.channel AND s.session=h.session_id "
                       "AND s.subject=h.subject AND s.input=h.user_input "
                       "WHERE h.subject='user' AND h.sensitive=0 AND h.channel IN ('pet','qq_private') "
                       "AND h.user_input<>'' AND h.id<>? "
                       "AND julianday(coalesce(s.reported,h.timestamp))>julianday(?) "
                       "ORDER BY h.id LIMIT 64");
    sinceQuery.addBindValue(turn);
    sinceQuery.addBindValue(reported);
    if (sinceQuery.exec()) while (sinceQuery.next()) {
        const auto sinceChannel=sinceQuery.value(1).toString();
        const auto sinceSession=sinceQuery.value(2).toString();
        const auto text=sinceQuery.value(3).toString();
        if (!visibleSession(sinceChannel,sinceSession,channel,session)
            || !policy.allows(sinceChannel,sinceSession,text)) continue;
        since.append(QJsonObject{{"turn",sinceQuery.value(0).toLongLong()},
                                 {"text",text.left(512)},
                                 {"reported_at",sinceQuery.value(4).toString()}});
        if (since.size()>=6) break;
    }
    // 相关事项的最新认识：只用最新一条 revision，历史版本由 matter 表自己维护。
    QJsonObject matter;
    if (!matterName.isEmpty()) {
        QSqlQuery matterQuery(m_db);
        matterQuery.prepare("SELECT r.topic,r.status,r.understanding,s.reported FROM matter_revision r "
                            "JOIN cognitive_source s ON s.id=r.source WHERE r.matter=? "
                            "ORDER BY r.version DESC LIMIT 1");
        matterQuery.addBindValue(matterName);
        if (matterQuery.exec() && matterQuery.next())
            matter=QJsonObject{{"topic",matterQuery.value(0).toString()},
                               {"status",matterQuery.value(1).toString()},
                               {"understanding",matterQuery.value(2).toString()},
                               {"reported",matterQuery.value(3).toString()}};
    }
    QJsonObject concernRecord{{"ref",QStringLiteral("concern/%1").arg(root>0 ? root : turn)},
                              {"head",QStringLiteral("reflection/%1").arg(turn)},
                              {"topic",topic},
                              {"watch",watch},
                              {"own_reflection",note},
                              {"reported_at",reported}};
    if (nextCheck>0) concernRecord["next_check_at"]=isoFromEpoch(nextCheck);
    if (!matterName.isEmpty()) concernRecord["matter"]=matterName;
    QJsonObject result{{"id",turn},{"version",version},{"kind","concern"},
                       {"concern",concernRecord},{"since",since},{"contacts",contacts}};
    if (!matter.isEmpty()) result["matter"]=matter;
    return result;
}

bool MemoryManager::finishConcernReview(qlonglong turn,int version,const QJsonObject &proposal)
{
    static const QSet<QString> actions{QStringLiteral("keep"),QStringLiteral("revise"),
                                       QStringLiteral("resolve"),QStringLiteral("drop"),
                                       QStringLiteral("contact")};
    const auto action=proposal["concern_action"].toString();
    if (!actions.contains(action)) return false;
    const auto note=proposal["note"].toString().trimmed();
    if (note.size()>240) return false;
    // 模型没给 next_check_after_minutes 时用配置的默认间隔；显式 0 仍表示"只在新证据时"。
    const int concernDefaultMinutes =
        ConfigManager::instance().getProactiveConfig().concernDefaultCheckMinutes;
    const int minutes=qBound(0,proposal["forget"].toObject()["next_check_after_minutes"]
                                    .toInt(concernDefaultMinutes),10080);
    if (!m_db.transaction()) return false;
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    // 与领取时一致的来源校验：只有仍处于 pending 且 version 匹配的领取才能落库。
    q.prepare("SELECT h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND h.id=? AND r.version=? AND r.state='pending'");
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return false;
    const auto channel=q.value(0).toString();
    const auto session=q.value(1).toString();
    const MemoryReadPolicy policy;
    if (!policy.allows(channel,session,q.value(2).toString())) return false;
    if (!note.isEmpty() && !policy.allows(channel,session,note)) return false;
    q.finish();
    QString sql;
    QList<QVariant> bindings;
    QString contactTiming,contactReason,contactMessage;
    if (action=="keep") {
        const qint64 delay=nextCheckAfter(minutes);
        sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                           "next_check_at=CASE WHEN ?>0 THEN unixepoch()+? ELSE 0 END,"
                           "updated_at=unixepoch() WHERE turn_id=? AND version=? AND state='pending'");
        bindings<<delay<<delay<<turn<<version;
    } else if (action=="revise") {
        const auto topic=proposal["topic"].toString().trimmed().left(60);
        const auto watch=proposal["watch"].toString().trimmed().left(200);
        if ((!topic.isEmpty() && !policy.allows(channel,session,topic))
            || (!watch.isEmpty() && !policy.allows(channel,session,watch))) return false;
        const qint64 delay=nextCheckAfter(minutes);
        sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                           "next_check_at=CASE WHEN ?>0 THEN unixepoch()+? ELSE 0 END,"
                           "note=CASE WHEN ?<>'' THEN ? ELSE note END,"
                           "topic=CASE WHEN ?<>'' THEN ? ELSE topic END,"
                           "watch=CASE WHEN ?<>'' THEN ? ELSE watch END,"
                           "updated_at=unixepoch() WHERE turn_id=? AND version=? AND state='pending'");
        bindings<<delay<<delay<<note<<note<<topic<<topic<<watch<<watch<<turn<<version;
    } else if (action=="resolve") {
        const auto kind=proposal["kind"].toString();
        const bool kindKnown=kind==QStringLiteral("answer")||kind==QStringLiteral("contradiction")
            ||kind==QStringLiteral("cancelled");
        // 了结由回顾做出，不指向具体轮次（resolved_by=0）；无 kind 时按 answer 记录。
        sql=QStringLiteral("UPDATE private_reflection SET state='resolved',resolved_by=0,"
                           "resolved_kind=coalesce(?,'answer'),updated_at=unixepoch() "
                           "WHERE turn_id=? AND version=? AND state='pending'");
        bindings<<(kindKnown ? QVariant(kind) : QVariant())<<turn<<version;
    } else if (action=="drop") {
        sql=QStringLiteral("UPDATE private_reflection SET state='resolved',resolved_kind='dropped',"
                           "updated_at=unixepoch() WHERE turn_id=? AND version=? AND state='pending'");
        bindings<<turn<<version;
    } else {
        const auto contact=proposal["contact"].toObject();
        contactTiming=contact["timing"].toString();
        contactReason=contact["reason"].toString().trimmed();
        contactMessage=contact["message"].toString().trimmed();
        if (contactTiming!=QStringLiteral("private") && contactTiming!=QStringLiteral("next_chat")
            && contactTiming!=QStringLiteral("now")) return false;
        if (contactReason.isEmpty() || contactReason.size()>240
            || !policy.allows(channel,session,contactReason)) return false;
        if (contactTiming==QStringLiteral("now")
            && (contactMessage.isEmpty() || contactMessage.size()>300
                || !policy.allows(channel,session,contactMessage))) return false;
        sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                           "next_check_at=unixepoch()+7200,updated_at=unixepoch() "
                           "WHERE turn_id=? AND version=? AND state='pending'");
        bindings<<turn<<version;
    }
    q.prepare(sql);
    for (const auto &value : bindings) q.addBindValue(value);
    if (!q.exec() || q.numRowsAffected()!=1) return false;
    if (action=="contact") {
        // 联系意图与关注状态同一事务落库；投递层之后按 outcome='proposed' 取走。
        q.prepare("INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome,channel,session) "
                  "VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(turn_id) DO UPDATE SET "
                  "version=excluded.version,timing=excluded.timing,reason=excluded.reason,"
                  "message=excluded.message,outcome=excluded.outcome,"
                  "channel=excluded.channel,session=excluded.session,"
                  // 这是一次新的联系意图：重试预算重新计算（旧投递的失败次数不占用新意图）。
                  "attempts=0,last_outcome=NULL,last_attempt_at=NULL");
        q.addBindValue(turn);
        q.addBindValue(version);
        q.addBindValue(contactTiming);
        q.addBindValue(contactReason);
        q.addBindValue(contactTiming==QStringLiteral("now") ? contactMessage : QString());
        q.addBindValue(contactTiming==QStringLiteral("now") ? QStringLiteral("proposed")
                                                            : QStringLiteral("retained"));
        q.addBindValue(channel);
        q.addBindValue(session);
        if (!q.exec()) return false;
    }
    if (!m_db.commit()) return false;
    rollback.dismiss();
    qInfo()<<"[Concern] review committed turn="<<turn<<"version="<<version<<"action="<<action;
    return true;
}

bool MemoryManager::deferConcernReview(qlonglong turn,int version,const QString &reason)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE private_reflection SET state='deferred',eligible_at=unixepoch(),updated_at=unixepoch() "
              "WHERE turn_id=? AND version=? AND state='pending'");
    q.addBindValue(turn);
    q.addBindValue(version);
    const bool saved=q.exec() && q.numRowsAffected()==1;
    qInfo()<<"[Concern] review deferred turn="<<turn<<"version="<<version
           <<"saved="<<saved<<"reason="<<reason;
    return saved;
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
                                            const QJsonObject &prior,const QString &priorAction,
                                            const QJsonObject &concern)
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
        // prior 可能来自新格式（ref=concern/<根轮次>）或兜底的旧格式（ref=reflection/<轮次>）；
        // 两种都要能对上同一个当前关注行，否则回顾模型无法修订经由兜底注入的旧关注。
        const auto priorRef=prior["ref"].toString();
        const bool sameRef=priorRef==current["ref"].toString()
            || priorRef==QStringLiteral("reflection/%1").arg(previous);
        if (previous<=0 || previous==turn || current.isEmpty()
            || !sameRef
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
    if (choice=="consider") {
        // 回顾模型可以顺带给出关注的结构化描述（topic/watch/matter/下一次检查时间）。
        // 空字符串表示这次没给，保留旧值；0 分钟表示只在新证据或显式唤醒时再看，落库为 0。
        const auto topic=concern["topic"].toString().trimmed().left(60);
        const auto watch=concern["watch"].toString().trimmed().left(200);
        const auto matter=concern["matter"].toString().trimmed();
        if ((!topic.isEmpty() && !policy.allows(channel,session,topic))
            || (!watch.isEmpty() && !policy.allows(channel,session,watch))
            || (!matter.isEmpty() && !policy.allows(channel,session,matter))) return false;
        // 模型没给 next_check_after_minutes 时用配置的默认间隔；显式 0 仍表示只在新证据时。
        const int concernDefaultMinutes =
            ConfigManager::instance().getProactiveConfig().concernDefaultCheckMinutes;
        const int minutes=qBound(0,concern["next_check_after_minutes"]
                                        .toInt(concernDefaultMinutes),10080);
        const qint64 delay=nextCheckAfter(minutes);
        // 修订时新行沿用旧关注的根轮次；否则用本轮次，已有根轮次则保留（同一 head 被重新回顾）。
        const int revise=priorAction=="revise" ? 1 : 0;
        qlonglong rootTurn=turn;
        if (revise) {
            const auto priorRoot=prior["root_turn"].toInteger();
            rootTurn=priorRoot>0 ? priorRoot : prior["source_turn"].toInteger();
            if (rootTurn<=0) rootTurn=turn;
        }
        q.prepare("UPDATE private_reflection SET state='consider',note=?,updated_at=unixepoch(),"
                  "topic=CASE WHEN ?<>'' THEN ? ELSE topic END,"
                  "watch=CASE WHEN ?<>'' THEN ? ELSE watch END,"
                  "next_check_at=CASE WHEN ?>0 THEN unixepoch()+? ELSE 0 END,"
                  "matter=CASE WHEN ?<>'' THEN ? ELSE matter END,"
                  "root_turn=CASE WHEN ? THEN ? ELSE coalesce(root_turn,?) END "
                  "WHERE turn_id=? AND version=? AND state='pending'");
        q.addBindValue(note.trimmed());
        q.addBindValue(topic); q.addBindValue(topic);
        q.addBindValue(watch); q.addBindValue(watch);
        q.addBindValue(delay); q.addBindValue(delay);
        q.addBindValue(matter); q.addBindValue(matter);
        q.addBindValue(revise); q.addBindValue(rootTurn); q.addBindValue(turn);
        q.addBindValue(turn);
        q.addBindValue(version);
    } else {
        q.prepare("UPDATE private_reflection SET state=?,note=?,updated_at=unixepoch() "
                  "WHERE turn_id=? AND version=? AND state='pending'");
        q.addBindValue(choice);
        q.addBindValue(QString{});
        q.addBindValue(turn);
        q.addBindValue(version);
    }
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
    // 主动联系与后台写作解耦：只看"主动联系开关 + 长期记忆 + 目标可达"。
    // 关闭 privateWriting 只是不做私下写作，不应该让"现在联系"永远不可达。
    if (!opts.enabled || !memory.longTermMemoryEnabled
        || !opts.targets.contains(qMakePair(channel,session))) return {};
    if (!(channel==Channel::kPet && session==Channel::kDefaultSession)
        && !(channel==Channel::kQQPrivate && cfg.isMasterQQ(session))) return {};
    // 每日"现在联系"投递上限：按 delivery_ledger 的 contact 行计数。
    // 同一 (kind,ref) 重试只复用同一行，不重复占额度；withheld 表示并未真正投递，不计入。
    if (opts.contactDailyLimit > 0) {
        QSqlQuery used(m_db);
        if (used.exec("SELECT count(*) FROM delivery_ledger WHERE kind='contact' "
                      "AND date(created_at,'localtime')=date('now','localtime') "
                      "AND state<>'withheld'")
            && used.next() && used.value(0).toInt() >= opts.contactDailyLimit) {
            qInfo() << "[Contact] daily_limit reachable";
            return {};
        }
    }
    if (!m_db.transaction()) return {};
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    // 可领取状态：首次 proposed；或上一次被渠道明确拒绝（rejected）且还没用完一次重试。
    // unknown 表示协议层可能已送达，不重发；handed_off/protocol_accepted/withheld 是终态。
    q.prepare("SELECT c.message,h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id JOIN reflection_contact c ON c.turn_id=r.turn_id "
              "WHERE " + eligible + " AND c.turn_id=? AND c.version=? AND r.version=c.version "
              "AND r.state='consider' AND (c.outcome='proposed' "
              "OR (c.outcome='rejected' AND c.attempts<2)) AND NOT EXISTS "
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
              "WHERE turn_id=? AND version=? AND (outcome='proposed' "
              "OR (outcome='rejected' AND attempts<2))");
    q.addBindValue(channel);
    q.addBindValue(session);
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || q.numRowsAffected()!=1) return {};
    // 领取即写流水：投递结果之后由 recordContactOutcome 回填同一行。
    noteDelivery(m_db,QStringLiteral("contact"),
                 QStringLiteral("reflection:%1:%2").arg(turn).arg(version),
                 channel,session,message,QStringLiteral("claimed"),0);
    if (!m_db.commit()) return {};
    rollback.dismiss();
    return message;
}

bool MemoryManager::recordContactOutcome(qlonglong turn, int version, const QString &outcome)
{
    if (outcome!="handed_off" && outcome!="rejected" && outcome!="withheld"
        && outcome!="unknown" && outcome!="protocol_accepted") return false;
    // rejected/unknown 才算一次失败尝试：attempts 供 claim 判断"最多再试一次"。
    // 状态机保持 proposed→dispatching→终态；handed_off 之后协议层还能补一个终态回执。
    const bool failed = outcome=="rejected" || outcome=="unknown";
    QSqlQuery q(m_db);
    q.prepare("UPDATE reflection_contact SET outcome=?,last_outcome=?,last_attempt_at=unixepoch(),"
              "attempts=attempts+? "
              "WHERE turn_id=? AND version=? AND (outcome=? OR (outcome='handed_off' AND ?))");
    q.addBindValue(outcome);
    q.addBindValue(outcome);
    q.addBindValue(failed ? 1 : 0);
    q.addBindValue(turn);
    q.addBindValue(version);
    q.addBindValue(outcome=="withheld" ? "proposed" : "dispatching");
    q.addBindValue(outcome=="unknown" || outcome=="rejected" || outcome=="protocol_accepted");
    if (!q.exec() || q.numRowsAffected()!=1) return false;
    noteDelivery(m_db,QStringLiteral("contact"),
                 QStringLiteral("reflection:%1:%2").arg(turn).arg(version),
                 QString{},QString{},QString{},outcome,failed ? 1 : 0);
    return true;
}

QJsonArray MemoryManager::retryableReflectionContacts(int limit) const
{
    QJsonArray result;
    if (limit<=0) return result;
    auto &cfg=ConfigManager::instance();
    if (!cfg.getProactiveConfig().enabled || !cfg.getMemoryPolicyConfig().longTermMemoryEnabled)
        return result;
    QSqlQuery q(m_db);
    // 与 claimReflectionContact 的可领取条件保持一致：只取仍可重试一次的联系。
    q.prepare("SELECT c.turn_id,c.version,c.message FROM reflection_contact c "
              "JOIN private_reflection r ON r.turn_id=c.turn_id "
              "JOIN chat_history h ON h.id=r.turn_id "
              "WHERE c.outcome='rejected' AND c.attempts<2 AND r.state='consider' "
              "AND r.version=c.version "
              "AND h.subject='user' AND h.sensitive=0 "
              "AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id) "
              "ORDER BY coalesce(c.last_attempt_at,0) LIMIT ?");
    q.addBindValue(limit);
    if (!q.exec()) return result;
    while (q.next())
        result.append(QJsonObject{{"turn",q.value(0).toLongLong()},
                                  {"version",q.value(1).toInt()},
                                  {"message",q.value(2).toString()}});
    return result;
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
    // 到期未发也要在流水里留痕，避免出现"消失的提醒"。
    {
        QSqlQuery expiry(m_db);
        if (!expiry.exec("INSERT INTO delivery_ledger(kind,ref,channel,session,text,state,updated_at) "
                         "SELECT 'reminder','reminder:'||id,channel,session,message,'expired',CURRENT_TIMESTAMP "
                         "FROM reminder WHERE state='pending' AND expires<unixepoch() "
                         "ON CONFLICT(kind,ref) DO UPDATE SET state='expired',updated_at=CURRENT_TIMESTAMP"))
            qWarning()<<"[Reminder] 过期流水写入失败"<<expiry.lastError();
    }
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
    if (!q.exec() || q.numRowsAffected()!=1) return {};
    noteDelivery(m_db,QStringLiteral("reminder"),QStringLiteral("reminder:%1").arg(id),
                 channel,session,message,
                 allowed ? QStringLiteral("claimed") : QStringLiteral("withheld"),0);
    if (!m_db.commit()) return {};
    rollback.dismiss();
    return allowed ? QJsonObject{{"id",id},{"message",message}} : QJsonObject{};
}

void MemoryManager::reminderOutcome(qint64 id,const QString &state)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE reminder SET state=? WHERE id=? AND (state='dispatching' OR (state='handed_off' AND ?))");
    q.addBindValue(state); q.addBindValue(id); q.addBindValue(state!="handed_off");
    if (!q.exec()) qWarning()<<"[Reminder] outcome write failed"<<id;
    // rejected/unknown 记一次失败尝试；handed_off/protocol_accepted 只更新终态。
    const bool failed=state=="rejected" || state=="unknown";
    noteDelivery(m_db,QStringLiteral("reminder"),QStringLiteral("reminder:%1").arg(id),
                 QString{},QString{},QString{},state,failed ? 1 : 0);
}

