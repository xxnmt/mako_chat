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

    QString m_lastUserInput;

    CharacterWidget *m_character;
    BubbleWidget *m_bubble;
    LLMService *m_llmService;
    AppearanceManager *m_appearance;
    TTSService *m_ttsService;
    ChatWidget *m_chatWidget;
    AnchorManager *m_anchorManager;
    // SettingsWidget *m_settingsWidget;
    SettingsWidget *m_settingsWidget;
    MemoryManager *m_memoryManager;
    TimeManager *m_timeManager;
    PetChannelAdapter *m_petChannel;    // 渠道：桌宠（气泡私有表现）
    QQChannelAdapter *m_qqChannel;      // 渠道：QQ（Phase 1 启用）
    SessionRouter *m_sessionRouter;     // 输入路由（渠道→大脑）
    RequestMeta m_lastMeta;             // 最近一轮请求元数据（回复入库时随存）
};

#endif // APPCONTROLLER_H
