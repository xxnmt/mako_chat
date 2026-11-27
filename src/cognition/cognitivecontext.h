#ifndef COGNITIVECONTEXT_H
#define COGNITIVECONTEXT_H

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "cognitivesnapshot.h"   // EvidenceAuthority / CognitiveSnapshot / SnapshotBuildStats
#include "historyturn.h"
#include "memoryaccesspolicy.h"  // MemoryAccessContext（claim 二次授权复用）
#include "memoryagenttypes.h"    // MemoryAgentItem（工具结果身份映射）
#include "memoryevidence.h"      // MemoryEvidence / RetrievalResult 形状
#include "promptassembler.h"     // PromptAssembler::MemoryContext（MEMORY_DATA 材料）
#include "recentcontext.h"

class MemoryManager;

// ===== R12-3a：统一认知上下文与证据目录（shadow 切片）=====
//
// 目标：给前台对话、私下回顾、matter consult、memory_recall/memory_read、行动核验
// 等入口提供同一套“证据身份 + 语义 + 时间 + 隐私投影 + 覆盖状态”的数据语义；各入口
// 只做不同投影，不要求 prompt 完全一致。
//
// 本切片只做 shadow：
//   * 不替换生产 prompt（旧 CURRENT_UNDERSTANDING / MEMORY_DATA / 快照 / 历史继续注入）；
//   * 不发起任何模型请求，不重跑认知（LLM/verifier）；只做内存归一化/去重/统计；
//   * 不新增数据库 schema，不删除旧模块；
//   * shadow 打开时由调用方把“已经读取到的材料”传进来，builder 不额外查库。
//
// 身份规则（R12-3a 第一切片的关键）：
//   * 同一原始事件在不同模块必须得到同一个 eventKey（facet 除外）：如
//     chat/12 的 user 面与 assistant 面是同一事件的两个 facet，而不是两条独立事件；
//   * 不同版本是不同身份（matter revision 1 与 2 共存，必须显式区分）；
//   * 派生摘要/认识/推断与原始证据是不同身份，用 derivedFrom 关联，绝不按文本合并；
//   * 不同日期的相同文本因 ref 不同而保持区分（禁止文本相等即合并）。

// 语义用途：authority 说明“谁说的/能证明什么”，kind 说明“这条资料在上下文里是什么”。
enum class EvidenceKind {
    UserStatement,             // 主人本人原话
    ExternalObservation,       // 渠道观察/第三方发言/外部真实事件
    AssistantExpression,       // 她自己说过/写过的话（只能证明“说过”）
    DerivedSummary,            // 派生摘要/事项认识/长期摘要
    SelfInference,             // 她自己的理解、猜测、关注 note
    InternalAutonomousRecord,  // 内部自主记录/调度状态/自发动机
    UserDelegation,            // 用户真实委托（提醒）
    ExecutionResult,           // 真实执行回执（投递/发送结果）
    ActivityArtifact           // 活动状态/成果（草稿/产物）
};

QString evidenceKindName(EvidenceKind kind);
EvidenceKind evidenceKindFromName(const QString &name);
// authority 到 kind 的保守映射：只用于“材料本身没有更具体语义”时。
EvidenceKind evidenceKindFromAuthority(EvidenceAuthority authority);

// 时间语义：四条时间线不混用。reported_at 是“这条资料何时被记录/报告”，
// event_time 是“原始事件何时发生”，effective_at 是“认识/委托何时生效”，
// recorded_at 兼容检索层已有的派生记录时间。未知保持未知，不由文本猜测补全。
struct EvidenceTime
{
    QDateTime reportedAtUtc;
    QDateTime eventAtUtc;
    QDateTime effectiveAtUtc;
    QDateTime recordedAtUtc;
    bool eventTimeUnknown = true;
    // 文本含“今天/明天/昨晚”等相对表达，但服务端没有锚定事件时间：
    // 统一上下文必须把它标记为“相对时间未锚定”，不能继承旧消息里的今天/明天。
    bool relativeExpressionUnanchored = false;
};

