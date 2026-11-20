#ifndef TIMEFRAMERESOLVER_H
#define TIMEFRAMERESOLVER_H

#include <QDateTime>
#include <QString>

#include "memoryrecalltypes.h"
#include "memorytimeframe.h"
#include "memorytimeresolver.h"

// 结构化时间帧解析：模型只给语义，服务端计算日期和时间范围。
// frame 无效时回退到旧 MemoryTimeResolver（legacy 兼容）。
class TimeFrameResolver
{
public:
    static MemoryResolvedTime resolve(const QString &expression,
                                      MemoryTimeTarget target,
                                      const MemoryTimeFrame &frame,
                                      const QDateTime &anchor,
                                      const QString &timezone);
};

#endif // TIMEFRAMERESOLVER_H
