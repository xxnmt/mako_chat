#ifndef MEMORYRETRIEVALWORKER_H
#define MEMORYRETRIEVALWORKER_H

#include <QObject>
#include <QString>

#include "memoryevidence.h"

// 查询侧网络检索 Worker：独立 SQL 连接 + 独立 Provider，
// 完成后把共同的 RetrievalResult 发回主线程。
class MemoryRetrievalWorker : public QObject
{
    Q_OBJECT
public:
    MemoryRetrievalWorker(const QString &memoryPath,
                          const RetrievalRequest &request,
                          const QString &embeddingEndpoint,
                          const QString &embeddingApiKey,
                          const QString &embeddingModel,
                          int embeddingDimension,
                          const QString &rerankEndpoint,
                          const QString &rerankApiKey,
                          const QString &rerankModel,
                          QObject *parent = nullptr);

public slots:
    void run();

signals:
    void finished(const RetrievalResult &result);

private:
    QString m_memoryPath;
    RetrievalRequest m_request;
    QString m_embeddingEndpoint;
    QString m_embeddingApiKey;
    QString m_embeddingModel;
    int m_embeddingDimension = 1024;
    QString m_rerankEndpoint;
    QString m_rerankApiKey;
    QString m_rerankModel;
};

#endif // MEMORYRETRIEVALWORKER_H
