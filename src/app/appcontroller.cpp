#include "appcontroller.h"
#include "memoryindexworker.h"
#include "cognitivecoordinator.h"
#include "runtimelog.h"
#include "characterwidget.h"
#include "bubblewidget.h"
#include "llmservice.h"
#include "appearancemanager.h"
#include "globalmoodstate.h"
#include "ttsservice.h"
#include "chatwidget.h"
#include "anchormanager.h"
#include "configmanager.h"
// #include "settingswidget.h"
#include "settingswidget.h"
#include "memorymanager.h"
#include "timemanager.h"
#include "utterance.h"
#include "petchanneladapter.h"
#include "qqchanneladapter.h"
#include "sessionrouter.h"
#include "qqprotocolclient.h"
#include "groupmembershipservice.h"
#include "qqmessageparser.h"
#include "qqreplypolicy.h"
#include "memorypolicy.h"
#include "debuglog.h"
#include "raweventprocessor.h"
#include "actioncommitter.h"
#include "actionoutboxdispatcher.h"

#include <QDebug>
#include <QTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonDocument>

namespace {

bool isAllowedExtractionSubject(const QString &subject, const RequestMeta &meta)
{
    if (subject.isEmpty()) {
        return false;
    }
    if (subject == QStringLiteral("mako")) {
        return true;
    }
    if (!meta.subject.isEmpty() && subject == meta.subject) {
        return true;
    }
    if (subject == QStringLiteral("user")
        && (meta.channel == Channel::kPet || meta.subject == QStringLiteral("user"))) {
        return true;
    }
    if (meta.channel == Channel::kQQGroup
        && subject == QStringLiteral("group:%1").arg(meta.sessionId)) {
        return true;
    }
    return false;
}

QString privacyRulesJson(const QStringList &ruleIds)
{
    QJsonArray rules;
    for (const QString &ruleId : ruleIds) {
        rules.append(ruleId);
    }
    return QString::fromUtf8(QJsonDocument(rules).toJson(QJsonDocument::Compact));
}

} // namespace

