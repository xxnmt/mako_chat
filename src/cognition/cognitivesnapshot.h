#ifndef COGNITIVESNAPSHOT_H
#define COGNITIVESNAPSHOT_H

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

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

// R12-0/R12-1a：每域统计口径：
//   queried    = 记忆层查询命中的原始行数（读取策略过滤前；受查询 LIMIT 影响，是下界）
//   eligible   = 通过读取策略、进入快照的行数
//   serialized = 最终进入提示词 JSON 的行数（可能被每域上限或字节预算裁掉）
//   omitted    = eligible - serialized（区分预算裁剪与策略过滤见 status/notes）
//   queriedDomain = 本次是否真的查询了该域（群聊投影显式不查询私人域，与"查询了但为空"分开）
//   status     = empty|filtered|query_cap|serialized|budget_trimmed|overflow
//                （build 先给出查询侧状态，toPromptJson 再按最终序列化结果收敛）
struct SnapshotDomainStats
{
    int queried = 0;
    int eligible = 0;
    int serialized = 0;
    int omitted = 0;
    bool queriedDomain = false;
    QString status;
};

// 一次构建 + 序列化的整体统计。bytesBefore/bytesAfter 是 QJsonDocument::Compact 的
// UTF-8 字节数（裁剪前/后，只统计数据域，不含提示词固定说明），
// notes 记录裁剪/溢出/群聊投影/策略过滤/查询上限等口径说明。
struct SnapshotBuildStats
{
    QHash<QString, SnapshotDomainStats> domains;
    int bytesBefore = 0;
    int bytesAfter = 0;
    QStringList notes;
};

// 一次构建的只读认知快照。不新增数据库表，也不永久保存整份快照。
struct CognitiveSnapshot
{
    QString runId;
    QString triggerRef;
    QString purpose;                           // foreground_reply / private_review，进哈希的 scope 契约
    QString subjectId;
    QString currentChannel;
    QString currentSession;
    QDateTime builtAtUtc;
    QList<CognitiveFactRef> observations;      // 用户/第三方/真实观察/执行回执，按 authority 区分
    QList<CognitiveFactRef> selfExpressions;   // 她说过/写过的话，只能证明“说过”
    QList<CognitiveFactRef> inferences;        // 她的理解/猜测，不能当事实
    QList<SelfMotivation> motivations;         // concern / interest / activity 等自发动机
    QList<DelegatedCommitment> commitments;    // 真实 delegation（如 reminder）
    QJsonArray activities;                     // 私人活动状态/产物（active/paused 调度视图）
    // R12-1b：有界完成成果目录（completed 且存在真实 artifact），按完成时间倒序；
    // 只含 ref/version/artifact_ref/artifact_version/status/goal/title/summary/completed_at，
    // 不含过时 next_step；群聊/第三方投影为空。
    QJsonArray recentActivityResults;
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

    // 输出给模型的紧凑 JSON：每类只给必要字段、文本截断、每域最多 kDomainLimit 条。
    // R12-1a：快照 JSON 只含数据域，不再带静态说明键（note/internal_wake_note）；
    // 固定规则由 LLMService::cognitiveSnapshotMessage 放进 system content，不占数据预算。
    // 预算不够时先按低优先域裁剪到"每个非空域至少 1 条"，连最小集都放不下才整域
    // overflow 并写 budget:overflow:<domain>/budget:unresolved 说明，绝不静默。
    QJsonObject toPromptJson(const CognitiveSnapshot &snapshot) const;

    // 注入提示词时的固定前缀（前台与回顾共用，测试据此识别注入消息）。
    static QString promptPrefix();

    // 快照数据 JSON 的字节预算（UTF-8 bytes）：读 ConfigManager 的
    // cognition.snapshot_budget_bytes（默认 4096，配置层已夹紧 2048..8192，这里再兜底）。
    static int promptByteBudget();

    // R12-0：最近一次 build/toPromptJson 的每域统计（只读）。build 记录 queried/eligible，
    // toPromptJson 记录 serialized/omitted 与裁剪前后字节数。build 会重置统计。
    const SnapshotBuildStats &stats() const { return m_stats; }

private:
    MemoryManager *m_memory = nullptr;
    // build/toPromptJson 是 const 方法，但统计是可观测性副作用，用 mutable 保存。
    mutable SnapshotBuildStats m_stats;
};

#endif // COGNITIVESNAPSHOT_H
