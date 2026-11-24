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

    QJsonObject claimPrivateReflection(int dailyLimit, int cooldownSeconds);
    bool finishPrivateReflection(qlonglong turn, int version, const QString &choice, const QString &note,
                                 const QJsonObject &contact = {}, const QJsonArray &consulted = {},
                                 const QJsonObject &prior = {}, const QString &priorAction = {},
                                 const QJsonObject &concern = {});
    QJsonObject reflectionHistory(qint64 turn,int version,const QJsonArray &rankedTurns,
                                  const QStringList &terms = {}) const;
    bool consultPrivateReflection(qint64 turn, int version,const QStringList &terms,QJsonArray &results) const;
    QString claimReflectionContact(qlonglong turn, int version, const QString &channel, const QString &session);
    bool recordContactOutcome(qlonglong turn, int version, const QString &outcome);
    // 上次投递被拒、且还没用完一次重试机会的联系。投递层（5s 调度）据此再次领取；
    // 领取本身由 claimReflectionContact 原子完成，重复轮询不会重复发送。
    QJsonArray retryableReflectionContacts(int limit = 4) const;
    QJsonArray pendingReminders(const RequestMeta &viewer) const;
    bool applyReminders(qint64 turn,const RequestMeta &viewer,const QJsonObject &plan);
    QJsonObject claimDueReminder(const QString &channel,const QString &session);
    void reminderOutcome(qint64 id,const QString &state);
    bool deferPrivateReflection(qlonglong turn, int version);
    QJsonObject recentPrivateReflection(const RequestMeta &viewer, const QJsonArray &rankedTurns = {}, bool requireRanked = false) const;
    // 关注解析器：与当前输入相关的开放关注。前台注入与后台回顾共用同一份选择逻辑，
    // 不再要求"旧关注的来源轮次恰好被检索命中"。
    QJsonArray matchedConcerns(const RequestMeta &viewer, const QStringList &terms,
                               const QString &queryText = QString(),
                               qint64 excludeTurn = 0, int limit = 2,
                               const QString &beforeReported = QString()) const;
    QJsonObject concernByHead(qint64 headTurn) const;
    // 到期回顾：不需要新用户消息，也不要求检索命中。领取时占用与回顾相同的预算/冷却。
    QJsonObject claimDueConcern(int dailyLimit, int cooldownSeconds);
    QJsonObject concernReviewContext(qint64 turn, int version) const;
    bool finishConcernReview(qlonglong turn, int version, const QJsonObject &proposal);
    bool deferConcernReview(qlonglong turn, int version, const QString &reason);
    // 从正常对话了结一个关注：答案到达、被否定或计划取消。提交后不再注入、不再重复追问。
    bool resolveConcern(qlonglong headTurn, qlonglong byTurn, const QString &kind,
                        const QString &evidenceRef);
    // 认识更新队列的可观测性：用于区分"模型判断错"与"新资料还没处理到"。
    QJsonObject observationQueueStatus() const;
    QJsonObject nextThought(bool dueOnly) const;
    bool claimThoughtReview(qlonglong id, int version, int dailyLimit, int cooldownSeconds);
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
    // 读取一条活动念头（thought_result 快照），用于能力清单里的"当前活动"。
    QJsonObject activityById(qlonglong id) const;
    // 写一条"茉子主动说出的话"到历史与 raw_events，供下次对话回认与检索。
    // 主动轮次 user_input 为 NULL、turn_kind='proactive'，不参与回顾/认识更新。
    qlonglong recordProactiveMessage(const QString &channel, const QString &session, const QString &text,
                                     const QString &kind, const QString &ref);

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
    bool chargePrivateReview(qlonglong turn, int dailyLimit, int cooldownSeconds);
    void initDatabase(const QString &dbFilePath, const QString &connectionName = QString());
    bool upsertMemoryChunk(const MemoryChunkWrite &chunk);
    bool indexRawEvent(qlonglong id);
    bool indexChatTurn(qlonglong id);
    bool ensurePerson(const QString &personId,
                      const QString &role,
                      const QString &canonicalName);
    QSqlDatabase m_db;
    QString m_databasePath;
    QSet<QString> m_masterUserIds;
};

#endif // MEMORYMANAGER_H
