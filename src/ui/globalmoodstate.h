#ifndef GLOBALMOODSTATE_H
#define GLOBALMOODSTATE_H

#include <QObject>
#include <QMap>
#include <QString>

// 全局情绪状态（HUMANIZATION_PLAN D2）：emotion / blush / distance 是跨渠道的“她现在的情绪”，
// 立绘表现仍是桌宠渠道私有。externalMoodApplied 只在「外部渠道情绪且 followExternalMood=true」时发出，
// 由 AppController 决定是否驱动桌宠立绘（D1）；退火仍只归桌宠渠道（TimeManager）。
class GlobalMoodState : public QObject
{
    Q_OBJECT
public:
    explicit GlobalMoodState(QObject *parent = nullptr);

    // D1：外部渠道情绪是否驱动桌宠立绘（默认 true，来自 identity.json [appearance]）
    void setFollowExternalMood(bool follow);
    bool followExternalMood() const { return m_followExternalMood; }

    // 应用情绪标签；sourceChannel 为 Channel::kPet 或外部渠道
    void applyTags(const QMap<QString, QString> &tags, const QString &sourceChannel);

    QString getEmotion() const { return m_emotion; }
    QString getBlush() const { return m_blush; }
    QString getDistance() const { return m_distance; }

    // 与 AppearanceManager::getCurrentStateDescription() 同格式（服装由桌宠外观层提供）
    QString getCurrentStateDescription(const QString &clothing) const;

signals:
    // 外部渠道情绪需要驱动桌宠立绘时发出（仅含 emotion/blush/distance）
    void externalMoodApplied(const QMap<QString, QString> &moodTags);

private:
    QString m_emotion = QStringLiteral("happyIdle");
    QString m_blush = QStringLiteral("unblushing");
    QString m_distance = QStringLiteral("far");
    bool m_followExternalMood = true;
};

#endif // GLOBALMOODSTATE_H
