#ifndef RECENTCONTEXT_H
#define RECENTCONTEXT_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QDateTime>

enum class RecentContextMode {
    TargetedLookup,
    RecentOverview
};

struct RecentContextQuery {
    QString viewerId;
    QStringList viewerIds; // 兼容旧调用：当前用户平台 ID
    QString viewerPersonId; // 服务端解析后的当前用户 person_id
    bool onlyViewer = false; // 跨群只返回 viewer 自己的消息
    QString locationChannel;
    QString locationSessionId;
    QStringList allowedGroupIds;
    QString queryText;
    RecentContextMode mode = RecentContextMode::RecentOverview;
    QDateTime since;
    QDateTime until; // 可选半开区间上界，用于“下午/昨天”等时间检索
    int perGroupCandidateLimit = 50;
    int maxGroups = 2;
    int itemLimit = 10;
    int itemLimitOverview = 50;
    int charBudget = 1200;
    int overviewCharBudget = 2200;
    int hotWindowItems = 10;
    int warmLineMaxChars = 24;
};

struct RecentContextItem {
    qlonglong rawEventId = -1;
    QString sourceGroupId;
    QString senderId;
    QString senderDisplayName;
    QString personId;
    QDateTime occurredAt;
    QString sanitizedText;
    QString messageId;
    QString replyToMessageId;
    double lexicalScore = 0.0;
    bool warm = false;
    bool isViewer = false;
};

struct RecentContextBundle {
    QList<RecentContextItem> items;
    RecentContextMode mode = RecentContextMode::RecentOverview;
    bool truncated = false;
    int candidatesScanned = 0;
};

class RecentContextPlanner
{
public:
    static RecentContextMode detectMode(const QString &queryText);
    static QStringList extractTerms(const QString &queryText);
    static bool isRecentCue(const QString &queryText);
    static bool isHistoricalCue(const QString &queryText);
    static QStringList allowedGroupIdsForRequest(
        bool isGroup,
        bool ownerPrivateOrPet,
        const QString &sessionId,
        bool groupMemoryEnabled,
        const QList<qlonglong> &groupAllow);
    static bool shouldFallbackToOverview(RecentContextMode mode,
                                         bool bundleEmpty,
                                         bool isGroup,
                                         bool ownerPrivateOrPet);
    static RecentContextBundle build(const QList<RecentContextItem> &candidates,
                                     const RecentContextQuery &query);
};

#endif // RECENTCONTEXT_H
