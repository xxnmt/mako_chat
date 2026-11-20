#include "ttsservice.h"
#include "apittsprovider.h"
#include "mockttsprovider.h"
#include "configmanager.h"
#include "iaudiosink.h"

#include <QDebug>
#include <QTimer>

TTSService::TTSService(QObject *parent)
    : QObject(parent)
{
    reloadProvider();

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
    const auto &config = ConfigManager::instance();
    const QStringList loadedConfig{config.getTTSMode(), config.getPythonPath(), config.getGPTSovitsRootPath()};
    const QStringList weights{config.getGPTWeightsPath(), config.getSoVITSWeightsPath()};
    if (m_provider && loadedConfig == m_loadedConfig && !m_apiFailed) {
        if (weights != m_loadedWeights && m_isApiReady)
            switchModel(weights[0], weights[1]);
        m_loadedWeights = weights;
        return;
    }
    m_loadedConfig = loadedConfig;
    m_loadedWeights = weights;
    const quint64 generation = ++m_providerGeneration;
    if (m_isSynthesizing) m_ttsQueue.prepend(m_currentSentence);
    if (m_audioSink) m_audioSink->abortPendingStream();
    if (m_provider) {
        disconnect(m_provider, nullptr, this, nullptr);
        delete m_provider;
        m_provider = nullptr;
    }
    if (m_processManager) {
        disconnect(m_processManager, nullptr, this, nullptr);
        delete m_processManager;
        m_processManager = nullptr;
    }
    m_isSynthesizing = false;
    m_lastSynthWasStream = false;
    m_isApiReady = false;
    m_apiFailed = false;
    const QString mode = ConfigManager::instance().getTTSMode();
    if (mode == "api") {
        m_provider = new ApiTTSProvider(this);
        m_processManager = new TTSProcessManager(
            ConfigManager::instance().getPythonPath(),
            ConfigManager::instance().getGPTSovitsRootPath(),
            {"-a", "127.0.0.1", "-p", "9880"}, 9880, this);
        connect(m_processManager, &TTSProcessManager::apiReady, this, &TTSService::onApiReady);
        connect(m_processManager, &TTSProcessManager::apiFailed, this, &TTSService::onApiFailed);
    } else {
        m_provider = new MockTTSProvider(this);
    }
    connect(m_provider, &ITTSProvider::synthesisFinished, this,
            [this, generation](const QString &path, const SentenceText &sentence) {
        if (generation == m_providerGeneration) onTtsFinished(path, sentence);
    });
    connect(m_provider, &ITTSProvider::synthesisFailed, this,
            [this, generation](const QString &error, const SentenceText &sentence) {
        if (generation == m_providerGeneration) onTtsFailed(error, sentence);
    });
    if (m_processManager) m_processManager->apiStart();
    processPendingSentences();
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
    if (m_processManager && !m_isApiReady && !m_apiFailed) {
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
    m_currentSentence = currentSentence;
    m_lastSynthWasStream = m_provider->isStreamingMode();

    if (m_lastSynthWasStream) {
        disconnect(m_provider, &ITTSProvider::pcmDataReady, nullptr, nullptr);
        qDebug() << "[TTS]开始合成语音(日文流式):" << currentSentence.jaText;

        if (m_audioSink) {
            m_audioSink->beginStream(currentSentence, apiSampleRate());
            const quint64 generation = m_providerGeneration;
            connect(m_provider, &ITTSProvider::pcmDataReady, this,
                    [this, generation](const QByteArray &pcm) {
                        if (!m_audioSink || generation != m_providerGeneration) {
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

    if (m_apiFailed) {
        QTimer::singleShot(0, m_provider, [this, currentSentence] {
            onTtsFailed(QStringLiteral("TTS service unavailable"), currentSentence);
        });
    } else {
        m_provider->synthesize(currentSentence);
    }
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
    m_apiFailed = false;
    m_provider->warmUp();
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
    m_isApiReady = false;
    m_apiFailed = true;
    processPendingSentences();
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
