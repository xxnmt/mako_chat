#ifndef TTSREFERENCE_H
#define TTSREFERENCE_H

#include <QString>

struct TTSReference {
    QString audioFilePath;
    QString promptText;
    QString promptLanguage;
};
#endif // TTSREFERENCE_H
