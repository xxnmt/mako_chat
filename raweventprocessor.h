#ifndef RAWEVENTPROCESSOR_H
#define RAWEVENTPROCESSOR_H

#include <QObject>
#include <QList>

#include "historyturn.h"
#include "memorypolicy.h"
#include "interactionsegmenter.h"

class MemoryManager;
class LLMService;
class QTimer;

class RawEventProcessor : public QObject
{
    Q_OBJECT

public:
    RawEventProcessor(MemoryManager *memoryManager,
                      LLMService *llmService,
                      QObject *parent = nullptr);

    void start();
    void notifyNewEvent();
    void processNow();

private slots:
    void tick();
    void onSummaryReady(qlonglong interactionId, const QString &summary);
    void onSummaryFailed(qlonglong interactionId, const QString &reason);

private:
    void scan();
    void processSession(const QString &sessionId);
    void processPendingSummaries();

    MemoryManager *m_memoryManager = nullptr;
    LLMService *m_llmService = nullptr;
    QTimer *m_timer = nullptr;
    bool m_summaryInFlight = false;
};

#endif // RAWEVENTPROCESSOR_H
