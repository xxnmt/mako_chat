#ifndef INTERACTIONSEGMENTER_H
#define INTERACTIONSEGMENTER_H

#include <QList>

#include "historyturn.h"
#include "memorypolicy.h"

// 纯分段逻辑：时间窗口 + 显式 reply 关联，不依赖数据库/网络。
class InteractionSegmenter
{
public:
    static QList<InteractionDraft> segment(const QList<RawEvent> &events,
                                           const InteractionConfig &config);
};

#endif // INTERACTIONSEGMENTER_H
