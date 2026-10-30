#include "memorytimeresolver.h"

#include <QRegularExpression>
#include <QTimeZone>

namespace {

QString compact(const QString &text)
{
    QString value = text.trimmed();
    value.remove(QLatin1Char(' '));
    value.remove(QLatin1Char('\t'));
    value.remove(QLatin1Char('\n'));
    value.remove(QLatin1Char('\r'));
    return value;
}

QDateTime makeDateTime(const QDate &date,
                       const QTime &time,
                       const QString &timezone)
{
    const QTimeZone zone = timezone.isEmpty()
                               ? QTimeZone::systemTimeZone()
                               : QTimeZone(timezone.toUtf8());
    if (zone.isValid()) {
        return QDateTime(date, time, zone);
    }
    return QDateTime(date, time);
}

bool hasAny(const QString &text, const QStringList &values)
{
    for (const QString &value : values) {
        if (text.contains(value)) {
            return true;
        }
    }
    return false;
}

int weekdayFromChinese(const QString &value)
{
    if (value == QStringLiteral("一")) {
        return 1;
    }
    if (value == QStringLiteral("二")) {
        return 2;
    }
    if (value == QStringLiteral("三")) {
        return 3;
    }
    if (value == QStringLiteral("四")) {
        return 4;
    }
    if (value == QStringLiteral("五")) {
        return 5;
    }
    if (value == QStringLiteral("六")) {
        return 6;
    }
    if (value == QStringLiteral("日") || value == QStringLiteral("天")) {
        return 7;
    }
    return -1;
}

QDate mondayOfWeek(const QDate &date)
{
    return date.addDays(1 - date.dayOfWeek());
}

struct Period {
    int start = -1;
    int end = -1;
};

void includePeriod(Period *period, int start, int end)
{
    if (period->start < 0) {
        period->start = start;
        period->end = end;
        return;
    }
    period->start = qMin(period->start, start);
    period->end = qMax(period->end, end);
}

} // namespace

