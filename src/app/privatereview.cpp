#include "privatereview.h"
#include "memorymanager.h"
#include "configmanager.h"

PrivateReview::PrivateReview(MemoryManager &memory, QObject *parent)
    : QObject(parent), store(memory)
{
}

void PrivateReview::wake(int dailyLimit, int cooldownSeconds)
{
    if (active) return;
    auto context = store.nextThought(true);
    const bool reflection = context.isEmpty();
    if (reflection) context = store.claimPrivateReflection(dailyLimit, cooldownSeconds);
    if (context.isEmpty()) return;
    const auto id = context.take("id").toInteger();
    int version = context.take("version").toInt();
    if (!reflection) {
        if (!store.claimThoughtReview(id,version,dailyLimit,cooldownSeconds)) return;
        ++version;
    }
    const auto options = ConfigManager::instance().getProactiveConfig();
    active = Attempt{id,version,reflection,options.enabled && !options.targets.isEmpty()};
    qInfo() << (reflection ? "[Reflection] reviewing" : "[Activity] reviewing") << id << version;
    emit requested(id, active->version, context);
}

void PrivateReview::yieldAttention(const QString &reason)
{
    if (!active) return;
    const auto attempt = *active;
    active.reset();
    const bool saved = attempt.reflection ? store.deferPrivateReflection(attempt.id,attempt.version)
                                          : store.deferThoughtReview(attempt.id, attempt.version, reason);
    qInfo() << "[Activity] yielded" << attempt.id << attempt.version << reason << saved;
    emit cancelled();
}

void PrivateReview::receive(qlonglong id, int version, const QJsonObject &proposal, const QString &error)
{
    if (!active || active->id != id || active->version != version) return;
    const bool reflection = active->reflection;
    const bool contactAllowed = active->contactAllowed && ConfigManager::instance().getProactiveConfig().enabled;
    active.reset();
    if (reflection) {
        const auto choice = proposal["choice"].toString();
        const auto contact = contactAllowed ? proposal["contact"].toObject() : QJsonObject{};
        if (error.isEmpty() && store.finishPrivateReflection(id,version,choice,proposal["note"].toString(),contact)) {
            qInfo() << "[Reflection] committed" << id << version << choice << contact["timing"].toString();
            if (choice=="consider" && contact["timing"].toString()=="now") emit contactProposed(id,version);
        } else {
            store.deferPrivateReflection(id,version);
            qInfo() << "[Reflection] deferred" << id << version << error;
        }
        return;
    }
    if (!error.isEmpty() || !store.finishThoughtReview(id, version, proposal)) {
        // The original claim still counts towards the cooldown and daily budget.
        const QString reason = error.isEmpty() ? QStringLiteral("invalid_or_stale_proposal") : error;
        store.deferThoughtReview(id, version, reason);
        qInfo() << "[Activity] review did not commit" << id << version << reason;
        return;
    }
    qInfo() << "[Activity] review committed" << id << version + 1;
    const auto draft = proposal["draft"].toString().trimmed();
    if (!draft.isEmpty() && proposal["share_with_owner"].toBool() && store.canShareThought(id, version + 1))
        emit shareRequested(QStringLiteral("thought/%1").arg(id), version + 1, draft);
}