// 证据身份。eventKey 用于“同一原始事件跨模块识别”；stableKey 在 eventKey 上再带
// facet，用于“同一事件的两个语义面是否被重复完整序列化”。
struct EvidenceIdentity
{
    QString sourceKind;  // chat/reply->chat/raw/episodic/interaction_summary/claim/summary/
                         // profile/relationship/matter/source/reflection/concern/thought/
                         // activity/reminder/wake/delivery/receipt/current_input/tool
    QString ref;         // 规范 stable_ref（version 由 version 字段承载；matter 用 matter/<name>）
    int version = 0;     // 0 = 无版本概念
    QString facet;       // "" / user_statement / assistant_expression / summary / artifact
    bool stable = true;  // false 仅用于“只有本次 run 句柄、没有持久 stable_ref”的材料

    bool isValid() const { return !ref.isEmpty(); }
    QString eventKey() const;   // sourceKind|ref@version（不含 facet）
    QString stableKey() const;  // eventKey#facet
    // 供日志/测试/回指模型可见编号使用的展示 ref。
    QString displayRef() const;
};

// 从既有 ref 拼写归一身份。existing 代码里同一行有多种合法拼写：
//   reply/12 -> chat/12#assistant_expression；chat/12:assistant -> chat/12#assistant_expression；
//   revision/<matter>/<v> -> matter/matter%1#...@v；interaction/<id> -> interaction_summary/<id>；
//   episode/<id> -> episodic/<id>；long_summary/<id> -> summary/<id>；delivery/... 原样。
// sourceKindHint 只用于无法从 ref 判断前缀的场合（例如工具项只有内部 id）。
EvidenceIdentity evidenceIdentityForRef(const QString &rawRef,
                                        const QString &sourceKindHint = {},
                                        int version = 0,
                                        const QString &facet = {});
// matter 认识的身份：understanding 路径给 matter 名 + revision，快照给 revision/<matter>/<v>，
// 两者必须归一成同一 key。
EvidenceIdentity evidenceIdentityForMatter(const QString &matter, int version);

// 统一目录中的一条证据。text 可以按用途裁剪，但身份/时间/隐私/来源不裁剪。
struct CognitiveEvidence
{
    EvidenceIdentity identity;
    EvidenceKind kind = EvidenceKind::DerivedSummary;
    EvidenceAuthority authority = EvidenceAuthority::SelfInference;
    QString subjectId;
    QString channel;
    QString sessionId;
    QString privacy = QStringLiteral("unknown");
    bool sensitive = false;
    EvidenceTime time;
    QString purpose;       // 首个用途（current_understanding / snapshot / history / retrieval / tool / review / action_source ...）
    QStringList purposes;  // 该证据的全部语义用途（同一证据可被多个域/入口使用）
    QStringList usages;    // 该身份被哪些模块/入口使用（去重）
    QStringList derivedFrom;   // 派生摘要/认识/推断的原始证据 refs
    QStringList modelHandles;  // 模型侧可见编号（ref:N、chunk/<id>、source/<id> 等），只报告不合并
    QString text;
    QString sourceState = QStringLiteral("complete");
    // 各用途对应的“旧领域模型可见结构化字段”。键为 purpose（如 snapshot:commitments），
    // 值为该领域原本会注入模型的行（来自既有读取投影，不新造状态）。
    QHash<QString, QJsonObject> structuredByPurpose;
    QJsonObject structuredFor(const QString &purpose) const
    {
        return structuredByPurpose.value(purpose);
    }

    int textChars() const { return text.size(); }
    QJsonObject toJson() const;
};

// 按域统计（复用快照已有的 queried/eligible/serialized/omitted 口径；新域也按同一形状）。
struct CognitiveDomainView
{
    QString name;
    QString status = QStringLiteral("not_queried");
    int queried = 0;
    int eligible = 0;
    int serialized = 0;
    int omitted = 0;
    bool queriedDomain = false;
    QStringList notes;
    QStringList refs;
    QJsonObject toJson() const;
};

