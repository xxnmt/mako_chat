#include "petchanneladapter.h"
#include "petaudiosink.h"
#include "bubblewidget.h"
#include <QDebug>

PetChannelAdapter::PetChannelAdapter(BubbleWidget *bubble, QObject *parent)
    : QObject(parent), m_bubble(bubble)
{
    m_audioSink = new PetAudioSink(this);
}

QString PetChannelAdapter::channelName() const
{
    return Channel::kPet;
}

IAudioSink *PetChannelAdapter::audioSink() const
{
    return m_audioSink;
}

void PetChannelAdapter::sendUtterance(const Utterance &u)
{
    // 桌宠渠道目前只有文本气泡一种表达
    if (m_bubble) {
        m_bubble->showMessage(u.text);
    }
    qDebug() << "[PetChannel]气泡显示:" << u.text;
}

void PetChannelAdapter::onAudioReady(const QString &zhText, const QMap<QString, QString> &tags)
{
    Q_UNUSED(tags);  // tags 由全局情绪层消费（AppController::onPlayAudioAction），渠道只管气泡
    if (m_bubble) {
        m_bubble->showMessage(zhText);
    }
    qDebug() << "[PetChannel]播放跟随气泡:" << zhText;
}