AppController::AppController(QObject *parent)
    : QObject{parent}
{
    m_character = new CharacterWidget;
    m_bubble = new BubbleWidget;
    m_llmService = new LLMService(ConfigManager::instance().getApiKey());
    m_appearance = new AppearanceManager;
    m_globalMood = new GlobalMoodState(this);
    m_globalMood->setFollowExternalMood(ConfigManager::instance().getFollowExternalMood());
    m_ttsService = new TTSService;
    m_chatWidget = new ChatWidget;
    m_memoryManager = new MemoryManager(ConfigManager::instance().getMemoryPath());
    m_coordinator = new CognitiveCoordinator(*m_memoryManager, *m_llmService, this);
    m_memoryManager->seedIdentity(ConfigManager::instance().getMasterQQAccounts());
    const auto retrievalConfig =
        ConfigManager::instance().getMemoryPolicyConfig().retrieval;
    m_memoryIndex = new MemoryIndexWorker(*m_memoryManager, retrievalConfig);
    // m_settingsWidget = new SettingsWidget;
    // m_settingsWidget->setMemoryManager(m_memoryManager);
    m_settingsWidget =new SettingsWidget;
    m_settingsWidget->setMemoryManager(m_memoryManager);

    m_memoryManager->decayEpisodicMemory();        // 启动时执行一次情景记忆衰减
    m_llmService->setMemoryManager(m_memoryManager);
    m_rawEventProcessor = new RawEventProcessor(m_memoryManager, m_llmService, this);
    m_rawEventProcessor->start();

    m_timeManager =new TimeManager(this);
    applyTimeSettings();   // TD-037：启动时应用用户配置（夜晚时段/退火时长）

    // 渠道装配（Phase 0：桌宠渠道接管气泡私有表现；QQ 为 Phase 1 占位）
    m_petChannel = new PetChannelAdapter(m_bubble, this);
    // 批次 C：播放器归桌宠渠道；TTSService 只合成，sink 负责播放与气泡/立绘时序
    m_ttsService->setAudioSink(m_petChannel->audioSink());
    m_qqChannel = new QQChannelAdapter(this);
    m_sessionRouter = new SessionRouter(m_memoryManager, this);
    m_qqClient = new QQProtocolClient(this);
    m_qqClient->setConfig(ConfigManager::instance().getQQConfig());
    m_qqChannel->setClient(m_qqClient);
    m_membershipService = new GroupMembershipService(
        m_memoryManager, m_qqClient, this);
    m_membershipService->start();
    connect(m_qqClient, &QQProtocolClient::messageEvent, this,
            [this](const QJsonObject &event) {
        if (QQMessageParser::isSelfMessage(event)) {
            return;
        }
        const QQInboundMessage message = QQMessageParser::parse(event);
        if (!message.valid) {
            return;
        }
        const QQConfig qqConfig = ConfigManager::instance().getQQConfig();
        const MemoryPolicyConfig memoryPolicy =
            ConfigManager::instance().getMemoryPolicyConfig();
        const QQInboundDecision decision = QQInboundPolicyEngine::decide(
            message, qqConfig.reply, qqConfig.groupMemory, memoryPolicy,
            ConfigManager::instance().getMasterQQAccounts());
        if (!decision.persistRaw && !decision.persistChat && !decision.allowReply) {
            qDebug() << "[QQ]未通过发送者/记忆策略，已忽略（不记录）:"
                     << message.channel << message.sessionId
                     << "senderHash=" << qHash(message.senderId);
            return;
        }

        const SanitizeResult sanitized = SensitiveContentFilter::sanitize(
            message.text, message.rawJson, memoryPolicy.privacyRules);
        if (sanitized.blocked) {
            m_memoryManager->appendPrivacyBlockedEvent(
                QStringLiteral("qq"), message.channel, message.sessionId,
                message.messageId, message.senderId, message.speaker,
                message.occurredAt, sanitized.ruleIds, memoryPolicy.policyVersion);
            qDebug() << "[QQ]消息触发隐私拦截，已丢弃（不回复/不入上下文）:"
                     << message.channel << message.sessionId
                     << "senderHash=" << qHash(message.senderId);
            return;
        }

        if (decision.persistRaw) {
            RawEventWrite rawEvent;
            rawEvent.platform = QStringLiteral("qq");
            rawEvent.channel = message.channel;
            rawEvent.sessionId = message.sessionId;
            rawEvent.eventType = QStringLiteral("message");
            rawEvent.content = sanitized.text;
            rawEvent.messageId = message.messageId;
            rawEvent.direction = QStringLiteral("in");
            rawEvent.senderId = message.senderId;
            rawEvent.senderName = message.speaker;
            rawEvent.payloadJson = sanitized.payload;
            rawEvent.payloadNull = sanitized.payloadDropped;
            rawEvent.occurredAt = message.occurredAt;
            rawEvent.sensitive = sanitized.redacted;
            rawEvent.visibility = message.channel == Channel::kQQGroup
                                      ? QStringLiteral("channel")
                                      : QStringLiteral("private");
            rawEvent.redactionStatus = sanitized.redacted
                                           ? QStringLiteral("redacted")
                                           : QStringLiteral("none");
            rawEvent.redactionRules = privacyRulesJson(sanitized.ruleIds);
            rawEvent.policyVersion = memoryPolicy.policyVersion;
            m_memoryManager->appendRawEvent(rawEvent);
            if (m_rawEventProcessor) {
                m_rawEventProcessor->notifyNewEvent();
            }
        }

        qDebug() << "[QQ]收到消息:" << message.channel << message.sessionId
                 << "senderHash=" << qHash(message.senderId)
                 << "len=" << sanitized.text.size()
                 << "persistRaw=" << decision.persistRaw
                 << "persistChat=" << decision.persistChat
                 << "reply=" << decision.allowReply;
        if (debugFullLogsEnabled()) {
            qDebug() << "[QQ][debug] sanitized:" << sanitized.text;
        }

        if (!decision.allowReply) {
            qDebug() << "[QQ]记录但不回复";
            return;
        }
        m_sessionRouter->submitInput(message.channel, message.sessionId,
                                     message.speaker, sanitized.text, message.senderId,
                                     decision.persistChat, decision.persistRaw,
                                     sanitized.redacted, message.occurredAt, message.replyToMessageId,
                                     message.messageId, message.imageSegments,message.imageSources);
    });
    if (ConfigManager::instance().getQQConfig().enabled) {
        m_qqClient->start();
    }

    m_anchorManager = new AnchorManager(m_character, this);
    //ui绑定
    m_anchorManager->registerWidget(m_bubble, AnchorConfig{AnchorPosition::HeadRight});

    m_anchorManager->registerWidget(m_chatWidget, AnchorConfig{AnchorPosition::WaistCenter, QPoint(0, 20)});
    //业务绑定
    m_llmService->registerStateProvider([this](){return m_appearance->getCurrentStateDescription();});

    initConnections();
    // 主动投递成功后把"茉子主动说过的话"写进历史/raw_events，供下次对话回认与检索。
    const auto recordProactive = [this](const QString &channel, const QString &session,
                                        const QString &text, const QString &kind, const QString &ref) {
        if (text.trimmed().isEmpty()) return;
        if (m_memoryManager->recordProactiveMessage(channel, session, text, kind, ref) <= 0)
            qWarning() << "[Proactive] history write failed:" << kind << ref;
    };
    // 联系投递：claim 自己保证"proposed 首投 + rejected 最多补一次"，这里只负责发送与回执。
    // 切片 B 起改为 committer 提交 + OutboxDispatcher 唯一发送：三条主动外发不再有直接发送路径。
    m_actionCommitter = new ActionCommitter(m_memoryManager);
    m_actionOutbox = new ActionOutboxDispatcher(m_memoryManager, this);
    // sender 是唯一的渠道出口：桌宠直接显示气泡；QQ 私聊只在已连接且是主人时发送。
    // 不确定/被拒的回执由 dispatcher 落库（at-most-once），这里只回答"提交是否进入渠道"。
    // R12-2c-2：这里也是"真正入渠道队列"的最后一个可取消边界——dispatcher 已在 sender 前
    // 复验过一次，但领取到这里的窗口里主人仍可能插话/关闭权限/撤回成果，因此入队前再复验。
    m_actionOutbox->setSender([this](const Utterance &u) {
        const auto guard = m_memoryManager->validateOutboxActionBeforeSend(u.deliveryEcho);
        if (!guard.value(QStringLiteral("ok")).toBool()) {
            qInfo() << "[Outbox] channel_boundary_blocked" << u.deliveryEcho
                    << guard.value(QStringLiteral("reason")).toString()
                    << "permanent=" << guard.value(QStringLiteral("permanent")).toBool();
            return false;
        }
        if (u.channel == Channel::kPet && u.sessionId == Channel::kDefaultSession)
            return m_petChannel->sendUtterance(u);
        if (u.channel == Channel::kQQPrivate
            && ConfigManager::instance().isMasterQQ(u.sessionId)
            && m_qqClient->isConnected())
            return m_qqChannel->sendUtterance(u);
        return false;
    });
    connect(m_actionOutbox, &ActionOutboxDispatcher::actionSettled, this,
            [this, recordProactive](const QString &actionKey,const QString &outcome,
                              const QJsonObject &payload) {
        qInfo() << "[Outbox] settled" << actionKey << outcome;
        // R12-2b：新来源（wake:/activity:）的回执统一写回 delivery_ledger + 领域行。
        // recordActionReceipt 幂等：重复回执不重复历史/版本；unknown/rejected/withheld 不写成功；
        // legacy reflection:/reminder:/thought: 回执仍由既有 settleOutboxAction 领域回写处理。
        if (m_memoryManager
            && (actionKey.startsWith(QStringLiteral("wake:"))
                || actionKey.startsWith(QStringLiteral("activity:")))) {
            m_memoryManager->recordActionReceipt(actionKey, outcome,
                                                 payload.value(QStringLiteral("detail")).toString());
        }
        // "她说过"只能回写至少已经交给渠道的结果：rejected/withheld 明确未发送，绝不写；
        // unknown 只是"可能已送达但无法确认"，也不能写成她说过（执行层仍冻结不重发）。
        // pet 没有协议层回执，handed_off 即气泡已显示；QQ 以 protocol_accepted 为准。
        const bool delivered = ActionOutboxDispatcher::proactiveHistoryEligible(
            outcome, payload.value(QStringLiteral("channel")).toString());
        if (!delivered) return;
        const QString text = payload.value(QStringLiteral("text")).toString();
        const QString channel = payload.value(QStringLiteral("channel")).toString();
        const QString session = payload.value(QStringLiteral("session")).toString();
        if (text.trimmed().isEmpty() || channel.isEmpty()) return;
        // kind/ref 与旧主动历史约定保持一致，便于既有回认/检索继续命中。
        if (actionKey.startsWith(QStringLiteral("reflection:"))) {
            recordProactive(channel, session, text, QStringLiteral("contact"),
                            actionKey.section(QLatin1Char(':'), 0, 2));
        } else if (actionKey.startsWith(QStringLiteral("wake:"))) {
            // R12-2b：OpenWake contact 的主动历史用 wake/<id>:<version> 作为回认引用。
            recordProactive(channel, session, text, QStringLiteral("contact"),
                            actionKey.section(QLatin1Char(':'), 0, 2));
        } else if (actionKey.startsWith(QStringLiteral("reminder:"))) {
            recordProactive(channel, session, text, QStringLiteral("reminder"),
                            actionKey.section(QLatin1Char(':'), 1, 1));
        } else if (actionKey.startsWith(QStringLiteral("thought:"))) {
            const auto parts = actionKey.split(QLatin1Char(':'));
            if (parts.size() >= 3)
                recordProactive(channel, session, text, QStringLiteral("share"),
                                QStringLiteral("%1:%2").arg(parts[1]).arg(parts[2]));
        } else if (actionKey.startsWith(QStringLiteral("activity:"))) {
            // R12-2b：internal 活动分享的主动历史引用 activity/<id>:<artifact_version>；
            // 只写"她说过"的历史行，不改变活动完成状态（result_json 由 recordActionReceipt 写）。
            recordProactive(channel, session, text, QStringLiteral("share"),
                            actionKey.section(QLatin1Char(':'), 0, 2));
        }
    });
    // 普通回复的有限补发：只补"已经决定要发、且被渠道明确拒绝"的话，
    // 因此不受主动联系的安静时段/空闲门控限制（不调用 canContactNow），只要求 QQ 连接可用。
    // 每次最多补 1 条；发送前先 recordDeliveryAttempt 占位（state 回到 claimed），
    // 保证同一行不会被相邻两个 5s 周期同时领取。unknown 表示协议层可能已送达，不重发。
    const auto retryReplyDeliveries = [this] {
        if (!m_qqClient->isConnected()) return;
        const auto retries = m_memoryManager->retryableDeliveries(QStringLiteral("reply"), 2, 1);
        for (const auto &value : retries) {
            const auto job = value.toObject();
            const auto ref = job["ref"].toString();
            if (ref.isEmpty()) continue;
            Utterance retry;
            retry.channel = job["channel"].toString();
            retry.sessionId = job["session"].toString();
            retry.text = job["text"].toString();
            retry.deliveryEcho = ref;
            if (retry.channel.isEmpty() || retry.text.trimmed().isEmpty()) {
                // 账本缺路由/原文（正常流程不会出现）：落"expired"终态（非 rejected，不会再次
                // 入选补发清单），避免每 5s 空转。
                m_memoryManager->recordDeliveryOutcome(QStringLiteral("reply"), ref,
                                                       QStringLiteral("expired"),
                                                       QStringLiteral("retry_incomplete"));
                continue;
            }
            if (!m_memoryManager->recordDeliveryAttempt(QStringLiteral("reply"), ref)) continue;
            if (!m_qqChannel->sendUtterance(retry)) {
                m_memoryManager->recordDeliveryOutcome(QStringLiteral("reply"), ref,
                                                       QStringLiteral("rejected"),
                                                       QStringLiteral("channel_unavailable"));
                qWarning() << "[Reply] retry submission failed" << ref;
                continue;
            }
            qInfo() << "[Reply] retry submitted" << ref;
        }
    };
    auto *reminders=new QTimer(this);
    connect(reminders,&QTimer::timeout,this,[this, retryReplyDeliveries] {
        // 普通回复补发与主动联系解耦：只要 QQ 可用就补，不受提醒开关/安静时段影响，
        // 因此先于下面的主动联系门控执行（不能放在早退之后）。
        retryReplyDeliveries();
        const auto options=ConfigManager::instance().getProactiveConfig();
        const auto now=QTime::currentTime(), start=QTime::fromString(options.quietStart,"HH:mm"), end=QTime::fromString(options.quietEnd,"HH:mm");
        if (!options.enabled || (start<end ? now>=start && now<end : start>end && (now>=start || now<end))) return;
        // 切片 B：到期提醒只做"提交"，发送由 OutboxDispatcher 唯一执行。
        for (const auto &target : options.targets) {
            const bool pet=target.first==Channel::kPet;
            if (!pet && !m_qqClient->isConnected()) continue;
            RequestMeta viewer;
            viewer.channel=target.first;
            viewer.sessionId=target.second;
            viewer.subject=QStringLiteral("user");
            viewer.longTermMemoryEnabled=true;
            const auto pending=m_memoryManager->pendingReminders(viewer);
            const auto nowTime=QDateTime::currentDateTime();
            for (const auto &value : pending) {
                const auto job=value.toObject();
                const auto id=job.value(QStringLiteral("id")).toInteger();
                if (id<=0) continue;
                // 先按到期时间过滤：未到期的提醒不应每 5 秒提交一次再被领域层拒绝，
                // 否则会刷出大量 "reminder rejected not_due" 日志（纯噪声，非错误）。
                const auto dueAt=QDateTime::fromString(job.value(QStringLiteral("at")).toString(),
                                                       Qt::ISODate);
                if (!dueAt.isValid()) {
                    qDebug()<<"[Reminder] skip malformed due"<<id
                            <<job.value(QStringLiteral("at")).toString();
                    continue;
                }
                if (dueAt>nowTime) continue;
                // commit 自己再做一次"pending/到期/未过期"的领域校验；未到期/已过期不写任何行。
                const auto committed=m_actionCommitter->commitReminderDelivery(id);
                if (committed.contains(QStringLiteral("error"))) 
                    qDebug()<<"[Reminder] not deliverable now"<<id
                            <<committed.value(QStringLiteral("error")).toString();
                if (committed.contains(QStringLiteral("error"))) continue;
                qInfo()<<"[Reminder] dispatch"<<id
                       <<committed.value(QStringLiteral("action_key")).toString();
            }
        }
    });
    reminders->start(5000);
    // 重连成功立即补一次，避免最长等 5s；5s 周期仍会在下一拍轮到。
    connect(m_qqClient, &QQProtocolClient::connected, this, [retryReplyDeliveries] {
        retryReplyDeliveries();
    });
    connect(m_coordinator, &CognitiveCoordinator::contactProposed, this, [this](qlonglong turn,int version) {
        // 切片 B：不再直接发送。可达目标不存在或提交校验失败时把意图落 withheld（不写动作行），
        // 成功则等待 OutboxDispatcher 领取发送。
        if (!canContactNow() || !hasReachableProactiveTarget()) {
            m_memoryManager->recordContactOutcome(turn,version,QStringLiteral("withheld"));
            qInfo() << "[ReflectionContact] withheld" << turn << version;
            return;
        }
        const auto committed=m_actionCommitter->commitReflectionContact(turn,version);
        if (committed.contains(QStringLiteral("error"))) {
            m_memoryManager->recordContactOutcome(turn,version,QStringLiteral("withheld"));
            qInfo() << "[ReflectionContact] withheld" << turn << version
                    << committed.value(QStringLiteral("error")).toString();
            return;
        }
        qInfo() << "[ReflectionContact] committed" << turn << version
                << committed.value(QStringLiteral("action_key")).toString();
    });
    connect(m_qqClient, &QQProtocolClient::sendResult, this, [this](const QString &echo,const QString &outcome) {
        if (echo.startsWith(QStringLiteral("reply:"))) {
            // 普通回复回执（ForegroundReply 保持现状）：只落终态。unknown 表示协议层可能已送达，
            // 补发逻辑不会选它；rejected 才进入有限补发（attempts<2）。
            if (!m_memoryManager->recordDeliveryOutcome(QStringLiteral("reply"), echo, outcome))
                qWarning() << "[Reply] receipt not recorded" << echo << outcome;
            else
                qInfo() << "[Reply] receipt" << echo << outcome;
            return;
        }
        // 主动外发（contact/reminder/share）回执：echo 现在就是 action_key，
        // 统一交给 OutboxDispatcher 在一个事务里更新 action/outbox/领域状态。
        if (m_actionOutbox)
            m_actionOutbox->onSendResult(echo,outcome);
    });
    connect(m_coordinator, &CognitiveCoordinator::draftReadyToShare, this,
            [this](const QString &activityRef, int version, const QString &draft) {
        if (!canContactNow() || !hasReachableProactiveTarget())
            return;
        const qlonglong activityId = activityRef.startsWith(QStringLiteral("thought/"))
            ? activityRef.mid(QStringLiteral("thought/").size()).toLongLong()
            : 0;
        if (activityId <= 0) return;
        // 切片 B：不再直接发送。canShareThought/artifact/版本/去重校验都在 commit 内完成；
        // 历史回写由 actionSettled（protocol_accepted，或 pet 的 handed_off）驱动，
        // unknown/rejected/withheld 不写，避免"没确认发出去就说她说过"。
        const auto committed=m_actionCommitter->commitActivityShare(activityId,version,draft);
        if (committed.contains(QStringLiteral("error")))
            qInfo() << "[Share] commit rejected" << activityId << version
                    << committed.value(QStringLiteral("error")).toString();
        else
            qInfo() << "[Share] committed" << committed.value(QStringLiteral("action_key")).toString();
    });

    m_timeManager->start();

    m_character->updatePath(m_appearance->getPath());

}

