#include "memoryevidence.h"

#include <QJsonArray>
#include <QTimeZone>

namespace {

QString timeText(const QDateTime &value)
{
    return value.isValid() ? value.toString(Qt::ISODateWithMs) : QString();
}

QDateTime parseTime(const QJsonValue &value)
{
    if (!value.isString())
        return {};
    const auto text = value.toString();
    auto parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(text, Qt::ISODate);
    return parsed;
}

QString derivedFor(const QString &sourceType)
{
    if (sourceType == QStringLiteral("revision"))
        return QStringLiteral("inference");
    if (sourceType == QStringLiteral("summary")
        || sourceType == QStringLiteral("interaction"))
        return QStringLiteral("summary");
    if (sourceType == QStringLiteral("claim"))
        return QStringLiteral("claim");
    if (sourceType == QStringLiteral("episode"))
        return QStringLiteral("memory");
    return QStringLiteral("original");
}

} // namespace

MemorySituation MemorySituation::fromRequest(const RequestMeta &meta, bool voiceAvailable)
{
    MemorySituation situation;
    situation.runId = meta.runId;
    situation.eventId = meta.eventId;
    situation.currentTime = QDateTime::currentDateTime();
    situation.timezone = QString::fromUtf8(QDateTime::currentDateTime().timeZone().id());
    situation.inputOccurredAt = meta.occurredAtUtc.isValid() ? meta.occurredAtUtc
                                                            : meta.receivedAtUtc;
    situation.surface = meta.channel;
    situation.session = meta.sessionId;
    situation.speakerId = !meta.personId.isEmpty() ? meta.personId
        : !meta.actorId.isEmpty() ? meta.actorId
        : (meta.subject == QStringLiteral("user") ? QStringLiteral("owner") : meta.subject);
    situation.speakerName = meta.speaker;
    situation.speakerRole = meta.subject == QStringLiteral("user")
        ? QStringLiteral("owner") : QStringLiteral("participant");
    situation.audienceKind = meta.channel == Channel::kQQGroup
        ? QStringLiteral("group")
        : (meta.subject == QStringLiteral("user") ? QStringLiteral("owner")
                                                  : QStringLiteral("participant"));
    situation.audienceLabel = meta.channel == Channel::kQQGroup
        ? QStringLiteral("group") : situation.audienceKind;
    // subject 是记忆归属/当前对话对象，不是本轮事情涉及的人。
    situation.about = QStringLiteral("unknown");
    situation.modality = (meta.channel == Channel::kPet && voiceAvailable)
        ? QStringLiteral("voice+text") : QStringLiteral("text");
    situation.privacy = meta.channel == Channel::kQQGroup
        ? QStringLiteral("public") : QStringLiteral("private");
    situation.knowledgeProfile = meta.longTermMemoryEnabled
        ? QStringLiteral("long_term_enabled") : QStringLiteral("short_term_only");
    situation.expressionPreferences = meta.channel == Channel::kQQGroup
        ? QStringLiteral("public_audience") : QStringLiteral("private_audience");
    return situation;
}

QJsonObject MemorySituation::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("run"), runId);
    object.insert(QStringLiteral("event"), eventId);
    object.insert(QStringLiteral("current_time"), timeText(currentTime));
    object.insert(QStringLiteral("timezone"), timezone);
    object.insert(QStringLiteral("input_occurred_at"), timeText(inputOccurredAt));
    object.insert(QStringLiteral("surface"), surface);
    object.insert(QStringLiteral("session"), session);
    object.insert(QStringLiteral("speaker"), QJsonObject{
        {QStringLiteral("id"), speakerId},
        {QStringLiteral("display_name"), speakerName},
        {QStringLiteral("role"), speakerRole}});
    object.insert(QStringLiteral("audience"), QJsonObject{
        {QStringLiteral("kind"), audienceKind},
        {QStringLiteral("label"), audienceLabel}});
    object.insert(QStringLiteral("about"), about);
    object.insert(QStringLiteral("modality"), modality);
    object.insert(QStringLiteral("knowledge_profile"), knowledgeProfile);
    object.insert(QStringLiteral("expression_preferences"), expressionPreferences);
    return object;
}

QString MemorySituation::toPromptBlock() const
{
    const QString speakerLine = speakerName.isEmpty()
        ? speakerId : QStringLiteral("%1 (%2)").arg(speakerId, speakerName);
    return QStringLiteral(
        "# 当前场景（服务端事实）\n"
        "- surface: %1\n"
        "- speaker: %2\n"
        "- audience: %3\n"
        "- about: %4\n"
        "- modality: %5\n"
        "- privacy: %6\n"
        "- input_occurred_at: %7\n"
        "- knowledge_profile: %8\n"
        "- expression_preferences: %9\n"
        "- about=unknown 表示服务端没有确认本轮事情涉及谁；不要把 speaker 当成事情主体。\n"
        "- 场景只说明在哪里、对谁、能用什么形式；不要求不同渠道换一种说法。\n")
        .arg(surface, speakerLine, audienceLabel, about, modality, privacy,
             timeText(inputOccurredAt), knowledgeProfile, expressionPreferences);
}

