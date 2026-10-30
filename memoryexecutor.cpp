#include "memoryexecutor.h"

#include "memoryagent.h"
#include "memorytimeresolver.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QSet>

MemoryExecutor::MemoryExecutor(MemoryManager *memoryManager,
                               const MemoryAgentScope &scope)
    : m_memoryManager(memoryManager)
    , m_scope(scope)
{
}

MemoryAgentResult MemoryExecutor::execute(const MemoryRecallPlan &plan)
{
    MemoryAgentResult result;
    result.needs.reserve(plan.needs.size());
    QHash<QString, int> itemIndexByToolId;
    int opaqueId = 0;
    bool anyFailure = false;

    for (const MemoryRecallNeed &need : plan.needs) {
        MemoryNeedResult needResult;
        needResult.needId = need.id;
        needResult.query = need.query;
        needResult.expression = need.timeExpression;
        needResult.timeTarget = need.timeTarget;

        QElapsedTimer timer;
        timer.start();

        if (need.timeTarget == MemoryTimeTarget::FactValidity) {
            needResult.status = QStringLiteral("unsupported");
            needResult.error = QStringLiteral("fact_validity_not_indexed");
            anyFailure = true;
            result.needs.append(needResult);
            continue;
        }

        QStringList kinds = need.kinds;
        if (kinds.isEmpty()) {
            kinds = {QStringLiteral("recent"), QStringLiteral("summary"),
                     QStringLiteral("claim")};
        }
        QJsonObject args;
        args.insert(QStringLiteral("query"), need.query);
        args.insert(QStringLiteral("kinds"), QJsonArray::fromStringList(kinds));
        args.insert(QStringLiteral("limit"), qMin(8, qMax(1, m_scope.maxResults)));

        if (need.hasTime) {
            MemoryResolvedTime resolved = MemoryTimeResolver::resolve(
                need.timeExpression, need.timeTarget,
                QDateTime::currentDateTime(), m_scope.timezone);
            needResult.resolvedFrom = resolved.from.toString(Qt::ISODate);
            needResult.resolvedTo = resolved.to.toString(Qt::ISODate);
            needResult.interpretation = resolved.interpretation;
            if (!resolved.valid) {
                needResult.status = QStringLiteral("unresolved");
                needResult.error = resolved.error;
                anyFailure = true;
                result.needs.append(needResult);
                continue;
            }
            args.insert(QStringLiteral("time_from"), resolved.from.toString(Qt::ISODate));
            args.insert(QStringLiteral("time_to"), resolved.to.toString(Qt::ISODate));
        }

        MemoryAgentScope needScope = m_scope;
        needScope.maxResults = qMin(8, qMax(1, m_scope.maxResults));
        MemoryAgentTools tools(m_memoryManager, needScope, false);
        QString error;
        const QJsonObject toolResult = tools.execute(
            QStringLiteral("memory_search"), args, &error);

        const QList<MemoryAgentItem> found = tools.items();
        needResult.resultCount = found.size();
        if (!error.isEmpty()) {
            needResult.status = error == QStringLiteral("denied")
                                    ? QStringLiteral("denied")
                                    : (error == QStringLiteral("invalid_arguments")
                                           ? QStringLiteral("invalid")
                                           : QStringLiteral("error"));
            needResult.error = error;
            anyFailure = true;
        } else if (found.isEmpty()) {
            needResult.status = QStringLiteral("empty");
        } else {
            needResult.status = need.timeTarget == MemoryTimeTarget::EventTime
                                    ? QStringLiteral("fallback")
                                    : QStringLiteral("success");
        }
        if (need.timeTarget == MemoryTimeTarget::EventTime) {
            // 当前没有独立事件时间索引。这里明确标记为 message_time fallback，
            // 不把消息时间伪装成事件发生时间。
            needResult.interpretation =
                needResult.interpretation.isEmpty()
                    ? QStringLiteral("event_time_message_time_fallback")
                    : needResult.interpretation
                          + QStringLiteral("|event_time_message_time_fallback");
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

    if (result.items.isEmpty()) {
        result.status = QStringLiteral("empty");
    } else if (anyFailure) {
        result.status = QStringLiteral("partial");
    } else {
        result.status = QStringLiteral("ok");
    }
    return result;
}
