#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QString>
#include <QStringList>
#include <QJsonObject>

#include "qqreplypolicy.h"
#include "memorypolicy.h"
#include <QSettings>
#include <QTime>

// QQ 协议端连接配置（setting.json 的 qq 段）
struct QQConfig {
    bool enabled = false;
    QString wsUrl = QStringLiteral("ws://127.0.0.1:3001");
    QString accessToken;
    QQReplyPolicy reply;
    QQGroupMemoryConfig groupMemory;
};

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

// 主动对话/私下写作配置：enabled 控制主动联系/分享，privateWriting 控制后台写作/回顾。
struct ProactiveConfig {
    bool enabled = false;
    // Private writing/review is authorized separately from proactive contact.
    bool privateWriting = false;
    QList<QPair<QString, QString>> targets;  // 发起对象 (channel, sessionId)，默认 [pet/default]
    int dailyLimit = 3;                      // 每日主动上限
    int cooldownSeconds = 900;               // 两次主动的最小间隔
    QString quietStart = "23:00";            // 免打扰时段
    QString quietEnd = "08:00";
    int idleSeconds = 1800;                  // 闲聊冷却（自上次对话起）
    QList<ScheduleItem> schedule;            // 定时主动
    // 执行端约束：主动联系额度与关注（concern）的到期策略。
    int contactDailyLimit = 3;               // 每日"现在联系"投递上限；0=不限制
    int concernDefaultCheckMinutes = 720;    // 关注默认多久后再考虑；0=只在新证据时
    int concernRetentionDays = 14;           // 超过多少天的关注不再按时间到期回顾（仍可被新证据命中）
    // R12-0 回顾失败退避（仅 Reflection 强制退避）：identity.json 的
    // cognition.review_backoff_seconds / cognition.max_review_failures。
    // 内存里的连续失败计数是临时方案；跨重启的可靠计数留给 R12-1a v31 持久化字段。
    QList<int> reviewBackoffSeconds{120,300,900};
    // 默认 4：前三次失败按 120/300/900 退避，第 4 次连续失败才 suspended。
    int maxReviewFailures = 4;
};

// 语义核验（报告切片 C）。identity.json：
//   cognition.semantic_verifier.enabled / max_verifier_calls / max_repairs
// 缺省值即安全默认：核验开启、每 action 最多 1 次核验 + 1 次修复。
struct SemanticVerifierConfig {
    bool enabled = true;                       // 是否允许调用一次性语义验证器
    int maxVerifierCalls = 1;                  // 每个 action 最多几次 verifier 调用
    int maxRepairs = 1;                        // 每个 action 最多几次 constrained repair
};

// R12-1a 内部唤醒（OpenWake）配置。identity.json 的 cognition.open_wake 段。
// 本片只做内部 rest/reflect：allowContact 会被解析保存，但运行时绝不产生 contact；
// enabled=false 时不创建唤醒，daily_limit=0 时连 OpenWake 一起停掉（由协调器门控）。
// 所有整数字段在 ConfigManager 里夹紧，运行期不再二次判断。
struct OpenWakeConfig {
    bool enabled = false;             // cognition.open_wake.enabled
    bool allowContact = false;        // 本片仅解析，运行时不产生 contact
    int initialDelaySeconds = 900;    // 首次调度延迟（60..86400）
    int minIntervalSeconds = 300;     // 模型可提议的最小间隔（300..86400，内部秒）
    int defaultIntervalSeconds = 1800;// 模型没给 next_wake 时的默认间隔（300..86400）
    int maxIntervalSeconds = 86400;   // 模型可提议的最大间隔（300..86400）
    int maxNonOpenJobsWhenDue = 3;    // 到期后先让出的普通回顾次数（0..20）
    int maxModelStepsPerWake = 3;     // 单次唤醒模型步数上限（1..8；本片用于日志/常量）
    int maxPerDay = 12;               // OpenWake 独立每日上限（0..96）
    int sharedBudgetPercent = 30;     // 共享回顾每日预算占比（1..100）
};


class ConfigManager
{
public:
    static ConfigManager& instance(){
        static ConfigManager instance;
        return instance;
    }

    bool loadSetting();
    bool writeConfigDocument(const QString &name, const QByteArray &contents, QString *error);
    bool saveSetting();
    QJsonObject effectiveSettings() const;


    // 删除拷贝构造和赋值操作，确保单例纯粹性
    ConfigManager(const ConfigManager&) = delete;
    void operator=(const ConfigManager&) = delete;

