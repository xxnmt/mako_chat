#include "memorytimerange.h"

#include <QDate>
#include <QStringList>
#include <QTime>

namespace {

bool containsAny(const QString &text, const QStringList &values)
{
    for (const QString &value : values) {
        if (text.contains(value)) {
            return true;
        }
    }
    return false;
}

} // namespace

MemoryTimeRange MemoryTimeRangePlanner::parse(const QString &query,
                                              const QDateTime &now)
{
    MemoryTimeRange range;
    const QString text = query.trimmed();
    if (text.isEmpty()) {
        return range;
    }

    const bool hasJustNow = containsAny(
        text, {QStringLiteral("刚才"), QStringLiteral("刚刚")});
    const bool hasToday = containsAny(text, {QStringLiteral("今天"),
                                             QStringLiteral("今日")});
    const bool hasLastNight = containsAny(
        text, {QStringLiteral("昨晚"), QStringLiteral("昨夜"),
               QStringLiteral("昨儿晚上")});
    const bool hasYesterday = hasLastNight
                              || containsAny(text, {QStringLiteral("昨天"),
                                                    QStringLiteral("昨日")});
    const bool hasBeforeYesterday = text.contains(QStringLiteral("前天"));
    const bool hasDawn = text.contains(QStringLiteral("凌晨"));
    const bool hasMorning = containsAny(text, {QStringLiteral("上午"),
                                               QStringLiteral("早上"),
                                               QStringLiteral("早晨")});
    const bool hasNoon = text.contains(QStringLiteral("中午"));
    const bool hasAfternoon = text.contains(QStringLiteral("下午"));
    const bool hasEvening = hasLastNight
                            || containsAny(text, {QStringLiteral("晚上"),
                                                  QStringLiteral("今晚"),
                                                  QStringLiteral("夜里"),
                                                  QStringLiteral("夜间")});

    const bool hasPart = hasDawn || hasMorning || hasNoon
                         || hasAfternoon || hasEvening;
    const bool hasRetrievalCue = containsAny(
        text, {QStringLiteral("聊"), QStringLiteral("说"), QStringLiteral("发生"),
               QStringLiteral("做"), QStringLiteral("干"), QStringLiteral("谁"),
               QStringLiteral("什么"), QStringLiteral("记得"), QStringLiteral("群")});

    // “刚刚”只能在没有明确日期/时段时作为 recent 快捷方式。
    // “刚刚修复了系统，昨晚吃了什么”必须优先解析“昨晚”。
    if (!hasToday && !hasYesterday && !hasBeforeYesterday && !hasPart) {
        if (hasJustNow) {
            range.from = now.addSecs(-30 * 60);
            range.to = now.addSecs(1);
            range.label = QStringLiteral("recent");
            range.valid = true;
        }
        return range;
    }
    if (!hasPart && !hasRetrievalCue
        && !hasYesterday && !hasBeforeYesterday) {
        return range;
    }

    QDate date = now.date();
    if (hasYesterday) {
        date = date.addDays(-1);
    } else if (hasBeforeYesterday) {
        date = date.addDays(-2);
    } else if (hasToday && now.time() < QTime(6, 0) && !hasDawn) {
        // 凌晨说“今天下午/今天上午”通常指刚过去的那个白天。
        date = date.addDays(-1);
    } else if (!hasToday && !hasYesterday && !hasBeforeYesterday
               && now.time() < QTime(6, 0) && (hasAfternoon || hasEvening)) {
        date = date.addDays(-1);
    }

    int startHour = -1;
    int endHour = -1;
    auto include = [&](int start, int end) {
        startHour = startHour < 0 ? start : qMin(startHour, start);
        endHour = endHour < 0 ? end : qMax(endHour, end);
    };
    if (hasDawn) {
        include(0, 6);
    }
    if (hasMorning) {
        include(6, 12);
    }
    if (hasNoon) {
        include(11, 14);
    }
    if (hasAfternoon) {
        include(12, 18);
    }
    if (hasEvening) {
        include(18, 24);
    }
    if (startHour < 0) {
        include(0, 24);
    }

    range.from = QDateTime(date, QTime(startHour, 0));
    range.to = endHour >= 24
                   ? QDateTime(date.addDays(1), QTime(0, 0))
                   : QDateTime(date, QTime(endHour, 0));
    range.label = QStringLiteral("%1 %2-%3")
                      .arg(date.toString(QStringLiteral("yyyy-MM-dd")))
                      .arg(startHour)
                      .arg(endHour);
    range.valid = range.from.isValid() && range.to.isValid()
                  && range.from < range.to;
    return range;
}

bool MemoryTimeRangePlanner::hasExplicitCue(const QString &query,
                                            const QDateTime &now)
{
    return parse(query, now).valid;
}
