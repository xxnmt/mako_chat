#ifndef SENTENCEDATA_H
#define SENTENCEDATA_H

#include <QString>
#include <QMap>
#include <QList>
#include <QMetaType>

struct SentenceText
{
    SentenceText() {}

    QString zhText;
    QString jaText;
    QMap<QString,QString> rawTags;
    bool isValidated=false;
    QString originalJSON;

};

Q_DECLARE_METATYPE(SentenceText)
Q_DECLARE_METATYPE(QList<SentenceText>)
#endif // SENTENCEDATA_H
