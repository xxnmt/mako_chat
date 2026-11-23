#include "privatereview.h"
#include "memorymanager.h"
#include "configmanager.h"

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

void PrivateReview::wake(int dailyLimit, int cooldownSeconds)
{
    if (active) return;
    // 优先级：到期活动 → 到期关注 → 新来源回顾。活动和关注都有"到期"，
    // 新来源回顾有 24h 窗口、可以补，所以先处理有时效的，避免回顾额度被新来源吃掉。
    auto context = store.nextThought(true);
    auto kind = Kind::Activity;
    if (context.isEmpty()) {
        context = store.claimDueConcern(dailyLimit, cooldownSeconds);
        kind = Kind::Concern;
    }
    if (context.isEmpty()) {
        context = store.claimPrivateReflection(dailyLimit, cooldownSeconds);
        kind = Kind::Reflection;
    }
    if (context.isEmpty()) return;
    const auto id = context.take("id").toInteger();
    int version = context.take("version").toInt();
    if (kind == Kind::Activity) {
        // 活动回顾的预算由 claimThoughtReview 扣；关注/新来源回顾的预算已在各自 claim 内扣。
        if (!store.claimThoughtReview(id,version,dailyLimit,cooldownSeconds)) return;
        ++version;
    } else if (kind == Kind::Concern) {
        // 记忆层的契约要求上下文带 kind=concern；这里显式保证，避免路由到活动提示词。
        context.insert(QStringLiteral("kind"), QStringLiteral("concern"));
    }
    const auto options = ConfigManager::instance().getProactiveConfig();
    active = Attempt{id,version,kind,options.enabled && !options.targets.isEmpty()};
    // B1：当前活动（仅活动回顾有"活动"语义；关注/新来源回顾不写入 current_activity）。
    if (kind == Kind::Activity) {
        const auto activityRef = QStringLiteral("thought/%1").arg(id);
        store.setAttentionState(QStringLiteral("current_activity"), activityRef);
        qInfo() << "[Attention] current_activity=" << activityRef << "version=" << version;
    }
    qInfo() << kindTag(kind) + QStringLiteral(" reviewing") << id << version;
    emit requested(id, active->version, context);
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
        saved = store.deferPrivateReflection(attempt.id, attempt.version);
        break;
    case Kind::Activity:
        saved = store.deferThoughtReview(attempt.id, attempt.version, reason);
        break;
    }
    qInfo() << kindTag(attempt.kind) + QStringLiteral(" yielded")
            << attempt.id << attempt.version << reason << saved;
    emit cancelled();
}

void PrivateReview::receive(qlonglong id, int version, const QJsonObject &proposal, const QString &error)
{
    if (!active || active->id != id || active->version != version) return;
    const auto attempt = *active;
    const bool contactAllowed = attempt.contactAllowed && ConfigManager::instance().getProactiveConfig().enabled;
    active.reset();
    if (attempt.kind == Kind::Reflection) {
        const auto choice = proposal["choice"].toString();
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
            if (choice=="consider" && contact["timing"].toString()=="now") emit contactProposed(id,version);
        } else {
            store.deferPrivateReflection(id,version);
            qInfo() << "[Reflection] deferred" << id << version << error;
        }
        return;
    }
    if (attempt.kind == Kind::Concern) {
        const auto contact = proposal["contact"].toObject();
        const auto action = proposal["concern_action"].toString();
        if (error.isEmpty() && store.finishConcernReview(id,version,proposal)) {
            qInfo() << "[Concern] committed" << id << version << action << contact["timing"].toString();
            // 投递权限与安静时段由 AppController 的 canContactNow 再核验；这里只把"现在联系"的
            // 决定交给投递层，keep/revise/resolve/drop 只在记忆层落库，不产生外部消息。
            if (action == "contact" && contact["timing"].toString() == "now")
                emit contactProposed(id,version);
            return;
        }
        // 领取时已经扣过预算/冷却，失败只把关注放回队列，不重新领取。
        const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
        store.deferConcernReview(id,version,reason);
        qInfo() << "[Concern] review did not commit" << id << version << reason;
        return;
    }
    if (!error.isEmpty() || !store.finishThoughtReview(id, version, proposal)) {
        // 原来的领取仍然计入冷却与每日预算。
        const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
        store.deferThoughtReview(id, version, reason);
        qInfo() << "[Activity] review did not commit" << id << version << reason;
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
    const auto draft = proposal["draft"].toString().trimmed();
    if (!draft.isEmpty() && proposal["share_with_owner"].toBool() && store.canShareThought(id, version + 1))
        emit shareRequested(QStringLiteral("thought/%1").arg(id), version + 1, draft);
}
