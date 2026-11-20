#include "appearancemanager.h"

#include <QDebug>

AppearanceManager::AppearanceManager(QObject *parent)
    : QObject{parent}
{
    setDefault();
}

void AppearanceManager::applyTags(const QMap<QString, QString> &tags)
{
    if(tags.contains("distance")){
        m_distance = tags.value("distance");
    }
    if(tags.contains("clothing")){
        m_clothing = tags.value("clothing");
    }
    if(tags.contains("emotion")){
        m_emotion = tags.value("emotion");
    }
//timeManager已有自动退火机制
    // //脸红更新 (加入退热机制;如果本句没有显式要求脸红，自动帮她退红，防止茉子一直红着脸)
    if (tags.contains("blush")) {
        m_blush = tags.value("blush");
    }
    // else {
    //     m_blush = "unblushing";
    // }

    checkPathAndUpdate();
}

QString AppearanceManager::getPath() const
{
    return QString(":/image/%1/%2/%3/%4.png")
        .arg(m_distance)
        .arg(m_clothing)
        .arg(m_blush)
        .arg(m_emotion);
}

void AppearanceManager::setDefault()
{
    m_distance = "far";
    m_clothing = "schoolUniform";
    m_blush    = "unblushing";
    m_emotion  = "happyIdle";
    m_lastPath = getPath();
}

QString AppearanceManager::getCurrentStateDescription() const
{
    return QString("距离:%1\n服装:%2\n脸红状态:%3\n当前表情:%4")
        .arg(m_distance)
        .arg(m_clothing)
        .arg(m_blush)
        .arg(m_emotion);
}

QString AppearanceManager::getDistance() const
{
    return m_distance;
}

QString AppearanceManager::getClothing() const
{
    return m_clothing;
}

QString AppearanceManager::getBlush() const
{
    return m_blush;
}

QString AppearanceManager::getEmotion() const
{
    return m_emotion;
}

void AppearanceManager::checkPathAndUpdate()
{
    QString newPath = getPath();
    if (newPath != m_lastPath) {
        m_lastPath = newPath;
        emit characterPathChanged(newPath);
    }
}
