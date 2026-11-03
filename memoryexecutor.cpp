#include "memoryexecutor.h"

#include "memoryagent.h"
#include "memorytimeresolver.h"
#include "timeframeresolver.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSet>

namespace {

QString frameAnchorName(MemoryTimeFrame::Anchor anchor)
{
    switch (anchor) {
    case MemoryTimeFrame::Anchor::RelativeDay:
        return QStringLiteral("relative_day");
    case MemoryTimeFrame::Anchor::RelativeWeek:
        return QStringLiteral("relative_week");
    case MemoryTimeFrame::Anchor::RelativeMonth:
        return QStringLiteral("relative_month");
    case MemoryTimeFrame::Anchor::AbsoluteDate:
        return QStringLiteral("absolute_date");
    case MemoryTimeFrame::Anchor::Festival:
        return QStringLiteral("festival");
    case MemoryTimeFrame::Anchor::RecentWindow:
        return QStringLiteral("recent_window");
    case MemoryTimeFrame::Anchor::None:
    default:
        return QStringLiteral("none");
    }
}

QString frameSpanName(MemoryTimeFrame::Span span)
{
    switch (span) {
    case MemoryTimeFrame::Span::WeekContaining:
        return QStringLiteral("week_containing");
    case MemoryTimeFrame::Span::SurroundingDays:
        return QStringLiteral("surrounding_days");
    case MemoryTimeFrame::Span::MonthContaining:
        return QStringLiteral("month_containing");
    case MemoryTimeFrame::Span::Day:
    default:
        return QStringLiteral("day");
    }
}

QString expressionHash(const QString &expression)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(expression.toUtf8(),
                                 QCryptographicHash::Sha256)
            .left(8)
            .toHex());
}

} // namespace

MemoryExecutor::MemoryExecutor(MemoryManager *memoryManager,
                               const MemoryAgentScope &scope)
    : m_memoryManager(memoryManager)
    , m_scope(scope)
{
}

