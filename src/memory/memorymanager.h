#ifndef MEMORYMANAGER_H
#define MEMORYMANAGER_H

#include <QObject>
#include <QObject>
#include <QSqlDatabase>
#include <QVariantMap>
#include <QList>
#include <QDateTime>
#include <QStringList>
#include <QSet>
#include <QJsonArray>
#include <QJsonObject>
#include "sentencedata.h"
#include "historyturn.h"
#include "utterance.h"
#include "recentcontext.h"
#include "activityref.h"

class MemoryEmbeddingProvider;
class MemoryReadPolicy;

struct RawEventWrite
{
    QString platform;
    QString channel;
    QString sessionId;
    QString eventType;
    QString content;
    QString messageId;
    QString direction = QStringLiteral("in");
    QString senderId;
    QString senderName;
    QString personId;
    QString payloadJson;
    QDateTime occurredAt;
    bool sensitive = false;
    QString visibility = QStringLiteral("private");
    QString redactionStatus = QStringLiteral("none");
    QString redactionRules;
    QString policyVersion;
    bool contentNull = false;
    bool payloadNull = false;
    bool processed = false;
};

struct MemoryChunkWrite
{
    QString sourceType;
    QString sourceRef;
    QString chunkKind;
    QString derived;
    QString text;
    QString channel;
    QString session;
    QString speakerId;
    QString speakerName;
    QString speakerRole;
    QString speechAt;
    QString eventTime;
    QString privacy;
    QString sourceTable;
    QString sourceId;
};

class MemoryManager : public QObject
{
    Q_OBJECT
public:
    explicit MemoryManager(const QString &dbFilePath,QObject *parent = nullptr,
                           const QString &connectionName = QString());    ~MemoryManager();
    QString databasePath() const { return m_databasePath; }
    bool saveQATurn(const QString &userInput,
                          const QString &rawReply,
                          const QList<SentenceText> &sentences,
                          const RequestMeta &meta = RequestMeta(), qint64 receivedTurn = 0);

    qint64 recordInput(const QString &input, const RequestMeta &meta, bool observe = false);
    bool recordQuiet(qint64 turn, const QString &action);
    bool linkGroupedInput(qint64 previousTurn, qint64 followingTurn);
    bool interruptTurn(qint64 turn, const QString &reason);
    bool enqueueObservation(const RequestMeta &meta, const QString &input);
    QJsonObject pendingObservation(qint64 source = 0) const;
    QJsonArray matterHeads(const QString &subject, const QStringList &terms = {}) const;
    bool commitObservation(qint64 source, const QJsonObject &proposal, QString *failure = nullptr,
                           const QJsonArray &historyEvidence = {}, QJsonObject *conflict = nullptr);
    QJsonArray historicalReports(const QString &subject, const QString &channel, const QString &session,
                                 const QString &before, const QStringList &terms, bool forRevision) const;
    void deferObservation(qint64 source, const QString &reason, bool rejected = false);

    // Internal consideration of this speaker's retained observations. Never serialize
    // these records directly into the audience-facing prompt.
    QJsonArray internalMatters(const RequestMeta &meta, const QStringList &terms,
                               const QStringList &historyOf = {}) const;
    QJsonArray currentUnderstanding(const RequestMeta &audience, const QString &input,
                                    QStringList *keys = nullptr, const QStringList &only = {},
                                    bool includeAudiencePrivate = false) const;
    QJsonArray matterEvidence(const RequestMeta &audience, const QString &matter) const;
    QString conversationPhase(const RequestMeta &audience) const;
    QJsonArray structuredMemoryRows(const QStringList &kinds, const QString &subject,
                                    bool includeOthers, bool allEpisodic) const;

    // ===== 认知快照只读查询（服务端事实分层；不新增表、不改迁移）=====
    // 全部按既有 MemoryReadPolicy / sensitive / 盲区过滤，时间统一 UTC ISO。
    // R12-0：queried/eligible 是可选输出计数（行数），只做可观测性，不改变返回内容/顺序：
    //   queried = 数据库原始命中行数（策略过滤前）；eligible = 通过读取策略后实际返回的行数。
    // 主人陈述：chat_history 中当前 subject 的 user_input 行。
    QJsonArray recentOwnerObservations(const QString &subject,const QString &channel,
                                       const QString &session,int limit=8,
                                       int *queried=nullptr,int *eligible=nullptr) const;
    // 她自己的表达：assistant reply 与 proactive 行（只证明“说过”）。
    QJsonArray recentSelfExpressions(const QString &subject,const QString &channel,
                                     const QString &session,int limit=8,
                                     int *queried=nullptr,int *eligible=nullptr) const;
    // 她自己的理解：matter_revision 最新 understanding + reflection note。
    QJsonArray recentInferences(const QString &subject,int limit=8,
                                int *queried=nullptr,int *eligible=nullptr) const;
    // 自发动机：open concern + active thought（ref/kind/summary/version）。
    QJsonArray currentMotivations(int limit=8,int *queried=nullptr,int *eligible=nullptr) const;
    // 真实委托：目标 channel/session 匹配的 pending reminder。
    QJsonArray activeCommitments(const QString &channel,const QString &session,
                                 int limit=8,int *queried=nullptr,int *eligible=nullptr) const;
    // 私人活动状态/产物：thought 行（ref/status/version/artifact_ref/progress）。
    QJsonArray currentActivities(int limit=8,int *queried=nullptr,int *eligible=nullptr) const;
    // 内部唤醒：attention_state.last_wake_at + 未了结关注的 next_check_at + 已排期的活动回顾。
    // 这些都不是主人约定，不能作为“现在联系”的依据。
    QJsonArray internalWakes(int limit=8,int *queried=nullptr,int *eligible=nullptr) const;
    // canonical subject：主人归一为 'user'，第三方取 actor（没有 actor 时沿用 subject）。
    QString snapshotSubjectId(const RequestMeta &meta) const;
    // R12-0/R12-1a：回顾提示词的服务端时间上下文（只读，不新造状态）。
    // R12-1a 区分“当前会话最近交互”与“跨渠道最近交互”，并保留旧字段别名：
    //   last_owner_session_at / last_owner_cross_channel_at：
    //     当前渠道+会话 / 主人私聊域（pet、qq_private）最近一条 subject=user 的 reply；
    //   last_confirmed_outbound_session_at / last_confirmed_outbound_cross_channel_at：
    //     同范围最近一条已落库的成功表达（turn_state='handed_off' 的 reply/proactive，
    //     沿用现有落库语义：只证明“她确实说过/发出过”，不代表主人读过）；
    //   last_owner_interaction_at / last_confirmed_outbound_at：旧别名（有渠道时=当前会话，
    //     无渠道时=主人私聊域），保持 R12-0 调用方兼容。
    // 跨渠道字段仅在 subject='user' 且来源渠道非群聊时返回（群聊/第三方不扩大私人资料权限）；
    // 没有记录时返回空串。
    QJsonObject reviewInteractionTimes(const QString &channel,const QString &session,
                                       const QString &subject=QStringLiteral("user")) const;

