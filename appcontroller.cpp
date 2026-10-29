#include "appcontroller.h"
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
    m_memoryManager->seedIdentity(ConfigManager::instance().getMasterQQAccounts());
    // m_settingsWidget = new SettingsWidget;
    // m_settingsWidget->setMemoryManager(m_memoryManager);
    // m_settingsWidget->setMemoryLength(ConfigManager::instance().getShortMemoryLength());
    m_settingsWidget =new SettingsWidget;
    m_settingsWidget->setMemoryManager(m_memoryManager);
    m_settingsWidget->setMemoryLength(ConfigManager::instance().getShortMemoryLength());

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
                                     sanitized.redacted);
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

    m_timeManager->start();

    m_character->updatePath(m_appearance->getPath());

}

AppController::~AppController()
{
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

    if (m_qqClient) {
        m_qqClient->stop();
    }

    qDebug()<<"[AppController]:所有中枢资源已安全清理完毕";
}

void AppController::startApp()
{
    m_character->show();

    qDebug()<<"[AppController]:我已启动！";

}

void AppController::handleMakoReply(const QList<SentenceText> &sentences, const QString &rawReply)
{
    // TD-034：本轮回复入队 TTS，记下它属于哪一轮（退火门控用）
    m_lastPlaybackRound = m_roundSeq;
    if (m_lastMeta.persistTurn) {
        m_memoryManager->saveQATurn(m_lastUserInput, rawReply, sentences, m_lastMeta);
    }
    if (m_lastMeta.channel == Channel::kPet) {
        m_ttsService->enqueueSentences(sentences);
    } else {
        // QQ：文字回复走 QQ 渠道；情绪写入全局（D1 控制是否驱动桌宠立绘），
        // 不触发桌宠气泡/TTS（D2：播放与退火归桌宠渠道）。
        QStringList replyParts;
        for (const SentenceText &sentence : sentences) {
            replyParts << sentence.zhText;
            m_globalMood->applyTags(sentence.rawTags, m_lastMeta.channel);
        }
        Utterance utterance;
        utterance.channel = m_lastMeta.channel;
        utterance.sessionId = m_lastMeta.sessionId;
        utterance.speaker = m_lastMeta.speaker;
        utterance.text = replyParts.join(QString());
        m_qqChannel->sendUtterance(utterance);
    }

    if (!m_lastMeta.persistTurn || !m_lastMeta.longTermMemoryEnabled) {
        return;
    }

    qlonglong lastEndId = -1;
    QList<qlonglong> sourceIds;
    QList<HistoryTurn> unsummarizedTurns = m_memoryManager->getUnsummarizedTurns(lastEndId, sourceIds,
                                                                                m_lastMeta.channel, m_lastMeta.sessionId);


    // 长期记忆自动摘轮数（TD-037：由设置界面独立配置，默认与短期记忆同为 15）
    const int SUMMARY_THRESHOLD = ConfigManager::instance().getSummaryMemoryLength();
    if (unsummarizedTurns.size() >= SUMMARY_THRESHOLD) {
        qDebug()<<"[AppController]未摘要对话达到阈值，开始触发后台记忆提取...";
        // 按本轮主体取已有档案（群聊=qq:<uin>；桌宠=user），并附上茉子人设
        QList<CharacterProfile> existingProfiles = m_memoryManager->getCharacterProfiles(m_lastMeta.subject);
        existingProfiles.append(m_memoryManager->getCharacterProfiles("mako"));
        QList<EpisodicMemory> existingMemories = m_memoryManager->getActiveEpisodicMemories(
            0.2, 20, m_lastMeta.channel, QString(), m_lastMeta.sessionId);
        m_llmService->extractMemoryAsync(unsummarizedTurns, lastEndId, sourceIds,
                                         existingProfiles, existingMemories, m_lastMeta);
    }
}


void AppController::handleSystemError(const QString &errorMsg)
{
    qDebug() << "[AppController 拦截到系统错误]：" << errorMsg;
    m_bubble->showMessage("啊哦，欧尼酱，网络好像断开了呢，茉子联系不到服务器啦...");
    QMap<QString, QString> errorTags;
    errorTags.insert("emotion", "sad");
    errorTags.insert("blush", "unblushing");
    applyPetTags(errorTags);
    //按最终状态启动退火：sad 表情 15 秒后自动恢复 happyIdle
    m_timeManager->notifyRoundEnded(errorTags);
}

void AppController::handleAbnormalReply(const QString &reason)
{
    qDebug() << "[AppController 拦截到异常回复]：" << reason;
    m_bubble->showMessage("啊哦……茉子的脑袋突然一片空白，刚才想到什么忘掉了啦！");
    QMap<QString, QString> errorTags;
    errorTags.insert("emotion", "sad");
    errorTags.insert("blush", "unblushing");
    applyPetTags(errorTags);

    //反馈后按最终状态启动退火：sad≠happyIdle → 15秒后自动恢复
    m_timeManager->notifyRoundEnded(errorTags);
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
        m_llmService->setApiKey(ConfigManager::instance().getApiKey());
        // TD-037：时间/退火配置热生效（start() 会立即重估昼夜状态）
        applyTimeSettings();
        m_timeManager->start();
    });

    connect(m_llmService,&LLMService::sentencesReady,this,&AppController::handleMakoReply);
    // 输入路由：渠道 → SessionRouter（身份解析+历史组装）→ 大脑
    connect(m_sessionRouter, &SessionRouter::requestReply, this,
            [this](const QString &userText, const QList<HistoryTurn> &history, const RequestMeta &meta) {
        m_lastUserInput = userText;
        ++m_roundSeq;   // TD-034：用户开口即开启新一轮
        m_lastMeta = meta;
        m_timeManager->notifyUserInputStarted();
        m_llmService->askDeepSeek(userText, history, meta);
    });
    connect(m_chatWidget,&ChatWidget::textSubmitted,this,[this](const QString &text){
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
                                     sanitized.redacted);
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

    connect(m_settingsWidget, &SettingsWidget::ttsModelSwitchRequested,
            this, [this](const QString &gptPath, const QString &sovitsPath) {
                m_ttsService->switchModel(gptPath, sovitsPath);
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
                // 等新一轮播放结束（m_lastPlaybackRound == m_roundSeq）再按最终状态处理。
                if (m_lastPlaybackRound != m_roundSeq) {
                    qDebug() << "[AppController]忽略旧轮收尾的退火触发: 播放轮次"
                             << m_lastPlaybackRound << "当前轮次" << m_roundSeq;
                    return;
                }
                QMap<QString, QString> finalTags;
                finalTags["blush"]    = m_appearance->getBlush();
                finalTags["emotion"]  = m_appearance->getEmotion();
                finalTags["distance"] = m_appearance->getDistance();
                m_timeManager->notifyRoundEnded(finalTags);
            });
}