    QString getApiKey() const;
    void setApiKey(const QString &apiKey);
    bool visionEnabled() const { return m_visionEnabled; }
    QString visionModel() const { return m_visionModel; }
    QString getDashScopeApiKey() const;
    void setDashScopeApiKey(const QString &apiKey);
    QString getDashScopeBaseUrl() const;
    void setDashScopeBaseUrl(const QString &baseUrl);

    QString getTTSUrl() const;
    void setTTSUrl(const QString &ttsUrl);
    QString getTTSMode() const;
    void setTTSMode(const QString &mode);
    QString defaultTTSReferenceDirectory() const;
    QString ttsReferenceDirectory() const;

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
    bool getDebugFullLogs() const;
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
    QString logDirectory() const;
    int inputPauseMs() const { return m_inputPauseMs; }
    bool separateLogRuns() const { return m_separateLogRuns; }
    QString getMemoryPath()const;

    // ===== 多渠道扩展配置（identity.json，Phase 0 只读解析，缺失时用默认值）=====
    QStringList getSelfQQAccounts();      // 她自己的 QQ（防自触发、群聊回声排除）
    QStringList getMasterQQAccounts();    // 主人的 QQ 列表（多账号均归 subject='user'）
    bool isMasterQQ(const QString &uin);
    QList<BlindZone> blindZones() const { ensureIdentityLoaded(); return m_blindZones; }
    bool isBlindZone(const QString &channel, const QString &sessionId);
    bool getFollowExternalMood();         // D1：外部渠道情绪是否驱动桌设立绘（默认 true）
    ProactiveConfig getProactiveConfig();
    // 语义核验配置（identity.json 的 cognition 段），已夹紧。
    SemanticVerifierConfig getSemanticVerifierConfig();
    // R12-1a：OpenWake 调度配置（identity.json 的 cognition.open_wake 段），已夹紧。
    OpenWakeConfig getOpenWakeConfig() const;
    // R12-2a：OpenWake 主动联系授权（cognition.open_wake.allow_contact，默认 false）。
    // 与 enabled 独立：允许联系不代表允许创建唤醒；未显式开启时永远返回 false。
    // 这是纯读取确认接口，不改变任何 UI/写配置行为。
    bool openWakeAllowContact() const;
    // R12-1a：认知快照提示词字节预算（cognition.snapshot_budget_bytes，默认 4096，夹 2048..8192）。
    // 本片只解析/保存，快照 builder 的接线由下一 agent 完成。
    int getSnapshotBudgetBytes() const;
    QQConfig getQQConfig() const;
    MemoryPolicyConfig getMemoryPolicyConfig() const;
    QQGroupMemoryConfig getQQGroupMemoryConfig() const;
    void reloadIdentity();                // 重新读取 identity.json（配置热更新/测试用）
    void loadIdentityConfig();            // 启动时显式加载 identity.json（触发校验与日志）

private:
    ConfigManager();
    ~ConfigManager() = default;

    void initAppDataPath();

    QString m_apiKey;
    bool m_visionEnabled=false;
    QString m_visionModel="qwen3-vl-plus";
    QString m_dashscopeApiKey;
    QString m_dashscopeBaseUrl;

    QString m_ttsUrl;
    QString m_ttsMode = "api";
    QString m_ttsReferenceDirectory;
    QString m_gptWeightsPath;
    QString m_sovitsWeightsPath;
    QString m_gptSovitsRootPath;
    QString m_pythonPath;

    int m_shortMemoryLength=15;
    bool m_qqEnabled=false;
    QString m_qqWsUrl=QStringLiteral("ws://127.0.0.1:3001");
    QString m_qqAccessToken;
    QQReplyPolicy m_qqReply;
    QQGroupMemoryConfig m_qqGroupMemory;
    MemoryPolicyConfig m_memoryPolicy;
    bool m_debugFullLogs = false;
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
    mutable SemanticVerifierConfig m_semanticVerifier;
    mutable OpenWakeConfig m_openWake;
    mutable int m_snapshotBudgetBytes = 4096;
    void ensureIdentityLoaded() const;

    QString m_logDirectory;
    int m_inputPauseMs = 1600;
    bool m_separateLogRuns = true;
    QString m_configDirPath;
    QString m_configFilePath;
    QString m_gptModelDir;
    QString m_sovitsModelDir;


};

#endif // CONFIGMANAGER_H
