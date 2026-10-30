#ifndef MEMORYTIMERANGE_H
#define MEMORYTIMERANGE_H

#include <QDateTime>
#include <QString>

// 从中文时间表达解析出的半开区间 [from, to)。
// 仅用于服务端检索，不允许模型直接指定时间或权限范围。
struct MemoryTimeRange {
    QDateTime from;
    QDateTime to;
    QString label;
    bool valid = false;
};

class MemoryTimeRangePlanner
{
public:
    static MemoryTimeRange parse(const QString &query,
                                 const QDateTime &now = QDateTime::currentDateTime());
    static bool hasExplicitCue(const QString &query,
                               const QDateTime &now = QDateTime::currentDateTime());
};

#endif // MEMORYTIMERANGE_H
