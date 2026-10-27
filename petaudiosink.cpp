#include "petaudiosink.h"

#include <QDebug>

PetAudioSink::PetAudioSink(QObject *parent)
    : IAudioSink(parent)
{
    m_mockTimer = new QTimer(this);
    m_mockTimer->setSingleShot(true);
    connect(m_mockTimer, &QTimer::timeout, this, &PetAudioSink::onMockFinished);
}

PetAudioSink::~PetAudioSink()
{
    stop();
}

bool PetAudioSink::isIdle() const
{
    return !m_isPlaying && m_playQueue.isEmpty() && m_pendingStreamPlayer == nullptr;
}

void PetAudioSink::beginStream(const SentenceText &sentence, int sampleRate)
{
    if (m_pendingStreamPlayer) {
        // 防御：上一句尚未收尾就开始了下一句
        abortPendingStream();
    }

    m_pendingSentence = sentence;
    m_pendingSampleRate = sampleRate;
    m_pendingStreamPlayer = IPcmPlayer::create(IPcmPlayer::Stream, this);

    if (!m_isPlaying && m_playQueue.isEmpty()) {
        // 首句：立即起播，首块 PCM 到达时再通知 UI（与拆分前一致）
        m_player = m_pendingStreamPlayer;
        connect(m_player, &IPcmPlayer::PcmPlayerFinished, this, &PetAudioSink::onPlayerFinished);
        connect(m_player, &IPcmPlayer::PcmPlayerError, this, &PetAudioSink::onPlayerError);
        m_player->startPlayer(sampleRate, 1, 16);
        m_isPlaying = true;
        m_pendingUiPending = true;
        qDebug() << "[PetAudio]流式起播:" << sentence.zhText;
    }
}

void PetAudioSink::writePcm(const QByteArray &pcm)
{
    IPcmPlayer *target = m_pendingStreamPlayer ? m_pendingStreamPlayer : m_player;
    if (!target) {
        return;
    }

    if (m_pendingUiPending && target == m_player) {
        m_pendingUiPending = false;
        emit sentenceStarted(m_pendingSentence);
    }
    target->writePcm(pcm);
}

void PetAudioSink::updateSampleRate(int sampleRate)
{
    if (m_pendingStreamPlayer) {
        m_pendingStreamPlayer->updateSampleRate(sampleRate);
    } else if (m_player) {
        m_player->updateSampleRate(sampleRate);
    }
}

void PetAudioSink::endStream(const SentenceText &sentence)
{
    if (!m_pendingStreamPlayer) {
        return;
    }

    m_pendingStreamPlayer->setSynthesisDone();

    if (m_pendingStreamPlayer != m_player) {
        PlayItem item;
        item.sentence = sentence;
        item.isStream = true;
        item.streamPlayer = m_pendingStreamPlayer;
        item.sampleRate = m_pendingSampleRate;
        m_playQueue.enqueue(item);
        m_pendingStreamPlayer = nullptr;
        processPlayQueue();
    } else {
        // 当前句：播放器会在播放结束时回调
        m_pendingStreamPlayer = nullptr;
    }
}

void PetAudioSink::abortPendingStream()
{
    if (!m_pendingStreamPlayer) {
        return;
    }

    IPcmPlayer *pending = m_pendingStreamPlayer;
    m_pendingStreamPlayer = nullptr;
    m_pendingUiPending = false;

    if (pending == m_player) {
        disconnect(pending, nullptr, this, nullptr);
        pending->stopPlayer();
        pending->deleteLater();
        m_player = nullptr;
        m_isPlaying = false;
    } else {
        pending->deleteLater();
    }
}

void PetAudioSink::playFile(const QString &audioPath, const SentenceText &sentence, int sampleRate)
{
    PlayItem item;
    item.sentence = sentence;
    item.audioPath = audioPath;
    item.isStream = false;
    item.sampleRate = sampleRate;
    m_playQueue.enqueue(item);
    processPlayQueue();
}

void PetAudioSink::processPlayQueue()
{
    if (m_isPlaying || m_playQueue.isEmpty()) {
        return;
    }

    PlayItem item = m_playQueue.dequeue();
    m_isPlaying = true;

    if (item.isStream) {
        m_player = item.streamPlayer;
        connect(m_player, &IPcmPlayer::PcmPlayerFinished, this, &PetAudioSink::onPlayerFinished);
        connect(m_player, &IPcmPlayer::PcmPlayerError, this, &PetAudioSink::onPlayerError);
        m_player->startPlayer(item.sampleRate, 1, 16);
        emit sentenceStarted(item.sentence);
        qDebug() << "[PetAudio]开始流式播放:" << item.sentence.zhText;
        return;
    }

    if (item.audioPath.isEmpty()) {
        // MockTTSProvider：按日文字数模拟播放时长
        m_mockSampleRate = 24000;
        int playDuration = qMax(1000, item.sentence.jaText.length() * 250);
        emit sentenceStarted(item.sentence);
        qDebug() << "[PetAudio]模拟播放:" << item.sentence.zhText << playDuration << "ms";
        m_mockTimer->start(playDuration);
        return;
    }

    m_player = IPcmPlayer::create(IPcmPlayer::File, this);
    connect(m_player, &IPcmPlayer::PcmPlayerFinished, this, &PetAudioSink::onPlayerFinished);
    connect(m_player, &IPcmPlayer::PcmPlayerError, this, &PetAudioSink::onPlayerError);
    m_player->startPlayer(item.sampleRate, 1, 16);  // FilePlayer 会从 WAV 头自行校正
    m_player->setSource(item.audioPath);
    emit sentenceStarted(item.sentence);
    qDebug() << "[PetAudio]开始文件播放:" << item.sentence.zhText;
}

void PetAudioSink::onPlayerFinished()
{
    finishCurrent();
}

void PetAudioSink::onPlayerError(const QString &error)
{
    qDebug() << "[PetAudio]播放错误:" << error;
    finishCurrent();
}

void PetAudioSink::onMockFinished()
{
    finishCurrent();
}

void PetAudioSink::finishCurrent()
{
    if (m_mockTimer) {
        m_mockTimer->stop();
    }

    if (m_player) {
        disconnect(m_player, nullptr, this, nullptr);
        m_player->stopPlayer();
        m_player->deleteLater();
        m_player = nullptr;
    }
    m_isPlaying = false;

    if (!m_playQueue.isEmpty()) {
        processPlayQueue();
        return;
    }
    if (!m_pendingStreamPlayer) {
        emit idle();
    }
}

void PetAudioSink::stop()
{
    if (m_mockTimer) {
        m_mockTimer->stop();
    }
    abortPendingStream();

    while (!m_playQueue.isEmpty()) {
        PlayItem item = m_playQueue.dequeue();
        if (item.streamPlayer) {
            item.streamPlayer->stopPlayer();
            item.streamPlayer->deleteLater();
        }
    }

    if (m_player) {
        disconnect(m_player, nullptr, this, nullptr);
        m_player->stopPlayer();
        m_player->deleteLater();
        m_player = nullptr;
    }
    m_isPlaying = false;
}
