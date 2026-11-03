#ifndef MEMORYRECALLTYPES_H
#define MEMORYRECALLTYPES_H

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "memorytimeframe.h"

// 一次独立回忆任务的时间目标。message_time 可直接过滤；
// event_time / fact_validity 需要额外的数据字段，当前不能普遍硬过滤。
enum class MemoryTimeTarget {
    MessageTime,
    EventTime,
    FactValidity
};

QString memoryTimeTargetToString(MemoryTimeTarget target);
MemoryTimeTarget memoryTimeTargetFromString(const QString &value);

struct PlannerTurn {
    QString role;       // user / assistant
    QDateTime timestamp;
    QString text;
};

// Planner 输出的一个独立回忆目标。timeExpression 是原始表达，
// 不允许 Planner 直接给 from/to。
struct MemoryRecallNeed {
    QString id;
    QString query;
    QStringList kinds;
    QString sourceHint;
    QString timeExpression;
    MemoryTimeTarget timeTarget = MemoryTimeTarget::MessageTime;
    bool hasTime = false;
    MemoryTimeFrame timeFrame;
    bool hasTimeFrame = false;
};

struct MemoryRecallPlan {
    QString mode = QStringLiteral("lookup"); // lookup / overview
    QJsonObject overview;                    // bucket_minutes / max_buckets (server clamps)
    QList<MemoryRecallNeed> needs;

    static bool fromJson(const QJsonObject &object,
                         MemoryRecallPlan *plan,
                         QString *error);
    // json_envelope 与 native tool 共用同一套参数语义：
    // 保留 mode / overview / needs，避免回退协议丢掉 TimeFrame 或 Overview。
    static bool fromEnvelope(const QJsonObject &content,
                             MemoryRecallPlan *plan,
                             QString *error);
};

struct MemoryNeedResult {
    QString needId;
    QString query;
    QString status;      // success / empty / unresolved / unsupported / denied / error
    QString expression;
    MemoryTimeTarget timeTarget = MemoryTimeTarget::MessageTime;
    QString resolvedFrom;
    QString resolvedTo;
    QString interpretation;
    QString evidenceState;   // found / empty / unresolved / denied / partial
    QString timeCertainty;   // exact / approximate / unknown
    QStringList sourceKinds;
    bool framePresent = false;
    QString anchorType;
    int frameOffset = 0;
    QString spanType;
    QString resolverType;    // time_frame / legacy / none
    QString resolverStatus;  // ok / unresolved / frame_required / unsupported
    QString accessScopeStatus;
    bool timeConstraintApplied = false;
    int expressionLength = 0;
    QString expressionHash;
    int scannedCount = 0;
    int totalCandidates = 0;
    int resultCount = 0;
    int latencyMs = 0;
    QString error;
};

#endif // MEMORYRECALLTYPES_H