    // 可重建的检索块。原文仍留在原表；这些表只保存检索视图与向量。
    bool rebuildSearchIndex(QString *error = nullptr);
    bool clearSearchIndex(QString *error = nullptr);
    bool backupDatabase(const QString &destination, QString *error = nullptr) const;
    QJsonObject searchIndexStatus() const;
    int memoryChunkCount() const;
    QJsonArray searchMemoryChunks(const QStringList &terms, int limit = 40,
                                  const QString &exactQuery = {}) const;
    QJsonArray memoryChunksForEmbedding(int limit = 2000) const;
    QJsonArray memoryChunksMissingEmbedding(int limit, const QString &model,
                                           qlonglong afterId = 0, QString *error = nullptr) const;
    bool storeChunkEmbedding(qlonglong chunkId, const QString &model, int dimension,
                             const QByteArray &blob, const QString &contentHash);
    QByteArray chunkEmbedding(qlonglong chunkId, const QString &model) const;
    bool embedSearchChunks(MemoryEmbeddingProvider *provider, int limit = 2000,
                           QString *error = nullptr, const MemoryReadPolicy *policy = nullptr);

    QJsonObject claimPrivateReflection(int dailyLimit, int cooldownSeconds,
                                       QString *reason = nullptr, QDateTime *nextEligibleAt = nullptr);
    bool finishPrivateReflection(qlonglong turn, int version, const QString &choice, const QString &note,
                                 const QJsonObject &contact = {}, const QJsonArray &consulted = {},
                                 const QJsonObject &prior = {}, const QString &priorAction = {},
                                 const QJsonObject &concern = {});
    QJsonObject reflectionHistory(qint64 turn,int version,const QJsonArray &rankedTurns,
                                  const QStringList &terms = {}) const;
    bool consultPrivateReflection(qint64 turn, int version,const QStringList &terms,QJsonArray &results) const;
    QString claimReflectionContact(qlonglong turn, int version, const QString &channel, const QString &session);
    bool recordContactOutcome(qlonglong turn, int version, const QString &outcome);
    // 只有真正核验通过、落库为 proposed 的"现在联系"才算提出了联系；观察层据此决定是否
    // 通知投递层，避免依据被拒时仍按模型原始 proposal 触发一次（实际不会发送的）联系。
    bool hasProposedContact(qlonglong turn,int version) const;
    // 上次投递被拒、且还没用完一次重试机会的联系。投递层（5s 调度）据此再次领取；
    // 领取本身由 claimReflectionContact 原子完成，重复轮询不会重复发送。
    QJsonArray retryableReflectionContacts(int limit = 4) const;
    QJsonArray pendingReminders(const RequestMeta &viewer) const;
    bool applyReminders(qint64 turn,const RequestMeta &viewer,const QJsonObject &plan);
    QJsonObject claimDueReminder(const QString &channel,const QString &session);
    void reminderOutcome(qint64 id,const QString &state);
    // R12-0 失败退避：backoffSeconds>0 时写 eligible_at=unixepoch()+backoffSeconds；
    // 默认 0 保持原有"立即可重领"语义。仅 Reflection 使用非零退避（内存计数为临时方案，
    // 跨重启可靠计数留给 R12-1a v31 的持久化字段）。
    bool deferPrivateReflection(qlonglong turn, int version, int backoffSeconds = 0);
    QJsonObject recentPrivateReflection(const RequestMeta &viewer, const QJsonArray &rankedTurns = {}, bool requireRanked = false) const;
    // 关注解析器：与当前输入相关的开放关注。前台注入与后台回顾共用同一份选择逻辑，
    // 不再要求"旧关注的来源轮次恰好被检索命中"。
    QJsonArray matchedConcerns(const RequestMeta &viewer, const QStringList &terms,
                               const QString &queryText = QString(),
                               qint64 excludeTurn = 0, int limit = 2,
                               const QString &beforeReported = QString()) const;
    QJsonObject concernByHead(qint64 headTurn) const;
    // 到期回顾：不需要新用户消息，也不要求检索命中。领取时占用与回顾相同的预算/冷却。
    // reason/nextEligibleAt 是 R12-0 诊断输出：no_candidate/daily_limit/cooldown/db_error/
    // storage_error/cas_conflict；nextEligibleAt 仅在已知下次可领取时刻时填写。
    QJsonObject claimDueConcern(int dailyLimit, int cooldownSeconds,
                                QString *reason = nullptr, QDateTime *nextEligibleAt = nullptr);
    QJsonObject concernReviewContext(qint64 turn, int version) const;
    bool finishConcernReview(qlonglong turn, int version, const QJsonObject &proposal);
    // 只有 contact.timing=="now" 需要可核验的行动依据；返回空串=通过，否则返回拒绝原因
    // （missing_basis / invalid_basis_type / invalid_basis_ref / stale_owner_evidence /
    //   artifact_already_shared / time_delegation_owned_by_reminder）。
    // 时间委托不再作为 now 的依据：提醒链会在到期时自己发送，回顾不能代表它再发第二次。
    QString validateContactBasis(const QString &channel,const QString &session,
                                 qlonglong concernTurn,const QJsonObject &contact) const;
    bool deferConcernReview(qlonglong turn, int version, const QString &reason);
    // 从正常对话了结一个关注：答案到达、被否定或计划取消。提交后不再注入、不再重复追问。
    bool resolveConcern(qlonglong headTurn, qlonglong byTurn, const QString &kind,
                        const QString &evidenceRef);
    // 认识更新队列的可观测性：用于区分"模型判断错"与"新资料还没处理到"。
    QJsonObject observationQueueStatus() const;
    QJsonObject nextThought(bool dueOnly) const;
    bool claimThoughtReview(qlonglong id, int version, int dailyLimit, int cooldownSeconds,
                            QString *reason = nullptr, QDateTime *nextEligibleAt = nullptr);
    bool deferThoughtReview(qlonglong id, int version, const QString &reason);
    bool finishThoughtReview(qlonglong id, int version, const QJsonObject &proposal);
    // Activity actions are conditional on a stable row id and the expected
    // thought_version, so a late review callback cannot revive a paused or
    // abandoned activity.
    bool applyThoughtAction(qlonglong id, int expectedVersion, const QString &action,
                            int afterMinutes = -1, const QString &reason = {});
    bool hasActiveThought(const QString &subject = QStringLiteral("user")) const;
    // 分享去重：已送达（handed_off/protocol_accepted）不再分享；被拒最多补一次；
    // 协议层"未知"不重发（可能已送达）。
    bool canShareThought(qlonglong id, int expectedVersion) const;
    // 分享回执：把结果补进 thought_result 并存 delivery_ledger（kind='share'）。
    bool recordShareOutcome(qlonglong activityId, int version, const QString &outcome,
                            const QString &channel, const QString &session);
    // 普通对话回复的投递账本（kind='reply'，与 contact/share 共用 delivery_ledger，无迁移）。
    // 状态机：claimed --回执--> protocol_accepted | handed_off | rejected | unknown。
    // rejected 且 attempts<maxAttempts 才允许补发；unknown 表示协议层可能已送达，绝不重发。
    // recordDelivery 是幂等 upsert：重复 claim 刷新路由/原文但保留 attempts；
    // 发送方在每次提交（首发送/补发）前用 recordDeliveryAttempt 递增，回执用
    // recordDeliveryOutcome 落终态。所有写操作失败返回 false，不抛异常。
    bool recordDelivery(const QString &kind,const QString &ref,const QString &channel,
                        const QString &session,const QString &text);
    bool recordDeliveryAttempt(const QString &kind,const QString &ref);
    bool recordDeliveryOutcome(const QString &kind,const QString &ref,const QString &outcome,
                               const QString &detail = QString());
    // 仍可补发的投递：state='rejected' 且 attempts<maxAttempts，按 id 升序。
    QJsonArray retryableDeliveries(const QString &kind,int maxAttempts = 2,int limit = 20) const;

