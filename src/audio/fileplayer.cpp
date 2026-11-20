#include "fileplayer.h"
#include <QAudioFormat>
#include <QDebug>


FilePlayer::FilePlayer(QObject *parent)
{
    m_isplaying=false;
    m_timer=new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &FilePlayer::pushData);

}

FilePlayer::~FilePlayer()
{
    stopPlayer();
}

void FilePlayer::startPlayer(int sampleRate, int channels, int sampleBits)
{
    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(channels);
    fmt.setSampleFormat(sampleBits == 16 ? QAudioFormat::Int16 : QAudioFormat::Int32);

    m_audioSink = new QAudioSink(fmt, this);
    connect(m_audioSink, &QAudioSink::stateChanged, this, &FilePlayer::onStateChanged);
    m_buffer= new QBuffer(this);
    m_buffer->open(QIODevice::ReadWrite);
    m_audioSink->start(m_buffer);
    m_isplaying = true;
    qDebug()<<"[filePlayer]PCM播放器已启动:";
}

void FilePlayer::stopPlayer()
{
    m_timer->stop();
    m_playbackStarted=false;
    if (m_file.isOpen()){
        m_file.close();
    }
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
    if (m_buffer) {
        m_buffer->close();
        delete m_buffer;
        m_buffer = nullptr;
    }
    m_isplaying = false;
    qDebug()<<"[filePlayer]PCM播放器已关闭:";

}

void FilePlayer::writePcm(const QByteArray &pcmData)
{
    qDebug()<<"[FilePlayer]:直接读取文件，无需写入pcm";
}

bool FilePlayer::getisPlaying() const
{
    return m_isplaying;
}

void FilePlayer::setSource(const QString &filePath)
{
    if (m_file.isOpen())
        m_file.close();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        qDebug()<<"[filePlayer]无法打开文件:"<<filePath;
        emit PcmPlayerError("无法打开文件: " + filePath);
        return;
    }
    if(m_file.size()>=44){
        QByteArray header=m_file.read(44);
        if(header.startsWith("RIFF")&&header.mid(8,4)=="WAVE"){
            quint32 sr=*reinterpret_cast<const quint32*>(header.constData()+24);
            quint16 channels=*reinterpret_cast<const quint16*>(header.constData()+22);
            quint16 bitsPerSample=*reinterpret_cast<const quint16*>(header.constData()+34);
            if(sr>0){
                qDebug()<<"[filePlayer]WAV采样率:"<<sr<<"通道:"<<channels<<"位深:"<<bitsPerSample;
                if(sr!=m_sampleRate || channels!=m_channels || bitsPerSample!=m_bitsPerSample){
                    m_sampleRate=sr;
                    m_channels=channels;
                    m_bitsPerSample=bitsPerSample;
                    if(m_audioSink){
                        m_audioSink->stop();
                        delete m_audioSink;
                        m_audioSink=nullptr;
                    }
                    if(m_buffer){
                        m_buffer->close();
                        delete m_buffer;
                        m_buffer=nullptr;
                    }
                    QAudioFormat fmt;
                    fmt.setSampleRate(m_sampleRate);
                    fmt.setChannelCount(m_channels);
                    fmt.setSampleFormat(m_bitsPerSample==16?QAudioFormat::Int16:QAudioFormat::Int32);
                    m_audioSink = new QAudioSink(fmt, this);
                    connect(m_audioSink, &QAudioSink::stateChanged,this, &FilePlayer::onStateChanged);
                    m_buffer = new QBuffer(this);
                    m_buffer->open(QIODevice::ReadWrite);
                    m_audioSink->start(m_buffer);
                }
            }
        }
    }
    m_file.seek(44);
    m_playbackStarted=true;

    // 关键：预填充数据到m_buffer，避免QAudioSink启动时buffer为空导致吞开头
    // 此时QAudioSink已在startPlayer中start但处于IdleState（buffer为空）
    QByteArray preload = m_file.read(CHUNK_SIZE * 4);  // 预填充较大块
    if (!preload.isEmpty() && m_buffer) {
        m_buffer->write(preload);
        m_buffer->seek(0);  // 重置读取位置
    }
    if (m_audioSink && m_audioSink->state() == QAudio::IdleState) {
        m_audioSink->resume();
    }

    m_timer->start(10);
    qDebug()<<"[filePlayer]开始播放:"<<filePath<<"预填充字节="<<preload.size();

}

void FilePlayer::onStateChanged(QAudio::State state)
{
    qDebug() << "[FilePlayer] 音频状态:" << state;
    if (state == QAudio::IdleState) {
        if (m_playbackStarted&&!m_file.isOpen() && !m_timer->isActive()) {
            m_isplaying = false;
            m_playbackStarted = false;
            emit PcmPlayerFinished();
        }
    }
    else if (state == QAudio::StoppedState) {
        if(m_playbackStarted){
        m_isplaying = false;
        m_playbackStarted=false;
        emit PcmPlayerError("音频播放异常停止");
        }
    }
}

void FilePlayer::pushData()
{
    if (!m_isplaying || !m_audioSink ||!m_buffer|| !m_file.isOpen())
        return;

    QByteArray data = m_file.read(CHUNK_SIZE);
    if (data.isEmpty()) {
        m_timer->stop();
        m_file.close();
        return;
    }
    // 保存QAudioSink的读取位置，seek到末尾追加写入，再恢复读取位置
    qint64 readPos = m_buffer->pos();
    m_buffer->seek(m_buffer->size());
    qint64 written = m_buffer->write(data);
    m_buffer->seek(readPos);
    if(written<0){
        qDebug()<<"[FilePlayer]:写入m_buffer失败";
        emit PcmPlayerError("写入音频缓冲区失败");
        m_timer->stop();
    }
    if (m_audioSink->state() == QAudio::IdleState) {
        m_audioSink->resume();
    }
}

