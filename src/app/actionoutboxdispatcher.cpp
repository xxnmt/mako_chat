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
    // P2：这里不再隐式 recoverStale(0)。启动恢复只由 AppController::startApp 的
    // recoverStale(600) 执行一次，避免 600s 阈值被第二次 0s 恢复覆盖。
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

    // 租约 300s：网络渠道提交后要等协议回执，留足回执窗口（启动恢复阈值仍是 600s，
    // 保证进程崩溃遗留的 dispatching 行不会被误当成可重领）。
    const auto job=m_memory->claimOutbox(m_owner,300);
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

    // pet 没有协议层回执：提交成功即本地送达，直接落 handed_off 并通知订阅方。
    if (utterance.channel==Channel::kPet) {
        if (m_memory->settleOutboxAction(actionKey,QStringLiteral("handed_off")))
            emit actionSettled(actionKey,QStringLiteral("handed_off"),settledPayload);
        return;
    }

    // P1：网络渠道（QQ 等）提交成功只代表已进入渠道，不是送达。这里不 settle、
    // 不 emit handed_off：outbox/action 保持 dispatching，等 onSendResult 的协议回执
    // （protocol_accepted/rejected/unknown）在一个事务里落终态。
    // 若进程在回执前崩溃，重启后由 recoverStale(600) 冻结为 unknown，绝不重发。
    m_inflight.insert(actionKey,settledPayload);
}

bool ActionOutboxDispatcher::proactiveHistoryEligible(const QString &outcome,
                                                      const QString &channel)
{
    if (outcome==QStringLiteral("protocol_accepted")) return true;
    // pet 没有协议回执，handed_off 表示气泡已经显示；其它渠道的 handed_off 只是入队。
    if (outcome==QStringLiteral("handed_off") && channel==Channel::kPet) return true;
    // unknown 表示"可能已送达但无法确认"，不能写成"茉子说过"；rejected/withheld 明确未发。
    return false;
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
