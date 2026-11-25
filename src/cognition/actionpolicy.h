#ifndef ACTIONPOLICY_H
#define ACTIONPOLICY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

class MemoryManager;

// 一条 claim 的核验结果。确定性校验只会给出 Entailed / Insufficient
// （没有依据就是无法证明，而不是"事实为假"）；Contradicted 只由语义验证器给出。
struct ClaimValidation
{
    enum Result { Entailed, Contradicted, Insufficient };
    Result result = Insufficient;
    QString reason;
    QStringList supportingRefs;
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

// 主动联系的授权/事实策略（报告切片 C）。
//
// 设计要点：
// - 动机与事实分离：motivation 只说明"她为什么想开口"（self_desire 不需要主人新证据），
//   claims 才是消息里对事实/承诺/执行结果的声称，必须逐条给出 typed refs。
// - 确定性校验只查库、不做语义判断：ref 是否存在、authority 是否允许、subject/时序/可见性。
//   语义蕴含由注入的 SemanticVerifier 判定（LLMService 用一次模型请求实现，测试注入假函数）。
// - 未注入验证器（provider 不可用）时，凡带事实 claim 的联系一律保守拒绝；
//   纯 self_desire/问候（claims 为空）不调用验证器。
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

    explicit ActionPolicy(MemoryManager *memory);

    void setSemanticVerifier(SemanticVerifier verifier);
    bool hasSemanticVerifier() const { return static_cast<bool>(m_verifier); }

    // 确定性校验：type/ref 存在、authority 允许、subject、隐私、时序；不做语义判断。
    // claim: {id,type,text,refs[],core?}
    ClaimValidation validateClaimDeterministic(const QString &channel, const QString &session,
                                               qlonglong concernTurn,
                                               const QJsonObject &claim) const;

    // 动机合法性（确定性）：self_desire / concern_followup / activity_share 及各自 ref 要求。
    // 返回空串=合法，否则返回可审计的原因码。
    QString validateMotivation(const QString &channel, const QString &session,
                               qlonglong concernTurn, const QJsonObject &motivation) const;

    // 主动联系策略：motivation 合法性 + claims 校验 + closure/cooldown/quiet/目标/重复。
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
    MemoryManager *m_memory = nullptr;
    SemanticVerifier m_verifier;
};

#endif // ACTIONPOLICY_H
