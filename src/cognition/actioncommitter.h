#ifndef ACTIONCOMMITTER_H
#define ACTIONCOMMITTER_H

#include <QJsonObject>
#include <QString>

class MemoryManager;

// 切片 B：主动外发的"唯一提交入口"。
// 本类只做提交与唯一性，不发任何消息：把三条主动动作（SpontaneousContact /
// ReminderDelivery / ActivityShare）的域状态 CAS、cognitive_action、action_outbox
// 收口到 MemoryManager 的同一个 SQLite 事务里；发送由 ActionOutboxDispatcher 负责。
// 策略/认知快照的接入留给后续切片（这里不依赖 LLM/QQ/网络）。
class ActionCommitter
{
public:
    explicit ActionCommitter(MemoryManager *memory);

    // contact：复用联系意图的渠道/权限/依据重验/每日额度校验；校验失败返回 {"error":...}，不写任何行。
    QJsonObject commitReflectionContact(qlonglong turn,int version,const QString &policyReason = {});
    // reminder：等价 claimDueReminder 的领域校验（pending、due<=now、expires>now）。
    QJsonObject commitReminderDelivery(qlonglong reminderId);
    // share：等价 canShareThought/draft 校验（completed、版本、artifact、同版本未送达）。
    QJsonObject commitActivityShare(qlonglong activityId,int version,const QString &draft);

    // ===== R12-2b：已验证 ActionProposal 的唯一提交入口 =====
    // 从 action_proposal 读 state='verified' 的提案，再次经 ActionPolicy 复核后在单事务内
    // 写 cognitive_action + action_outbox + delivery_ledger，并把提案 CAS 到 committed。
    // 不直接发送（发送只由 ActionOutboxDispatcher 做）；不写 reflection_contact/chat_history。
    QJsonObject commitVerifiedActionProposal(const QString &actionKey);
    // 便捷入口：wake→contact / activity→share 的来源类型前置校验（等价于上面的通用入口）。
    QJsonObject commitOpenWakeContact(const QString &actionKey);
    QJsonObject commitInternalActivityShare(const QString &actionKey);
    // 启动/定时恢复：扫描 verified 提案逐条提交（有界）。委托 MemoryManager 统一实现。
    QJsonObject drainVerifiedActionProposals(int limit = 8);

    // 三条动作的 action_key 约定只暴露给调用方做日志/回执关联。
    static QString contactActionKey(qlonglong turn,int version);
    static QString reminderActionKey(qlonglong reminderId);
    static QString shareActionKey(qlonglong activityId,int version);

private:
    MemoryManager *m_memory = nullptr; // 非所有权
};

#endif // ACTIONCOMMITTER_H