AppController::~AppController()
{
    // 切片 B：先停掉主动外发轮询并释放 committer，避免它们持有已析构的 MemoryManager。
    if (m_actionOutbox) {
        m_actionOutbox->stop();
        delete m_actionOutbox;
        m_actionOutbox = nullptr;
    }
    delete m_actionCommitter;
    m_actionCommitter = nullptr;
    if (m_qqClient)
        m_qqClient->stop();
    delete m_coordinator;

    //1. 优先析构 AnchorManager（因为它持有了 m_character 及各个 UI 窗口的指针）
    if (m_anchorManager) {
        delete m_anchorManager;
        m_anchorManager = nullptr;
    }

    // 2. 析构独立 UI 窗口组件
    if (m_character) {
        delete m_character;
        m_character = nullptr;
    }
    if (m_bubble) {
        delete m_bubble;
        m_bubble = nullptr;
    }
    if (m_chatWidget) {
        delete m_chatWidget;
        m_chatWidget = nullptr;
    }
    // if (m_settingsWidget) {
    //     delete m_settingsWidget;
    //     m_settingsWidget = nullptr;
    // }
    if(m_settingsWidget){
        delete m_settingsWidget;
        m_settingsWidget=nullptr;
    }
    // 3. 析构无 UI 的核心业务服务模块
    if (m_llmService) {
        delete m_llmService;
        m_llmService = nullptr;
    }
    if (m_appearance) {
        delete m_appearance;
        m_appearance = nullptr;
    }
    if (m_ttsService) {
        delete m_ttsService;
        m_ttsService = nullptr;
    }
    if (m_memoryManager) {
        delete m_memoryManager;
        m_memoryManager = nullptr;
    }

    qDebug()<<"[AppController]:所有中枢资源已安全清理完毕";
}

