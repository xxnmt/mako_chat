#include "ttsservice.h"
#include "apittsprovider.h"
#include "configmanager.h"
#include "iaudiosink.h"

#include <QDebug>
#include <QTimer>

TTSService::TTSService(QObject *parent)
    : QObject(parent)
{
    reloadProvider();
    m_processManager = new TTSProcessManager(
        ConfigManager::instance().getPythonPath(),
        ConfigManager::instance().getGPTSovitsRootPath(),
        {"-a", "127.0.0.1", "-p", "9880"},
        9880,
        this);
    connect(m_processManager, &TTSProcessManager::apiReady, this, &TTSService::onApiReady);
    connect(m_processManager, &TTSProcessManager::apiFailed, this, &TTSService::onApiFailed);
    m_processManager->apiStart();
    qDebug() << "[TTS]:正在启动api";
}

void TTSService::setAudioSink(IAudioSink *sink)
{
    if (m_audioSink == sink) {
        return;
    }
    if (m_audioSink) {
        disconnect(m_audioSink, nullptr, this, nullptr);
    }
    m_audioSink = sink;
    if (m_audioSink) {
        connect(m_audioSink, &IAudioSink::sentenceStarted, this,
                [this](const SentenceText &sentence) {
                    emit playAudioAction(sentence.zhText, sentence.rawTags);
                });
        connect(m_audioSink, &IAudioSink::idle, this, &TTSService::checkPlaybackQueueEmpty);
    }
    qDebug() << "[TTS]:音频输出" << (m_audioSink ? "已接入（桌宠渠道）" : "未设置（只合成不播放）");
}

void TTSService::enqueueSentences(const QList<SentenceText> &sentences)
{
    for (const SentenceText &s : sentences) {
        m_ttsQueue.enqueue(s);
    }
    processTtsQueue();
}

void TTSService::reloadProvider()
{
    if (m_provider) {
        m_provider->deleteLater();
        m_provider = nullptr;
    }
    QString mode = ConfigManager::instance().getTTSMode();

    if (mode == "api") {
        m_provider = new ApiTTSProvider;
        qDebug() << "[TTS]:已加载ApiProvider";
        QTimer::singleShot(100, this, [this]() {
            if (m_provider) {
                qDebug() << "[TTS]:预热中";
                m_provider->warmUp();
            }
        });
    }
    if (mode == "mock") {
        qDebug() << "[TTS]:已加载MoceTTSProvider";
    }
    if (m_provider) {
        connect(m_provider, &ITTSProvider::synthesisFinished, this, &TTSService::onTtsFinished);
        connect(m_provider, &ITTSProvider::synthesisFailed, this, &TTSService::onTtsFailed);
    }
}

void TTSService::switchModel(const QString &gptPath, const QString &sovitsPath)
{
    if (ApiTTSProvider *apiProvider = qobject_cast<ApiTTSProvider *>(m_provider)) {
        apiProvider->switchModel(gptPath, sovitsPath);
    }
}

void TTSService::processTtsQueue()
{
    if (m_isSynthesizing || m_ttsQueue.isEmpty()) {
        return;
    }

    // API 未就绪：全部转入 pending，保持顺序
    if (!m_processManager->isApiReady()) {
        while (!m_ttsQueue.isEmpty()) {
            m_pendingSentences.enqueue(m_ttsQueue.dequeue());
        }
        static bool firstTime = true;
        if (firstTime) {
            if (m_audioSink) {
                QMap<QString, QString> tags;
                emit playAudioAction("茉子还在准备声音哦，再等一下啦...", tags);
            }
            firstTime = false;
            qDebug() << "[TTS]:api未完成启动，缓存本次请求";
        }
        return;
    }

    m_isSynthesizing = true;
    SentenceText currentSentence = m_ttsQueue.dequeue();
    m_lastSynthWasStream = m_provider->isStreamingMode();

    if (m_lastSynthWasStream) {
        disconnect(m_provider, &ITTSProvider::pcmDataReady, nullptr, nullptr);
        qDebug() << "[TTS]开始合成语音(日文流式):" << currentSentence.jaText;

        if (m_audioSink) {
            m_audioSink->beginStream(currentSentence, apiSampleRate());
            connect(m_provider, &ITTSProvider::pcmDataReady, this,
                    [this](const QByteArray &pcm) {
                        if (!m_audioSink) {
                            return;
                        }
                        // 首块到达时服务端 WAV 头已解析，用真实采样率校正播放器
                        m_audioSink->updateSampleRate(apiSampleRate());
                        m_audioSink->writePcm(pcm);
                    });
        }
    } else {
        qDebug() << "[TTS]开始合成语音(日文):" << currentSentence.jaText;
    }

    m_provider->synthesize(currentSentence);
}

void TTSService::onTtsFinished(const QString &audioPath, const SentenceText &sentence)
{
    qDebug() << "[TTS]合成成功:" << audioPath;
    m_isSynthesizing = false;

    if (m_audioSink) {
        if (m_lastSynthWasStream) {
            m_audioSink->endStream(sentence);
        } else {
            m_audioSink->playFile(audioPath, sentence, apiSampleRate());
        }
    }

    processTtsQueue();
    checkPlaybackQueueEmpty();
}

void TTSService::onTtsFailed(const QString &errorMsg, const SentenceText &sentence)
{
    qDebug() << "[TTS]合成失败，使用模拟替代:" << errorMsg;
    m_isSynthesizing = false;

    if (m_audioSink) {
        if (m_lastSynthWasStream) {
            m_audioSink->abortPendingStream();
        }
        m_audioSink->playFile(QString(), sentence);  // 空路径 = 模拟播放
    }

    processTtsQueue();
    checkPlaybackQueueEmpty();
}

void TTSService::onApiReady()
{
    m_isApiReady = true;
    qDebug() << "[TTS]:apiReady,正在推入缓存的未合成语音";
    QString gptPath = ConfigManager::instance().getGPTWeightsPath();
    QString sovitsPath = ConfigManager::instance().getSoVITSWeightsPath();
    if (!gptPath.isEmpty() || !sovitsPath.isEmpty()) {
        switchModel(gptPath, sovitsPath);
        qDebug() << "[TTS]:自动切换模型 GPT:" << gptPath << "SoVITS:" << sovitsPath;
    }
    processPendingSentences();
}

void TTSService::onApiFailed(const QString &error)
{
    qDebug() << "[TTS]:api启动失败:" << error;
    m_pendingSentences.clear();
    if (m_audioSink) {
        QMap<QString, QString> tags;
        tags.insert("emotion", "sad");
        emit playAudioAction("茉子的声音出问题了，待会儿再试吧...", tags);
    }
}

void TTSService::processPendingSentences()
{
    while (!m_pendingSentences.isEmpty()) {
        m_ttsQueue.enqueue(m_pendingSentences.dequeue());
    }
    processTtsQueue();
}

void TTSService::checkPlaybackQueueEmpty()
{
    if (m_ttsQueue.isEmpty() && m_pendingSentences.isEmpty() && !m_isSynthesizing
        && (!m_audioSink || m_audioSink->isIdle())) {
        qDebug() << "[TTS]:本轮对话已全部播放";
        emit playbackQueueEmpty();
    }
}

int TTSService::apiSampleRate() const
{
    if (auto *api = qobject_cast<ApiTTSProvider *>(m_provider)) {
        return api->getSampleRate();
    }
    return 24000;  // 兜底默认（非 ApiTTSProvider 路径）
}
