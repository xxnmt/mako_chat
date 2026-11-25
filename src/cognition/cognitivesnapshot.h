#ifndef COGNITIVESNAPSHOT_H
#define COGNITIVESNAPSHOT_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "utterance.h"

class MemoryManager;

// 证据权威域：服务端对“这条资料是谁说的、能证明什么”的唯一分类。
// 模型只能按 authority 使用资料：只有 UserStatement 能当主人本人的事实；
// AssistantExpression 只证明她说过/写过；SelfInference 不能当事实；
// InternalState 是服务端自己的调度状态，永远不是主人委托。
enum class EvidenceAuthority {
    UserStatement,        // 主人本人原话（主人亲口报告/委托）
    ExternalObservation,  // 渠道观察：第三方发言、感知到的真实事件
    ExecutionResult,      // 执行回执：提醒/投递等确实执行过
    AssistantExpression,  // 她自己说过/写过的话，只能证明“说过”
    SelfInference,        // 她自己的理解/猜测，不能当事实
    InternalState         // 自己的内部调度状态（唤醒/活动），不是主人约定
};

// 权威域的可序列化名称（快照 JSON 与审计共用同一套拼写）。
QString evidenceAuthorityName(EvidenceAuthority authority);

// 一条可分类事实的引用。ref 与既有证据编号保持同一格式
// （chat/<turn>、reflection/<turn>、revision/<matter>/<version>、thought/<id>、reminder/<id>），
// 便于模型引用时与既有 claims / evidence 契约对齐。
struct CognitiveFactRef
{
    QString ref;
    EvidenceAuthority authority;
    QString subjectId;
    QString channel;
    QString sessionId;
    QDateTime occurredAtUtc;
    QString text;
    QString privacy;
    bool sensitive = false;
};

// 自发动机：关注、兴趣、活动等“她自己想继续”的东西，不是对主人的承诺。
struct SelfMotivation
{
    QString ref;
    QString kind;
    QString summary;
    int version = 0;
};

// 真实委托：目前只有提醒（reminder）这一种。due/expires 都可能为空。
struct DelegatedCommitment
{
    QString ref;
    QString kind;
    QString state;
    QDateTime dueAtUtc;
    QDateTime expiresAtUtc;
    QString text; // 委托内容（提醒原文，已按读取策略过滤并截断）
};

// 一次构建的只读认知快照。不新增数据库表，也不永久保存整份快照。
struct CognitiveSnapshot
{
    QString runId;
    QString triggerRef;
    QString subjectId;
    QString currentChannel;
    QString currentSession;
    QDateTime builtAtUtc;
    QList<CognitiveFactRef> observations;      // 用户/第三方/真实观察/执行回执，按 authority 区分
    QList<CognitiveFactRef> selfExpressions;   // 她说过/写过的话，只能证明“说过”
    QList<CognitiveFactRef> inferences;        // 她的理解/猜测，不能当事实
    QList<SelfMotivation> motivations;         // concern / interest / activity 等自发动机
    QList<DelegatedCommitment> commitments;    // 真实 delegation（如 reminder）
    QJsonArray activities;                     // 私人活动状态/产物
    QJsonArray internalWakes;                  // own_next_review_at / idle / scheduled，永远不是主人委托
    QJsonObject policyView;                    // 读取边界（渠道/会话/长期记忆/盲区等）
    QString snapshotHash;                      // 可分类内容的 SHA-256 前 16 位
};

// 服务端快照构建器：所有域都来自 MemoryManager 的只读查询，
// 前台对话、关注回顾、活动回顾共用同一套 authority/type 语义。
class CognitiveSnapshotBuilder
{
public:
    explicit CognitiveSnapshotBuilder(MemoryManager *memory);

    // 构建一份快照。purpose 只用于 policy_view 与审计（foreground_reply / private_review）。
    CognitiveSnapshot build(const RequestMeta &meta, const QString &runId,
                            const QString &triggerRef, const QString &purpose) const;

    // 输出给模型的紧凑 JSON：每类只给必要字段、文本截断、每域最多 8 条，
    // 并带 internal_wake_note 说明“内部唤醒不是主人约定”。
    QJsonObject toPromptJson(const CognitiveSnapshot &snapshot) const;

    // 注入提示词时的固定前缀（前台与回顾共用，测试据此识别注入消息）。
    static QString promptPrefix();

    // 快照 JSON 的字符预算：超量按域裁剪，先保留 observations / commitments / internal_wakes。
    static int promptCharBudget();

private:
    MemoryManager *m_memory = nullptr;
};

#endif // COGNITIVESNAPSHOT_H
