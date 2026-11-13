#include "memoryindexworker.h"

#include "memorymanager.h"
#include "memoryembedding.h"
#include "configmanager.h"

#include <QThread>

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
        if (retrieval.embeddingEnabled
            && retrieval.embeddingProvider == QStringLiteral("local_hash_test")) {
            const int dimension = retrieval.embeddingDimension > 0
                ? retrieval.embeddingDimension : 64;
            LocalHashEmbeddingProvider provider(dimension);
            QString embedError;
            if (!memory.embedSearchChunks(&provider, 2000, &embedError)) {
                emit finished(false, embedError, memory.memoryChunkCount());
                return;
            }
        }
    }
    emit finished(ok, error, memory.memoryChunkCount());
}
