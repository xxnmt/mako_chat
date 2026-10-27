#ifndef PETCHANNELADAPTER_H
#define PETCHANNELADAPTER_H

#include <QObject>
#include <QMap>
#include "ichanneladapter.h"

class BubbleWidget;
class PetAudioSink;
class IAudioSink;

// 桌宠渠道：气泡文字显示 + 语音播放（渠道私有表现层）
// 注意：立绘/情绪联动是全局层（AppController::onPlayAudioAction），不在此类内。
class PetChannelAdapter : public QObject, public IChannelAdapter
{
    Q_OBJECT
public:
    explicit PetChannelAdapter(BubbleWidget *bubble, QObject *parent = nullptr);

    QString channelName() const override;
    void sendUtterance(const Utterance &u) override;

    // 桌宠音频输出（由 AppController 注入 TTSService）
    IAudioSink *audioSink() const;

public slots:
    // TTSService::playAudioAction 直连入口：播放跟随气泡（保持现状时序）
    void onAudioReady(const QString &zhText, const QMap<QString, QString> &tags);

private:
    BubbleWidget *m_bubble;   // 非所有权，由 UI 层管理生命周期
    PetAudioSink *m_audioSink; // 本渠道所有
};

#endif // PETCHANNELADAPTER_H
