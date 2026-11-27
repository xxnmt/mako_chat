#include "privatereview.h"
#include "actioncommitter.h"
#include "actionorigin.h"
#include "actionpolicy.h"
#include "configmanager.h"
#include "memorymanager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QUuid>

namespace {

// R12-0：回顾错误 -> 诊断阶段（只用于日志字段，不改变任何错误处理路径）。
// 阶段取自既有错误语义：parse=模型输出不可解析/被截断；schema=proposal 结构不合法；
// policy=授权/隐私过滤；verify=联系核验未通过；cancel=让出注意力；commit=提交/CAS/来源失效。
QString reviewErrorStage(const QString &error)
{
    // R12-1b 补充：ActivityReview 的成果/结构诊断码单独归到 schema/commit 阶段，
    // 便于从失败日志直接区分"模型输出格式错"与"存储/并发失败"。
    if (error == QStringLiteral("network_error") || error == QStringLiteral("truncated")
        || error == QStringLiteral("json_parse") || error == QStringLiteral("json_parse_failed"))
        return QStringLiteral("parse");
    if (error == QStringLiteral("filtered") || error == QStringLiteral("writing_disabled")
        || error == QStringLiteral("not_visible"))
        return QStringLiteral("policy");
    if (error == QStringLiteral("invalid_or_stale_proposal")
        || error == QStringLiteral("schema")
        || error == QStringLiteral("status_missing")
        || error == QStringLiteral("status_type_invalid")
        || error == QStringLiteral("status_enum_invalid")
        || error == QStringLiteral("missing_artifact")
        || error == QStringLiteral("missing_completion_condition")
        || error == QStringLiteral("missing_reflection")
        || error == QStringLiteral("share_intent_invalid")
        || error == QStringLiteral("invalid_activity_kind")
        || error == QStringLiteral("unsupported_external_activity")
        || error == QStringLiteral("artifact_ref_invalid")
        || error == QStringLiteral("artifact_mismatch")
        || error == QStringLiteral("draft_too_long"))
        return QStringLiteral("schema");
    if (error == QStringLiteral("cancelled") || error == QStringLiteral("cancel"))
        return QStringLiteral("cancel");
    // source_unavailable / evidence_changed / db_error / commit_conflict 等：提交前来源或状态失效。
    return QStringLiteral("commit");
}

QString reviewIsoFromEpoch(qint64 seconds)
{
    return seconds > 0
        ? QDateTime::fromSecsSinceEpoch(seconds).toUTC().toString(Qt::ISODateWithMs)
        : QString{};
}

// R12-1b：ActivityReview 结构化诊断。只记录字段形状/类型/枚举值/长度/阶段，
// 不记录模型输出或私聊正文，便于真实模型排查而不泄漏内容。
QString activityJsonTypeName(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Bool: return QStringLiteral("bool");
    case QJsonValue::Double: return QStringLiteral("number");
    case QJsonValue::String: return QStringLiteral("string");
    case QJsonValue::Array: return QStringLiteral("array");
    case QJsonValue::Object: return QStringLiteral("object");
    case QJsonValue::Undefined: break;
    }
    return QStringLiteral("missing");
}

// ActivityReview proposal 的提交前结构诊断码（与 finishActivity 的成果校验互补）。
QString activityProposalError(const QJsonObject &proposal)
{
    if (!proposal.contains(QStringLiteral("status")))
        return QStringLiteral("status_missing");
    if (!proposal.value(QStringLiteral("status")).isString())
        return QStringLiteral("status_type_invalid");
    static const QStringList allowedStatuses{QStringLiteral("active"), QStringLiteral("paused"),
                                             QStringLiteral("completed"),
                                             QStringLiteral("abandoned")};
    if (!allowedStatuses.contains(proposal.value(QStringLiteral("status")).toString()))
        return QStringLiteral("status_enum_invalid");
    if (proposal.contains(QStringLiteral("share_with_owner"))
        && !proposal.value(QStringLiteral("share_with_owner")).isBool())
        return QStringLiteral("share_intent_invalid");
    if (proposal.contains(QStringLiteral("share_request"))) {
        const QJsonValue shareRequest = proposal.value(QStringLiteral("share_request"));
        if (shareRequest.isString()) {
            if (shareRequest.toString().trimmed().size() > 120)
                return QStringLiteral("share_intent_invalid");
        } else if (!shareRequest.isBool()) {
            return QStringLiteral("share_intent_invalid");
        }
    }
    if (proposal.contains(QStringLiteral("share_with_owner"))
        && proposal.value(QStringLiteral("share_with_owner")).toBool()
        && proposal.value(QStringLiteral("status")).toString() != QStringLiteral("completed"))
        return QStringLiteral("share_intent_invalid");
    return {};
}

void logActivityProposalFailure(const QString &ref, int version, const QString &reason,
                                const QJsonObject &proposal, const QString &transportError)
{
    const QJsonValue status = proposal.value(QStringLiteral("status"));
    const QJsonValue draft = proposal.value(QStringLiteral("draft"));
    const QJsonValue condition = proposal.value(QStringLiteral("completion_condition"));
    const QJsonValue shareRequest = proposal.value(QStringLiteral("share_request"));
    const QJsonValue shareWithOwner = proposal.value(QStringLiteral("share_with_owner"));
    const QString activityKind =
        proposal.value(QStringLiteral("activity_kind")).toString().trimmed().left(16);
    const int draftLength = draft.isString() ? draft.toString().trimmed().size() : -1;
    const int conditionLength =
        condition.isString() ? condition.toString().trimmed().size() : -1;
    qInfo().noquote()
        << QStringLiteral("[Activity] review_failed ref=%1 version=%2 stage=%3 code=%4"
                          " status_present=%5 status_type=%6 status_enum=%7"
                          " activity_kind=%8 draft_len=%9 completion_condition_len=%10"
                          " share_with_owner_type=%11 share_request_type=%12 transport=%13")
               .arg(ref)
               .arg(version)
               .arg(reviewErrorStage(reason), reason)
               .arg(proposal.contains(QStringLiteral("status")) ? 1 : 0)
               .arg(activityJsonTypeName(status))
               .arg(status.isString() ? status.toString().left(24) : QStringLiteral("-"))
               .arg(activityKind.isEmpty() ? QStringLiteral("-") : activityKind)
               .arg(draftLength)
               .arg(conditionLength)
               .arg(activityJsonTypeName(shareWithOwner))
               .arg(activityJsonTypeName(shareRequest))
               .arg(transportError.isEmpty() ? QStringLiteral("-") : transportError);
}

// R12-1b：ActivityReview 的统一上下文。同时提供旧字段（id/interest/last_review）和统一字段
// （activity_ref/ref/version/origin/origin_kind/activity），LLM prompt 可以只认 activity_ref。
// last_review 保持旧 thought_result 的字段形状；internal 活动用同名键，模型不必区分两套存储。
QJsonObject activityReviewContext(const QString &ref, qlonglong id, int version,
                                  const QString &originKind, const QJsonObject &activity)
{
    QJsonObject lastReview;
    for (const auto &key : {QStringLiteral("status"), QStringLiteral("goal_kind"),
                            QStringLiteral("goal"), QStringLiteral("progress"),
                            QStringLiteral("next_step"), QStringLiteral("draft"),
                            QStringLiteral("artifact_ref"), QStringLiteral("reflection"),
                            QStringLiteral("mood"), QStringLiteral("paused_reason"),
                            QStringLiteral("deferred_reason"), QStringLiteral("share_intent")}) {
        if (!activity.contains(key)) continue;
        lastReview.insert(key, activity.value(key));
    }
    QJsonObject context{
        {QStringLiteral("kind"), QStringLiteral("activity")},
        {QStringLiteral("id"), id},
        {QStringLiteral("interest"), activity.value(QStringLiteral("interest"))},
        {QStringLiteral("last_review"), lastReview},
        {QStringLiteral("activity_ref"), ref},
        {QStringLiteral("ref"), ref},
        {QStringLiteral("version"), version},
        {QStringLiteral("origin_kind"), originKind},
        {QStringLiteral("activity"), activity}};
    // 来源渠道/会话给回顾快照定位用（群聊/第三方不会被扩权：活动只属于 owner 私聊域）。
    const QString channel = activity.value(QStringLiteral("channel")).toString();
    if (!channel.isEmpty()) context.insert(QStringLiteral("source_channel"), channel);
    const QString session = activity.value(QStringLiteral("session")).toString();
    if (!session.isEmpty()) context.insert(QStringLiteral("source_session"), session);
    return context;
}

