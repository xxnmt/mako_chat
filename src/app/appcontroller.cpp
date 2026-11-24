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

#include <QDebug>
#include <QTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonDocument>

namespace {

// QQ 的发送结果异步回来时只有 deliveryEcho，这里暂存草稿分享的路由（渠道/会话/原文），
// 供 sendResult 落分享回执并补写"茉子主动说过的话"。键为 share:<activityId>:<version>。
// 只在主线程访问，条目在回执到达（或入队失败）时移除；重复分享同一版本时覆盖。
struct ShareRoute {
    QString channel;
    QString session;
    QString text;
};

QHash<QString, ShareRoute> &shareRoutes()
{
    static QHash<QString, ShareRoute> routes;
    return routes;
}

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
    // 返回本次投递的 outcome；空串表示没有可投递的目标（调用方据此判 withheld）。
    const auto dispatchContact = [this, recordProactive](qlonglong turn, int version) -> QString {
        auto &cfg = ConfigManager::instance();
        for (const auto &target : cfg.getProactiveConfig().targets) {
            const bool pet = target.first==Channel::kPet;
            if (!pet && !m_qqClient->isConnected()) continue;
            Utterance u;
            u.channel=target.first;
            u.sessionId=target.second;
            u.text=m_memoryManager->claimReflectionContact(turn,version,u.channel,u.sessionId);
            if (u.text.isEmpty()) continue;
            const auto ref=QStringLiteral("reflection:%1:%2").arg(turn).arg(version);
            u.deliveryEcho=ref;
            const bool submitted = pet ? m_petChannel->sendUtterance(u) : m_qqChannel->sendUtterance(u);
            const QString outcome = submitted ? QStringLiteral("handed_off") : QStringLiteral("rejected");
            m_memoryManager->recordContactOutcome(turn,version,outcome);
            if (submitted)
                recordProactive(u.channel,u.sessionId,u.text,QStringLiteral("contact"),ref);
            qInfo() << "[ReflectionContact]" << turn << version << u.channel << outcome;
            return outcome;
        }
        return {};
    };
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
    connect(reminders,&QTimer::timeout,this,[this, dispatchContact, retryReplyDeliveries] {
        // 普通回复补发与主动联系解耦：只要 QQ 可用就补，不受提醒开关/安静时段影响，
        // 因此先于下面的主动联系门控执行（不能放在早退之后）。
        retryReplyDeliveries();
        const auto options=ConfigManager::instance().getProactiveConfig();
        const auto now=QTime::currentTime(), start=QTime::fromString(options.quietStart,"HH:mm"), end=QTime::fromString(options.quietEnd,"HH:mm");
        if (!options.enabled || (start<end ? now>=start && now<end : start>end && (now>=start || now<end))) return;
        for (const auto &target : options.targets) {
            const bool pet=target.first==Channel::kPet;
            if (!pet && !m_qqClient->isConnected()) continue;
            const auto due=m_memoryManager->claimDueReminder(target.first,target.second);
            if (due.isEmpty()) continue;
            Utterance u;
            u.channel=target.first; u.sessionId=target.second; u.text=due["message"].toString();
            const auto id=due["id"].toInteger();
            u.deliveryEcho=QString("reminder:%1").arg(id);
            const bool ok=pet ? m_petChannel->sendUtterance(u) : m_qqChannel->sendUtterance(u);
            m_memoryManager->reminderOutcome(id,ok ? "handed_off" : "rejected");
            if (ok)
                m_memoryManager->recordProactiveMessage(u.channel,u.sessionId,u.text,
                                                        QStringLiteral("reminder"),QString::number(id));
            qInfo()<<"[Reminder] dispatch"<<id<<ok;
        }
        // 上一次被渠道拒绝的联系：每个周期最多补发一条；claim 保证不会重复发送。
        if (canContactNow()) {
            const auto retries = m_memoryManager->retryableReflectionContacts(4);
            for (const auto &value : retries) {
                const auto retry = value.toObject();
                if (!dispatchContact(retry["turn"].toInteger(), retry["version"].toInt()).isEmpty())
                    break;
            }
        }
    });
    reminders->start(5000);
    // 重连成功立即补一次，避免最长等 5s；5s 周期仍会在下一拍轮到。
    connect(m_qqClient, &QQProtocolClient::connected, this, [retryReplyDeliveries] {
        retryReplyDeliveries();
    });
    connect(m_coordinator, &CognitiveCoordinator::contactProposed, this, [this, dispatchContact](qlonglong turn,int version) {
        if (canContactNow() && !dispatchContact(turn,version).isEmpty())
            return;
        m_memoryManager->recordContactOutcome(turn,version,"withheld");
        qInfo() << "[ReflectionContact] withheld" << turn << version;
    });
    connect(m_qqClient, &QQProtocolClient::sendResult, this, [this](const QString &echo,const QString &outcome) {
        if (echo.startsWith("reminder:")) {
            m_memoryManager->reminderOutcome(echo.mid(9).toLongLong(),outcome);
            qInfo()<<"[Reminder] receipt"<<echo<<outcome;
            return;
        }
        if (echo.startsWith("reply:")) {
            // 普通回复回执：只落终态。unknown 表示协议层可能已送达，补发逻辑不会选它；
            // rejected 才进入有限补发（attempts<2）。
            if (!m_memoryManager->recordDeliveryOutcome(QStringLiteral("reply"), echo, outcome))
                qWarning() << "[Reply] receipt not recorded" << echo << outcome;
            else
                qInfo() << "[Reply] receipt" << echo << outcome;
            return;
        }
        if (echo.startsWith("share:")) {
            const auto parts=echo.split(':');
            if (parts.size()!=3) return;
            // 路由可能在进程重启后丢失：此时仍记录结果，只是不补历史原文/不重发。
            const auto route=shareRoutes().take(echo);
            const auto activityId=parts[1].toLongLong();
            const auto version=parts[2].toInt();
            if (!m_memoryManager->recordShareOutcome(activityId,version,outcome,route.channel,route.session)) {
                qWarning()<<"[Share] receipt rejected"<<echo<<outcome;
                return;
            }
            qInfo()<<"[Share] receipt"<<echo<<outcome;
            if (outcome=="handed_off" || outcome=="protocol_accepted") {
                if (!route.text.trimmed().isEmpty())
                    m_memoryManager->recordProactiveMessage(route.channel,route.session,route.text,
                                                            QStringLiteral("share"),
                                                            QStringLiteral("%1:%2").arg(activityId).arg(version));
                return;
            }
            // 渠道明确拒绝时把同一份草稿补发一次（canShareThought 用 attempts<2 挡住第二次拒绝）。
            // 延迟到当前回执处理栈结束之后再发，避免与协议客户端的在途队列冲突。
            if (outcome=="rejected" && !route.channel.isEmpty() && !route.text.trimmed().isEmpty()
                && m_memoryManager->canShareThought(activityId, version)) {
                QTimer::singleShot(4000, this, [this, activityId, version, route, echo] {
                    if (!m_memoryManager->canShareThought(activityId, version)) return;
                    Utterance retry;
                    retry.channel = route.channel;
                    retry.sessionId = route.session;
                    retry.text = route.text;
                    retry.deliveryEcho = echo;
                    shareRoutes().insert(echo, route);
                    const bool submitted = route.channel == Channel::kPet
                        ? m_petChannel->sendUtterance(retry) : m_qqChannel->sendUtterance(retry);
                    if (!submitted) {
                        shareRoutes().remove(echo);
                        m_memoryManager->recordShareOutcome(activityId, version,
                                                            QStringLiteral("rejected"),
                                                            route.channel, route.session);
                    }
                });
            }
            return;
        }
        if (!echo.startsWith("reflection:")) return;
        const auto parts=echo.split(':');
        if (parts.size()==3 && m_memoryManager->recordContactOutcome(parts[1].toLongLong(),parts[2].toInt(),outcome))
            qInfo() << "[ReflectionContact]" << echo << outcome;
    });
    connect(m_coordinator, &CognitiveCoordinator::draftReadyToShare, this, [this, recordProactive](const QString &activityRef, int version, const QString &draft) {
        if (!canContactNow())
            return;
        const qlonglong activityId = activityRef.startsWith(QStringLiteral("thought/"))
            ? activityRef.mid(QStringLiteral("thought/").size()).toLongLong()
            : 0;
        if (activityId <= 0 || !m_memoryManager->canShareThought(activityId, version))
            return;
        auto &cfg = ConfigManager::instance();
        const auto targets = cfg.getProactiveConfig().targets;
        const auto ref = QStringLiteral("%1:%2").arg(activityId).arg(version);
        for (const auto &target : targets) {
            Utterance u;
            u.channel = target.first;
            u.sessionId = target.second;
            u.text = draft;
            if (cfg.isBlindZone(u.channel,u.sessionId))
                continue;
            if (u.channel == Channel::kPet && u.sessionId == Channel::kDefaultSession) {
                const bool submitted = m_petChannel->sendUtterance(u);
                m_memoryManager->recordShareOutcome(activityId, version,
                                                    submitted ? QStringLiteral("handed_off")
                                                              : QStringLiteral("rejected"),
                                                    u.channel, u.sessionId);
                if (submitted)
                    recordProactive(u.channel,u.sessionId,draft,QStringLiteral("share"),ref);
                break;
            }
            if (u.channel == Channel::kQQPrivate && cfg.isMasterQQ(u.sessionId)
                && m_qqClient->isConnected()) {
                // QQ 是"已入队"而非送达：回执在 sendResult 里按 deliveryEcho 回来。
                u.deliveryEcho = QStringLiteral("share:%1").arg(ref);
                shareRoutes().insert(u.deliveryEcho, ShareRoute{u.channel,u.sessionId,draft});
                if (!m_qqChannel->sendUtterance(u)) {
                    shareRoutes().remove(u.deliveryEcho);
                    m_memoryManager->recordShareOutcome(activityId, version,
                                                        QStringLiteral("rejected"), u.channel, u.sessionId);
                }
                break;
            }
        }
    });

    m_timeManager->start();

    m_character->updatePath(m_appearance->getPath());

}

AppController::~AppController()
{
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
