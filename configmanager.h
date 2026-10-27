#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QString>
#include <QStringList>
#include <QSettings>
#include <QTime>
#include "ttsreference.h"

// 记忆盲区（该渠道会话产生的对话不入长期记忆）
struct BlindZone {
    QString channel;
    QString sessionId;
};

// 主动对话调度项（Phase 2 使用）
struct ScheduleItem {
    QString at;      // "07:30" 固定时刻
    QString type;    // "fixed" | "random"（random=在窗口内随机）
};

// 主动对话配置（Phase 0 只解析不调度；Phase 2 接入 ChatPlanner）
struct ProactiveConfig {
    bool enabled = false;
    QList<QPair<QString, QString>> targets;  // 发起对象 (channel, sessionId)，默认 [pet/default]
    int dailyLimit = 3;                      // 每日主动上限
    int cooldownSeconds = 900;               // 两次主动的最小间隔
    QString quietStart = "23:00";            // 免打扰时段
    QString quietEnd = "08:00";
    int idleSeconds = 1800;                  // 闲聊冷却（自上次对话起）
    QList<ScheduleItem> schedule;            // 定时主动
};

class ConfigManager
{
public:
    static ConfigManager& instance(){
        static ConfigManager instance;
        return instance;
    }

    bool loadSetting();
    bool saveSetting();


    // 删除拷贝构造和赋值操作，确保单例纯粹性
    ConfigManager(const ConfigManager&) = delete;
    void operator=(const ConfigManager&) = delete;

    QString getApiKey() const;
    void setApiKey(const QString &apiKey);

    QString getTTSUrl() const;
    void setTTSUrl(const QString &ttsUrl);
    QString getTTSMode() const;
    void setTTSMode(const QString &mode);
    TTSReference getTTSReference() const;
    void setTTSReference(const TTSReference &ref);

    QString getGPTWeightsPath() const;
    void setGPTWeightsPath(const QString &path);
    QString getSoVITSWeightsPath() const;
    void setSoVITSWeightsPath(const QString &path);

    QString getGPTSovitsRootPath() const;
    QString getPythonPath() const;
    void setPythonPath(const QString &path);
    void setGPTSovitsRootPath(const QString &path);
    QString getGPTModelDir() const;
    void setGPTModelDir(const QString &path);
    QString getSoVITSModelDir() const;
    void setSoVITSModelDir(const QString &path);

    int getShortMemoryLength();
    void setShortMemoryLength(const int length);

    // ===== 长期记忆 / 时间管理（TD-037：设置界面接线）=====
    int getSummaryMemoryLength() const;
    void setSummaryMemoryLength(int length);
    QTime getNightStartTime() const;
    void setNightStartTime(const QTime &time);
    QTime getNightEndTime() const;
    void setNightEndTime(const QTime &time);
    int getBlushLasting() const;
    void setBlushLasting(int seconds);
    int getEmotionLasting() const;
    void setEmotionLasting(int seconds);
    int getDistanceLasting() const;
    void setDistanceLasting(int seconds);

    QString getConfigDirPath()const;
    QString getConfigFilePath()const;
    QString getAppDataPath()const;
    QString getMemoryPath()const;

    // ===== 多渠道扩展配置（identity.json，Phase 0 只读解析，缺失时用默认值）=====
    QStringList getSelfQQAccounts();      // 她自己的 QQ（防自触发、群聊回声排除）
    QStringList getMasterQQAccounts();    // 主人的 QQ 列表（多账号均归 subject='user'）
    bool isMasterQQ(const QString &uin);
    bool isBlindZone(const QString &channel, const QString &sessionId);
    bool getFollowExternalMood();         // D1：外部渠道情绪是否驱动桌设立绘（默认 true）
    ProactiveConfig getProactiveConfig();
    void reloadIdentity();                // 重新读取 identity.json（配置热更新/测试用）
    void loadIdentityConfig();            // 启动时显式加载 identity.json（触发校验与日志）

private:
    ConfigManager();
    ~ConfigManager() = default;

    void initAppDataPath();

    QString m_apiKey;

    QString m_ttsUrl;
    QString m_ttsMode = "api";
    TTSReference m_ttsRef;
    QString m_gptWeightsPath;
    QString m_sovitsWeightsPath;
    QString m_gptSovitsRootPath;
    QString m_pythonPath;

    int m_shortMemoryLength=15;
    int m_summaryMemoryLength=15;          // 长期记忆自动摘轮数（未摘要轮次阈值）
    QTime m_nightStart{22, 0};             // 夜晚开始
    QTime m_nightEnd{7, 0};                // 夜晚结束（支持跨零点）
    int m_blushLasting=30;                 // 脸红退火（秒）
    int m_emotionLasting=15;               // 表情退火（秒）
    int m_distanceLasting=15;              // 距离退火（秒）

    // identity.json 懒加载缓存（mutable：只读接口内部触发加载）
    mutable bool m_identityLoaded = false;
    mutable QStringList m_selfQQ;
    mutable QStringList m_masterQQ;
    mutable QList<BlindZone> m_blindZones;
    mutable bool m_followExternalMood = true;
    mutable ProactiveConfig m_proactive;
    void ensureIdentityLoaded() const;

    QString m_configDirPath;
    QString m_configFilePath;
    QString m_gptModelDir;
    QString m_sovitsModelDir;


};

#endif // CONFIGMANAGER_H
