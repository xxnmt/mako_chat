#ifndef APPCONTROLLER_H
#define APPCONTROLLER_H

#include <QObject>
#include <QQueue>
#include <QMap>
#include "sentencedata.h"
#include "utterance.h"

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

class AppController : public QObject
{
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController();

    void startApp();

private slots:
    void handleMakoReply(const QList<SentenceText> &sentences,const QString &rawReply);
    void handleSystemError(const QString &errorMsg);
    void handleAbnormalReply(const QString &reason);
    void onPlayAudioAction(const QString &zhText, const QMap<QString, QString> &tags);
signals:

private:
    void initConnections();
    void applyTimeSettings();   // 把 ConfigManager 的时间/退火配置应用到 TimeManager
    void applyPetTags(const QMap<QString, QString> &tags); // 桌宠：立绘 + 全局情绪同步

    QString m_lastUserInput;

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
    RequestMeta m_lastMeta;             // 最近一轮请求元数据（回复入库时随存）
    quint64 m_roundSeq = 0;             // 对话轮次序号：每次用户输入 +1（TD-034）
    quint64 m_lastPlaybackRound = 0;    // 最近一次入队 TTS 的轮次（退火门控用）
};

#endif // APPCONTROLLER_H
