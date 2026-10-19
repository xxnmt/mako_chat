#ifndef ICHANNELADAPTER_H
#define ICHANNELADAPTER_H

#include <QString>
#include "utterance.h"

// 渠道适配器接口：所有对话渠道（桌宠/QQ私聊/群聊/QQ空间）的统一出口
// 大脑侧（AppController/LLMService）只产出 Utterance，不感知具体渠道
class IChannelAdapter
{
public:
    virtual ~IChannelAdapter() = default;

    // 渠道名（与 Utterance::channel / DB channel 列取值一致）
    virtual QString channelName() const = 0;

    // 消费一句话：渠道私有表现（气泡/IM发送/语音消息等）
    // 立绘与情绪联动是全局层（AppController），不属于任何渠道
    virtual void sendUtterance(const Utterance &u) = 0;
};

#endif // ICHANNELADAPTER_H
