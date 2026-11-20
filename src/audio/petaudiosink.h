#ifndef PETAUDIOSINK_H
#define PETAUDIOSINK_H

#include <QQueue>
#include <QTimer>
#include <QString>

#include "iaudiosink.h"
#include "ipcmplayer.h"

// 桌宠渠道音频输出：拥有播放器与播放队列（原 TTSService 的播放半区）。
// 时序与拆分前保持一致：
//   - 首句：beginStream 时立即 start，首块 PCM 到达时通知 UI
//   - 后续句：PCM 先写入待播缓冲，上一句播完后起播并通知 UI
//   - 文件/模拟：入队后按序播放
class PetAudioSink : public IAudioSink
{
    Q_OBJECT
public:
    explicit PetAudioSink(QObject *parent = nullptr);
    ~PetAudioSink() override;

    void beginStream(const SentenceText &sentence, int sampleRate) override;
    void writePcm(const QByteArray &pcm) override;
    void updateSampleRate(int sampleRate) override;
    void endStream(const SentenceText &sentence) override;
    void abortPendingStream() override;

    void playFile(const QString &audioPath, const SentenceText &sentence, int sampleRate = 24000) override;

    bool isIdle() const override;
    void stop() override;

private slots:
    void processPlayQueue();
    void onPlayerFinished();
    void onPlayerError(const QString &error);
    void onMockFinished();

private:
    struct PlayItem {
        SentenceText sentence;
        QString audioPath;
        bool isStream = false;
        IPcmPlayer *streamPlayer = nullptr;
        int sampleRate = 24000;
    };

    void finishCurrent();

    QQueue<PlayItem> m_playQueue;
    IPcmPlayer *m_player = nullptr;              // 当前正在播放
    IPcmPlayer *m_pendingStreamPlayer = nullptr; // 正在合成、尚未排到播放的下一句
    SentenceText m_pendingSentence;
    int m_pendingSampleRate = 24000;

    bool m_isPlaying = false;
    bool m_pendingUiPending = false;             // 首句等首块 PCM 再通知 UI（保持旧时序）

    QTimer *m_mockTimer = nullptr;
    int m_mockSampleRate = 24000;
};

#endif // PETAUDIOSINK_H
