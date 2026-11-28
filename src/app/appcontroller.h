#ifndef APPCONTROLLER_H
#define APPCONTROLLER_H

#include <QObject>
#include <QQueue>
#include <QMap>
#include <QJsonObject>
#include "sentencedata.h"
#include "utterance.h"

class CognitiveCoordinator;
class CharacterWidget;
class BubbleWidget;
class LLMService;
class AppearanceManager;
class GlobalMoodState;
class GlobalMoodState;
class TTSService;
class ChatWidget;
class AnchorManager;
// class SettingsWidget;
class SettingsWidget;
class MemoryManager;
class TimeManager;
class PetChannelAdapter;
class QQChannelAdapter;
class SessionRouter;
class QQProtocolClient;
class RawEventProcessor;
class GroupMembershipService;
class ActionCommitter;
class ActionOutboxDispatcher;

class AppController : public QObject
{
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController();

    void startApp();

private slots:
    void handleMakoReply(const QList<SentenceText> &sentences, const QString &rawReply,
                         const QString &userInput, const RequestMeta &meta);
    void handleSystemError(const QString &errorMsg);
    void handleAbnormalReply(const QString &reason);
    void onPlayAudioAction(const QString &zhText, const QMap<QString, QString> &tags);
signals:

private:
    class MemoryIndexWorker *m_memoryIndex = nullptr;
    void initConnections();
    void applyTimeSettings();   // 把 ConfigManager 的时间/退火配置应用到 TimeManager
    // B5：设置保存按有效配置差异只更新真正受影响的模块；未变化不触发任何运行更新。
    QJsonObject m_appliedConfig;
    QJsonObject configSnapshot() const;
    void applyConfigDiff(const QJsonObject &before, const QJsonObject &after);
    void applyPetTags(const QMap<QString, QString> &tags); // 桌宠：立绘 + 全局情绪同步

    CognitiveCoordinator *m_coordinator;
    void resumeMoodTimers();
    void recordInterruption(const QString &reason);
    bool canContactNow() const;
    // 切片 B：当前是否至少有一个可达的主动投递目标（桌宠恒可达；QQ 需已连接且是主人私聊）。
    // 提交前用它门控，避免把动作提交进一个此刻无论如何发不出去的渠道。
    bool hasReachableProactiveTarget() const;

    CharacterWidget *m_character;
    BubbleWidget *m_bubble;
    LLMService *m_llmService;
    AppearanceManager *m_appearance;
    GlobalMoodState *m_globalMood;   // 全局情绪（D2；外部情绪驱动桌宠受 D1 控制）
    TTSService *m_ttsService;
    ChatWidget *m_chatWidget;
    AnchorManager *m_anchorManager;
    // SettingsWidget *m_settingsWidget;
    SettingsWidget *m_settingsWidget;
    MemoryManager *m_memoryManager;
    TimeManager *m_timeManager;
    PetChannelAdapter *m_petChannel;    // 渠道：桌宠（气泡私有表现）
    QQChannelAdapter *m_qqChannel;      // 渠道：QQ（Phase 1 启用）
    QQProtocolClient *m_qqClient;       // OneBot WS 传输（入站暂只落 raw_events）
    SessionRouter *m_sessionRouter;     // 输入路由（渠道→大脑）
    RawEventProcessor *m_rawEventProcessor; // raw_events → interactions → summary
    GroupMembershipService *m_membershipService; // 群成员关系快照
    // 切片 B：三条主动外发（contact/reminder/share）统一走 committer 提交、dispatcher 发送。
    ActionCommitter *m_actionCommitter = nullptr;
    ActionOutboxDispatcher *m_actionOutbox = nullptr;

    quint64 m_lastPlaybackRound = 0;    // 最近一次入队 TTS 的轮次（退火门控用）
};

#endif // APPCONTROLLER_H
