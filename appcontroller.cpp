#include "appcontroller.h"
#include "characterwidget.h"
#include "bubblewidget.h"
#include "llmservice.h"
#include "appearancemanager.h"
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

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonDocument>

AppController::AppController(QObject *parent)
    : QObject{parent}
{
    m_character = new CharacterWidget;
    m_bubble = new BubbleWidget;
    m_llmService = new LLMService(ConfigManager::instance().getApiKey());
    m_appearance = new AppearanceManager;
    m_ttsService = new TTSService;
    m_chatWidget = new ChatWidget;
    m_memoryManager = new MemoryManager(ConfigManager::instance().getMemoryPath());
    // m_settingsWidget = new SettingsWidget;
    // m_settingsWidget->setMemoryManager(m_memoryManager);
    // m_settingsWidget->setMemoryLength(ConfigManager::instance().getShortMemoryLength());
    m_settingsWidget =new SettingsWidget;
    m_settingsWidget->setMemoryManager(m_memoryManager);
    m_settingsWidget->setMemoryLength(ConfigManager::instance().getShortMemoryLength());

    m_memoryManager->decayEpisodicMemory();        // 启动时执行一次情景记忆衰减
    m_llmService->setMemoryManager(m_memoryManager);

    m_timeManager =new TimeManager(this);
    applyTimeSettings();   // TD-037：启动时应用用户配置（夜晚时段/退火时长）

    // 渠道装配（Phase 0：桌宠渠道接管气泡私有表现；QQ 为 Phase 1 占位）
    m_petChannel = new PetChannelAdapter(m_bubble, this);
    m_qqChannel = new QQChannelAdapter(this);
    m_sessionRouter = new SessionRouter(m_memoryManager, this);

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
    m_memoryManager->saveQATurn(m_lastUserInput,rawReply,sentences,m_lastMeta);
    m_ttsService->enqueueSentences(sentences);

    qlonglong lastEndId = -1;
    QList<qlonglong> sourceIds;
    QList<HistoryTurn> unsummarizedTurns = m_memoryManager->getUnsummarizedTurns(lastEndId, sourceIds,
                                                                                m_lastMeta.channel, m_lastMeta.sessionId);


    // 长期记忆自动摘轮数（TD-037：由设置界面独立配置，默认与短期记忆同为 15）
    const int SUMMARY_THRESHOLD = ConfigManager::instance().getSummaryMemoryLength();
    if (unsummarizedTurns.size() >= SUMMARY_THRESHOLD) {
        qDebug()<<"[AppController]未摘要对话达到阈值，开始触发后台记忆提取...";
        QList<CharacterProfile> existingProfiles = m_memoryManager->getCharacterProfiles("user");
        QList<EpisodicMemory> existingMemories = m_memoryManager->getActiveEpisodicMemories(0.2, 20);
        m_llmService->extractMemoryAsync(unsummarizedTurns, lastEndId, sourceIds, existingProfiles, existingMemories);
    }
}


void AppController::handleSystemError(const QString &errorMsg)
{
    qDebug() << "[AppController 拦截到系统错误]：" << errorMsg;
    m_bubble->showMessage("啊哦，欧尼酱，网络好像断开了呢，茉子联系不到服务器啦...");
    QMap<QString, QString> errorTags;
    errorTags.insert("emotion", "sad");
    errorTags.insert("blush", "unblushing");
    m_appearance->applyTags(errorTags);
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
    m_appearance->applyTags(errorTags);

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
    m_appearance->applyTags(tags);

    qDebug()<<"[AppController] 当前播放文本:" << zhText;
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

void AppController::initConnections()
{
    connect(m_ttsService, &TTSService::playAudioAction, this, &AppController::onPlayAudioAction);
    connect(m_appearance, &AppearanceManager::characterPathChanged, m_character, &CharacterWidget::updatePath);
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
        m_sessionRouter->submitInput(Channel::kPet, Channel::kDefaultSession, QStringLiteral("欧尼酱"), text);
    });
    connect(m_llmService, &LLMService::memoryExtractionReady, this, [this](const QJsonObject &extractionResult, qlonglong lastEndId, const QString &sourceIdsJson) {
        // 1. 角色档案更新
        QJsonArray charUpdates = extractionResult["character_updates"].toArray();
        for (const QJsonValue &val : charUpdates) {
            QJsonObject obj = val.toObject();
            QString subject = obj["subject"].toString();
            QString key = obj["key"].toString();
            QString value = obj["value"].toString();
            if (!subject.isEmpty() && !key.isEmpty() && !value.isEmpty()) {
                m_memoryManager->upsertCharacterProfile(subject, key, value);
            }
        }

        // 2. 情景记忆写入
        QJsonArray episodicMemories = extractionResult["episodic_memories"].toArray();
        for (const QJsonValue &val : episodicMemories) {
            QJsonObject obj = val.toObject();
            QString content = obj["content"].toString();
            QString type = obj["type"].toString("event");
            double importance = obj["importance"].toDouble(0.5);
            QString eventTimeStr = obj["event_time"].toString();
            QDateTime eventTime = QDateTime::fromString(eventTimeStr, "yyyy-MM-dd HH:mm:ss");
            if (!content.isEmpty()) {
                m_memoryManager->addEpisodicMemory(content, type, importance, eventTime, sourceIdsJson);
            }
        }

        // 3. 工作摘要 → 写入长期记忆摘要表（保留兼容，按渠道分桶写入）
        QString summary = extractionResult["working_summary"].toString();
        if (!summary.isEmpty()) {
            m_memoryManager->addLongTermSummary(summary, lastEndId, sourceIdsJson,
                                                m_lastMeta.channel, m_lastMeta.sessionId);
        }

        // 4. 关系状态更新（subject 隔离：'user' 即主人）
        QJsonArray relUpdates = extractionResult["relationship_updates"].toArray();
        for (const QJsonValue &val : relUpdates) {
            QJsonObject obj = val.toObject();
            QString dimension = obj["dimension"].toString();
            double delta = obj["delta"].toDouble(0.0);
            if (!dimension.isEmpty() && delta != 0.0) {
                m_memoryManager->upsertRelationshipState(dimension, delta, m_lastMeta.subject);
            }
        }

        qDebug()<<"[AppController]本次记忆提取流程已全部完成: 档案更新"<<charUpdates.size()
                <<"条, 情景记忆"<<episodicMemories.size()<<"条, 摘要:"<<summary
                <<", 关系更新"<<relUpdates.size()<<"条";
    });

    connect(m_settingsWidget, &SettingsWidget::ttsModelSwitchRequested,
            this, [this](const QString &gptPath, const QString &sovitsPath) {
                m_ttsService->switchModel(gptPath, sovitsPath);
            });

    connect(m_timeManager, &TimeManager::clotheChanged,m_appearance, [this](const QString &clothing) {
                QMap<QString, QString> tags;
                tags["clothing"] = clothing;
                m_appearance->applyTags(tags);
            });

    connect(m_timeManager, &TimeManager::blushingReset,m_appearance, [this]() {
                QMap<QString, QString> tags;
                tags["blush"] = "unblushing";
                m_appearance->applyTags(tags);
            });

    connect(m_timeManager, &TimeManager::emotionReset,m_appearance, [this]() {
                QMap<QString, QString> tags;
                tags["emotion"] = "happyIdle";
                m_appearance->applyTags(tags);
            });

    connect(m_timeManager, &TimeManager::distanceReset,m_appearance, [this]() {
                QMap<QString, QString> tags;
                tags["distance"] = "far";
                m_appearance->applyTags(tags);
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