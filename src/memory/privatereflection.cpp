#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include "recentcontext.h"
#include "actionpolicy.h"
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

// 活动结果是否已经分享过：与 recordShareOutcome 的 ref 约定一致（thought/<id>:<version>）。
// handed_off/protocol_accepted 是明确送达；unknown 表示协议层"可能已送达、不重发"，
// 因此同样视为已分享，避免把同一结果当成"还没告诉主人的新结果"再发一遍。
bool artifactAlreadyShared(const QSqlDatabase &db,qlonglong id)
{
    if (id<=0) return false;
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM delivery_ledger WHERE kind='share' AND ref LIKE ? "
              "AND state IN ('handed_off','protocol_accepted','unknown') LIMIT 1");
    q.addBindValue(QStringLiteral("thought:%1:%").arg(id));
    return q.exec() && q.next();
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

// ===== 切片 C：动机/事实分离后的联系契约 =====
// 旧契约（contact.basis: new_owner_evidence/activity_result）只证明"有依据"，
// 新契约（contact.motivation + contact.claims）说明"她为什么想开口"以及"消息里
// 每条事实/承诺/执行结果声称由哪条资料支持"。两者互斥：新契约优先。
bool contactUsesNewContract(const QJsonObject &contact)
{
    return (contact.contains(QStringLiteral("claims"))
            && contact.value(QStringLiteral("claims")).isArray())
        || (contact.contains(QStringLiteral("motivation"))
            && contact.value(QStringLiteral("motivation")).isObject());
}

