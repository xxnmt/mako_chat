#ifndef MEMORYAGENTTYPES_H
#define MEMORYAGENTTYPES_H

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>

#include "memoryaccesspolicy.h"
#include "memoryrecalltypes.h"
#include "recentcontext.h"

// MemoryAgent 的服务端作用域。所有字段由 LLMService 在服务端构建；
// Planner 只能提供 query/kinds/source_hint/time.expression，不能覆盖
// viewer/owner/location/allowed_groups。
struct MemoryAgentScope {
    QString requestId;
    QString channel;
    QString sessionId;
    QString viewerPersonId;
    bool viewerIsOwner = false;
    QStringList allowedGroupIds;
    bool membershipVerified = false;
    bool membershipFailed = false;
    int maxResults = 8;
    int timeoutMs = 6000;
    int maxRounds = 2;
    bool planV2 = true;
    QString timezone;
    QList<PlannerTurn> recentTurns;
    MemoryAccessContext access;
    RecentContextQuery recentQueryTemplate;
};

// 工具返回的单条结果。id 只是本次 run 内的句柄，不是数据库主键暴露。
struct MemoryAgentItem {
    QString id;                 // recent:<eventId> / summary:<interactionId> / claim:<claimId>
    QString kind;               // recent / summary / claim
    QString text;
    QDateTime time;
    QString source;             // current_group / allowed_group / internal
    QString groupLabel;         // 可展示的群名，不含原始群号
    QString personId;
    qlonglong rawEventId = -1;
    qlonglong interactionId = -1;
    qlonglong claimId = -1;
    QString sourceSessionId;    // 仅服务端内部使用，不进入工具结果
    QStringList matchedNeedIds; // 本次 run 内命中的 need，不暴露数据库 ID
    bool isViewer = false;
};

struct MemoryAgentResult {
    QList<MemoryAgentItem> items;
    QList<MemoryNeedResult> needs;
    bool truncated = false;
    QString status;             // ok / partial / timeout / failed / empty
};

struct MemoryAgentAudit {
    QString requestId;
    QString channel;
    QString tool;
    QString argsHash;           // 参数摘要，不存 query 原文
    int queryLength = 0;
    int resultCount = 0;
    int latencyMs = 0;
    QString status;             // ok / denied / invalid_arguments / error / timeout
};

struct MemoryAgentMetrics {
    int totalRuns = 0;
    int okRuns = 0;
    int failedRuns = 0;
    int timeoutRuns = 0;
    int deniedTools = 0;
    QVector<qint64> runLatenciesMs;

    void recordRun(bool ok, bool timedOut, qint64 latencyMs)
    {
        ++totalRuns;
        if (ok) {
            ++okRuns;
        } else {
            ++failedRuns;
        }
        if (timedOut) {
            ++timeoutRuns;
        }
        runLatenciesMs.append(latencyMs);
        if (runLatenciesMs.size() > 200) {
            runLatenciesMs.remove(0, runLatenciesMs.size() - 200);
        }
    }

    qint64 percentile(int p) const
    {
        if (runLatenciesMs.isEmpty()) {
            return 0;
        }
        QVector<qint64> sorted = runLatenciesMs;
        std::sort(sorted.begin(), sorted.end());
        const int index = qBound(0, (sorted.size() * p + 99) / 100 - 1,
                                 sorted.size() - 1);
        return sorted.at(index);
    }

    qint64 p50() const { return percentile(50); }
    qint64 p95() const { return percentile(95); }
};

Q_DECLARE_METATYPE(MemoryAgentResult)
Q_DECLARE_METATYPE(MemoryAgentAudit)

#endif // MEMORYAGENTTYPES_H
