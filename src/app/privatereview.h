#pragma once

#include <QObject>
#include <QJsonObject>
#include <optional>

class MemoryManager;

// One in-flight private review (existing activity or independent reflection). Its version also guards callbacks after restart.
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
    struct Attempt { qlonglong id; int version; bool reflection; bool contactAllowed; };
    MemoryManager &store;
    std::optional<Attempt> active;
};
