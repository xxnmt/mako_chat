#ifndef MEMORYMANAGER_H
#define MEMORYMANAGER_H

#include <QObject>
#include <QObject>
#include <QSqlDatabase>
#include <QVariantMap>
#include <QList>
#include "sentencedata.h"
#include "historyturn.h"
#include "utterance.h"


class MemoryManager : public QObject
{
    Q_OBJECT
public:
    explicit MemoryManager(const QString &dbFilePath,QObject *parent = nullptr);
    ~MemoryManager();
    bool saveQATurn(const QString &userInput,
                          const QString &rawReply,
                          const QList<SentenceText> &sentences,
                          const RequestMeta &meta = RequestMeta());

//历史对话crud
    QList<HistoryTurn> getHistoryTurn(int N, const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession);
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

//蒸馏游标（memory_cursor）：每 channel+session 一条；Phase 1 蒸馏接线时使用
    bool updateMemoryCursor(const QString &channel, const QString &sessionId,
                            qlonglong lastEventId, qlonglong lastTurnId);
    qlonglong getMemoryCursorEventId(const QString &channel, const QString &sessionId);
    QList<HistoryTurn> getUnsummarizedTurns(qlonglong &outLastEndId,QList<qlonglong> &outSourceIds,
                                            const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession);

//角色档案crud (v4)
    bool upsertCharacterProfile(const QString &subject, const QString &key, const QString &value);
    QList<CharacterProfile> getCharacterProfiles(const QString &subject);
    bool deleteCharacterProfile(qlonglong id);

//情景记忆crud (v4)
    bool addEpisodicMemory(const QString &content, const QString &type, double importance,
                           const QDateTime &eventTime = QDateTime(), const QString &sourceIds = QString(),
                           const QString &origin = QStringLiteral("pet"),
                           const QString &visibility = QStringLiteral("private"),
                           const QString &subject = QStringLiteral("user"));
    QList<EpisodicMemory> getActiveEpisodicMemories(double minImportance = 0.2, int limit = 20,
                                                   const QString &origin = QString(),
                                                   const QString &visibility = QString());
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

private:
    void initDatabase(const QString &dbFilePath);
    QSqlDatabase m_db;
};

#endif // MEMORYMANAGER_H
