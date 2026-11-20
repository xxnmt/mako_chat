#pragma once

#include <QString>
#include "ttsreference.h"

class TTSReferenceManager
{
public:
    static const TTSReferenceManager &instance()
    {
        static const TTSReferenceManager instance;
        return instance;
    }

    static bool validateDirectory(const QString &directory, QString *error);
    TTSReference getReferenceForEmotion(const QString &emotion) const;

private:
    TTSReferenceManager() = default;
};