void AppController::startApp()
{
    // 切片 B：启动恢复先于轮询，且这里是唯一的启动恢复点。上一进程留在 dispatching 的
    // 主动外发无法确认是否已送达，按 at-most-once 冻结为 unknown（绝不自动重发）；
    // 随后启动 outbox 唯一发送循环（dispatcher.start 不再隐式恢复，避免覆盖 600s 阈值）。
    if (m_actionOutbox) {
        const int recovered = m_actionOutbox->recoverStale(600);
        if (recovered > 0)
            qInfo() << "[Outbox] stale actions frozen as unknown:" << recovered;
        // R12-2b：启动恢复的第二个显式入口。
        // 1) 回执对账：outbox 已 settle 但领域行可能没回写（进程在 emit 后崩溃），幂等补齐；
        // 2) drain verified 提案：逐条复核后入 Outbox（已 committed 的不重发）；
        // 3) 最后才启动唯一发送轮询。
        if (m_memoryManager) {
            const int reconciled = m_memoryManager->reconcileActionReceipts();
            if (reconciled > 0)
                qInfo() << "[Receipt] startup reconciled:" << reconciled;
            const auto drained = m_memoryManager->drainVerifiedActionProposals(8);
            if (drained.value(QStringLiteral("scanned")).toInt() > 0) {
                qInfo().noquote()
                    << QStringLiteral("[Proposal] startup drain committed=%1 withheld=%2"
                                      " failed=%3")
                           .arg(drained.value(QStringLiteral("committed")).toInt())
                           .arg(drained.value(QStringLiteral("withheld")).toInt())
                           .arg(drained.value(QStringLiteral("failed")).toInt());
            }
        }
        m_actionOutbox->start(1000);
    }
    m_character->show();

    qDebug()<<"[AppController]:我已启动！";

}