QString compactObject(const QJsonObject &object)
{
    if (object.isEmpty()) return {};
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString compactArray(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

struct NewContractCheck
{
    bool withheld = false;        // 需要压下这条联系（仍可写入 withheld 审计行）
    bool needsVerifier = false;   // 确定性通过，但还有事实 claim 等语义核验
    QString reason;               // 非空=被拒原因
    QString motivationJson;
    QString claimsJson;
    QString semanticStatus;
};

// 提交前的确定性复核。timing=="now" 走完整的主动联系策略（动机/claims/quiet/冷却/目标）；
// private/next_chat 只是保留意图，但仍要保证 claims 不是编造。
NewContractCheck checkNewContractContact(const MemoryManager *memory, const QString &channel,
                                         const QString &session, qlonglong turn,
                                         const QJsonObject &contact, const QString &timing)
{
    NewContractCheck check;
    auto *mutableMemory = const_cast<MemoryManager *>(memory);
    const ActionPolicy policy(mutableMemory);
    const auto motivation = contact.value(QStringLiteral("motivation")).toObject();
    check.motivationJson = compactObject(motivation);
    const auto claims = contact.value(QStringLiteral("claims")).toArray();
    check.claimsJson = compactArray(claims);
    QString reason = policy.validateMotivation(channel, session, turn, motivation);
    if (reason.isEmpty()) {
        for (const auto &value : claims) {
            const auto validation = policy.validateClaimDeterministic(channel, session, turn,
                                                                      value.toObject());
            if (validation.result != ClaimValidation::Entailed) {
                reason = validation.reason;
                break;
            }
        }
    }
    if (reason.isEmpty() && timing == QStringLiteral("now")) {
        const auto decision = policy.evaluateSpontaneousContactDeterministic(channel, session, turn,
                                                                             contact);
        if (!decision.value(QStringLiteral("allowed")).toBool())
            reason = decision.value(QStringLiteral("reason")).toString();
        check.needsVerifier = decision.value(QStringLiteral("needs_verifier")).toBool();
        check.semanticStatus = contact.value(QStringLiteral("semantic_status")).toString();
        if (reason.isEmpty() && check.needsVerifier
            && check.semanticStatus != QStringLiteral("entailed")
            && check.semanticStatus != QStringLiteral("repaired")) {
            reason = QStringLiteral("unverified_claims");
        }
        if (check.semanticStatus.isEmpty())
            check.semanticStatus = check.needsVerifier ? QStringLiteral("unverified")
                                                       : QStringLiteral("not_required");
    } else if (reason.isEmpty()) {
        check.semanticStatus = contact.value(QStringLiteral("semantic_status")).toString();
        if (check.semanticStatus.isEmpty()) check.semanticStatus = QStringLiteral("retained");
    }
    if (reason.isEmpty() && check.semanticStatus == QStringLiteral("withheld")) {
        reason = contact.value(QStringLiteral("withhold_reason")).toString();
        if (reason.isEmpty()) reason = QStringLiteral("semantic_withheld");
    }
    if (!reason.isEmpty()) {
        check.withheld = true;
        check.reason = reason;
        if (check.semanticStatus.isEmpty() || check.semanticStatus == QStringLiteral("retained"))
            check.semanticStatus = QStringLiteral("withheld");
    }
    return check;
}

} // namespace

QJsonObject MemoryManager::claimPrivateReflection(int dailyLimit, int cooldownSeconds,
                                                  QString *reason, QDateTime *nextEligibleAt)
{
    if (reason) reason->clear();
    if (nextEligibleAt) *nextEligibleAt = QDateTime{};
    if (dailyLimit <= 0) {
        if (reason) *reason = QStringLiteral("daily_limit");
        return {};
    }
    if (!m_db.transaction()) {
        if (reason) *reason = QStringLiteral("db_error");
        return {};
    }
    auto rollback = qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    // R12-1a：新来源候选仍要求 24h 窗口；但该源存在失败行且 next_retry_at<=now 时允许重领
    //（即使源已超出 24h）。只覆盖"领取过、失败过"的任务，不会把陈旧来源无限拉回；
    // 仍在暂停期内的失败行永远不进入候选。
    q.prepare("SELECT h.id,h.channel,h.session_id,s.input,s.reported,coalesce(r.version,0),h.raw_reply,h.turn_state" + sourceRows +
              "LEFT JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND coalesce(h.thought,'')='' "
              " AND (julianday(s.reported) BETWEEN julianday('now','-1 day') AND julianday('now') "
              "      OR EXISTS (SELECT 1 FROM review_failure f WHERE f.source_kind='reflection' "
              "                 AND f.source_ref='reflection/'||h.id AND f.next_retry_at IS NOT NULL "
              "                 AND f.next_retry_at<=unixepoch())) "
              " AND NOT EXISTS (SELECT 1 FROM review_failure f WHERE f.source_kind='reflection' "
              "                 AND f.source_ref='reflection/'||h.id "
              "                 AND f.suspended_until IS NOT NULL AND f.suspended_until>unixepoch()) "
              " AND (r.turn_id IS NULL OR (r.state IN ('pending','deferred') AND r.eligible_at<=unixepoch())) "
              "ORDER BY h.id DESC LIMIT 24");
    if (!q.exec()) {
        if (reason) *reason = QStringLiteral("db_error");
        return {};
    }
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
        // R12-1a：暂停到期后的恢复尝试（重置连续失败计数，保留 last_reason）。
        if (!recoverSuspendedFailure(QStringLiteral("reflection"),
                                     QStringLiteral("reflection/%1").arg(id))) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        QString chargeReason;
        QDateTime next;
        if (!chargePrivateReview(id,dailyLimit,cooldownSeconds,&chargeReason,&next,
                                 QStringLiteral("reflection"),
                                 QStringLiteral("reflection/%1").arg(id))) {
            if (reason) *reason = chargeReason.isEmpty() ? QStringLiteral("db_error") : chargeReason;
            if (nextEligibleAt) *nextEligibleAt = next;
            return {};
        }
        q.prepare("INSERT INTO private_reflection(turn_id,version,state,eligible_at,updated_at,claimed_by,root_turn) "
                  "VALUES(?,?,'pending',unixepoch()+1800,unixepoch(),'source',?) ON CONFLICT(turn_id) DO UPDATE SET "
                  "version=excluded.version,state='pending',eligible_at=excluded.eligible_at,"
                  "claimed_by=excluded.claimed_by,updated_at=unixepoch() "
                  "WHERE private_reflection.version=excluded.version-1 AND private_reflection.state IN ('deferred','pending')");
        q.addBindValue(id);
        q.addBindValue(version);
        q.addBindValue(id);
        if (!q.exec()) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        if (q.numRowsAffected()!=1) {
            // 领取行在事务内被并发修改，CAS 不再匹配。
            if (reason) *reason = QStringLiteral("cas_conflict");
            return {};
        }
        // 公平性：成功领取非 OpenWake 任务时，在同一事务内推进持久化公平计数。
        if (!bumpOpenWakeFairness()) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        if (!m_db.commit()) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
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
    // 没有可领取的新来源（查询为空、窗口外、策略过滤或都已在处理中）。
    if (reason) *reason = QStringLiteral("no_candidate");
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

QJsonObject MemoryManager::claimDueConcern(int dailyLimit,int cooldownSeconds,
                                            QString *reason,QDateTime *nextEligibleAt)
{
    if (reason) reason->clear();
    if (nextEligibleAt) *nextEligibleAt = QDateTime{};
    if (dailyLimit<=0) {
        if (reason) *reason = QStringLiteral("daily_limit");
        return {};
    }
    // 保留期只约束"按时间到期"的回顾；新证据命中走 matchedConcerns，不受这里影响。
    const int retentionDays=qBound(1,
        ConfigManager::instance().getProactiveConfig().concernRetentionDays,365);
    // R12-1a：领取前先终止已越过保留期的定时重试（单事务清 failure 行 + next_check_at=0，
    // 保留 consider/note/topic/fact）。这一步在领取事务之前执行，失败按 db_error 上报。
    if (expireConcernScheduledRetries(retentionDays) < 0) {
        if (reason) *reason = QStringLiteral("db_error");
        return {};
    }
    if (!m_db.transaction()) {
        if (reason) *reason = QStringLiteral("db_error");
        return {};
    }
    auto rollback=qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    // 保留期外/定时重试越界的关注已在 expireConcernScheduledRetries 里终止（next_check_at=0、
    // 清 failure 行、保留 consider/note/topic/fact 与日志），这里只领取仍在保留期内、
    // 尚未被终止的到期关注。
    // 只领"按时间到期"的开放关注；被回顾链修订过的旧行不再作为 head。
    q.prepare("SELECT r.turn_id,r.version,r.note,h.channel,h.session_id,s.input" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND r.state='consider' AND NOT EXISTS (SELECT 1 FROM reflection_change WHERE prior_turn=r.turn_id)"
              " AND r.next_check_at>0 AND r.next_check_at<=unixepoch()"
              " AND julianday(s.reported)>=julianday('now','-" + QString::number(retentionDays) + " day')"
              " ORDER BY r.next_check_at,r.turn_id LIMIT 24");
    if (!q.exec()) {
        if (reason) *reason = QStringLiteral("db_error");
        return {};
    }
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
        // R12-1a：暂停到期后的恢复尝试（重置连续失败计数，保留 last_reason）。
        if (!recoverSuspendedFailure(QStringLiteral("concern"),
                                     QStringLiteral("concern/%1").arg(item.turn))) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        // 到期回顾与新来源回顾共用同一份每日预算与冷却；预算用尽时本轮不再领取。
        QString chargeReason;
        QDateTime next;
        if (!chargePrivateReview(item.turn,dailyLimit,cooldownSeconds,&chargeReason,&next,
                                 QStringLiteral("concern"),
                                 QStringLiteral("concern/%1").arg(item.turn))) {
            if (reason) *reason = chargeReason.isEmpty() ? QStringLiteral("db_error") : chargeReason;
            if (nextEligibleAt) *nextEligibleAt = next;
            return {};
        }
        const int nextVersion=item.version+1;
        q.prepare("UPDATE private_reflection SET state='pending',claimed_by='due',version=?,"
                  "eligible_at=unixepoch()+1800 WHERE turn_id=? AND version=? AND state='consider'");
        q.addBindValue(nextVersion);
        q.addBindValue(item.turn);
        q.addBindValue(item.version);
        // 必须精确命中一行：并发/迟到的领取不能把已变化的关注再取走。
        if (!q.exec()) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        if (q.numRowsAffected()!=1) {
            if (reason) *reason = QStringLiteral("cas_conflict");
            return {};
        }
        // 公平性：成功领取非 OpenWake 任务时，在同一事务内推进持久化公平计数。
        if (!bumpOpenWakeFairness()) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        if (!m_db.commit()) {
            if (reason) *reason = QStringLiteral("db_error");
            return {};
        }
        rollback.dismiss();
        qInfo()<<"[Concern] due claimed turn="<<item.turn<<"version="<<nextVersion;
        return concernReviewContext(item.turn,nextVersion);
    }
    // 没有可领取的到期关注（可能全部超出保留期、被策略过滤或没有到期项）。
    if (reason) *reason = QStringLiteral("no_candidate");
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
        // ref 与 contact.basis 的 new_owner_evidence 约定一致：模型可以直接引用这一行的发言。
        since.append(QJsonObject{{"ref",QStringLiteral("chat/%1").arg(sinceQuery.value(0).toLongLong())},
                                 {"turn",sinceQuery.value(0).toLongLong()},
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
    // 时间概念分离：next_check_at 是茉子自己排的"再检查时间"，不是与主人的约定。
    // 键名改成 own_next_review_at 并配一句说明，避免模型把它当成"约定时间到了"而现在联系。
    QJsonObject concernRecord{{"ref",QStringLiteral("concern/%1").arg(root>0 ? root : turn)},
                              {"head",QStringLiteral("reflection/%1").arg(turn)},
                              {"topic",topic},
                              {"watch",watch},
                              {"own_reflection",note},
                              {"reported_at",reported}};
    if (nextCheck>0) {
        concernRecord["own_next_review_at"]=isoFromEpoch(nextCheck);
        concernRecord["note"]=QStringLiteral(
            "你自己决定何时再看一眼的时间，不是与主人的约定；不能作为现在联系的理由。");
    }
    if (!matterName.isEmpty()) concernRecord["matter"]=matterName;
    // 候选依据：只给可核验的引用，让模型从中选择并在 contact.basis 写明，服务端再逐条核验。
    // owner_evidence 复用上面的 since 查询结果（同一行同时保留 turn/text/reported_at 原字段）。
    // reminders 只是"待发提醒"的信息，服务端会在每项标 usable_for_now=false。
    QJsonArray ownerEvidence;
    for (const auto &value : since) {
        const auto row=value.toObject();
        ownerEvidence.append(QJsonObject{{"ref",row["ref"]},
                                         {"reported_at",row["reported_at"]},
                                         {"text",row["text"].toString().left(160)}});
    }
    // 仍为 pending 且未过期的提醒：只让模型"知道有这个提醒"，不能作为现在联系的依据——
    // 时间委托一律由提醒链在到期时执行，回顾若再代表它联系一次就是同一委托两发。
    QJsonArray reminders;
    {
        QSqlQuery reminderQuery(m_db);
        reminderQuery.prepare("SELECT id,due,message FROM reminder WHERE channel=? AND session=? "
                              "AND state='pending' AND expires>unixepoch() ORDER BY due LIMIT 8");
        reminderQuery.addBindValue(channel);
        reminderQuery.addBindValue(session);
        if (reminderQuery.exec()) while (reminderQuery.next()) {
            const auto message=reminderQuery.value(2).toString();
            if (!policy.allows(channel,session,message)) continue;
            reminders.append(QJsonObject{{"ref",QStringLiteral("reminder/%1").arg(reminderQuery.value(0).toLongLong())},
                                         {"state",QStringLiteral("pending")},
                                         {"due_at",isoFromEpoch(reminderQuery.value(1).toLongLong())},
                                         {"message",message.left(80)},
                                         {"usable_for_now",false},
                                         {"note",QStringLiteral("时间委托由提醒链执行，不能作为 contact(now) 依据")}});
        }
    }
    // 已经完成、有产物、且还没分享给主人的活动结果：可以作为"活动有了结果"的依据。
    QJsonArray artifacts;
    {
        QSqlQuery artifactQuery(m_db);
        artifactQuery.prepare("SELECT id,thought_version,json_extract(thought_result,'$.artifact_ref') "
                              "FROM chat_history WHERE thought IS NOT NULL AND thought<>'' "
                              "AND sensitive=0 AND subject='user' "
                              "AND json_extract(thought_result,'$.status')='completed' "
                              "AND coalesce(json_extract(thought_result,'$.artifact_ref'),'')<>'' "
                              "ORDER BY id DESC LIMIT 8");
        if (artifactQuery.exec()) while (artifactQuery.next()) {
            const auto activityId=artifactQuery.value(0).toLongLong();
            artifacts.append(QJsonObject{
                {"ref",QStringLiteral("thought/%1").arg(activityId)},
                {"status",QStringLiteral("completed")},
                {"version",artifactQuery.value(1).toInt()},
                {"already_shared",artifactAlreadyShared(m_db,activityId)}});
        }
    }
    const QJsonObject basis{{"owner_evidence",ownerEvidence},
                            {"reminders",reminders},
                            {"artifacts",artifacts}};
    QJsonObject result{{"id",turn},{"version",version},{"kind","concern"},
                       {"concern",concernRecord},{"since",since},{"contacts",contacts},
                       {"basis",basis},
                       // 来源发言的 reported_at：R12-0 回顾时间上下文用它区分"当前"与历史。
                       {"reported_at",reported},
                       // 服务端内部定位键：切片 C 的策略/验证器需要来源渠道会话做可见性判定，
                       // LLMService 会把它们从模型上下文里剥掉。
                       {"source_channel",channel},{"source_session",session}};
    if (!matter.isEmpty()) result["matter"]=matter;
    return result;
}

QString MemoryManager::validateContactBasis(const QString &channel,const QString &session,
                                            qlonglong concernTurn,const QJsonObject &contact) const
{
    // 只有"现在联系"需要依据；private/next_chat 只是保留意图，不会真正打扰主人。
    if (contact.value(QStringLiteral("timing")).toString()!=QStringLiteral("now")) return {};
    if (!contact.value(QStringLiteral("basis")).isObject()) return QStringLiteral("missing_basis");
    const QJsonObject basis=contact.value(QStringLiteral("basis")).toObject();
    const auto typeValue=basis.value(QStringLiteral("type"));
    if (!typeValue.isString()) return QStringLiteral("invalid_basis_type");
    const auto type=typeValue.toString();
    // v28：真实时间委托一律由提醒链在到期时执行。回顾若也能代表它"现在联系"，
    // 同一次委托会被发两次（回顾一次 + 5s 后提醒一次），所以这里直接拒绝并给出可审计原因。
    if (type==QStringLiteral("explicit_time_delegation"))
        return QStringLiteral("time_delegation_owned_by_reminder");
    static const QSet<QString> basisTypes{
        QStringLiteral("new_owner_evidence"),
        QStringLiteral("activity_result")};
    if (!basisTypes.contains(type)) return QStringLiteral("invalid_basis_type");
    const auto refsValue=basis.value(QStringLiteral("refs"));
    if (!refsValue.isArray()) return QStringLiteral("missing_basis");
    const auto refs=refsValue.toArray();
    if (refs.isEmpty() || refs.size()>4) return QStringLiteral("missing_basis");
    for (const auto &value : refs) {
        if (!value.isString() || value.toString().trimmed().isEmpty())
            return QStringLiteral("invalid_basis_ref");
    }
    // 关注来源：决定"之后"的时间下界与可见性范围。
    QSqlQuery origin(m_db);
    origin.prepare("SELECT h.channel,h.session_id,coalesce(s.reported,h.timestamp),"
                   "h.subject,h.sensitive,h.turn_kind "
                   "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
                   "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
                   "AND s.input=h.user_input WHERE h.id=?");
    origin.addBindValue(concernTurn);
    if (!origin.exec() || !origin.next()) return QStringLiteral("invalid_basis_ref");
    const auto originChannel=origin.value(0).toString();
    const auto originSession=origin.value(1).toString();
    // 渠道/会话只用于校验来源轮次确实落在主人私聊域；具体依据核验不需要它们。
    Q_UNUSED(originChannel);
    Q_UNUSED(originSession);
    const auto originReported=origin.value(2).toString();
    if (origin.value(3).toString()!=QStringLiteral("user") || origin.value(4).toBool()
        || origin.value(5).toString()!=QStringLiteral("reply"))
        return QStringLiteral("invalid_basis_ref");
    origin.finish();
    // 用 SQLite 的 julianday 比较混合格式时间（UTC ISO 与本地 datetime 都支持）。
    const auto after=[this](const QString &later,const QString &earlier) {
        if (later.isEmpty() || earlier.isEmpty()) return false;
        QSqlQuery compare(m_db);
        compare.prepare("SELECT julianday(?)>julianday(?)");
        compare.addBindValue(later);
        compare.addBindValue(earlier);
        return compare.exec() && compare.next() && compare.value(0).toInt()==1;
    };
    for (const auto &value : refs) {
        const auto ref=value.toString();
        if (type==QStringLiteral("new_owner_evidence")) {
            // 必须是关注产生之后主人真实说过的话；助手自己的话、主动消息、自己的关注都不算。
            if (!ref.startsWith(QStringLiteral("chat/"))) return QStringLiteral("invalid_basis_ref");
            const auto refTurn=ref.mid(5).section(':',0,0).toLongLong();
            if (refTurn<=0) return QStringLiteral("invalid_basis_ref");
            QSqlQuery row(m_db);
            row.prepare("SELECT h.channel,h.session_id,coalesce(s.reported,h.timestamp),"
                        "h.subject,h.sensitive,h.turn_kind,coalesce(h.user_input,'') "
                        "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
                        "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
                        "AND s.input=h.user_input WHERE h.id=?");
            row.addBindValue(refTurn);
            if (!row.exec() || !row.next()) return QStringLiteral("invalid_basis_ref");
            // turn_kind='proactive' 的主动消息不是主人委托；'received' 等未落定轮次同样不算。
            if (row.value(3).toString()!=QStringLiteral("user") || row.value(4).toBool()
                || row.value(5).toString()!=QStringLiteral("reply")
                || row.value(6).toString().trimmed().isEmpty())
                return QStringLiteral("invalid_basis_ref");
            if (!visibleSession(row.value(0).toString(),row.value(1).toString(),channel,session))
                return QStringLiteral("invalid_basis_ref");
            const auto reported=row.value(2).toString();
            row.finish();
            if (!after(reported,originReported)) return QStringLiteral("stale_owner_evidence");
        } else {
            // 活动结果：必须真的完成、有产物、且还没分享过（unknown 也可能已送达，按已分享处理）。
            if (!ref.startsWith(QStringLiteral("thought/"))) return QStringLiteral("invalid_basis_ref");
            const auto activityId=ref.mid(8).section(':',0,0).toLongLong();
            if (activityId<=0) return QStringLiteral("invalid_basis_ref");
            const auto activity=activityById(activityId);
            if (activity.isEmpty()) return QStringLiteral("invalid_basis_ref");
            const auto result=activity.value(QStringLiteral("result")).toObject();
            if (result.value(QStringLiteral("status")).toString()!=QStringLiteral("completed")
                || result.value(QStringLiteral("artifact_ref")).toString().trimmed().isEmpty())
                return QStringLiteral("invalid_basis_ref");
            if (artifactAlreadyShared(m_db,activityId))
                return QStringLiteral("artifact_already_shared");
        }
    }
    return {};
}

MemoryManager::ReflectionCommitResult MemoryManager::finishConcernReview(qlonglong turn,int version,const QJsonObject &proposal)
{
    const auto fail = [](const char *code) {
        return ReflectionCommitResult{QString::fromLatin1(code), {}};
    };
    QString contactRejection;
    static const QSet<QString> actions{QStringLiteral("keep"),QStringLiteral("revise"),
                                       QStringLiteral("resolve"),QStringLiteral("drop"),
                                       QStringLiteral("contact")};
    const auto action=proposal["concern_action"].toString();
    if (!actions.contains(action)) return fail("invalid_internal_structure");
    const auto note=proposal["note"].toString().trimmed();
    if (note.size()>240) return fail("invalid_internal_structure");
    // 模型没给 next_check_after_minutes 时用配置的默认间隔；显式 0 仍表示"只在新证据时"。
    const int concernDefaultMinutes =
        ConfigManager::instance().getProactiveConfig().concernDefaultCheckMinutes;
    const int minutes=qBound(0,proposal["forget"].toObject()["next_check_after_minutes"]
                                    .toInt(concernDefaultMinutes),10080);
    if (!m_db.transaction()) return fail("db_error");
    auto rollback=qScopeGuard([this]{m_db.rollback();});
    QSqlQuery q(m_db);
    // 与领取时一致的来源校验：只有仍处于 pending 且 version 匹配的领取才能落库。
    q.prepare("SELECT h.channel,h.session_id,s.input,r.note,r.topic,r.watch,r.version,r.state" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible +
              " AND h.id=?");
    q.addBindValue(turn);
    if (!q.exec()) return fail("db_error");
    if (!q.next()) return fail("source_unavailable");
    if (q.value(6).toInt()!=version || q.value(7).toString()!="pending")
        return fail("version_conflict");
    const auto channel=q.value(0).toString();
    const auto session=q.value(1).toString();
    const auto beforeNote=q.value(3).toString();
    const auto beforeTopic=q.value(4).toString();
    const auto beforeWatch=q.value(5).toString();
    const MemoryReadPolicy policy;
    if (!policy.allows(channel,session,q.value(2).toString())) return fail("permission_denied");
    if (!note.isEmpty() && !policy.allows(channel,session,note)) return fail("internal_note_filtered");
    q.finish();
    QString sql;
    QList<QVariant> bindings;
    QString contactTiming,contactReason,contactMessage;
    // contact now 的可核验依据：只有通过核验才落 reflection_contact，并把依据一起存档。
    QString basisType,basisRefs;
    bool contactBasisRejected=false;
    bool contactNewContract=false;
    NewContractCheck newCheck;
    QString revisionAction=action;
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
            || (!watch.isEmpty() && !policy.allows(channel,session,watch))) return fail("internal_concern_filtered");
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
        // 切片 C：contact 带 claims 数组或 motivation 对象即走新契约；否则保持 v29 依据路径。
        contactNewContract=contactUsesNewContract(contact);
        const bool contactValid = (contactTiming=="private" || contactTiming=="next_chat" || contactTiming=="now")
            && !contactReason.isEmpty() && contactReason.size()<=240
            && policy.allows(channel,session,contactReason)
            && (contactTiming!="now" || (!contactMessage.isEmpty() && contactMessage.size()<=300
                                        && policy.allows(channel,session,contactMessage)));
        if (!contactValid) {
            contactRejection=QStringLiteral("invalid_contact");
            qInfo()<<"[Concern] invalid_contact turn="<<turn<<"version="<<version;
            sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                               "next_check_at=0,updated_at=unixepoch() "
                               "WHERE turn_id=? AND version=? AND state='pending'");
            bindings<<turn<<version;
            contactBasisRejected=true;
            contactNewContract=false;
            revisionAction=QStringLiteral("contact_rejected");
        } else if (contactNewContract) {
            // 确定性复核（动机合法性 + claims refs/authority/时序 + quiet/冷却/目标）。
            // 语义核验已由 LLMService 完成并写进 contact.semantic_status；这里不再调用模型。
            newCheck=checkNewContractContact(this,channel,session,turn,contact,contactTiming);
            if (newCheck.withheld) {
                contactRejection=newCheck.reason;
                // 新契约被拒：不投递，但按 keep 语义把关注放回队列；联系意图与
                // semantic_status=withheld 一起落库，保证"为什么没开口"可审计。
                qInfo()<<"[Concern] new_contract_withheld turn="<<turn<<"version="<<version
                       <<"reason="<<newCheck.reason;
                sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                                   "next_check_at=0,updated_at=unixepoch() "
                                   "WHERE turn_id=? AND version=? AND state='pending'");
                bindings<<turn<<version;
                contactBasisRejected=true;
                revisionAction=QStringLiteral("contact_rejected");
            } else {
                sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                                   "next_check_at=unixepoch()+7200,updated_at=unixepoch() "
                                   "WHERE turn_id=? AND version=? AND state='pending'");
                bindings<<turn<<version;
            }
        } else if (contactTiming==QStringLiteral("now")) {
            const auto rejection=validateContactBasis(channel,session,turn,contact);
            if (!rejection.isEmpty()) {
                contactRejection=rejection;
                // 依据核验不过：不要投递，但也不能让关注卡在 pending 里无限 defer。
                // 保留关注，等待新的依据，并留一条可审计的修订。
                const auto declaredType=contact["basis"].toObject()["type"].toString();
                qInfo()<<"[Concern] contact_rejected turn="<<turn<<"version="<<version
                       <<"type="<<declaredType<<"reason="<<rejection;
                sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                                   "next_check_at=0,updated_at=unixepoch() "
                                   "WHERE turn_id=? AND version=? AND state='pending'");
                bindings<<turn<<version;
                contactBasisRejected=true;
                revisionAction=QStringLiteral("contact_rejected");
            } else {
                const auto basis=contact["basis"].toObject();
                basisType=basis["type"].toString();
                basisRefs=QString::fromUtf8(QJsonDocument(basis["refs"].toArray())
                                                .toJson(QJsonDocument::Compact));
                sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                                   "next_check_at=unixepoch()+7200,updated_at=unixepoch() "
                                   "WHERE turn_id=? AND version=? AND state='pending'");
                bindings<<turn<<version;
            }
        } else {
            sql=QStringLiteral("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
                               "next_check_at=unixepoch()+7200,updated_at=unixepoch() "
                               "WHERE turn_id=? AND version=? AND state='pending'");
            bindings<<turn<<version;
        }
    }
    q.prepare(sql);
    for (const auto &value : bindings) q.addBindValue(value);
    if (!q.exec()) return fail("db_error");
    if (q.numRowsAffected()!=1) return fail("cas_conflict");
    if (action=="contact" && (contactNewContract || !contactBasisRejected)) {
        // 联系意图与关注状态同一事务落库；投递层之后按 outcome='proposed' 取走。
        // basis_type/basis_refs 记录这次"现在联系"凭什么发生，非 now 的联系留 NULL。
        // 新契约另存 motivation_json/claims_json/semantic_status。
        q.prepare("INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome,channel,session,"
                  "basis_type,basis_refs,motivation_json,claims_json,semantic_status) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(turn_id) DO UPDATE SET "
                  "version=excluded.version,timing=excluded.timing,reason=excluded.reason,"
                  "message=excluded.message,outcome=excluded.outcome,"
                  "channel=excluded.channel,session=excluded.session,"
                  "basis_type=excluded.basis_type,basis_refs=excluded.basis_refs,"
                  "motivation_json=excluded.motivation_json,claims_json=excluded.claims_json,"
                  "semantic_status=excluded.semantic_status,"
                  // 这是一次新的联系意图：重试预算重新计算（旧投递的失败次数不占用新意图）。
                  "attempts=0,last_attempt_at=NULL,"
                  "last_outcome=CASE WHEN excluded.outcome='withheld' THEN excluded.semantic_status "
                  "ELSE NULL END");
        q.addBindValue(turn);
        q.addBindValue(version);
        q.addBindValue(contactTiming);
        q.addBindValue(contactReason);
        q.addBindValue(contactTiming==QStringLiteral("now") ? contactMessage : QString());
        QString outcome;
        if (contactNewContract)
            outcome = newCheck.withheld ? QStringLiteral("withheld")
                      : contactTiming==QStringLiteral("now") ? QStringLiteral("proposed")
                                                             : QStringLiteral("retained");
        else
            outcome = contactTiming==QStringLiteral("now") ? QStringLiteral("proposed")
                                                           : QStringLiteral("retained");
        q.addBindValue(outcome);
        q.addBindValue(channel);
        q.addBindValue(session);
        q.addBindValue(basisType.isEmpty() ? QVariant() : QVariant(basisType));
        q.addBindValue(basisRefs.isEmpty() ? QVariant() : QVariant(basisRefs));
        q.addBindValue(contactNewContract && !newCheck.motivationJson.isEmpty()
                           ? QVariant(newCheck.motivationJson) : QVariant());
        q.addBindValue(contactNewContract && !newCheck.claimsJson.isEmpty()
                           ? QVariant(newCheck.claimsJson) : QVariant());
        q.addBindValue(contactNewContract && !newCheck.semanticStatus.isEmpty()
                           ? QVariant(newCheck.semanticStatus) : QVariant());
        if (!q.exec()) return fail("db_error");
    }
    {
        // 到期回顾是原地更新同一行，旧想法会被覆盖；这里把"决策前/后"快照留成流水，
        // 与来源回顾的 reflection_change 链互补，保证可追溯。
        // 依据被拒时同样留痕（action=contact_rejected），但不写 contact_timing。
        const bool rejectedContact=contactBasisRejected;
        QSqlQuery rev(m_db);
        rev.prepare("INSERT INTO concern_revision(turn,version,action,note_before,note_after,"
                    "topic,watch,next_check_at,contact_timing) "
                    "SELECT turn_id,version,?,?,note,topic,watch,next_check_at,? "
                    "FROM private_reflection WHERE turn_id=?");
        rev.addBindValue(revisionAction);
        rev.addBindValue(beforeNote);
        rev.addBindValue((rejectedContact || contactTiming.isEmpty()) ? QVariant() : QVariant(contactTiming));
        rev.addBindValue(turn);
        if (!rev.exec()) return fail("db_error");
    }
    if (!m_db.commit()) return fail("db_error");
    rollback.dismiss();
    qInfo()<<"[Concern] review committed turn="<<turn<<"version="<<version<<"action="<<revisionAction;
    return {{}, contactRejection};
}