    // B1：统一注意力事件。事件表是通用的"注意力待办"队列，不绑定单一调用方：
    // 消息到达、到期唤醒、活动结果、崩溃恢复都按 (kind,ref) 幂等入队，协调器按
    // priority 领取并落终态（done/abandoned）。payload 只做可观测/恢复用，不承载业务状态。
    qlonglong enqueueAttentionEvent(const QString &kind, const QString &ref,
                                    const QJsonObject &payload = {}, const QString &reason = {},
                                    int priority = 50);
    // 领取 pending 中 priority 最小（最高）且 id 最早的一条，置 claimed。
    QJsonObject claimAttentionEvent();
    // 只允许 pending/claimed → done/abandoned；影响 1 行才成功。
    bool completeAttentionEvent(qlonglong id, const QString &state, const QString &reason = {});
    QJsonObject attentionBacklog() const;
    // 当前活动/切换原因等"当下状态"的键值存档（attention_state）。
    void setAttentionState(const QString &key, const QString &value);
    QString attentionState(const QString &key) const;
    // ===== R12-2c-2：Action Context Fence（行动相关语境版本）=====
    // 三维区分：origin_version=来源/成果版本（ActionOrigin.version）；
    //           evidence_revision=真实证据修订（artifact_version/hash 或来源修订）；
    //           action_context_epoch=当前行动相关语境的单调版本（本接口）。
    // epoch 以十进制整数持久化在既有 attention_state（键 action_context_epoch），
    // 不新增表、不清零：重启后从 SQLite 原值读回。提案落库时快照到
    // action_proposal.action_context_epoch；提交/claimOutbox/实际发送前都要比较，
    // 不一致则提案 expired/cancelled 审计、动作不发送（历史事实/成果永不删除）。
    qint64 actionContextEpoch() const;
    // 递增并返回新 epoch；失败返回 -1（不改变已存值）。reason 只做审计（最长 80 字），
    // 同时刷新 action_context_updated_at/reason 两个 attention_state 键。
    // 触发点只应是"与主人当前行动相关"的变化（主人私聊输入、成果撤回等），
    // 无关群聊/索引/摘要不得调用。
    qint64 bumpActionContextEpoch(const QString &reason = QString());
    // 扫描进程中断遗留的 received 轮次，幂等入队 recovery 事件；返回新入队条数。
    int enqueueRecoveryEvents(int olderThanSeconds = 120);
    // 启动恢复：把租约已过期仍停在 pending 的关注回顾放回可重领状态。
    // 到期关注回 consider（立即可再领取），新来源回顾回 deferred（交由 claimPrivateReflection 重领）。
    int recoverStaleReflections(int staleSeconds = 1800);
    // 启动恢复：投递层领取后（dispatching）超过 staleSeconds 仍未回执，视为进程崩溃遗留。
    // at-most-once：置为 unknown（协议层可能已送达），不重发；返回本次恢复条数。
    int recoverStaleContacts(int staleSeconds = 600);

    // ===== 切片 B：cognitive_action + action_outbox（v29）=====
    // 三条主动外发（ReminderDelivery / SpontaneousContact / ActivityShare）统一走
    // "域状态 CAS + cognitive_action + action_outbox 同一事务"，提交后由 OutboxDispatcher
    // 唯一发送，拿到回执再 settle。事务内绝不调用 QQ/网络。
    // 已有同 action_key 时幂等返回其当前状态（不再重复建 outbox）。
    QJsonObject commitReflectionContactAction(qlonglong turn,int version,
                                              const QString &policyReason = {});
    QJsonObject commitReminderDeliveryAction(qlonglong reminderId);
    QJsonObject commitActivityShareAction(qlonglong activityId,int version,const QString &draft);
    // outbox 领取：state='queued'（或失败的受控重试）→ dispatching，写租约并 attempts+1；
    // 同一行不会被第二个领取者拿到。返回 {action_key,kind,channel,session,payload,payload_json,...}。
    // R12-2c-2：领取时先做发送前最终复验（见 validateOutboxActionBeforeSend）。
    //   * 永久失效（提案未 committed/已取消/已过期/epoch 变化/verified 内容指纹不符/
    //     成果撤回/权限关闭/action 已终态等）→ 行落 outbox failed + action withheld、
    //     账本 withheld、提案 cancel_reason 审计，然后继续找下一条，绝不发送/重领；
    //   * 可恢复阻塞（安静时段/每日额度/目标暂时不可达）→ 保留 queued（不占 attempts），
    //     本次返回空，等后续轮询；
    //   * withheld/unknown/handed_off 对应行（含 outbox=failed 的 withheld 遗留）不可重领。
    QJsonObject claimOutbox(const QString &owner,int leaseSeconds = 60);
    // R12-2c-2：发送前最终复验（只读分类，不发送）。dispatcher 在 sender 前调用；
    // AppController 的渠道 sender 在"真正入队"这个最后可取消边界再调用一次。
    // 返回 {ok:bool,permanent:bool,reason:QString,action_state,outbox_state,channel,session}。
    // 新来源（wake:/activity:）额外复验：提案 state=committed、cancel_reason、expires_at、
    // action_context_epoch 与当前 attention_state epoch、verified_payload_hash/
    // verified_claims_hash 与权威 payload/claims、target/开关/权限/安静时段/共享额度、
    // 来源版本/runId/scope/target、成果 artifact_ref/version/hash/share_intent。
    // legacy（reminder/reflection/thought）保留既有领域有效期语义，只做状态一致性复验。
    QJsonObject validateOutboxActionBeforeSend(const QString &actionKey);
    // 可恢复阻塞：把刚领取的 dispatching 行放回 queued（attempts 回退，不算一次发送），
    // action 回 queued；用于安静时段/额度/目标暂不可达。仅用于新来源动作。
    bool deferOutboxAction(const QString &actionKey,const QString &reason);
    // 永久失效取消：一个事务内 outbox=failed、action=withheld、领域账本=withheld，
    // 并把未提交的提案置 expired/已提交提案写 cancel_reason 审计；幂等。绝不发送。
    bool cancelOutboxAction(const QString &actionKey,const QString &reason);
    // 回执/结果落库：一个事务内同时更新 action/outbox 与对应领域行（contact/reminder/share）。
    // outcome ∈ protocol_accepted|handed_off|rejected|unknown|withheld。
    // rejected 且 outbox attempts<2 → 回 queued（受控一次重试）；unknown → 冻结，永不重发。
    bool settleOutboxAction(const QString &actionKey,const QString &outcome,
                            const QString &detail = {});
    // 启动恢复：dispatching 且租约过期超过 staleSeconds → action/outbox 都置 unknown，
    // at-most-once，绝不自动重发；返回本次恢复条数。
    int recoverStaleOutbox(int staleSeconds = 600);
    // 读取一条活动念头（thought_result 快照），用于能力清单里的"当前活动"。
    QJsonObject activityById(qlonglong id) const;
    // 写一条"茉子主动说出的话"到历史与 raw_events，供下次对话回认与检索。
    // 主动轮次 user_input 为 NULL、turn_kind='proactive'，不参与回顾/认识更新。
    qlonglong recordProactiveMessage(const QString &channel, const QString &session, const QString &text,
                                     const QString &kind, const QString &ref);

