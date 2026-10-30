#ifndef MEMORYTIMERESOLVER_H
#define MEMORYTIMERESOLVER_H

#include <QDateTime>
#include <QString>

#include "memoryrecalltypes.h"

struct MemoryResolvedTime {
    QDateTime from;
    QDateTime to;
    QString timezone;
    QString originalExpression;
    QString interpretation; // exact / previous_completed_period / ambiguous / unresolved
    MemoryTimeTarget target = MemoryTimeTarget::MessageTime;
    bool valid = false;
    QString error;
};

// 独立时间解析器：每次只解析一个时间表达，不判断整句里哪个表达优先。
class MemoryTimeResolver
{
public:
    static MemoryResolvedTime resolve(const QString &expression,
                                      MemoryTimeTarget target,
                                      const QDateTime &anchor,
                                      const QString &timezone);
};

#endif // MEMORYTIMERESOLVER_H
