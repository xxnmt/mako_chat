#ifndef UTTERANCE_H
#define UTTERANCE_H

#include <QString>
#include <QMap>

// 渠道常量（与 DB 的 channel 列、config 的 [memory] blindZones 取值保持一致）
namespace Channel {
    inline const QString kPet           = QStringLiteral("pet");        // 桌宠
    inline const QString kQQPrivate     = QStringLiteral("qq_private"); // QQ 好友私聊
    inline const QString kQQGroup       = QStringLiteral("qq_group");   // QQ 群聊
    inline const QString kQZone         = QStringLiteral("qzone");      // QQ 空间
    inline const QString kDefaultSession = QStringLiteral("default");   // 桌宠会话固定值
}

// 输入侧请求元数据：随 askDeepSeek 走完整链路，直至记忆写入
struct RequestMeta {
    QString channel = Channel::kPet;            // 渠道
    QString sessionId = Channel::kDefaultSession; // 桌宠恒 default；QQ=对方QQ号/群号
    QString speaker;                            // 桌宠="欧尼酱"；群聊=对方昵称
    QString subject = QStringLiteral("user");   // 身份解析结果（'user' 即主人，IdentityResolver 产出）
    bool sensitive = false;                     // 记忆盲区标记（渠道侧查配置后设置，MemoryManager 保持中立）
    QString turnKind = QStringLiteral("reply"); // 轮次类型：reply / proactive / perception
    QString triggerEventIds;                    // 触发事件 id 列表（JSON 数组，指向 raw_events.id）
};

// 输出侧：一句话 + 路由元数据（由 SentenceText 在大脑侧一次映射，渠道层协议）
struct Utterance {
    QString text;                   // 展示文本（原 zh_text）
    QString ttsText;                // 语音目标（原 ja_text，桌宠渠道用）
    QMap<QString, QString> tags;    // 四维状态（原 raw_tags）
    QString channel = Channel::kPet;
    QString sessionId = Channel::kDefaultSession;
    QString speaker;                // 触发本句的说话人（群聊场景）
};

#endif // UTTERANCE_H