    // ===== R12-1a：持久化回顾失败 / 退避 / 暂停（v31）=====
    // 四类来源（reflection|concern|activity|open_wake）共用 review_failure：
    //   recordReviewFailure 在同一个事务内更新失败行与对应源状态（各步影响行数都要为 1），
    //   countsAsFailure=false（权限/策略拦截、用户主动让出）只记录 last_stage/last_reason，
    //   不增加计数、不改退避/源状态，由调用方决定是否 defer；
    //   clearReviewFailure 在成功提交后清行；resetReviewFailure 是管理员重置（清行并让源立即可领取）。
    // 来源实质修订（content_hash 变化）会把连续失败计数重置为 0 后再计本次失败；
    // 普通领取 bump 的 version 不参与重置判断，避免把正常重试误判成修订。
    QString classifyReviewError(const QString &error) const;
    QJsonObject reviewFailure(const QString &sourceKind,const QString &sourceRef) const;
    // R12-2c-3：expectedVersion>0 时先比对源当前版本；不一致说明这是旧版本尝试的迟到失败，
    // 不写失败行、不推进新版本来源的退避状态（只打 [Review] stale_failure_ignored 审计）。
    bool recordReviewFailure(const QString &sourceKind,const QString &sourceRef,
                             const QString &stage,const QString &reason,
                             bool countsAsFailure,QString *error=nullptr,
                             int expectedVersion=0);
    bool clearReviewFailure(const QString &sourceKind,const QString &sourceRef);
    bool resetReviewFailure(const QString &sourceKind,const QString &sourceRef);
    // R12-1a：关注保留期终止定时重试。扫描 state='consider' 且仍有定时检查
    // （next_check_at>0）的关注：若当前时间已越过 reported+retention，或失败行的
    // next_retry_at/suspended_until 已越过该截止，则在同一事务里清 failure 行并把
    // next_check_at 置 0（state 保持 consider，note/topic/fact 不动，后续合法新证据
    // 仍可按 matchedConcerns 命中/更新）。返回终止条数；-1 表示数据库错误。
    // 日志：[Review] retry_expired kind=concern ref=... reported=... retention_days=...
    int expireConcernScheduledRetries(int retentionDays);
    // 今日回顾预算诊断（只读）：total + 各 source 次数。
    QJsonObject reviewBudgetUsage() const;

    // ===== R12-1a：OpenWake 状态 / 预算 / 公平 / 租约（本片仅 rest|reflect）=====
    // 调度状态与租约都在 SQLite 里：重启后由 recoverOpenWakeLeases 恢复过期租约；
    // 领取/提交/延后/取消都是带 version CAS 的单一事务。本片不创建内部活动，
    // 也不迁移存量 thought 活动；internal_activity 只提供只读访问骨架。
    QJsonObject openWakeState(const QString &scopeKey=QStringLiteral("owner")) const;
    bool ensureOpenWakeScheduled(const QString &scopeKey,int initialDelaySeconds,
                                 QString *error=nullptr);
    // reason 输出：budget|cooldown|busy|suspended|not_due|no_candidate|db_error。
    QJsonObject claimOpenWake(int dailyLimit,int cooldownSeconds,int openWakeDailyLimit,
                              int sharedBudgetPercent,int maxConsecutiveFailures,
                              QString *reason=nullptr,QDateTime *nextEligibleAt=nullptr,
                              const QString &scopeKey=QStringLiteral("owner"));
    bool finishOpenWake(qlonglong id,int version,const QString &decision,const QString &note,
                        const QJsonObject &intent,int nextWakeMinutes,QString *error=nullptr);
    // R12-1b：OpenWake 的 start_activity 提交。与 finishOpenWake 一样是带 version CAS 的
    // 单一事务：先创建 internal 活动（origin_kind=open_wake、origin_ref=wake/<id>、
    // run_id=open_wake.run_id，单 active 跨存储检查），再完成 open_wake/更新调度状态；
    // 任一步失败整体回滚，绝不出现"只建活动"或"只完成 wake"的半写状态。
    // decision 必须为 start_activity；activityProposal 字段：
    //   interest（必填，≤120）、goal（可空，≤200）、initial_review_after_minutes（可选 5..1440，
    //   默认 30）、channel/session（可选，只允许主人私聊域 pet/qq_private，默认 pet/default）。
    // 成功时 result 输出新活动统一视图（ref=activity/<id>），error 为空；
    // 失败（already_active/db_error/cas_conflict/invalid_*）返回 false 并写 error。
    bool finishOpenWakeWithActivity(qlonglong id,int version,const QString &decision,
                                    const QString &note,const QJsonObject &intent,
                                    int nextWakeMinutes,const QJsonObject &activityProposal,
                                    QJsonObject *result=nullptr,QString *error=nullptr);
    bool deferOpenWake(qlonglong id,int version,int retrySeconds,const QString &reason);
    bool cancelOpenWake(qlonglong id,int version,const QString &reason);
    int recoverOpenWakeLeases(int staleSeconds=1800);
    // R12-1a：最近 committed rest/reflect 非空 note（ref/version/decision/note/at），
    // 上限 20；只读。scopeKey 过滤 open_wake.scope_key，默认只读主人私聊域 owner；
    // 调用方（快照 builder/PrivateReview）只传 owner，不扩大任何读取范围。
    QJsonArray recentOpenWakeNotes(int limit=2,
                                   const QString &scopeKey=QStringLiteral("owner")) const;
    // 统一活动访问骨架（只读）：legacy=chat_history thought，internal=internal_activity。
    // hasActiveInternalActivity 合并检查两种来源的 active/paused；currentActivitiesUnified
    // 返回带 origin 字段的合并列表，不改变既有 currentActivities 的返回形状。
    bool hasActiveInternalActivity() const;
    QJsonArray currentActivitiesUnified(int limit = 8,int *queried=nullptr,
                                        int *eligible=nullptr) const;

