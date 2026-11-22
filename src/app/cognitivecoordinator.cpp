#include "cognitivecoordinator.h"
#include "configmanager.h"
#include "llmservice.h"
#include "memorymanager.h"
#include <QTimer>

namespace {
constexpr int maximumWindowMs = 6000;

bool sameInputStream(const RequestMeta &a, const RequestMeta &b)
{
    return a.turnKind == "reply" && b.turnKind == "reply" && a.channel == b.channel
        && a.sessionId == b.sessionId && a.subject == b.subject && a.actorId == b.actorId
        && (a.channel == Channel::kPet || !a.actorId.isEmpty())
        && a.replyToMessageId == b.replyToMessageId && a.sensitive == b.sensitive
        && a.persistTurn == b.persistTurn && a.longTermMemoryEnabled == b.longTermMemoryEnabled;
}
}

CognitiveCoordinator::CognitiveCoordinator(MemoryManager &memory, LLMService &model, QObject *parent)
    : QObject(parent), review(memory), store(memory), service(model)
{
    idle.start();
    arrivalClock.start();
    collectTimer.setSingleShot(true);
    connect(&collectTimer, &QTimer::timeout, this, &CognitiveCoordinator::advance);
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
    connect(&service,&LLMService::concernUpdatesReady,this,
            [this](const QString &runId,const QJsonArray &updates,const RequestMeta &meta) {
        if (!owns(runId) || updates.isEmpty()) return;
        const auto turn=turnId();
        int committed=0, failed=0;
        for (const auto &value : updates) {
            const auto update=value.toObject();
            const auto ref=update["ref"].toString();
            const qlonglong head=ref.startsWith(QStringLiteral("reflection/"))
                ? ref.mid(QStringLiteral("reflection/").size()).toLongLong() : 0;
            if (head>0 && store.resolveConcern(head,turn,update["kind"].toString(),
                                               update["evidence_ref"].toString())) {
                ++committed;
                qInfo()<<"[Concern] resolved by="<<turn<<"ref="<<ref<<"kind="<<update["kind"].toString();
                continue;
            }
            ++failed;
            qWarning()<<"[Concern] update_failed by="<<turn<<"ref="<<ref;
        }
        store.recordCognitiveAudit("concern_update",runId,0,
            QJsonObject{{"event_id",meta.eventId},{"turn",turn},{"requested",updates.size()},
                        {"committed",committed},{"failed",failed},{"updates",updates}});
    });
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
    const auto now = arrivalClock.elapsed();
    const int quietWindowMs = config.inputPauseMs();
    ++sequence;
    if (quietWindowMs > 0 && !pending.isEmpty() && now < pending.back().readyAt
        && sameInputStream(pending.back().meta, meta)
        && (!meta.persistTurn || (id && pending.back().turnId && store.linkGroupedInput(pending.back().turnId,id)))) {
        auto &turn = pending.back();
        turn.text += '\n' + input;
        auto parts = turn.meta.inputEvents;
        for (const auto &part : meta.inputEvents) parts.append(part);
        auto sources=turn.meta.imageSources;
        sources.insert(meta.imageSources);
        turn.meta = meta;
        turn.meta.imageSources=std::move(sources);
        turn.meta.inputEvents = parts;
        turn.turnId = id;
        turn.sequence = sequence;
        turn.readyAt = qMin(now + quietWindowMs, turn.firstArrival + maximumWindowMs);
        qInfo() << "[Conversation] grouped inputs" << turn.firstTurn << id;
    } else {
        pending.enqueue({input, meta, sequence, id, id, now, now + quietWindowMs});
    }
    advance();
    if (observe && id) service.processObservation();
}

void CognitiveCoordinator::advance()
{
    if (active || pending.isEmpty()) return;
    const auto remaining = pending.head().readyAt - arrivalClock.elapsed();
    if (remaining > 0) {
        collectTimer.start(int(remaining));
        return;
    }
    active = pending.dequeue();
    const auto &meta = active->meta;
    const auto history = meta.persistTurn
        ? store.getHistoryTurn(ConfigManager::instance().getShortMemoryLength(), meta.channel, meta.sessionId, active->firstTurn)
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
