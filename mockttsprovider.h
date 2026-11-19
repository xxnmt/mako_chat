#ifndef MOCKTTSPROVIDER_H
#define MOCKTTSPROVIDER_H

#include <QObject>
#include <QTimer>
#include "ittsprovider.h"

class MockTTSProvider : public ITTSProvider
{
    Q_OBJECT
public:
    explicit MockTTSProvider(QObject *parent = nullptr);
    void synthesize(const SentenceText &sentence) override;
    void warmUp()override;
    bool isStreamingMode() const override { return false; }
private slots:
    void onMockSynthesisFinished();

private:
    SentenceText m_currentSentence;
};

#endif // MOCKTTSPROVIDER_H
