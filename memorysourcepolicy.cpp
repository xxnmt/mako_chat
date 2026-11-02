#include "memorysourcepolicy.h"

bool MemorySourcePolicy::isOwnerPrivate(const MemoryAgentScope &scope)
{
    return scope.viewerIsOwner && scope.channel == Channel::kQQPrivate;
}

bool MemorySourcePolicy::isOwnerGroup(const MemoryAgentScope &scope)
{
    return scope.viewerIsOwner && scope.channel == Channel::kQQGroup;
}

bool MemorySourcePolicy::canReadGroupRecent(const MemoryAgentScope &scope,
                                            const QString &sourceGroupId)
{
    if (sourceGroupId.isEmpty()) {
        return false;
    }
    if (scope.channel == Channel::kQQGroup) {
        return sourceGroupId == scope.sessionId
               && scope.allowedGroupIds.contains(sourceGroupId);
    }
    return scope.allowedGroupIds.contains(sourceGroupId);
}

bool MemorySourcePolicy::canReadInteractionSummary(
    const MemoryAgentScope &scope,
    const QString &channel,
    const QString &sessionId)
{
    // 群级摘要包含多个群友的表达，非 Owner 私聊不直接读取。
    if (isOwnerPrivate(scope)) {
        return channel == Channel::kQQGroup
               && scope.allowedGroupIds.contains(sessionId);
    }
    if (scope.channel == Channel::kQQGroup) {
        return channel == Channel::kQQGroup && sessionId == scope.sessionId;
    }
    return false;
}

bool MemorySourcePolicy::canReadEpisodic(const MemoryAgentScope &scope,
                                         const QString &origin,
                                         const QString &visibility,
                                         const QString &subject,
                                         const QString &sessionId)
{
    if (visibility == QStringLiteral("public")) {
        return true;
    }
    if (visibility == QStringLiteral("channel")) {
        return origin == scope.channel && sessionId == scope.sessionId;
    }
    if (visibility != QStringLiteral("private")) {
        return false;
    }

    if (!isOwnerPrivate(scope)) {
        return false;
    }
    if (subject != QStringLiteral("user") && subject != scope.viewerPersonId) {
        return false;
    }
    if (origin == QStringLiteral("pet")
        && sessionId == Channel::kDefaultSession) {
        return scope.ownerCrossChannelPrivate;
    }
    return origin == scope.channel && sessionId == scope.sessionId;
}

bool MemorySourcePolicy::canReadLongTermSummary(const MemoryAgentScope &scope,
                                                const QString &channel,
                                                const QString &sessionId)
{
    if (isOwnerPrivate(scope)) {
        if (channel == Channel::kPet
            && sessionId == Channel::kDefaultSession) {
            return scope.ownerCrossChannelPrivate;
        }
        return channel == scope.channel && sessionId == scope.sessionId;
    }
    if (scope.channel == Channel::kQQGroup) {
        return channel == Channel::kQQGroup && sessionId == scope.sessionId
               && scope.viewerIsOwner;
    }
    return false;
}

bool MemorySourcePolicy::canReadChatHistory(const MemoryAgentScope &scope,
                                            const QString &channel,
                                            const QString &sessionId)
{
    if (isOwnerPrivate(scope)) {
        if (channel == Channel::kPet
            && sessionId == Channel::kDefaultSession) {
            return scope.ownerCrossChannelPrivate;
        }
        return channel == scope.channel && sessionId == scope.sessionId;
    }
    if (scope.channel == Channel::kQQGroup) {
        return channel == Channel::kQQGroup && sessionId == scope.sessionId;
    }
    return false;
}
