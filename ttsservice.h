#ifndef TTSSERVICE_H
#define TTSSERVICE_H

#include <QObject>
#include <QQueue>

#include "sentencedata.h"
#include "ittsprovider.h"
#include "ttsprocessmanager.h"

class IAudioSink;

// TTS 合成服务：只负责「句子 → 音频」的合成与排队。
// 播放（播放器、播放队列、气泡/立绘时序）由渠道侧的 IAudioSink 承担：
//   - 桌宠：PetAudioSink（真实播放）
//   - QQ：不设置 sink（只合成不播放）
class TTSService : public QObject
{
    Q_OBJECT
public:
    explicit TTSService(QObject *parent = nullptr);

    // 接收 LLM 解析完毕的多句话
    void enqueueSentences(const QList<SentenceText> &sentences);

    void reloadProvider();
    void switchModel(const QString &gptPath, const QString &sovitsPath);

    // 设置/替换音频输出（桌宠渠道在装配时注入；为空则只合成不播放）
    void setAudioSink(IAudioSink *sink);
    IAudioSink *audioSink() const { return m_audioSink; }

signals:
    // 通知中枢更新 UI（仅在有渠道 sink 时发出）
    void playAudioAction(const QString &zhText, const QMap<QString, QString> &tags);

    void playbackQueueEmpty();

private slots:
    // TTS 生成线程
    void processTtsQueue();
    void onTtsFinished(const QString &audioPath, const SentenceText &sentence);
    void onTtsFailed(const QString &errorMsg, const SentenceText &sentence);
    // ttsProcess
    void onApiReady();
    void onApiFailed(const QString &error);
    void processPendingSentences();

private:
    void checkPlaybackQueueEmpty();
    int apiSampleRate() const;  // 当前 Provider 的实际采样率（流式路径为 ApiTTSProvider）

    // ttsProcessApi
    TTSProcessManager *m_processManager = nullptr;
    QQueue<SentenceText> m_pendingSentences;
    bool m_isApiReady = false;

    // 合成队列
    QQueue<SentenceText> m_ttsQueue;

    // 状态锁
    bool m_isSynthesizing = false;
    bool m_lastSynthWasStream = false;

    ITTSProvider *m_provider = nullptr;   // 当前 TTS 抽象策略
    IAudioSink *m_audioSink = nullptr;    // 非所有权（由渠道持有）
};

#endif // TTSSERVICE_H
