#ifndef MEMORYEVIDENCE_H
#define MEMORYEVIDENCE_H

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include "utterance.h"

class MemoryManager;

// 服务端构造的当前情境。模型不能伪造身份、听众或知识配置。
struct MemorySituation
{
    QString runId;
    QString eventId;
    QDateTime currentTime;
    QString timezone;
    QDateTime inputOccurredAt;
    QString surface;
    QString session;
    QString speakerId;
    QString speakerName;
    QString speakerRole;
    QString audienceKind;
    QString audienceLabel;
    QString about = QStringLiteral("unknown");
    QString modality;
    QString privacy;
    QString knowledgeProfile;
    QString expressionPreferences;

    static MemorySituation fromRequest(const RequestMeta &meta, bool voiceAvailable);
    QJsonObject toJson() const;
    QString toPromptBlock() const;
    bool isEmpty() const { return surface.isEmpty(); }
};

// 一条可回查的检索证据。handle 只在本次 run 内使用，不暴露数据库主键。
struct MemoryEvidence
{
    QString handle;
    QString sourceType;       // raw / episode / interaction / summary / claim / source / revision / legacy
    QString sourceRef;        // 内部来源映射
    QString speakerId;
    QString speakerName;
    QString speakerRole;      // user / assistant / other / unknown
    QStringList aboutIds;
    QString text;
    QString derived;          // original / summary / inference / assistant_expression / execution_result
    QDateTime speechAt;       // 发言时间；未知为空
    QDateTime eventTime;      // 已知事件时间；未知为空
    QDateTime recordedAt;     // 派生记录生成时间；未知为空
    QString channel;
    QString session;
    QString privacy = QStringLiteral("unknown");
    QString status = QStringLiteral("complete");
    QString expressionHint;
    bool own = false;

    static MemoryEvidence fromLegacy(const QJsonObject &row);
    static MemoryEvidence fromChunk(const QJsonObject &row,
                                    const QString &viewerPersonId = {},
                                    bool viewerIsOwner = false);
    QJsonObject toJson() const;
};

struct RetrievalRequest
{
    QString query;
    QString purpose;
    QString subject = QStringLiteral("user");
    QStringList accounts;
    QStringList terms;
    bool includeOthers = true;
    QString viewerPersonId;
    bool viewerIsOwner = false;
    QString currentChannel;
    QString currentSession;
    bool excludeCurrent = true;
    bool includeUnmatched = false;
    bool includeAllEpisodic = false;
    bool requireTermMatch = false;
    bool allowLegacyFallback = true;
    bool includeAuxiliarySources = true;
    QStringList sourceKinds;
    QStringList allowedSources; // "channel|session" 或 "channel|*"
    int limit = 12;
    int candidateLimit = 40;
    int rerankLimit = 10;
    bool useIndex = true;
    MemorySituation situation;
};

struct RetrievalResult
{
    QList<MemoryEvidence> evidence;
    QString status = QStringLiteral("complete"); // complete / partial / degraded
    QString backend = QStringLiteral("unknown"); // legacy / index
    int candidateCount = 0;
    int lexicalCount = 0;
    int vectorCount = 0;
    int auxiliaryCount = 0;
    qint64 latencyMs = 0;
    bool truncated = false;
    QJsonObject toJson() const;
};

Q_DECLARE_METATYPE(RetrievalResult)

#endif // MEMORYEVIDENCE_H