// R12-1a：OpenWake 回顾的服务端时间上下文。复用 R12-0 的时间字段形状，但来源类是内部唤醒，
// 没有对应的用户消息，source_reported_at 固定 unknown；最近主人互动/最近成功表达仍按
// 主人私聊域只读查询（沿用 reviewInteractionTimes 的空 channel 语义）。
QJsonObject openWakeTimeContext(MemoryManager &store, const ProactiveConfig &options)
{
    const auto times = store.reviewInteractionTimes(QString(), QString());
    const auto known = [&times](const QString &key) {
        const QString value = times.value(key).toString().trimmed();
        return value.isEmpty() ? QStringLiteral("unknown") : value;
    };
    return QJsonObject{
        {QStringLiteral("now_local"), QDateTime::currentDateTime().toString(Qt::ISODate)},
        {QStringLiteral("now_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("source_reported_at"), QStringLiteral("unknown")},
        {QStringLiteral("last_owner_interaction_at"),
         known(QStringLiteral("last_owner_interaction_at"))},
        {QStringLiteral("last_confirmed_outbound_at"),
         known(QStringLiteral("last_confirmed_outbound_at"))},
        {QStringLiteral("quiet_hours"),
         QJsonObject{{QStringLiteral("start"), options.quietStart},
                     {QStringLiteral("end"), options.quietEnd}}}};
}

// R12-2b：OpenWake contact 的模型输出结构校验（只检查形状/长度；事实由服务端再核验）。
bool wakeContactShapeValid(const QJsonObject &contact)
{
    if (contact.isEmpty()) return false;
    const QString message = contact.value(QStringLiteral("message")).toString().trimmed();
    if (message.isEmpty() || message.size() > 1000) return false;
    if (!contact.value(QStringLiteral("reason")).isString()) return false;
    if (!contact.value(QStringLiteral("motivation")).isObject()) return false;
    if (!contact.value(QStringLiteral("claims")).isArray()) return false;
    if (contact.contains(QStringLiteral("core_claim_ids"))
        && !contact.value(QStringLiteral("core_claim_ids")).isArray())
        return false;
    return true;
}

// R12-2b：internal 活动分享的服务端提案请求。origin/target/payload 全部由服务端组装：
// 模型只提供 draft（已持久化成果），不提供版本/哈希/目标/元数据。
QJsonObject buildInternalShareRequest(MemoryManager &store, const QString &activityRef,
                                      const QJsonObject &intent, const QString &draft)
{
    if (!activityRef.startsWith(QStringLiteral("activity/"))) return {};
    bool ok = false;
    const qlonglong activityId =
        activityRef.mid(QStringLiteral("activity/").size()).toLongLong(&ok);
    const int artifactVersion = intent.value(QStringLiteral("artifact_version")).toInt();
    if (!ok || activityId <= 0 || artifactVersion <= 0 || draft.trimmed().isEmpty()) return {};
    QJsonObject target = store.chooseProactiveTarget(draft, true);
    QString channel = target.value(QStringLiteral("channel")).toString();
    QString session = target.value(QStringLiteral("session")).toString();
    if (!target.value(QStringLiteral("found")).toBool()) {
        // 提案需要 target 才能持久化；没有可达目标时用主人桌宠默认域占位，
        // 提交层会返回 target_unreachable 并保留 verified 等下一次恢复。
        channel = Channel::kPet;
        session = Channel::kDefaultSession;
    }
    const ActionOrigin origin = ActionOrigin::forActivity(activityId, artifactVersion,
                                                          QStringLiteral("owner"),
                                                          channel, session);
    if (!origin.isValid()) return {};
    const QJsonObject payload{
        {QStringLiteral("draft"), draft},
        {QStringLiteral("artifact_ref"), activityRef},
        {QStringLiteral("artifact_version"), artifactVersion},
        {QStringLiteral("artifact_hash"), intent.value(QStringLiteral("content_hash"))}};
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("activity_share")},
        {QStringLiteral("action_key"), origin.actionKey(QStringLiteral("activity_share"))},
        {QStringLiteral("origin"), origin.toJson()},
        {QStringLiteral("target"),
         QJsonObject{{QStringLiteral("channel"), channel},
                     {QStringLiteral("session_id"), session}}},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("claims"), QJsonArray{}},
        {QStringLiteral("expires_at"), QDateTime::currentSecsSinceEpoch() + 3600}};
}

} // namespace

PrivateReview::PrivateReview(MemoryManager &memory, QObject *parent)
    : QObject(parent), store(memory)
{
}

QString PrivateReview::kindTag(CognitiveOrigin::Kind kind)
{
    switch (kind) {
    case CognitiveOrigin::Reflection: return QStringLiteral("[Reflection]");
    case CognitiveOrigin::Concern: return QStringLiteral("[Concern]");
    case CognitiveOrigin::Activity: return QStringLiteral("[Activity]");
    case CognitiveOrigin::OpenWake: return QStringLiteral("[OpenWake]");
    }
    return QStringLiteral("[Review]");
}

QString PrivateReview::kindName(CognitiveOrigin::Kind kind)
{
    return CognitiveOrigin::kindNameOf(kind);
}

QString PrivateReview::sourceRef(const Attempt &attempt)
{
    return attempt.origin.ref;
}

QString PrivateReview::reviewStage(const QJsonObject &proposal, const QString &error, bool committed)
{
    // 联系核验结果已经在 proposal.contact.semantic_status 上（withheld/retained/entailed），
    // 这是"核验阶段"而不是提交失败，单独标出来便于区分。
    const auto contact = proposal.value(QStringLiteral("contact")).toObject();
    if (contact.value(QStringLiteral("semantic_status")).toString()
        == QStringLiteral("withheld"))
        return QStringLiteral("verify");
    if (!error.isEmpty()) return reviewErrorStage(error);
    if (!committed) return proposal.isEmpty() ? QStringLiteral("parse")
                                              : QStringLiteral("commit");
    return QStringLiteral("commit");
}

bool PrivateReview::errorCountsAsFailure(const QString &error)
{
    const QString text = error.trimmed();
    if (text.isEmpty()) return true;
    // 策略/权限拦截：不计入认知失败，但调用方仍按原语义 defer。
    if (text == QStringLiteral("filtered") || text == QStringLiteral("writing_disabled")
        || text == QStringLiteral("permission_denied")
        || text == QStringLiteral("not_visible"))
        return false;
    // 来源已变/CAS 冲突不是"她想错了"，记录诊断但不累计连续失败。
    if (text == QStringLiteral("invalid_or_stale_proposal")
        || text == QStringLiteral("evidence_changed")
        || text == QStringLiteral("source_unavailable")
        || text == QStringLiteral("commit_conflict")
        || text == QStringLiteral("cas_conflict"))
        return false;
    // 网络/截断/JSON 解析/schema/成果真实性/提交失败/DB 错误都算一次真实失败。
    return true;
}

bool PrivateReview::originMatches(const Attempt &attempt, const QJsonObject &proposal,
                                  const QString &error) const
{
    // R12-1a 严格来源校验：所有真实回顾来源（含失败回调）都必须携带合法 _origin。
    // error 只用于日志/失败分类，不再放宽来源校验；缺失 _origin 的旧 proposal 一律拒绝。
    Q_UNUSED(error);
    const auto originJson = proposal.value(QStringLiteral("_origin")).toObject();
    if (originJson.isEmpty()) return false;
    CognitiveOrigin echoed;
    if (!CognitiveOrigin::fromJson(originJson, &echoed)) return false;
    return attempt.origin.sameIdentity(echoed);
}

void PrivateReview::recordFailure(const Attempt &attempt, const QString &reason,
                                  const QString &stage, bool countsAsFailure)
{
    const QString kind = attempt.origin.kindName();
    const QString ref = attempt.origin.ref;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QString recordError;
    const bool recorded = store.recordReviewFailure(kind, ref, stage, reason,
                                                    countsAsFailure, &recordError);
    if (!recorded) {
        qWarning().noquote()
            << QStringLiteral("[Review] failure_not_recorded kind=%1 ref=%2 version=%3"
                              " stage=%4 reason=%5 error=%6")
                   .arg(kind, ref)
                   .arg(attempt.origin.version)
                   .arg(stage, reason, recordError.isEmpty() ? QStringLiteral("-") : recordError);
    } else {
        const auto failure = store.reviewFailure(kind, ref);
        const int failures = failure.value(QStringLiteral("consecutive_failures")).toInt();
        const qint64 nextRetry = failure.value(QStringLiteral("next_retry_at")).toInteger();
        const qint64 suspended = failure.value(QStringLiteral("suspended_until")).toInteger();
        if (suspended > now) {
            qInfo().noquote()
                << QStringLiteral("[Review] suspended kind=%1 ref=%2 version=%3 failures=%4"
                                  " reason=%5 next_eligible=%6")
                       .arg(kind, ref)
                       .arg(attempt.origin.version)
                       .arg(failures)
                       .arg(reason, reviewIsoFromEpoch(suspended));
        } else if (countsAsFailure) {
            const qint64 seconds = qMax<qint64>(0, nextRetry - now);
            qInfo().noquote()
                << QStringLiteral("[Review] backoff kind=%1 ref=%2 version=%3 failures=%4"
                                  " seconds=%5 reason=%6 next_eligible=%7")
                       .arg(kind, ref)
                       .arg(attempt.origin.version)
                       .arg(failures)
                       .arg(seconds)
                       .arg(reason, reviewIsoFromEpoch(nextRetry));
        } else {
            qInfo().noquote()
                << QStringLiteral("[Review] deferred kind=%1 ref=%2 version=%3"
                                  " reason=%4 counts=false")
                       .arg(kind, ref)
                       .arg(attempt.origin.version)
                       .arg(reason);
        }
    }
    if (countsAsFailure) return;
    // 非认知失败：保持既有"立即放回队列"语义（权限拦截 / 来源已变 / 用户主动让出）。
    bool deferred = false;
    switch (attempt.origin.kind) {
    case CognitiveOrigin::Reflection:
        deferred = store.deferPrivateReflection(attempt.origin.id, attempt.origin.version);
        break;
    case CognitiveOrigin::Concern:
        deferred = store.deferConcernReview(attempt.origin.id, attempt.origin.version, reason);
        break;
    case CognitiveOrigin::Activity:
        // R12-1b：legacy thought/<id> 与 internal activity/<id> 共用统一 defer（不改 lifecycle）。
        deferred = store.deferActivity(attempt.origin.ref, attempt.origin.version, 0, reason);
        break;
    case CognitiveOrigin::OpenWake:
        deferred = store.deferOpenWake(attempt.origin.id, attempt.origin.version, 0, reason);
        break;
    }
    if (!deferred) {
        qWarning().noquote()
            << QStringLiteral("[Review] defer_failed kind=%1 ref=%2 version=%3 reason=%4")
                   .arg(kind, ref)
                   .arg(attempt.origin.version)
                   .arg(reason);
    }
}