    // ===== R12-1b：统一活动存取 / 生命周期（legacy 适配 + internal 新写）=====
    // 引用格式见 activityref.h：legacy=thought/<id>，internal=activity/<id>。
    // 新建活动只写 internal_activity；存量 thought 活动通过适配器读写，不复制/不伪造聊天行。
    // 所有写接口返回 QJsonObject；失败返回空对象并把原因写入 error（可为 nullptr）。
    // createActivity：事务内校验 + 同 (origin_kind,origin_ref,run_id) 幂等 + 两存储
    //   active/paused 单活动检查；成功返回 {ref,id,version,status,review_after,origin,...}。
    //   新建只允许 originKind=foreground/open_wake、scopeKey=owner、runId 非空。
    QJsonObject createActivity(const QString &scopeKey,const QString &channel,const QString &session,
        const QString &interest,const QString &goal,const QString &originKind,const QString &originRef,
        const QString &runId,QString *error=nullptr);
    // 统一视图：任何状态的 legacy/internal 活动；不存在返回 {}。字段含
    // ref/origin/kind/id/status/version/goal/progress/next_step/draft/artifact_ref/
    // artifact_version/artifact_hash/share_intent/review_after/channel/session/updated_at 等
    // （按来源可用字段）。
    // R12-1b 补充：activityByRef 只作管理读取（内部/测试/核验），**不是模型入口**；
    // 所有模型可见读取必须走 activityForModel / currentActivitiesForModel /
    // recentActivityResultsForModel 的投影（只读、按 scope/subject/channel/session/盲区/
    // 长期记忆/隐私规则过滤，嵌套字段同样过滤）。
    QJsonObject activityByRef(const QString &ref) const;
    // 模型可见投影：不可见返回 {}。调用方传入模型本轮所在的 channel/session/subject
    // 与长期记忆开关；投影要求 scope=owner、subject=user、channel 属于主人私聊域
    // （pet/qq_private；空渠道表示"只知道是主人私聊来源"）、会话不在盲区、长期记忆开启，
    // 且活动自身的渠道/会话也满足同一可见性；sensitive/privacy 命中的整活动不可见。
    // 嵌套字段（goal/progress/next_step/draft/reflection/mood/paused_reason/
    // artifact_ref/share_intent 等）逐项按读取策略过滤；completed 不返回过时 next_step。
    QJsonObject activityForModel(const QString &ref,const QString &channel,
                                 const QString &session,const QString &subject,
                                 bool longTermMemoryEnabled) const;
    // 当前 active/paused 活动的模型可见列表（limit 上限 8）；不可见活动直接跳过。
    QJsonArray currentActivitiesForModel(int limit,const QString &channel,
                                         const QString &session,const QString &subject,
                                         bool longTermMemoryEnabled) const;
    // 完成成果目录的模型可见投影（limit 上限 8）：只含真实有成果的 completed；
    // 群聊/第三方/盲区/长期记忆关闭时为空。字段：ref/version/artifact_ref/
    // artifact_version/status/goal/title/summary/completed_at，不含过时 next_step。
    QJsonArray recentActivityResultsForModel(int limit,const QString &channel,
                                             const QString &session,const QString &subject,
                                             bool longTermMemoryEnabled) const;
    // 有界完成成果目录（管理/快照内部读取；limit 上限 8）。只返回 status=completed 且
    // artifact_ref 能指回真实成果（正文非空）的活动，按完成时间倒序；旧 completed 记录
    // 只读兼容（缺少 artifact_version/hash 时按行版本兜底），不追溯改写。
    QJsonArray recentActivityResults(const QString &scopeKey=QStringLiteral("owner"),
                                     int limit=8) const;
    // 两存储 active/paused 统一判断（scope 隔离：legacy 只属于 owner）。
    bool hasActiveActivity(const QString &scopeKey=QStringLiteral("owner")) const;
    // 两存储中 status=active、review_after<=now、sensitive=0 的最早候选；
    // 返回 {ref,kind,version,activity}（kind=legacy|internal），无则 {}。
    // R12-1b 补充：只有对 owner 私聊可见（activityForModel 非空）的活动才进入候选；
    // 不可见候选在查询层跳过，不阻塞其它合法活动。
    QJsonObject nextDueActivity(const QString &scopeKey=QStringLiteral("owner")) const;
    // 统一领取：legacy 走 claimThoughtReview，internal 走 version CAS + 共享预算/冷却；
    // 成功返回 {ref,kind,id,version,activity,origin_kind,run_id,scope_key}。
    // reason 输出与现有领取一致：daily_limit/cooldown/cas_conflict/db_error/not_due；
    // R12-1b 补充：提交前再次复核模型可见投影（scope/subject/渠道/会话/盲区/长期记忆/
    // 隐私），不可见活动返回 not_visible，不进入 ActivityReview。
    QJsonObject claimActivity(const QString &ref,int expectedVersion,int dailyLimit,
        int cooldownSeconds,QString *reason=nullptr);
    // 统一提交：legacy 走 finishThoughtReview，internal 单事务 CAS 写 status/result_json/
    // artifact_ref/artifact_version/artifact_hash/review_after/version；返回
    // {status,version(新),ref[,artifact_ref/artifact_version/artifact_hash/completed_at]}。
    // R12-1b 补充（成果真实性）：
    //   * status 缺失/类型错误/枚举非法 → status_missing/status_type_invalid/status_enum_invalid；
    //   * completed 的**文字创作**必须有本次或已持久化的完整 draft（可沿用已有草稿），
    //     否则 missing_artifact，绝不自动改成 completed；
    //   * completed 的**纯思考**必须已有/本次给出 completion_condition 且 reflection 非空，
    //     否则 missing_completion_condition / missing_reflection；
    //   * 未接入执行工具的外部活动（browser/painting 等）不能 completed；提案携带
    //     external_action/external_result/execution_result 或 activity_kind 为外部类型时
    //     返回 unsupported_external_activity/invalid_activity_kind；
    //   * 保存成果时由服务端写 artifact_ref=activity/<id>、artifact_version=提交后版本、
    //     artifact_hash（draft 的 SHA-256 前 16），模型自填 artifact_ref 不生效；
    //   * 分享意愿（share_with_owner/share_request）与完成状态在同一事务提交，
    //     share_intent 只绑定服务端成果 ref/版本/hash；请求不合法 → share_intent_invalid，
    //     整体回滚。
    // 旧 completed 记录只读兼容，不做追溯改写。
    QJsonObject finishActivity(const QString &ref,int version,const QJsonObject &proposal,
        QString *error=nullptr);
    // 统一延后：legacy 复用 deferThoughtReview；internal CAS 只推 review_after，不改 lifecycle。
    bool deferActivity(const QString &ref,int version,int retrySeconds,const QString &reason);
    // 统一动作：pause/resume/abandon/complete/reschedule。legacy 复用 applyThoughtAction
    // （complete 不支持，返回 unsupported_action）；internal 在单事务内做状态机 + CAS，
    // resume 会做跨存储 active/paused 检查（排除自身）。返回 {ref,status,version,origin}。
    // R12-1b 补充：动作提交前同样复核模型可见投影（不可见返回 not_visible）；
    // complete 与 finishActivity 使用同一套成果真实性校验（missing_artifact 等）。
    QJsonObject applyActivityAction(const QString &ref,int expectedVersion,const QString &action,
        int afterMinutes,const QString &reason,QString *error=nullptr);
    // 分享意愿只落 internal_activity.result_json.share_intent，不写 delivery_ledger/
    // action_outbox/chat_history、不发消息；legacy 返回 unsupported_legacy。
    // 旧签名保留兼容：等价于 action=share、自动递增 share 版本、request 最多 120 字。
    bool setActivityShareIntent(const QString &ref,int version,const QString &draft,
        QString *error=nullptr);
    // R12-1b 补充：显式分享版本语义（修改/撤回/重新分享）。
    //   action=share|modify|withdraw；expectedVersion 是活动当前版本（CAS）；
    //   shareVersion<=0 时自动=已有 share 版本+1，显式给值时必须严格大于旧版本；
    //   只接受当前已完成成果的 artifact_ref/artifact_version/artifact_hash 匹配
    //   （重新计算 draft 哈希核对），不接受任意 draft 冒充成果，也不允许无成果分享；
    //   withdraw 置 state="withdrawn" 并保留历史；修改/重新分享产生新的 share 版本；
    //   历史版本只保留有界最近几条。成功返回 true，失败写 error。
    bool setActivityShareIntent(const QString &ref,int expectedVersion,const QString &action,
        const QString &request,int shareVersion,QString *error=nullptr);
    // 启动恢复诊断（只读、幂等）：报告两存储 active/paused 的跨存储冲突与 internal 到期
    // 列表；不删除活动、不伪造完成。
    QJsonArray recoverActivities(const QString &scopeKey=QStringLiteral("owner"));

