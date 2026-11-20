#include "festivalcalendar.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

namespace {

struct FestivalEntry {
    QString id;
    QStringList aliases;
    QMap<int, QDate> dates;
};

const QList<FestivalEntry> &entries()
{
    static QList<FestivalEntry> loaded;
    static bool attempted = false;
    if (attempted) {
        return loaded;
    }
    attempted = true;

    QFile file(QStringLiteral(":/calendar/festivals.json"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return loaded;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject object = value.toObject();
        FestivalEntry entry;
        entry.id = object.value(QStringLiteral("id")).toString();
        for (const QJsonValue &alias : object.value(QStringLiteral("aliases")).toArray()) {
            entry.aliases.append(alias.toString());
        }
        const QJsonObject dates = object.value(QStringLiteral("dates")).toObject();
        for (auto it = dates.constBegin(); it != dates.constEnd(); ++it) {
            const QDate date = QDate::fromString(it.value().toString(), Qt::ISODate);
            if (date.isValid()) {
                entry.dates.insert(it.key().toInt(), date);
            }
        }
        if (!entry.id.isEmpty() && !entry.aliases.isEmpty()) {
            loaded.append(entry);
        }
    }
    return loaded;
}

} // namespace

bool FestivalCalendar::resolve(const QString &name, int year,
                               QDate *date, QString *festivalId)
{
    if (!date || name.trimmed().isEmpty()) {
        return false;
    }
    for (const FestivalEntry &entry : entries()) {
        if (!entry.aliases.contains(name.trimmed())) {
            continue;
        }
        const auto it = entry.dates.constFind(year);
        if (it == entry.dates.constEnd()) {
            return false;
        }
        *date = it.value();
        if (festivalId) {
            *festivalId = entry.id;
        }
        return true;
    }
    return false;
}

QString FestivalCalendar::version()
{
    return QStringLiteral("festivals-2024-2026");
}