// 服务端统一时间上下文：绝对时间 + 相对标签都由服务端从“本次构建的 now”计算。
// 跨午夜场景绝不继承旧消息里的“今天/明天”。
struct CognitiveContextTime
{
    QDateTime nowLocal;
    QDateTime nowUtc;
    QString timezone;
    QString localIso;
    QString utcIso;
    // 返回“今天/昨天/N天前 + 绝对日期时间”；未知时间返回 unknown，不猜测。
    QString relativeLabel(const QDateTime &atUtc, int *daysAgo = nullptr) const;
    QJsonObject toJson() const;
};

// 入口场景。ownerPrivate 是唯一的私有投影条件；群聊/第三方不因统一上下文而扩权。
struct CognitiveContextScene
{
    QString entry;          // foreground_reply/private_review/matter_consult/memory_recall/
                            // memory_read/action_verification/shadow
    QString subjectId;
    QString actorId;
    QString speaker;
    QString speakerRole;
    QString channel;
    QString sessionId;
    QString audienceKind;   // owner_private / owner_group / third_party_private / public_group
    QString audienceLabel;
    QString privacy;
    bool blindZone = false;
    bool longTermMemoryEnabled = false;
    bool ownerPrivate = false;
    bool viewerIsOwner = false;   // 当前对话对象是否主人（群内主人 ≠ 第三方）
    QString capabilityScope;
    QJsonObject toJson() const;
};

// 覆盖报告：complete/empty/partial/degraded/denied/error/budget_trimmed/not_queried
// 是不同事实，不能互相代替。“检索失败”必须与“确实没有历史”分开。
struct CognitiveCoverageReport
{
    QHash<QString, QString> sourceStates;
    QString overall = QStringLiteral("complete");
    QStringList emptySources;
    QStringList notQueriedSources;
    QStringList degradedSources;
    QStringList deniedSources;
    QStringList errorSources;
    QStringList trimmedSources;
    QStringList notes;
    void setSource(const QString &source, const QString &state, const QString &note = {});
    void finalize();
    QJsonObject toJson() const;
};

// 预算报告。snapshot_budget_bytes=4096 只约束快照投影，不当作统一上下文总预算；
// 本切片不裁剪生产 prompt，只记录统一上下文实际大小与对照材料的估算。
struct CognitiveBudgetReport
{
    int contextBytes = 0;       // 统一上下文数据紧凑 JSON 的 UTF-8 字节
    int contextChars = 0;
    int evidenceBytes = 0;      // 目录文本字节
    int uniqueEvidenceBytes = 0;
    int totalMaterialBytes = 0; // 归一化前材料文本字节（用于重复度）
    int duplicateMaterialBytes = 0;
    int estimatedTokens = 0;    // 估算：ceil(字符数/2)（CJK 粗略，不给 provider 精确 tokenizer）
    int legacyPromptBytes = 0;  // 对照：当前实际注入 messages 的总字节
    int legacyDataBytes = 0;    // 对照：数据消息字节（调用方给）
    int snapshotBudgetBytes = 4096;
    bool overLegacyPrompt = false;
    QHash<QString, int> domainBytes;
    int evidenceCount = 0;
    int uniqueEventCount = 0;
    QJsonObject toJson() const;
};

struct CognitiveContext
{
    QString runId;
    QString triggerRef;
    QString purpose;
    CognitiveContextScene scene;
    CognitiveContextTime time;
    QString currentInput;
    QString inputEventId;
    QDateTime inputOccurredAtUtc;
    QJsonArray inputEvents;

    QList<CognitiveEvidence> evidence;              // 已去重的目录（first-seen 顺序）
    QHash<QString, CognitiveDomainView> domains;    // 多域认知状态
    QJsonObject capabilities;                       // 能力视图（原样保留服务端清单）
    CognitiveCoverageReport coverage;
    CognitiveBudgetReport budget;

    // 入口投影：同一目录 + 不同用途；shadow 只报告，不替换生产 prompt。
    QStringList visibleDomains;
    QStringList deniedDomains;
    QJsonObject projectionJson() const;
    QJsonObject toShadowJson() const;

