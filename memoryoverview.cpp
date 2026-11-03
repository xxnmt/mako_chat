#include "memoryoverview.h"

#include <QDateTime>
#include <QTimeZone>
#include <QJsonValue>

#include "memorymanager.h"
#include "memorysourcepolicy.h"
#include "timeframeresolver.h"

namespace {

int boundedInt(const QJsonValue &value, int fallback, int minimum, int maximum)
{
    if (!value.isDouble()) {
        return fallback;
    }
    return qBound(minimum, value.toInt(fallback), maximum);
}

QJsonObject errorPayload(const QString &error, const QString &message)
{
    return QJsonObject{
        {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("mode"), QStringLiteral("overview")},
        {QStringLiteral("error"), error},
        {QStringLiteral("message"), message}};
}

} // namespace

MemoryOverviewResult buildMemoryOverview(MemoryManager *manager,
                                         const MemoryAgentScope &scope,
                                         const MemoryRecallPlan &plan,
                                         QString *error)
{
    if (error) {
        error->clear();
    }
    MemoryOverviewResult result;
    if (!manager || plan.needs.isEmpty()) {
        if (error) {
            *error = QStringLiteral("invalid_overview_plan");
        }
        result.payload = errorPayload(QStringLiteral("invalid_overview_plan"),
                                      QStringLiteral("overview needs one need"));
        return result;
    }

    const MemoryRecallNeed &need = plan.needs.first();
    if (need.hasTime && !need.hasTimeFrame && scope.requireTimeFrame) {
        if (error) {
            *error = QStringLiteral("time_frame_required");
        }
        result.payload = errorPayload(
            QStringLiteral("time_frame_required"),
            QStringLiteral("overview requires a structured time frame"));
        return result;
    }
    MemoryResolvedTime time;
    if (need.hasTime) {
        time = need.hasTimeFrame
            ? TimeFrameResolver::resolve(
                  need.timeExpression, need.timeTarget, need.timeFrame,
                  QDateTime::currentDateTime(), scope.timezone)
            : MemoryTimeResolver::resolve(
                  need.timeExpression, need.timeTarget,
                  QDateTime::currentDateTime(), scope.timezone);
    }
    if (!need.hasTime || !time.valid) {
        if (error) {
            *error = time.error.isEmpty()
                         ? QStringLiteral("unresolved_time")
                         : time.error;
        }
        result.payload = errorPayload(
            QStringLiteral("unresolved_time"),
            QStringLiteral("overview requires a resolvable time range"));
        return result;
    }

    const int bucketMinutes = boundedInt(
        plan.overview.value(QStringLiteral("bucket_minutes")), 180, 30, 360);
    const int maxBuckets = boundedInt(
        plan.overview.value(QStringLiteral("max_buckets")), 8, 2, 12);

    QString channel = scope.channel;
    QString sessionId = scope.sessionId;
    if (scope.channel != Channel::kQQGroup) {
        if (scope.allowedGroupIds.isEmpty()) {
            if (error) {
                *error = QStringLiteral("no_authorized_group");
            }
            result.payload = errorPayload(
                QStringLiteral("no_authorized_group"),
                QStringLiteral("no authorized group for overview"));
            return result;
        }
        channel = Channel::kQQGroup;
        sessionId = scope.allowedGroupIds.first();
    }
    if (!MemorySourcePolicy::canReadInteractionSummary(scope, channel,
                                                       sessionId)) {
        if (error) {
            *error = QStringLiteral("denied");
        }
        result.payload = errorPayload(
            QStringLiteral("denied"),
            QStringLiteral("interaction summaries are not available"));
        return result;
    }

    const int scanLimit = 1000;
    const QList<InteractionSummary> summaries =
        manager->getInteractionSummariesOverlapping(
            channel, sessionId, time.from, time.to, scanLimit);
    const int totalInteractions =
        manager->countInteractionsInRange(channel, sessionId, time.from, time.to);
    const int readySummaries = summaries.size();
    const bool scanTruncated = readySummaries >= scanLimit;

    const qint64 fromMs = time.from.toMSecsSinceEpoch();
    const qint64 toMs = time.to.toMSecsSinceEpoch();
    const qint64 bucketMs = qint64(bucketMinutes) * 60 * 1000;
    const int bucketCount = qMin(
        maxBuckets,
        qMax(1, int((toMs - fromMs + bucketMs - 1) / bucketMs)));

    struct Bucket {
        qint64 fromMs = 0;
        qint64 toMs = 0;
        QList<InteractionSummary> summaries;
    };
    QList<Bucket> buckets;
    buckets.reserve(bucketCount);
    for (int i = 0; i < bucketCount; ++i) {
        Bucket bucket;
        bucket.fromMs = fromMs + i * bucketMs;
        bucket.toMs = qMin(toMs, fromMs + (i + 1) * bucketMs);
        buckets.append(bucket);
    }

    int aggregated = 0;
    for (const InteractionSummary &summary : summaries) {
        if (!summary.endedAt.isValid()) {
            continue;
        }
        const qint64 at = summary.endedAt.toMSecsSinceEpoch();
        int index = int((at - fromMs) / bucketMs);
        index = qBound(0, index, buckets.size() - 1);
        buckets[index].summaries.append(summary);
        ++aggregated;
    }

    QJsonArray bucketArray;
    int exposed = 0;
    int emptyBuckets = 0;
    int handleIndex = 0;
    int itemOrdinal = 0;
    for (const Bucket &bucket : buckets) {
        QJsonArray summariesJson;
        QJsonArray handleItems;
        const int count = bucket.summaries.size();
        QList<int> picks;
        if (count > 0) {
            picks.append(0);
            if (count > 2) {
                picks.append(count / 2);
            }
            if (count > 1) {
                picks.append(count - 1);
            }
        }
        auto summaryJson = [&itemOrdinal](const InteractionSummary &summary) {
            QJsonObject item;
            item.insert(QStringLiteral("id"),
                        QStringLiteral("overview-item:%1").arg(++itemOrdinal));
            item.insert(QStringLiteral("kind"),
                        QStringLiteral("interaction_summary"));
            item.insert(QStringLiteral("time"),
                        summary.endedAt.toString(
                            QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            item.insert(QStringLiteral("text"), summary.summaryText);
            return item;
        };
        // handles 指向桶内完整摘要集合；representative 只是直接暴露给模型的子集。
        for (const InteractionSummary &summary : bucket.summaries) {
            handleItems.append(summaryJson(summary));
        }
        for (int index : picks) {
            summariesJson.append(summaryJson(bucket.summaries.at(index)));
            ++exposed;
        }

        QJsonObject bucketJson;
        bucketJson.insert(QStringLiteral("from"),
                          QDateTime::fromMSecsSinceEpoch(bucket.fromMs).toString(
                              QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        bucketJson.insert(QStringLiteral("to"),
                          QDateTime::fromMSecsSinceEpoch(bucket.toMs).toString(
                              QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        bucketJson.insert(QStringLiteral("count"), count);
        bucketJson.insert(QStringLiteral("representative_summaries"), summariesJson);
        const QString handle = QStringLiteral("bucket:%1").arg(++handleIndex);
        bucketJson.insert(QStringLiteral("handles"), QJsonArray{handle});
        bucketArray.append(bucketJson);
        result.handles.insert(handle, handleItems);
        if (count == 0) {
            ++emptyBuckets;
        }
    }

    const int missingSummaries = qMax(0, totalInteractions - readySummaries);
    const int missingRanges =
        (scanTruncated || missingSummaries > 0) ? emptyBuckets : 0;
    const bool scanComplete = !scanTruncated && readySummaries == totalInteractions;
    const bool aggregationComplete = aggregated == readySummaries;
    const bool exposureComplete = exposed > 0;
    const bool complete = scanComplete && aggregationComplete
                          && exposureComplete && totalInteractions > 0;

    QJsonObject coverage;
    coverage.insert(QStringLiteral("from"),
                    time.from.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    coverage.insert(QStringLiteral("to"),
                    time.to.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    coverage.insert(QStringLiteral("timezone"), time.timezone);
    coverage.insert(QStringLiteral("total_interactions"), totalInteractions);
    coverage.insert(QStringLiteral("ready_summaries"), readySummaries);
    coverage.insert(QStringLiteral("scanned"), readySummaries);
    coverage.insert(QStringLiteral("aggregated"), aggregated);
    coverage.insert(QStringLiteral("exposed"), exposed);
    coverage.insert(QStringLiteral("missing_summaries"), missingSummaries);
    coverage.insert(QStringLiteral("missing_ranges"), missingRanges);
    coverage.insert(QStringLiteral("empty_buckets"), emptyBuckets);
    coverage.insert(QStringLiteral("scan_complete"), scanComplete);
    coverage.insert(QStringLiteral("aggregation_complete"), aggregationComplete);
    coverage.insert(QStringLiteral("exposure_complete"), exposureComplete);
    coverage.insert(QStringLiteral("truncated"), scanTruncated);

    QJsonObject payload;
    payload.insert(QStringLiteral("mode"), QStringLiteral("overview"));
    payload.insert(QStringLiteral("status"),
                   complete ? QStringLiteral("complete")
                            : (totalInteractions == 0 ? QStringLiteral("empty")
                                                      : QStringLiteral("partial")));
    payload.insert(QStringLiteral("evidence_state"),
                   complete ? QStringLiteral("found")
                            : (totalInteractions == 0 ? QStringLiteral("empty")
                                                      : QStringLiteral("partial")));
    payload.insert(QStringLiteral("untrusted"), true);
    if (!scope.accessScope.isEmpty()) {
        payload.insert(QStringLiteral("scope"),
                       QJsonObject{{QStringLiteral("access_scope"),
                                    scope.accessScope},
                                   {QStringLiteral("other_channels_visible"),
                                    scope.otherChannelsVisible}});
    }
    payload.insert(QStringLiteral("coverage"), coverage);
    payload.insert(QStringLiteral("buckets"), bucketArray);
    result.payload = payload;
    return result;
}
