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
    QList<HistoryTurn> getUnsummarizedTurns(qlonglong &outLastEndId,QList<qlonglong> &outSourceIds,
                                            const QString &channel = Channel::kPet, const QString &sessionId = Channel::kDefaultSession);

//角色档案crud (v4)
    bool upsertCharacterProfile(const QString &subject, const QString &key, const QString &value);
    QList<CharacterProfile> getCharacterProfiles(const QString &subject);
    bool deleteCharacterProfile(qlonglong id);

//情景记忆crud (v4)
    bool addEpisodicMemory(const QString &content, const QString &type, double importance, const QDateTime &eventTime = QDateTime(), const QString &sourceIds = QString());
    QList<EpisodicMemory> getActiveEpisodicMemories(double minImportance = 0.2, int limit = 20);
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