    int evidenceCount() const { return int(evidence.size()); }
    int uniqueEventCount() const;
    const CognitiveEvidence *findByKey(const QString &stableKey) const;
    CognitiveDomainView &domain(const QString &name);
};

// 新旧上下文对照。legacy 指当前实际注入的 evidence 目录（前台 m_run.evidence；
// 回顾 thread 的 refs 等），unified 指本模块构建的目录。
struct ShadowComparison
{
    int legacyRefCount = 0;
    int legacyStableRefCount = 0;
    int unifiedEvidenceCount = 0;
    int matchedRefs = 0;          // legacy 中能在 unified 找到同一事件
    int missingInUnified = 0;
    int extraInUnified = 0;
    int duplicateAcrossModules = 0; // 同一身份被 >=2 个入口/模块使用
    int repeatedEventFacets = 0;    // 同一事件的多个 facet（正常但需显式）
    int crossVersionGroups = 0;     // 同一 ref 存在多个版本（正常但需显式）
    int identityConflicts = 0;      // 同一 stableKey 被合并成不同 authority/kind（错误合并）
    int privacyFiltered = 0;
    int privacyLeaks = 0;
    int relativeTimeUnanchored = 0;
    int rawEvidenceCovered = 0;
    int rawEvidenceTotal = 0;
    double rawEvidenceCoverage = 0.0;
    QStringList notes;
    QJsonObject toJson() const;
};

// ===== R12-3a 第二切片：实际模型输入的投影 =====
//
// 第一切片只做 shadow；第二切片把统一证据目录投影成实际 messages 的数据块：
//   * 快照数据块（同域形状）——同一身份若已由短期历史/最近群消息/当前输入完整序列化，
//     这里只给 ref + 时间 + 语义说明，不重复完整文本；
//   * 跨渠道私密证据块——同上，已完整出现过的只保留 ref；
//   * MEMORY_DATA 继续由既有 PromptAssembler 生成（派生语义记录，与原始事件不是同一身份），
//     本切片不重写它的形状，避免破坏 evidence_decision/consult 的既有契约；
//   * 统一时间/权限最小说明。
// 投影只改变“文本放哪里”，不删除任何证据引用，也不改变 evidence 目录（模型引用的
// ref 仍按旧解析路径查得到）。
struct CognitiveProjectionStats
{
    bool actualMode = false;
    QString entry;
    int baselineChars = 0;       // 同轮旧组装 messages 总字符
    int baselineBytes = 0;
    int inputChars = 0;          // 投影后实际 messages 总字符
    int inputBytes = 0;
    int savedChars = 0;
    int savedBytes = 0;
    int promptTokens = -1;       // 真实响应 usage.prompt_tokens（可获得时）
    int duplicateEvidenceCount = 0;   // 目录中 usages>=2 的身份数
    int duplicateEntriesOmitted = 0;  // 因已完整序列化而改写为引用的条目数
    int effectiveEvidenceCount = 0;   // 实际完整给出文本的唯一身份数（本投影内）
    int refsEmitted = 0;
    int refsResolved = 0;
    int refsUnresolved = 0;
    // R12-3b：模型实际可见性检查（A 服务端可解析 / B 正文在最终输入 / C 结构化状态在最终输入 / D 授权）
    int refsBodyVisible = 0;
    int refsStructuredVisible = 0;
    int refsAuthorized = 0;
    int duplicateRefsReplaced = 0;
    int duplicateTextCharsRemoved = 0;
    int privacyFiltered = 0;
    int privacyLeaks = 0;
    qint64 buildMs = 0;
    int modelRequests = 0;       // 由调用方填；用于断言“不额外增加请求”
    int snapshotBudgetBytes = 4096;
    bool snapshotBudgetTrimmed = false;
    QStringList trimmedDomains;
    QStringList overflowDomains;
    QHash<QString, int> domainKept; // 每域完整条目数
    QJsonObject toJson() const;
};

