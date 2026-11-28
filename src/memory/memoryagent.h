#ifndef MEMORYAGENT_H
#define MEMORYAGENT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QScopedPointer>
#include <functional>

#include "memoryagenttypes.h"
#include "memorypolicy.h"
#include "utterance.h"

class MemoryManager;

// PR-4：只读记忆检索工具。所有 scope 由服务端注入；参数中不存在
// viewer/owner/location/allowed_groups 等权限字段。
class MemoryAgentTools
{
public:
    MemoryAgentTools(MemoryManager *memoryManager, const MemoryAgentScope &scope,
                     bool useQueryTimeFallback = true);

    static bool canExposeTools(const RequestMeta &meta, const MemoryAgentConfig &config);

    // 返回 {"items":[...],"truncated":bool} 或 {"error":"...","message":"..."}。
    QJsonObject execute(const QString &name,
                        const QJsonObject &args,
                        QString *error);

    const QList<MemoryAgentItem> &items() const { return m_ordered; }
    bool truncated() const { return m_truncated; }

private:
    QJsonObject search(const QJsonObject &args, QString *error);
    QJsonObject read(const QJsonObject &args, QString *error);
    QJsonObject related(const QJsonObject &args, QString *error);
    QString groupLabelFor(const QString &groupId);

    void remember(const MemoryAgentItem &item);
    QJsonObject itemToJson(const MemoryAgentItem &item) const;
    QJsonObject errorObject(const QString &code, const QString &message) const;

    MemoryManager *m_memoryManager = nullptr;
    MemoryAgentScope m_scope;
    QHash<QString, MemoryAgentItem> m_cache;
    QList<MemoryAgentItem> m_ordered;
    QHash<QString, QString> m_groupNames;
    bool m_useQueryTimeFallback = true;
    bool m_truncated = false;
};

#endif // MEMORYAGENT_H
