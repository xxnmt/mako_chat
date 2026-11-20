#ifndef TTSREFERENCEMANAGER_H
#define TTSREFERENCEMANAGER_H

#include <QString>
#include <QJsonObject>
#include "ttsreference.h"


class TTSReferenceManager
{
public:
    static TTSReferenceManager& instance() {
        static TTSReferenceManager instance;
        return instance;
    }
    bool loadMap();
    TTSReference getReferenceForEmotion(const QString &emotion)const;
private:
    TTSReferenceManager();
    ~TTSReferenceManager() = default;

    QJsonObject m_mapRoot;

};

#endif // TTSREFERENCEMANAGER_H
