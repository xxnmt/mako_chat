#ifndef MEMORYOVERVIEW_H
#define MEMORYOVERVIEW_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

#include "memoryagenttypes.h"
#include "memoryrecalltypes.h"

class MemoryManager;

struct MemoryOverviewResult {
    QJsonObject payload;
    QHash<QString, QJsonArray> handles;
};

// 区间概览：扫描完整时间范围、按桶聚合、返回覆盖率与可展开句柄。
// 它不是 top-k 检索，也不是数据库新表。
MemoryOverviewResult buildMemoryOverview(MemoryManager *manager,
                                         const MemoryAgentScope &scope,
                                         const MemoryRecallPlan &plan,
                                         QString *error);

#endif // MEMORYOVERVIEW_H