WakeResult PrivateReview::wake(int dailyLimit, int cooldownSeconds)
{
    // 旧调用/旧测试：不启用 OpenWake，保持 R12-0 的三步顺序。
    return wakeInternal(dailyLimit, cooldownSeconds, nullptr, true);
}

WakeResult PrivateReview::wake(int dailyLimit, int cooldownSeconds, const OpenWakeConfig &openWake)
{
    // 按任务契约，调度顺序只由 openWake.enabled / daily_limit 决定：启用 OpenWake 时
    // 仍先走现有 Activity→Concern→Reflection，三步无候选/被预算挡住后才尝试 OpenWake。
    return wakeInternal(dailyLimit, cooldownSeconds, openWake.enabled ? &openWake : nullptr, true);
}

WakeResult PrivateReview::wakeInternal(int dailyLimit, int cooldownSeconds,
                                       const OpenWakeConfig *openWake, bool nonOpenAllowed)
{
    WakeResult result;
    result.kind = QStringLiteral("none");
    result.reason = QStringLiteral("no_candidate");
    if (active) {
        // 已有在途回顾：不重复领取，但把当前在途的类型化状态带回给调用方。
        result.kind = kindName(active->origin.kind);
        result.sourceRef = sourceRef(*active);
        result.version = active->origin.version;
        result.reason = QStringLiteral("busy");
        return result;
    }

    // OpenWake 本地到期检查：只读状态，不调用模型、不消耗预算。
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool openWakeDue = false;
    bool openWakeAttempted = false;
    if (openWake && dailyLimit > 0) {
        QString scheduleError;
        if (!store.ensureOpenWakeScheduled(QStringLiteral("owner"),
                                           openWake->initialDelaySeconds, &scheduleError)) {
            qWarning() << "[OpenWake] schedule_failed" << scheduleError;
        }
        const auto state = store.openWakeState(QStringLiteral("owner"));
        const qint64 dueAt = state.value(QStringLiteral("next_due_at")).toInteger();
        const qint64 suspendedUntil = state.value(QStringLiteral("suspended_until")).toInteger();
        openWakeDue = dueAt > 0 && dueAt <= now
            && (suspendedUntil == 0 || suspendedUntil <= now);
        // 公平性：到期后先让出 maxNonOpenJobsWhenDue 次普通回顾，再优先 OpenWake。
        if (openWakeDue
            && state.value(QStringLiteral("non_open_since_due")).toInt()
                >= openWake->maxNonOpenJobsWhenDue) {
            openWakeAttempted = true;
            const auto openWakeResult = wakeOpenWake(*openWake, dailyLimit, cooldownSeconds);
            if (openWakeResult.started) return openWakeResult;
            if (openWakeResult.reason != QStringLiteral("not_due")
                && openWakeResult.reason != QStringLiteral("no_candidate"))
                result = openWakeResult;
        }
    }

    // 第二步：原有 到期活动 → 到期关注 → 新来源回顾。OpenWake 单独运行时跳过它们。
    WakeResult nonOpenResult;
    nonOpenResult.kind = QStringLiteral("none");
    nonOpenResult.reason = QStringLiteral("no_candidate");
    if (nonOpenAllowed) {
        nonOpenResult = wakeNonOpen(dailyLimit, cooldownSeconds);
        if (nonOpenResult.started) return nonOpenResult;
    }

    // 第三步：普通来源都无候选/被预算挡住时，若 OpenWake 到期再尝试领取。
    if (openWake && dailyLimit > 0 && openWakeDue && !openWakeAttempted) {
        const auto openWakeResult = wakeOpenWake(*openWake, dailyLimit, cooldownSeconds);
        if (openWakeResult.started) return openWakeResult;
        if (openWakeResult.reason != QStringLiteral("not_due")
            && openWakeResult.reason != QStringLiteral("no_candidate"))
            return openWakeResult;
    }
    if (nonOpenResult.reason != QStringLiteral("no_candidate")) return nonOpenResult;
    // fairness 阶段已经尝试并记录了阻塞原因时，用它解释这次 tick。
    return result;
}

