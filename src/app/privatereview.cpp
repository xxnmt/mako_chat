#include "privatereview.h"
#include "memorymanager.h"
#include "configmanager.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace {

// R12-0：回顾错误 -> 诊断阶段（只用于日志字段，不改变任何错误处理路径）。
// 阶段取自既有错误语义：parse=模型输出不可解析/被截断；schema=proposal 结构不合法；
// policy=授权/隐私过滤；verify=联系核验未通过；cancel=让出注意力；commit=提交/CAS/来源失效。
QString reviewErrorStage(const QString &error)
{
    if (error == QStringLiteral("truncated") || error == QStringLiteral("network_error"))
        return QStringLiteral("parse");
    if (error == QStringLiteral("filtered") || error == QStringLiteral("writing_disabled"))
        return QStringLiteral("policy");
    if (error == QStringLiteral("invalid_or_stale_proposal"))
        return QStringLiteral("schema");
    if (error == QStringLiteral("cancelled") || error == QStringLiteral("cancel"))
        return QStringLiteral("cancel");
    // source_unavailable / evidence_changed / db_error 等：提交前来源或状态失效。
    return QStringLiteral("commit");
}

} // namespace

PrivateReview::PrivateReview(MemoryManager &memory, QObject *parent)
    : QObject(parent), store(memory)
{
}

QString PrivateReview::kindTag(Kind kind)
{
    switch (kind) {
    case Kind::Reflection: return QStringLiteral("[Reflection]");
    case Kind::Concern: return QStringLiteral("[Concern]");
    case Kind::Activity: break;
    }
    return QStringLiteral("[Activity]");
}

QString PrivateReview::kindName(Kind kind)
{
    switch (kind) {
    case Kind::Reflection: return QStringLiteral("reflection");
    case Kind::Concern: return QStringLiteral("concern");
    case Kind::Activity: break;
    }
    return QStringLiteral("activity");
}

