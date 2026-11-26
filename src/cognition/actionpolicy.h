#ifndef ACTIONPOLICY_H
#define ACTIONPOLICY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

#include "actionorigin.h"

class MemoryManager;

// 一条 claim 的核验结果。确定性校验只会给出 Entailed / Insufficient
// （没有依据就是无法证明，而不是"事实为假"）；Contradicted 只由语义验证器给出。
struct ClaimValidation
{
    enum Result { Entailed, Contradicted, Insufficient };
    Result result = Insufficient;
    QString reason;
    QStringList supportingRefs;
    // R12-2a：明确"不确定/未知"（CurrentState 允许以 unknown 通过，而不是当成事实确认）。
    bool unknown = false;
    // 服务端按 claim 类型判定的用途名（审计用；调用方输入的 purpose 不能改写它）。
    QString purpose;
};

// R12-2a：证据用途（Purpose）。由服务端按 claim 类型/来源决定，模型不能自定义。
//
//   * HistoricalFact  真实历史来源（主人以前说过的话/历史事件）。不因早于本次 wake/
//                     关注时间而 stale；仍要求来源 authority/可见性。
//   * CurrentState    当下状态。不能仅凭历史相似记录确认：必须有当前/较新证据，
//                     或明确 unknown（explicit_unknown），否则拒绝。
//   * NewEvidence     本次回顾期间的新证据。**唯一保留 stale_ref 的路径**：
//                     来源时间必须晚于本次 wake/关注来源时间。
//   * UserCommitment  仅真实用户原话（chat/user reply 或 source subject=user）；
//                     不能由助手旧回复、主动消息、内部记录/提醒推出。
//   * ExecutionResult 真实 delivery/action 回执（handed_off）或已送达的 chat 行。
//   * ArtifactResult  真实存在且可核验的 thought/<id> 或 activity/<id> 成果
//                     （completed、artifact_ref 指回自身、draft 非空；新成果还要求版本/哈希）。
//   * InternalState   自己的内部记录/想法/调度（reflection/matter/wake 等）：
//                     只能证明"她内部有过这条记录"，不得当作用户事实或承诺；
//                     这类 claim 不能成为动作核心。
enum class EvidencePurpose {
    HistoricalFact,
    CurrentState,
    NewEvidence,
    UserCommitment,
    ExecutionResult,
    ArtifactResult,
    InternalState
};

QString evidencePurposeName(EvidencePurpose purpose);
bool evidencePurposeFromName(const QString &name, EvidencePurpose *purpose);

// R12-2a：coverage 核验结果。即使 claims=[] 也必须能识别 message/reason 里
// 没有被 declared claims 覆盖的外部事实/承诺；complete=false 时整项动作 withhold。
struct CoverageCheck
{
    bool complete = true;       // true=未发现未声明的外部事实/引用
    bool semantic = false;      // true=经过注入的语义 coverage verifier（至多一次）
    QString reason;             // 失败原因码（undeclared_reference / undeclared_external_fact / ...）
    QStringList undeclared;     // 未声明片段（有界：ref token 或事实断言原文）
};

// 主动联系策略的内部决策形状。evaluateSpontaneousContact 对外返回等价的 JSON，
// 这里保留结构体便于调用方在 C++ 侧做类型安全的中间传递/审计。
struct PolicyDecision
{
    bool allowed = false;
    QString reason;
    bool needsVerifier = false;
    QStringList verifierRefs;
};

// 主动联系/通用动作的授权/事实策略（报告切片 C + R12-2a）。
//
// 设计要点：
// - 动机与事实分离：motivation 只说明"她为什么想开口"（self_desire 不需要主人新证据），
//   claims 才是消息里对事实/承诺/执行结果的声称，必须逐条给出 typed refs。
// - 确定性校验只查库、不做语义判断：ref 是否存在、authority 是否允许、subject/时序/可见性、
//   成果是否真实、用途（Purpose）是否成立。语义蕴含由注入的 SemanticVerifier 判定。
// - R12-2a：claim 的用途由服务端按 type 判定（purposeForClaimType），refs 校验统一走
//   validateClaimForPurpose；stale_ref 只作用于 NewEvidence/需要新证据的路径。
// - coverage 核验独立于 declared claims：claims 为空也不能直接放行；
//   默认确定性检测始终可用，注入的 coverage verifier（真实模型）至多调用一次。
// - 未注入验证器（provider 不可用）时，凡带事实 claim 的联系一律保守拒绝；
//   纯 self_desire/问候（claims 为空）不调用语义验证器。
// - 本类不负责"提醒链"：reminder 由提醒链独占执行，这里只把 reminder 引用判为
//   不能支持 user_commitment（不是主人原话）。
class ActionPolicy
{
public:
    // 语义验证器：输入 claim 类型/文本与来源文本，输出 entailed/contradicted/insufficient。
    // 未注入时表示"无法验证"，带事实 claim 的联系会被保守拒绝。
    using SemanticVerifier = std::function<ClaimValidation(const QString &claimType,
                                                           const QString &claimText,
                                                           const QJsonArray &sources)>;

    // R12-2a：coverage 验证器（真实模型只接这一处；本片测试用假函数）。
    // 输入 message/reason/已声明 claims/origin，输出是否完整覆盖外部事实。
    // verifyContactCoverage 保证单次动作内至多调用一次，不做 repair。
    using CoverageVerifier = std::function<CoverageCheck(const QString &message,
                                                         const QString &reason,
                                                         const QJsonArray &claims,
                                                         const ActionOrigin &origin)>;

    explicit ActionPolicy(MemoryManager *memory);

    void setSemanticVerifier(SemanticVerifier verifier);
    bool hasSemanticVerifier() const { return static_cast<bool>(m_verifier); }

