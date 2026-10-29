#ifndef MEMORYACCESSPOLICY_H
#define MEMORYACCESSPOLICY_H

#include <QString>
#include <QStringList>

#include "historyturn.h"

enum class MemoryAccessState {
    Shareable,
    OwnerCandidate,
    KnownOnly,
    Forbidden
};

struct MemoryAccessContext {
    QString viewerPersonId;
    QString channel;
    QString sessionId;
    QStringList allowedGroupIds;
    bool viewerIsOwner = false;
    bool membershipVerified = false;
    bool membershipFailed = false;
};

struct MemoryAccessDecision {
    MemoryAccessState state = MemoryAccessState::Forbidden;
    QString reason;
};

class MemoryAccessPolicy
{
public:
    static MemoryAccessDecision evaluate(const MemoryClaim &claim,
                                         const MemoryAccessContext &context);
};

#endif // MEMORYACCESSPOLICY_H