void AppController::handleMakoReply(const QList<SentenceText> &sentences, const QString &rawReply,
                                    const QString &userInput, const RequestMeta &meta)
{
    if (!m_coordinator->owns(meta.runId)) return;
    const auto response=QJsonDocument::fromJson(rawReply.toUtf8()).object();
    if (response.contains("reminder_plan")) {
        const bool saved=m_memoryManager->applyReminders(m_coordinator->turnId(),meta,response["reminder_plan"].toObject());
        qInfo()<<"[Reminder] plan"<<meta.runId<<"committed="<<saved;
        if (!saved) {
            SentenceText notice;
            notice.zhText=QStringLiteral("这次提醒安排没有保存成功。");
            handleMakoReply({notice},QStringLiteral("{}"),userInput,meta);
            return;
        }
    }
    // TD-034：本轮回复入队 TTS，记下它属于哪一轮（退火门控用）
    if (meta.channel == Channel::kPet) {
        m_lastPlaybackRound = m_coordinator->activeSequence();
        m_ttsService->enqueueSentences(sentences);
    } else {
        // QQ：文字回复走 QQ 渠道；情绪写入全局（D1 控制是否驱动桌宠立绘），
        // 不触发桌宠气泡/TTS（D2：播放与退火归桌宠渠道）。
        Utterance utterance;
        utterance.channel = meta.channel;
        utterance.sessionId = meta.sessionId;
        utterance.speaker = meta.speaker;
        // 投递回执键：优先本轮 chat_history.id；本轮没有持久化轮次（turnId=0）时退回
        // 事件 id，保证同一轮内每句都有稳定唯一的 (turn,index) 关联，重启后仍可按 ref 补发。
        const qlonglong replyTurn = m_coordinator->turnId();
        const QString turnKey = replyTurn > 0
            ? QString::number(replyTurn)
            : (meta.eventId.isEmpty() ? meta.runId : meta.eventId);
        int submitted = 0;
        for (int index = 0; index < sentences.size(); ++index) {
            const SentenceText &sentence = sentences[index];
            utterance.text = sentence.zhText;
            utterance.deliveryEcho = QStringLiteral("reply:%1:%2").arg(turnKey).arg(index);
            // 普通回复也建投递账本：先 claim 再提交，协议层回执按同一 ref 落终态，
            // 被明确拒绝时才允许 5s 调度补发一次。pet 渠道是本地气泡，不建账本。
            if (!m_memoryManager->recordDelivery(QStringLiteral("reply"), utterance.deliveryEcho,
                                                 meta.channel, meta.sessionId, sentence.zhText)) {
                qWarning() << "[Reply] delivery claim not recorded" << utterance.deliveryEcho;
            } else {
                // 首发送前也占一次尝试：attempts 语义="已提交的发送次数"，
                // 这样补发条件的 attempts<2 恰好等于"被明确拒绝后最多再补一次"。
                m_memoryManager->recordDeliveryAttempt(QStringLiteral("reply"), utterance.deliveryEcho);
            }
            if (!m_qqChannel->sendUtterance(utterance)) {
                // 提交失败=确定未进入协议队列：直接落拒绝，供补发调度处理。
                if (!m_memoryManager->recordDeliveryOutcome(QStringLiteral("reply"),
                                                            utterance.deliveryEcho,
                                                            QStringLiteral("rejected"),
                                                            QStringLiteral("channel_unavailable")))
                    qWarning() << "[Reply] rejected outcome not recorded" << utterance.deliveryEcho;
                qWarning() << "[QQChannel] sentence submission interrupted submitted=" << submitted
                           << "total=" << sentences.size();
                recordInterruption(submitted ? "channel_partial_submission" : "channel_unavailable");
                return;
            }
            ++submitted;
        }
        for (const auto &sentence : sentences) m_globalMood->applyTags(sentence.rawTags, meta.channel);
    }

    if (!m_coordinator->handedOff(userInput, rawReply, sentences, meta)) return;
    const qlonglong turnId=m_coordinator->turnId();
    if (!turnId || !meta.longTermMemoryEnabled) {
        return;
    }

    // 关注写回：本轮主人的话如果回答了此前某个关注，立即了结，避免下一次再追问。
    // 与回复解耦：写回失败不回滚已发出的回复，但必须留审计，便于区分"模型没判断"与"写回失败"。
    const auto evidence=response.value("evidence_decision").toObject();
    auto concernUpdates=evidence.value("concern_updates").toArray();
    if (concernUpdates.isEmpty()) concernUpdates=response.value("concern_updates").toArray();
    if (!concernUpdates.isEmpty() && m_memoryManager) {
        int committed=0, failed=0;
        for (const auto &value : concernUpdates) {
            const auto update=value.toObject();
            const auto ref=update["ref"].toString();
            const qlonglong head=ref.startsWith(QStringLiteral("reflection/"))
                ? ref.mid(QStringLiteral("reflection/").size()).toLongLong() : 0;
            if (head>0 && m_memoryManager->resolveConcern(head,turnId,update["kind"].toString(),
                                                          update["evidence_ref"].toString())) {
                ++committed;
                qInfo()<<"[Concern] resolved by="<<turnId<<"ref="<<ref<<"kind="<<update["kind"].toString();
                continue;
            }
            ++failed;
            qWarning()<<"[Concern] update_failed by="<<turnId<<"ref="<<ref
                      <<"kind="<<update["kind"].toString();
        }
        m_memoryManager->recordCognitiveAudit("concern_update",meta.runId,0,
            QJsonObject{{"event_id",meta.eventId},{"turn",turnId},
                        {"requested",concernUpdates.size()},{"committed",committed},
                        {"failed",failed},{"updates",concernUpdates}});
        qInfo()<<"[Concern] update run="<<meta.runId<<"committed="<<committed<<"failed="<<failed;
    }

    qlonglong lastEndId = -1;
    QList<qlonglong> sourceIds;
    QList<HistoryTurn> unsummarizedTurns = m_memoryManager->getUnsummarizedTurns(lastEndId, sourceIds,
                                                                                meta.channel, meta.sessionId, m_coordinator->turnId());


    // 长期记忆自动摘轮数（TD-037：由设置界面独立配置，默认与短期记忆同为 15）
    const int SUMMARY_THRESHOLD = ConfigManager::instance().getSummaryMemoryLength();
    if (unsummarizedTurns.size() >= SUMMARY_THRESHOLD) {
        qDebug()<<"[AppController]未摘要对话达到阈值，开始触发后台记忆提取...";
        // 按本轮主体取已有档案（群聊=qq:<uin>；桌宠=user），并附上茉子人设
        QList<CharacterProfile> existingProfiles = m_memoryManager->getCharacterProfiles(meta.subject);
        existingProfiles.append(m_memoryManager->getCharacterProfiles("mako"));
        QList<EpisodicMemory> existingMemories = m_memoryManager->getActiveEpisodicMemories(
            0.2, 20, meta.channel, QString(), meta.sessionId);
        m_llmService->extractMemoryAsync(unsummarizedTurns, lastEndId, sourceIds,
                                         existingProfiles, existingMemories, meta);
    }
}


