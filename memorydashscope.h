#ifndef MEMORYDASHSCOPE_H
#define MEMORYDASHSCOPE_H

#include <QString>
#include <QUrl>

#include "memoryembedding.h"
#include "memoryretrieval.h"

// DashScope 百炼 OpenAI 兼容 Embedding Provider。
// 注意：embed() 是同步网络调用，只应在后台索引 Worker/CLI 中使用；
// 查询侧接入前必须改成异步，不能直接在 UI 线程调用。
class DashScopeEmbeddingProvider : public MemoryEmbeddingProvider
{
public:
    DashScopeEmbeddingProvider(const QUrl &endpoint, const QString &apiKey,
                               const QString &model = QStringLiteral("text-embedding-v4"),
                               int dimension = 1024);
    QString modelId() const override;
    int dimension() const override;
    bool embed(const QString &text, QVector<float> *vector,
               QString *error = nullptr) const override;
    bool embedBatch(const QStringList &texts,
                    QVector<QVector<float>> *vectors,
                    QString *error = nullptr) const override;

private:
    QUrl m_endpoint;
    QString m_apiKey;
    QString m_model;
    int m_dimension = 1024;
};

// DashScope 文本 Rerank Provider（qwen3-rerank）。
// 同样为同步调用；查询侧应放入后台任务后再回到主线程。
class DashScopeReranker : public MemoryReranker
{
public:
    DashScopeReranker(const QUrl &endpoint, const QString &apiKey,
                      const QString &model = QStringLiteral("qwen3-rerank"));
    QString id() const override;
    QList<MemoryEvidence> rerank(const QString &query,
                                  const QList<MemoryEvidence> &candidates,
                                  int limit, QString *error = nullptr) const override;

private:
    QUrl m_endpoint;
    QString m_apiKey;
    QString m_model;
};

#endif // MEMORYDASHSCOPE_H