WakeResult PrivateReview::wakeNonOpen(int dailyLimit, int cooldownSeconds)
{
    WakeResult result;
    result.kind = QStringLiteral("none");
    result.reason = QStringLiteral("no_candidate");

    // 优先级：到期活动 → 到期关注 → 新来源回顾。活动和关注都有"到期"，
    // 新来源回顾有 24h 窗口、可以补，所以先处理有时效的，避免回顾额度被新来源吃掉。
    QString claimReason;
    QDateTime nextEligibleAt;
    // 关注领取被预算/冷却/DB 挡住时仍按原行为继续尝试新来源回顾（两者共用预算）；
    // 若新来源也没有候选，再用这里的原因解释这次 tick 为什么没启动。
    QString blockedReason;
    QDateTime blockedNextEligibleAt;
    QJsonObject context;
    auto kind = CognitiveOrigin::Activity;
    const QString scopeKey = QStringLiteral("owner");
    // R12-1b：ActivityReview 统一走 nextDueActivity + claimActivity（legacy/internal 同一契约）。
    // privateWriting=false 时 ActivityReview 不领取（不继续在途活动由 receive 拒绝），
    // 但关注/新来源回顾仍按原规则运行。
    if (ConfigManager::instance().getProactiveConfig().privateWriting) {
        const auto due = store.nextDueActivity(scopeKey);
        if (!due.isEmpty()) {
            const QString activityRef = due.value(QStringLiteral("ref")).toString();
            const int expectedVersion = due.value(QStringLiteral("version")).toInt();
            const auto claim = store.claimActivity(activityRef, expectedVersion, dailyLimit,
                                                   cooldownSeconds, &claimReason);
            if (claim.isEmpty()) {
                // 领取被预算/冷却/版本/CAS/DB 挡住：失败原因透传给调用方。
                result.kind = QStringLiteral("activity");
                result.reason = claimReason.isEmpty() ? QStringLiteral("db_error") : claimReason;
                result.sourceRef = activityRef;
                result.version = expectedVersion;
                return result;
            }
            const qlonglong activityId = claim.value(QStringLiteral("id")).toInteger();
            const int currentVersion = claim.value(QStringLiteral("version")).toInt();
            const auto activity = claim.value(QStringLiteral("activity")).toObject();
            const QString claimOriginKind = claim.value(QStringLiteral("origin_kind")).toString();
            // R12-1b 补充：ActivityReview 的模型上下文只能来自可见投影，
            // 不可见活动（盲区/敏感/越界渠道/长记忆关闭）不进入回顾，直接放回队列。
            const QJsonObject visibleActivity = store.activityForModel(
                activityRef, activity.value(QStringLiteral("channel")).toString(),
                activity.value(QStringLiteral("session")).toString(), QStringLiteral("user"),
                true);
            if (visibleActivity.isEmpty()) {
                store.deferActivity(activityRef, currentVersion, 0,
                                    QStringLiteral("not_visible"));
                result.kind = QStringLiteral("activity");
                result.reason = QStringLiteral("not_visible");
                result.sourceRef = activityRef;
                result.version = currentVersion;
                return result;
            }
            // origin_kind 对模型是存储来源（legacy/internal）；具体产生方（foreground/open_wake）
            // 单独放 source_origin_kind，避免把 activity 的来源类型和存储类型混为一谈。
            const QString storageKind = activityRef.startsWith(QStringLiteral("activity/"))
                ? QStringLiteral("internal") : QStringLiteral("legacy");
            context = activityReviewContext(activityRef, activityId, currentVersion,
                                            storageKind, visibleActivity);
            if (!claimOriginKind.isEmpty())
                context.insert(QStringLiteral("source_origin_kind"), claimOriginKind);
            kind = CognitiveOrigin::Activity;
        }
    }
    if (context.isEmpty()) {
        context = store.claimDueConcern(dailyLimit, cooldownSeconds, &claimReason, &nextEligibleAt);
        kind = CognitiveOrigin::Concern;
        if (context.isEmpty() && claimReason != QStringLiteral("no_candidate")) {
            blockedReason = claimReason;
            blockedNextEligibleAt = nextEligibleAt;
        }
    }
    if (context.isEmpty()) {
        claimReason.clear();
        nextEligibleAt = QDateTime{};
        context = store.claimPrivateReflection(dailyLimit, cooldownSeconds, &claimReason, &nextEligibleAt);
        kind = CognitiveOrigin::Reflection;
        if (context.isEmpty()) {
            const bool reflectionBlocked = !claimReason.isEmpty()
                && claimReason != QStringLiteral("no_candidate");
            if (reflectionBlocked) {
                result.reason = claimReason;
                result.kind = QStringLiteral("reflection");
                result.nextEligibleAt = nextEligibleAt;
            } else if (!blockedReason.isEmpty()) {
                result.reason = blockedReason;
                result.kind = QStringLiteral("concern");
                result.nextEligibleAt = blockedNextEligibleAt;
            } else {
                result.reason = QStringLiteral("no_candidate");
                result.kind = QStringLiteral("none");
            }
            return result;
        }
    }
    qlonglong id = 0;
    int version = 0;
    if (kind == CognitiveOrigin::Activity) {
        // 活动上下文已经带统一字段 id/version（不能 take 掉，LLM 需要 activity_ref/activity）。
        id = context.value(QStringLiteral("id")).toInteger();
        version = context.value(QStringLiteral("version")).toInt();
    } else {
        id = context.take(QStringLiteral("id")).toInteger();
        version = context.take(QStringLiteral("version")).toInt();
    }
    if (id <= 0 || version < 0) {
        // 领取上下文缺少可提交的行标识：不进入在途状态，交给下一轮重新领取。
        // 注意 version==0 是新建活动/首次回顾的合法版本，不能拒绝。
        result.kind = kindName(kind);
        result.reason = QStringLiteral("storage_error");
        return result;
    }
    if (kind == CognitiveOrigin::Concern) {
        // 记忆层的契约要求上下文带 kind=concern；这里显式保证，避免路由到活动提示词。
        context.insert(QStringLiteral("kind"), QStringLiteral("concern"));
    }
    const auto options = ConfigManager::instance().getProactiveConfig();
    CognitiveOrigin origin;
    switch (kind) {
    case CognitiveOrigin::Activity:
        origin = CognitiveOrigin::forActivityRef(
            context.value(QStringLiteral("activity_ref")).toString(), version);
        break;
    case CognitiveOrigin::Concern: origin = CognitiveOrigin::forConcern(id, version); break;
    case CognitiveOrigin::Reflection: origin = CognitiveOrigin::forReflection(id, version); break;
    case CognitiveOrigin::OpenWake: break; // 不会走到这里
    }
    if (!origin.isValid()) {
        result.kind = kindName(kind);
        result.reason = QStringLiteral("storage_error");
        return result;
    }
    // R12-1a：非 OpenWake 来源没有服务端持久 runId，领取时生成独立 UUID；
    // 该 runId 随 requested context 下发并要求模型原样回显，迟到/伪造回调无法通过校验。
    if (origin.runId.isEmpty())
        origin.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (origin.runId.isEmpty()) {
        // 理论上不会发生（UUID 生成失败）；仍按存储错误处理，绝不进入无身份的在途状态。
        result.kind = kindName(kind);
        result.reason = QStringLiteral("storage_error");
        return result;
    }
    context.insert(QStringLiteral("origin"), origin.toJson());
    active = Attempt{origin, options.enabled && !options.targets.isEmpty()};
    // B1：当前活动（仅活动回顾有"活动"语义；关注/新来源回顾/OpenWake 不写入 current_activity）。
    if (kind == CognitiveOrigin::Activity) {
        store.setAttentionState(QStringLiteral("current_activity"), origin.ref);
        qInfo() << "[Attention] current_activity=" << origin.ref
                << "version=" << version;
    }
    qInfo() << kindTag(kind) + QStringLiteral(" reviewing") << id << version;
    qInfo().noquote()
        << QStringLiteral("[Review] attempt kind=%1 ref=%2 version=%3 run_id=%4 scope=%5"
                          " epoch=%6 steps_limit=%7 current_local_time=%8")
               .arg(kindName(kind), origin.ref)
               .arg(version)
               .arg(origin.runId.isEmpty() ? QStringLiteral("-") : origin.runId)
               .arg(origin.scope.isEmpty() ? QStringLiteral("-") : origin.scope)
               .arg(origin.epoch)
               .arg(0)
               .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    result.started = true;
    result.kind = kindName(kind);
    result.reason = QStringLiteral("started");
    result.sourceRef = origin.ref;
    result.version = version;
    emit requested(id, version, context);
    return result;
}

WakeResult PrivateReview::wakeOpenWake(const OpenWakeConfig &openWake, int dailyLimit,
                                       int cooldownSeconds)
{
    WakeResult result;
    result.kind = QStringLiteral("open_wake");
    result.reason = QStringLiteral("no_candidate");
    const auto options = ConfigManager::instance().getProactiveConfig();
    QString reason;
    QDateTime nextEligibleAt;
    auto context = store.claimOpenWake(dailyLimit, cooldownSeconds, openWake.maxPerDay,
                                       openWake.sharedBudgetPercent,
                                       options.maxReviewFailures, &reason, &nextEligibleAt,
                                       QStringLiteral("owner"));
    if (context.isEmpty()) {
        result.reason = reason.isEmpty() ? QStringLiteral("no_candidate") : reason;
        result.nextEligibleAt = nextEligibleAt;
        return result;
    }
    const qlonglong id = context.take(QStringLiteral("id")).toInteger();
    const int version = context.take(QStringLiteral("version")).toInt();
    const QString runId = context.take(QStringLiteral("run_id")).toString();
    const QString scope = context.value(QStringLiteral("scope_key")).toString();
    const qint64 epoch = context.value(QStringLiteral("current_context_epoch")).toInteger();
    if (id <= 0 || version <= 0 || runId.isEmpty()) {
        // 领取上下文不自洽：不进入在途状态；下次 tick 由租约恢复/重新领取兜底。
        result.reason = QStringLiteral("storage_error");
        return result;
    }
    const auto origin = CognitiveOrigin::forOpenWake(id, version, runId,
                                                     scope.isEmpty() ? QStringLiteral("owner")
                                                                     : scope,
                                                     epoch);
    if (!origin.isValid()) {
        result.reason = QStringLiteral("storage_error");
        return result;
    }
    // 模型上下文：内部唤醒没有用户输入；带服务端时间/触发原因/最近内部记录，
    // R12-2b 起 allow_contact 来自独立配置（默认 false）：false 时 contact 一律不发送，
    // true 时才允许模型输出 contact 提案（仍要经过确定性+语义核验与提交层复核）。
    context.insert(QStringLiteral("kind"), QStringLiteral("open_wake"));
    context.insert(QStringLiteral("origin"), origin.toJson());
    context.insert(QStringLiteral("current_input"), QJsonValue::Null);
    const bool allowContact = ConfigManager::instance().openWakeAllowContact();
    context.insert(QStringLiteral("allow_contact"), allowContact);
    // R12-1b：start_activity 还要求 private_writing=true。
    context.insert(QStringLiteral("private_writing"),
                   ConfigManager::instance().getProactiveConfig().privateWriting);
    // 目标由服务端选：只给模型可读的授权摘要，模型不能自己指定 channel/session。
    if (allowContact) {
        QJsonArray targets;
        for (const auto &pair : options.targets)
            targets.append(QJsonObject{{QStringLiteral("channel"), pair.first},
                                       {QStringLiteral("session_id"), pair.second}});
        context.insert(QStringLiteral("contact_targets"), targets);
    }
    context.insert(QStringLiteral("trigger"), QStringLiteral("due"));
    context.insert(QStringLiteral("time_context"), openWakeTimeContext(store, options));
    context.insert(QStringLiteral("recent_open_wake_notes"),
                   store.recentOpenWakeNotes(2, QStringLiteral("owner")));
    active = Attempt{origin, false};
    qInfo() << "[OpenWake] reviewing" << id << version << "run=" << runId
            << "scope=" << origin.scope;
    qInfo().noquote()
        << QStringLiteral("[Review] attempt kind=open_wake ref=%1 version=%2 run_id=%3"
                          " scope=%4 epoch=%5 steps_limit=%6 current_local_time=%7")
               .arg(origin.ref)
               .arg(version)
               .arg(runId, origin.scope)
               .arg(origin.epoch)
               .arg(openWake.maxModelStepsPerWake)
               .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    result.started = true;
    result.reason = QStringLiteral("started");
    result.sourceRef = origin.ref;
    result.version = version;
    emit requested(id, version, context);
    return result;
}