    void setCoverageVerifier(CoverageVerifier verifier);
    bool hasCoverageVerifier() const { return static_cast<bool>(m_coverageVerifier); }

    // ===== R12-2a：Evidence Purpose =====
    // claim.type → 服务端判定用途；未知类型返回 false（不默认成事实）。
    static bool purposeForClaimType(const QString &type, EvidencePurpose *purpose);
    static QString claimTypeForPurpose(EvidencePurpose purpose);

    // 按 purpose 校验一条 claim 的依据（确定性、只读，不调用语义/coverage verifier）。
    // origin 提供时序基准与目标可见性上下文（targetChannel/targetSession）。
    ClaimValidation validateClaimForPurpose(const QString &channel, const QString &session,
                                            const ActionOrigin &origin,
                                            EvidencePurpose purpose,
                                            const QJsonObject &claim) const;

    // 按 purpose 校验单条 ref（供组合校验/测试）；返回 {ok/reason/text/版本/哈希}。
    ClaimValidation validateEvidenceRef(const QString &channel, const QString &session,
                                        const ActionOrigin &origin,
                                        EvidencePurpose purpose,
                                        const QString &ref) const;

    // 默认确定性 coverage 检测（不接模型）：识别显式内部 ref 未被 claimed refs 覆盖、
    // 以及 claims 为空/无事实 claim 时 message/reason 中出现的外部事实/承诺断言。
    CoverageCheck deterministicCoverageCheck(const QString &message, const QString &reason,
                                             const QJsonArray &claims) const;

    // 统一 coverage 入口：先确定性，再（若注入）一次语义 coverage 核验。
    // 语义核验至多一次；不实现 repair（修复预算留到需要真实模型时）。
    CoverageCheck verifyContactCoverage(const QString &message, const QString &reason,
                                        const QJsonArray &claims,
                                        const ActionOrigin &origin) const;

    // ===== R12-2a：通用动作提案统一核验（不发送、不落库）=====
    // 输入 proposal：{origin, action_key?, kind:"contact"|"activity_share",
    //                  target?:{channel,session_id}, payload:{message/reason 或 draft}, claims:[]}
    // 返回：allowed/withheld + reason + semantic_status + claims/coverage 细节。
    // 校验：origin 真实身份（wake/private_reflection/成果）、action_key 派生一致、
    //       target 可见性、claims 用途/权威、artifact 真实（completed+可核验）、
    //       coverage（claims 为空也执行）；OpenWake contact 在 allow_contact=false 时拒绝。
    // 注意：本接口只做核验，绝不发送、不写 action_outbox/cognitive_action。
    QJsonObject validateActionProposal(const QJsonObject &proposal, QString *error = nullptr) const;

    // 确定性校验：type/ref 存在、authority 允许、subject、隐私、时序；不做语义判断。
    // claim: {id,type,text,refs[],core?}。R12-2a 起内部按 Purpose 调度：
    //   reported_fact  → NewEvidence（保留"相对关注来源的新证据"旧语义）
    //   user_commitment → UserCommitment；execution_result → ExecutionResult；
    //   inference → InternalState；新类型按 purposeForClaimType 判定。
    ClaimValidation validateClaimDeterministic(const QString &channel, const QString &session,
                                               qlonglong concernTurn,
                                               const QJsonObject &claim) const;

    // 动机合法性（确定性）：self_desire / concern_followup / activity_share 及各自 ref 要求。
    // activity_share 支持 legacy thought/<id> 与 internal activity/<id> 两种统一引用，
    // 只接受 completed 且有真实成果的活动（内部想法/未完成目标不能当成果）。
    // 返回空串=合法，否则返回可审计的原因码。
    QString validateMotivation(const QString &channel, const QString &session,
                               qlonglong concernTurn, const QJsonObject &motivation) const;

    // 主动联系策略：motivation 合法性 + claims 校验 + cooldown/quiet/目标/重复。
    // 返回 {allowed, reason, needs_verifier, verifier_sources:[{ref,text}], ...}。
    // claims 为空（纯 self_desire/问候）时不调用语义验证器。
    QJsonObject evaluateSpontaneousContact(const QString &channel, const QString &session,
                                           qlonglong concernTurn,
                                           const QJsonObject &contact) const;

    // 只做确定性部分的同一套策略：供记忆层在提交前/领取前复核，
    // 不会发起任何语义验证（needs_verifier 表示"若真要发送还需语义核验"）。
    QJsonObject evaluateSpontaneousContactDeterministic(const QString &channel, const QString &session,
                                                        qlonglong concernTurn,
                                                        const QJsonObject &contact) const;

    // 把 claim refs 对应的库内原文摘要收集成 verifier sources（去重、最多 4 条、总字符有上限）。
    QJsonArray claimSources(const QString &channel, const QString &session,
                            qlonglong concernTurn, const QJsonArray &claims) const;

    // 单条 ref 的文本摘要（无文本时 ref 仍保留，text 为空）。供审计/调试。
    QString refExcerpt(const QString &ref, int maxChars = 512) const;

private:
    // 来源时间（时序基准）：wake 用 open_wake.started_at/created_at，
    // reflection/concern 用 chat_history/cognitive_source 的 reported；找不到返回空串。
    QString originReportedText(const ActionOrigin &origin) const;
    // 来源真实身份校验：wake/private_reflection/成果 与 origin 的 id/version/runId/scope/
    // epoch 是否一致；contact 额外检查 open_wake.allow_contact。返回空串=通过。
    QString originSourceError(const ActionOrigin &origin, const QString &actionKind) const;

    MemoryManager *m_memory = nullptr;
    SemanticVerifier m_verifier;
    CoverageVerifier m_coverageVerifier;
};

#endif // ACTIONPOLICY_H
