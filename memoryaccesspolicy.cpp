#include "memoryaccesspolicy.h"

MemoryAccessDecision MemoryAccessPolicy::evaluate(
    const MemoryClaim &claim,
    const MemoryAccessContext &context)
{
    auto forbidden = [](const QString &reason) {
        return MemoryAccessDecision{MemoryAccessState::Forbidden, reason};
    };

    if (claim.claimStatus != QStringLiteral("active")) {
        return forbidden(QStringLiteral("claim_not_active"));
    }
    if (context.viewerPersonId.isEmpty()) {
        return forbidden(QStringLiteral("no_viewer"));
    }
    if (claim.aboutPersonId == QStringLiteral("unknown")
        || claim.privacyOwnerPersonId == QStringLiteral("unknown")
        || claim.assertedByPersonId == QStringLiteral("unknown")) {
        return forbidden(QStringLiteral("unknown_subject"));
    }
    if (claim.privacyLevel >= 3) {
        return forbidden(QStringLiteral("secret"));
    }
    if (claim.sourceChannel != QStringLiteral("qq_group")) {
        return forbidden(QStringLiteral("non_group_source"));
    }

    const bool aboutViewer = claim.aboutPersonId == context.viewerPersonId;
    const bool ownerOfClaim = claim.privacyOwnerPersonId == context.viewerPersonId;

    if (context.viewerIsOwner) {
        if (context.channel == QStringLiteral("qq_group")) {
            if (claim.sourceSessionId == context.sessionId
                && (aboutViewer || ownerOfClaim)) {
                return {MemoryAccessState::Shareable, QStringLiteral("owner_current_group")};
            }
            return forbidden(QStringLiteral("owner_group_other_source"));
        }
        if (context.allowedGroupIds.contains(claim.sourceSessionId)
            && (aboutViewer || ownerOfClaim)) {
            return {MemoryAccessState::Shareable, QStringLiteral("owner_allowed_group")};
        }
        return forbidden(QStringLiteral("owner_source_not_allowed"));
    }

    if (context.membershipFailed) {
        return forbidden(QStringLiteral("membership_failed"));
    }
    if (context.channel == QStringLiteral("qq_group")) {
        if (claim.sourceSessionId == context.sessionId
            && (aboutViewer || ownerOfClaim)) {
            return {MemoryAccessState::Shareable, QStringLiteral("viewer_current_group")};
        }
        return forbidden(QStringLiteral("other_group_or_subject"));
    }
    if (context.membershipVerified
        && context.allowedGroupIds.contains(claim.sourceSessionId)
        && (aboutViewer || ownerOfClaim)) {
        return {MemoryAccessState::Shareable, QStringLiteral("viewer_common_group")};
    }
    return forbidden(QStringLiteral("viewer_scope_denied"));
}
