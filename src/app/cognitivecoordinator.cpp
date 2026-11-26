#include "cognitivecoordinator.h"
#include "configmanager.h"
#include "llmservice.h"
#include "memorymanager.h"
#include <QDateTime>
#include <QJsonDocument>
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

// R12-0：[Wake] tick 是每轮 reviewIfDue 一行的结构化诊断。正常空转（no_candidate/no_idle/busy）
// 用 DEBUG，真正领到回顾或出现可解释的阻塞原因才用 INFO，避免每分钟刷一条 INFO。
void logWakeTick(const WakeResult &result)
{
    const bool informative = result.started
        || (result.reason != QStringLiteral("no_candidate")
            && result.reason != QStringLiteral("no_idle")
            && result.reason != QStringLiteral("busy"));
    const QString selected = result.started && !result.kind.isEmpty()
        ? result.kind : QStringLiteral("none");
    const auto line = QStringLiteral("[Wake] tick selected=%1 reason=%2 source=%3 next_due=%4")
        .arg(selected,
             result.reason.isEmpty() ? QStringLiteral("no_candidate") : result.reason,
             result.sourceRef.isEmpty() ? QStringLiteral("-") : result.sourceRef,
             result.nextEligibleAt.isValid()
                 ? result.nextEligibleAt.toLocalTime().toString(Qt::ISODate)
                 : QStringLiteral("-"));
    if (informative) qInfo().noquote() << line;
    else qDebug().noquote() << line;
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
        // B1：安静的答复也是一次"注意力处理完成"（close/defer 是明确终态），事件落 done，
        // 不让 message 事件永久停在 pending 里污染 backlog。
        if (active && active->attentionEvent > 0)
            store.completeAttentionEvent(active->attentionEvent, QStringLiteral("done"), action);
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
    // B1：进程启动时把崩溃遗留的 received 轮次登记为 recovery 事件（reference 幂等），
    // 由 reviewIfDue 在启动后的第一轮优先恢复；同时记录本次启动时刻供观测。
    qInfo() << "[Attention] recovery_queued=" << store.enqueueRecoveryEvents(120);
    // 上次运行若在回顾中退出，会留下 pending 的关注回顾；租约过期后放回可重领状态。
    const int staleReflections = store.recoverStaleReflections(1800);
    if (staleReflections > 0)
        qInfo() << "[Attention] stale_reflections_recovered=" << staleReflections;
    // R12-1a：OpenWake 用 SQLite 租约防重入。进程重启后把 running 且租约过期的唤醒
    // 置为 interrupted，并把调度推到 now，由 reviewIfDue 重新领取；本片只恢复内部唤醒。
    const int recoveredWakes = store.recoverOpenWakeLeases(1800);
    qInfo() << "[OpenWake] recovered=" << recoveredWakes;
    // 上次运行若在"联系投递"中退出，会留下 dispatching 的 contact 行。领取后无法确认消息
    // 是否已经交给渠道，只能按 at-most-once 落 unknown（可能已送达），不再重发。
    store.recoverStaleContacts(600);
    store.setAttentionState(QStringLiteral("last_start_at"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

CognitiveCoordinator::~CognitiveCoordinator()
{
    review.yieldAttention(QStringLiteral("shutdown"));
    if (!finishing && turnId()) store.interruptTurn(turnId(), "shutdown");
    for (const auto &turn : std::as_const(pending))
        if (turn.turnId) store.interruptTurn(turn.turnId, "shutdown");
    // 进程退出前把在途消息事件落终态，避免下次启动看到永远 pending 的消息。
    if (active && active->attentionEvent > 0)
        store.completeAttentionEvent(active->attentionEvent, QStringLiteral("abandoned"),
                                     QStringLiteral("shutdown"));
}

void CognitiveCoordinator::reviewIfDue()
{
    // 每轮打一次 backlog，用于区分"没东西可回顾"和"回顾被事件挡住"。
    // R12-0 降噪：只有 pending 非空、或最老 pending 已等待>0 秒时才 INFO；
    // 仅剩 by_state.done 的历史终态属于正常，用 DEBUG（此前 by_state 非空导致每分钟 INFO）。
    const auto backlog = store.attentionBacklog();
    const bool backlogHasPending =
        !backlog.value(QStringLiteral("pending")).toObject().isEmpty();
    const bool backlogHasOldest =
        backlog.value(QStringLiteral("oldest_age_seconds")).toInteger() > 0;
    const auto backlogLine = QStringLiteral("[Attention] backlog=")
        + QString::fromUtf8(QJsonDocument(backlog).toJson(QJsonDocument::Compact));
    if (backlogHasPending || backlogHasOldest) qInfo().noquote() << backlogLine;
    else qDebug().noquote() << backlogLine;
    // 中断恢复优先于一切新回顾：recovery 是最高优先级事件，处理完本轮已排队的恢复就结束
    // 本轮，避免回顾在"上次没说完的话还没收尾"时抢占注意力。只在确实存在 pending recovery
    // 时才领取，避免把 message/due_wake 这类普通事件错误地置成 claimed。
    if (backlog.value(QStringLiteral("pending")).toObject()
            .value(QStringLiteral("recovery")).toInt() > 0) {
        bool recoveredAny = false;
        // 单次 tick 最多恢复 16 轮，防止异常数据把一次 tick 拖成长事务。
        for (int recovered = 0; recovered < 16; ++recovered) {
            const auto event = store.claimAttentionEvent();
            if (event.value(QStringLiteral("kind")).toString() != QStringLiteral("recovery"))
                break;
            const auto turn = event.value(QStringLiteral("payload")).toObject()
                                  .value(QStringLiteral("turn")).toInteger();
            // turn 已不是 received（正常收尾或已删除）时 interruptTurn 返回 false，跳过即可；
            // 事件本身仍要落终态，否则会永远卡在 claimed 挡住后续恢复。
            if (turn > 0 && store.interruptTurn(turn, QStringLiteral("restart_recovery")))
                qInfo() << "[Attention] recovered turn=" << turn;
            store.completeAttentionEvent(event.value(QStringLiteral("id")).toInteger(),
                                         QStringLiteral("done"), QStringLiteral("recovered"));
            recoveredAny = true;
            if (store.attentionBacklog().value(QStringLiteral("pending")).toObject()
                    .value(QStringLiteral("recovery")).toInt() == 0)
                break;
        }
        if (recoveredAny) {
            WakeResult recovered;
            recovered.reason = QStringLiteral("recovery_handled");
            logWakeTick(recovered);
            return;
        }
    }
    auto &cfg = ConfigManager::instance();
    const auto options = cfg.getProactiveConfig();
    const auto openWake = cfg.getOpenWakeConfig();
    // 到期关注属于主动联系链（B4），privateWriting=false 但 proactive.enabled=true 时也要允许唤醒；
    // 具体能领取到活动/关注/新来源中的哪一类，由 PrivateReview 与记忆层的领取规则决定。
    // R12-1a：OpenWake 是独立内部入口（privateWriting=false 也可运行），但 daily_limit=0 时
    // 连它一起停掉——所有后台认知共享同一份每日预算。
    QString reason;
    if (!cfg.getMemoryPolicyConfig().longTermMemoryEnabled)
        reason = QStringLiteral("memory_disabled");
    else if (options.dailyLimit <= 0)
        reason = QStringLiteral("daily_limit");
    else if (!options.privateWriting && !options.enabled && !openWake.enabled)
        reason = QStringLiteral("disabled");
    else if (!service.hasWritingProvider())
        reason = QStringLiteral("no_provider");
    else if (!idleForAttention())
        reason = QStringLiteral("no_idle");
    if (!reason.isEmpty()) {
        // no_idle 是每分钟都会出现的正常状态，降为 DEBUG；配置/能力类原因保留 INFO。
        if (reason == QStringLiteral("no_idle"))
            qDebug().noquote() << QStringLiteral("[Attention] idle skip reason=%1").arg(reason);
        else
            qInfo().noquote() << QStringLiteral("[Attention] idle skip reason=%1").arg(reason);
        WakeResult skipped;
        skipped.reason = reason;
        logWakeTick(skipped);
        return;
    }
    // 空闲：登记一次"唤醒请求"事件（同一分钟幂等），按实际是否真的启动了回顾落 done。
    store.setAttentionState(QStringLiteral("last_wake_at"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    const auto wakeEvent = store.enqueueAttentionEvent(
        QStringLiteral("due_wake"),
        QStringLiteral("idle:%1").arg(QDateTime::currentSecsSinceEpoch() / 60),
        QJsonObject{{QStringLiteral("reason"), QStringLiteral("idle")}},
        QStringLiteral("idle"), 30);
    const auto wake = review.wake(options.dailyLimit, options.cooldownSeconds, openWake);
    store.setAttentionState(QStringLiteral("last_wake_at"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (wakeEvent > 0)
        store.completeAttentionEvent(wakeEvent, QStringLiteral("done"),
                                     wake.started ? QStringLiteral("requested") : QStringLiteral("none"));
    logWakeTick(wake);
}

bool CognitiveCoordinator::idleForAttention() const
{
    const auto options = ConfigManager::instance().getProactiveConfig();
    return !busy() && idle.elapsed()/1000 >= options.idleSeconds;
}

bool CognitiveCoordinator::idleForWriting() const
{
    // 保持既有行为：后台写作首先要求 privateWriting，空闲判定复用 idleForAttention。
    const auto options = ConfigManager::instance().getProactiveConfig();
    return options.privateWriting && idleForAttention();
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
    // B1：每条入站消息登记一个注意力事件，只为观测（不阻塞回复）。空 eventId 不产生事件；
    // (kind,ref) 幂等，重复提交同一事件不会重复入队。
    const qint64 attention = meta.eventId.isEmpty() ? 0
        : store.enqueueAttentionEvent(
              QStringLiteral("message"), meta.eventId,
              QJsonObject{{QStringLiteral("channel"), meta.channel},
                          {QStringLiteral("session"), meta.sessionId},
                          {QStringLiteral("sequence"), qint64(sequence)}},
              QString(), 50);
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
        // 合并后的轮次以最后一次提交的事件为代表（之前的事件自然留在 backlog 里可观测）。
        turn.attentionEvent = attention;
        qInfo() << "[Conversation] grouped inputs" << turn.firstTurn << id;
    } else {
        pending.enqueue({input, meta, sequence, id, id, now, now + quietWindowMs, attention});
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
    // B1：被打断的消息事件落 abandoned；延后原因单独存档，供下次启动/审计解释。
    if (active->attentionEvent > 0)
        store.completeAttentionEvent(active->attentionEvent, QStringLiteral("abandoned"), reason);
    store.setAttentionState(QStringLiteral("last_defer_reason"), reason);
    finish();
}

bool CognitiveCoordinator::handedOff(const QString &input, const QString &reply, const QList<SentenceText> &sentences,
                                     const RequestMeta &meta)
{
    if (!owns(meta.runId)) return false;
    const bool saved = !turnId() || store.saveQATurn(input,reply,sentences,meta,turnId());
    if (!saved) qWarning() << "[Conversation] handoff not recorded" << meta.runId;
    // B1：回复已交接给渠道，消息事件完成（失败时保持 pending，留待恢复/观测）。
    if (saved && active->attentionEvent > 0)
        store.completeAttentionEvent(active->attentionEvent, QStringLiteral("done"),
                                     QStringLiteral("handed_off"));
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
