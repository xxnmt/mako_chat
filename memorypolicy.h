#ifndef MEMORYPOLICY_H
#define MEMORYPOLICY_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>

#include "qqmessageparser.h"
#include "qqreplypolicy.h"

enum class QQGroupMemoryMode {
    StrictWhitelist,
    GroupAuthorized
};

QString qqGroupMemoryModeToString(QQGroupMemoryMode mode);
QQGroupMemoryMode qqGroupMemoryModeFromString(const QString &value, bool *ok = nullptr);

struct PrivacyRule {
    QString id;
    QString type;       // exact / keyword / pattern
    QString value;
    QString action;     // redact / block
};

struct InteractionConfig {
    int groupIdleMinutes = 15;
    int groupBatchSize = 50;
    int gapSeconds = 90;
    int maxEvents = 20;
    int maxMinutes = 10;
};

struct RecentContextConfig {
    bool enabled = true;
    int maxAgeMinutes = 30;
    int candidateLimitPerGroup = 50;
    int maxOwnerGroups = 2;
    int targetedItemLimit = 10;
    int targetedCharBudget = 1200;
    int overviewItemLimit = 50;
    int overviewCharBudget = 2200;
    int hotWindowItems = 10;
    int warmLineMaxChars = 24;
};

struct MemoryPolicyConfig {
    bool shortContextEnabled = true;
    bool longTermMemoryEnabled = true;
    QList<PrivacyRule> privacyRules;
    QString policyVersion = QStringLiteral("r3");
    InteractionConfig interaction;
    RecentContextConfig recentContext;
};

struct QQGroupMemoryConfig {
    bool enabled = false;
    QList<qlonglong> groupAllow;
    QMap<QString, QQGroupMemoryMode> modeByGroup;
    QMap<QString, QList<qlonglong>> senderAllowByGroup;
    int retentionDays = 30;
    int membershipRefreshMinutes = 30;
    int membershipMaxAgeMinutes = 1440;
};

struct QQInboundDecision {
    bool persistRaw = false;
    bool persistChat = false;
    bool allowReply = false;
    bool ephemeralReply = false;
};

// 纯逻辑入站策略：记录资格、回复资格、短期上下文、长期记忆分别裁决。
class QQInboundPolicyEngine
{
public:
    static QQInboundDecision decide(const QQInboundMessage &message,
                                    const QQReplyPolicy &replyPolicy,
                                    const QQGroupMemoryConfig &groupMemory,
                                    const MemoryPolicyConfig &memoryPolicy,
                                    const QStringList &masterQqs);
};

struct SanitizeResult {
    bool blocked = false;
    bool redacted = false;
    bool payloadDropped = false;
    QString text;
    QString payload;
    QStringList ruleIds;
};

// 本地 exact / keyword / pattern 脱敏；不宣称语义级隐私识别。
class SensitiveContentFilter
{
public:
    static SanitizeResult sanitize(const QString &text,
                                   const QString &payload,
                                   const QList<PrivacyRule> &rules);
};

#endif // MEMORYPOLICY_H