QString PrivateReview::sourceRef(const Attempt &attempt)
{
    switch (attempt.kind) {
    case Kind::Reflection: return QStringLiteral("reflection/%1").arg(attempt.id);
    case Kind::Concern: return QStringLiteral("concern/%1").arg(attempt.id);
    case Kind::Activity: break;
    }
    return QStringLiteral("thought/%1").arg(attempt.id);
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

void PrivateReview::clearFailures(const QString &ref)
{
    failures.remove(ref);
}

void PrivateReview::noteFailure(const Attempt &attempt, const QString &ref, const QString &reason)
{
    const auto options = ConfigManager::instance().getProactiveConfig();
    auto &state = failures[ref];
    // 版本跳变 >1：说明中间有外部操作刷新了源（例如活动被用户改过），旧连续失败链
    // 不再成立，按"新的源版本"恢复。进程重启会清空整个 QHash（内存计数归零）。
    if (state.version >= 0 && attempt.version > state.version + 1)
        state = FailureState{};
    state.version = attempt.version;
    ++state.count;
    const int failureCount = state.count;
    if (attempt.kind != Kind::Reflection) {
        // 不扩大退避范围：关注/活动保持原来的"立即放回队列"语义，原因只进 [Review] result。
        if (attempt.kind == Kind::Concern)
            store.deferConcernReview(attempt.id, attempt.version, reason);
        else
            store.deferThoughtReview(attempt.id, attempt.version, reason);
        return;
    }
    const int maxFailures = qMax(1, options.maxReviewFailures);
    if (failureCount >= maxFailures) {
        // 连续失败达到上限：suspended，24 小时后再由正常的领取条件重试。
        constexpr int suspendSeconds = 86400;
        store.deferPrivateReflection(attempt.id, attempt.version, suspendSeconds);
        const auto nextEligible = QDateTime::currentDateTimeUtc().addSecs(suspendSeconds);
        qInfo().noquote()
            << QStringLiteral("[Review] suspended kind=reflection ref=%1 version=%2 failures=%3"
                              " reason=%4 next_eligible=%5")
                   .arg(ref)
                   .arg(attempt.version)
                   .arg(failureCount)
                   .arg(reason, nextEligible.toString(Qt::ISODate));
        return;
    }
    const auto &backoff = options.reviewBackoffSeconds;
    const int index = qMin(failureCount - 1,
                           backoff.isEmpty() ? 0 : int(backoff.size()) - 1);
    const int seconds = backoff.isEmpty() ? 120 : backoff.at(index);
    store.deferPrivateReflection(attempt.id, attempt.version, seconds);
    qInfo().noquote()
        << QStringLiteral("[Review] backoff kind=reflection ref=%1 version=%2 failures=%3"
                          " seconds=%4 reason=%5")
               .arg(ref)
               .arg(attempt.version)
               .arg(failureCount)
               .arg(seconds)
               .arg(reason);
}

WakeResult PrivateReview::wake(int dailyLimit, int cooldownSeconds)
{
    WakeResult result;
    result.kind = QStringLiteral("none");
    result.reason = QStringLiteral("no_candidate");
    if (active) {
        // 已有在途回顾：不重复领取，但把当前在途的类型化状态带回给调用方。
        result.kind = kindName(active->kind);
        result.sourceRef = sourceRef(*active);
        result.version = active->version;
        result.reason = QStringLiteral("busy");
        return result;
    }
    // 优先级：到期活动 → 到期关注 → 新来源回顾。活动和关注都有"到期"，
    // 新来源回顾有 24h 窗口、可以补，所以先处理有时效的，避免回顾额度被新来源吃掉。
    QString claimReason;
    QDateTime nextEligibleAt;
    // 关注领取被预算/冷却/DB 挡住时仍按原行为继续尝试新来源回顾（两者共用预算）；
    // 若新来源也没有候选，再用这里的原因解释这次 tick 为什么没启动。
    QString blockedReason;
    QDateTime blockedNextEligibleAt;
    auto context = store.nextThought(true);
    auto kind = Kind::Activity;
    if (context.isEmpty()) {
        context = store.claimDueConcern(dailyLimit, cooldownSeconds, &claimReason, &nextEligibleAt);
        kind = Kind::Concern;
        if (context.isEmpty() && claimReason != QStringLiteral("no_candidate")) {
            blockedReason = claimReason;
            blockedNextEligibleAt = nextEligibleAt;
        }
    }
    if (context.isEmpty()) {
        claimReason.clear();
        nextEligibleAt = QDateTime{};
        context = store.claimPrivateReflection(dailyLimit, cooldownSeconds, &claimReason, &nextEligibleAt);
        kind = Kind::Reflection;
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
    const auto id = context.take("id").toInteger();
    int version = context.take("version").toInt();
    if (id <= 0 || version < 0) {
        // 领取上下文缺少可提交的行标识：不进入在途状态，交给下一轮重新领取。
        // 注意 version==0 是新建活动/首次回顾的合法版本，不能拒绝。
        result.kind = kindName(kind);
        result.reason = QStringLiteral("storage_error");
        return result;
    }
    if (kind == Kind::Activity) {
        // 活动回顾的预算由 claimThoughtReview 扣；关注/新来源回顾的预算已在各自 claim 内扣。
        QString reason;
        QDateTime next;
        if (!store.claimThoughtReview(id, version, dailyLimit, cooldownSeconds, &reason, &next)) {
            result.kind = kindName(kind);
            result.reason = reason.isEmpty() ? QStringLiteral("db_error") : reason;
            result.sourceRef = QStringLiteral("thought/%1").arg(id);
            result.version = version;
            result.nextEligibleAt = next;
            return result;
        }
        ++version;
    } else if (kind == Kind::Concern) {
        // 记忆层的契约要求上下文带 kind=concern；这里显式保证，避免路由到活动提示词。
        context.insert(QStringLiteral("kind"), QStringLiteral("concern"));
    }
    const auto options = ConfigManager::instance().getProactiveConfig();
    active = Attempt{id, version, kind, options.enabled && !options.targets.isEmpty()};
    // B1：当前活动（仅活动回顾有"活动"语义；关注/新来源回顾不写入 current_activity）。
    if (kind == Kind::Activity) {
        const auto activityRef = QStringLiteral("thought/%1").arg(id);
        store.setAttentionState(QStringLiteral("current_activity"), activityRef);
        qInfo() << "[Attention] current_activity=" << activityRef << "version=" << version;
    }
    qInfo() << kindTag(kind) + QStringLiteral(" reviewing") << id << version;
    // R12-0：真正开始时留一行结构化诊断（INFO，只含定位字段与服务端时间，不含原文）。
    qInfo().noquote()
        << QStringLiteral("[Review] attempt kind=%1 ref=%2 version=%3 current_local_time=%4")
               .arg(kindName(kind), sourceRef(*active))
               .arg(version)
               .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    result.started = true;
    result.kind = kindName(kind);
    result.reason = QStringLiteral("started");
    result.sourceRef = sourceRef(*active);
    result.version = version;
    emit requested(id, active->version, context);
    return result;
}

void PrivateReview::yieldAttention(const QString &reason)
{
    if (!active) return;
    // B1：让出注意力时记录"切换原因"，并清掉当前活动；重启后据此解释活动为什么停下。
    store.setAttentionState(QStringLiteral("last_switch_reason"), reason);
    store.setAttentionState(QStringLiteral("current_activity"), QString());
    qInfo() << "[Attention] switch reason=" << reason;
    const auto attempt = *active;
    active.reset();
    bool saved = false;
    switch (attempt.kind) {
    case Kind::Concern:
        saved = store.deferConcernReview(attempt.id, attempt.version, reason);
        break;
    case Kind::Reflection:
        // 用户插话/关停造成的主动让出：立即 defer，不累计失败、不退避。
        saved = store.deferPrivateReflection(attempt.id, attempt.version);
        break;
    case Kind::Activity:
        saved = store.deferThoughtReview(attempt.id, attempt.version, reason);
        break;
    }
    qInfo() << kindTag(attempt.kind) + QStringLiteral(" yielded")
            << attempt.id << attempt.version << reason << saved;
    qInfo().noquote()
        << QStringLiteral("[Review] result kind=%1 ref=%2 version=%3 decision=yielded"
                          " stage=cancel reason=%4")
               .arg(kindName(attempt.kind), sourceRef(attempt))
               .arg(attempt.version)
               .arg(reason);
    emit cancelled();
}

void PrivateReview::receive(qlonglong id, int version, const QJsonObject &proposal, const QString &error)
{
    if (!active || active->id != id || active->version != version) return;
    const auto attempt = *active;
    const bool contactAllowed = attempt.contactAllowed && ConfigManager::instance().getProactiveConfig().enabled;
    active.reset();
    const auto ref = sourceRef(attempt);
    const auto logResult = [&](const QString &decision, const QString &stage, const QString &reason) {
        qInfo().noquote()
            << QStringLiteral("[Review] result kind=%1 ref=%2 version=%3 decision=%4"
                              " stage=%5 reason=%6")
                   .arg(kindName(attempt.kind), ref)
                   .arg(attempt.version)
                   .arg(decision, stage,
                        reason.trimmed().isEmpty() ? QStringLiteral("-") : reason);
    };
    if (attempt.kind == Kind::Reflection) {
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
            clearFailures(ref);
            logResult(decision, reviewStage(proposal,error,true),
                      proposal["contact"].toObject()["withhold_reason"].toString());
        } else {
            const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
            // R12-0 失败退避：只有真正的失败（error 或 commit 失败）才累计；
            // yieldAttention 走的是另一条路径，不经过这里。
            noteFailure(attempt, ref, reason);
            logResult(decision, reviewStage(proposal,error,false), reason);
        }
        return;
    }
    if (attempt.kind == Kind::Concern) {
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
            clearFailures(ref);
            logResult(decision, reviewStage(proposal,error,true),
                      contact["withhold_reason"].toString());
            return;
        }
        // 领取时已经扣过预算/冷却，失败只把关注放回队列，不重新领取。
        const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
        noteFailure(attempt, ref, reason);
        logResult(decision, reviewStage(proposal,error,false), reason);
        return;
    }
    if (!error.isEmpty() || !store.finishThoughtReview(id, version, proposal)) {
        // 原来的领取仍然计入冷却与每日预算。
        const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
        noteFailure(attempt, ref, reason);
        logResult(proposal["status"].toString().isEmpty() ? QStringLiteral("none")
                                                         : proposal["status"].toString(),
                  reviewStage(proposal,error,false), reason);
        return;
    }
    qInfo() << "[Activity] review committed" << id << version + 1;
    // B1：活动结果作为注意力事件留档（供协调器/观测按 kind 汇总），并把当前活动指回该活动。
    const auto activityRef = QStringLiteral("thought/%1").arg(id);
    const auto resultStatus = proposal["status"].toString();
    store.setAttentionState(QStringLiteral("current_activity"), activityRef);
    store.setAttentionState(QStringLiteral("last_activity_result"),
                            QStringLiteral("thought/%1:%2:%3").arg(id).arg(version).arg(resultStatus));
    store.enqueueAttentionEvent(QStringLiteral("activity_result"),
                                QStringLiteral("thought/%1:%2").arg(id).arg(version),
                                QJsonObject{{QStringLiteral("status"), resultStatus}},
                                QStringLiteral("activity"), 40);
    qInfo() << "[Attention] current_activity=" << activityRef << "result=" << resultStatus;
    clearFailures(ref);
    logResult(resultStatus.isEmpty() ? QStringLiteral("none") : resultStatus,
              QStringLiteral("commit"), QString());
    const auto draft = proposal["draft"].toString().trimmed();
    if (!draft.isEmpty() && proposal["share_with_owner"].toBool() && store.canShareThought(id, version + 1))
        emit shareRequested(QStringLiteral("thought/%1").arg(id), version + 1, draft);
}
