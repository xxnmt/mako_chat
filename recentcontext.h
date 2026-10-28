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
    QStringList viewerIds; // 服务端解析出的当前用户平台 ID，用于标记“你”
    QString locationChannel;
    QString locationSessionId;
    QStringList allowedGroupIds;
    QString queryText;
    RecentContextMode mode = RecentContextMode::RecentOverview;
    QDateTime since;
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
