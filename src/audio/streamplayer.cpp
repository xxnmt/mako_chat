#include "streamplayer.h"
#include <qdebug>


StreamPlayer::StreamPlayer(QObject *parent)
{
    m_audioSink=nullptr;
    m_buffer=nullptr;
    m_endTimer=nullptr;
    m_isplaying=false;
    m_isSynthesisDone=false;
    m_lastProcessedUsecs=0;
    m_endCheckCount=0;
    m_timer=new QTimer(this);
    connect(m_timer,&QTimer::timeout,this,&StreamPlayer::pushData);
}

StreamPlayer::~StreamPlayer()
{
    stopPlayer();
}

void StreamPlayer::startPlayer(int sampleRate, int channels, int sampleBits)
{
    QAudioFormat audioFormat;
    audioFormat.setSampleRate(sampleRate);
    audioFormat.setChannelCount(channels);
    audioFormat.setSampleFormat(sampleBits==16?QAudioFormat::Int16:QAudioFormat::Int32);
    m_sampleRate = sampleRate;

    m_audioSink=new QAudioSink(audioFormat,this);
    connect(m_audioSink,&QAudioSink::stateChanged,this,&StreamPlayer::onStateChanged);

    m_buffer=new QBuffer(this);
    m_buffer->open(QIODevice::ReadWrite);

    // 关键：先把队列中已有的PCM数据填入buffer，再启动QAudioSink
    // 避免QAudioSink启动时buffer为空进入IdleState，导致开头数据被吞
    QMutexLocker locker(&m_mutex);
    QByteArray allData;
    while (!m_queue.isEmpty()) {
        allData.append(m_queue.dequeue());
    }
    locker.unlock();
    if (!allData.isEmpty()) {
        m_buffer->write(allData);
        m_buffer->seek(0);  // 重置读取位置到开头，供QAudioSink读取
    }

    m_audioSink->start(m_buffer);
    m_timer->start(20);
    m_isplaying=true;
    m_playbackStarted=true;
    // 注意：m_isSynthesisDone 不在此处重置，因为 setSynthesisDone 可能先于 startPlayer 被调用
    m_lastProcessedUsecs=0;
    m_endCheckCount=0;

    // 播放结束兜底检测定时器
    if (!m_endTimer) {
        m_endTimer = new QTimer(this);
        connect(m_endTimer, &QTimer::timeout, this, &StreamPlayer::checkPlayEnd);
    }
    m_endTimer->start(500);

    qDebug()<<QString("[StreamPlayer]开始播放:采样率:%1;通道:%2;位深:%3 预填充字节=%4")
                    .arg(sampleRate).arg(channels).arg(sampleBits).arg(allData.size());
}

void StreamPlayer::stopPlayer()
{
    if (m_endTimer) {
        m_endTimer->stop();
    }
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
    if(m_buffer){
        m_buffer->close();
        delete m_buffer;
        m_buffer=nullptr;
    }
    QMutexLocker locker(&m_mutex);
    m_queue.clear();
    m_isplaying = false;
    m_playbackStarted = false;
    m_isSynthesisDone = false;
    m_endCheckCount = 0;
}

void StreamPlayer::updateSampleRate(int sampleRate)
{
    if (sampleRate <= 0 || sampleRate == m_sampleRate) return;  // 无变化则不重建
    m_sampleRate = sampleRate;
    qDebug() << "[StreamPlayer]采样率变化，重建QAudioSink:" << sampleRate;
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
    QAudioFormat fmt;
    fmt.setSampleRate(m_sampleRate);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);
    m_audioSink = new QAudioSink(fmt, this);
    connect(m_audioSink, &QAudioSink::stateChanged, this, &StreamPlayer::onStateChanged);
    // 首块PCM到达时校正，此时buffer为空，直接以新采样率启动不会丢数据
    m_audioSink->start(m_buffer);
}

