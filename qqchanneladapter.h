#ifndef QQCHANNELADAPTER_H
#define QQCHANNELADAPTER_H

#include <QObject>
#include "ichanneladapter.h"

// QQ 渠道占位（Phase 1 实现：经 OneBot v11 反向 WebSocket 收发 NapCat）
// Phase 1 计划：
//   - QWebSocket 连接 ws://127.0.0.1:3001（access_token 鉴权）
//   - 消息事件(message.private/friend_add...) → SessionRouter.submitInput
//   - sendUtterance → send_private_msg / send_group_msg
//   - 群聊滑动窗口 + 门控接入（见 docs/HUMANIZATION_PLAN.md 第七节）
class QQChannelAdapter : public QObject, public IChannelAdapter
{
    Q_OBJECT
public:
    explicit QQChannelAdapter(QObject *parent = nullptr);

    QString channelName() const override;
    void sendUtterance(const Utterance &u) override;
};

#endif // QQCHANNELADAPTER_H
