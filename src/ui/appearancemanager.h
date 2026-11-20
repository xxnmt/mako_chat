#ifndef APPEARANCEMANAGER_H
#define APPEARANCEMANAGER_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QObject>

class AppearanceManager : public QObject
{
    Q_OBJECT
public:
    explicit AppearanceManager(QObject *parent = nullptr);

    void applyTags(const QMap<QString,QString> &tags);
    QString getPath()const;
    void setDefault();

    QString getCurrentStateDescription() const;
    QString getDistance() const;
    QString getClothing() const;
    QString getBlush() const;
    QString getEmotion() const;

signals:
    void characterPathChanged(const QString &newPath);
private:
    void checkPathAndUpdate();
    //立绘四维状态数据
    QString m_distance;
    QString m_clothing;
    QString m_blush;
    QString m_emotion;

    QString m_lastPath;
};

#endif // APPEARANCEMANAGER_H
