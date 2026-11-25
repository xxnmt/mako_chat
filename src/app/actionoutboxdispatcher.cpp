#include "actionoutboxdispatcher.h"
#include "memorymanager.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTimer>

ActionOutboxDispatcher::ActionOutboxDispatcher(MemoryManager *memory,QObject *parent)
    : QObject(parent), m_memory(memory)
{
    // 唯一实例串：租约/日志用；同进程只有一个 dispatcher，跨进程靠 lease 恢复兜底。
    m_owner=QStringLiteral("outbox-dispatcher:")
            + QString::number(reinterpret_cast<quintptr>(this),16);
}

void ActionOutboxDispatcher::setSender(std::function<bool(const Utterance &)> sender)
{
    m_sender=std::move(sender);
}

void ActionOutboxDispatcher::start(int intervalMs)
{
    if (!m_timer) {
        m_timer=new QTimer(this);
        connect(m_timer,&QTimer::timeout,this,&ActionOutboxDispatcher::pollOnce);
    }
    // 启动即冻结上一进程遗留的 dispatching：按 at-most-once 落 unknown，绝不重发。
    recoverStale(0);
    if (intervalMs<100) intervalMs=100;
    m_timer->start(intervalMs);
}

void ActionOutboxDispatcher::stop()
{
    if (m_timer) m_timer->stop();
}

QString ActionOutboxDispatcher::payloadText(const QJsonObject &payload) const
{
    // 多句合并成一条消息：一条 action 只发一次 sendUtterance，echo=action_key。
    // 若按句拆成多次发送，同一个 action_key 会收到多个回执、破坏唯一回执语义。
    const auto sentences=payload.value(QStringLiteral("sentences")).toArray();
    QStringList lines;
    for (const auto &value : sentences) {
        const auto text=value.toObject().value(QStringLiteral("zh_text")).toString();
        if (!text.trimmed().isEmpty()) lines << text;
    }
    if (!lines.isEmpty()) return lines.join(QLatin1Char('\n'));
    for (const auto &key : {QStringLiteral("text"),QStringLiteral("draft"),
                            QStringLiteral("message")}) {
        const auto text=payload.value(key).toString();
        if (!text.trimmed().isEmpty()) return text;
    }
    return {};
}

void ActionOutboxDispatcher::pollOnce()
{
    if (!m_memory || !m_sender || m_polling) return;
    m_polling=true;
    auto resetPolling=qScopeGuard([this]{ m_polling=false; });

    const auto job=m_memory->claimOutbox(m_owner,60);
    if (job.isEmpty()) return;
    const auto actionKey=job.value(QStringLiteral("action_key")).toString();
    const auto payload=job.value(QStringLiteral("payload")).toObject();
    if (actionKey.isEmpty()) return;

    Utterance utterance;
    utterance.channel=job.value(QStringLiteral("channel")).toString();
    utterance.sessionId=job.value(QStringLiteral("session")).toString();
    utterance.text=payloadText(payload);
    // 回执归一：一条 action 一个 echo，QQ/Pet 的回执都按 action_key 落库。
    utterance.deliveryEcho=actionKey;
    // 供 actionSettled 的订阅方（历史回写/日志）拿到最终发送文本与路由；
    // payload 里的 sentences/draft/message 保持原样，方便审计。
    QJsonObject settledPayload=payload;
    settledPayload[QStringLiteral("text")]=utterance.text;
    settledPayload[QStringLiteral("channel")]=utterance.channel;
    settledPayload[QStringLiteral("session")]=utterance.sessionId;
    if (utterance.text.trimmed().isEmpty()) {
        m_memory->settleOutboxAction(actionKey,QStringLiteral("withheld"),
                                     QStringLiteral("empty_payload"));
        emit actionSettled(actionKey,QStringLiteral("withheld"),settledPayload);
        return;
    }

    if (!m_sender(utterance)) {
        // 确定没有进入渠道：落 rejected；attempts<2 时 outbox 回 queued，下一次 poll 受控重试一次。
        m_memory->settleOutboxAction(actionKey,QStringLiteral("rejected"),
                                     QStringLiteral("submit_failed"));
        qInfo()<<"[Outbox] submit_failed"<<actionKey;
        emit actionSettled(actionKey,QStringLiteral("rejected"),settledPayload);
        return;
    }

    // 已提交。pet 没有协议层回执，handed_off 即本地送达；QQ 的 handed_off 只是入队，
    // 真正的终态由 onSendResult(protocol_accepted/rejected/unknown) 再 settle。
    // 只有 QQ 才需要保留 in-flight payload 等协议回执；pet 不会再有回执。
    if (utterance.channel!=Channel::kPet) m_inflight.insert(actionKey,settledPayload);
    if (m_memory->settleOutboxAction(actionKey,QStringLiteral("handed_off")))
        emit actionSettled(actionKey,QStringLiteral("handed_off"),settledPayload);
}

void ActionOutboxDispatcher::onSendResult(const QString &echo,const QString &outcome)
{
    if (!m_memory || echo.isEmpty() || outcome.isEmpty()) return;
    const auto payload=m_inflight.take(echo);
    if (!m_memory->settleOutboxAction(echo,outcome)) {
        qWarning()<<"[Outbox] receipt not recorded"<<echo<<outcome;
        return;
    }
    emit actionSettled(echo,outcome,payload);
}

int ActionOutboxDispatcher::recoverStale(int staleSeconds)
{
    if (!m_memory) return 0;
    return m_memory->recoverStaleOutbox(staleSeconds);
}
