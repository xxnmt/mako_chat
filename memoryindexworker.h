#ifndef MEMORYINDEXWORKER_H
#define MEMORYINDEXWORKER_H

#include <QObject>
#include <QString>

// 独立线程重建索引。Worker 自己创建 MemoryManager 和独立 SQL 连接，
// 不跨线程复用主线程的 QSqlDatabase。
class MemoryIndexWorker : public QObject
{
    Q_OBJECT
public:
    explicit MemoryIndexWorker(const QString &memoryPath, QObject *parent = nullptr);

public slots:
    void run();

signals:
    void finished(bool ok, const QString &error, int chunks);

private:
    QString m_memoryPath;
};

#endif // MEMORYINDEXWORKER_H
