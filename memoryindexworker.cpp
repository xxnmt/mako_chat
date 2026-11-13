#include "memoryindexworker.h"

#include "memorymanager.h"
#include "memoryembedding.h"
#include "memorydashscope.h"
#include "configmanager.h"

#include <QThread>

#include <memory>

MemoryIndexWorker::MemoryIndexWorker(const QString &memoryPath, QObject *parent)
    : QObject(parent), m_memoryPath(memoryPath)
{
}

void MemoryIndexWorker::run()
{
    if (m_memoryPath.isEmpty()) {
        emit finished(false, QStringLiteral("empty memory path"), 0);
        return;
    }
    const QString connection = QStringLiteral("memory_index_worker_%1")
        .arg(quintptr(QThread::currentThreadId()));
    MemoryManager memory(m_memoryPath, nullptr, connection);
    QString error;
    const bool ok = memory.rebuildSearchIndex(&error);
    if (ok) {
        const auto retrieval = ConfigManager::instance().getMemoryPolicyConfig().retrieval;
        std::unique_ptr<MemoryEmbeddingProvider> provider;
        if (retrieval.embeddingEnabled
            && retrieval.embeddingProvider == QStringLiteral("local_hash_test")) {
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 64;
            provider = std::make_unique<LocalHashEmbeddingProvider>(dimension);
        } else if (retrieval.embeddingEnabled
                   && retrieval.embeddingProvider == QStringLiteral("dashscope")) {
            const QString apiKey = !retrieval.embeddingApiKey.isEmpty()
                ? retrieval.embeddingApiKey
                : qEnvironmentVariable(retrieval.embeddingApiKeyEnv.toUtf8().constData());
            if (apiKey.isEmpty() || retrieval.embeddingEndpoint.isEmpty()) {
                emit finished(false, QStringLiteral("DashScope endpoint or API key missing"),
                              memory.memoryChunkCount());
                return;
            }
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 1024;
            provider = std::make_unique<DashScopeEmbeddingProvider>(
                QUrl(retrieval.embeddingEndpoint), apiKey,
                retrieval.embeddingModel.isEmpty()
                    ? QStringLiteral("text-embedding-v4") : retrieval.embeddingModel,
                dimension);
        }
        if (provider) {
            QString embedError;
            if (!memory.embedSearchChunks(provider.get(), 2000, &embedError)) {
                emit finished(false, embedError, memory.memoryChunkCount());
                return;
            }
        }
    }
    emit finished(ok, error, memory.memoryChunkCount());
}
