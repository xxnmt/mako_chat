#ifndef MEMORYRESULTJSON_H
#define MEMORYRESULTJSON_H

#include <QJsonObject>

#include "memoryagenttypes.h"

// 把一次只读检索结果转换成模型可读的 JSON。
// 不暴露数据库主键、person_id、session_id 等内部字段；item.id 只是本次 run 的句柄。
QJsonObject memoryAgentResultToJson(const MemoryAgentResult &result,
                                    int maxChars = 2000,
                                    int maxItemChars = 400);

#endif // MEMORYRESULTJSON_H