void PrivateReview::yieldAttention(const QString &reason)
{
    if (!active) return;
    const auto attempt = *active;
    // B1：让出注意力时记录"切换原因"；OpenWake 不得写 attention_state.current_activity。
    store.setAttentionState(QStringLiteral("last_switch_reason"), reason);
    if (attempt.origin.kind != CognitiveOrigin::OpenWake)
        store.setAttentionState(QStringLiteral("current_activity"), QString());
    qInfo() << "[Attention] switch reason=" << reason;
    active.reset();
    bool saved = false;
    switch (attempt.origin.kind) {
    case CognitiveOrigin::Concern:
        saved = store.deferConcernReview(attempt.origin.id, attempt.origin.version, reason);
        break;
    case CognitiveOrigin::Reflection:
        // 用户插话/关停造成的主动让出：立即 defer，不累计失败、不退避。
        saved = store.deferPrivateReflection(attempt.origin.id, attempt.origin.version);
        break;
    case CognitiveOrigin::Activity:
        // R12-1b：统一 defer（legacy/internal 同一接口），只推下一次回顾时间，不改 lifecycle。
        saved = store.deferActivity(attempt.origin.ref, attempt.origin.version, 0, reason);
        break;
    case CognitiveOrigin::OpenWake:
        // R12-1a：主动让出不增失败计数，只把调度推到（可立即）再试。
        saved = store.deferOpenWake(attempt.origin.id, attempt.origin.version, 0, reason);
        break;
    }
    qInfo() << kindTag(attempt.origin.kind) + QStringLiteral(" yielded")
            << attempt.origin.id << attempt.origin.version << reason << saved;
    qInfo().noquote()
        << QStringLiteral("[Review] result kind=%1 ref=%2 version=%3 decision=yielded"
                          " stage=cancel reason=%4")
               .arg(kindName(attempt.origin.kind), attempt.origin.ref)
               .arg(attempt.origin.version)
               .arg(reason);
    emit cancelled();
}

void PrivateReview::receive(qlonglong id, int version, const QJsonObject &proposal,
                            const QString &error)
{
    if (!active || active->origin.id != id || active->origin.version != version) return;
    const auto attempt = *active;
    if (!originMatches(attempt, proposal, error)) {
        // 迟到的/伪造的回调：不提交、不发送，也不写失败（不是一次认知失败）。
        qInfo().noquote()
            << QStringLiteral("[Review] stale_callback kind=%1 ref=%2 version=%3"
                              " reason=origin_mismatch")
                   .arg(kindName(attempt.origin.kind), attempt.origin.ref)
                   .arg(attempt.origin.version);
        return;
    }
    const bool contactAllowed = attempt.contactAllowed
        && ConfigManager::instance().getProactiveConfig().enabled;
    active.reset();
    const auto ref = sourceRef(attempt);
    const auto logResult = [&](const QString &decision, const QString &stage, const QString &reason) {
        qInfo().noquote()
            << QStringLiteral("[Review] result kind=%1 ref=%2 version=%3 decision=%4"
                              " stage=%5 reason=%6")
                   .arg(kindName(attempt.origin.kind), ref)
                   .arg(attempt.origin.version)
                   .arg(decision, stage,
                        reason.trimmed().isEmpty() ? QStringLiteral("-") : reason);
    };

    if (attempt.origin.kind == CognitiveOrigin::OpenWake) {
        receiveOpenWake(attempt, proposal, error);
        return;
    }

    if (attempt.origin.kind == CognitiveOrigin::Reflection) {
        const auto choice = proposal["choice"].toString();
        const auto decision = choice.isEmpty() ? QStringLiteral("none") : choice;
        const auto contact = contactAllowed ? proposal["contact"].toObject() : QJsonObject{};
        auto evidence=proposal["_history"].toArray();
        for (const auto &row : proposal["_consulted"].toArray()) evidence.append(row);
        const auto prior=proposal["_prior"].toObject();
        const auto action=proposal["prior_action"].toString();
        // 新来源回顾可以顺带更新关注的结构化描述（topic/watch/next_check_after_minutes）。
        if (error.isEmpty() && store.finishPrivateReflection(id,version,choice,proposal["note"].toString(),
                                                             contact,evidence,prior,action,
                                                             proposal["concern"].toObject())) {
            qInfo() << "[Reflection] committed" << id << version << choice << contact["timing"].toString()
                    << "prior_turn=" << prior["source_turn"].toInteger() << "prior_action=" << action;
            // 只有服务端确实落库了 proposed 的 now 联系才通知投递层；
            // 依据被拒时只更新关注，不产生外部动作（也不再出现误导性的 withheld 日志）。
            if (choice=="consider" && contact["timing"].toString()=="now"
                && store.hasProposedContact(id,version))
                emit contactProposed(id,version);
            store.clearReviewFailure(kindName(attempt.origin.kind), ref);
            logResult(decision, reviewStage(proposal,error,true),
                      proposal["contact"].toObject()["withhold_reason"].toString());
        } else {
            const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
            // R12-1a：失败退避持久化到 review_failure（跨重启可靠），
            // 主动让出（yieldAttention）走另一条路径，不经过这里。
            recordFailure(attempt, reason, reviewStage(proposal,error,false),
                          errorCountsAsFailure(reason));
            logResult(decision, reviewStage(proposal,error,false), reason);
        }
        return;
    }
    if (attempt.origin.kind == CognitiveOrigin::Concern) {
        const auto contact = proposal["contact"].toObject();
        const auto action = proposal["concern_action"].toString();
        const auto decision = action.isEmpty() ? QStringLiteral("none") : action;
        if (error.isEmpty() && store.finishConcernReview(id,version,proposal)) {
            qInfo() << "[Concern] committed" << id << version << action << contact["timing"].toString();
            // 投递权限与安静时段由 AppController 的 canContactNow 再核验；这里只把"现在联系"的
            // 决定交给投递层，keep/revise/resolve/drop 只在记忆层落库，不产生外部消息。
            // 依据核验被拒时 finishConcernReview 仍返回 true 但不会写 contact 行，此时不 emit。
            if (action == "contact" && contact["timing"].toString() == "now"
                && store.hasProposedContact(id,version))
                emit contactProposed(id,version);
            store.clearReviewFailure(kindName(attempt.origin.kind), ref);
            logResult(decision, reviewStage(proposal,error,true),
                      contact["withhold_reason"].toString());
            return;
        }
        // 领取时已经扣过预算/冷却，失败把关注放回队列（持久失败行决定退避）。
        const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
        recordFailure(attempt, reason, reviewStage(proposal,error,false),
                      errorCountsAsFailure(reason));
        logResult(decision, reviewStage(proposal,error,false), reason);
        return;
    }
    // R12-1b：ActivityReview 统一提交（legacy/internal 同一 finishActivity + CAS）。
    // privateWriting 关闭后不再继续在途活动回顾：按策略拦截放回队列，不计认知失败。
    if (!ConfigManager::instance().getProactiveConfig().privateWriting) {
        recordFailure(attempt, QStringLiteral("writing_disabled"), QStringLiteral("policy"), false);
        logResult(QStringLiteral("none"), QStringLiteral("policy"), QStringLiteral("writing_disabled"));
        return;
    }
    // R12-1b 补充：提交前先做结构诊断（status 缺失/类型/枚举、分享请求形状）；
    // 所有失败日志只记录诊断码/字段形状/长度，不记录模型输出或私聊正文。
    QString proposalError;
    if (error.isEmpty()) proposalError = activityProposalError(proposal);
    QString finishError;
    QJsonObject finished;
    if (error.isEmpty() && proposalError.isEmpty())
        finished = store.finishActivity(ref, version, proposal, &finishError);
    const QString reason = !error.isEmpty()
        ? error
        : (!proposalError.isEmpty()
               ? proposalError
               : (finished.isEmpty()
                      ? (finishError.isEmpty() ? QStringLiteral("invalid_or_stale_proposal")
                                               : finishError)
                      : QString{}));
    if (!reason.isEmpty()) {
        // 领取已经计入冷却与每日预算；失败走统一 review_failure（legacy/internal 同一接口）。
        // R12-1b 补充：ActivityReview 的阶段必须体现具体错误码（schema/commit/parse），
        // 不能一律归为 commit；日志/失败行只记录诊断码与长度。
        const QString stage = error.isEmpty() ? reviewErrorStage(reason)
                                              : reviewStage(proposal, error, false);
        recordFailure(attempt, reason, stage, errorCountsAsFailure(reason));
        logActivityProposalFailure(ref, version, reason, proposal, error);
        logResult(proposal["status"].toString().isEmpty() ? QStringLiteral("none")
                                                          : proposal["status"].toString(),
                  stage, reason);
        return;
    }
    const int newVersion = finished.value(QStringLiteral("version")).toInt();
    const auto resultStatus = finished.value(QStringLiteral("status")).toString();
    qInfo() << "[Activity] review committed" << ref << newVersion;
    // B1：活动结果作为注意力事件留档（供协调器/观测按 kind 汇总），并把当前活动指回该活动。
    store.setAttentionState(QStringLiteral("current_activity"), ref);
    store.setAttentionState(QStringLiteral("last_activity_result"),
                            QStringLiteral("%1:%2:%3").arg(ref).arg(version).arg(resultStatus));
    store.enqueueAttentionEvent(QStringLiteral("activity_result"),
                                QStringLiteral("%1:%2").arg(ref).arg(version),
                                QJsonObject{{QStringLiteral("status"), resultStatus},
                                            {QStringLiteral("activity_ref"), ref},
                                            {QStringLiteral("origin_kind"),
                                             attempt.origin.ref.startsWith(QStringLiteral("activity/"))
                                                 ? QStringLiteral("internal")
                                                 : QStringLiteral("legacy")}},
                                QStringLiteral("activity"), 40);
    qInfo() << "[Attention] current_activity=" << ref << "result=" << resultStatus;
    store.clearReviewFailure(kindName(attempt.origin.kind), ref);
    logResult(resultStatus.isEmpty() ? QStringLiteral("none") : resultStatus,
              QStringLiteral("commit"), QString());
    // 分享意愿：internal 活动已在 finishActivity 的同一事务里原子写入 share_intent
    // （绑定服务端实际成果 ref/版本/哈希），这里只记录日志，不再二次写库、不发送。
    if (finished.contains(QStringLiteral("share_intent"))) {
        qInfo().noquote()
            << QStringLiteral("[Activity] share_intent_stored ref=%1 version=%2"
                              " artifact_version=%3")
                   .arg(ref)
                   .arg(newVersion)
                   .arg(finished.value(QStringLiteral("artifact_version")).toInt());
        // R12-2b：internal 活动走统一的 action_proposal 核验/提交链（确定性+coverage+
        // 一次语义 verifier/repair → Outbox），不再有第二条发送路径；legacy thought 分享
        // 仍走下面的 shareRequested 旧链，语义不变。
        if (ref.startsWith(QStringLiteral("activity/"))) {
            const auto intent = finished.value(QStringLiteral("share_intent")).toObject();
            QString draft = proposal.value(QStringLiteral("draft")).toString().trimmed();
            if (draft.isEmpty()) {
                draft = store.activityByRef(ref).value(QStringLiteral("draft")).toString().trimmed();
            }
            const auto request = buildInternalShareRequest(store, ref, intent, draft);
            if (request.isEmpty()) {
                qInfo().noquote()
                    << QStringLiteral("[Activity] share_proposal_skipped ref=%1 reason=invalid")
                           .arg(ref);
                return;
            }
            const QString shareKey = request.value(QStringLiteral("action_key")).toString();
            const auto existing = store.actionProposal(shareKey);
            if (!existing.isEmpty()) {
                // 同一个成果版本只发一次：已提交/已核验的提案不重复生成，等待 drain/回执即可。
                qInfo().noquote()
                    << QStringLiteral("[Activity] share_proposal_exists ref=%1 state=%2")
                           .arg(ref, existing.value(QStringLiteral("state")).toString());
                return;
            }
            qInfo().noquote()
                << QStringLiteral("[Activity] share_proposal_requested ref=%1 key=%2")
                       .arg(ref, shareKey);
            emit internalShareProposalReady(request);
        }
        return;
    }
    // legacy 分享链完全保留：canShareThought + shareRequested → Outbox 唯一发送。
    if (!ref.startsWith(QStringLiteral("thought/"))) return;
    const auto draft = proposal["draft"].toString().trimmed();
    if (!draft.isEmpty() && proposal[QStringLiteral("share_with_owner")].toBool()
        && store.canShareThought(attempt.origin.id, newVersion))
        emit shareRequested(ref, newVersion, draft);
}

