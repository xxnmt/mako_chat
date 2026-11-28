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
// 主模型只能提供 query/kinds/source_hint/time.expression，不能覆盖
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
    // 仅对 Owner QQ 私聊生效：允许读取自己的 pet/private 记忆。
    bool ownerCrossChannelPrivate = false;
    // tool 模式下有 time 的 need 必须提供结构化 frame。
    bool requireTimeFrame = false;
    // 仅用于审计/回复边界，不包含敏感 ID。
    QString accessScope;
    bool otherChannelsVisible = false;
    QDateTime referenceTime;
    QDateTime now() const { return referenceTime.isValid() ? referenceTime : QDateTime::currentDateTime(); }
    QString timezone;
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
    QString speaker;            // 可读说话人
    QString speakerRole;        // user / assistant / other / unknown
    QString derived;            // original / assistant_expression / summary / inference
    qlonglong rawEventId = -1;
    qlonglong interactionId = -1;
    qlonglong claimId = -1;
    qlonglong episodicId = -1;
    qlonglong longTermSummaryId = -1;
    qlonglong historyTurnId = -1;
    QString sourceSessionId;    // 仅服务端内部使用，不进入工具结果
    QStringList matchedNeedIds; // 本次 run 内命中的 need，不暴露数据库 ID
    bool isViewer = false;
};

struct MemoryAgentResult {
    QString requestId;
    QList<MemoryAgentItem> items;
    QList<MemoryNeedResult> needs;
    bool truncated = false;
    QString accessScope;
    bool otherChannelsVisible = false;
    // complete / partial / empty / failed / timeout（timeout 仅 legacy）
    QString status;
};


Q_DECLARE_METATYPE(MemoryAgentResult)

#endif // MEMORYAGENTTYPES_H
