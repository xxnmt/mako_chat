#include "recentcontext.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

const QStringList &stopWords()
{
    static const QStringList words = {
        QStringLiteral("刚才"), QStringLiteral("刚刚"), QStringLiteral("前面"),
        QStringLiteral("群里"), QStringLiteral("群"), QStringLiteral("聊"),
        QStringLiteral("什么"), QStringLiteral("谁"), QStringLiteral("提议"),
        QStringLiteral("记得"), QStringLiteral("还"), QStringLiteral("在"),
        QStringLiteral("说"), QStringLiteral("讨论"), QStringLiteral("的"),
        QStringLiteral("了"), QStringLiteral("吗"), QStringLiteral("呢"),
        QStringLiteral("吧"), QStringLiteral("啊"), QStringLiteral("呀"),
        QStringLiteral("哦"), QStringLiteral("嗯"), QStringLiteral("是"),
        QStringLiteral("那"), QStringLiteral("个"), QStringLiteral("这"),
        QStringLiteral("有"), QStringLiteral("人"), QStringLiteral("我"),
        QStringLiteral("你"), QStringLiteral("他"), QStringLiteral("她"),
        QStringLiteral("它"), QStringLiteral("大家"), QStringLiteral("一下"),
        QStringLiteral("关于"), QStringLiteral("那个"), QStringLiteral("这个"),
        QStringLiteral("叫"), QStringLiteral("名字"), QStringLiteral("告诉"),
        QStringLiteral("提起"), QStringLiteral("提到")
    };
    return words;
}

int itemCost(const RecentContextItem &item)
{
    return item.senderDisplayName.size() + item.sanitizedText.size() + 32;
}

} // namespace

RecentContextMode RecentContextPlanner::detectMode(const QString &queryText)
{
    // “刚才群里聊了什么”即使夹带了某个昵称，也仍然是泛问。
    static const QStringList overviewPhrases = {
        QStringLiteral("聊什么"), QStringLiteral("聊了什么"),
        QStringLiteral("聊的什么"), QStringLiteral("聊啥"),
        QStringLiteral("说什么"), QStringLiteral("说啥"),
        QStringLiteral("讨论什么"), QStringLiteral("讨论啥"),
        QStringLiteral("在聊什么"), QStringLiteral("在说什么")
    };
    for (const QString &phrase : overviewPhrases) {
        if (queryText.contains(phrase)) {
            return RecentContextMode::RecentOverview;
        }
    }

    return extractTerms(queryText).isEmpty()
               ? RecentContextMode::RecentOverview
               : RecentContextMode::TargetedLookup;
}

QStringList RecentContextPlanner::extractTerms(const QString &queryText)
{
    QString cleaned = queryText;
    cleaned.remove(QRegularExpression(QStringLiteral("[\\p{P}\\p{S}\\s]")));
    for (const QString &word : stopWords()) {
        cleaned.remove(word);
    }
    if (cleaned.size() < 2) {
        return {};
    }

    QSet<QString> unique;
    QStringList terms;
    const int maxLength = qMin(4, cleaned.size());
    for (int len = 2; len <= maxLength; ++len) {
        for (int i = 0; i + len <= cleaned.size(); ++i) {
            const QString term = cleaned.mid(i, len);
            if (!unique.contains(term)) {
                unique.insert(term);
                terms.append(term);
            }
        }
    }
    if (terms.size() > 16) {
        terms = terms.mid(terms.size() - 16);
    }
    return terms;
}

bool RecentContextPlanner::isRecentCue(const QString &queryText)
{
    static const QStringList cues = {
        QStringLiteral("刚才"), QStringLiteral("刚刚"), QStringLiteral("前面"),
        QStringLiteral("上一个"), QStringLiteral("上一条"), QStringLiteral("群里"),
        QStringLiteral("聊什么"), QStringLiteral("聊了"), QStringLiteral("说什么"),
        QStringLiteral("讨论什么"), QStringLiteral("还记得")
    };
    for (const QString &cue : cues) {
        if (queryText.contains(cue)) {
            return true;
        }
    }
    return false;
}

QStringList RecentContextPlanner::allowedGroupIdsForRequest(
    bool isGroup,
    bool ownerPrivateOrPet,
    const QString &sessionId,
    bool groupMemoryEnabled,
    const QList<qlonglong> &groupAllow)
{
    QStringList groups;
    if (!groupMemoryEnabled) {
        return groups;
    }
    if (isGroup) {
        if (groupAllow.contains(sessionId.toLongLong())) {
            groups << sessionId;
        }
        return groups;
    }
    if (ownerPrivateOrPet) {
        for (qlonglong groupId : groupAllow) {
            groups << QString::number(groupId);
        }
    }
    return groups;
}

bool RecentContextPlanner::shouldFallbackToOverview(RecentContextMode mode,
                                                    bool bundleEmpty,
                                                    bool isGroup,
                                                    bool ownerPrivateOrPet)
{
    return bundleEmpty
           && mode == RecentContextMode::TargetedLookup
           && (isGroup || ownerPrivateOrPet);
}

