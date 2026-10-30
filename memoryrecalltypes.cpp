#include "memoryrecalltypes.h"

#include <QJsonArray>
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

bool isValidKind(const QString &kind)
{
    return kind == QStringLiteral("recent")
           || kind == QStringLiteral("summary")
           || kind == QStringLiteral("claim");
}

bool isValidSourceHint(const QString &value)
{
    return value.isEmpty()
           || value == QStringLiteral("current_group")
           || value == QStringLiteral("allowed_groups")
           || value == QStringLiteral("private");
}

} // namespace

QString memoryTimeTargetToString(MemoryTimeTarget target)
{
    switch (target) {
    case MemoryTimeTarget::EventTime:
        return QStringLiteral("event_time");
    case MemoryTimeTarget::FactValidity:
        return QStringLiteral("fact_validity");
    case MemoryTimeTarget::MessageTime:
    default:
        return QStringLiteral("message_time");
    }
}

MemoryTimeTarget memoryTimeTargetFromString(const QString &value)
{
    if (value == QStringLiteral("event_time")) {
        return MemoryTimeTarget::EventTime;
    }
    if (value == QStringLiteral("fact_validity")) {
        return MemoryTimeTarget::FactValidity;
    }
    return MemoryTimeTarget::MessageTime;
}

bool MemoryRecallPlan::fromJson(const QJsonObject &object,
                                MemoryRecallPlan *plan,
                                QString *error)
{
    if (error) {
        error->clear();
    }
    if (!plan || !hasOnlyKeys(object, {QStringLiteral("needs")})) {
        if (error) {
            *error = QStringLiteral("invalid_plan_shape");
        }
        return false;
    }

    const QJsonValue needsValue = object.value(QStringLiteral("needs"));
    if (!needsValue.isArray()) {
        if (error) {
            *error = QStringLiteral("invalid_needs");
        }
        return false;
    }
    const QJsonArray needs = needsValue.toArray();
    if (needs.size() > 4) {
        if (error) {
            *error = QStringLiteral("too_many_needs");
        }
        return false;
    }

    MemoryRecallPlan parsed;
    QSet<QString> ids;
    for (const QJsonValue &value : needs) {
        if (!value.isObject()) {
            if (error) {
                *error = QStringLiteral("invalid_need");
            }
            return false;
        }
        const QJsonObject needObject = value.toObject();
        if (!hasOnlyKeys(needObject, {QStringLiteral("id"), QStringLiteral("query"),
                                      QStringLiteral("kinds"),
                                      QStringLiteral("source_hint"),
                                      QStringLiteral("time")})) {
            if (error) {
                *error = QStringLiteral("unsupported_need_field");
            }
            return false;
        }

        MemoryRecallNeed need;
        need.id = needObject.value(QStringLiteral("id")).toString().trimmed();
        need.query = needObject.value(QStringLiteral("query")).toString().trimmed();
        if (need.id.isEmpty() || need.id.size() > 32 || ids.contains(need.id)
            || need.query.isEmpty()) {
            if (error) {
                *error = QStringLiteral("invalid_need_id_or_query");
            }
            return false;
        }
        ids.insert(need.id);

        const QJsonValue kindsValue = needObject.value(QStringLiteral("kinds"));
        if (kindsValue.isArray()) {
            const QJsonArray kinds = kindsValue.toArray();
            for (const QJsonValue &kindValue : kinds) {
                const QString kind = kindValue.toString();
                if (!isValidKind(kind) || need.kinds.contains(kind)) {
                    if (error) {
                        *error = QStringLiteral("invalid_kinds");
                    }
                    return false;
                }
                need.kinds.append(kind);
            }
        } else if (!kindsValue.isUndefined() && !kindsValue.isNull()) {
            if (error) {
                *error = QStringLiteral("invalid_kinds");
            }
            return false;
        }
        if (need.kinds.isEmpty()) {
            need.kinds = {QStringLiteral("recent"), QStringLiteral("summary"),
                          QStringLiteral("claim")};
        }

        need.sourceHint = needObject.value(QStringLiteral("source_hint")).toString();
        if (!isValidSourceHint(need.sourceHint)) {
            if (error) {
                *error = QStringLiteral("invalid_source_hint");
            }
            return false;
        }

        const QJsonValue timeValue = needObject.value(QStringLiteral("time"));
        if (!timeValue.isUndefined() && !timeValue.isNull()) {
            if (!timeValue.isObject()) {
                if (error) {
                    *error = QStringLiteral("invalid_time");
                }
                return false;
            }
            const QJsonObject timeObject = timeValue.toObject();
            if (!hasOnlyKeys(timeObject, {QStringLiteral("expression"),
                                          QStringLiteral("target")})) {
                if (error) {
                    *error = QStringLiteral("unsupported_time_field");
                }
                return false;
            }
            need.timeExpression =
                timeObject.value(QStringLiteral("expression")).toString().trimmed();
            if (need.timeExpression.isEmpty() || need.timeExpression.size() > 64) {
                if (error) {
                    *error = QStringLiteral("invalid_time_expression");
                }
                return false;
            }
            const QString target = timeObject.value(QStringLiteral("target")).toString();
            if (!target.isEmpty()
                && target != QStringLiteral("message_time")
                && target != QStringLiteral("event_time")
                && target != QStringLiteral("fact_validity")) {
                if (error) {
                    *error = QStringLiteral("invalid_time_target");
                }
                return false;
            }
            need.timeTarget = memoryTimeTargetFromString(target);
            need.hasTime = true;
        }

        parsed.needs.append(need);
    }

    if (plan) {
        *plan = parsed;
    }
    return true;
}