MemoryResolvedTime MemoryTimeResolver::resolve(const QString &expression,
                                               MemoryTimeTarget target,
                                               const QDateTime &anchor,
                                               const QString &timezone)
{
    MemoryResolvedTime result;
    result.originalExpression = expression.trimmed();
    result.timezone = timezone;
    result.target = target;
    if (!anchor.isValid()) {
        result.error = QStringLiteral("invalid_anchor");
        return result;
    }

    const QString text = compact(expression);
    if (text.isEmpty()) {
        result.error = QStringLiteral("empty_expression");
        return result;
    }

    if (hasAny(text, {QStringLiteral("刚才"), QStringLiteral("刚刚")})) {
        result.from = anchor.addSecs(-30 * 60);
        result.to = anchor.addSecs(1);
        result.interpretation = QStringLiteral("exact");
        result.valid = true;
        return result;
    }

    bool explicitDate = false;
    bool today = false;
    bool previousCompleted = false;
    QDate date = anchor.date();

    if (hasAny(text, {QStringLiteral("大前天")})) {
        date = date.addDays(-3);
        explicitDate = true;
    } else if (text.contains(QStringLiteral("前天"))) {
        date = date.addDays(-2);
        explicitDate = true;
    } else if (hasAny(text, {QStringLiteral("昨晚"), QStringLiteral("昨夜"),
                             QStringLiteral("昨儿晚上")})) {
        date = date.addDays(-1);
        explicitDate = true;
    } else if (hasAny(text, {QStringLiteral("昨天"), QStringLiteral("昨日")})) {
        date = date.addDays(-1);
        explicitDate = true;
    } else if (hasAny(text, {QStringLiteral("今天"), QStringLiteral("今日")})) {
        today = true;
        explicitDate = true;
    } else if (hasAny(text, {QStringLiteral("明天"), QStringLiteral("明日")})) {
        date = date.addDays(1);
        explicitDate = true;
    }

    if (!explicitDate) {
        const QRegularExpression weekdayRe(
            QStringLiteral("(上|本|这|下)?(?:周|星期)([一二三四五六日天])"));
        const QRegularExpressionMatch match = weekdayRe.match(text);
        if (match.hasMatch()) {
            const QString prefix = match.captured(1);
            const int weekday = weekdayFromChinese(match.captured(2));
            if (weekday > 0) {
                int weekOffset = 0;
                if (prefix == QStringLiteral("上")) {
                    weekOffset = -1;
                } else if (prefix == QStringLiteral("下")) {
                    weekOffset = 1;
                }
                QDate candidate = mondayOfWeek(anchor.date())
                                      .addDays(weekOffset * 7 + weekday - 1);
                if (prefix.isEmpty() && candidate > anchor.date()) {
                    candidate = candidate.addDays(-7);
                }
                date = candidate;
                explicitDate = true;
            }
        }
    }

    if (!explicitDate) {
        const QRegularExpression weekRe(
            QStringLiteral("(上|本|这|下)(?:周|星期)"));
        const QRegularExpressionMatch match = weekRe.match(text);
        if (match.hasMatch()) {
            int weekOffset = 0;
            if (match.captured(1) == QStringLiteral("上")) {
                weekOffset = -1;
            } else if (match.captured(1) == QStringLiteral("下")) {
                weekOffset = 1;
            }
            date = mondayOfWeek(anchor.date()).addDays(weekOffset * 7);
            explicitDate = true;
            result.interpretation = QStringLiteral("whole_week");
        }
    }

    if (!explicitDate) {
        const QRegularExpression fullDateRe(
            QStringLiteral("(\\d{4})[-/.年](\\d{1,2})[-/.月](\\d{1,2})日?"));
        const QRegularExpressionMatch match = fullDateRe.match(text);
        if (match.hasMatch()) {
            date = QDate(match.captured(1).toInt(),
                         match.captured(2).toInt(),
                         match.captured(3).toInt());
            explicitDate = date.isValid();
        }
    }
    if (!explicitDate) {
        const QRegularExpression monthDayRe(
            QStringLiteral("(\\d{1,2})月(\\d{1,2})日?"));
        const QRegularExpressionMatch match = monthDayRe.match(text);
        if (match.hasMatch()) {
            date = QDate(anchor.date().year(), match.captured(1).toInt(),
                         match.captured(2).toInt());
            explicitDate = date.isValid();
        }
    }

    Period period;
    const bool dawn = text.contains(QStringLiteral("凌晨"));
    const bool morning = hasAny(text, {QStringLiteral("上午"),
                                       QStringLiteral("早上"),
                                       QStringLiteral("早晨")});
    const bool noon = text.contains(QStringLiteral("中午"));
    const bool afternoon = text.contains(QStringLiteral("下午"));
    const bool dusk = text.contains(QStringLiteral("傍晚"));
    const bool evening = hasAny(text, {QStringLiteral("晚上"),
                                       QStringLiteral("今晚"),
                                       QStringLiteral("夜里"),
                                       QStringLiteral("夜间"),
                                       QStringLiteral("昨晚"),
                                       QStringLiteral("昨夜"),
                                       QStringLiteral("昨儿晚上")});
    if (dawn) {
        includePeriod(&period, 0, 6);
    }
    if (morning) {
        includePeriod(&period, 6, 12);
    }
    if (noon) {
        includePeriod(&period, 11, 14);
    }
    if (afternoon) {
        includePeriod(&period, 12, 18);
    }
    if (dusk) {
        includePeriod(&period, 17, 19);
    }
    if (evening) {
        includePeriod(&period, 18, 24);
    }

    if (!explicitDate && period.start < 0) {
        result.error = QStringLiteral("unresolved");
        result.interpretation = QStringLiteral("unresolved");
        return result;
    }

    if (!explicitDate) {
        // 只有时段时使用当天；凌晨问“下午/晚上”通常指刚过去的那个时段。
        if (anchor.time() < QTime(6, 0)
            && (afternoon || dusk || evening)) {
            date = date.addDays(-1);
            previousCompleted = true;
        }
    } else if (today && anchor.time() < QTime(6, 0)
               && !dawn && (afternoon || dusk || evening)) {
        date = date.addDays(-1);
        previousCompleted = true;
    }

    if (period.start < 0) {
        period.start = 0;
        period.end = 24;
    }

    result.from = makeDateTime(date, QTime(period.start, 0), timezone);
    if (!result.from.isValid()) {
        result.error = QStringLiteral("invalid_date");
        return result;
    }
    int endHour = period.end;
    QDate endDate = date;
    if (endHour >= 24) {
        endHour = 0;
        endDate = endDate.addDays(1);
    }
    result.to = makeDateTime(endDate, QTime(endHour, 0), timezone);
    if (!result.to.isValid() || result.from >= result.to) {
        result.error = QStringLiteral("invalid_range");
        return result;
    }
    if (result.from.secsTo(result.to) > 31LL * 24 * 3600) {
        result.error = QStringLiteral("range_too_large");
        return result;
    }

    if (previousCompleted) {
        result.interpretation = QStringLiteral("previous_completed_period");
    } else if (result.interpretation.isEmpty()) {
        result.interpretation = QStringLiteral("exact");
    }
    result.valid = true;
    return result;
}