bool MemoryManager::deferConcernReview(qlonglong turn,int version,const QString &reason)
{
    QSqlQuery q(m_db);
    q.prepare("UPDATE private_reflection SET state='consider',claimed_by=NULL,next_check_at=unixepoch(),"
              "eligible_at=unixepoch(),updated_at=unixepoch() "
              "WHERE turn_id=? AND version=? AND state='pending' AND claimed_by='due'");
    q.addBindValue(turn);
    q.addBindValue(version);
    const bool saved=q.exec() && q.numRowsAffected()==1;
    qInfo()<<"[Concern] review deferred turn="<<turn<<"version="<<version
           <<"saved="<<saved<<"reason="<<reason;
    return saved;
}

int MemoryManager::recoverStaleReflections(int staleSeconds)
{
    if (staleSeconds < 0) staleSeconds = 0;
    if (!m_db.transaction()) return 0;
    auto rollback = qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    int recovered = 0;
    // 到期关注的租约过期：回到 consider 并立即可再领取（保留关注本身，不丢）。
    q.prepare("UPDATE private_reflection SET state='consider',claimed_by=NULL,"
              "next_check_at=unixepoch(),updated_at=unixepoch() "
              "WHERE state='pending' AND claimed_by='due' AND eligible_at<unixepoch()-?");
    q.addBindValue(staleSeconds);
    if (!q.exec()) return 0;
    recovered += q.numRowsAffected();
    // 新来源回顾的租约过期：回到 deferred，等待 claimPrivateReflection 重领
    // （仍受来源 24 小时窗口约束，超窗则不重领）。
    q.prepare("UPDATE private_reflection SET state='deferred',claimed_by=NULL,"
              "eligible_at=unixepoch(),updated_at=unixepoch() "
              "WHERE state='pending' AND (claimed_by IS NULL OR claimed_by<>'due') "
              "AND eligible_at<unixepoch()-?");
    q.addBindValue(staleSeconds);
    if (!q.exec()) return 0;
    recovered += q.numRowsAffected();
    if (!m_db.commit()) return 0;
    rollback.dismiss();
    if (recovered > 0)
        qInfo() << "[Concern] stale_reflections_recovered=" << recovered;
    return recovered;
}