struct CognitiveProjectionOptions
{
    QString entry;
    QString reviewKind;          // private_review 时：reflection/concern/open_wake/activity
    bool ownerPrivate = true;
    // 已经由更权威/更原始消息完整序列化的身份（stableKey）。投影遇到这些身份时只给 ref。
    QSet<QString> alreadySerialized;
    // 该身份在最终实际输入里的来源（history:user / review_thread / current_input ...），
    // 用于 B/C 可见性检查与报告，避免“服务端认为已序列化但实际没有注入”。
    QHash<QString, QString> serializedSources;
    int snapshotBudgetBytes = 4096;
    // 该入口是否真的会注入“跨渠道记忆资料/执行回执”消息块；没有注入时不得把其中的
    // 证据当成“已序列化”，否则会出现服务端认为可见但模型实际看不到的假阳性。
    bool privateEvidenceInjected = true;
    bool receiptsInjected = true;
    void markSerialized(const QString &stableKey, const QString &source)
    {
        if (stableKey.isEmpty()) return;
        alreadySerialized.insert(stableKey);
        if (!source.isEmpty()) serializedSources.insert(stableKey, source);
    }
};

struct CognitiveProjection
{
    QJsonObject snapshotJson;        // 快照数据块（与 CognitiveSnapshotBuilder::toPromptJson 同域形状）
    QJsonArray privateEvidenceItems; // 跨渠道私密证据 items（可含 duplicate_ref_only 项）
    QJsonArray receipts;             // 真实执行回执（只读；有空表就单独注入）
    QString unifiedTimeBlock;        // 统一时间/权限最小 system 说明
    QStringList refOnlyKeys;         // 被改写为“仅引用”的身份（服务端 stableKey）
    QStringList structuredRefOnlyKeys; // 其中保留了必要结构化状态的身份
    QStringList fullKeys;            // 本次给出完整正文的身份
    CognitiveProjectionStats stats;
    QJsonObject toJson() const;
};

// 计算实际模型输入投影。纯内存：不读库、不发请求、不写库。
CognitiveProjection projectModelInput(const CognitiveContext &context,
                                      const CognitiveProjectionOptions &options);

// 从投影里收集模型可见的 ref 字符串（用于引用解析率统计/测试）。
QStringList projectionRefs(const CognitiveProjection &projection);

// 判断模型可见 ref 是否对应统一目录里的某条证据（按 sourceKind+ref 匹配，版本可缺省）。
bool evidenceRefMatches(const CognitiveEvidence &evidence, const QString &ref);

// R12-3b：两入口的“已序列化来源”构造。按真实 CognitiveOrigin kind 区分来源身份，
// 禁止把 OpenWake/Activity 的数值 id 无条件映射成 chat/reflection id。
// 这些函数只做身份与来源登记，不查库、不写状态。
CognitiveProjectionOptions foregroundProjectionOptions(
    bool ownerPrivate, int snapshotBudgetBytes, const QList<HistoryTurn> &history,
    const RecentContextBundle &recent, const QString &currentInput,
    const QJsonArray &concerns, const QJsonArray &activities, const QJsonArray &reminders);
CognitiveProjectionOptions reviewProjectionOptions(
    const QJsonObject &thread, const QString &kind, qlonglong id, int version,
    bool ownerPrivate, int snapshotBudgetBytes);

// 按入口/用途返回快照域优先级（从高到低）；预算裁剪从低优先级开始。
QStringList projectionDomainPriority(const QString &entry, const QString &reviewKind);

// shadow 输入。所有材料都来自调用方已经读取的视图；builder 不额外查库、不发请求。
struct CognitiveContextRequest
{
    QString entry = QStringLiteral("shadow");
    RequestMeta meta;
    QString runId;
    QString triggerRef;
    QString purpose;
    QString currentInput;
    QString eventId;
    QJsonArray inputEvents;

    // 认知快照（可选；由 CognitiveSnapshotBuilder 在同一个调用路径里构建一次后传入）
    bool hasSnapshot = false;
    CognitiveSnapshot snapshot;
    SnapshotBuildStats snapshotStats;