    // ===== R12-2a：通用 ActionOrigin / ActionProposal 持久化（v32）=====
    // 提案（proposal）只记录"准备做什么"，本片**不发送**、不建 Outbox、不写 cognitive_action：
    //   proposed（持久化）→ verified（统一核验通过）→ committed（R12-2b 才接发送）
    //                      ↘ withheld / expired / unknown（任一态终态，不再自动推进）
    // 身份/target 由 ActionOrigin 派生：输入 proposal 必须包含
    //   {origin:{...ActionOrigin...}, kind:"contact"|"activity_share",
    //    target:{channel,session_id}（可选，必须与 origin 一致）,
    //    payload:{...}, claims:[...], expires_at:0}
    // action_key 由 origin+kind 唯一确定（见 actionorigin.h）；库里 action_key UNIQUE。
    // 服务端字段（state/semantic_status/origin*/target*/时间戳）不接受调用方伪造。
    // 本入口只校验身份"结构/派生一致性"；来源行在库内是否真实存在、版本/runId/scope
    // 是否对得上，由 ActionPolicy::validateActionProposal 统一核验后再进入本持久化层。
    //
    // 幂等：同 action_key 且 kind/target/payload/claims/expires_at 完全一致时返回已存在行
    //（result["idempotent"]=true，绝不覆盖成新 payload）；不一致返回
    // {"error":"conflict","action_key":...}。非法输入返回 {"error":<原因码>}。
    QJsonObject persistActionProposal(const QJsonObject &proposal, QString *error = nullptr);
    // 只读回读：行转 JSON（origin/target/payload/claims 解析为对象/数组）；不存在返回 {}。
    QJsonObject actionProposal(const QString &actionKey) const;
    // 状态 CAS：仅当当前 state==fromState 时推进；非法状态/非法迁移/0 行返回 false。
    // semanticStatus 只能由服务端（本接口调用方）写入；空串表示保持 NULL。
    // 允许的迁移：proposed→verified|withheld|expired|unknown；
    //             verified→committed（仅 markProposalCommitted）|withheld|expired|unknown；
    //             committed/withheld/expired/unknown 为终态。
    // R12-2c-2：toState='verified' 一律拒绝（verified_binding_required）——verified 必须经
    // markActionProposalVerified 在同一事务写入内容指纹/request_id/verified_at/fence epoch。
    bool updateActionProposalState(const QString &actionKey, const QString &fromState,
                                   const QString &toState, const QString &semanticStatus,
                                   const QString &errorCode = QString(), QString *error = nullptr);
    // R12-2c-2：proposed→verified 的唯一入口（单事务）。重新读取权威提案行后：
    //   * state 必须是 proposed（verified/committed 幂等返回 true；其它终态失败）；
    //   * semantic_status 必须是服务端封闭枚举且非 withheld；
    //   * verified_request_id 必须非空（verifier 请求的服务端关联 id）；
    //   * 从库内 payload/claims 重新计算 verified_payload_hash/verified_claims_hash
    //     （与 verifier 输入同一规范化算法）并持久化，同时写 verified_at；
    //   * action_context_epoch 必须仍等于当前 fence epoch（主人私聊插话/成果撤回后旧在途
    //     核验失效），不一致 → 置 expired + cancel_reason=context_epoch_changed，返回 false。
    // 任一步失败整体回滚；成功后提交/发送前还会再次复核这些绑定。
    bool markActionProposalVerified(const QString &actionKey,const QString &semanticStatus,
                                    const QString &verifiedRequestId,QString *error = nullptr);
    // 取消审计（幂等、单事务）：未终态（proposed/verified）→ expired；committed 保持 committed
    // （回执闭环需要），只写 cancel_reason 供审计；绝不删除历史事实/成果。
    bool cancelActionProposal(const QString &actionKey,const QString &reason);
    // 扫描 proposed/verified 且 expires_at>0 且已过期的提案 → expired；返回更新行数（幂等）。
    // now=0 表示使用数据库当前时间（unixepoch）。
    int expireActionProposals(qint64 now = 0);
    // verified→committed，并记录实际提交的 cognitive_action.action_key（R12-2b 使用）。
    // 只接受非空 committedActionKey；0 行/状态不是 verified 返回 false 并写原因。
    bool markProposalCommitted(const QString &actionKey, const QString &committedActionKey,
                               QString *error = nullptr);

    // ===== R12-2b：verified 提案的原子提交 / 回执 / 恢复 =====
    // R12-2c-2 起依赖 v33 的 verified_payload_hash/verified_claims_hash/verified_request_id/
    // verified_at/cancel_reason/action_context_epoch 列（历史投影字段预留）。
    // 从 action_proposal 读 state='verified' 的提案，再次用 ActionPolicy::validateActionProposal
    // 复核来源/版本/runId/scope/epoch/target/成果新鲜度/功能开关/权限/coverage，再复核安静时段、
    // 可达目标与共享 contact 额度；通过后在**单事务**内写 cognitive_action + action_outbox +
    // delivery_ledger，并把提案 CAS verified→committed。**只入 Outbox，绝不直接调 QQ/pet 发送。**
    // 返回 {action_key,action_id,kind,channel,session,state,proposal_state}；失败返回
    // {error:<code>}。幂等：同 action_key 已有动作行 → 返回既有一行（idempotent=true），
    // 同 key 不同 payload/来源 → error=duplicate，不重复建 outbox。
    // 错误码：invalid_action_key/not_found/not_verified/invalid_state/invalid_kind/
    //         allow_contact_disabled/target_unreachable/quiet/limit/duplicate/stale_origin/
    //         artifact_mismatch/policy_rejected/db_error。
    // 不可恢复的失败（来源失效/成果不符/allow_contact 关闭/重复）把提案落 withheld；
    // 可恢复的失败（quiet/limit/target_unreachable/db_error）保留 verified，交由
    // drainVerifiedActionProposals 在之后的启动/定时点重试，不丢提案。
    // R12-2c-2：进入事务后重新读取权威提案行并复验 state='verified'、来源/版本/runId/scope/
    // context_epoch/target/expires、成果 artifact_ref/version/hash、verified_request_id 绑定、
    // verified_payload_hash/verified_claims_hash 与即将提交的 payload/claims 完全一致，
    // 以及 action_context_epoch 未变；任一不一致 → conflict/stale/expired，不写任何动作行。
    // 语义网络请求只在本函数之外（LLMService 核验链），事务内绝不等待网络。
    QJsonObject commitVerifiedActionProposal(const QString &actionKey,QString *error = nullptr);
    // 便捷入口：先确认 kind/origin 再提交（wake→contact、activity→share）。
    QJsonObject commitVerifiedOpenWakeContact(const QString &actionKey,QString *error = nullptr);
    QJsonObject commitVerifiedInternalActivityShare(const QString &actionKey,QString *error = nullptr);
    // 启动/定时恢复：有界扫描 verified 且未过期的提案并逐条提交；已 committed 不重发，
    // 已在 outbox 的由 dispatcher 恢复。返回 {scanned,committed,withheld,failed,results:[]}。
    QJsonObject drainVerifiedActionProposals(int limit = 8);
    // actionSettled 回执回写（幂等，只处理新来源 wake:/activity:；legacy 回执仍走既有
    // settleOutboxAction 语义）。写 delivery_ledger 终态；handed_off/protocol_accepted 时
    // 回写 open_wake_state.last_outcome / internal_activity.result_json.share_outcome/share_version；
    // unknown/rejected/withheld 不写成功、不重发。重复回执不重复累加。
    // R12-2c-3：以 action_proposal.history_projected 为"历史已投影"持久化标记，成功回执在同一
    // 事务内写主动历史并置 1；不可投影（非本地/协议成功）置 2 审计；unknown/withheld 永不写成功历史。
    // 校验：动作存在、kind 与 action_key 前缀一致、提案 committed、Outbox 路由与动作行一致；
    // 不存在/不匹配只审计，不写 ledger/历史。活动分享历史绑定原成果 artifact_ref/version/hash。
    bool recordActionReceipt(const QString &actionKey,const QString &outcome,
                             const QString &detail = QString());
    // 启动对账：扫描所有"已成功回执但 history_projected=0"的新来源动作（状态标记扫描，
    // 不再固定最早 64 条；每轮有界多批，投影后标记 1/2 因而不会饿死后续行），幂等、可重入、
    // 重启补齐；已投影（1/2）不回退，不存在/冲突只审计。返回本次处理条数。
    int reconcileActionReceipts();