int MemoryManager::recoverStaleContacts(int staleSeconds)
{
    if (staleSeconds < 0) staleSeconds = 0;
    // at-most-once：领取后崩溃时无法知道消息是否已经交给渠道，只能按"可能已送达"落 unknown，
    // 绝不重发（unknown 不在 claim/retryable 的可选状态里）。
    QSqlQuery q(m_db);
    q.prepare("UPDATE reflection_contact SET outcome='unknown',last_outcome='dispatch_abandoned',"
              "updated_at=unixepoch() WHERE outcome='dispatching' "
              "AND coalesce(last_attempt_at,0)>0 AND last_attempt_at<unixepoch()-?");
    q.addBindValue(staleSeconds);
    if (!q.exec()) return 0;
    const int abandoned=q.numRowsAffected();
    if (abandoned>0)
        qInfo()<<"[ReflectionContact] dispatch_abandoned count="<<abandoned;
    return abandoned;
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

MemoryManager::ReflectionCommitResult MemoryManager::finishPrivateReflection(qlonglong turn, int version, const QString &choice, const QString &note,
                                            const QJsonObject &contact,const QJsonArray &consulted,
                                            const QJsonObject &prior,const QString &priorAction,
                                            const QJsonObject &concern)
{
    const auto fail = [](const char *code) {
        return ReflectionCommitResult{QString::fromLatin1(code), {}};
    };
    QString contactRejection;
    if (choice != "rest" && choice != "consider") return fail("invalid_internal_structure");
    if (choice == "consider" && (note.trimmed().isEmpty() || note.size()>240)) return fail("invalid_internal_structure");
    if (!m_db.transaction()) return fail("db_error");
    auto rollback = qScopeGuard([this]{ m_db.rollback(); });
    QSqlQuery q(m_db);
    q.prepare("SELECT h.channel,h.session_id,s.input,r.version,r.state" + sourceRows +
              "JOIN private_reflection r ON r.turn_id=h.id WHERE " + eligible + " AND h.id=?");
    q.addBindValue(turn);
    if (!q.exec()) return fail("db_error");
    if (!q.next()) return fail("source_unavailable");
    if (q.value(3).toInt()!=version || q.value(4).toString()!="pending")
        return fail("version_conflict");
    const MemoryReadPolicy policy;
    const auto channel = q.value(0).toString(), session = q.value(1).toString();
    if (!policy.allows(channel,session,q.value(2).toString())) return fail("permission_denied");
    if (!policy.allows(channel,session,note)) return fail("internal_note_filtered");
    q.finish();
    if (!priorAction.isEmpty() && (prior.isEmpty() ||
        (priorAction!="resolve" && !(priorAction=="revise" && choice=="consider")))) return fail("invalid_internal_structure");
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
            || current["version"].toInteger()!=prior["version"].toInteger()) return fail("prior_conflict");
        if (!visibleSession(current["source_channel"].toString(),current["source_session"].toString(),
                            channel,session)) return fail("prior_permission_changed");
        if (!basisVisible(m_db,previous,policy,channel,session)) return fail("prior_evidence_unavailable");
        // The new note may rely on any evidence that supported the previous note.
        q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) SELECT ?,source FROM reflection_basis WHERE turn=?");
        q.addBindValue(turn);
        q.addBindValue(previous);
        if (!q.exec()) return fail("db_error");
        q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) VALUES(?,?)");
        q.addBindValue(turn);
        q.addBindValue(previous);
        if (!q.exec()) return fail("db_error");
        if (!priorAction.isEmpty()) {
            q.prepare("INSERT INTO reflection_change(turn,prior_turn,action) VALUES(?,?,?)");
            q.addBindValue(turn);
            q.addBindValue(previous);
            q.addBindValue(priorAction);
            if (!q.exec()) return fail("db_error");
        }
    }
    for (const auto &value : consulted) {
        const auto row=value.toObject();
        q.prepare("SELECT user_input,channel,session_id FROM chat_history WHERE id=? AND subject='user' AND sensitive=0");
        q.addBindValue(row["turn"].toInteger());
        if (!q.exec()) return fail("db_error");
        if (!q.next()) return fail("evidence_unavailable");
        const auto text=q.value(0).toString();
        if (row["turn"].toInteger()==turn || !visibleSession(q.value(1).toString(),q.value(2).toString(),channel,session) ||
            text.left(512)!=row["text"].toString()) return fail("evidence_changed");
        if (!policy.allows(q.value(1).toString(),q.value(2).toString(),text)) return fail("evidence_permission_changed");
        q.finish();
        q.prepare("INSERT OR IGNORE INTO reflection_basis(turn,source) VALUES(?,?)");
        q.addBindValue(turn); q.addBindValue(row["turn"].toInteger());
        if (!q.exec()) return fail("db_error");
    }
    if (choice=="consider") {
        // 回顾模型可以顺带给出关注的结构化描述（topic/watch/matter/下一次检查时间）。
        // 空字符串表示这次没给，保留旧值；0 分钟表示只在新证据或显式唤醒时再看，落库为 0。
        const auto topic=concern["topic"].toString().trimmed().left(60);
        const auto watch=concern["watch"].toString().trimmed().left(200);
        const auto matter=concern["matter"].toString().trimmed();
        if ((!topic.isEmpty() && !policy.allows(channel,session,topic))
            || (!watch.isEmpty() && !policy.allows(channel,session,watch))
            || (!matter.isEmpty() && !policy.allows(channel,session,matter))) return fail("internal_concern_filtered");
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
    if (!q.exec()) return fail("db_error");
    if (q.numRowsAffected()!=1) return fail("cas_conflict");
    if (choice=="consider" && !contact.isEmpty() && ConfigManager::instance().getProactiveConfig().enabled) {
        const auto timing = contact["timing"].toString();
        const auto reason = contact["reason"].toString().trimmed();
        const auto message = contact["message"].toString().trimmed();
        const bool shapeValid = (timing=="private" || timing=="next_chat" || timing=="now")
            && !reason.isEmpty() && reason.size()<=240 && policy.allows(channel,session,reason)
            && (timing!="now" || (!message.isEmpty() && message.size()<=300
                                  && policy.allows(channel,session,message)));
        if (!shapeValid) contactRejection = QStringLiteral("invalid_contact");
        const bool newContract = contactUsesNewContract(contact);
        QString basisType,basisRefs,motivationJson,claimsJson,semanticStatus;
        bool writeContact=shapeValid;
        bool withheldContact=false;
        if (writeContact && newContract) {
            const auto check=checkNewContractContact(this,channel,session,turn,contact,timing);
            motivationJson=check.motivationJson;
            claimsJson=check.claimsJson;
            semanticStatus=check.semanticStatus;
            withheldContact=check.withheld;
            if (withheldContact) contactRejection=check.reason;
        } else if (writeContact && timing=="now") {
            // "现在联系"必须先通过服务端依据核验；不通过只是不写联系意图，
            // 本次仍按 consider 落库（上面的关注更新已生效），不因依据不足而 defer。
            const auto rejection=validateContactBasis(channel,session,turn,contact);
            if (!rejection.isEmpty()) {
                qInfo()<<"[Reflection] contact_rejected turn="<<turn<<"version="<<version
                       <<"type="<<contact["basis"].toObject()["type"].toString()
                       <<"reason="<<rejection;
                writeContact=false;
                contactRejection=rejection;
            } else {
                const auto basis=contact["basis"].toObject();
                basisType=basis["type"].toString();
                basisRefs=QString::fromUtf8(QJsonDocument(basis["refs"].toArray())
                                                .toJson(QJsonDocument::Compact));
            }
        }
        if (writeContact) {
            if (newContract) {
                // 新契约把动机/claims/语义状态与联系意图同一行落库，供投递前复核与事后审计。
                q.prepare("INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome,"
                          "channel,session,motivation_json,claims_json,semantic_status) "
                          "VALUES(?,?,?,?,?,?,?,?,?,?,?)");
                q.addBindValue(turn);
                q.addBindValue(version);
                q.addBindValue(timing);
                q.addBindValue(reason);
                q.addBindValue(timing=="now" ? message : QString());
                q.addBindValue(withheldContact ? QStringLiteral("withheld")
                                               : timing=="now" ? QStringLiteral("proposed")
                                                                : QStringLiteral("retained"));
                q.addBindValue(channel);
                q.addBindValue(session);
                q.addBindValue(motivationJson.isEmpty() ? QVariant() : QVariant(motivationJson));
                q.addBindValue(claimsJson.isEmpty() ? QVariant() : QVariant(claimsJson));
                q.addBindValue(semanticStatus.isEmpty() ? QVariant() : QVariant(semanticStatus));
            } else {
                // basis_type/basis_refs 与联系意图同一行落库；非 now 的联系留 NULL。
                q.prepare("INSERT INTO reflection_contact(turn_id,version,timing,reason,message,outcome,"
                          "basis_type,basis_refs) VALUES(?,?,?,?,?,?,?,?)");
                q.addBindValue(turn);
                q.addBindValue(version);
                q.addBindValue(timing);
                q.addBindValue(reason);
                q.addBindValue(timing=="now" ? message : QString());
                q.addBindValue(timing=="now" ? "proposed" : "retained");
                q.addBindValue(basisType.isEmpty() ? QVariant() : QVariant(basisType));
                q.addBindValue(basisRefs.isEmpty() ? QVariant() : QVariant(basisRefs));
            }
            if (!q.exec()) return fail("db_error");
        }
    }
    if (!m_db.commit()) return fail("db_error");
    rollback.dismiss();
    return {{}, contactRejection};
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
    // v28：事务领取前用持久化依据重验。提出联系到真正投递之间可能隔了几秒到几分钟，
    // 期间提醒可能已被提醒链发送/取消、活动可能已分享、依据可能已失效或出现新证据；
    // 只有"此刻仍站得住"的 now 联系才允许领取，否则落 withheld（basis_expired）不再发送。
    q.prepare("SELECT c.timing,coalesce(c.basis_type,''),coalesce(c.basis_refs,''),"
              "coalesce(c.claims_json,''),coalesce(c.motivation_json,''),"
              "coalesce(c.semantic_status,'') "
              "FROM reflection_contact c WHERE c.turn_id=? AND c.version=?");
    q.addBindValue(turn);
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return {};
    const auto contactTiming=q.value(0).toString();
    const auto basisType=q.value(1).toString();
    const auto basisRefsText=q.value(2).toString();
    const auto claimsText=q.value(3).toString();
    const auto motivationText=q.value(4).toString();
    const auto semanticStatus=q.value(5).toString();
    q.finish();
    const bool newContract=!claimsText.isEmpty() || !motivationText.isEmpty();
    if (newContract && contactTiming==QStringLiteral("now")) {
        // 切片 C：领取前用同一套确定性规则复核 claims，并拒绝已标记 withheld 的意图。
        // 语义结论在提出联系时已写入 semantic_status；这里不发起新的模型请求。
        QString rejection;
        if (semanticStatus==QStringLiteral("withheld")
            || semanticStatus==QStringLiteral("repair_needed")
            || semanticStatus==QStringLiteral("unverified"))
            rejection=QStringLiteral("semantic_withheld");
        if (rejection.isEmpty()) {
            const ActionPolicy policy(this);
            const auto claims=QJsonDocument::fromJson(claimsText.toUtf8()).array();
            for (const auto &value : claims) {
                const auto validation=policy.validateClaimDeterministic(sourceChannel,sourceSession,
                                                                        turn,value.toObject());
                if (validation.result!=ClaimValidation::Entailed) {
                    rejection=validation.reason;
                    break;
                }
            }
        }
        if (!rejection.isEmpty()) {
            q.prepare("UPDATE reflection_contact SET outcome='withheld',last_outcome='claims_expired',"
                      "updated_at=unixepoch() WHERE turn_id=? AND version=? "
                      "AND outcome IN ('proposed','rejected')");
            q.addBindValue(turn);
            q.addBindValue(version);
            if (!q.exec()) return {};
            qInfo()<<"[ReflectionContact] claims_expired turn="<<turn<<"version="<<version
                   <<"reason="<<rejection;
            if (!m_db.commit()) return {};
            rollback.dismiss();
            return {};
        }
    } else if (contactTiming==QStringLiteral("now")) {
        // 旧行（v27 之前）没有 basis：新授权模型下不可投递，按 missing_basis 处理。
        QString rejection;
        QJsonArray refs;
        if (basisType.isEmpty() || basisRefsText.isEmpty()) {
            rejection=QStringLiteral("missing_basis");
        } else {
            const auto parsed=QJsonDocument::fromJson(basisRefsText.toUtf8()).array();
            if (parsed.isEmpty()) rejection=QStringLiteral("missing_basis");
            else refs=parsed;
        }
        if (rejection.isEmpty()) {
            const QJsonObject contact{
                {QStringLiteral("timing"),contactTiming},
                {QStringLiteral("basis"),QJsonObject{{QStringLiteral("type"),basisType},
                                                     {QStringLiteral("refs"),refs}}}};
            // 用联系意图落库时的同一来源渠道/会话重验，保证与提出时判定口径一致。
            rejection=validateContactBasis(sourceChannel,sourceSession,turn,contact);
        }
        if (!rejection.isEmpty()) {
            q.prepare("UPDATE reflection_contact SET outcome='withheld',last_outcome='basis_expired',"
                      "updated_at=unixepoch() WHERE turn_id=? AND version=? "
                      "AND outcome IN ('proposed','rejected')");
            q.addBindValue(turn);
            q.addBindValue(version);
            if (!q.exec()) return {};
            qInfo()<<"[ReflectionContact] basis_expired turn="<<turn<<"version="<<version
                   <<"reason="<<rejection;
            if (!m_db.commit()) return {};
            rollback.dismiss();
            return {};
        }
    }
    q.prepare("UPDATE reflection_contact SET outcome='dispatching',channel=?,session=?,"
              // 领取即记一次尝试时刻：崩溃后 recoverStaleContacts 靠它判断是否过期遗留。
              "last_attempt_at=unixepoch(),updated_at=unixepoch() "
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

bool MemoryManager::hasProposedContact(qlonglong turn,int version) const
{
    if (turn<=0 || version<=0) return false;
    QSqlQuery q(m_db);
    q.prepare("SELECT 1 FROM reflection_contact WHERE turn_id=? AND version=? "
              "AND timing='now' AND outcome='proposed' LIMIT 1");
    q.addBindValue(turn);
    q.addBindValue(version);
    return q.exec() && q.next();
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

bool MemoryManager::deferPrivateReflection(qlonglong turn, int version, int backoffSeconds)
{
    // R12-0：只有真正的失败才传非零退避；主动让出注意力（用户插话/关停）仍为 0，
    // 保持"eligible_at=now 立即可重领"的原有语义。
    const int backoff = qBound(0, backoffSeconds, 7 * 24 * 3600);
    QSqlQuery q(m_db);
    q.prepare("UPDATE private_reflection SET state='deferred',eligible_at=unixepoch()+?,"
              "updated_at=unixepoch() WHERE turn_id=? AND version=? AND state='pending'");
    q.addBindValue(backoff);
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

