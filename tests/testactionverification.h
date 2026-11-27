#pragma once

#include "actionpolicy.h"
#include "memorymanager.h"

// Fixtures can promote a proposal without a network service.
inline bool markTestActionProposalVerified(MemoryManager &store, const QString &actionKey,
                                               const QString &semanticStatus,
                                               const QString &verifiedRequestId, QString *error)
{
    if (error) error->clear();
    const auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return false;
    };
    const QString key = actionKey.trimmed();
    const QString semantic = semanticStatus.trimmed();
    const QString requestId = verifiedRequestId.trimmed();
    if (key.isEmpty()) return fail(QStringLiteral("invalid_action_key"));
    if (semantic.isEmpty() || semantic == QStringLiteral("withheld"))
        return fail(QStringLiteral("invalid_semantic_status"));
    if (requestId.isEmpty()) return fail(QStringLiteral("missing_verified_request_id"));
    if (requestId.size() > 120) return fail(QStringLiteral("invalid_verified_request_id"));
    const QJsonObject row = store.actionProposal(key);
    if (row.isEmpty()) return fail(QStringLiteral("not_found"));
    ActionOrigin origin;
    QString originError;
    if (!ActionOrigin::fromJson(row.value(QStringLiteral("origin")).toObject(), &origin,
                                &originError))
        return fail(originError.isEmpty() ? QStringLiteral("invalid_origin") : originError);
    const QJsonObject payload = row.value(QStringLiteral("payload")).toObject();
    const QJsonArray claims = row.value(QStringLiteral("claims")).toArray();
    VerificationBinding binding;
    binding.requestId = requestId;
    binding.actionKey = key;
    binding.kind = row.value(QStringLiteral("kind")).toString();
    binding.originKind = origin.kindName();
    binding.originRef = origin.ref;
    binding.originId = origin.id;
    binding.originVersion = origin.version;
    binding.runId = origin.runId;
    binding.scope = origin.scope;
    binding.originContextEpoch = origin.contextEpoch;
    binding.actionContextEpoch = row.value(QStringLiteral("action_context_epoch")).toInteger();
    binding.targetChannel = origin.targetChannel;
    binding.targetSession = origin.targetSession;
    binding.payloadDigest = ActionPolicy::canonicalProposalPayloadDigest(payload, claims);
    binding.claimsDigest = ActionPolicy::canonicalProposalClaimsDigest(claims);
    ActionPolicy policy(&store);
    QJsonArray sources;
    if (!policy.proposalEvidence(claims, &sources))
        return fail(QStringLiteral("verification_evidence_unavailable"));
    binding.evidenceDigest = ActionPolicy::canonicalEvidenceDigest(claims, sources);
    binding.verifierInputDigest = ActionPolicy::canonicalJsonDigest(
        QJsonObject{{QStringLiteral("payload"), payload}, {QStringLiteral("claims"), claims}});
    binding.state = QStringLiteral("verified");
    binding.semanticStatus = semantic;
    return store.markActionProposalVerified(key, binding, error);
}