    // ===== R12-2c-3：internal activity share 的崩溃恢复 =====
    // 扫描已完成活动的有效未处理分享意愿（只有 status='completed'、share_intent.state='saved'、
    // 成果 ref/版本/哈希仍匹配、未撤回、无同成果版本提案、无成功/未知分享回执的意图）。
    // 单事务内有界领取（limit<=8），并把 share_intent.recovery_state 置 'requested'+recovery_at；
    // 非 force 时同一意图在 retryAfterSeconds 内不重复领取（避免每 tick 重复核验）。
    // 成果已被修改/撤回等不再发送的旧意图只写 recovery_state='withheld'+原因（活动状态不变）。
    // 返回行：{ref,activity_id,channel,session,activity_version,artifact_ref,artifact_version,
    //          artifact_hash,draft,intent}。绝不把所有历史成果自动加入分享队列。
    QJsonArray claimPendingActivityShareIntents(int limit,bool force=false,
                                                int retryAfterSeconds=300);
    // 恢复/构建失败时把意图标记 withheld（审计；不改活动完成状态、不删历史/成果）。
    bool markActivityShareIntentWithheld(const QString &ref,int artifactVersion,
                                         const QString &reason);
    // 正常完成路径发出核验请求时标记 requested（设置 recovery_at 节流），避免恢复扫描
    // 在 verifier 在途时重复发起同一成果版本的核验；提案落库后扫描按唯一键跳过。
    bool markActivityShareIntentRequested(const QString &ref,int artifactVersion);
    // R12-2c-3：每次真实回顾尝试独立 run_id。领取时 thought_review_log 先落行，runId 生成后
    // 由调用方回填最近一条空 run_id 的领取日志；迟到失败据此可区分旧尝试。
    bool attachReviewAttemptRunId(const QString &sourceKind,const QString &sourceRef,
                                  const QString &runId);

    // R12-2b：服务端选择主动投递目标（按配置顺序、宠物优先、跳过盲区/不可读文本）。
    // 返回 {found,channel,session}；仅用于提案 origin 的 target，真实可达性由 dispatcher 决定。
    QJsonObject chooseProactiveTarget(const QString &draftLikeText,bool requireShareable) const;

//历史对话crud
    QList<HistoryTurn> getHistoryTurn(int N, const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession, qint64 beforeTurn = 0);
    QList<HistoryTurn> getHistoryTurn(int offset,int limit); // 管理界面分页，跨渠道浏览
    qlonglong getTotalHistoryCount();
    bool deleteTurnByID(int id);
    bool clearAllHistory();

//原始事件流水（raw_events）：Phase 1 由 QQ/感知渠道写入；本阶段只提供方法，不接线
    bool appendRawEvent(const QString &platform,
                        const QString &channel,
                        const QString &sessionId,
                        const QString &eventType,
                        const QString &content,
                        const QString &messageId = QString(),
                        const QString &direction = QStringLiteral("in"),
                        const QString &senderId = QString(),
                        const QString &senderName = QString(),
                        const QString &payloadJson = QString(),
                        const QDateTime &occurredAt = QDateTime(),
                        bool sensitive = false,
                        const QString &visibility = QStringLiteral("private"));

    bool appendRawEvent(const RawEventWrite &event);
    bool appendPrivacyBlockedEvent(const QString &platform,
                                   const QString &channel,
                                   const QString &sessionId,
                                   const QString &messageId,
                                   const QString &senderId,
                                   const QString &senderName,
                                   const QDateTime &occurredAt,
                                   const QStringList &ruleIds,
                                   const QString &policyVersion);

    // PR-2: raw_events 消费与 interactions
    QList<RawEvent> getUnprocessedGroupEvents(const QString &sessionId, int limit);
    QList<QString> getPendingGroupSessions();
    int countUnprocessedGroupEvents(const QString &sessionId);
    QDateTime latestUnprocessedGroupEventTime(const QString &sessionId);
    qlonglong createInteractionBatch(const InteractionDraft &draft,
                                     const QList<qlonglong> &eventIds);
    QList<RawEvent> getRawEventsByIds(const QList<qlonglong> &ids);
    QList<qlonglong> getPendingInteractionIds(int limit);
    QList<RawEvent> getInteractionSourceEvents(qlonglong interactionId);
    bool saveInteractionSummary(qlonglong id, const QString &summary, const QString &status);
    bool saveInteractionSummaryWithClaims(qlonglong id,
                                          const QString &summary,
                                          const QString &summaryFormat,
                                          const QString &segmentsJson,
                                          const QList<ClaimDraft> &claims,
                                          const QString &status);
    bool markInteractionSummaryFailed(qlonglong id);
    QList<ClaimDraft> getClaimsForInteraction(qlonglong interactionId);
    QList<MemoryClaim> getActiveClaims(int limit);
    QList<InteractionSummary> getRecentInteractionSummaries(const QString &channel,
                                                            const QString &sessionId,
                                                            int limit);
    QList<InteractionSummary> getInteractionSummariesInRange(
        const QString &channel,
        const QString &sessionId,
        const QDateTime &from,
        const QDateTime &to,
        int limit);
    QList<InteractionSummary> getInteractionSummariesOverlapping(
        const QString &channel,
        const QString &sessionId,
        const QDateTime &from,
        const QDateTime &to,
        int limit);
    int countInteractionsInRange(const QString &channel,
                                 const QString &sessionId,
                                 const QDateTime &from,
                                 const QDateTime &to);
    QString getGroupDisplayName(const QString &groupId);
    QList<qlonglong> getFailedInteractionsWithUnsafeParticipantNames(int limit = 100);
    bool resetInteractionSummaryForRetry(qlonglong interactionId);