bool RecentContextPlanner::isHistoricalCue(const QString &queryText)
{
    static const QStringList cues = {
        QStringLiteral("上次"), QStringLiteral("以前"), QStringLiteral("之前"),
        QStringLiteral("过去"), QStringLiteral("曾经"), QStringLiteral("很久")
    };
    for (const QString &cue : cues) {
        if (queryText.contains(cue)) {
            return true;
        }
    }
    return false;
}

RecentContextBundle RecentContextPlanner::build(
    const QList<RecentContextItem> &candidates,
    const RecentContextQuery &query)
{
    RecentContextBundle bundle;
    bundle.mode = query.mode;
    bundle.candidatesScanned = candidates.size();

    QList<RecentContextItem> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(),
              [](const RecentContextItem &a, const RecentContextItem &b) {
        if (a.occurredAt == b.occurredAt) {
            return a.rawEventId < b.rawEventId;
        }
        return a.occurredAt < b.occurredAt;
    });

    if (query.mode == RecentContextMode::TargetedLookup) {
        const QStringList terms = extractTerms(query.queryText);
        QSet<qlonglong> selectedIds;
        QSet<qlonglong> hitIds;
        QList<RecentContextItem> scored;

        // 命中项本身。
        for (int i = 0; i < sorted.size(); ++i) {
            double score = 0.0;
            for (const QString &term : terms) {
                const int textHits = sorted[i].sanitizedText.count(
                    term, Qt::CaseInsensitive);
                const int senderHits = sorted[i].senderDisplayName.count(
                    term, Qt::CaseInsensitive);
                score += (textHits + senderHits) * (1.0 + term.size() * 0.1);
            }
            if (score <= 0.0) {
                continue;
            }
            RecentContextItem hit = sorted[i];
            hit.lexicalScore = score;
            scored.append(hit);
            selectedIds.insert(hit.rawEventId);
            hitIds.insert(hit.rawEventId);
        }

        // 围绕命中项补前后 2 条同群消息，避免“谁先/谁开始”这类问题只看
        // 到孤立的一句“复读机”，却看不到前后对话。
        const int neighborRadius = 2;
        for (int i = 0; i < sorted.size(); ++i) {
            if (!hitIds.contains(sorted[i].rawEventId)) {
                continue;
            }
            for (int delta = -neighborRadius; delta <= neighborRadius; ++delta) {
                const int index = i + delta;
                if (index < 0 || index >= sorted.size()) {
                    continue;
                }
                if (sorted[index].sourceGroupId != sorted[i].sourceGroupId) {
                    continue;
                }
                if (selectedIds.contains(sorted[index].rawEventId)) {
                    continue;
                }
                RecentContextItem neighbor = sorted[index];
                neighbor.lexicalScore = 0.0;
                scored.append(neighbor);
                selectedIds.insert(neighbor.rawEventId);
            }
        }

        std::sort(scored.begin(), scored.end(),
                  [](const RecentContextItem &a, const RecentContextItem &b) {
            if (a.lexicalScore != b.lexicalScore) {
                return a.lexicalScore > b.lexicalScore;
            }
            return a.occurredAt > b.occurredAt;
        });

        int chars = 0;
        for (const RecentContextItem &item : scored) {
            if (bundle.items.size() >= query.itemLimit) {
                bundle.truncated = true;
                break;
            }
            const int cost = itemCost(item);
            if (chars + cost > query.charBudget) {
                bundle.truncated = true;
                continue;
            }
            bundle.items.append(item);
            chars += cost;
        }
        std::sort(bundle.items.begin(), bundle.items.end(),
                  [](const RecentContextItem &a, const RecentContextItem &b) {
            return a.occurredAt < b.occurredAt;
        });
        return bundle;
    }

    // RecentOverview：最近 hotWindowItems 条保留原文，更早的消息压缩成一行。
    const int hotStart = qMax(0, sorted.size() - query.hotWindowItems);
    QList<RecentContextItem> overview = sorted.mid(
        qMax(0, sorted.size() - query.itemLimitOverview));
    const int hotIndex = qMax(0, overview.size() - query.hotWindowItems);
    for (int i = 0; i < overview.size(); ++i) {
        overview[i].warm = i < hotIndex;
        if (overview[i].warm) {
            overview[i].sanitizedText =
                overview[i].sanitizedText.left(query.warmLineMaxChars);
        }
    }

    // 先保留热窗口，再从新到旧补温窗口，避免预算不足时只留下最旧消息。
    QList<RecentContextItem> selected;
    int chars = 0;
    for (int i = hotIndex; i < overview.size(); ++i) {
        selected.append(overview[i]);
        chars += itemCost(overview[i]);
    }
    for (int i = hotIndex - 1; i >= 0; --i) {
        const int cost = itemCost(overview[i]);
        if (chars + cost > query.overviewCharBudget) {
            bundle.truncated = true;
            continue;
        }
        selected.append(overview[i]);
        chars += cost;
    }
    std::sort(selected.begin(), selected.end(),
              [](const RecentContextItem &a, const RecentContextItem &b) {
        return a.occurredAt < b.occurredAt;
    });
    bundle.items = selected;
    Q_UNUSED(hotStart);
    return bundle;
}
