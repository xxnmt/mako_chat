#pragma once

#include <QJsonObject>
#include <QSet>

// A revision describes knowledge at its report time, not a standing instruction.
namespace MatterSemantics {
inline QString compatibilityStatus(const QJsonObject &value)
{
    const auto lifecycle = value["lifecycle"].toString();
    return lifecycle == "completed" || lifecycle == "ended" ? "ended"
        : lifecycle == "cancelled" ? "cancelled"
        : value["certainty"] == "unknown" ? "unconfirmed" : value["certainty"].toString();
}
inline QJsonObject legacy(const QString &status)
{
    return {{"lifecycle", status == "ended" || status == "cancelled" ? status : "unknown"},
            {"certainty", status == "confirmed" || status == "tentative" || status == "disputed" ? status : "unknown"},
            {"temporal_scope", "unknown"}, {"completion_condition", ""},
            {"time_expression", ""}, {"time_anchor", ""}, {"legacy", true}};
}

inline bool valid(const QJsonObject &value)
{
    static const QSet<QString> keys{"lifecycle", "certainty", "temporal_scope", "completion_condition", "time_expression"};
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!keys.contains(it.key()) || !it.value().isString()) return false;
    if (value.size() != keys.size()) return false;
    return QSet<QString>{"planned", "active", "completed", "ended", "cancelled", "unknown"}.contains(value["lifecycle"].toString())
        && QSet<QString>{"confirmed", "tentative", "disputed", "unknown"}.contains(value["certainty"].toString())
        && QSet<QString>{"occurrence", "ongoing", "unknown"}.contains(value["temporal_scope"].toString())
        && value["completion_condition"].toString().size() <= 256
        && value["time_expression"].toString().size() <= 128;
}
}