MemoryAgentResult MemoryExecutor::execute(const MemoryRecallPlan &plan)
{
    MemoryAgentResult result;
    result.accessScope = m_scope.accessScope;
    result.otherChannelsVisible = m_scope.otherChannelsVisible;
    result.needs.reserve(plan.needs.size());
    QHash<QString, int> itemIndexByToolId;
    int opaqueId = 0;
    bool anyFailure = false;
    QElapsedTimer runTimer;
    runTimer.start();

    for (const MemoryRecallNeed &need : plan.needs) {
        MemoryNeedResult needResult;
        needResult.needId = need.id;
        needResult.query = need.query;
        needResult.expression = need.timeExpression;
        needResult.timeTarget = need.timeTarget;
        needResult.expressionLength = need.timeExpression.size();
        needResult.expressionHash = expressionHash(need.timeExpression);
        needResult.framePresent = need.hasTimeFrame;
        needResult.accessScopeStatus = m_scope.accessScope;
        if (need.hasTimeFrame) {
            needResult.resolverType = QStringLiteral("time_frame");
            needResult.anchorType = frameAnchorName(need.timeFrame.anchor);
            needResult.frameOffset = need.timeFrame.offset;
            needResult.spanType = frameSpanName(need.timeFrame.span);
        } else {
            needResult.resolverType = need.hasTime
                                          ? QStringLiteral("legacy")
                                          : QStringLiteral("none");
        }

        QElapsedTimer timer;
        timer.start();

        if (m_scope.timeoutMs > 0
            && runTimer.elapsed() >= m_scope.timeoutMs) {
            needResult.status = QStringLiteral("timeout");
            needResult.evidenceState = QStringLiteral("partial");
            needResult.timeCertainty = QStringLiteral("unknown");
            needResult.error = QStringLiteral("retrieval_budget_exceeded");
            anyFailure = true;
            result.needs.append(needResult);
            continue;
        }

        if (need.timeTarget == MemoryTimeTarget::FactValidity) {
            needResult.status = QStringLiteral("unsupported");
            needResult.resolverStatus = QStringLiteral("unsupported");
            needResult.evidenceState = QStringLiteral("unresolved");
            needResult.timeCertainty = QStringLiteral("unknown");
            needResult.error = QStringLiteral("fact_validity_not_indexed");
            anyFailure = true;
            result.needs.append(needResult);
            continue;
        }

        // tool 路径：有 time 必须由模型给出结构化 frame。旧 Resolver 仍可服务
        // legacy 路径，但不能继续作为普通工具路径的默认后备。
        if (need.hasTime && !need.hasTimeFrame && m_scope.requireTimeFrame) {
            needResult.status = QStringLiteral("frame_required");
            needResult.resolverType = QStringLiteral("none");
            needResult.resolverStatus = QStringLiteral("frame_required");
            needResult.evidenceState = QStringLiteral("unresolved");
            needResult.timeCertainty = QStringLiteral("unknown");
            needResult.error = QStringLiteral("time_frame_required");
            anyFailure = true;
            result.needs.append(needResult);
            continue;
        }

        QStringList kinds = need.kinds;
        if (kinds.isEmpty()) {
            kinds = {QStringLiteral("group_recent"),
                     QStringLiteral("interaction_summary"),
                     QStringLiteral("claim"),
                     QStringLiteral("episodic"),
                     QStringLiteral("long_term_summary")};
        }
        needResult.sourceKinds = kinds;
        QJsonObject args;
        args.insert(QStringLiteral("query"), need.query);
        args.insert(QStringLiteral("kinds"), QJsonArray::fromStringList(kinds));
        args.insert(QStringLiteral("limit"), qMin(8, qMax(1, m_scope.maxResults)));
        args.insert(QStringLiteral("time_target"),
                    memoryTimeTargetToString(need.timeTarget));

        if (need.hasTime) {
            MemoryResolvedTime resolved = need.hasTimeFrame
                ? TimeFrameResolver::resolve(
                      need.timeExpression, need.timeTarget, need.timeFrame,
                      QDateTime::currentDateTime(), m_scope.timezone)
                : MemoryTimeResolver::resolve(
                      need.timeExpression, need.timeTarget,
                      QDateTime::currentDateTime(), m_scope.timezone);
            needResult.resolvedFrom = resolved.from.toString(Qt::ISODate);
            needResult.resolvedTo = resolved.to.toString(Qt::ISODate);
            needResult.interpretation = resolved.interpretation;
            if (!resolved.valid) {
                needResult.status = QStringLiteral("unresolved");
                needResult.resolverStatus = QStringLiteral("unresolved");
                needResult.evidenceState = QStringLiteral("unresolved");
                needResult.timeCertainty = QStringLiteral("unknown");
                needResult.error = resolved.error;
                anyFailure = true;
                result.needs.append(needResult);
                continue;
            }
            needResult.resolverStatus = QStringLiteral("ok");
            needResult.timeConstraintApplied = true;
            args.insert(QStringLiteral("time_from"), resolved.from.toString(Qt::ISODate));
            args.insert(QStringLiteral("time_to"), resolved.to.toString(Qt::ISODate));
            // episodic.event_time / long_term_summary 没有可靠的 message_time 映射；
            // 这类数据源只能做 content fallback，不能声称时间条件已满足。
            if (need.timeTarget == MemoryTimeTarget::MessageTime
                && (kinds.contains(QStringLiteral("episodic"))
                    || kinds.contains(QStringLiteral("long_term_summary")))) {
                needResult.timeConstraintApplied = false;
                needResult.timeCertainty = QStringLiteral("approximate");
                anyFailure = true;
            }
        } else {
            needResult.resolverStatus = QStringLiteral("not_required");
        }

        MemoryAgentScope needScope = m_scope;
        needScope.maxResults = qMin(8, qMax(1, m_scope.maxResults));
        MemoryAgentTools tools(m_memoryManager, needScope, false);
        QString error;
        const QJsonObject toolResult = tools.execute(
            QStringLiteral("memory_search"), args, &error);

        const QList<MemoryAgentItem> found = tools.items();
        needResult.resultCount = found.size();
        needResult.scannedCount = found.size();
        needResult.totalCandidates = found.size();
        if (!error.isEmpty()) {
            needResult.status = error == QStringLiteral("denied")
                                    ? QStringLiteral("denied")
                                    : (error == QStringLiteral("invalid_arguments")
                                           ? QStringLiteral("invalid")
                                           : QStringLiteral("error"));
            needResult.evidenceState = needResult.status == QStringLiteral("denied")
                                           ? QStringLiteral("denied")
                                           : QStringLiteral("partial");
            needResult.timeCertainty = QStringLiteral("unknown");
            needResult.error = error;
            anyFailure = true;
        } else if (found.isEmpty()) {
            needResult.status = QStringLiteral("empty");
            needResult.evidenceState = QStringLiteral("empty");
            needResult.timeCertainty = needResult.timeConstraintApplied
                                           ? QStringLiteral("exact")
                                           : QStringLiteral("unknown");
        } else {
            needResult.status = need.timeTarget == MemoryTimeTarget::EventTime
                                    && !need.hasTimeFrame
                                    ? QStringLiteral("fallback")
                                    : QStringLiteral("success");
            needResult.evidenceState = QStringLiteral("found");
            needResult.timeCertainty = needResult.timeConstraintApplied
                                           ? QStringLiteral("exact")
                                           : QStringLiteral("unknown");
        }
        if (need.timeTarget == MemoryTimeTarget::EventTime) {
            // 当前没有独立事件时间索引。这里明确标记为 message_time fallback，
            // 不把消息时间伪装成事件发生时间。
            needResult.interpretation =
                needResult.interpretation.isEmpty()
                    ? QStringLiteral("event_time_message_time_fallback")
                    : needResult.interpretation
                          + QStringLiteral("|event_time_message_time_fallback");
            needResult.timeConstraintApplied = false;
            needResult.timeCertainty = QStringLiteral("approximate");
            anyFailure = true;
        }
        if (toolResult.value(QStringLiteral("truncated")).toBool(false)) {
            result.truncated = true;
        }

        for (const MemoryAgentItem &item : found) {
            auto existing = itemIndexByToolId.find(item.id);
            if (existing != itemIndexByToolId.end()) {
                MemoryAgentItem &target = result.items[existing.value()];
                if (!target.matchedNeedIds.contains(need.id)) {
                    target.matchedNeedIds.append(need.id);
                }
                continue;
            }
            if (result.items.size() >= 16) {
                result.truncated = true;
                break;
            }
            MemoryAgentItem copy = item;
            copy.id = QStringLiteral("item:%1").arg(++opaqueId);
            copy.matchedNeedIds = {need.id};
            itemIndexByToolId.insert(item.id, result.items.size());
            result.items.append(copy);
        }

        needResult.latencyMs = int(timer.elapsed());
        result.needs.append(needResult);
    }

    anyFailure = anyFailure || result.truncated;
    // complete：所有 need 都正常结束（包括明确 empty）；
    // empty：全部正常结束但没有任何证据；
    // partial：至少一个 need 未完成（unresolved/timeout/error/denied/fallback/truncated）；
    // failed：执行器不可用且没有任何可安全交付的结果。
    bool anyNeed = false;
    bool allFailed = true;
    for (const MemoryNeedResult &need : result.needs) {
        anyNeed = true;
        if (need.status != QStringLiteral("error")
            && need.status != QStringLiteral("denied")) {
            allFailed = false;
        }
    }

    if (result.items.isEmpty() && anyNeed && allFailed) {
        result.status = QStringLiteral("failed");
    } else if (result.items.isEmpty() && anyFailure) {
        result.status = QStringLiteral("partial");
    } else if (result.items.isEmpty()) {
        result.status = QStringLiteral("empty");
    } else if (anyFailure) {
        result.status = QStringLiteral("partial");
    } else {
        result.status = QStringLiteral("complete");
    }
    return result;
}
