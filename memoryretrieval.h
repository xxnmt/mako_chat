#ifndef MEMORYRETRIEVAL_H
#define MEMORYRETRIEVAL_H

#include <memory>

#include "memoryevidence.h"
#include "memoryembedding.h"

class MemoryManager;

class MemoryReranker
{
public:
    virtual ~MemoryReranker() = default;
    virtual QString id() const = 0;
    virtual QList<MemoryEvidence> rerank(const QString &query,
                                         const QList<MemoryEvidence> &candidates,
                                         int limit, QString *error = nullptr) const = 0;
};

class IdentityMemoryReranker : public MemoryReranker
{
public:
    QString id() const override { return QStringLiteral("identity_rrf"); }
    QList<MemoryEvidence> rerank(const QString &query,
                                  const QList<MemoryEvidence> &candidates,
                                  int limit, QString *error = nullptr) const override;
};

// 无真实 Reranker Provider 时的本地启发式：RRF 顺序 + 词面命中 + 时间近因。
// 它不是语义重排，只用于在降级路径上避免完全按时间截断。
class HeuristicMemoryReranker : public MemoryReranker
{
public:
    QString id() const override { return QStringLiteral("heuristic_lexical_recency"); }
    QList<MemoryEvidence> rerank(const QString &query,
                                  const QList<MemoryEvidence> &candidates,
                                  int limit, QString *error = nullptr) const override;
};

// 一个来源适配器只负责把原始记录映射成共同证据，不做第二套裁剪。
class MemorySourceAdapter
{
public:
    virtual ~MemorySourceAdapter() = default;
    virtual QList<MemoryEvidence> fetch(const RetrievalRequest &request) const = 0;
};

class LegacyMemorySourceAdapter : public MemorySourceAdapter
{
public:
    explicit LegacyMemorySourceAdapter(MemoryManager *manager);
    QList<MemoryEvidence> fetch(const RetrievalRequest &request) const override;

private:
    MemoryManager *m_manager = nullptr;
};

// 第一版统一服务：复用 personalEvidence 作为来源，先统一证据与入口；
// 词面/向量/RRF/重排属于后续阶段，不在这里伪装成已实现。
class MemoryRetrievalService
{
public:
    explicit MemoryRetrievalService(MemoryManager *manager = nullptr,
                                    std::shared_ptr<MemoryEmbeddingProvider> embedding = {},
                                    std::shared_ptr<MemoryReranker> reranker = {});
    void setMemoryManager(MemoryManager *manager);
    void setEmbeddingProvider(std::shared_ptr<MemoryEmbeddingProvider> embedding);
    void setReranker(std::shared_ptr<MemoryReranker> reranker);
    RetrievalResult retrieve(const RetrievalRequest &request) const;

private:
    QList<MemoryEvidence> retrieveIndexed(const RetrievalRequest &request,
                                          RetrievalResult *result) const;
    std::unique_ptr<MemorySourceAdapter> m_adapter;
    MemoryManager *m_manager = nullptr;
    std::shared_ptr<MemoryEmbeddingProvider> m_embedding;
    std::shared_ptr<MemoryReranker> m_reranker;
};

#endif // MEMORYRETRIEVAL_H
