#pragma once

#include <QJsonObject>
#include <QTemporaryDir>

class BenchmarkSnapshot
{
public:
    bool open(const QString &source, const QJsonObject &question, const QStringList &arguments);
    QString path() const { return m_directory.filePath("replay.db"); }

private:
    QTemporaryDir m_directory;
};
