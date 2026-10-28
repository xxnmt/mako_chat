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

struct MemoryPolicyConfig {
    bool shortContextEnabled = true;
    bool longTermMemoryEnabled = true;
    QList<PrivacyRule> privacyRules;
    QString policyVersion = QStringLiteral("r3");
};

struct QQGroupMemoryConfig {
    bool enabled = false;
    QList<qlonglong> groupAllow;
    QMap<QString, QQGroupMemoryMode> modeByGroup;
    QMap<QString, QList<qlonglong>> senderAllowByGroup;
    int retentionDays = 30;
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
