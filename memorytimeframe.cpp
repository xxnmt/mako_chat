#include "memorytimeframe.h"

#include <QJsonValue>
#include <QSet>

namespace {

bool hasOnlyKeys(const QJsonObject &object, const QSet<QString> &allowed)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            return false;
        }
    }
    return true;
}

QString stringValue(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString().trimmed();
}

bool intValue(const QJsonObject &object, const QString &key, int *out)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return false;
    }
    *out = value.toInt();
    return true;
}

} // namespace

bool MemoryTimeFrame::fromJson(const QJsonObject &object,
                               MemoryTimeFrame *frame,
                               QString *error)
{
    if (error) {
        error->clear();
    }
    if (!frame || !hasOnlyKeys(object,
                               {QStringLiteral("anchor"),
                                QStringLiteral("occurrence"),
                                QStringLiteral("span"),
                                QStringLiteral("period")})) {
        if (error) {
            *error = QStringLiteral("invalid_frame_shape");
        }
        return false;
    }

    const QJsonValue anchorValue = object.value(QStringLiteral("anchor"));
    if (!anchorValue.isObject()) {
        if (error) {
            *error = QStringLiteral("invalid_anchor");
        }
        return false;
    }
    const QJsonObject anchor = anchorValue.toObject();
    const QString anchorType = stringValue(anchor, QStringLiteral("type"));
    MemoryTimeFrame parsed;

    if (anchorType == QStringLiteral("relative_day")) {
        if (!hasOnlyKeys(anchor, {QStringLiteral("type"),
                                  QStringLiteral("offset")})
            || !intValue(anchor, QStringLiteral("offset"), &parsed.offset)) {
            if (error) {
                *error = QStringLiteral("invalid_relative_day");
            }
            return false;
        }
        parsed.anchor = Anchor::RelativeDay;
    } else if (anchorType == QStringLiteral("relative_week")) {
        if (!hasOnlyKeys(anchor, {QStringLiteral("type"),
                                  QStringLiteral("offset"),
                                  QStringLiteral("weekday")})
            || !intValue(anchor, QStringLiteral("offset"), &parsed.offset)
            || !intValue(anchor, QStringLiteral("weekday"), &parsed.weekday)
            || parsed.weekday < 1 || parsed.weekday > 7) {
            if (error) {
                *error = QStringLiteral("invalid_relative_week");
            }
            return false;
        }
        parsed.anchor = Anchor::RelativeWeek;
    } else if (anchorType == QStringLiteral("relative_month")) {
        if (!hasOnlyKeys(anchor, {QStringLiteral("type"),
                                  QStringLiteral("offset")})
            || !intValue(anchor, QStringLiteral("offset"), &parsed.offset)) {
            if (error) {
                *error = QStringLiteral("invalid_relative_month");
            }
            return false;
        }
        parsed.anchor = Anchor::RelativeMonth;
    } else if (anchorType == QStringLiteral("absolute_date")) {
        parsed.date = stringValue(anchor, QStringLiteral("date"));
        if (!hasOnlyKeys(anchor, {QStringLiteral("type"),
                                  QStringLiteral("date")})
            || !QDate::fromString(parsed.date, Qt::ISODate).isValid()) {
            if (error) {
                *error = QStringLiteral("invalid_absolute_date");
            }
            return false;
        }
        parsed.anchor = Anchor::AbsoluteDate;
    } else if (anchorType == QStringLiteral("festival")) {
        parsed.festival = stringValue(anchor, QStringLiteral("name"));
        if (!hasOnlyKeys(anchor, {QStringLiteral("type"),
                                  QStringLiteral("name")})
            || parsed.festival.isEmpty()) {
            if (error) {
                *error = QStringLiteral("invalid_festival");
            }
            return false;
        }
        parsed.anchor = Anchor::Festival;
    } else if (anchorType == QStringLiteral("recent_window")) {
        if (!hasOnlyKeys(anchor, {QStringLiteral("type"),
                                  QStringLiteral("minutes")})
            || !intValue(anchor, QStringLiteral("minutes"), &parsed.offset)
            || parsed.offset < 1 || parsed.offset > 1440) {
            if (error) {
                *error = QStringLiteral("invalid_recent_window");
            }
            return false;
        }
        parsed.anchor = Anchor::RecentWindow;
    } else {
        if (error) {
            *error = QStringLiteral("unsupported_anchor");
        }
        return false;
    }

    const QJsonValue occurrenceValue = object.value(QStringLiteral("occurrence"));
    if (!occurrenceValue.isUndefined() && !occurrenceValue.isNull()) {
        if (!occurrenceValue.isObject()) {
            if (error) {
                *error = QStringLiteral("invalid_occurrence");
            }
            return false;
        }
        const QJsonObject occurrence = occurrenceValue.toObject();
        const QString type = stringValue(occurrence, QStringLiteral("type"));
        if (type == QStringLiteral("most_recent_past")) {
            if (!hasOnlyKeys(occurrence, {QStringLiteral("type")})) {
                if (error) {
                    *error = QStringLiteral("invalid_occurrence");
                }
                return false;
            }
            parsed.occurrence = Occurrence::MostRecentPast;
        } else if (type == QStringLiteral("next_future")) {
            if (!hasOnlyKeys(occurrence, {QStringLiteral("type")})) {
                if (error) {
                    *error = QStringLiteral("invalid_occurrence");
                }
                return false;
            }
            parsed.occurrence = Occurrence::NextFuture;
        } else if (type == QStringLiteral("year")
                   || type == QStringLiteral("year_offset")) {
            if (!hasOnlyKeys(occurrence, {QStringLiteral("type"),
                                          QStringLiteral("value")})
                || !intValue(occurrence, QStringLiteral("value"),
                             &parsed.occurrenceValue)) {
                if (error) {
                    *error = QStringLiteral("invalid_occurrence_value");
                }
                return false;
            }
            parsed.occurrence = type == QStringLiteral("year")
                                    ? Occurrence::Year
                                    : Occurrence::YearOffset;
        } else {
            if (error) {
                *error = QStringLiteral("unsupported_occurrence");
            }
            return false;
        }
    }

    const QJsonValue spanValue = object.value(QStringLiteral("span"));
    if (!spanValue.isUndefined() && !spanValue.isNull()) {
        if (!spanValue.isObject()) {
            if (error) {
                *error = QStringLiteral("invalid_span");
            }
            return false;
        }
        const QJsonObject span = spanValue.toObject();
        const QString type = stringValue(span, QStringLiteral("type"));
        if (type == QStringLiteral("day")) {
            if (!hasOnlyKeys(span, {QStringLiteral("type")})) {
                if (error) {
                    *error = QStringLiteral("invalid_span");
                }
                return false;
            }
            parsed.span = Span::Day;
        } else if (type == QStringLiteral("week_containing")) {
            if (!hasOnlyKeys(span, {QStringLiteral("type")})) {
                if (error) {
                    *error = QStringLiteral("invalid_span");
                }
                return false;
            }
            parsed.span = Span::WeekContaining;
        } else if (type == QStringLiteral("month_containing")) {
            if (!hasOnlyKeys(span, {QStringLiteral("type")})) {
                if (error) {
                    *error = QStringLiteral("invalid_span");
                }
                return false;
            }
            parsed.span = Span::MonthContaining;
        } else if (type == QStringLiteral("surrounding_days")) {
            if (!hasOnlyKeys(span, {QStringLiteral("type"),
                                    QStringLiteral("before_days"),
                                    QStringLiteral("after_days")})
                || !intValue(span, QStringLiteral("before_days"),
                             &parsed.beforeDays)
                || !intValue(span, QStringLiteral("after_days"),
                             &parsed.afterDays)
                || parsed.beforeDays < 0 || parsed.afterDays < 0
                || parsed.beforeDays > 31 || parsed.afterDays > 31) {
                if (error) {
                    *error = QStringLiteral("invalid_surrounding_days");
                }
                return false;
            }
            parsed.span = Span::SurroundingDays;
        } else {
            if (error) {
                *error = QStringLiteral("unsupported_span");
            }
            return false;
        }
    }

    parsed.period = stringValue(object, QStringLiteral("period"));
    if (!parsed.period.isEmpty()
        && parsed.period != QStringLiteral("morning")
        && parsed.period != QStringLiteral("afternoon")
        && parsed.period != QStringLiteral("evening")
        && parsed.period != QStringLiteral("night")) {
        if (error) {
            *error = QStringLiteral("unsupported_period");
        }
        return false;
    }
    if (!parsed.period.isEmpty()
        && parsed.span != Span::Day
        && parsed.span != Span::SurroundingDays) {
        if (error) {
            *error = QStringLiteral("period_requires_day_span");
        }
        return false;
    }

    *frame = parsed;
    return true;
}
