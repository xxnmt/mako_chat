#include "timeframeresolver.h"

#include <QTime>
#include <QTimeZone>

#include "festivalcalendar.h"

namespace {

QDateTime atDate(const QDate &date, const QTime &time,
                 const QTimeZone &zone)
{
    return QDateTime(date, time, zone);
}

QTime periodStart(const QString &period)
{
    if (period == QStringLiteral("morning")) {
        return QTime(6, 0);
    }
    if (period == QStringLiteral("afternoon")) {
        return QTime(12, 0);
    }
    if (period == QStringLiteral("evening")) {
        return QTime(18, 0);
    }
    if (period == QStringLiteral("night")) {
        return QTime(0, 0);
    }
    return QTime(0, 0);
}

QTime periodEnd(const QString &period)
{
    if (period == QStringLiteral("morning")) {
        return QTime(12, 0);
    }
    if (period == QStringLiteral("afternoon")) {
        return QTime(18, 0);
    }
    if (period == QStringLiteral("evening")) {
        return QTime(0, 0);
    }
    if (period == QStringLiteral("night")) {
        return QTime(6, 0);
    }
    return QTime(0, 0);
}

QDate mondayOf(const QDate &date)
{
    return date.addDays(1 - date.dayOfWeek());
}

bool resolveFestivalDate(const MemoryTimeFrame &frame,
                         const QDateTime &anchor,
                         QDate *date,
                         QString *festivalId,
                         QString *error)
{
    int year = anchor.date().year();
    if (frame.occurrence == MemoryTimeFrame::Occurrence::Year) {
        year = frame.occurrenceValue;
    } else if (frame.occurrence == MemoryTimeFrame::Occurrence::YearOffset) {
        year += frame.occurrenceValue;
    } else if (frame.occurrence == MemoryTimeFrame::Occurrence::MostRecentPast) {
        QDate candidate;
        for (int i = 0; i < 4; ++i) {
            if (FestivalCalendar::resolve(frame.festival, year - i,
                                          &candidate, festivalId)
                && candidate <= anchor.date()) {
                *date = candidate;
                return true;
            }
        }
        if (error) {
            *error = QStringLiteral("festival_not_in_calendar");
        }
        return false;
    } else if (frame.occurrence == MemoryTimeFrame::Occurrence::NextFuture) {
        QDate candidate;
        for (int i = 0; i < 4; ++i) {
            if (FestivalCalendar::resolve(frame.festival, year + i,
                                          &candidate, festivalId)
                && candidate >= anchor.date()) {
                *date = candidate;
                return true;
            }
        }
        if (error) {
            *error = QStringLiteral("festival_not_in_calendar");
        }
        return false;
    }

    if (!FestivalCalendar::resolve(frame.festival, year, date, festivalId)) {
        if (error) {
            *error = QStringLiteral("festival_not_in_calendar");
        }
        return false;
    }
    return true;
}

} // namespace

MemoryResolvedTime TimeFrameResolver::resolve(
    const QString &expression,
    MemoryTimeTarget target,
    const MemoryTimeFrame &frame,
    const QDateTime &anchor,
    const QString &timezone)
{
    if (!frame.isValid()) {
        return MemoryTimeResolver::resolve(expression, target, anchor, timezone);
    }

    MemoryResolvedTime result;
    result.originalExpression = expression;
    result.target = target;
    result.timezone = timezone;
    result.interpretation = QStringLiteral("frame");

    const QTimeZone zone = timezone.isEmpty()
                               ? QTimeZone::systemTimeZone()
                               : QTimeZone(timezone.toUtf8());
    const QTimeZone effectiveZone = zone.isValid() ? zone : QTimeZone::systemTimeZone();

    QDate date = anchor.date();
    bool periodAdjusted = false;

    switch (frame.anchor) {
    case MemoryTimeFrame::Anchor::RelativeDay:
        date = anchor.date().addDays(frame.offset);
        break;
    case MemoryTimeFrame::Anchor::RelativeWeek:
        date = mondayOf(anchor.date())
                   .addDays(frame.offset * 7 + (frame.weekday - 1));
        break;
    case MemoryTimeFrame::Anchor::RelativeMonth:
        date = QDate(anchor.date().year(), anchor.date().month(), 1)
                   .addMonths(frame.offset);
        break;
    case MemoryTimeFrame::Anchor::AbsoluteDate:
        date = QDate::fromString(frame.date, Qt::ISODate);
        if (!date.isValid()) {
            result.error = QStringLiteral("invalid_absolute_date");
            return result;
        }
        break;
    case MemoryTimeFrame::Anchor::RecentWindow:
        result.from = anchor.addSecs(-frame.offset * 60);
        result.to = anchor.addSecs(1);
        result.valid = true;
        result.interpretation = QStringLiteral("recent_window");
        return result;
    case MemoryTimeFrame::Anchor::Festival: {
        QString festivalId;
        if (!resolveFestivalDate(frame, anchor, &date, &festivalId,
                                 &result.error)) {
            return result;
        }
        result.interpretation =
            QStringLiteral("festival:%1").arg(festivalId);
        break;
    }
    case MemoryTimeFrame::Anchor::None:
    default:
        result.error = QStringLiteral("unsupported_anchor");
        return result;
    }

    // 凌晨问“今天下午/晚上”时，回忆语境下解释为最近一个已结束的时段。
    if (frame.anchor == MemoryTimeFrame::Anchor::RelativeDay
        && frame.offset == 0 && anchor.time() < QTime(6, 0)
        && (frame.period == QStringLiteral("afternoon")
            || frame.period == QStringLiteral("evening"))) {
        date = date.addDays(-1);
        periodAdjusted = true;
        result.interpretation = QStringLiteral("previous_completed_period");
    }

    QDate fromDate = date;
    QDate toDate = date.addDays(1);
    if (frame.span == MemoryTimeFrame::Span::WeekContaining) {
        fromDate = mondayOf(date);
        toDate = fromDate.addDays(7);
    } else if (frame.span == MemoryTimeFrame::Span::MonthContaining) {
        fromDate = QDate(date.year(), date.month(), 1);
        toDate = fromDate.addMonths(1);
    } else if (frame.span == MemoryTimeFrame::Span::SurroundingDays) {
        fromDate = date.addDays(-frame.beforeDays);
        toDate = date.addDays(frame.afterDays + 1);
    }

    const bool hasPeriod =
        !frame.period.isEmpty()
        && (frame.span == MemoryTimeFrame::Span::Day
            || frame.span == MemoryTimeFrame::Span::SurroundingDays);
    if (hasPeriod) {
        const QTime fromTime = periodStart(frame.period);
        const QTime toTime = periodEnd(frame.period);
        QDate periodToDate = date;
        if (toTime <= fromTime) {
            periodToDate = periodToDate.addDays(1);
        }
        result.from = atDate(date, fromTime, effectiveZone);
        result.to = atDate(periodToDate, toTime, effectiveZone);
    } else {
        result.from = atDate(fromDate, QTime(0, 0), effectiveZone);
        result.to = atDate(toDate, QTime(0, 0), effectiveZone);
    }

    if (!result.from.isValid() || !result.to.isValid()
        || result.from >= result.to) {
        result.error = QStringLiteral("invalid_range");
        return result;
    }
    if (result.from.secsTo(result.to) > 31 * 24 * 3600) {
        result.error = QStringLiteral("range_too_large");
        return result;
    }
    if (periodAdjusted) {
        result.interpretation = QStringLiteral("previous_completed_period");
    }
    result.valid = true;
    return result;
}