    // PR-2.1: 只读近期群聊上下文检索
    QStringList getRecentGroupIds(const QStringList &allowedGroupIds,
                                  int maxGroups,
                                  const QDateTime &since, const QDateTime &until = {});
    RecentContextBundle searchRecentGroupContext(const RecentContextQuery &query);
    QStringList getKnownDisplayNamesForUserIds(const QStringList &userIds, int limit);

    // PR-3.0: person / person_identity
    void updateMasterAccounts(const QStringList &masterUserIds);
    void seedIdentity(const QStringList &masterUserIds);
    QString resolvePersonId(const QString &platform,
                            const QString &platformUserId,
                            const QString &groupId = QString());
    bool ensurePersonIdentity(const QString &platform,
                              const QString &platformUserId,
                              const QString &groupId,
                              const QString &displayName,
                              const QDateTime &occurredAt,
                              const QString &source = QStringLiteral("message"));
    QStringList getPersonAliases(const QString &personId,
                                 const QString &groupId,
                                 int limit);

    // PR-3.2a: 群成员关系快照
    bool replaceGroupMembershipSnapshot(
        const QString &groupId,
        const QList<QPair<QString, QString>> &members,
        const QDateTime &verifiedAt);
    bool markGroupMembershipSnapshotFailed(const QString &groupId,
                                           const QString &error,
                                           const QDateTime &verifiedAt);
    bool hasValidGroupMembershipSnapshot(const QString &groupId,
                                         int maxAgeMinutes,
                                         const QDateTime &now = QDateTime());
    QStringList getOpenGroupMemberIds(const QString &groupId);
    QStringList getOpenGroupsForUser(const QString &userId, int limit);
    bool openGroupMembershipPeriod(const QString &groupId,
                                   const QString &userId,
                                   const QString &displayName,
                                   const QString &source,
                                   const QDateTime &at);
    bool closeGroupMembershipPeriod(const QString &groupId,
                                    const QString &userId,
                                    const QDateTime &at);

//蒸馏游标（memory_cursor）：每 channel+session 一条；Phase 1 蒸馏接线时使用
    bool updateMemoryCursor(const QString &channel, const QString &sessionId,
                            qlonglong lastEventId, qlonglong lastTurnId);
    qlonglong getMemoryCursorEventId(const QString &channel, const QString &sessionId);
    QList<HistoryTurn> getUnsummarizedTurns(qlonglong &outLastEndId,QList<qlonglong> &outSourceIds,
                                            const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession, qint64 throughTurn = 0);

//角色档案crud (v4)
    bool upsertCharacterProfile(const QString &subject, const QString &key, const QString &value);
    QList<CharacterProfile> getCharacterProfiles(const QString &subject);
    bool deleteCharacterProfile(qlonglong id);

//情景记忆crud (v4)
    bool addEpisodicMemory(const QString &content, const QString &type, double importance,
                           const QDateTime &eventTime = QDateTime(), const QString &sourceIds = QString(),
                           const QString &origin = QStringLiteral("pet"),
                           const QString &visibility = QStringLiteral("private"),
                           const QString &subject = QStringLiteral("user"),
                           const QString &sessionId = QStringLiteral("default"));
    QList<EpisodicMemory> getActiveEpisodicMemories(double minImportance = 0.2, int limit = 20,
                                                   const QString &origin = QString(),
                                                   const QString &visibility = QString(),
                                                   const QString &sessionId = QString(), bool touch = true);
    bool deleteEpisodicMemory(qlonglong id);
    bool updateEpisodicMemoryStatus(qlonglong id, const QString &status);
    int decayEpisodicMemory();

    QString normalizeProfileKey(const QString &subject, const QString &rawKey);
    QString mergeProfileValue(const QString &key, const QString &oldVal, const QString &newVal);
    int levenshteinDistance(const QString &s1, const QString &s2);
    int getEditDistanceThreshold(const QString &value);

//记忆摘要crud
    bool addLongTermSummary(const QString &summaryText, qlonglong coveredEndId, const QString &sourceIdsJson,
                            const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession);
    QList<LongTermSummary> getLatestSummaries(int limit = 5,
                                              const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession);
    bool deleteLongTermSummary(qlonglong id);

//关系状态crud
    bool upsertRelationshipState(const QString &dimension, double delta, const QString &subject = QStringLiteral("user"));
    QList<RelationshipState> getRelationshipStates(const QString &subject = QStringLiteral("user"));
    bool initRelationshipState();

    bool recordCognitiveAudit(const QString &kind, const QString &run, qint64 source, const QJsonObject &data);
signals:
    void understandingChanged();
    void searchChunksChanged();

private:
    // ActionPolicy 是主动联系策略的确定性校验器：它只读本类的连接做 ref 存在性/
    // authority/时序校验，不写库。friend 避免把整张库的查询接口暴露到公共 API。
    friend class ActionPolicy;
    // R12-0 诊断：reason 输出 daily_limit/cooldown/db_error；nextEligibleAt 仅在可计算时填写。
    // R12-1a：sourceKind/sourceRef/runId 写入 thought_review_log；sourceDailyLimit>0 时
    // 额外限制该来源今日次数（OpenWake 独立预算，仍受共享 dailyLimit 与冷却约束）。
    bool chargePrivateReview(qlonglong turn, int dailyLimit, int cooldownSeconds,
                             QString *reason = nullptr, QDateTime *nextEligibleAt = nullptr,
                             const QString &sourceKind = QString(),
                             const QString &sourceRef = QString(),
                             const QString &runId = QString(),
                             int sourceDailyLimit = 0);

    void initDatabase(const QString &dbFilePath, const QString &connectionName = QString());
    bool upsertMemoryChunk(const MemoryChunkWrite &chunk);
    bool indexRawEvent(qlonglong id);
    bool indexChatTurn(qlonglong id);
    bool ensurePerson(const QString &personId,
                      const QString &role,
                      const QString &canonicalName);

    // R12-1a：非 OpenWake 任务成功领取后、在同一事务内递增公平计数。
    // 按"实际成功领取的任务"计数（不是 tick）；没有调度行/未到期/暂停中时为空操作。
    bool bumpOpenWakeFairness();
    // R12-1a：暂停到期后的恢复尝试：若该源存在 failure 行且 suspended_until<=now，
    // 把 consecutive_failures 置 0（保留 last_reason）。返回 SQL 是否执行成功。
    bool recoverSuspendedFailure(const QString &sourceKind, const QString &sourceRef);

    // R12-2c-3：事务内写一条主动历史（chat_history + raw_events），不提交。
    // 同 channel/session/kind/ref 幂等（已存在返回既有 id，created=false）；任何一步失败返回 0，
    // 调用方必须回滚。返回新历史轮次 id，供提交后建检索索引。
    qlonglong writeProactiveHistoryLocked(const QString &channel, const QString &session,
                                          const QString &text, const QString &kind,
                                          const QString &ref, bool *created);

    QSqlDatabase m_db;
    QString m_databasePath;
    QSet<QString> m_masterUserIds;
};

#endif // MEMORYMANAGER_H
