#include "globalmoodstate.h"
#include "utterance.h"

#include <QDebug>

GlobalMoodState::GlobalMoodState(QObject *parent)
    : QObject(parent)
{
}

void GlobalMoodState::setFollowExternalMood(bool follow)
{
    m_followExternalMood = follow;
}

void GlobalMoodState::applyTags(const QMap<QString, QString> &tags, const QString &sourceChannel)
{
    QMap<QString, QString> moodTags;

    if (tags.contains("emotion") && !tags.value("emotion").isEmpty()) {
        m_emotion = tags.value("emotion");
        moodTags.insert("emotion", m_emotion);
    }
    if (tags.contains("blush") && !tags.value("blush").isEmpty()) {
        m_blush = tags.value("blush");
        moodTags.insert("blush", m_blush);
    }
    if (tags.contains("distance") && !tags.value("distance").isEmpty()) {
        m_distance = tags.value("distance");
        moodTags.insert("distance", m_distance);
    }

    if (moodTags.isEmpty()) {
        return;
    }

    const bool fromPet = (sourceChannel == Channel::kPet);
    if (!fromPet && m_followExternalMood) {
        emit externalMoodApplied(moodTags);
    }

    qDebug() << "[GlobalMood]情绪更新 source=" << sourceChannel
             << "emotion=" << m_emotion << "blush=" << m_blush << "distance=" << m_distance;
}

QString GlobalMoodState::getCurrentStateDescription(const QString &clothing) const
{
    return QString("距离:%1\n服装:%2\n脸红状态:%3\n当前表情:%4")
        .arg(m_distance)
        .arg(clothing)
        .arg(m_blush)
        .arg(m_emotion);
}
