#include "timemanager.h"

TimeManager::TimeManager(QObject *parent)
    : QObject{parent}
{
    m_nightStart = QTime(22, 0);
    m_nightEnd = QTime(7, 0);
    m_blushTime = 30;
    m_BackIdleTime = 15;
    m_distanceTime = 15;
    m_isNight = false;

    m_timer = new QTimer(this);
    m_timer->setInterval(60000);
    connect(m_timer, &QTimer::timeout, this, &TimeManager::onMinuteTick);

    m_blushResetTimer = new QTimer(this);
    m_blushResetTimer->setSingleShot(true);
    connect(m_blushResetTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[TimeManager]:脸红退火";
        emit blushingReset();
    });

    m_emotionResetTimer = new QTimer(this);
    m_emotionResetTimer->setSingleShot(true);
    connect(m_emotionResetTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[TimeManager]:表情退火";
        emit emotionReset();
    });

    m_distanceResetTimer = new QTimer(this);
    m_distanceResetTimer->setSingleShot(true);
    connect(m_distanceResetTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[TimeManager]:距离退火";
        emit distanceReset();
    });
}

void TimeManager::start()
{
    m_timer->start();
    onMinuteTick();
    qDebug() << "[TimerManager]:已启动，夜间时间为" << m_nightStart << "-" << m_nightEnd;
    qDebug() << "[TimerManager]:脸红持续" << m_blushTime << " 表情持续" << m_BackIdleTime << " 距离持续" << m_distanceTime;
}

void TimeManager::stop()
{
    if (m_timer) {
        m_timer->stop();
    }
    if (m_blushResetTimer) {
        m_blushResetTimer->stop();
    }
    if (m_emotionResetTimer) {
        m_emotionResetTimer->stop();
    }
    if (m_distanceResetTimer) {
        m_distanceResetTimer->stop();
    }
}

void TimeManager::setNightStart(const QTime &time)
{
    m_nightStart = time;
}

void TimeManager::setNightEnd(const QTime &time)
{
    m_nightEnd = time;
}

void TimeManager::setBlushTime(const int time)
{
    m_blushTime = time;
}

void TimeManager::setBackIdleTime(const int time)
{
    m_BackIdleTime = time;
}

void TimeManager::setDistanceTime(const int time)
{
    m_distanceTime = time;
}

void TimeManager::notifyRoundEnded(const QMap<QString, QString> &finalTags)
{
    // 每轮对话结束，先取消所有退火，再按最终状态启动对应退火定时器
    m_blushResetTimer->stop();
    m_emotionResetTimer->stop();
    m_distanceResetTimer->stop();

    if (finalTags.value("blush") == "blushing") {
        m_blushResetTimer->start(m_blushTime * 1000);
    }
    if (finalTags.value("emotion") != "happyIdle") {
        m_emotionResetTimer->start(m_BackIdleTime * 1000);
    }
    if (finalTags.value("distance") == "closer") {
        m_distanceResetTimer->start(m_distanceTime * 1000);
    }
}

void TimeManager::notifyUserInputStarted()
{
    // 用户发新消息，取消所有退火
    m_blushResetTimer->stop();
    m_emotionResetTimer->stop();
    m_distanceResetTimer->stop();
}

void TimeManager::onMinuteTick()
{
    // 白天黑夜切换
    QTime now = QTime::currentTime();
    bool isNight;
    if (m_nightStart <= m_nightEnd) {
        isNight = (now >= m_nightStart && now < m_nightEnd);
    } else {
        isNight = (now >= m_nightStart || now < m_nightEnd);
    }
    if (isNight != m_isNight) {
        m_isNight = isNight;
        QString clothes = isNight ? "pajama" : "schoolUniform";
        qDebug() << "[TimeManager]:时段切换，服装变为" << clothes;
        emit clotheChanged(clothes);
    }
}
