#ifndef IAUDIOSINK_H
#define IAUDIOSINK_H

#include <QObject>
#include <QByteArray>
#include <QString>

#include "sentencedata.h"

// 音频输出抽象：TTSService 只负责「合成」，把音频交给具体输出端。
// 桌宠渠道实现 PetAudioSink（真实播放 + 气泡/立绘时序）；QQ 渠道不设置 sink（只合成不播放）。
class IAudioSink : public QObject
{
    Q_OBJECT
public:
    explicit IAudioSink(QObject *parent = nullptr) : QObject(parent) {}
    ~IAudioSink() override = default;

    // 流式合成：一句开始 / 写入 PCM / 采样率校正 / 合成结束
    virtual void beginStream(const SentenceText &sentence, int sampleRate) = 0;
    virtual void writePcm(const QByteArray &pcm) = 0;
    virtual void updateSampleRate(int sampleRate) = 0;
    virtual void endStream(const SentenceText &sentence) = 0;
    virtual void abortPendingStream() = 0;

    // 文件型 / 模拟播放（audioPath 为空表示模拟）
    virtual void playFile(const QString &audioPath, const SentenceText &sentence, int sampleRate = 24000) = 0;

    virtual bool isIdle() const = 0;
    virtual void stop() = 0;

signals:
    // 真正开始播放某一句（桌宠用于同步气泡 + 立绘）
    void sentenceStarted(const SentenceText &sentence);
    // 队列排空、当前无播放
    void idle();
};

#endif // IAUDIOSINK_H
