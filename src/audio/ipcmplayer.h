#ifndef IPCMPLAYER_H
#define IPCMPLAYER_H

#include <QObject>
#include <QByteArray>

class IPcmPlayer :public QObject{
    Q_OBJECT
public:
    enum Type { Stream, File };
    static IPcmPlayer* create(Type type, QObject *parent = nullptr);

    explicit IPcmPlayer(QObject *parent=nullptr):QObject(parent){}
    ~IPcmPlayer()=default;

    virtual void startPlayer(int sampleRate,int channels,int sampleBits)=0;
    virtual void stopPlayer() = 0;
    virtual void writePcm(const QByteArray &pcmData) = 0;
    virtual bool getisPlaying() const = 0;
    virtual void setSynthesisDone() {}  // 流式合成全部完成，提示播放器检测播放结束
    virtual void setSource(const QString &path) {}  // 文件型播放器加载音频源
    virtual void updateSampleRate(int sampleRate) {}  // 采样率校正（流式播放器重写，默认空实现）
signals:
    void PcmPlayerFinished();
    void PcmPlayerError(const QString &error);

    };

#endif // IPCMPLAYER_H
