#include "memoryoutputguard.h"

#include <QRegularExpression>

OutputGuardResult MemoryOutputGuard::check(const QString &text,
                                           const RequestMeta &meta,
                                           const QList<PrivacyRule> &rules,
                                           const QStringList &forbiddenFacts)
{
    OutputGuardResult result;
    if (text.isEmpty()) {
        return result;
    }

    const SanitizeResult scanned = SensitiveContentFilter::sanitize(
        text, QString(), rules);
    if (scanned.blocked || scanned.redacted) {
        result.blocked = true;
        result.ruleIds = scanned.ruleIds;
        result.reason = QStringLiteral("privacy_rule");
        return result;
    }

    for (const QString &fact : forbiddenFacts) {
        if (fact.isEmpty()) {
            continue;
        }
        if (text.contains(fact, Qt::CaseInsensitive)) {
            result.blocked = true;
            result.ruleIds.append(QStringLiteral("forbidden_fact"));
            result.reason = QStringLiteral("forbidden_fact");
            return result;
        }
    }

    // 群聊中不允许输出明显的 QQ 号形式；不做宽泛数字匹配，避免误伤时间/价格。
    if (meta.channel == Channel::kQQGroup) {
        static const QRegularExpression qqIdPattern(
            QStringLiteral("\\bqq:\\d{5,}\\b"),
            QRegularExpression::CaseInsensitiveOption);
        if (qqIdPattern.match(text).hasMatch()) {
            result.blocked = true;
            result.ruleIds.append(QStringLiteral("identity:qq"));
            result.reason = QStringLiteral("identity_exposure");
            return result;
        }
    }

    return result;
}
