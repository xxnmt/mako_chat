#pragma once

#include "configmanager.h"

// Snapshot the configured knowledge boundary once per read, including worker reads.
class MemoryReadPolicy
{
public:
    MemoryReadPolicy()
        : m_rules(ConfigManager::instance().getMemoryPolicyConfig().privacyRules),
          m_blindZones(ConfigManager::instance().blindZones()) {}

    bool allowsSession(const QString &channel, const QString &session) const
    {
        for (const auto &zone : m_blindZones)
            if (zone.channel == channel && zone.sessionId == session)
                return false;
        return true;
    }

    bool allows(const QString &channel, const QString &session, const QString &text) const
    {
        if (!allowsSession(channel, session))
            return false;
        const auto scan = SensitiveContentFilter::sanitize(text, {}, m_rules);
        return !scan.blocked && !scan.redacted;
    }

    static QString rawMessagePredicate()
    {
        return QStringLiteral("direction='in' AND event_type='message' AND sensitive=0 "
                              "AND redaction_status='none' AND content IS NOT NULL AND content<>''");
    }

private:
    QList<PrivacyRule> m_rules;
    QList<BlindZone> m_blindZones;
};
