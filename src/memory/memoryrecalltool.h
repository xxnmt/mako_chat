#ifndef MEMORYRECALLTOOL_H
#define MEMORYRECALLTOOL_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

#include "memoryagenttypes.h"

class MemoryManager;

// 主 Conversation Agent 可调用的唯一记忆工具。
// 只接受检索意图（needs），不信任任何 viewer/owner/group/session 权限字段。
class MemoryRecallTool
{
public:
    MemoryRecallTool(MemoryManager *memoryManager, const MemoryAgentScope &scope);

    static QJsonObject definition();
    static QJsonObject readDefinition();

    // 返回工具结果 JSON；invalid_arguments 等错误通过 error 返回，
    // 同时仍返回结构化错误对象，便于模型自行纠正。
    QJsonObject execute(const QJsonObject &arguments, QString *error);
    QJsonObject read(const QJsonObject &arguments, QString *error);

    const MemoryAgentResult &result() const { return m_result; }

private:
    MemoryManager *m_memoryManager = nullptr;
    MemoryAgentScope m_scope;
    MemoryAgentResult m_result;
    QHash<QString, QJsonArray> m_handles;
};

#endif // MEMORYRECALLTOOL_H