    bool canReadUnderstanding = false;
    QJsonArray understanding;        // currentUnderstanding() 的行
    QStringList understandingMatters; // 与 understanding 下标对应的 matter 名（catalogKeys）
    QJsonArray concerns;             // matchedConcerns()/recentPrivateReflection() 行
    QJsonArray activities;           // currentActivitiesForModel() 行
    QJsonArray reminders;            // pendingReminders() 行

    // MEMORY_DATA 材料（已按渠道/可见性过滤）+ 访问上下文（claim 二次授权校验复用）。
    bool hasMemoryContext = false;
    PromptAssembler::MemoryContext memoryContext;
    MemoryAccessContext access;

    QList<HistoryTurn> history;

    // 自动检索材料。
    bool retrievalDone = false;
    QString retrievalStatus;         // complete / degraded / empty / ...
    QString retrievalBackend;
    bool retrievalTruncated = false;
    QList<MemoryEvidence> retrieved;

    RecentContextBundle recent;

    QJsonArray privateEvidence;      // 跨渠道私密证据（已过滤）
    QJsonObject capabilities;        // 能力清单（服务端核验后的只读视图）

    // 工具/consult/回顾/回执材料。
    QList<MemoryAgentItem> toolItems;
    QString toolStatus;              // complete/partial/empty/failed/timeout/空=未调用
    QJsonArray consultedRows;        // consult 结果（turn/text/reported_at）
    QJsonObject reviewThread;        // 私下回顾 thread（kind/history/prior/concern/basis/...）
    qlonglong reviewSourceTurn = 0;  // 回顾来源轮次（reflection/concern 的 id）
    QJsonArray receipts;             // 服务端执行回执材料（可选，含 ref/action_key/outcome）

    // 对照材料。
    QJsonObject legacyEvidence;      // 当前实际注入的 evidence 目录（前台 m_run.evidence）
    int legacyPromptBytes = 0;       // 当前实际 messages 总字节
    int legacyDataBytes = 0;         // 当前数据消息字节

    QStringList missingSources;      // 明确“本轮没有读取”的来源（与 empty 区分）
};

class CognitiveContextBuilder
{
public:
    explicit CognitiveContextBuilder(MemoryManager *memory);

    // 构建 shadow 上下文 + 新旧对照。纯内存操作：不查库、不发模型请求、不写库。
    CognitiveContext buildShadow(const CognitiveContextRequest &request,
                                 ShadowComparison *comparison = nullptr) const;

    // 把当前 legacy evidence 目录与统一目录比较。legacy 里的 ref:N 等 run 内编号
    // 不是 stable_ref，计入 notes，不伪装成覆盖。
    static ShadowComparison compare(const CognitiveContext &context,
                                    const QJsonObject &legacyEvidence);

    // 入口投影规则（供集成与测试使用）：群聊/第三方入口不携带 owner 私有域。
    static bool isOwnerPrivateScene(const RequestMeta &meta, bool blindZone);

private:
    MemoryManager *m_memory = nullptr;
    // buildShadow 期间记录当前对照对象，供 addEvidence 累计身份冲突/未锚定相对时间等
    // 观测值（addEvidence 在多个 ingest 里被调用，避免每个调用点都传指针）。
    mutable ShadowComparison *m_comparison = nullptr;

    void ingestSnapshot(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestMemoryContext(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestUnderstanding(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestConcerns(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestActivities(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestReminders(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestHistory(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestRetrieval(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestRecentContext(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestPrivateEvidence(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestToolItems(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestConsulted(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestReviewThread(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestReceipts(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void ingestCurrentInput(CognitiveContext &context, const CognitiveContextRequest &request) const;

    // 投影 + 证据计数。放在所有材料归一化之后，隐私过滤结果计入 comparison。
    void applyProjection(CognitiveContext &context, ShadowComparison *comparison) const;
    void finalizeBudget(CognitiveContext &context, const CognitiveContextRequest &request) const;
    void addEvidence(CognitiveContext &context, CognitiveEvidence evidence,
                     ShadowComparison *comparison) const;
};

#endif // COGNITIVECONTEXT_H
