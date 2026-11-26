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
    QJsonObject claimOutbox(const QString &owner,int leaseSeconds = 60);
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
    bool recordReviewFailure(const QString &sourceKind,const QString &sourceRef,
                             const QString &stage,const QString &reason,
                             bool countsAsFailure,QString *error=nullptr);
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
    // share_intent/review_after/channel/session/updated_at 等（按来源可用字段）。
    QJsonObject activityByRef(const QString &ref) const;
    // 两存储 active/paused 统一判断（scope 隔离：legacy 只属于 owner）。
    bool hasActiveActivity(const QString &scopeKey=QStringLiteral("owner")) const;
    // 两存储中 status=active、review_after<=now、sensitive=0 的最早候选；
    // 返回 {ref,kind,version,activity}（kind=legacy|internal），无则 {}。
    QJsonObject nextDueActivity(const QString &scopeKey=QStringLiteral("owner")) const;
    // 统一领取：legacy 走 claimThoughtReview，internal 走 version CAS + 共享预算/冷却；
    // 成功返回 {ref,kind,id,version,activity,origin_kind,run_id,scope_key}。
    // reason 输出与现有领取一致：daily_limit/cooldown/cas_conflict/db_error/not_due。
    QJsonObject claimActivity(const QString &ref,int expectedVersion,int dailyLimit,
        int cooldownSeconds,QString *reason=nullptr);
    // 统一提交：legacy 走 finishThoughtReview，internal 单事务 CAS 写 status/result_json/
    // artifact_ref/review_after/version；返回 {status,version(新),ref[,artifact_ref]}。
    QJsonObject finishActivity(const QString &ref,int version,const QJsonObject &proposal,
        QString *error=nullptr);
    // 统一延后：legacy 复用 deferThoughtReview；internal CAS 只推 review_after，不改 lifecycle。
    bool deferActivity(const QString &ref,int version,int retrySeconds,const QString &reason);
    // 统一动作：pause/resume/abandon/complete/reschedule。legacy 复用 applyThoughtAction
    // （complete 不支持，返回 unsupported_action）；internal 在单事务内做状态机 + CAS，
    // resume 会做跨存储 active/paused 检查（排除自身）。返回 {ref,status,version,origin}。
    QJsonObject applyActivityAction(const QString &ref,int expectedVersion,const QString &action,
        int afterMinutes,const QString &reason,QString *error=nullptr);
    // 分享意愿只落 internal_activity.result_json.share_intent，不写 delivery_ledger/
    // action_outbox/chat_history、不发消息；legacy 返回 unsupported_legacy。
    bool setActivityShareIntent(const QString &ref,int version,const QString &draft,
        QString *error=nullptr);
    // 启动恢复诊断（只读、幂等）：报告两存储 active/paused 的跨存储冲突与 internal 到期
    // 列表；不删除活动、不伪造完成。
    QJsonArray recoverActivities(const QString &scopeKey=QStringLiteral("owner"));

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

    QSqlDatabase m_db;
    QString m_databasePath;
    QSet<QString> m_masterUserIds;
};

#endif // MEMORYMANAGER_H
