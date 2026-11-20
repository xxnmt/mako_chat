#ifndef FILEPLAYER_H
#define FILEPLAYER_H

#include "ipcmplayer.h"
#include <QAudioSink>
#include <QFile>
#include <QTimer>
#include <QBuffer>

class FilePlayer : public IPcmPlayer
{
    Q_OBJECT
public:
    explicit FilePlayer(QObject *parent = nullptr);
    ~FilePlayer();
    void startPlayer(int sampleRate,int channels,int sampleBits)override;
    void stopPlayer()override;
    void writePcm(const QByteArray &pcmData)override;
    bool getisPlaying()const override;
    void setSource(const QString &filePath) override;

private slots:
    void onStateChanged(QAudio::State state);
    void pushData();
private:
    bool m_playbackStarted = false;
    QAudioSink *m_audioSink = nullptr;
    QFile m_file;
    bool m_isplaying = false;
    QTimer *m_timer;
    QBuffer *m_buffer = nullptr;
    int m_sampleRate=24000;
    int m_channels=1;
    int m_bitsPerSample=16;
    static const int CHUNK_SIZE = 4096;
};

#endif // FILEPLAYER_H