void StreamPlayer::writePcm(const QByteArray &pcmData)
{
    QMutexLocker locker(&m_mutex);
    m_queue.enqueue(pcmData);
}

void StreamPlayer::setSynthesisDone()
{
    QMutexLocker locker(&m_mutex);
    m_isSynthesisDone = true;
    m_endCheckCount = 0;
    m_lastProcessedUsecs = 0;
    qDebug()<<"[StreamPlayer]setSynthesisDone, queueSize="<<m_queue.size();
}

bool StreamPlayer::getisPlaying() const
{
    return m_isplaying;
}

void StreamPlayer::checkPlayEnd()
{
    if (!m_playbackStarted || !m_audioSink) return;
    if (!m_isSynthesisDone) return;  // 合成还在继续，不检测

    QMutexLocker locker(&m_mutex);
    bool queueEmpty = m_queue.isEmpty();
    bool bufferDone = m_buffer ? (m_buffer->size() == m_buffer->pos()) : false;

    qint64 currentUsecs = m_audioSink ? m_audioSink->processedUSecs() : 0;
    qint64 bytesFree = m_audioSink ? m_audioSink->bytesFree() : 0;
    qint64 bufferSize = m_audioSink ? m_audioSink->bufferSize() : 0;
    bool deviceEmpty = (currentUsecs > 0 && currentUsecs == m_lastProcessedUsecs)
                        || (bufferSize > 0 && bytesFree >= bufferSize);

    if (queueEmpty && bufferDone && deviceEmpty) {
        m_endCheckCount++;
        if (m_endCheckCount >= 3) {   // 连续3次(1.5秒)满足，判定播放完
            locker.unlock();
            qDebug()<<"[StreamPlayer]兜底检测触发, this="<<(void*)this;
            finishAndEmit();
            return;
        }
    } else {
        m_endCheckCount = 0;
    }
    m_lastProcessedUsecs = currentUsecs;
}

void StreamPlayer::finishAndEmit()
{
    if (!m_playbackStarted) return;
    if (m_timer) m_timer->stop();
    if (m_endTimer) m_endTimer->stop();
    m_isplaying = false;
    m_playbackStarted = false;
    qDebug()<<"[StreamPlayer]emit PcmPlayerFinished, this="<<(void*)this;
    emit PcmPlayerFinished();
}

void StreamPlayer::onStateChanged(QAudio::State state)
{
    qDebug()<<"[StreamPlayer]音频状态:"<<state;
    if (state == QAudio::IdleState) {
        QMutexLocker locker(&m_mutex);
        bool queueEmpty = m_queue.isEmpty();
        bool bufferDone = m_buffer ? (m_buffer->size() == m_buffer->pos()) : false;

        // 只在合成完成且数据都已消费时才发射完成信号
        if (m_playbackStarted && queueEmpty && bufferDone && m_isSynthesisDone) {
            locker.unlock();
            finishAndEmit();
        }
    }
    else if (state == QAudio::StoppedState) {
        if(m_playbackStarted){
            if(m_timer) m_timer->stop();
            if(m_endTimer) m_endTimer->stop();
            m_isplaying = false;
            m_playbackStarted=false;
            emit PcmPlayerError("流式播放异常停止");
        }
    }
}

void StreamPlayer::pushData()
{
    if (!m_isplaying || !m_audioSink){
        return;
    }
    QMutexLocker locker(&m_mutex);
    if (m_queue.isEmpty()) {
        return;
    }

    QByteArray allData;
    while (!m_queue.isEmpty()) {
        allData.append(m_queue.dequeue());
    }
    locker.unlock();

    if(!allData.isEmpty()){
        qint64 readPos = m_buffer->pos();
        m_buffer->seek(m_buffer->size());
        m_buffer->write(allData);
        m_buffer->seek(readPos);
    }

    if (m_audioSink && m_audioSink->state() == QAudio::IdleState) {
        m_audioSink->resume();
    }
}
