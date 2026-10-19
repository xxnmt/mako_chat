#ifndef SESSIONROUTER_H
#define SESSIONROUTER_H

#include <QObject>
#include "utterance.h"
#include "historyturn.h"

class MemoryManager;

// 输入路由：所有渠道的用户输入经此进入大脑
// 职责：身份解析（IdentityResolver）→ 会话历史组装（channel+session_id 双键隔离）→ 发起大脑请求
// Phase 0 仅有桌宠渠道；QQ 消息上报与群聊滑动窗口/门控在 Phase 1/2 接入
// （见 docs/HUMANIZATION_PLAN.md 第七节）
class SessionRouter : public QObject
{
    Q_OBJECT
public:
    explicit SessionRouter(MemoryManager *memoryManager, QObject *parent = nullptr);

    // 渠道上报用户输入
    // speaker 为显示名（气泡/记忆展示用）；QQ 渠道 Phase 1 另传 senderId 做身份解析
    void submitInput(const QString &channel, const QString &sessionId,
                     const QString &speaker, const QString &text);

signals:
    // 交给大脑侧（AppController → LLMService::askDeepSeek）
    void requestReply(const QString &userText,
                      const QList<HistoryTurn> &history,
                      const RequestMeta &meta);

private:
    // 身份解析（C2 决策：归属权在宿主配置层，渠道/模型无权判定）
    // Phase 0：桌宠对话恒 'user'；Phase 1 查 identity 配置（self→丢弃 / master→'user' / 未知→'qq:<uin>'）
    QString resolveSubject(const QString &channel, const QString &senderId);

    MemoryManager *m_memoryManager;   // 非所有权
};

#endif // SESSIONROUTER_H
