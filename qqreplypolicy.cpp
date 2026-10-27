#include "qqreplypolicy.h"
#include "utterance.h"

QQReplyDecision QQReplyPolicyEngine::decide(const QQInboundMessage &message,
                                            const QQReplyPolicy &policy,
                                            const QStringList &masterQqs)
{
    if (!message.valid) {
        return QQReplyDecision::Drop;
    }

    const bool isMaster = masterQqs.contains(message.senderId);

    if (message.channel == Channel::kQQPrivate) {
        if (policy.privateMode == QStringLiteral("none")) {
            return QQReplyDecision::Drop;
        }
        if (policy.privateMode == QStringLiteral("all")) {
            return QQReplyDecision::Reply;
        }
        if (policy.privateMode == QStringLiteral("master")) {
            return isMaster ? QQReplyDecision::Reply : QQReplyDecision::Drop;
        }
        if (policy.privateMode == QStringLiteral("allowlist")) {
            if (isMaster) {
                return QQReplyDecision::Reply;
            }
            return policy.privateAllow.contains(message.senderId.toLongLong())
                       ? QQReplyDecision::Reply
                       : QQReplyDecision::Drop;
        }
        return QQReplyDecision::Drop;
    }

    if (message.channel == Channel::kQQGroup) {
        if (policy.groupMode == QStringLiteral("none")) {
            return QQReplyDecision::Drop;
        }

        const qlonglong groupId = message.sessionId.toLongLong();
        bool groupAllowed = policy.groupAllow.isEmpty() || policy.groupAllow.contains(groupId);
        if (!groupAllowed && policy.masterAnyGroup && isMaster) {
            groupAllowed = true;
        }
        if (!groupAllowed) {
            return QQReplyDecision::Drop;
        }

        if (policy.groupMode == QStringLiteral("mention") && !message.mentionedSelf) {
            return QQReplyDecision::RecordOnly;
        }
        return QQReplyDecision::Reply;
    }

    return QQReplyDecision::Drop;
}