MemoryEvidence MemoryEvidence::fromLegacy(const QJsonObject &row)
{
    MemoryEvidence evidence;
    evidence.sourceRef = row.value(QStringLiteral("reference")).toString(
        row.value(QStringLiteral("evidence_ref")).toString());
    evidence.handle = evidence.sourceRef;
    const int slash = evidence.sourceRef.indexOf(QLatin1Char('/'));
    evidence.sourceType = slash > 0 ? evidence.sourceRef.left(slash)
                                    : QStringLiteral("unknown");
    evidence.text = row.value(QStringLiteral("text")).toString();
    evidence.own = row.value(QStringLiteral("own")).toBool();
    evidence.channel = row.value(QStringLiteral("channel")).toString();
    evidence.session = row.value(QStringLiteral("session")).toString();
    evidence.speakerName = row.value(QStringLiteral("speaker")).toString();
    evidence.speakerId = evidence.own ? QStringLiteral("owner") : QString();
    evidence.speakerRole = evidence.own ? QStringLiteral("user") : QStringLiteral("other");
    if (evidence.speakerName.isEmpty())
        evidence.speakerName = evidence.own ? QStringLiteral("主人") : QStringLiteral("未知");
    evidence.derived = row.value(QStringLiteral("derived")).toString(derivedFor(evidence.sourceType));
    evidence.speechAt = parseTime(row.value(QStringLiteral("time")));
    evidence.eventTime = parseTime(row.value(QStringLiteral("event_time")));
    evidence.recordedAt = parseTime(row.value(QStringLiteral("recorded_at")));
    evidence.privacy = row.value(QStringLiteral("privacy")).toString(QStringLiteral("unknown"));
    evidence.status = row.value(QStringLiteral("status")).toString(QStringLiteral("complete"));
    evidence.expressionHint = row.value(QStringLiteral("expression_hint")).toString();
    for (const auto &id : row.value(QStringLiteral("about_ids")).toArray())
        evidence.aboutIds.append(id.toString());
    return evidence;
}

MemoryEvidence MemoryEvidence::fromChunk(const QJsonObject &row)
{
    MemoryEvidence evidence;
    evidence.sourceRef = row.value(QStringLiteral("source_ref")).toString();
    evidence.handle = QStringLiteral("chunk/%1").arg(row.value(QStringLiteral("id")).toInteger());
    evidence.sourceType = row.value(QStringLiteral("source_type")).toString();
    evidence.text = row.value(QStringLiteral("text")).toString();
    evidence.derived = row.value(QStringLiteral("derived")).toString(
        derivedFor(evidence.sourceType));
    evidence.speakerId = row.value(QStringLiteral("speaker_id")).toString();
    evidence.speakerName = row.value(QStringLiteral("speaker")).toString();
    evidence.speakerRole = row.value(QStringLiteral("speaker_role")).toString(
        QStringLiteral("unknown"));
    evidence.own = evidence.speakerRole == QStringLiteral("user");
    evidence.channel = row.value(QStringLiteral("channel")).toString();
    evidence.session = row.value(QStringLiteral("session")).toString();
    evidence.speechAt = parseTime(row.value(QStringLiteral("speech_at")));
    evidence.eventTime = parseTime(row.value(QStringLiteral("event_time")));
    evidence.recordedAt = parseTime(row.value(QStringLiteral("recorded_at")));
    evidence.privacy = row.value(QStringLiteral("privacy")).toString(QStringLiteral("unknown"));
    evidence.status = row.value(QStringLiteral("status")).toString(QStringLiteral("complete"));
    return evidence;
}

QJsonObject MemoryEvidence::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("handle"), handle);
    object.insert(QStringLiteral("evidence_ref"), handle);
    object.insert(QStringLiteral("source_ref"), sourceRef);
    object.insert(QStringLiteral("source_type"), sourceType);
    object.insert(QStringLiteral("text"), text);
    object.insert(QStringLiteral("speaker_id"), speakerId);
    object.insert(QStringLiteral("speaker"), speakerName);
    object.insert(QStringLiteral("speaker_role"), speakerRole);
    object.insert(QStringLiteral("own"), own);
    object.insert(QStringLiteral("derived"), derived);
    object.insert(QStringLiteral("time"), timeText(speechAt));
    object.insert(QStringLiteral("speech_at"), timeText(speechAt));
    object.insert(QStringLiteral("event_time"), timeText(eventTime));
    object.insert(QStringLiteral("recorded_at"), timeText(recordedAt));
    object.insert(QStringLiteral("channel"), channel);
    object.insert(QStringLiteral("session"), session);
    object.insert(QStringLiteral("privacy"), privacy);
    object.insert(QStringLiteral("status"), status);
    if (!expressionHint.isEmpty())
        object.insert(QStringLiteral("expression_hint"), expressionHint);
    if (!aboutIds.isEmpty())
        object.insert(QStringLiteral("about_ids"), QJsonArray::fromStringList(aboutIds));
    return object;
}

QJsonObject RetrievalResult::toJson() const
{
    QJsonArray items;
    for (const auto &item : evidence)
        items.append(item.toJson());
    return QJsonObject{{QStringLiteral("status"), status},
                       {QStringLiteral("backend"), backend},
                       {QStringLiteral("candidates"), candidateCount},
                       {QStringLiteral("lexical_candidates"), lexicalCount},
                       {QStringLiteral("vector_candidates"), vectorCount},
                       {QStringLiteral("latency_ms"), latencyMs},
                       {QStringLiteral("truncated"), truncated},
                       {QStringLiteral("evidence"), items}};
}