bool AppController::canContactNow() const
{
    const auto cfg = ConfigManager::instance().getProactiveConfig();
    // 主动联系与后台写作解耦：只要求"没有在途对话/足够安静"，不看 privateWriting。
    if (!cfg.enabled || !m_coordinator->idleForAttention())
        return false;
    const auto now = QTime::currentTime();
    const auto start = QTime::fromString(cfg.quietStart, "HH:mm");
    const auto end = QTime::fromString(cfg.quietEnd, "HH:mm");
    return start < end ? (now < start || now >= end)
                       : (start == end || (now < start && now >= end));
}

bool AppController::hasReachableProactiveTarget() const
{
    // 只做"提交前"的粗门控：桌宠渠道恒可达；QQ 私聊要求已连接且是主人号。
    // 真正的发送结果仍以 OutboxDispatcher 的 sender 返回值/回执为准（escalate 到 settle）。
    const auto targets = ConfigManager::instance().getProactiveConfig().targets;
    for (const auto &target : targets) {
        if (target.first == Channel::kPet && target.second == Channel::kDefaultSession)
            return true;
        if (target.first == Channel::kQQPrivate
            && ConfigManager::instance().isMasterQQ(target.second)
            && m_qqClient->isConnected())
            return true;
    }
    return false;
}

void AppController::resumeMoodTimers()
{
    m_timeManager->notifyRoundEnded({{"emotion",m_globalMood->getEmotion()},
                                     {"distance",m_globalMood->getDistance()},
                                     {"blush",m_globalMood->getBlush()}});
}

