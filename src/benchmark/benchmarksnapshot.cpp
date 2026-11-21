#include "benchmarksnapshot.h"
#include "memorymanager.h"

#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

bool copyDatabase(const QString &source, const QString &destination, const QString &connection)
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setConnectOptions("QSQLITE_OPEN_READONLY");
    db.setDatabaseName(source);
    if (!db.open()) return false;
    QSqlQuery q(db);
    q.prepare("VACUUM INTO ?");
    q.addBindValue(destination);
    if (q.exec()) return true;
    qWarning() << "[BenchmarkSnapshot] copy:" << q.lastError();
    return false;
}

bool restrictSnapshot(QSqlDatabase db, int turn, QDateTime at)
{
    QSqlQuery q(db);
    if (turn > 0) {
        q.prepare("SELECT timestamp FROM chat_history WHERE id=?");
        q.addBindValue(turn);
        if (!q.exec() || !q.next()) return false;
        const auto boundary = q.value(0).toDateTime();
        if (!boundary.isValid()) return false;
        if (!at.isValid() || boundary < at) at = boundary;
        q.finish();
    }
    if (!at.isValid()) return true;
    if (!db.transaction()) return false;
    // SQLite local wall-clock columns and UTC revision timestamps have different
    // origins. Unknown write times cannot establish historical availability.
    auto apply = [&](const QString &sql) {
        q.prepare(sql);
        if (sql.contains(":at")) q.bindValue(":at", at.toUTC().toString(Qt::ISODateWithMs));
        if (sql.contains(":turn")) q.bindValue(":turn", turn);
        if (q.exec()) return true;
        qWarning() << "[BenchmarkSnapshot] cutoff:" << q.lastError();
        db.rollback();
        return false;
    };
    const QStringList statements = {
        "DELETE FROM chat_history WHERE (:turn>0 AND id>:turn) OR "
        "coalesce(julianday(timestamp,'utc')<=julianday(:at),0)=0",
        "DELETE FROM raw_events WHERE coalesce(julianday(occurred_at,'utc')<=julianday(:at),0)=0 "
        "OR julianday(content_recorded_at)>julianday(:at)",
        "DELETE FROM memory_claims WHERE coalesce(julianday(updated_at,'utc')<=julianday(:at),0)=0",
        "DELETE FROM interactions WHERE coalesce(julianday(ended_at,'utc')<=julianday(:at),0)=0 "
        "OR EXISTS(SELECT 1 FROM json_each(interactions.source_event_ids) e "
        "WHERE NOT EXISTS(SELECT 1 FROM raw_events WHERE id=e.value))",
        "DELETE FROM memory_claims WHERE NOT EXISTS(SELECT 1 FROM interactions WHERE id=interaction_id)",
        "UPDATE interactions SET safe_summary=NULL,summary_segments=NULL,summary_status='pending' "
        "WHERE coalesce(julianday(content_recorded_at)<=julianday(:at),0)=0",
        "DELETE FROM long_term_summary WHERE (:turn>0 AND covered_turn_end_id>:turn) "
        "OR coalesce(julianday(updated_at,'utc')<=julianday(:at),0)=0",
        "DELETE FROM episodic_memory WHERE coalesce(julianday(content_recorded_at)<=julianday(:at),0)=0",
        "DELETE FROM cognitive_source WHERE coalesce(julianday(reported)<=julianday(:at),0)=0",
        "DELETE FROM private_reflection WHERE updated_at>=unixepoch(:at)",
        "DELETE FROM matter_revision WHERE coalesce(julianday(recorded)<=julianday(:at),0)=0",
        "DELETE FROM character_profile WHERE coalesce(julianday(updated_at,'utc')<=julianday(:at),0)=0",
        "DELETE FROM relationship_state WHERE coalesce(julianday(updated_at,'utc')<=julianday(:at),0)=0",
        "UPDATE chat_history SET thought_result=NULL,thought=NULL WHERE thought IS NOT NULL"
    };
    for (const auto &sql : statements)
        if (!apply(sql)) return false;
    return db.commit();
}

} // namespace

bool BenchmarkSnapshot::open(const QString &source, const QJsonObject &question,
                             const QStringList &arguments)
{
    if (!m_directory.isValid() || !QFileInfo(source).isFile()) return false;
    int turn = question.value("cutoff_turn").toInt();
    const auto index = arguments.indexOf("--max-turn");
    if (index >= 0) {
        bool ok = false;
        turn = arguments.value(index + 1).toInt(&ok);
        if (!ok || turn < 0) return false;
    }
    const auto now = question.value("now").toString();
    const auto at = QDateTime::fromString(now, Qt::ISODate);
    if (!now.isEmpty() && !at.isValid()) return false;
    const auto name = QUuid::createUuid().toString();
    const bool copied = copyDatabase(source, path(), name);
    QSqlDatabase::removeDatabase(name);
    if (!copied) return false;
    bool ok;
    {
        MemoryManager snapshot(path(), nullptr, name);
        ok = restrictSnapshot(QSqlDatabase::database(name), turn, at);
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}