// R12-2b：内部活动分享提案的终态接线。LLMService 已完成确定性+coverage+一次语义核验、
// 至多一次约束修复；这里只做持久化与状态 CAS，然后交给 ActionCommitter 的唯一提交入口。
// 通过 → proposed→verified→committed（入 Outbox，等待 dispatcher 发送）；
// 拒绝 → proposed→withheld（活动完成状态保留，不发消息）。
void PrivateReview::receiveShareProposalResult(const QJsonObject &result)
{
    const QString actionKey = result.value(QStringLiteral("action_key")).toString().trimmed();
    if (actionKey.isEmpty()) return;
    const QString kind = result.value(QStringLiteral("kind")).toString();
    if (kind != QStringLiteral("activity_share")) {
        qWarning() << "[ShareProposal] unexpected kind" << kind << actionKey;
        return;
    }
    ActionOrigin origin;
    QString originError;
    if (!ActionOrigin::fromJson(result.value(QStringLiteral("origin")).toObject(),
                                &origin, &originError)
        || origin.kind != ActionOrigin::Activity
        || origin.actionKey(QStringLiteral("activity_share")) != actionKey) {
        qWarning() << "[ShareProposal] invalid origin" << actionKey << originError;
        return;
    }
    QJsonObject persistProposal{
        {QStringLiteral("kind"), kind},
        {QStringLiteral("origin"), origin.toJson()},
        {QStringLiteral("action_key"), actionKey},
        {QStringLiteral("target"), result.value(QStringLiteral("target"))},
        {QStringLiteral("payload"), result.value(QStringLiteral("payload"))},
        {QStringLiteral("claims"), result.value(QStringLiteral("claims"))},
        {QStringLiteral("expires_at"), result.value(QStringLiteral("expires_at"))}};
    QString persistError;
    const QJsonObject stored = store.persistActionProposal(persistProposal, &persistError);
    if (stored.contains(QStringLiteral("error"))) {
        // 同 key 不同 payload 的冲突不能覆盖已落库提案；保持既有版本等待人工/后续版本。
        qWarning() << "[ShareProposal] persist rejected" << actionKey << persistError;
        return;
    }
    const bool verified = result.value(QStringLiteral("state")).toString()
        == QStringLiteral("verified");
    const QString semanticStatus = result.value(QStringLiteral("semantic_status")).toString();
    // R12-2c-1：verified 终态只能携带服务端封闭枚举里的 semantic_status；空串/未知值
    // 一律不提升，按 schema_invalid 落 withheld（模型输出不能直接决定状态）。
    if (verified && !ActionPolicy::isValidSemanticStatus(semanticStatus)) {
        QString stateError;
        store.updateActionProposalState(actionKey, QStringLiteral("proposed"),
                                        QStringLiteral("withheld"),
                                        QStringLiteral("withheld"),
                                        QStringLiteral("schema_invalid"), &stateError);
        qInfo().noquote()
            << QStringLiteral("[ShareProposal] withheld key=%1 reason=schema_invalid")
                   .arg(actionKey);
        return;
    }
    const QString withholdReason = result.value(QStringLiteral("withhold_reason")).toString();
    if (!verified) {
        QString stateError;
        store.updateActionProposalState(actionKey, QStringLiteral("proposed"),
                                        QStringLiteral("withheld"),
                                        QStringLiteral("withheld"),
                                        withholdReason.isEmpty()
                                            ? QStringLiteral("policy_rejected")
                                            : withholdReason,
                                        &stateError);
        qInfo().noquote()
            << QStringLiteral("[ShareProposal] withheld key=%1 reason=%2")
                   .arg(actionKey, withholdReason);
        return;
    }
    QString stateError;
    const bool promoted = store.updateActionProposalState(
        actionKey, QStringLiteral("proposed"), QStringLiteral("verified"),
        semanticStatus, QString(),
        &stateError);
    if (!promoted) {
        // CAS 失败（可能已被 drain 并发推进）：只要最终状态不是 verified/committed 就放弃。
        const QJsonObject now = store.actionProposal(actionKey);
        const QString state = now.value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("verified") && state != QStringLiteral("committed")) {
            qWarning() << "[ShareProposal] verify CAS failed" << actionKey << stateError << state;
            return;
        }
    }
    ActionCommitter committer(&store);
    const auto committed = committer.commitInternalActivityShare(actionKey);
    if (committed.contains(QStringLiteral("error"))) {
        const QJsonObject now = store.actionProposal(actionKey);
        const QString state = now.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("withheld"))
            qInfo().noquote()
                << QStringLiteral("[ShareProposal] commit_withheld key=%1 error=%2")
                       .arg(actionKey, committed.value(QStringLiteral("error")).toString());
        else
            qInfo().noquote()
                << QStringLiteral("[ShareProposal] commit_pending key=%1 error=%2 state=%3")
                       .arg(actionKey, committed.value(QStringLiteral("error")).toString(), state);
        return;
    }
    qInfo().noquote()
        << QStringLiteral("[ShareProposal] committed key=%1 action=%2")
               .arg(actionKey, committed.value(QStringLiteral("action_key")).toString());
}