void AppController::recordInterruption(const QString &reason)
{
    resumeMoodTimers();
    m_coordinator->interrupt(reason);
}

void AppController::handleSystemError(const QString &errorMsg)
{
    resumeMoodTimers();
    qDebug() << "[AppController 拦截到系统错误]：" << errorMsg;
    m_bubble->showMessage(QStringLiteral("网络请求失败，本轮处理已中断。"));

}

void AppController::handleAbnormalReply(const QString &reason)
{
    resumeMoodTimers();
    qDebug() << "[AppController 拦截到异常回复]：" << reason;
    m_bubble->showMessage(QStringLiteral("本轮处理已中断（%1）。").arg(reason));

}

void AppController::onPlayAudioAction(const QString &zhText, const QMap<QString, QString> &tags)
{
    // 渠道私有表现：气泡跟读（PetChannelAdapter）
    Utterance u;
    u.text = zhText;
    u.channel = Channel::kPet;
    m_petChannel->sendUtterance(u);

    // 全局情绪层：立绘四维状态不随渠道拆分（D1 开关 Phase 1 在 QQ 渠道路径生效）
    applyPetTags(tags);

    qDebug() << "[AppController] 当前播放文本长度:" << zhText.size();
    qDebug()<<"[AppController] 当前立绘路径:" << m_appearance->getPath();

}

void AppController::applyTimeSettings()
{
    ConfigManager &cfg = ConfigManager::instance();
    m_timeManager->setNightStart(cfg.getNightStartTime());
    m_timeManager->setNightEnd(cfg.getNightEndTime());
    m_timeManager->setBlushTime(cfg.getBlushLasting());
    m_timeManager->setBackIdleTime(cfg.getEmotionLasting());
    m_timeManager->setDistanceTime(cfg.getDistanceLasting());
}

void AppController::applyPetTags(const QMap<QString, QString> &tags)
{
    // 桌宠渠道：立绘（含服装）+ 全局情绪同步
    m_appearance->applyTags(tags);
    m_globalMood->applyTags(tags, Channel::kPet);
}

