#ifndef STREAMPLAYER_H
#define STREAMPLAYER_H

#include "ipcmplayer.h"
#include <QAudioSink>
#include <QBuffer>
#include <QTimer>
#include <QAudioFormat>
#include <QQueue>
#include <QMutex>
class StreamPlayer : public IPcmPlayer
{
    Q_OBJECT
public:
    explicit StreamPlayer(QObject *parent=nullptr);
    ~StreamPlayer();

    void startPlayer(int sampleRate,int channels,int sampleBits)override;
    void stopPlayer()override;
    void writePcm(const QByteArray &pcmData)override;
    bool getisPlaying()const override;
    void setSynthesisDone() override;  // 标记流式合成全部完成
    void updateSampleRate(int sampleRate) override;  // 采样率校正：真实采样率到达后重建QAudioSink
    // 注意：PcmPlayerFinished 和 PcmPlayerError 信号继承自 IPcmPlayer，不可在此重复声明
    // 否则 moc 会生成两个不同信号，导致 connect 到基类信号但 emit 子类信号，槽函数收不到

private slots:
    void onStateChanged(QAudio::State state);
    void pushData();
    void checkPlayEnd();   // 兜底检测播放结束
private:
    void finishAndEmit();  // 内部：停止并发射完成信号
    private:
    QAudioSink *m_audioSink=nullptr;
    QBuffer *m_buffer=nullptr;
    QTimer *m_timer=nullptr;
    QTimer *m_endTimer=nullptr;   // 合成结束后的播放完成兜底检测定时器
    QQueue<QByteArray> m_queue;
    bool m_isplaying=false;
    bool m_playbackStarted = false;
    bool m_isSynthesisDone = false;  // 合成是否已结束（所有数据都到了）
    qint64 m_lastProcessedUsecs = 0; // 上次检测到的已播放微秒数
    int m_endCheckCount = 0;         // 连续N次未前进则判定播放完
    int m_sampleRate = 24000;        // 当前QAudioSink配置的采样率，用于判断是否需要重建
    mutable QMutex m_mutex;


};

#endif // STREAMPLAYER_H
