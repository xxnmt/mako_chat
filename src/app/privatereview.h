#pragma once

#include "cognitiveorigin.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <optional>

class MemoryManager;
struct OpenWakeConfig;

// R12-0：一次唤醒的类型化结果。调用方可以忽略返回值（C++ 允许忽略返回值），
// 但协调器/测试/日志用 reason 区分"没有候选"与"被预算/冷却/数据库挡住"：
//   started       真正领到一次回顾
//   no_candidate  没有到期活动/关注/新来源/内部唤醒
//   busy          已有在途回顾
//   disabled / no_provider / no_idle 由协调器在调用前填写
//   daily_limit / cooldown / cas_conflict / db_error / storage_error 由记忆层领取结果填写
//   budget / suspended / not_due 由记忆层 OpenWake 领取结果填写（R12-1a）
struct WakeResult {
    bool started = false;
    QString kind;                 // activity | concern | reflection | open_wake | none
    QString reason;               // 见上
    QString sourceRef;            // thought/<id> | reflection/<turn> | concern/<turn> | wake/<id>；未知为空
    int version = 0;              // 本次领取的回顾版本（领到才有意义）
    QDateTime nextEligibleAt;     // 已知的下次可领取时刻（冷却/每日上限/暂停）；未知时无效
};

// 一次在途的私下回顾。领取优先级（R12-1a）：
//   1) OpenWake 到期且已让出足够的普通回顾（fairness）→ 先领 OpenWake；
//   2) 否则走原有 到期活动 → 到期关注 → 新来源回顾；
//   3) 前三步都没有候选/被预算挡住时，若 OpenWake 到期再尝试领取。
//
// 每次领取都生成一个 CognitiveOrigin 并随 requested context 下发；模型层原样回显到
// proposal["_origin"]，receive 逐字段校验 kind/ref/id/version/runId/scope/epoch，
// 不一致的迟到回调直接丢弃（[Review] stale_callback），不提交、不发送。
class PrivateReview final : public QObject {
    Q_OBJECT
public:
    explicit PrivateReview(MemoryManager &memory, QObject *parent = nullptr);
    // R12-0：返回类型化结果，reason 见 WakeResult；原有调用方可忽略返回值。
    // 两参数重载保持旧行为：默认 OpenWake disabled，与 R12-0 调用/测试兼容。
    WakeResult wake(int dailyLimit, int cooldownSeconds);
    // R12-1a：带 OpenWake 配置的调度。仅当 openWake.enabled 且 dailyLimit>0 时才会领取
    // 内部唤醒；allowContact 本片不产生任何联系。
    WakeResult wake(int dailyLimit, int cooldownSeconds, const OpenWakeConfig &openWake);
    void yieldAttention(const QString &reason);
    void receive(qlonglong id, int version, const QJsonObject &proposal, const QString &error);

signals:
    void contactProposed(qlonglong turn, int version);
    void requested(qlonglong id, int version, const QJsonObject &context);
    void cancelled();
    void shareRequested(const QString &ref, int version, const QString &draft);

private:
    // Activity=自己的文字活动；Reflection=新来源回顾；Concern=到期关注；OpenWake=内部唤醒。
    struct Attempt {
        CognitiveOrigin origin;
        bool contactAllowed = false;
    };

    static QString kindTag(CognitiveOrigin::Kind kind);
    static QString kindName(CognitiveOrigin::Kind kind);
    static QString sourceRef(const Attempt &attempt);
    static QString reviewStage(const QJsonObject &proposal, const QString &error, bool committed);
    // R12-1a：错误分类决定是否计入持久失败。网络/截断/解析/schema/提交失败/DB 错误计入；
    // 策略/权限拦截与"来源已变/CAS 冲突"不计入认知失败（但仍按旧语义 defer）。
    static bool errorCountsAsFailure(const QString &error);

    // 统一调度入口：openWake 为空=旧行为（不碰 OpenWake）；nonOpenAllowed=false 时
    // 跳过活动/关注/新来源（OpenWake 在 privateWriting=false 时也能单独运行）。
    WakeResult wakeInternal(int dailyLimit, int cooldownSeconds,
                            const OpenWakeConfig *openWake, bool nonOpenAllowed);
    // 原有 Activity→Concern→Reflection 三步。没有候选时返回 kind=none/no_candidate。
    WakeResult wakeNonOpen(int dailyLimit, int cooldownSeconds);
    // 领取一次 OpenWake；失败时返回 reason（budget/suspended/not_due/busy/db_error）。
    WakeResult wakeOpenWake(const OpenWakeConfig &openWake, int dailyLimit, int cooldownSeconds);
    // OpenWake 的 rest/reflect 回调处理（schema 校验 + 提交/失败接线）。
    void receiveOpenWake(const Attempt &attempt, const QJsonObject &proposal, const QString &error);
    // proposal["_origin"] 与 active origin 的比对。缺 _origin 时只允许两类兼容：
    //   1) 非 OpenWake 的旧路径（按 id/version 校验，兼容 R12-0 测试）；
    //   2) OpenWake 且带非空 error 的失败回调（LLMService 早期错误路径不解析 proposal）。
    // OpenWake 的成功 proposal 必须严格携带 _origin。
    bool originMatches(const Attempt &attempt, const QJsonObject &proposal,
                       const QString &error) const;
    // R12-1a 持久失败：写 review_failure 并记录 [Review] backoff/suspended；
    // countsAsFailure=false 时按原语义立即 defer（用户让出/策略拦截不计失败）。
    void recordFailure(const Attempt &attempt, const QString &reason,
                       const QString &stage, bool countsAsFailure);

    MemoryManager &store;
    std::optional<Attempt> active;
};
