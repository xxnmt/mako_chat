#include "cognitivecoordinator.h"
#include "configmanager.h"
#include "llmservice.h"
#include "memorymanager.h"
#include <QTimer>

CognitiveCoordinator::CognitiveCoordinator(MemoryManager &memory, LLMService &model, QObject *parent)
    : QObject(parent), review(memory), store(memory), service(model)
{
    idle.start();
    connect(&review, &PrivateReview::requested, &service, &LLMService::requestPrivateReview);
    connect(&review, &PrivateReview::cancelled, &service, &LLMService::cancelPrivateReview);
    connect(&service, &LLMService::privateReviewFinished, &review, &PrivateReview::receive);
    connect(&review, &PrivateReview::contactProposed, this, &CognitiveCoordinator::contactProposed);
    connect(&review, &PrivateReview::shareRequested, this, &CognitiveCoordinator::draftReadyToShare);
    auto *reviewTimer = new QTimer(this);
    connect(reviewTimer, &QTimer::timeout, this, &CognitiveCoordinator::reviewIfDue);
    reviewTimer->start(60000);
    auto *retryTimer = new QTimer(this);
    connect(retryTimer,&QTimer::timeout,&service,&LLMService::processObservation);
    retryTimer->start(30000);
    connect(&service,&LLMService::replyQuiet,this,[this](const QString &action,const QString &,const RequestMeta &meta) {
        if (!owns(meta.runId)) return;
        if (turnId() && !store.recordQuiet(turnId(),action))
            qWarning() << "[Conversation] quiet decision not recorded" << meta.runId;
        finish();
    });
    connect(&service,&LLMService::replyAbnormal,this,&CognitiveCoordinator::interrupt);
    connect(&service,&LLMService::internetErrorSignal,this,[this] { interrupt("network_error"); });
}

CognitiveCoordinator::~CognitiveCoordinator()
{
    review.yieldAttention(QStringLiteral("shutdown"));
    if (!finishing && turnId()) store.interruptTurn(turnId(), "shutdown");
    for (const auto &turn : std::as_const(pending))
        if (turn.turnId) store.interruptTurn(turn.turnId, "shutdown");
}

void CognitiveCoordinator::reviewIfDue()
{
    auto &cfg = ConfigManager::instance();
    if (!idleForWriting() || !cfg.getMemoryPolicyConfig().longTermMemoryEnabled
        || !service.hasWritingProvider()) return;
    const auto options = cfg.getProactiveConfig();
    review.wake(options.dailyLimit, options.cooldownSeconds);
}

bool CognitiveCoordinator::idleForWriting() const
{
    const auto options = ConfigManager::instance().getProactiveConfig();
    return options.privateWriting && !busy() && idle.elapsed()/1000 >= options.idleSeconds;
}

void CognitiveCoordinator::submit(const QString &input, const RequestMeta &meta)
{
    idle.restart();
    review.yieldAttention(QStringLiteral("incoming_message"));
    auto &config = ConfigManager::instance();
    const bool observe = config.getMemoryPolicyConfig().longTermMemoryEnabled &&
                         !config.isBlindZone(meta.channel, meta.sessionId);
    const auto id = store.recordInput(input, meta, observe);
    pending.enqueue({input, meta, ++sequence, id});
    advance();
    if (observe && id) service.processObservation();
}

void CognitiveCoordinator::advance()
{
    if (active || pending.isEmpty()) return;
    active = pending.dequeue();
    const auto &meta = active->meta;
    const auto history = meta.persistTurn
        ? store.getHistoryTurn(ConfigManager::instance().getShortMemoryLength(), meta.channel, meta.sessionId, turnId())
        : QList<HistoryTurn>{};
    service.askDeepSeek(active->text, history, meta);
}

void CognitiveCoordinator::interrupt(const QString &reason)
{
    if (!active || finishing) return;
    qInfo() << "[Conversation] interrupted run=" << active->meta.runId << "reason=" << reason;
    if (turnId() && !store.interruptTurn(turnId(), reason))
        qWarning() << "[Conversation] interruption not recorded" << active->meta.runId;
    finish();
}

bool CognitiveCoordinator::handedOff(const QString &input, const QString &reply, const QList<SentenceText> &sentences,
                                     const RequestMeta &meta)
{
    if (!owns(meta.runId)) return false;
    const bool saved = !turnId() || store.saveQATurn(input,reply,sentences,meta,turnId());
    if (!saved) qWarning() << "[Conversation] handoff not recorded" << meta.runId;
    finish();
    return saved;
}

void CognitiveCoordinator::finish()
{
    if (!active || finishing) return;
    finishing = true;
    const auto run = active->meta.runId;
    QTimer::singleShot(0,this,[this,run] {
        if (!active || active->meta.runId!=run) return;
        finishing = false;
        active.reset();
        advance();
    });
}
