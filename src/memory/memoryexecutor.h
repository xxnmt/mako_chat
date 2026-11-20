#ifndef MEMORYEXECUTOR_H
#define MEMORYEXECUTOR_H

#include "memoryagenttypes.h"
#include "memoryrecalltypes.h"

class MemoryManager;

// 确定性执行层：不调用 LLM，只按 Planner 的 needs 逐个检索。
class MemoryExecutor
{
public:
    MemoryExecutor(MemoryManager *memoryManager, const MemoryAgentScope &scope);

    MemoryAgentResult execute(const MemoryRecallPlan &plan);

private:
    MemoryManager *m_memoryManager = nullptr;
    MemoryAgentScope m_scope;
};

#endif // MEMORYEXECUTOR_H
