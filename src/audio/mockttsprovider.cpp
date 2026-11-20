#include "mockttsprovider.h"
#include <QDebug>
MockTTSProvider::MockTTSProvider(QObject *parent)
    : ITTSProvider{parent}
{}

void MockTTSProvider::synthesize(const SentenceText &sentence)
{
    m_currentSentence = sentence;

    // 模拟合成耗时：根据日文字数计算，每个字假定需要 100ms 推理时间
    int synthesisTime = qMax(500, sentence.jaText.length() * 100);
    qDebug() << "[MockTTS] 开始模拟合成等待:" << synthesisTime << "ms";

    QTimer::singleShot(synthesisTime, this, &MockTTSProvider::onMockSynthesisFinished);
}

void MockTTSProvider::warmUp()
{
    qDebug()<<"[MockTTS]:无预热";
}

void MockTTSProvider::onMockSynthesisFinished()
{
    emit synthesisFinished("", m_currentSentence);
}