void AppController::initConnections()
{
    connect(m_ttsService, &TTSService::playAudioAction, this, &AppController::onPlayAudioAction);
    connect(m_appearance, &AppearanceManager::characterPathChanged, m_character, &CharacterWidget::updatePath);
    // D1：外部渠道情绪驱动桌宠立绘（只带情绪三维；服装仍由时间/桌宠控制）
    connect(m_globalMood, &GlobalMoodState::externalMoodApplied, this,
            [this](const QMap<QString, QString> &moodTags) {
                m_appearance->applyTags(moodTags);
            });
    connect(m_llmService, &LLMService::internetErrorSignal, this, &AppController::handleSystemError);
    connect(m_llmService, &LLMService::replyAbnormal, this, &AppController::handleAbnormalReply);

    connect(m_character, &CharacterWidget::chatRequested, m_chatWidget,&ChatWidget::popup);

    connect(m_appearance, &AppearanceManager::characterPathChanged,m_anchorManager, &AnchorManager::updateAllAnchors);
    connect(m_bubble, &BubbleWidget::bubbleShown,m_anchorManager, &AnchorManager::updateAllAnchors);

    // connect(m_character, &CharacterWidget::settingsRequested, m_settingsWidget, &SettingsWidget::show);
    // connect(m_settingsWidget, &SettingsWidget::settingsSaved, this, [this](){
    //     m_llmService->setApiKey(ConfigManager::instance().getApiKey());});
    connect(m_character, &CharacterWidget::settingsRequested, m_settingsWidget, &SettingsWidget::show);
    connect(m_settingsWidget, &SettingsWidget::settingsSaved, this, [this](){
        auto &config = ConfigManager::instance();
        QString logError;
        if (!RuntimeLog::configure(config.logDirectory(),config.separateLogRuns(),&logError))
            qWarning().noquote() << "[RuntimeLog] could not switch directory:" << logError;
        m_llmService->setApiKey(config.getApiKey());
        m_globalMood->setFollowExternalMood(config.getFollowExternalMood());
        m_memoryManager->updateMasterAccounts(config.getMasterQQAccounts());
        m_qqClient->setConfig(config.getQQConfig());
        m_qqClient->start();
        m_membershipService->start();
        m_memoryIndex->reconfigure(config.getMemoryPolicyConfig().retrieval);
        m_ttsService->reloadProvider();
        // TD-037：时间/退火配置热生效（start() 会立即重估昼夜状态）
        applyTimeSettings();
        m_timeManager->start();
    });

    connect(m_llmService,&LLMService::sentencesReady,this,&AppController::handleMakoReply);
    connect(m_llmService,&LLMService::replyQuiet,this,&AppController::resumeMoodTimers);
    // 输入路由：渠道 → SessionRouter（身份解析+历史组装）→ 大脑
    connect(m_sessionRouter, &SessionRouter::requestReply, this,
            [this](const QString &userText, const QList<HistoryTurn> &history, const RequestMeta &meta) {
        Q_UNUSED(history);
        m_timeManager->notifyUserInputStarted();
        m_coordinator->submit(userText, meta);
    });
    connect(m_chatWidget,&ChatWidget::textSubmitted,this,[this](const QString &text,const QString &imageFile){
        const MemoryPolicyConfig memoryPolicy =
            ConfigManager::instance().getMemoryPolicyConfig();
        const SanitizeResult sanitized = SensitiveContentFilter::sanitize(
            text, QString(), memoryPolicy.privacyRules);
        if (sanitized.blocked) {
            qDebug() << "[Pet]消息触发隐私拦截，已丢弃（不回复/不入上下文）";
            return;
        }
        m_sessionRouter->submitInput(Channel::kPet, Channel::kDefaultSession,
                                     QStringLiteral("欧尼酱"), sanitized.text, QString(),
                                     memoryPolicy.shortContextEnabled,
                                     memoryPolicy.longTermMemoryEnabled,
                                     sanitized.redacted, {}, {}, {},
                                     imageFile.isEmpty() ? QJsonArray{} : QJsonArray{0},
                                     imageFile.isEmpty() ? QMap<int,QString>{} : QMap<int,QString>{{0,imageFile}});
    });
    connect(m_llmService, &LLMService::memoryExtractionReady, this,
            [this](const QJsonObject &extractionResult, qlonglong lastEndId,
                   const QString &sourceIdsJson, const RequestMeta &meta) {
        // v4：提取结果按“本轮 meta”归属，并记录 episodic 的渠道/会话/主体/可见性
        const QString visibility = (meta.channel == Channel::kPet)
                                       ? QStringLiteral("private")
                                       : QStringLiteral("channel");

        // 1. 角色档案更新（subject 由模型给出：user/mako/qq:<uin>/group:<群号>）
        QJsonArray charUpdates = extractionResult["character_updates"].toArray();
        for (const QJsonValue &val : charUpdates) {
            QJsonObject obj = val.toObject();
            QString subject = obj["subject"].toString(meta.subject);
            QString key = obj["key"].toString();
            QString value = obj["value"].toString();
            if (!isAllowedExtractionSubject(subject, meta)) {
                qDebug() << "[AppController]拒绝越权 profile subject:" << subject;
                continue;
            }
            if (!subject.isEmpty() && !key.isEmpty() && !value.isEmpty()) {
                m_memoryManager->upsertCharacterProfile(subject, key, value);
            }
        }

        // 2. 情景记忆写入（带 origin/session/subject/visibility）
        QJsonArray episodicMemories = extractionResult["episodic_memories"].toArray();
        for (const QJsonValue &val : episodicMemories) {
            QJsonObject obj = val.toObject();
            QString content = obj["content"].toString();
            QString type = obj["type"].toString("event");
            double importance = obj["importance"].toDouble(0.5);
            QString subject = obj["subject"].toString(meta.subject);
            QString eventTimeStr = obj["event_time"].toString();
            QDateTime eventTime = QDateTime::fromString(eventTimeStr, "yyyy-MM-dd HH:mm:ss");
            if (!isAllowedExtractionSubject(subject, meta)) {
                qDebug() << "[AppController]拒绝越权 episodic subject:" << subject;
                continue;
            }
            if (!content.isEmpty()) {
                m_memoryManager->addEpisodicMemory(content, type, importance, eventTime,
                                                   sourceIdsJson, meta.channel, visibility,
                                                   subject, meta.sessionId);
            }
        }

        // 3. 工作摘要 → 按本轮渠道+会话分桶
        QString summary = extractionResult["working_summary"].toString();
        if (!summary.isEmpty()) {
            m_memoryManager->addLongTermSummary(summary, lastEndId, sourceIdsJson,
                                                meta.channel, meta.sessionId);
        }

        // 4. 关系状态更新（主体=本轮说话人）
        QJsonArray relUpdates = extractionResult["relationship_updates"].toArray();
        for (const QJsonValue &val : relUpdates) {
            QJsonObject obj = val.toObject();
            QString dimension = obj["dimension"].toString();
            double delta = obj["delta"].toDouble(0.0);
            if (!dimension.isEmpty() && delta != 0.0) {
                m_memoryManager->upsertRelationshipState(dimension, delta, meta.subject);
            }
        }

        qDebug()<<"[AppController]本次记忆提取流程已全部完成: 档案更新"<<charUpdates.size()
                <<"条, 情景记忆"<<episodicMemories.size()<<"条, 摘要长度:"<<summary.size()
                <<", 关系更新"<<relUpdates.size()<<"条";
    });


    connect(m_timeManager, &TimeManager::clotheChanged,m_appearance, [this](const QString &clothing) {
                QMap<QString, QString> tags;
                tags["clothing"] = clothing;
                applyPetTags(tags);
            });

    connect(m_timeManager, &TimeManager::blushingReset,m_appearance, [this]() {
                QMap<QString, QString> tags;
                tags["blush"] = "unblushing";
                applyPetTags(tags);
            });

    connect(m_timeManager, &TimeManager::emotionReset,m_appearance, [this]() {
                QMap<QString, QString> tags;
                tags["emotion"] = "happyIdle";
                applyPetTags(tags);
            });

    connect(m_timeManager, &TimeManager::distanceReset,m_appearance, [this]() {
                QMap<QString, QString> tags;
                tags["distance"] = "far";
                applyPetTags(tags);
            });

    // 每轮对话结束：以最终视觉状态驱动退火（脸红/非默认表情/closer 各自开定时器）
    connect(m_ttsService, &TTSService::playbackQueueEmpty, m_timeManager, [this]() {
                // TD-034：退火只对最新一轮生效——上一轮播放收尾时若已经开启新一轮
                //（用户已输入但新回复尚未入队 TTS），这次收尾不属于当前轮，跳过退火；
                // 等新一轮播放结束（m_lastPlaybackRound == m_coordinator->lastSequence()）再按最终状态处理。
                if (m_lastPlaybackRound != m_coordinator->lastSequence()) {
                    qDebug() << "[AppController]忽略旧轮收尾的退火触发: 播放轮次"
                             << m_lastPlaybackRound << "当前轮次" << m_coordinator->lastSequence();
                    return;
                }
                QMap<QString, QString> finalTags;
                finalTags["blush"]    = m_appearance->getBlush();
                finalTags["emotion"]  = m_appearance->getEmotion();
                finalTags["distance"] = m_appearance->getDistance();
                m_timeManager->notifyRoundEnded(finalTags);
            });
}
