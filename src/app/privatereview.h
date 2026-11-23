#pragma once

#include <QObject>
#include <QJsonObject>
#include <optional>

class MemoryManager;

// 一次在途的私下回顾。领取优先级：到期活动 → 到期关注 → 新来源回顾。
// version 同时用于防止进程重启后的迟到回调落库。
class PrivateReview final : public QObject {
    Q_OBJECT
public:
    explicit PrivateReview(MemoryManager &memory, QObject *parent = nullptr);
    void wake(int dailyLimit, int cooldownSeconds);
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
    static QString kindTag(Kind kind);
    MemoryManager &store;
    std::optional<Attempt> active;
};
