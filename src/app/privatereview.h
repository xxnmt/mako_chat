#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <optional>

class MemoryManager;

// R12-0：一次唤醒的类型化结果。调用方可以忽略返回值（C++ 允许忽略返回值），
// 但协调器/测试/日志用 reason 区分"没有候选"与"被预算/冷却/数据库挡住"：
//   started       真正领到一次回顾
//   no_candidate  没有到期活动/关注/新来源
//   busy          已有在途回顾
//   disabled / no_provider / no_idle 由协调器在调用前填写
//   daily_limit / cooldown / cas_conflict / db_error / storage_error 由记忆层领取结果填写
struct WakeResult {
    bool started = false;
    QString kind;                 // activity | concern | reflection | none
    QString reason;               // 见上
    QString sourceRef;            // thought/<id> | reflection/<turn> | concern/<turn>；未知为空
    int version = 0;              // 本次领取的回顾版本（领到才有意义）
    QDateTime nextEligibleAt;     // 已知的下次可领取时刻（冷却/每日上限）；未知时无效
};

// 一次在途的私下回顾。领取优先级：到期活动 → 到期关注 → 新来源回顾。
// version 同时用于防止进程重启后的迟到回调落库。
class PrivateReview final : public QObject {
    Q_OBJECT
public:
    explicit PrivateReview(MemoryManager &memory, QObject *parent = nullptr);
    // R12-0：返回类型化结果，reason 见 WakeResult；原有调用方可忽略返回值。
    WakeResult wake(int dailyLimit, int cooldownSeconds);
    void yieldAttention(const QString &reason);
    void receive(qlonglong id, int version, const QJsonObject &proposal, const QString &error);

signals:
    void contactProposed(qlonglong turn, int version);
    void requested(qlonglong id, int version, const QJsonObject &context);
    void cancelled();
    void shareRequested(const QString &ref, int version, const QString &draft);

private:
    // Activity=自己的文字活动；Reflection=新来源回顾；Concern=到期关注回顾。
    enum class Kind { Activity, Reflection, Concern };
    struct Attempt { qlonglong id; int version; Kind kind; bool contactAllowed; };
    // R12-0 失败计数：key=kind/ref。count 是连续失败次数；version 记录最近一次失败的
    // 回顾版本，只在"版本跳变（>1）"时视作源被外部更新并允许恢复（启发式）。
    // 注意：这是内存计数，进程重启即归零；跨重启可靠计数留给 R12-1a v31 的持久化字段。
    struct FailureState { int count = 0; int version = -1; };
    static QString kindTag(Kind kind);
    static QString kindName(Kind kind);
    static QString sourceRef(const Attempt &attempt);
    static QString reviewStage(const QJsonObject &proposal, const QString &error, bool committed);
    // 失败退避：仅 Reflection 强制 120/300/900 → suspended(86400)；concern/activity
    // 保持原来的立即 defer，只记录诊断。yieldAttention 不经过这里，不累计失败。
    void noteFailure(const Attempt &attempt, const QString &ref, const QString &reason);
    void clearFailures(const QString &ref);
    MemoryManager &store;
    std::optional<Attempt> active;
    QHash<QString, FailureState> failures;
};
