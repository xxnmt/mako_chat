#ifndef ITTSPROVIDER_H
#define ITTSPROVIDER_H
#include <QObject>
#include <QString>
#include "sentencedata.h"

class ITTSProvider:public QObject
{
    Q_OBJECT
public:
    explicit ITTSProvider(QObject *parent=nullptr):QObject(parent){}
    virtual ~ITTSProvider()=default;

    virtual void synthesize(const SentenceText &sentence)=0;
    virtual void warmUp()=0;
    virtual bool isStreamingMode()const=0;

signals:
    void pcmDataReady(const QByteArray &pcmChunk);
    void synthesisFinished(const QString &audioPath, const SentenceText &sentence);
    void synthesisFailed(const QString &audioPath, const SentenceText &sentence);
};

#endif // ITTSPROVIDER_H
