#ifndef TIMEMANAGER_H
#define TIMEMANAGER_H

#include <QObject>
#include <QTimer>
#include <QTime>
#include <QMap>
#include <QString>

class TimeManager : public QObject
{
    Q_OBJECT
public:
    explicit TimeManager(QObject *parent = nullptr);
    void timeStart();

    void start();
    void stop();
    void setNightStart(const QTime &time);
    void setNightEnd(const QTime &time);
    void setBlushTime(const int time);
    void setBackIdleTime(const int time);
    void setDistanceTime(const int time);

    void notifyRoundEnded(const QMap<QString, QString> &finalTags);
    void notifyUserInputStarted();

signals:
    void clotheChanged(const QString &clothe);
    void blushingReset();
    void emotionReset();
    void distanceReset();
private slots:
    void onMinuteTick();

private:
    QTimer *m_timer;
    QTimer *m_blushResetTimer;
    QTimer *m_emotionResetTimer;
    QTimer *m_distanceResetTimer;

    QTime m_nightStart;
    QTime m_nightEnd;
    int m_blushTime;
    int m_BackIdleTime;
    int m_distanceTime;

    bool m_isNight;
};

#endif // TIMEMANAGER_H
