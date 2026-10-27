#ifndef QQCHANNELADAPTER_H
#define QQCHANNELADAPTER_H

#include <QObject>
#include <QJsonArray>
#include "ichanneladapter.h"

class QQProtocolClient;

// QQ 渠道：OneBot v11 输出（文字；语音/图片后续扩展）
// 传输由 QQProtocolClient 负责，本类只负责把 Utterance 转成 send_private_msg / send_group_msg。
class QQChannelAdapter : public QObject, public IChannelAdapter
{
    Q_OBJECT
public:
    explicit QQChannelAdapter(QObject *parent = nullptr);

    void setClient(QQProtocolClient *client);
    QString channelName() const override;
    void sendUtterance(const Utterance &u) override;

    // 纯格式化：文字消息段（供单测/复用）
    static QJsonArray buildTextMessage(const QString &text);

private:
    QQProtocolClient *m_client = nullptr;  // 非所有权
};

#endif // QQCHANNELADAPTER_H
