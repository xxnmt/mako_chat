#ifndef MEMORYSOURCEPOLICY_H
#define MEMORYSOURCEPOLICY_H

#include <QString>

#include "memoryagenttypes.h"
#include "utterance.h"

// 数据源访问策略。搜索范围与逐条披露分开；模型参数不能改变这些判断。
class MemorySourcePolicy
{
public:
    static bool isOwnerPrivate(const MemoryAgentScope &scope);
    static bool isOwnerGroup(const MemoryAgentScope &scope);

    static bool canReadGroupRecent(const MemoryAgentScope &scope,
                                   const QString &sourceGroupId);
    static bool canReadInteractionSummary(const MemoryAgentScope &scope,
                                          const QString &channel,
                                          const QString &sessionId);
    static bool canReadEpisodic(const MemoryAgentScope &scope,
                                const QString &origin,
                                const QString &visibility,
                                const QString &subject,
                                const QString &sessionId);
    static bool canReadLongTermSummary(const MemoryAgentScope &scope,
                                       const QString &channel,
                                       const QString &sessionId);
    static bool canReadChatHistory(const MemoryAgentScope &scope,
                                   const QString &channel,
                                   const QString &sessionId);
};

#endif // MEMORYSOURCEPOLICY_H
