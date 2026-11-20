#pragma once

#include "memoryreadpolicy.h"
#include <QThread>
#include <QTimer>

class MemoryManager;

// The timer and notifications live on the owner's thread. Each drain opens
// an independent SQL connection and performs its network requests in run().
class MemoryIndexWorker final : public QThread
{
    Q_OBJECT
public:
    MemoryIndexWorker(MemoryManager &memory, const MemoryRetrievalConfig &config);
    ~MemoryIndexWorker() override;
    void reconfigure(const MemoryRetrievalConfig &config);

signals:
    void drained(bool ok, const QString &error);

protected:
    void run() override;

private:
    void schedule();

    QString m_path;
    MemoryManager &m_memory;
    MemoryRetrievalConfig m_nextConfig;
    MemoryRetrievalConfig m_config;
    MemoryReadPolicy m_policy;
    QTimer m_timer;
    bool m_rebuild = false;
    bool m_busy = false;
    bool m_pending = false;
    int m_retryMs = 1000;
    QString m_error;
};
