#include "memoryretrievalworker.h"

#include "memorymanager.h"
#include "memoryretrieval.h"
#include "memorydashscope.h"

#include <QThread>

#include <memory>

MemoryRetrievalWorker::MemoryRetrievalWorker(
    const QString &memoryPath, const RetrievalRequest &request,
    const QString &embeddingEndpoint, const QString &embeddingApiKey,
    const QString &embeddingModel, int embeddingDimension,
    const QString &rerankEndpoint, const QString &rerankApiKey,
    const QString &rerankModel, QObject *parent)
    : QObject(parent), m_memoryPath(memoryPath), m_request(request),
      m_embeddingEndpoint(embeddingEndpoint), m_embeddingApiKey(embeddingApiKey),
      m_embeddingModel(embeddingModel), m_embeddingDimension(embeddingDimension),
      m_rerankEndpoint(rerankEndpoint), m_rerankApiKey(rerankApiKey),
      m_rerankModel(rerankModel)
{
}

void MemoryRetrievalWorker::run()
{
    if (m_memoryPath.isEmpty()) {
        RetrievalResult result;
        result.status = QStringLiteral("degraded");
        emit finished(result);
        return;
    }
    const QString connection = QStringLiteral("memory_retrieval_worker_%1")
        .arg(quintptr(QThread::currentThreadId()));
    MemoryManager memory(m_memoryPath, nullptr, connection);
    MemoryRetrievalService service(&memory);
    if (!m_embeddingEndpoint.isEmpty() && !m_embeddingApiKey.isEmpty()) {
        service.setEmbeddingProvider(std::make_shared<DashScopeEmbeddingProvider>(
            QUrl(m_embeddingEndpoint), m_embeddingApiKey,
            m_embeddingModel.isEmpty() ? QStringLiteral("text-embedding-v4")
                                       : m_embeddingModel,
            m_embeddingDimension > 0 ? m_embeddingDimension : 1024));
    }
    if (!m_rerankEndpoint.isEmpty() && !m_rerankApiKey.isEmpty()) {
        service.setReranker(std::make_shared<DashScopeReranker>(
            QUrl(m_rerankEndpoint), m_rerankApiKey,
            m_rerankModel.isEmpty() ? QStringLiteral("qwen3-rerank") : m_rerankModel));
    }
    emit finished(service.retrieve(m_request));
}
