#ifndef ACTIONOUTBOXDISPATCHER_H
#define ACTIONOUTBOXDISPATCHER_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

#include "utterance.h"

class MemoryManager;
class QTimer;

// 切片 B：主动外发的唯一发送者。
// 只从 action_outbox 领取（claimOutbox），每次领取构造一条 Utterance 发送；
// echo 统一使用 action_key，保证"一条 action 一个回执"。不直接依赖 QQChannelAdapter：
// 具体走 pet 还是 QQ、QQ 是否在线，由 AppController 注入的 sender 决定。
class ActionOutboxDispatcher : public QObject
{
    Q_OBJECT
public:
    explicit ActionOutboxDispatcher(MemoryManager *memory,QObject *parent = nullptr);

    // sender 返回 true 表示"已提交给渠道/已本地显示"，false 表示确定未发出（可受控重试一次）。
    void setSender(std::function<bool(const Utterance &)> sender);
    void start(int intervalMs = 1000);
    void stop();
    // 单次轮询：领取一条 outbox 并发送。测试可直接调用。
    void pollOnce();
    // 渠道回执入口：echo 现在就是 action_key。
    void onSendResult(const QString &echo,const QString &outcome);
    // 启动恢复：dispatching 且租约过期 → unknown（at-most-once），返回恢复条数。
    int recoverStale(int staleSeconds = 600);

signals:
    void actionSettled(const QString &actionKey,const QString &outcome,const QJsonObject &payload);

private:
    QString payloadText(const QJsonObject &payload) const;
    MemoryManager *m_memory = nullptr; // 非所有权
    std::function<bool(const Utterance &)> m_sender;
    QTimer *m_timer = nullptr;
    QString m_owner;
    bool m_polling = false;
    // action_key → 已提交但还未拿到协议回执的 payload（给 actionSettled 补历史原文用）。
    QHash<QString,QJsonObject> m_inflight;
};

#endif // ACTIONOUTBOXDISPATCHER_H