void PrivateReview::receiveOpenWake(const Attempt &attempt, const QJsonObject &proposal,
                                    const QString &error)
{
    const qlonglong id = attempt.origin.id;
    const int version = attempt.origin.version;
    const QString ref = attempt.origin.ref;
    const auto openWake = ConfigManager::instance().getOpenWakeConfig();
    const auto logResult = [&](const QString &decision, const QString &stage, const QString &reason) {
        qInfo().noquote()
            << QStringLiteral("[Review] result kind=open_wake ref=%1 version=%2 decision=%3"
                              " stage=%4 reason=%5")
                   .arg(ref)
                   .arg(version)
                   .arg(decision, stage,
                        reason.trimmed().isEmpty() ? QStringLiteral("-") : reason);
    };

    // R12-1a：间隔配置在提交边界生效。协议层仍只校验 after_minutes 在 5..1440；
    // 协议合法后再按配置 [minMinutes,maxMinutes] 夹紧得到 effective（夹紧不是失败，
    // 只有协议非法/commit 失败才计失败）。模型没给 next_wake 时用 default_interval_seconds，
    // 并走同一套 effective 夹紧；requested/effective 都写进日志与 intent 供审计。
    const int minMinutes = qBound(5, openWake.minIntervalSeconds / 60, 1440);
    const int maxMinutes = qBound(minMinutes, openWake.maxIntervalSeconds / 60, 1440);
    int requestedMinutes = qBound(5, openWake.defaultIntervalSeconds / 60, 1440);
    int effectiveMinutes = qBound(minMinutes, requestedMinutes, maxMinutes);

    QString rejectReason = error;
    QString decision;
    QString note;
    QJsonObject startActivityProposal;
    QJsonObject openWakeContact;
    if (rejectReason.isEmpty()) {
        const int schemaVersion = proposal.contains(QStringLiteral("schema_version"))
            ? proposal.value(QStringLiteral("schema_version")).toInt(-1)
            : proposal.value(QStringLiteral("schedule_version")).toInt(-1);
        if (proposal.isEmpty()) {
            rejectReason = QStringLiteral("json_parse");
        } else if (schemaVersion != 1) {
            rejectReason = QStringLiteral("schema");
        } else if (proposal.contains(QStringLiteral("activity"))) {
            // "activity" 是旧外部动作键，仍按越界 schema 拒绝。
            rejectReason = QStringLiteral("schema");
        } else {
            const QJsonObject contact = proposal.value(QStringLiteral("contact")).toObject();
            const bool hasContact = proposal.contains(QStringLiteral("contact"));
            const bool allowContact =
                ConfigManager::instance().openWakeAllowContact();
            // R12-2b：LLMService 在确定性/语义核验或策略拒绝时会带回 state=withheld 的
            // 服务端计划；这类联系不是模型 schema 错误，只保留内部记录并落 withheld 提案。
            const QJsonObject serverPlan = proposal.value(QStringLiteral("_action")).toObject();
            const bool rejectedPlan = !serverPlan.isEmpty()
                && serverPlan.value(QStringLiteral("state")).toString()
                       == QStringLiteral("withheld");
            decision = proposal.value(QStringLiteral("decision")).toString();
            // note 是所有决策共享的内部记录；contact 被拒绝/降级为 reflect 时也要保留。
            if (proposal.contains(QStringLiteral("note"))
                && !proposal.value(QStringLiteral("note")).isNull()
                && !proposal.value(QStringLiteral("note")).isString()) {
                rejectReason = QStringLiteral("schema");
            } else {
                note = proposal.value(QStringLiteral("note")).toString().trimmed();
                if (note.size() > 240) rejectReason = QStringLiteral("schema");
            }
            if (hasContact && !allowContact) {
                // R12-2b：allow_contact=false 时任何 contact 都不发送，但也不把一次合法的
                // 内部唤醒当认知失败：记录 contact_rejected，按内部记录（reflect）提交。
                qInfo().noquote()
                    << QStringLiteral("[OpenWake] contact_rejected"
                                      " reason=allow_contact_disabled wake=%1 version=%2")
                           .arg(id)
                           .arg(version);
                if (decision == QStringLiteral("contact"))
                    decision = QStringLiteral("reflect");
            }
            if (decision != QStringLiteral("rest") && decision != QStringLiteral("reflect")
                && decision != QStringLiteral("start_activity")
                && decision != QStringLiteral("contact")) {
                rejectReason = QStringLiteral("schema");
            } else if (decision == QStringLiteral("contact")) {
                if (!hasContact || !wakeContactShapeValid(contact))
                    rejectReason = QStringLiteral("schema");
                else
                    openWakeContact = contact;
            } else if (hasContact && allowContact) {
                if (rejectedPlan && decision == QStringLiteral("reflect")) {
                    // 服务端已拒绝的联系：保留 contact 以便持久化 withheld 提案，
                    // 不按 schema 失败处理（合法内部记录仍然保存）。
                    openWakeContact = contact;
                } else {
                    // 只有 decision=contact 才能携带 contact，避免"顺带联系"的歧义。
                    rejectReason = QStringLiteral("schema");
                }
            } else if (decision != QStringLiteral("start_activity")
                       && proposal.contains(QStringLiteral("start_activity"))) {
                // 只有 decision=start_activity 才能携带活动提案。
                rejectReason = QStringLiteral("schema");
            } else if (proposal.contains(QStringLiteral("note"))
                       && !proposal.value(QStringLiteral("note")).isNull()
                       && !proposal.value(QStringLiteral("note")).isString()) {
                rejectReason = QStringLiteral("schema");
            } else {
                note = proposal.value(QStringLiteral("note")).toString().trimmed();
                if (note.size() > 240) rejectReason = QStringLiteral("schema");
            }
            // R12-1b：start_activity 的接受条件与提案 schema。它只表示"产生意愿并开始安排"，
            // 不接受任何完成/产物/联系字段。
            if (rejectReason.isEmpty() && decision == QStringLiteral("start_activity")) {
                if (!ConfigManager::instance().getProactiveConfig().privateWriting) {
                    rejectReason = QStringLiteral("writing_disabled");
                } else {
                    const auto startValue = proposal.value(QStringLiteral("start_activity"));
                    if (!startValue.isObject()) {
                        rejectReason = QStringLiteral("schema");
                    } else {
                        const auto start = startValue.toObject();
                        if (start.contains(QStringLiteral("contact"))
                            || start.contains(QStringLiteral("artifact"))
                            || start.contains(QStringLiteral("status"))
                            || start.contains(QStringLiteral("draft"))
                            || start.contains(QStringLiteral("share_with_owner"))) {
                            rejectReason = QStringLiteral("schema");
                        }
                        const QString interest =
                            start.value(QStringLiteral("interest")).toString().trimmed();
                        if (rejectReason.isEmpty()
                            && (interest.isEmpty() || interest.size() > 120))
                            rejectReason = QStringLiteral("schema");
                        if (rejectReason.isEmpty() && start.contains(QStringLiteral("goal"))
                            && !start.value(QStringLiteral("goal")).isString())
                            rejectReason = QStringLiteral("schema");
                        const QString goal =
                            start.value(QStringLiteral("goal")).toString().trimmed();
                        if (rejectReason.isEmpty() && goal.size() > 200)
                            rejectReason = QStringLiteral("schema");
                        if (rejectReason.isEmpty()
                            && start.contains(QStringLiteral("initial_review_after_minutes"))) {
                            const auto minutesValue =
                                start.value(QStringLiteral("initial_review_after_minutes"));
                            const int minutes =
                                minutesValue.isDouble() ? minutesValue.toInt(-1) : -1;
                            if (minutes < 5 || minutes > 1440)
                                rejectReason = QStringLiteral("schema");
                        }
                        if (rejectReason.isEmpty()) startActivityProposal = start;
                    }
                }
            }
        }
        if (rejectReason.isEmpty() && proposal.contains(QStringLiteral("next_wake"))) {
            const auto nextWake = proposal.value(QStringLiteral("next_wake"));
            if (!nextWake.isObject()) {
                rejectReason = QStringLiteral("schema");
            } else {
                const auto nextWakeObj = nextWake.toObject();
                const QString mode = nextWakeObj.value(QStringLiteral("mode"))
                                         .toString(QStringLiteral("after"));
                if (mode != QStringLiteral("after")) {
                    // 本片只支持 after；on_event 不领取/不提交（按 schema 违规拒绝）。
                    rejectReason = QStringLiteral("schema");
                } else if (nextWakeObj.contains(QStringLiteral("after_minutes"))) {
                    const auto minutesValue = nextWakeObj.value(QStringLiteral("after_minutes"));
                    const int minutes = minutesValue.isDouble() ? minutesValue.toInt(-1) : -1;
                    if (minutes < 5 || minutes > 1440) {
                        // 协议 5..1440 之外才是 schema 失败；配置夹紧不在此处判定。
                        rejectReason = QStringLiteral("schema");
                    } else {
                        requestedMinutes = minutes;
                        effectiveMinutes = qBound(minMinutes, requestedMinutes, maxMinutes);
                    }
                }
            }
        }
    }

    if (!rejectReason.isEmpty()) {
        const QString stage = reviewErrorStage(rejectReason);
        const bool counts = errorCountsAsFailure(rejectReason);
        recordFailure(attempt, rejectReason, stage, counts);
        logResult(decision.isEmpty() ? QStringLiteral("none") : decision, stage, rejectReason);
        return;
    }

    // 合法间隔的审计日志：requested 是模型/默认请求值，effective 是按配置夹紧后实际提交值。
    qInfo().noquote()
        << QStringLiteral("[OpenWake] interval requested=%1 effective=%2 min=%3 max=%4")
               .arg(requestedMinutes)
               .arg(effectiveMinutes)
               .arg(minMinutes)
               .arg(maxMinutes);
    // R12-2b：contact 提案的持久化/状态 CAS/唯一提交入口。服务端计划（_action）由
    // LLMService 统一核验链产出；这里只接受与本次 wake 身份完全一致的计划，避免伪造/串线。
    // 通过后：persistActionProposal → proposed→verified(CAS) → commitVerifiedActionProposal；
    // 拒绝/无计划：落 withheld（可持久化时）并保留本次内部记录，仍按 reflect 完成 wake。
    bool contactVerified = false;
    if (!openWakeContact.isEmpty()) {
        const QJsonObject plan = proposal.value(QStringLiteral("_action")).toObject();
        QString planError;
        ActionOrigin planOrigin;
        const bool planOriginOk =
            !plan.isEmpty()
            && ActionOrigin::fromJson(plan.value(QStringLiteral("origin")).toObject(),
                                      &planOrigin, &planError)
            && planOrigin.kind == ActionOrigin::Wake
            && planOrigin.id == id
            && planOrigin.version == version
            && planOrigin.runId == attempt.origin.runId
            && planOrigin.scope == attempt.origin.scope
            && planOrigin.contextEpoch == attempt.origin.epoch
            && planOrigin.actionKey(QStringLiteral("contact"))
                   == plan.value(QStringLiteral("action_key")).toString();
        const QString planState = plan.value(QStringLiteral("state")).toString();
        const QString actionKey = plan.value(QStringLiteral("action_key")).toString();
        if (!planOriginOk) {
            qInfo().noquote()
                << QStringLiteral("[OpenWake] contact_rejected reason=contact_plan_missing"
                                  " wake=%1 version=%2 detail=%3")
                       .arg(id)
                       .arg(version)
                       .arg(planError.isEmpty() ? QStringLiteral("mismatch") : planError);
            decision = QStringLiteral("reflect");
        } else {
            const QJsonObject persistProposal{
                {QStringLiteral("kind"), QStringLiteral("contact")},
                {QStringLiteral("origin"), plan.value(QStringLiteral("origin"))},
                {QStringLiteral("action_key"), actionKey},
                {QStringLiteral("target"), plan.value(QStringLiteral("target"))},
                {QStringLiteral("payload"), plan.value(QStringLiteral("payload"))},
                {QStringLiteral("claims"), plan.value(QStringLiteral("claims"))},
                {QStringLiteral("expires_at"), plan.value(QStringLiteral("expires_at"))}};
            QString persistError;
            store.persistActionProposal(persistProposal, &persistError);
            const bool persistOk = persistError.isEmpty()
                || persistError == QStringLiteral("conflict");
            const QString planSemantic = plan.value(QStringLiteral("semantic_status")).toString();
            // R12-2c-1：verified 只能携带服务端封闭枚举里的 semantic_status；
            // 空串/未知值一律不提升 verified，按 schema_invalid 落 withheld。
            const bool planSemanticValid = ActionPolicy::isValidSemanticStatus(planSemantic);
            if (planState != QStringLiteral("verified") || !planSemanticValid) {
                const QString reason =
                    plan.value(QStringLiteral("withhold_reason")).toString().trimmed();
                const QString rejectReason = planState == QStringLiteral("verified")
                    ? QStringLiteral("schema_invalid")
                    : (reason.isEmpty() ? QStringLiteral("policy_rejected") : reason);
                if (persistOk) {
                    store.updateActionProposalState(
                        actionKey, QStringLiteral("proposed"), QStringLiteral("withheld"),
                        QStringLiteral("withheld"), rejectReason);
                }
                qInfo().noquote()
                    << QStringLiteral("[OpenWake] contact_rejected reason=%1 key=%2")
                           .arg(rejectReason, actionKey);
                decision = QStringLiteral("reflect");
            } else if (!persistOk) {
                qInfo().noquote()
                    << QStringLiteral("[OpenWake] contact_rejected"
                                      " reason=proposal_persist_failed key=%1 error=%2")
                           .arg(actionKey, persistError);
                decision = QStringLiteral("reflect");
            } else {
                const QString semantic = planSemantic;
                QString stateError;
                store.updateActionProposalState(
                    actionKey, QStringLiteral("proposed"), QStringLiteral("verified"),
                    semantic, QString(), &stateError);
                ActionCommitter committer(&store);
                const auto committed = committer.commitOpenWakeContact(actionKey);
                if (committed.contains(QStringLiteral("error"))) {
                    const QString commitErrorCode =
                        committed.value(QStringLiteral("error")).toString();
                    const QJsonObject now = store.actionProposal(actionKey);
                    if (now.value(QStringLiteral("state")).toString()
                        == QStringLiteral("withheld")) {
                        qInfo().noquote()
                            << QStringLiteral("[OpenWake] contact_rejected reason=%1 key=%2")
                                   .arg(commitErrorCode, actionKey);
                        decision = QStringLiteral("reflect");
                    } else {
                        // 可恢复失败（安静/额度/目标/DB）：提案保持 verified 供 drain 恢复，
                        // wake 本身仍按 contact 完成（不重复回顾、不无限重试）。
                        contactVerified = true;
                        qInfo().noquote()
                            << QStringLiteral("[OpenWake] contact_commit_pending key=%1"
                                              " reason=%2")
                                   .arg(actionKey, commitErrorCode);
                    }
                } else {
                    contactVerified = true;
                    qInfo().noquote()
                        << QStringLiteral("[OpenWake] contact_committed key=%1 state=%2")
                               .arg(actionKey,
                                    committed.value(QStringLiteral("state")).toString());
                }
            }
        }
    }
    if (decision == QStringLiteral("contact") && !contactVerified)
        decision = QStringLiteral("reflect");

    const QJsonObject intent{{QStringLiteral("mode"), QStringLiteral("after")},
                             {QStringLiteral("requested_after_minutes"), requestedMinutes},
                             {QStringLiteral("after_minutes"), effectiveMinutes}};
    QString commitError;
    if (decision == QStringLiteral("start_activity")) {
        // R12-1b：同一事务创建 internal 活动 + 完成 open_wake + 更新调度；失败整体回滚。
        QJsonObject activityResult;
        if (store.finishOpenWakeWithActivity(id, version, decision, note, intent,
                                             effectiveMinutes, startActivityProposal,
                                             &activityResult, &commitError)) {
            store.clearReviewFailure(QStringLiteral("open_wake"), ref);
            const qint64 nextDue = activityResult.value(QStringLiteral("next_due_at")).toInteger();
            qInfo().noquote()
                << QStringLiteral("[OpenWake] committed decision=start_activity"
                                  " activity=%1 next_due=%2")
                       .arg(activityResult.value(QStringLiteral("ref")).toString(),
                            reviewIsoFromEpoch(nextDue));
            logResult(decision, QStringLiteral("commit"), QString());
            return;
        }
        // 活动创建失败（already_active/db_error/...）→ 整次 OpenWake 失败计数/退避，
        // 不产生半活动；open_wake 行仍为 running，由失败状态机 defer。
        const QString reason = commitError.isEmpty() ? QStringLiteral("invalid_or_stale_proposal")
                                                     : commitError;
        recordFailure(attempt, reason, QStringLiteral("commit"), errorCountsAsFailure(reason));
        logResult(decision, QStringLiteral("commit"), reason);
        return;
    }
    if (store.finishOpenWake(id, version, decision, note, intent, effectiveMinutes, &commitError)) {
        store.clearReviewFailure(QStringLiteral("open_wake"), ref);
        const qint64 nextDue = QDateTime::currentSecsSinceEpoch() + qint64(effectiveMinutes) * 60;
        qInfo().noquote()
            << QStringLiteral("[OpenWake] committed decision=%1 next_due=%2 note_len=%3")
                   .arg(decision, reviewIsoFromEpoch(nextDue))
                   .arg(note.size());
        logResult(decision, QStringLiteral("commit"), QString());
        return;
    }
    const QString reason = commitError.isEmpty() ? QStringLiteral("invalid_or_stale_proposal")
                                                 : commitError;
    recordFailure(attempt, reason, QStringLiteral("commit"), errorCountsAsFailure(reason));
    logResult(decision.isEmpty() ? QStringLiteral("none") : decision,
              QStringLiteral("commit"), reason);
}
