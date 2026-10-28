#include "memorypolicy.h"
#include "utterance.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace {

void addRuleId(QStringList &ids, const QString &id)
{
    if (!id.isEmpty() && !ids.contains(id)) {
        ids.append(id);
    }
}

// 返回 false 表示命中 block 规则；redacted 表示发生了替换。
bool sanitizeString(QString &value,
                    const QList<PrivacyRule> &rules,
                    QStringList &ruleIds,
                    bool &redacted)
{
    for (const PrivacyRule &rule : rules) {
        if (rule.value.isEmpty()) {
            continue;
        }

        const QString type = rule.type.toLower();
        const QString action = rule.action.toLower();
        const bool block = action == QStringLiteral("block");

        if (type == QStringLiteral("exact")) {
            if (!value.contains(rule.value)) {
                continue;
            }
            addRuleId(ruleIds, rule.id);
            if (block) {
                return false;
            }
            value.replace(rule.value, QStringLiteral("[REDACTED]"));
            redacted = true;
            continue;
        }

        QRegularExpression expression;
        if (type == QStringLiteral("keyword")) {
            expression = QRegularExpression(
                QRegularExpression::escape(rule.value),
                QRegularExpression::CaseInsensitiveOption);
        } else if (type == QStringLiteral("pattern")) {
            expression = QRegularExpression(rule.value);
            if (!expression.isValid()) {
                continue;
            }
        } else {
            continue;
        }

        if (!expression.match(value).hasMatch()) {
            continue;
        }
        addRuleId(ruleIds, rule.id);
        if (block) {
            return false;
        }
        value.replace(expression, QStringLiteral("[REDACTED]"));
        redacted = true;
    }
    return true;
}

QJsonValue sanitizeJsonValue(const QJsonValue &input,
                             const QList<PrivacyRule> &rules,
                             QStringList &ruleIds,
                             bool &redacted,
                             bool &blocked)
{
    if (input.isString()) {
        QString value = input.toString();
        if (!sanitizeString(value, rules, ruleIds, redacted)) {
            blocked = true;
            return QJsonValue();
        }
        return QJsonValue(value);
    }

    if (input.isObject()) {
        const QJsonObject source = input.toObject();
        QJsonObject result;
        for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
            const QJsonValue child = sanitizeJsonValue(it.value(), rules, ruleIds, redacted, blocked);
            if (blocked) {
                return QJsonValue();
            }
            result.insert(it.key(), child);
        }
        return result;
    }

    if (input.isArray()) {
        const QJsonArray source = input.toArray();
        QJsonArray result;
        for (const QJsonValue &childValue : source) {
            const QJsonValue child = sanitizeJsonValue(childValue, rules, ruleIds, redacted, blocked);
            if (blocked) {
                return QJsonValue();
            }
            result.append(child);
        }
        return result;
    }

    return input;
}

} // namespace

QString qqGroupMemoryModeToString(QQGroupMemoryMode mode)
{
    return mode == QQGroupMemoryMode::GroupAuthorized
               ? QStringLiteral("group_authorized")
               : QStringLiteral("strict_whitelist");
}

QQGroupMemoryMode qqGroupMemoryModeFromString(const QString &value, bool *ok)
{
    if (ok) {
        *ok = true;
    }
    if (value == QStringLiteral("group_authorized")) {
        return QQGroupMemoryMode::GroupAuthorized;
    }
    if (value == QStringLiteral("strict_whitelist")) {
        return QQGroupMemoryMode::StrictWhitelist;
    }
    if (ok) {
        *ok = false;
    }
    return QQGroupMemoryMode::StrictWhitelist;
}

