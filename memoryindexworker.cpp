#include "memoryindexworker.h"

#include "memorymanager.h"
#include "memoryembedding.h"
#include "memorydashscope.h"

#include <QScopeGuard>
#include <memory>

MemoryIndexWorker::MemoryIndexWorker(MemoryManager &memory, const MemoryRetrievalConfig &config)
    : QThread(&memory), m_path(memory.databasePath()), m_config(config),
      m_rebuild(config.indexEnabled && config.autoBackfill && memory.memoryChunkCount() == 0)
{
    if (!config.indexEnabled || (!config.embeddingEnabled && !m_rebuild))
        return;
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_policy = MemoryReadPolicy();
        m_busy = true;
        m_pending = false;
        start();
    });
    connect(this, &QThread::finished, this, [this] {
        m_busy = false;
        if (!m_error.isEmpty()) {
            qWarning() << "[MemoryIndex] background indexing failed; retry in ms" << m_retryMs << m_error;
            m_timer.start(m_retryMs);
            m_retryMs = qMin(60000, 2 * m_retryMs);
        } else {
            m_retryMs = 1000;
            qInfo() << "[MemoryIndex] background indexing complete";
            if (m_pending) schedule();
        }
        emit drained(m_error.isEmpty(), m_error);
    }, Qt::QueuedConnection);
    if (config.embeddingEnabled)
        connect(&memory, &MemoryManager::searchChunksChanged, this, &MemoryIndexWorker::schedule);
    schedule();
}

void MemoryIndexWorker::schedule()
{
    m_pending = true;
    if (!m_busy && !m_timer.isActive())
        m_timer.start(250);
}

MemoryIndexWorker::~MemoryIndexWorker()
{
    m_timer.stop();
    requestInterruption();
    wait();
}

void MemoryIndexWorker::run()
{
    m_error.clear();
    const QString connection = QStringLiteral("memory_index_worker_%1").arg(quintptr(this));
    const auto cleanup = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
    MemoryManager memory(m_path, nullptr, connection);
    if (m_rebuild) {
        if (!memory.rebuildSearchIndex(&m_error)) return;
        m_rebuild = false;
    }
    if (!m_config.embeddingEnabled) return;
    std::unique_ptr<MemoryEmbeddingProvider> provider;
    if (m_config.embeddingProvider == QStringLiteral("local_hash_test")) {
        provider = std::make_unique<LocalHashEmbeddingProvider>(
            m_config.embeddingDimension > 0 ? m_config.embeddingDimension : 64);
    } else if (m_config.embeddingProvider == QStringLiteral("dashscope")) {
        const QString key = m_config.embeddingApiKey.isEmpty()
            ? qEnvironmentVariable(m_config.embeddingApiKeyEnv.toUtf8().constData())
            : m_config.embeddingApiKey;
        provider = std::make_unique<DashScopeEmbeddingProvider>(
            QUrl(m_config.embeddingEndpoint), key,
            m_config.embeddingModel.isEmpty() ? QStringLiteral("text-embedding-v4") : m_config.embeddingModel,
            m_config.embeddingDimension > 0 ? m_config.embeddingDimension : 1024);
    } else {
        m_error = QStringLiteral("unknown embedding provider");
        return;
    }
    if (!memory.embedSearchChunks(provider.get(), 10, &m_error, &m_policy) && m_error.isEmpty())
        m_error = QStringLiteral("background embedding failed");
}
