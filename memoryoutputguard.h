#ifndef MEMORYOUTPUTGUARD_H
#define MEMORYOUTPUTGUARD_H

#include <QString>
#include <QStringList>

#include "memorypolicy.h"
#include "utterance.h"

struct OutputGuardResult {
    bool blocked = false;
    QStringList ruleIds;
    QString reason; // 仅用于日志，不包含原文
};

class MemoryOutputGuard
{
public:
    static OutputGuardResult check(const QString &text,
                                   const RequestMeta &meta,
                                   const QList<PrivacyRule> &rules,
                                   const QStringList &forbiddenFacts = {});
};

#endif // MEMORYOUTPUTGUARD_H