QQInboundDecision QQInboundPolicyEngine::decide(
    const QQInboundMessage &message,
    const QQReplyPolicy &replyPolicy,
    const QQGroupMemoryConfig &groupMemory,
    const MemoryPolicyConfig &memoryPolicy,
    const QStringList &masterQqs)
{
    QQInboundDecision decision;
    if (!message.valid) {
        return decision;
    }

    const bool isOwner = masterQqs.contains(message.senderId);
    const QQReplyDecision replyDecision =
        QQReplyPolicyEngine::decide(message, replyPolicy, masterQqs);

    if (message.channel == Channel::kQQPrivate) {
        const bool allowlisted = replyPolicy.privateAllow.contains(message.senderId.toLongLong());
        if (!isOwner && !allowlisted) {
            // `all` 是显式兼容模式：允许回复，但不把非白名单内容落库。
            if (replyPolicy.privateMode == QStringLiteral("all")
                && replyDecision == QQReplyDecision::Reply) {
                decision.allowReply = true;
                decision.ephemeralReply = true;
            }
            return decision;
        }

        decision.persistRaw = memoryPolicy.longTermMemoryEnabled;
        decision.allowReply = replyDecision == QQReplyDecision::Reply;
        decision.persistChat = decision.allowReply && memoryPolicy.shortContextEnabled;
        decision.ephemeralReply = decision.allowReply && !decision.persistChat;
        return decision;
    }

    if (message.channel != Channel::kQQGroup) {
        return decision;
    }

    const QString groupId = message.sessionId;
    const qlonglong groupIdNumber = groupId.toLongLong();
    const bool groupOptedIn = groupMemory.enabled
                              && groupMemory.groupAllow.contains(groupIdNumber);
    const bool senderAllowed = isOwner
                               || groupMemory.senderAllowByGroup.value(groupId)
                                      .contains(message.senderId.toLongLong());
    const QQGroupMemoryMode mode = groupMemory.modeByGroup.value(
        groupId, QQGroupMemoryMode::StrictWhitelist);

    // strict：只有 owner / sender allowlist 有记录资格。
    // authorized：群级 opt-in 后，非白名单成员也有记录资格，但没有回复资格。
    const bool recordEligible =
        mode == QQGroupMemoryMode::StrictWhitelist
            ? senderAllowed
            : (senderAllowed || groupOptedIn);

    decision.persistRaw = groupOptedIn
                          && recordEligible
                          && memoryPolicy.longTermMemoryEnabled;
    decision.allowReply = senderAllowed
                          && replyDecision == QQReplyDecision::Reply;
    decision.persistChat = decision.allowReply && memoryPolicy.shortContextEnabled;
    decision.ephemeralReply = decision.allowReply && !decision.persistChat;
    return decision;
}

SanitizeResult SensitiveContentFilter::sanitize(
    const QString &text,
    const QString &payload,
    const QList<PrivacyRule> &rules)
{
    SanitizeResult result;
    result.text = text;
    result.payload = payload;

    if (rules.isEmpty()) {
        return result;
    }

    bool textRedacted = false;
    if (!sanitizeString(result.text, rules, result.ruleIds, textRedacted)) {
        result.blocked = true;
        result.text.clear();
        result.payload.clear();
        return result;
    }
    result.redacted = textRedacted;

    if (payload.isEmpty()) {
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError
        && (document.isObject() || document.isArray())) {
        bool payloadRedacted = false;
        bool payloadBlocked = false;
        const QJsonValue root = document.isObject()
                                    ? QJsonValue(document.object())
                                    : QJsonValue(document.array());
        const QJsonValue cleaned = sanitizeJsonValue(
            root, rules, result.ruleIds, payloadRedacted, payloadBlocked);

        if (payloadBlocked) {
            result.payload.clear();
            result.payloadDropped = true;
            result.redacted = true;
            return result;
        }

        if (payloadRedacted) {
            const QJsonDocument cleanedDocument = cleaned.isObject()
                                                       ? QJsonDocument(cleaned.toObject())
                                                       : QJsonDocument(cleaned.toArray());
            result.payload = QString::fromUtf8(
                cleanedDocument.toJson(QJsonDocument::Compact));
            result.redacted = true;
        }
        return result;
    }

    // payload 不是可安全重写的 JSON：命中规则就丢弃整个 payload。
    QString payloadText = payload;
    bool payloadRedacted = false;
    if (!sanitizeString(payloadText, rules, result.ruleIds, payloadRedacted)
        || payloadRedacted) {
        result.payload.clear();
        result.payloadDropped = true;
        result.redacted = true;
    }
    return result;
}
