#ifndef MEMORYTIMEFRAME_H
#define MEMORYTIMEFRAME_H

#include <QDate>
#include <QJsonObject>
#include <QString>

// 主模型输出的时间语义帧：只表达“哪一类时间”，不直接给 from/to。
// 服务端用 TimeFrameResolver 计算具体日期范围。
struct MemoryTimeFrame
{
    enum class Anchor {
        None,
        RelativeDay,
        RelativeWeek,
        RelativeMonth,
        AbsoluteDate,
        Festival,
        RecentWindow
    };
    enum class Occurrence {
        MostRecentPast,
        NextFuture,
        Year,
        YearOffset
    };
    enum class Span {
        Day,
        WeekContaining,
        SurroundingDays,
        MonthContaining
    };

    Anchor anchor = Anchor::None;
    int offset = 0;
    int weekday = 1;
    QString date;
    QString festival;
    Occurrence occurrence = Occurrence::MostRecentPast;
    int occurrenceValue = 0;
    Span span = Span::Day;
    int beforeDays = 0;
    int afterDays = 0;
    QString period; // morning / afternoon / evening / night

    bool isValid() const { return anchor != Anchor::None; }

    static bool fromJson(const QJsonObject &object,
                         MemoryTimeFrame *frame,
                         QString *error);
};

#endif // MEMORYTIMEFRAME_H
