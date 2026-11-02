#ifndef FESTIVALCALENDAR_H
#define FESTIVALCALENDAR_H

#include <QDate>
#include <QString>

// 公共节日数据（calendar/festivals.json）。节日名称/日期是数据，不是
// Resolver 里的 if/else；新增节日不需要改解析代码。
class FestivalCalendar
{
public:
    // name 可以是中文别名；成功时返回稳定 festival id 和公历日期。
    static bool resolve(const QString &name, int year,
                        QDate *date, QString *festivalId = nullptr);
    static QString version();
};

#endif // FESTIVALCALENDAR_H
