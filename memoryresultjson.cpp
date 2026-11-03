#include "memoryresultjson.h"

#include <QJsonArray>
#include <QStringList>

#include "memorytextutils.h"

namespace {

bool hasEmptyNeed(const MemoryAgentResult &result)
{
    for (const MemoryNeedResult &need : result.needs) {
        if (need.status == QStringLiteral("empty")) {
            return true;
        }
    }
    return false;
}

} // namespace

QJsonObject memoryAgentResultToJson(const MemoryAgentResult &result,
                                    int maxChars,
                                    int maxItemChars)
{
    const int charBudget = qMax(200, maxChars);
    const int itemBudget = qMax(80, maxItemChars);

    int used = 0;
    bool truncated = result.truncated;
    QJsonArray items;
    for (const MemoryAgentItem &item : result.items) {
        QString text = item.text.trimmed();
        if (text.isEmpty()) {
            continue;
        }
        if (text.size() > itemBudget) {
            text = text.left(itemBudget) + QStringLiteral("…");
            truncated = true;
        }
        if (used + text.size() > charBudget) {
            truncated = true;
            break;
        }

        QJsonObject json;
        json.insert(QStringLiteral("id"), item.id);
        json.insert(QStringLiteral("kind"), item.kind);
        json.insert(QStringLiteral("time"),
                    item.time.isValid()
                        ? item.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                        : QString());
        json.insert(QStringLiteral("text"), text);
        json.insert(QStringLiteral("source"), item.source);
        if (!item.groupLabel.isEmpty()) {
            json.insert(QStringLiteral("group"),
                        MemoryTextUtils::sanitizeIdentityText(
                            item.groupLabel, QStringLiteral("群聊")));
        }
        json.insert(QStringLiteral("is_viewer"), item.isViewer);
        if (!item.matchedNeedIds.isEmpty()) {
            json.insert(QStringLiteral("matched_need_ids"),
                        QJsonArray::fromStringList(item.matchedNeedIds));
        }
        items.append(json);
        used += text.size();
    }

    QJsonArray needs;
    bool sawFailure = false;
    for (const MemoryNeedResult &need : result.needs) {
        QJsonObject needJson;
        needJson.insert(QStringLiteral("id"), need.needId);
        needJson.insert(QStringLiteral("status"), need.status);
        needJson.insert(QStringLiteral("target"),
                        memoryTimeTargetToString(need.timeTarget));
        if (!need.interpretation.isEmpty()) {
            needJson.insert(QStringLiteral("interpretation"), need.interpretation);
        }
        if (!need.evidenceState.isEmpty()) {
            needJson.insert(QStringLiteral("evidence_state"), need.evidenceState);
        }
        if (!need.timeCertainty.isEmpty()) {
            needJson.insert(QStringLiteral("time_certainty"), need.timeCertainty);
        }
        needJson.insert(QStringLiteral("expression_length"),
                        need.expressionLength);
        needJson.insert(QStringLiteral("expression_hash"),
                        need.expressionHash);
        needJson.insert(QStringLiteral("frame_present"), need.framePresent);
        if (!need.anchorType.isEmpty()) {
            needJson.insert(QStringLiteral("anchor_type"), need.anchorType);
        }
        needJson.insert(QStringLiteral("frame_offset"), need.frameOffset);
        if (!need.spanType.isEmpty()) {
            needJson.insert(QStringLiteral("span_type"), need.spanType);
        }
        if (!need.resolverType.isEmpty()) {
            needJson.insert(QStringLiteral("resolver_type"), need.resolverType);
        }
        if (!need.resolverStatus.isEmpty()) {
            needJson.insert(QStringLiteral("resolver_status"), need.resolverStatus);
        }
        if (!need.accessScopeStatus.isEmpty()) {
            needJson.insert(QStringLiteral("access_scope_status"),
                            need.accessScopeStatus);
        }
        needJson.insert(QStringLiteral("time_constraint_applied"),
                        need.timeConstraintApplied);
        if (!need.sourceKinds.isEmpty()) {
            needJson.insert(QStringLiteral("source_kinds"),
                            QJsonArray::fromStringList(need.sourceKinds));
        }
        if (!need.resolvedFrom.isEmpty()) {
            needJson.insert(QStringLiteral("resolved_from"), need.resolvedFrom);
        }
        if (!need.resolvedTo.isEmpty()) {
            needJson.insert(QStringLiteral("resolved_to"), need.resolvedTo);
        }
        needJson.insert(QStringLiteral("scanned_count"), need.scannedCount);
        needJson.insert(QStringLiteral("result_count"), need.resultCount);
        needJson.insert(QStringLiteral("latency_ms"), need.latencyMs);
        if (!need.error.isEmpty()) {
            needJson.insert(QStringLiteral("error"), need.error);
        }
        needs.append(needJson);

        if (need.status != QStringLiteral("success")
            && need.status != QStringLiteral("empty")) {
            sawFailure = true;
        }
    }

    const QString status = result.status.isEmpty()
                               ? (items.isEmpty() ? QStringLiteral("empty")
                                                  : QStringLiteral("complete"))
                               : result.status;
    const bool partial = status == QStringLiteral("partial")
                         || status == QStringLiteral("failed")
                         || status == QStringLiteral("timeout")
                         || (sawFailure && status != QStringLiteral("complete"));

    QJsonObject payload;
    QString evidenceState = QStringLiteral("partial");
    if (status == QStringLiteral("complete")) {
        evidenceState = items.isEmpty() ? QStringLiteral("empty")
                                        : QStringLiteral("found");
    } else if (status == QStringLiteral("empty")) {
        evidenceState = QStringLiteral("empty");
    } else if (status == QStringLiteral("failed")) {
        evidenceState = QStringLiteral("denied");
    }
    payload.insert(QStringLiteral("status"), status);
    payload.insert(QStringLiteral("evidence_state"), evidenceState);
    payload.insert(QStringLiteral("partial"), partial);
    payload.insert(QStringLiteral("truncated"), truncated);
    payload.insert(QStringLiteral("has_empty"), hasEmptyNeed(result));
    payload.insert(QStringLiteral("untrusted"), true);
    if (!result.accessScope.isEmpty()) {
        payload.insert(QStringLiteral("scope"),
                       QJsonObject{{QStringLiteral("access_scope"),
                                    result.accessScope},
                                   {QStringLiteral("other_channels_visible"),
                                    result.otherChannelsVisible}});
    }
    payload.insert(QStringLiteral("items"), items);
    if (!needs.isEmpty()) {
        payload.insert(QStringLiteral("needs"), needs);
    }
    return payload;
}
