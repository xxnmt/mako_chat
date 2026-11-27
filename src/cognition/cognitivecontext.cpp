#include "cognitivecontext.h"

#include "configmanager.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>

namespace {

QString compactJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

int compactBytes(const QJsonObject &object)
{
    return compactJson(object).toUtf8().size();
}

int compactBytes(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)).toUtf8().size();
}

// 文本里的相对时间表达。服务端只有在拿到 event_time 时才承认“今天/明天”；
// 没有锚定事件时间时只能标记 unresolved，绝不继承旧消息里的相对时间。
bool hasRelativeTimeExpression(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("(今天|明天|昨天|前天|后天|今晚|明晚|昨晚|今早|明早|"
                       "这周|下周|上周|这个月|下个月|上个月|最近|刚才|待会|等会)"));
    return pattern.match(text).hasMatch();
}

QDateTime parseUtcFlexible(const QString &value)
{
    const QString text = value.trimmed();
    if (text.isEmpty()) return {};
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(text, Qt::ISODate);
    if (!parsed.isValid()) parsed = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!parsed.isValid()) return {};
    return parsed.toUTC();
}

QString firstNonEmpty(const QStringList &values)
{
    for (const QString &value : values) {
        if (!value.trimmed().isEmpty()) return value;
    }
    return {};
}

QString truncateText(const QString &text, int maxChars)
{
    if (maxChars <= 0 || text.size() <= maxChars) return text;
    return text.left(maxChars) + QStringLiteral("…");
}

QString kindPrefixOf(const QString &ref)
{
    return ref.section(QLatin1Char('/'), 0, 0);
}

bool parseInt(const QString &text, int *value)
{
    bool ok = false;
    const int parsed = text.toInt(&ok);
    if (!ok) return false;
    if (value) *value = parsed;
    return true;
}

// 把材料里的 policy 状态收敛成统一状态名。empty 与 denied/error 分开是本切片的关键。
QString normalizeState(const QString &state)
{
    if (state.isEmpty()) return QStringLiteral("complete");
    if (state == QStringLiteral("empty")) return QStringLiteral("empty");
    if (state == QStringLiteral("complete") || state == QStringLiteral("serialized")
        || state == QStringLiteral("ok") || state == QStringLiteral("found")) {
        return QStringLiteral("complete");
    }
    if (state == QStringLiteral("partial") || state == QStringLiteral("filtered")
        || state == QStringLiteral("query_cap")) {
        return QStringLiteral("partial");
    }
    if (state == QStringLiteral("degraded")) return QStringLiteral("degraded");
    if (state == QStringLiteral("denied") || state == QStringLiteral("failed")) return QStringLiteral("denied");
    if (state == QStringLiteral("error") || state == QStringLiteral("timeout")) return QStringLiteral("error");
    if (state == QStringLiteral("truncated") || state == QStringLiteral("budget_trimmed")
        || state == QStringLiteral("overflow")) {
        return QStringLiteral("budget_trimmed");
    }
    return QStringLiteral("partial");
}

// legacy evidence 目录里混有 run 内句柄（m_run.evidence 用工具项 id / chunk id 做键），
// 它们不是 stable_ref：只作诊断，不能伪装成“统一目录漏掉的稳定引用”。
bool isRunScopedHandle(const QString &key)
{
    if (key.startsWith(QStringLiteral("chunk/")) || key.startsWith(QStringLiteral("tool/")))
        return true;
    if (key == QStringLiteral("current_input")) return false;
    // 形如 recent:9 / claim:3 / summary:4 / episodic:5 / long_summary:6 / assistant:7。
    static const QRegularExpression handlePattern(
        QStringLiteral("^(recent|summary|claim|episodic|long_summary|assistant|chat):\\d+$"));
    return handlePattern.match(key).hasMatch();
}

// 只给报告用的身份冲突键：同一身份在不同入口被标成不同 authority/kind。
QString authorityName(EvidenceAuthority authority)
{
    return evidenceAuthorityName(authority);
}

} // namespace

QString evidenceKindName(EvidenceKind kind)
{
    switch (kind) {
    case EvidenceKind::UserStatement: return QStringLiteral("user_statement");
    case EvidenceKind::ExternalObservation: return QStringLiteral("external_observation");
    case EvidenceKind::AssistantExpression: return QStringLiteral("assistant_expression");
    case EvidenceKind::DerivedSummary: return QStringLiteral("derived_summary");
    case EvidenceKind::SelfInference: return QStringLiteral("self_inference");
    case EvidenceKind::InternalAutonomousRecord: return QStringLiteral("internal_autonomous_record");
    case EvidenceKind::UserDelegation: return QStringLiteral("user_delegation");
    case EvidenceKind::ExecutionResult: return QStringLiteral("execution_result");
    case EvidenceKind::ActivityArtifact: return QStringLiteral("activity_artifact");
    }
    return QStringLiteral("derived_summary");
}

EvidenceKind evidenceKindFromName(const QString &name)
{
    if (name == QStringLiteral("user_statement")) return EvidenceKind::UserStatement;
    if (name == QStringLiteral("external_observation")) return EvidenceKind::ExternalObservation;
    if (name == QStringLiteral("assistant_expression")) return EvidenceKind::AssistantExpression;
    if (name == QStringLiteral("derived_summary")) return EvidenceKind::DerivedSummary;
    if (name == QStringLiteral("self_inference")) return EvidenceKind::SelfInference;
    if (name == QStringLiteral("internal_autonomous_record")) return EvidenceKind::InternalAutonomousRecord;
    if (name == QStringLiteral("user_delegation")) return EvidenceKind::UserDelegation;
    if (name == QStringLiteral("execution_result")) return EvidenceKind::ExecutionResult;
    if (name == QStringLiteral("activity_artifact")) return EvidenceKind::ActivityArtifact;
    return EvidenceKind::DerivedSummary;
}

EvidenceKind evidenceKindFromAuthority(EvidenceAuthority authority)
{
    switch (authority) {
    case EvidenceAuthority::UserStatement: return EvidenceKind::UserStatement;
    case EvidenceAuthority::ExternalObservation: return EvidenceKind::ExternalObservation;
    case EvidenceAuthority::ExecutionResult: return EvidenceKind::ExecutionResult;
    case EvidenceAuthority::AssistantExpression: return EvidenceKind::AssistantExpression;
    case EvidenceAuthority::SelfInference: return EvidenceKind::SelfInference;
    case EvidenceAuthority::InternalState: return EvidenceKind::InternalAutonomousRecord;
    }
    return EvidenceKind::DerivedSummary;
}

QString EvidenceIdentity::eventKey() const
{
    QString key = sourceKind + QLatin1Char('|') + ref;
    if (version > 0) key += QLatin1Char('@') + QString::number(version);
    return key;
}

QString EvidenceIdentity::stableKey() const
{
    QString key = eventKey();
    if (!facet.isEmpty()) key += QLatin1Char('#') + facet;
    return key;
}

QString EvidenceIdentity::displayRef() const
{
    if (sourceKind == QStringLiteral("matter") && ref.startsWith(QStringLiteral("matter/"))) {
        return QStringLiteral("revision/%1/%2").arg(ref.mid(QStringLiteral("matter/").size()))
            .arg(version);
    }
    if (version > 0 && !ref.endsWith(QLatin1Char('/') + QString::number(version))) {
        return ref + QLatin1Char('@') + QString::number(version);
    }
    return ref;
}

EvidenceIdentity evidenceIdentityForRef(const QString &rawRef, const QString &sourceKindHint,
                                        int version, const QString &facet)
{
    EvidenceIdentity identity;
    const QString raw = rawRef.trimmed();
    if (raw.isEmpty()) return identity;
    identity.ref = raw;
    identity.sourceKind = kindPrefixOf(raw);
    identity.version = version;
    identity.facet = facet;

    if (raw == QStringLiteral("current_input")) {
        identity.sourceKind = QStringLiteral("current_input");
        identity.ref = QStringLiteral("current_input");
        identity.facet = QStringLiteral("user_statement");
        return identity;
    }

    const QString prefix = identity.sourceKind;
    const QString rest = raw.section(QLatin1Char('/'), 1);
    if (prefix == QStringLiteral("reply")) {
        // reply/<turn> 与 chat/<turn>:assistant 是同一行的 assistant 面。
        identity.sourceKind = QStringLiteral("chat");
        identity.ref = QStringLiteral("chat/") + rest.section(QLatin1Char(':'), 0, 0);
        if (identity.facet.isEmpty()) identity.facet = QStringLiteral("assistant_expression");
    } else if (prefix == QStringLiteral("chat")) {
        if (rest.contains(QLatin1Char(':'))) {
            const QString suffix = rest.section(QLatin1Char(':'), 1, 1);
            if (identity.facet.isEmpty())
                identity.facet = suffix == QStringLiteral("assistant")
                    ? QStringLiteral("assistant_expression") : QStringLiteral("user_statement");
            identity.ref = QStringLiteral("chat/") + rest.section(QLatin1Char(':'), 0, 0);
        } else if (!sourceKindHint.isEmpty() && facet.isEmpty()) {
            identity.facet = facet;
        }
    } else if (prefix == QStringLiteral("revision")) {
        // revision/<matter>/<version>：matter 名本身可能含空格/中文，按最后一个 / 切版本。
        const int lastSlash = raw.lastIndexOf(QLatin1Char('/'));
        int parsedVersion = 0;
        if (lastSlash > int(QStringLiteral("revision/").size()) - 1
            && parseInt(raw.mid(lastSlash + 1), &parsedVersion) && parsedVersion > 0) {
            const QString matter = raw.mid(int(QStringLiteral("revision/").size()),
                                           lastSlash - int(QStringLiteral("revision/").size()));
            if (!matter.isEmpty()) {
                identity = evidenceIdentityForMatter(matter, parsedVersion);
                if (!facet.isEmpty()) identity.facet = facet;
                return identity;
            }
        }
    } else if (prefix == QStringLiteral("matter")) {
        identity.sourceKind = QStringLiteral("matter");
        identity.ref = raw;
    } else if (prefix == QStringLiteral("episode")) {
        identity.sourceKind = QStringLiteral("episodic");
        identity.ref = QStringLiteral("episodic/") + rest;
    } else if (prefix == QStringLiteral("interaction")) {
        identity.sourceKind = QStringLiteral("interaction_summary");
        identity.ref = QStringLiteral("interaction_summary/") + rest;
    } else if (prefix == QStringLiteral("long_summary")) {
        identity.sourceKind = QStringLiteral("summary");
        identity.ref = QStringLiteral("summary/") + rest;
    } else if (!sourceKindHint.isEmpty()
               && prefix != QStringLiteral("episodic")
               && prefix != QStringLiteral("interaction_summary")
               && prefix != QStringLiteral("summary")
               && prefix != QStringLiteral("claim")
               && prefix != QStringLiteral("profile")
               && prefix != QStringLiteral("raw")
               && prefix != QStringLiteral("relationship")
               && prefix != QStringLiteral("source")
               && prefix != QStringLiteral("reflection")
               && prefix != QStringLiteral("concern")
               && prefix != QStringLiteral("thought")
               && prefix != QStringLiteral("activity")
               && prefix != QStringLiteral("wake")
               && prefix != QStringLiteral("reminder")
               && prefix != QStringLiteral("delivery")
               && prefix != QStringLiteral("receipt")) {
        identity.sourceKind = sourceKindHint;
    }
    if (identity.ref.isEmpty()) identity.ref = raw;
    return identity;
}

EvidenceIdentity evidenceIdentityForMatter(const QString &matter, int version)
{
    EvidenceIdentity identity;
    if (matter.trimmed().isEmpty()) return identity;
    identity.sourceKind = QStringLiteral("matter");
    identity.ref = QStringLiteral("matter/") + matter.trimmed();
    identity.version = version;
    return identity;
}

QJsonObject CognitiveEvidence::toJson() const
{
    QJsonObject timeJson;
    if (time.reportedAtUtc.isValid())
        timeJson.insert(QStringLiteral("reported_at"), time.reportedAtUtc.toString(Qt::ISODateWithMs));
    if (time.eventAtUtc.isValid())
        timeJson.insert(QStringLiteral("event_at"), time.eventAtUtc.toString(Qt::ISODateWithMs));
    if (time.effectiveAtUtc.isValid())
        timeJson.insert(QStringLiteral("effective_at"), time.effectiveAtUtc.toString(Qt::ISODateWithMs));
    if (time.recordedAtUtc.isValid())
        timeJson.insert(QStringLiteral("recorded_at"), time.recordedAtUtc.toString(Qt::ISODateWithMs));
    timeJson.insert(QStringLiteral("event_time_unknown"), time.eventTimeUnknown);
    timeJson.insert(QStringLiteral("relative_unanchored"), time.relativeExpressionUnanchored);

    QJsonObject identityJson{
        {QStringLiteral("source_kind"), identity.sourceKind},
        {QStringLiteral("ref"), identity.ref},
        {QStringLiteral("version"), identity.version},
        {QStringLiteral("display_ref"), identity.displayRef()},
        {QStringLiteral("event_key"), identity.eventKey()},
        {QStringLiteral("stable_key"), identity.stableKey()},
        {QStringLiteral("stable"), identity.stable}};
    if (!identity.facet.isEmpty()) identityJson.insert(QStringLiteral("facet"), identity.facet);

    QJsonObject json{
        {QStringLiteral("identity"), identityJson},
        {QStringLiteral("kind"), evidenceKindName(kind)},
        {QStringLiteral("authority"), authorityName(authority)},
        {QStringLiteral("purpose"), purpose},
        {QStringLiteral("text"), text},
        {QStringLiteral("text_chars"), textChars()},
        {QStringLiteral("privacy"), privacy},
        {QStringLiteral("sensitive"), sensitive},
        {QStringLiteral("time"), timeJson},
        {QStringLiteral("source_state"), sourceState},
        {QStringLiteral("usages"), QJsonArray::fromStringList(usages)}};
    if (!subjectId.isEmpty()) json.insert(QStringLiteral("subject_id"), subjectId);
    if (!channel.isEmpty()) json.insert(QStringLiteral("channel"), channel);
    if (!sessionId.isEmpty()) json.insert(QStringLiteral("session_id"), sessionId);
    if (!derivedFrom.isEmpty())
        json.insert(QStringLiteral("derived_from"), QJsonArray::fromStringList(derivedFrom));
    if (!modelHandles.isEmpty())
        json.insert(QStringLiteral("model_handles"), QJsonArray::fromStringList(modelHandles));
    return json;
}

QJsonObject CognitiveDomainView::toJson() const
{
    QJsonObject json{
        {QStringLiteral("name"), name},
        {QStringLiteral("status"), status},
        {QStringLiteral("queried"), queried},
        {QStringLiteral("eligible"), eligible},
        {QStringLiteral("serialized"), serialized},
        {QStringLiteral("omitted"), omitted},
        {QStringLiteral("queried_domain"), queriedDomain},
        {QStringLiteral("refs"), QJsonArray::fromStringList(refs)}};
    if (!notes.isEmpty())
        json.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return json;
}

QString CognitiveContextTime::relativeLabel(const QDateTime &atUtc, int *daysAgo) const
{
    if (!atUtc.isValid()) {
        if (daysAgo) *daysAgo = 0;
        return QStringLiteral("unknown");
    }
    const QDateTime local = atUtc.toLocalTime();
    qint64 days = 0;
    if (nowLocal.isValid() && local.isValid())
        days = nowLocal.date().toJulianDay() - local.date().toJulianDay();
    if (daysAgo) *daysAgo = int(days);
    QString label;
    if (days == 0) label = QStringLiteral("今天");
    else if (days == 1) label = QStringLiteral("昨天");
    else if (days == 2) label = QStringLiteral("前天");
    else if (days > 2) label = QStringLiteral("%1天前").arg(days);
    else if (days == -1) label = QStringLiteral("明天");
    else if (days == -2) label = QStringLiteral("后天");
    else label = QStringLiteral("%1天后").arg(-days);
    return QStringLiteral("%1 %2").arg(label, local.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
}

QJsonObject CognitiveContextTime::toJson() const
{
    return QJsonObject{{QStringLiteral("now_local"), localIso},
                       {QStringLiteral("now_utc"), utcIso},
                       {QStringLiteral("timezone"), timezone},
                       {QStringLiteral("relative_time_rule"),
                        QStringLiteral("由服务端按本次构建的 now 计算；不继承旧消息中的今天/明天")}};
}

QJsonObject CognitiveContextScene::toJson() const
{
    return QJsonObject{
        {QStringLiteral("entry"), entry},
        {QStringLiteral("subject_id"), subjectId},
        {QStringLiteral("actor_id"), actorId},
        {QStringLiteral("speaker"), speaker},
        {QStringLiteral("speaker_role"), speakerRole},
        {QStringLiteral("channel"), channel},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("audience_kind"), audienceKind},
        {QStringLiteral("audience_label"), audienceLabel},
        {QStringLiteral("privacy"), privacy},
        {QStringLiteral("blind_zone"), blindZone},
        {QStringLiteral("long_term_memory_enabled"), longTermMemoryEnabled},
        {QStringLiteral("owner_private"), ownerPrivate},
        {QStringLiteral("capability_scope"), capabilityScope}};
}

void CognitiveCoverageReport::setSource(const QString &source, const QString &state,
                                        const QString &note)
{
    sourceStates.insert(source, state);
    if (!note.isEmpty()) notes.append(source + QLatin1Char(':') + note);
}

void CognitiveCoverageReport::finalize()
{
    emptySources.clear();
    notQueriedSources.clear();
    degradedSources.clear();
    deniedSources.clear();
    errorSources.clear();
    trimmedSources.clear();
    for (auto it = sourceStates.constBegin(); it != sourceStates.constEnd(); ++it) {
        if (it.value() == QStringLiteral("empty")) emptySources.append(it.key());
        else if (it.value() == QStringLiteral("not_queried")) notQueriedSources.append(it.key());
        else if (it.value() == QStringLiteral("degraded")) degradedSources.append(it.key());
        else if (it.value() == QStringLiteral("denied")) deniedSources.append(it.key());
        else if (it.value() == QStringLiteral("error")) errorSources.append(it.key());
        else if (it.value() == QStringLiteral("budget_trimmed")) trimmedSources.append(it.key());
    }
    emptySources.sort();
    notQueriedSources.sort();
    degradedSources.sort();
    deniedSources.sort();
    errorSources.sort();
    trimmedSources.sort();
    if (!errorSources.isEmpty()) overall = QStringLiteral("error");
    else if (!degradedSources.isEmpty()) overall = QStringLiteral("degraded");
    else if (!deniedSources.isEmpty() || !trimmedSources.isEmpty()
             || !notQueriedSources.isEmpty()) overall = QStringLiteral("partial");
    else if (sourceStates.isEmpty()
             || emptySources.size() == sourceStates.size()) overall = QStringLiteral("empty");
    else overall = QStringLiteral("complete");
}

QJsonObject CognitiveCoverageReport::toJson() const
{
    QJsonObject states;
    for (auto it = sourceStates.constBegin(); it != sourceStates.constEnd(); ++it)
        states.insert(it.key(), it.value());
    QJsonObject json{{QStringLiteral("overall"), overall},
                     {QStringLiteral("sources"), states},
                     {QStringLiteral("empty_sources"), QJsonArray::fromStringList(emptySources)},
                     {QStringLiteral("not_queried_sources"), QJsonArray::fromStringList(notQueriedSources)},
                     {QStringLiteral("degraded_sources"), QJsonArray::fromStringList(degradedSources)},
                     {QStringLiteral("denied_sources"), QJsonArray::fromStringList(deniedSources)},
                     {QStringLiteral("error_sources"), QJsonArray::fromStringList(errorSources)},
                     {QStringLiteral("budget_trimmed_sources"), QJsonArray::fromStringList(trimmedSources)}};
    if (!notes.isEmpty())
        json.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return json;
}

QJsonObject CognitiveBudgetReport::toJson() const
{
    QJsonObject domains;
    for (auto it = domainBytes.constBegin(); it != domainBytes.constEnd(); ++it)
        domains.insert(it.key(), it.value());
    return QJsonObject{
        {QStringLiteral("context_bytes"), contextBytes},
        {QStringLiteral("context_chars"), contextChars},
        {QStringLiteral("evidence_bytes"), evidenceBytes},
        {QStringLiteral("unique_evidence_bytes"), uniqueEvidenceBytes},
        {QStringLiteral("total_material_bytes"), totalMaterialBytes},
        {QStringLiteral("duplicate_material_bytes"), duplicateMaterialBytes},
        {QStringLiteral("estimated_tokens"), estimatedTokens},
        {QStringLiteral("estimated_tokens_method"), QStringLiteral("ceil(chars/2)；CJK 粗略估算")},
        {QStringLiteral("legacy_prompt_bytes"), legacyPromptBytes},
        {QStringLiteral("legacy_data_bytes"), legacyDataBytes},
        {QStringLiteral("snapshot_budget_bytes"), snapshotBudgetBytes},
        {QStringLiteral("over_legacy_prompt"), overLegacyPrompt},
        {QStringLiteral("evidence_count"), evidenceCount},
        {QStringLiteral("unique_event_count"), uniqueEventCount},
        {QStringLiteral("domain_bytes"), domains}};
}

int CognitiveContext::uniqueEventCount() const
{
    QSet<QString> events;
    for (const auto &item : evidence) events.insert(item.identity.eventKey());
    return int(events.size());
}

const CognitiveEvidence *CognitiveContext::findByKey(const QString &stableKey) const
{
    for (const auto &item : evidence) {
        if (item.identity.stableKey() == stableKey) return &item;
    }
    return nullptr;
}

CognitiveDomainView &CognitiveContext::domain(const QString &name)
{
    auto it = domains.find(name);
    if (it == domains.end()) {
        CognitiveDomainView view;
        view.name = name;
        it = domains.insert(name, view);
    }
    return it.value();
}

QJsonObject CognitiveContext::projectionJson() const
{
    return QJsonObject{
        {QStringLiteral("entry"), scene.entry},
        {QStringLiteral("audience_kind"), scene.audienceKind},
        {QStringLiteral("owner_private"), scene.ownerPrivate},
        {QStringLiteral("visible_domains"), QJsonArray::fromStringList(visibleDomains)},
        {QStringLiteral("denied_domains"), QJsonArray::fromStringList(deniedDomains)},
        {QStringLiteral("rule"),
         QStringLiteral("同一证据目录，按入口投影：群聊/第三方不携带 owner 私有域；"
                        "统一上下文不扩大任何跨渠道读取权限")}};
}

QJsonObject CognitiveContext::toShadowJson() const
{
    QJsonArray evidenceJson;
    for (const auto &item : evidence) evidenceJson.append(item.toJson());
    QJsonObject domainJson;
    for (auto it = domains.constBegin(); it != domains.constEnd(); ++it)
        domainJson.insert(it.key(), it.value().toJson());
    QJsonObject json{
        {QStringLiteral("run_id"), runId},
        {QStringLiteral("trigger_ref"), triggerRef},
        {QStringLiteral("purpose"), purpose},
        {QStringLiteral("scene"), scene.toJson()},
        {QStringLiteral("time"), time.toJson()},
        {QStringLiteral("current_input"), currentInput},
        {QStringLiteral("input_event_id"), inputEventId},
        {QStringLiteral("capabilities"), capabilities},
        {QStringLiteral("evidence"), evidenceJson},
        {QStringLiteral("domains"), domainJson},
        {QStringLiteral("coverage"), coverage.toJson()},
        {QStringLiteral("projection"), projectionJson()},
        {QStringLiteral("budget"), budget.toJson()}};
    if (!inputEvents.isEmpty()) json.insert(QStringLiteral("input_events"), inputEvents);
    return json;
}

QJsonObject ShadowComparison::toJson() const
{
    QJsonObject json{
        {QStringLiteral("legacy_ref_count"), legacyRefCount},
        {QStringLiteral("legacy_stable_ref_count"), legacyStableRefCount},
        {QStringLiteral("unified_evidence_count"), unifiedEvidenceCount},
        {QStringLiteral("matched_refs"), matchedRefs},
        {QStringLiteral("missing_in_unified"), missingInUnified},
        {QStringLiteral("extra_in_unified"), extraInUnified},
        {QStringLiteral("duplicate_across_modules"), duplicateAcrossModules},
        {QStringLiteral("repeated_event_facets"), repeatedEventFacets},
        {QStringLiteral("cross_version_groups"), crossVersionGroups},
        {QStringLiteral("identity_conflicts"), identityConflicts},
        {QStringLiteral("privacy_filtered"), privacyFiltered},
        {QStringLiteral("privacy_leaks"), privacyLeaks},
        {QStringLiteral("relative_time_unanchored"), relativeTimeUnanchored},
        {QStringLiteral("raw_evidence_covered"), rawEvidenceCovered},
        {QStringLiteral("raw_evidence_total"), rawEvidenceTotal},
        {QStringLiteral("raw_evidence_coverage"), rawEvidenceCoverage},
        {QStringLiteral("notes"), QJsonArray::fromStringList(notes)}};
    return json;
}

CognitiveContextBuilder::CognitiveContextBuilder(MemoryManager *memory)
    : m_memory(memory)
{
}

bool CognitiveContextBuilder::isOwnerPrivateScene(const RequestMeta &meta, bool blindZone)
{
    if (blindZone) return false;
    const bool owner = meta.subject.isEmpty() || meta.subject == QStringLiteral("user");
    const bool privateChannel = meta.channel == Channel::kPet
        || meta.channel == Channel::kQQPrivate;
    return owner && privateChannel;
}

void CognitiveContextBuilder::addEvidence(CognitiveContext &context, CognitiveEvidence evidence,
                                          ShadowComparison *comparisonIn) const
{
    if (!evidence.identity.isValid()) return;
    ShadowComparison *comparison = comparisonIn ? comparisonIn : m_comparison;
    context.budget.totalMaterialBytes += evidence.text.toUtf8().size();
    if (evidence.time.eventTimeUnknown && hasRelativeTimeExpression(evidence.text))
        evidence.time.relativeExpressionUnanchored = true;
    if (evidence.time.relativeExpressionUnanchored && comparison)
        ++comparison->relativeTimeUnanchored;

    for (auto &existing : context.evidence) {
        if (existing.identity.stableKey() != evidence.identity.stableKey()) continue;
        // 同一身份重复出现：只保留一份完整文本，记录所有入口/句柄，不重复序列化。
        for (const QString &usage : evidence.usages)
            if (!existing.usages.contains(usage)) existing.usages.append(usage);
        for (const QString &handle : evidence.modelHandles)
            if (!existing.modelHandles.contains(handle)) existing.modelHandles.append(handle);
        for (const QString &source : evidence.derivedFrom)
            if (!existing.derivedFrom.contains(source)) existing.derivedFrom.append(source);
        if (existing.text.isEmpty()) existing.text = evidence.text;
        if (existing.purpose.isEmpty()) existing.purpose = evidence.purpose;
        if (comparison
            && (existing.authority != evidence.authority || existing.kind != evidence.kind)) {
            ++comparison->identityConflicts;
            const QString note = QStringLiteral("identity_conflict:%1 authority=%2/%3 kind=%4/%5")
                .arg(existing.identity.stableKey(), authorityName(existing.authority),
                     authorityName(evidence.authority), evidenceKindName(existing.kind),
                     evidenceKindName(evidence.kind));
            if (!comparison->notes.contains(note)) comparison->notes.append(note);
        }
        return;
    }
    context.budget.uniqueEvidenceBytes += evidence.text.toUtf8().size();
    context.evidence.append(evidence);
}

void CognitiveContextBuilder::ingestCurrentInput(CognitiveContext &context,
                                                 const CognitiveContextRequest &request) const
{
    if (request.currentInput.isEmpty() && request.inputEvents.isEmpty()) return;
    CognitiveEvidence evidence;
    evidence.identity = evidenceIdentityForRef(QStringLiteral("current_input"));
    evidence.identity.facet = QStringLiteral("user_statement");
    evidence.kind = EvidenceKind::UserStatement;
    evidence.authority = EvidenceAuthority::UserStatement;
    evidence.subjectId = request.meta.subject;
    evidence.channel = request.meta.channel;
    evidence.sessionId = request.meta.sessionId;
    evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                  : QStringLiteral("public");
    evidence.purpose = QStringLiteral("current_input");
    evidence.usages.append(QStringLiteral("foreground:current_input"));
    evidence.text = truncateText(request.currentInput, 1000);
    evidence.time.reportedAtUtc = request.meta.receivedAtUtc.isValid()
        ? request.meta.receivedAtUtc.toUTC() : context.time.nowUtc;
    evidence.time.eventAtUtc = request.meta.occurredAtUtc.isValid()
        ? request.meta.occurredAtUtc.toUTC() : evidence.time.reportedAtUtc;
    evidence.time.eventTimeUnknown = false;
    context.currentInput = request.currentInput;
    context.inputEventId = request.eventId;
    context.inputEvents = request.inputEvents;
    addEvidence(context, evidence, nullptr);
    auto &view = context.domain(QStringLiteral("current_input"));
    view.status = QStringLiteral("complete");
    view.queried = view.eligible = view.serialized = 1;
    view.queriedDomain = true;
    view.refs.append(QStringLiteral("current_input"));

    // 本轮视觉观察是“当前输入的派生态”：身份取自 vision/<event>/<segment>，
    // 只能当推断（视觉描述不能当原始事实），derivedFrom 指向 current_input。
    for (const auto &value : request.inputEvents) {
        for (const auto &observationValue :
             value.toObject().value(QStringLiteral("visual_observations")).toArray()) {
            const auto observation = observationValue.toObject();
            if (observation.value(QStringLiteral("status")).toString() != QStringLiteral("observed"))
                continue;
            const QString ref = observation.value(QStringLiteral("ref")).toString();
            if (ref.isEmpty()) continue;
            CognitiveEvidence vision;
            vision.identity = evidenceIdentityForRef(ref);
            vision.kind = EvidenceKind::SelfInference;
            vision.authority = EvidenceAuthority::SelfInference;
            vision.subjectId = request.meta.subject;
            vision.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                        : QStringLiteral("public");
            vision.purpose = QStringLiteral("current_visual_observation");
            vision.usages.append(QStringLiteral("foreground:visual"));
            vision.derivedFrom.append(QStringLiteral("current_input"));
            vision.text = truncateText(observation.value(QStringLiteral("description")).toString(), 300);
            vision.time.reportedAtUtc = evidence.time.reportedAtUtc;
            vision.time.eventTimeUnknown = false;
            addEvidence(context, vision, nullptr);
        }
    }
}

void CognitiveContextBuilder::ingestSnapshot(CognitiveContext &context,
                                             const CognitiveContextRequest &request) const
{
    if (!request.hasSnapshot) {
        context.coverage.setSource(QStringLiteral("snapshot"), QStringLiteral("not_queried"),
                                   QStringLiteral("本轮没有构建认知快照"));
        return;
    }
    const auto &snapshot = request.snapshot;
    auto addFact = [&](const CognitiveFactRef &fact, const QString &domainName,
                       const QString &facet, EvidenceKind kindHint,
                       const QString &privacyOverride) {
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(fact.ref, QString(), 0, facet);
        evidence.kind = kindHint;
        evidence.authority = fact.authority;
        evidence.subjectId = fact.subjectId;
        evidence.channel = fact.channel;
        evidence.sessionId = fact.sessionId;
        evidence.privacy = privacyOverride.isEmpty() ? fact.privacy : privacyOverride;
        evidence.sensitive = fact.sensitive;
        evidence.purpose = QStringLiteral("snapshot:%1").arg(domainName);
        evidence.usages.append(QStringLiteral("snapshot:%1").arg(domainName));
        evidence.text = truncateText(fact.text, 512);
        evidence.time.eventAtUtc = fact.occurredAtUtc.toUTC();
        evidence.time.reportedAtUtc = evidence.time.eventAtUtc;
        evidence.time.eventTimeUnknown = !fact.occurredAtUtc.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(domainName);
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        view.serialized += 1;
        view.status = QStringLiteral("complete");
    };

    for (const auto &fact : snapshot.observations)
        addFact(fact, QStringLiteral("observations"), QStringLiteral("user_statement"),
                evidenceKindFromAuthority(fact.authority), QString());
    for (const auto &fact : snapshot.selfExpressions)
        addFact(fact, QStringLiteral("self_expressions"), QStringLiteral("assistant_expression"),
                EvidenceKind::AssistantExpression, QString());
    for (const auto &fact : snapshot.inferences) {
        const bool internal = fact.authority == EvidenceAuthority::InternalState;
        addFact(fact, QStringLiteral("inferences"),
                internal ? QString() : QString(), // 同一行可以有多个语义面
                internal ? EvidenceKind::InternalAutonomousRecord : EvidenceKind::SelfInference,
                QStringLiteral("private"));
    }
    for (const auto &motivation : snapshot.motivations) {
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(motivation.ref, QString(),
                                                   motivation.version, QStringLiteral("motivation"));
        evidence.kind = EvidenceKind::InternalAutonomousRecord;
        evidence.authority = EvidenceAuthority::InternalState;
        evidence.purpose = QStringLiteral("snapshot:motivations");
        evidence.usages.append(QStringLiteral("snapshot:motivations"));
        evidence.privacy = QStringLiteral("private");
        evidence.text = truncateText(motivation.summary, 240);
        evidence.time.reportedAtUtc = snapshot.builtAtUtc.toUTC();
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("motivations"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    for (const auto &commitment : snapshot.commitments) {
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(commitment.ref, QString(),
                                                   0);
        evidence.kind = EvidenceKind::UserDelegation;
        evidence.authority = EvidenceAuthority::UserStatement;
        evidence.purpose = QStringLiteral("snapshot:commitments");
        evidence.usages.append(QStringLiteral("snapshot:commitments"));
        evidence.privacy = QStringLiteral("private");
        evidence.text = truncateText(commitment.text, 240);
        evidence.time.effectiveAtUtc = commitment.dueAtUtc.toUTC();
        evidence.time.reportedAtUtc = snapshot.builtAtUtc.toUTC();
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("commitments"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    for (const auto &value : snapshot.activities) {
        const auto row = value.toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QString(),
                                                   row.value(QStringLiteral("version")).toInt(),
                                                   QStringLiteral("activity"));
        evidence.kind = EvidenceKind::ActivityArtifact;
        evidence.authority = EvidenceAuthority::InternalState;
        evidence.purpose = QStringLiteral("snapshot:activities");
        evidence.usages.append(QStringLiteral("snapshot:activities"));
        evidence.privacy = QStringLiteral("private");
        evidence.text = truncateText(row.value(QStringLiteral("summary")).toString(
            row.value(QStringLiteral("goal")).toString()), 240);
        evidence.time.reportedAtUtc = snapshot.builtAtUtc.toUTC();
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("activities"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    for (const auto &value : snapshot.recentActivityResults) {
        const auto row = value.toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QString(),
                                                   row.value(QStringLiteral("version")).toInt(),
                                                   QStringLiteral("artifact"));
        evidence.kind = EvidenceKind::ActivityArtifact;
        evidence.authority = EvidenceAuthority::ExecutionResult;
        evidence.purpose = QStringLiteral("snapshot:recent_activity_results");
        evidence.usages.append(QStringLiteral("snapshot:recent_activity_results"));
        evidence.privacy = QStringLiteral("private");
        evidence.text = truncateText(row.value(QStringLiteral("summary")).toString(
            row.value(QStringLiteral("title")).toString()), 240);
        evidence.time.reportedAtUtc = parseUtcFlexible(
            row.value(QStringLiteral("completed_at")).toString());
        evidence.time.eventTimeUnknown = !evidence.time.reportedAtUtc.isValid();
        if (!evidence.time.reportedAtUtc.isValid()) evidence.time.reportedAtUtc = snapshot.builtAtUtc.toUTC();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("recent_activity_results"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    for (const auto &value : snapshot.internalWakes) {
        const auto row = value.toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QString(),
                                                   row.value(QStringLiteral("version")).toInt());
        evidence.kind = EvidenceKind::InternalAutonomousRecord;
        evidence.authority = EvidenceAuthority::InternalState;
        evidence.purpose = QStringLiteral("snapshot:internal_wakes");
        evidence.usages.append(QStringLiteral("snapshot:internal_wakes"));
        evidence.privacy = QStringLiteral("private");
        evidence.text = truncateText(row.value(QStringLiteral("summary")).toString(
            row.value(QStringLiteral("note")).toString()), 200);
        evidence.time.reportedAtUtc = snapshot.builtAtUtc.toUTC();
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("internal_wakes"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }

    // 用快照 builder 自己的每域统计覆盖默认值（queried/eligible/omitted 口径一致）。
    for (auto it = request.snapshotStats.domains.constBegin();
         it != request.snapshotStats.domains.constEnd(); ++it) {
        auto &view = context.domain(it.key());
        view.queried = it.value().queried;
        view.eligible = it.value().eligible;
        view.serialized = it.value().serialized > 0 ? it.value().serialized
                                                    : view.serialized;
        view.omitted = it.value().omitted;
        view.queriedDomain = it.value().queriedDomain;
        const QString mapped = normalizeState(it.value().status);
        // filtered = 读取策略过滤；在统一上下文里记录为 denied（隐私投影），不伪装成 empty。
        if (it.value().status == QStringLiteral("filtered")) {
            view.status = QStringLiteral("denied");
            view.notes.append(QStringLiteral("policy_filtered"));
        } else {
            view.status = mapped;
        }
    }
    context.coverage.setSource(QStringLiteral("snapshot"), QStringLiteral("complete"),
                               QStringLiteral("purpose=%1 build=1").arg(snapshot.purpose));
}

void CognitiveContextBuilder::ingestMemoryContext(CognitiveContext &context,
                                                  const CognitiveContextRequest &request) const
{
    if (!request.hasMemoryContext) {
        context.coverage.setSource(QStringLiteral("memory_data"), QStringLiteral("not_queried"),
                                   QStringLiteral("本轮不注入 MEMORY_DATA（工具模式或长期记忆关闭）"));
        return;
    }
    const auto &material = request.memoryContext;
    int count = 0;
    auto addMaterial = [&](const QString &kindName, qlonglong id, const QString &suffix,
                           const QString &text, const QString &channel, const QString &session,
                           const QDateTime &eventAt, const QDateTime &reportedAt,
                           const QString &privacy, EvidenceKind kind,
                           EvidenceAuthority authority, const QStringList &derivedFrom) {
        QString ref;
        if (!kindName.isEmpty())
            ref = kindName + QLatin1Char('/') + QString::number(id);
        if (ref.isEmpty() || id <= 0) return;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, kindName);
        if (!suffix.isEmpty()) evidence.identity.facet = suffix;
        evidence.kind = kind;
        evidence.authority = authority;
        evidence.channel = channel;
        evidence.sessionId = session;
        evidence.privacy = privacy;
        evidence.purpose = QStringLiteral("memory_data");
        evidence.usages.append(QStringLiteral("memory_data:%1").arg(kindName));
        evidence.text = truncateText(text, 400);
        evidence.time.eventAtUtc = eventAt;
        evidence.time.reportedAtUtc = reportedAt.isValid() ? reportedAt : eventAt;
        evidence.time.eventTimeUnknown = !eventAt.isValid();
        evidence.derivedFrom = derivedFrom;
        addEvidence(context, evidence, nullptr);
        ++count;
        auto &view = context.domain(QStringLiteral("memory_data"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    };

    for (const CharacterProfile &profile : material.userProfiles)
        addMaterial(QStringLiteral("profile"), profile.id, QString(),
                    profile.key + QStringLiteral(": ") + profile.value, QString(), QString(),
                    profile.updatedAt, profile.updatedAt, QStringLiteral("private"),
                    EvidenceKind::DerivedSummary, EvidenceAuthority::UserStatement, {});
    for (const EpisodicMemory &memory : material.memories)
        addMaterial(QStringLiteral("episodic"), memory.id, QString(), memory.content,
                    memory.origin, memory.sessionId, memory.eventTime, memory.eventTime,
                    memory.visibility == QStringLiteral("public")
                        ? QStringLiteral("public") : QStringLiteral("private"),
                    EvidenceKind::DerivedSummary, EvidenceAuthority::SelfInference, {});
    for (const InteractionSummary &summary : material.interactionSummaries)
        addMaterial(QStringLiteral("interaction_summary"), summary.id, QString(), summary.summaryText,
                    summary.channel, summary.sessionId, summary.endedAt, summary.endedAt,
                    QStringLiteral("private"), EvidenceKind::DerivedSummary,
                    EvidenceAuthority::ExternalObservation, {});
    for (const MemoryClaim &claim : material.claims) {
        // 复用现有 MemoryAccessPolicy 再校验一次：材料若绕过入口过滤会在这里被计入
        // denied，而不是被静默塞进统一上下文（统一上下文不扩权）。
        const MemoryAccessDecision decision = MemoryAccessPolicy::evaluate(claim, request.access);
        if (decision.state != MemoryAccessState::Shareable) {
            ++count;
            continue;
        }
        addMaterial(QStringLiteral("claim"), claim.id, QString(), claim.statement,
                    claim.sourceChannel, claim.sourceSessionId, claim.validTimeStart,
                    claim.validTimeStart, QStringLiteral("private"),
                    EvidenceKind::DerivedSummary, EvidenceAuthority::ExternalObservation, {});
    }
    for (const LongTermSummary &summary : material.summaries)
        addMaterial(QStringLiteral("summary"), summary.id, QString(), summary.summaryText,
                    QString(), QString(), summary.createdAt, summary.updatedAt,
                    QStringLiteral("private"), EvidenceKind::DerivedSummary,
                    EvidenceAuthority::SelfInference, {});
    for (const RelationshipState &relationship : material.relationships) {
        const QString ref = QStringLiteral("relationship/%1/%2")
            .arg(relationship.subject, relationship.dimension);
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QStringLiteral("relationship"));
        evidence.kind = EvidenceKind::InternalAutonomousRecord;
        evidence.authority = EvidenceAuthority::InternalState;
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("memory_data");
        evidence.usages.append(QStringLiteral("memory_data:relationship"));
        evidence.text = QStringLiteral("%1: %2").arg(relationship.dimension)
            .arg(relationship.value);
        evidence.time.reportedAtUtc = relationship.updatedAt;
        evidence.time.eventTimeUnknown = !relationship.updatedAt.isValid();
        addEvidence(context, evidence, nullptr);
        ++count;
    }
    auto &view = context.domain(QStringLiteral("memory_data"));
    view.queriedDomain = true;
    view.status = count > 0 ? QStringLiteral("complete") : QStringLiteral("empty");
    if (!request.access.viewerPersonId.isEmpty())
        view.notes.append(QStringLiteral("access_revalidated"));
    context.coverage.setSource(QStringLiteral("memory_data"),
                               count > 0 ? QStringLiteral("complete") : QStringLiteral("empty"));
}

void CognitiveContextBuilder::ingestUnderstanding(CognitiveContext &context,
                                                  const CognitiveContextRequest &request) const
{
    if (!request.canReadUnderstanding) {
        context.coverage.setSource(QStringLiteral("current_understanding"),
                                   QStringLiteral("denied"),
                                   QStringLiteral("当前入口没有 CURRENT_UNDERSTANDING 读取权限"));
        return;
    }
    int rows = 0;
    for (qsizetype i = 0; i < request.understanding.size(); ++i) {
        const auto row = request.understanding.at(i).toObject();
        const QString matter = request.understandingMatters.value(i).trimmed();
        const int revision = row.value(QStringLiteral("revision")).toInt();
        const QString reportedAt = row.value(QStringLiteral("reported_at")).toString();
        const QString quote = row.value(QStringLiteral("quote")).toString();
        const QString understanding = row.value(QStringLiteral("understanding")).toString();
        const QString topic = row.value(QStringLiteral("topic")).toString();
        const QJsonObject selfReport = row.value(QStringLiteral("self_report")).toObject();
        QStringList derived;
        const QString evidenceRef = row.value(QStringLiteral("evidence_ref")).toString();
        if (!evidenceRef.isEmpty()) derived.append(evidenceRef);
        const QString selfRef = selfReport.value(QStringLiteral("evidence_ref")).toString();
        if (!selfRef.isEmpty() && !derived.contains(selfRef)) derived.append(selfRef);

        if (!matter.isEmpty() || revision > 0) {
            CognitiveEvidence evidence;
            evidence.identity = evidenceIdentityForMatter(matter, revision);
            // 事项认识是“她自己的理解”，与快照 inferences 的 SelfInference 同语义；
            // 原始来源（source/<id>）另有 UserStatement 身份，二者绝不按内容合并。
            evidence.kind = EvidenceKind::SelfInference;
            evidence.authority = EvidenceAuthority::SelfInference;
            evidence.subjectId = request.meta.subject;
            evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                          : QStringLiteral("public");
            evidence.purpose = QStringLiteral("current_understanding");
            evidence.usages.append(QStringLiteral("understanding:matter"));
            evidence.text = truncateText(firstNonEmpty({understanding, quote, topic}), 400);
            evidence.time.reportedAtUtc = parseUtcFlexible(reportedAt);
            evidence.time.eventTimeUnknown = true;
            evidence.derivedFrom = derived;
            if (row.contains(QStringLiteral("event_time_expression"))) {
                evidence.time.relativeExpressionUnanchored =
                    hasRelativeTimeExpression(row.value(QStringLiteral("event_time_expression")).toString())
                    && !evidence.time.eventAtUtc.isValid();
            }
            addEvidence(context, evidence, nullptr);
            auto &view = context.domain(QStringLiteral("understanding"));
            view.queriedDomain = true;
            view.refs.append(evidence.identity.displayRef());
            ++view.serialized;
            ++rows;
        }
        // 原始来源（cognitive_source）与本人自述是不同身份：不能与事项认识合并。
        if (!evidenceRef.isEmpty()) {
            CognitiveEvidence evidence;
            evidence.identity = evidenceIdentityForRef(evidenceRef);
            evidence.kind = EvidenceKind::UserStatement;
            evidence.authority = EvidenceAuthority::UserStatement;
            evidence.subjectId = request.meta.subject;
            evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                          : QStringLiteral("public");
            evidence.purpose = QStringLiteral("understanding_evidence");
            evidence.usages.append(QStringLiteral("understanding:source"));
            evidence.text = truncateText(quote, 400);
            evidence.time.reportedAtUtc = parseUtcFlexible(reportedAt);
            evidence.time.eventTimeUnknown = true;
            addEvidence(context, evidence, nullptr);
            ++rows;
        }
        if (!selfRef.isEmpty()) {
            CognitiveEvidence evidence;
            evidence.identity = evidenceIdentityForRef(selfRef);
            evidence.kind = EvidenceKind::UserStatement;
            evidence.authority = EvidenceAuthority::UserStatement;
            evidence.subjectId = request.meta.subject;
            evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                          : QStringLiteral("public");
            evidence.purpose = QStringLiteral("understanding_self_report");
            evidence.usages.append(QStringLiteral("understanding:self_report"));
            evidence.text = truncateText(selfReport.value(QStringLiteral("quote")).toString(), 400);
            evidence.time.reportedAtUtc =
                parseUtcFlexible(selfReport.value(QStringLiteral("reported_at")).toString());
            evidence.time.eventTimeUnknown = true;
            addEvidence(context, evidence, nullptr);
            ++rows;
        }
    }
    auto &view = context.domain(QStringLiteral("understanding"));
    view.queriedDomain = true;
    view.status = rows > 0 ? QStringLiteral("complete") : QStringLiteral("empty");
    view.notes.append(QStringLiteral("empty 无法区分“没有事项”与“查询失败”（现状）"));
    context.coverage.setSource(QStringLiteral("current_understanding"),
                               rows > 0 ? QStringLiteral("complete") : QStringLiteral("empty"),
                               rows > 0 ? QString() : QStringLiteral("当前输入未命中事项；不代表历史为空"));
}

void CognitiveContextBuilder::ingestConcerns(CognitiveContext &context,
                                             const CognitiveContextRequest &request) const
{
    if (request.concerns.isEmpty()) {
        context.coverage.setSource(QStringLiteral("concern"), QStringLiteral("empty"));
        return;
    }
    for (const auto &value : request.concerns) {
        const auto row = value.toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        const QString rootRef = row.value(QStringLiteral("root_ref")).toString();
        const int version = row.value(QStringLiteral("version")).toInt();
        const QString stableRef = rootRef.isEmpty() ? ref : rootRef;
        if (stableRef.isEmpty()) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(stableRef, QString(), version);
        evidence.kind = EvidenceKind::SelfInference;
        evidence.authority = EvidenceAuthority::SelfInference;
        evidence.subjectId = request.meta.subject;
        // 关注是她自己的私人记录：无论当前入口是群聊还是私聊都是 private，
        // 群聊/第三方入口由统一投影过滤，不能因“入口是群聊”就把它标成 public。
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("concern");
        evidence.usages.append(QStringLiteral("concern:record"));
        if (!ref.isEmpty() && ref != stableRef) evidence.modelHandles.append(ref);
        evidence.text = truncateText(firstNonEmpty({row.value(QStringLiteral("topic")).toString(),
                                                    row.value(QStringLiteral("own_reflection")).toString(),
                                                    row.value(QStringLiteral("watch")).toString()}),
                                     320);
        evidence.time.reportedAtUtc =
            parseUtcFlexible(row.value(QStringLiteral("reported_at")).toString());
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        // 被回顾链修订过的前一版关注/想法是历史版本，保留为独立身份。
        if (!ref.isEmpty() && ref != stableRef) {
            CognitiveEvidence note;
            note.identity = evidenceIdentityForRef(ref, QString(), version, QStringLiteral("note"));
            note.kind = EvidenceKind::SelfInference;
            note.authority = EvidenceAuthority::SelfInference;
            note.privacy = QStringLiteral("private");
            note.purpose = QStringLiteral("concern_note");
            note.usages.append(QStringLiteral("concern:note"));
            note.text = truncateText(row.value(QStringLiteral("own_reflection")).toString(), 240);
            note.time.reportedAtUtc = evidence.time.reportedAtUtc;
            note.time.eventTimeUnknown = true;
            addEvidence(context, note, nullptr);
        }
        auto &view = context.domain(QStringLiteral("concern"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    context.coverage.setSource(QStringLiteral("concern"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestActivities(CognitiveContext &context,
                                               const CognitiveContextRequest &request) const
{
    if (request.activities.isEmpty()) {
        context.coverage.setSource(QStringLiteral("activity"), QStringLiteral("empty"));
        return;
    }
    for (const auto &value : request.activities) {
        const auto row = value.toObject();
        const QString ref = row.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QString(),
                                                   row.value(QStringLiteral("version")).toInt());
        evidence.kind = EvidenceKind::ActivityArtifact;
        evidence.authority = EvidenceAuthority::InternalState;
        evidence.subjectId = QStringLiteral("mako");
        evidence.channel = row.value(QStringLiteral("channel")).toString();
        evidence.sessionId = row.value(QStringLiteral("session")).toString();
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("activity");
        evidence.usages.append(QStringLiteral("activity:state"));
        evidence.text = truncateText(row.value(QStringLiteral("goal")).toString(
            row.value(QStringLiteral("interest")).toString()), 320);
        evidence.time.reportedAtUtc =
            parseUtcFlexible(row.value(QStringLiteral("updated_at")).toString());
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("activity"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    context.coverage.setSource(QStringLiteral("activity"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestReminders(CognitiveContext &context,
                                              const CognitiveContextRequest &request) const
{
    if (request.reminders.isEmpty()) {
        context.coverage.setSource(QStringLiteral("reminder"), QStringLiteral("empty"));
        return;
    }
    for (const auto &value : request.reminders) {
        const auto row = value.toObject();
        const qlonglong id = row.value(QStringLiteral("id")).toInteger();
        if (id <= 0) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(QStringLiteral("reminder/%1").arg(id));
        evidence.kind = EvidenceKind::UserDelegation;
        evidence.authority = EvidenceAuthority::UserStatement;
        evidence.subjectId = request.meta.subject;
        evidence.channel = request.meta.channel;
        evidence.sessionId = request.meta.sessionId;
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("reminder");
        evidence.usages.append(QStringLiteral("reminder:pending"));
        evidence.text = truncateText(row.value(QStringLiteral("message")).toString(), 320);
        evidence.time.effectiveAtUtc =
            parseUtcFlexible(row.value(QStringLiteral("due_at")).toString(
                row.value(QStringLiteral("due")).toString()));
        evidence.time.reportedAtUtc = context.time.nowUtc;
        evidence.time.eventTimeUnknown = !evidence.time.effectiveAtUtc.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("reminder"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
        view.status = QStringLiteral("complete");
    }
    context.coverage.setSource(QStringLiteral("reminder"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestHistory(CognitiveContext &context,
                                            const CognitiveContextRequest &request) const
{
    if (request.history.isEmpty()) {
        context.coverage.setSource(QStringLiteral("history"), QStringLiteral("empty"),
                                   QStringLiteral("短期历史为空（是否持久化由调用方决定）"));
        return;
    }
    for (const HistoryTurn &turn : request.history) {
        const QDateTime at = turn.timestamp.isValid() ? turn.timestamp.toUTC()
                                                      : context.time.nowUtc;
        if (!turn.userInput.isEmpty() && turn.id > 0) {
            CognitiveEvidence evidence;
            evidence.identity = evidenceIdentityForRef(QStringLiteral("chat/%1").arg(turn.id),
                                                       QString(), 0, QStringLiteral("user_statement"));
            evidence.kind = EvidenceKind::UserStatement;
            evidence.authority = EvidenceAuthority::UserStatement;
            evidence.subjectId = turn.subject;
            evidence.channel = request.meta.channel;
            evidence.sessionId = request.meta.sessionId;
            evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                          : QStringLiteral("public");
            evidence.purpose = QStringLiteral("history");
            evidence.usages.append(QStringLiteral("history:user"));
            evidence.modelHandles.append(QStringLiteral("chat/%1").arg(turn.id));
            evidence.text = truncateText(turn.userInput, 512);
            evidence.time.eventAtUtc = at;
            evidence.time.reportedAtUtc = at;
            evidence.time.eventTimeUnknown = false;
            addEvidence(context, evidence, nullptr);
        }
        if (!turn.rawReply.trimmed().isEmpty() && turn.id > 0) {
            CognitiveEvidence evidence;
            evidence.identity = evidenceIdentityForRef(QStringLiteral("reply/%1").arg(turn.id));
            evidence.kind = EvidenceKind::AssistantExpression;
            evidence.authority = EvidenceAuthority::AssistantExpression;
            evidence.subjectId = QStringLiteral("mako");
            evidence.channel = request.meta.channel;
            evidence.sessionId = request.meta.sessionId;
            evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                          : QStringLiteral("public");
            evidence.purpose = QStringLiteral("history");
            evidence.usages.append(QStringLiteral("history:assistant"));
            evidence.modelHandles.append(QStringLiteral("reply/%1").arg(turn.id));
            evidence.text = truncateText(turn.rawReply, 512);
            evidence.time.eventAtUtc = at;
            evidence.time.reportedAtUtc = at;
            evidence.time.eventTimeUnknown = false;
            addEvidence(context, evidence, nullptr);
        }
        auto &view = context.domain(QStringLiteral("history"));
        view.queriedDomain = true;
        view.status = QStringLiteral("complete");
    }
    context.coverage.setSource(QStringLiteral("history"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestRetrieval(CognitiveContext &context,
                                              const CognitiveContextRequest &request) const
{
    if (!request.retrievalDone) {
        context.coverage.setSource(QStringLiteral("retrieval"), QStringLiteral("not_queried"),
                                   QStringLiteral("本轮未执行自动检索（工具模式/关闭/盲区）"));
        return;
    }
    const QString state = normalizeState(request.retrievalStatus);
    for (const MemoryEvidence &entry : request.retrieved) {
        if (entry.text.trimmed().isEmpty()) continue;
        EvidenceIdentity identity;
        if (!entry.sourceRef.isEmpty()) {
            identity = evidenceIdentityForRef(entry.sourceRef, entry.sourceType);
        } else if (!entry.handle.isEmpty()) {
            identity.sourceKind = entry.sourceType.isEmpty()
                ? QStringLiteral("retrieval") : entry.sourceType;
            identity.ref = entry.handle;
            identity.stable = false;
        } else {
            continue;
        }
        CognitiveEvidence evidence;
        evidence.identity = identity;
        // 检索层 derived/speaker_role 决定语义与权威：助手表达只证明“说过”，
        // 摘要/推断不是原始事实，执行回执才是 execution_result。
        if (entry.derived == QStringLiteral("assistant_expression")) {
            evidence.kind = EvidenceKind::AssistantExpression;
            evidence.authority = EvidenceAuthority::AssistantExpression;
        } else if (entry.derived == QStringLiteral("summary")) {
            evidence.kind = EvidenceKind::DerivedSummary;
            evidence.authority = EvidenceAuthority::SelfInference;
        } else if (entry.derived == QStringLiteral("inference")) {
            evidence.kind = EvidenceKind::SelfInference;
            evidence.authority = EvidenceAuthority::SelfInference;
        } else if (entry.derived == QStringLiteral("execution_result")) {
            evidence.kind = EvidenceKind::ExecutionResult;
            evidence.authority = EvidenceAuthority::ExecutionResult;
        } else if (entry.speakerRole == QStringLiteral("assistant")) {
            evidence.kind = EvidenceKind::AssistantExpression;
            evidence.authority = EvidenceAuthority::AssistantExpression;
        } else if (entry.speakerRole == QStringLiteral("user")
                   || entry.derived == QStringLiteral("self_report")) {
            evidence.kind = EvidenceKind::UserStatement;
            evidence.authority = EvidenceAuthority::UserStatement;
        } else {
            evidence.kind = EvidenceKind::ExternalObservation;
            evidence.authority = EvidenceAuthority::ExternalObservation;
        }
        if (!entry.channel.isEmpty()) evidence.channel = entry.channel;
        if (!entry.session.isEmpty()) evidence.sessionId = entry.session;
        evidence.privacy = entry.privacy.isEmpty() ? QStringLiteral("unknown") : entry.privacy;
        evidence.purpose = QStringLiteral("retrieval");
        evidence.usages.append(QStringLiteral("retrieval:%1").arg(entry.sourceType));
        if (!entry.handle.isEmpty()) evidence.modelHandles.append(entry.handle);
        if (!entry.sourceRef.isEmpty() && entry.sourceRef != identity.ref
            && entry.sourceRef != identity.displayRef()) {
            evidence.modelHandles.append(entry.sourceRef);
        }
        evidence.text = truncateText(entry.text, 400);
        evidence.time.eventAtUtc = entry.eventTime.toUTC();
        evidence.time.reportedAtUtc = entry.speechAt.isValid() ? entry.speechAt.toUTC()
            : (entry.recordedAt.isValid() ? entry.recordedAt.toUTC() : QDateTime{});
        evidence.time.recordedAtUtc = entry.recordedAt.toUTC();
        evidence.time.eventTimeUnknown = !entry.eventTime.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("retrieval"));
        view.queriedDomain = true;
        view.refs.append(identity.displayRef());
        ++view.serialized;
    }
    auto &view = context.domain(QStringLiteral("retrieval"));
    view.queriedDomain = true;
    if (request.retrievalTruncated) {
        view.status = QStringLiteral("budget_trimmed");
        view.notes.append(QStringLiteral("truncated_by_limit"));
    } else {
        view.status = request.retrieved.isEmpty() ? QStringLiteral("empty") : state;
    }
    if (!request.retrievalStatus.isEmpty())
        view.notes.append(QStringLiteral("execution_status=%1").arg(request.retrievalStatus));
    auto coverageState = state;
    if (state == QStringLiteral("complete") && request.retrieved.isEmpty())
        coverageState = QStringLiteral("empty");
    if (state == QStringLiteral("partial") && request.retrieved.isEmpty())
        coverageState = QStringLiteral("degraded");
    context.coverage.setSource(QStringLiteral("retrieval"), coverageState,
                               request.retrievalStatus.isEmpty()
                                   ? QString()
                                   : QStringLiteral("status=%1").arg(request.retrievalStatus));
    if (state == QStringLiteral("degraded") || state == QStringLiteral("error")) {
        context.coverage.notes.append(
            QStringLiteral("检索失败/降级不得解释为“没有历史”：结果未知，不是空历史"));
    }
}

void CognitiveContextBuilder::ingestRecentContext(CognitiveContext &context,
                                                  const CognitiveContextRequest &request) const
{
    if (request.recent.items.isEmpty()) {
        const bool explicitlyMissing = request.missingSources.contains(QStringLiteral("recent"));
        context.coverage.setSource(QStringLiteral("recent_group_context"),
                                   explicitlyMissing ? QStringLiteral("not_queried")
                                                     : QStringLiteral("empty"));
        return;
    }
    for (const RecentContextItem &item : request.recent.items) {
        CognitiveEvidence evidence;
        if (item.rawEventId > 0) {
            evidence.identity = evidenceIdentityForRef(QStringLiteral("raw/%1").arg(item.rawEventId));
            evidence.modelHandles.append(QStringLiteral("raw/%1").arg(item.rawEventId));
        } else {
            evidence.identity.sourceKind = QStringLiteral("recent_group");
            evidence.identity.ref = QStringLiteral("recent_group/%1").arg(item.sourceGroupId);
            evidence.identity.stable = false;
        }
        evidence.kind = item.isViewer ? EvidenceKind::UserStatement
                                      : EvidenceKind::ExternalObservation;
        evidence.authority = item.isViewer ? EvidenceAuthority::UserStatement
                                           : EvidenceAuthority::ExternalObservation;
        evidence.subjectId = item.personId;
        evidence.channel = request.meta.channel;
        evidence.sessionId = item.sourceGroupId;
        evidence.privacy = QStringLiteral("public");
        evidence.purpose = QStringLiteral("recent_group_context");
        evidence.usages.append(QStringLiteral("recent:group_message"));
        evidence.text = truncateText(item.sanitizedText, 300);
        evidence.time.eventAtUtc = item.occurredAt.toUTC();
        evidence.time.reportedAtUtc = evidence.time.eventAtUtc;
        evidence.time.eventTimeUnknown = !item.occurredAt.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("recent_group_context"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    }
    auto &view = context.domain(QStringLiteral("recent_group_context"));
    view.status = request.recent.truncated ? QStringLiteral("budget_trimmed")
                                           : QStringLiteral("complete");
    context.coverage.setSource(QStringLiteral("recent_group_context"),
                               request.recent.truncated ? QStringLiteral("budget_trimmed")
                                                        : QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestPrivateEvidence(CognitiveContext &context,
                                                    const CognitiveContextRequest &request) const
{
    if (request.privateEvidence.isEmpty()) {
        context.coverage.setSource(QStringLiteral("private_evidence"),
                                   request.retrievalDone ? QStringLiteral("empty")
                                                         : QStringLiteral("not_queried"));
        return;
    }
    for (const auto &value : request.privateEvidence) {
        const auto item = value.toObject();
        const QString ref = item.value(QStringLiteral("source_ref")).toString(
            item.value(QStringLiteral("evidence_ref")).toString());
        if (ref.isEmpty()) continue;
        const int version = item.value(QStringLiteral("version")).toInt();
        const bool userFacet = item.value(QStringLiteral("own")).toBool()
            || item.value(QStringLiteral("speaker_role")).toString() == QStringLiteral("user");
        const bool assistantFacet =
            item.value(QStringLiteral("derived")).toString() == QStringLiteral("assistant_expression")
            || item.value(QStringLiteral("speaker_role")).toString() == QStringLiteral("assistant");
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(
            ref, QString(), version,
            userFacet ? QStringLiteral("user_statement")
                      : (assistantFacet ? QStringLiteral("assistant_expression") : QString()));
        if (userFacet) {
            evidence.kind = EvidenceKind::UserStatement;
            evidence.authority = EvidenceAuthority::UserStatement;
        } else if (assistantFacet) {
            evidence.kind = EvidenceKind::AssistantExpression;
            evidence.authority = EvidenceAuthority::AssistantExpression;
        } else {
            evidence.kind = EvidenceKind::ExternalObservation;
            evidence.authority = EvidenceAuthority::ExternalObservation;
        }
        evidence.subjectId = request.meta.subject;
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("private_evidence");
        evidence.usages.append(QStringLiteral("private_evidence:cross_channel"));
        evidence.text = truncateText(item.value(QStringLiteral("text")).toString(
            item.value(QStringLiteral("quote")).toString()), 400);
        evidence.time.reportedAtUtc =
            parseUtcFlexible(item.value(QStringLiteral("reported_at")).toString());
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("private_evidence"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    }
    auto &view = context.domain(QStringLiteral("private_evidence"));
    view.status = QStringLiteral("complete");
    context.coverage.setSource(QStringLiteral("private_evidence"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestToolItems(CognitiveContext &context,
                                              const CognitiveContextRequest &request) const
{
    if (request.toolStatus.isEmpty() && request.toolItems.isEmpty()) {
        context.coverage.setSource(QStringLiteral("memory_tool"), QStringLiteral("not_queried"),
                                   QStringLiteral("本轮未调用 memory_recall/memory_read"));
        return;
    }
    for (const MemoryAgentItem &item : request.toolItems) {
        EvidenceIdentity identity;
        if (item.historyTurnId > 0) {
            identity = evidenceIdentityForRef(QStringLiteral("chat/%1").arg(item.historyTurnId),
                                              QString(), 0,
                                              item.derived == QStringLiteral("assistant_expression")
                                                  ? QStringLiteral("assistant_expression")
                                                  : QStringLiteral("user_statement"));
        } else if (item.rawEventId > 0) {
            identity = evidenceIdentityForRef(QStringLiteral("raw/%1").arg(item.rawEventId));
        } else if (item.claimId > 0) {
            identity = evidenceIdentityForRef(QStringLiteral("claim/%1").arg(item.claimId));
        } else if (item.episodicId > 0) {
            identity = evidenceIdentityForRef(QStringLiteral("episodic/%1").arg(item.episodicId));
        } else if (item.longTermSummaryId > 0) {
            identity = evidenceIdentityForRef(QStringLiteral("summary/%1").arg(item.longTermSummaryId));
        } else if (item.interactionId > 0) {
            identity = evidenceIdentityForRef(
                QStringLiteral("interaction_summary/%1").arg(item.interactionId));
        } else if (!item.id.isEmpty()) {
            identity.sourceKind = QStringLiteral("tool");
            identity.ref = QStringLiteral("tool/%1").arg(item.id);
            identity.stable = false;
        } else {
            continue;
        }
        CognitiveEvidence evidence;
        evidence.identity = identity;
        evidence.kind = item.speakerRole == QStringLiteral("assistant")
            ? EvidenceKind::AssistantExpression
            : (item.derived == QStringLiteral("summary") ? EvidenceKind::DerivedSummary
               : (item.derived == QStringLiteral("inference") ? EvidenceKind::SelfInference
                  : (item.derived == QStringLiteral("execution_result")
                         ? EvidenceKind::ExecutionResult : EvidenceKind::UserStatement)));
        evidence.authority = evidence.kind == EvidenceKind::AssistantExpression
            ? EvidenceAuthority::AssistantExpression
            : (evidence.kind == EvidenceKind::ExecutionResult
                   ? EvidenceAuthority::ExecutionResult
                   : (evidence.kind == EvidenceKind::UserStatement
                          ? EvidenceAuthority::UserStatement : EvidenceAuthority::SelfInference));
        evidence.subjectId = item.personId;
        evidence.privacy = item.isViewer ? QStringLiteral("private") : QStringLiteral("public");
        evidence.purpose = QStringLiteral("memory_tool");
        evidence.usages.append(QStringLiteral("tool:%1")
                                   .arg(request.toolStatus.isEmpty()
                                            ? QStringLiteral("memory_recall") : request.toolStatus));
        if (!item.id.isEmpty()) evidence.modelHandles.append(item.id);
        evidence.text = truncateText(item.text, 400);
        evidence.time.eventAtUtc = item.time.toUTC();
        evidence.time.reportedAtUtc = evidence.time.eventAtUtc;
        evidence.time.eventTimeUnknown = !item.time.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("memory_tool"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    }
    const QString state = normalizeState(request.toolStatus);
    auto &view = context.domain(QStringLiteral("memory_tool"));
    view.queriedDomain = true;
    view.status = request.toolItems.isEmpty() && state == QStringLiteral("complete")
        ? QStringLiteral("empty") : state;
    context.coverage.setSource(QStringLiteral("memory_tool"),
                               request.toolItems.isEmpty() && state == QStringLiteral("complete")
                                   ? QStringLiteral("empty") : state);
}

void CognitiveContextBuilder::ingestConsulted(CognitiveContext &context,
                                              const CognitiveContextRequest &request) const
{
    if (request.consultedRows.isEmpty()) {
        context.coverage.setSource(QStringLiteral("matter_consult"), QStringLiteral("not_queried"),
                                   QStringLiteral("本轮没有 consult 结果（不是“没有历史”）"));
        return;
    }
    for (const auto &value : request.consultedRows) {
        const auto row = value.toObject();
        const qlonglong turn = row.value(QStringLiteral("turn")).toInteger();
        if (turn <= 0) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(QStringLiteral("chat/%1").arg(turn),
                                                   QString(), 0, QStringLiteral("user_statement"));
        evidence.kind = EvidenceKind::UserStatement;
        evidence.authority = EvidenceAuthority::UserStatement;
        evidence.subjectId = request.meta.subject;
        evidence.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                      : QStringLiteral("public");
        evidence.purpose = QStringLiteral("matter_consult");
        evidence.usages.append(QStringLiteral("consult:history"));
        evidence.modelHandles.append(QStringLiteral("chat/%1").arg(turn));
        evidence.text = truncateText(row.value(QStringLiteral("text")).toString(), 512);
        evidence.time.reportedAtUtc =
            parseUtcFlexible(row.value(QStringLiteral("reported_at")).toString());
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("matter_consult"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    }
    auto &view = context.domain(QStringLiteral("matter_consult"));
    view.status = QStringLiteral("complete");
    context.coverage.setSource(QStringLiteral("matter_consult"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::ingestReviewThread(CognitiveContext &context,
                                                 const CognitiveContextRequest &request) const
{
    if (request.reviewThread.isEmpty()) {
        context.coverage.setSource(QStringLiteral("review_thread"), QStringLiteral("not_queried"));
        return;
    }
    const auto thread = request.reviewThread;
    const int sourceTurn = int(request.reviewSourceTurn > 0
                                   ? request.reviewSourceTurn
                                   : thread.value(QStringLiteral("id")).toInteger());
    const bool ownerPrivate = context.scene.ownerPrivate;
    auto addRow = [&](const QString &ref, const QString &facet, const QString &purpose,
                      const QString &usage, const QString &text, const QDateTime &reportedAt,
                      EvidenceKind kind, EvidenceAuthority authority) {
        if (ref.isEmpty()) return;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QString(), 0, facet);
        evidence.kind = kind;
        evidence.authority = authority;
        evidence.subjectId = request.meta.subject;
        evidence.privacy = ownerPrivate ? QStringLiteral("private") : QStringLiteral("public");
        evidence.purpose = purpose;
        evidence.usages.append(usage);
        evidence.text = truncateText(text, 512);
        evidence.time.reportedAtUtc = reportedAt;
        evidence.time.eventTimeUnknown = !reportedAt.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("review_thread"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    };

    // 来源发言（reflection/concern 的 turn）本身是原始主人陈述。
    const QString ownerStatement = thread.value(QStringLiteral("owner_statement")).toString();
    if (sourceTurn > 0) {
        addRow(QStringLiteral("chat/%1").arg(sourceTurn), QStringLiteral("user_statement"),
               QStringLiteral("review_source"), QStringLiteral("review:source"),
               ownerStatement,
               parseUtcFlexible(thread.value(QStringLiteral("reported_at")).toString()),
               EvidenceKind::UserStatement, EvidenceAuthority::UserStatement);
        addRow(QStringLiteral("reflection/%1").arg(sourceTurn), QString(), QStringLiteral("review_source"),
               QStringLiteral("review:source_reflection"), ownerStatement,
               parseUtcFlexible(thread.value(QStringLiteral("reported_at")).toString()),
               EvidenceKind::SelfInference, EvidenceAuthority::SelfInference);
    }
    // 历史候选：与前台短期历史同身份（chat/<turn>）。
    for (const auto &value : thread.value(QStringLiteral("history")).toArray()) {
        const auto row = value.toObject();
        const qlonglong turn = row.value(QStringLiteral("turn")).toInteger();
        if (turn <= 0) continue;
        addRow(QStringLiteral("chat/%1").arg(turn), QStringLiteral("user_statement"),
               QStringLiteral("review_history"), QStringLiteral("review:history"),
               row.value(QStringLiteral("text")).toString(),
               parseUtcFlexible(row.value(QStringLiteral("reported_at")).toString()),
               EvidenceKind::UserStatement, EvidenceAuthority::UserStatement);
    }
    // 旧关注/想法：prior.ref 是 reflection/<turn>，root_ref 是 concern/<root>。
    const auto prior = thread.value(QStringLiteral("prior")).toObject();
    if (!prior.isEmpty()) {
        const QString ref = prior.value(QStringLiteral("ref")).toString();
        const QString rootRef = prior.value(QStringLiteral("root_ref")).toString();
        const int version = prior.value(QStringLiteral("version")).toInt();
        const QString reported = prior.value(QStringLiteral("reported_at")).toString();
        if (!rootRef.isEmpty()) {
            addRow(rootRef, QString(), QStringLiteral("review_prior"),
                   QStringLiteral("review:prior_concern"),
                   firstNonEmpty({prior.value(QStringLiteral("topic")).toString(),
                                  prior.value(QStringLiteral("own_reflection")).toString()}),
                   parseUtcFlexible(reported), EvidenceKind::SelfInference,
                   EvidenceAuthority::SelfInference);
        }
        if (!ref.isEmpty()) {
            CognitiveEvidence note;
            note.identity = evidenceIdentityForRef(ref, QString(), version,
                                                   QStringLiteral("note"));
            note.kind = EvidenceKind::SelfInference;
            note.authority = EvidenceAuthority::SelfInference;
            note.privacy = ownerPrivate ? QStringLiteral("private") : QStringLiteral("public");
            note.purpose = QStringLiteral("review_prior_note");
            note.usages.append(QStringLiteral("review:prior_note"));
            note.text = truncateText(prior.value(QStringLiteral("own_reflection")).toString(), 320);
            note.time.reportedAtUtc = parseUtcFlexible(reported);
            note.time.eventTimeUnknown = !note.time.reportedAtUtc.isValid();
            addEvidence(context, note, nullptr);
        }
    }
    // concern 对象（到期关注回顾）。
    const auto concern = thread.value(QStringLiteral("concern")).toObject();
    if (!concern.isEmpty()) {
        const QString ref = concern.value(QStringLiteral("ref")).toString();
        if (!ref.isEmpty()) {
            addRow(ref, QString(), QStringLiteral("review_concern"),
                   QStringLiteral("review:concern"),
                   firstNonEmpty({concern.value(QStringLiteral("topic")).toString(),
                                  concern.value(QStringLiteral("own_reflection")).toString()}),
                   parseUtcFlexible(concern.value(QStringLiteral("reported_at")).toString()),
                   EvidenceKind::SelfInference, EvidenceAuthority::SelfInference);
        }
    }
    // consult 结果：与前台 consult 同身份（chat/<turn>）。
    for (const auto &value : thread.value(QStringLiteral("consulted")).toArray()) {
        const auto row = value.toObject();
        const qlonglong turn = row.value(QStringLiteral("turn")).toInteger();
        if (turn <= 0) continue;
        addRow(QStringLiteral("chat/%1").arg(turn), QStringLiteral("user_statement"),
               QStringLiteral("review_consulted"), QStringLiteral("review:consulted"),
               row.value(QStringLiteral("text")).toString(),
               parseUtcFlexible(row.value(QStringLiteral("reported_at")).toString()),
               EvidenceKind::UserStatement, EvidenceAuthority::UserStatement);
    }
    // basis.reminders：待发提醒是真实委托，但不是“现在可以联系”的依据。
    for (const auto &value : thread.value(QStringLiteral("basis")).toObject()
                                  .value(QStringLiteral("reminders")).toArray()) {
        const auto row = value.toObject();
        const qlonglong id = row.value(QStringLiteral("id")).toInteger();
        if (id <= 0) continue;
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(QStringLiteral("reminder/%1").arg(id));
        evidence.kind = EvidenceKind::UserDelegation;
        evidence.authority = EvidenceAuthority::UserStatement;
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("review_basis");
        evidence.usages.append(QStringLiteral("review:basis_reminder"));
        evidence.text = truncateText(row.value(QStringLiteral("message")).toString(), 240);
        evidence.time.effectiveAtUtc = parseUtcFlexible(row.value(QStringLiteral("due_at")).toString());
        evidence.time.eventTimeUnknown = true;
        addEvidence(context, evidence, nullptr);
    }

    auto &view = context.domain(QStringLiteral("review_thread"));
    view.queriedDomain = true;
    view.status = view.serialized > 0 ? QStringLiteral("complete") : QStringLiteral("empty");
    context.coverage.setSource(QStringLiteral("review_thread"),
                               view.serialized > 0 ? QStringLiteral("complete")
                                                   : QStringLiteral("empty"));
    if (thread.contains(QStringLiteral("history_retrieval_status"))) {
        const QString status = thread.value(QStringLiteral("history_retrieval_status")).toString();
        const QString mapped = normalizeState(status);
        context.coverage.setSource(QStringLiteral("review_retrieval"), mapped,
                                   QStringLiteral("status=%1").arg(status));
        if (thread.value(QStringLiteral("history_retrieval_incomplete")).toBool()) {
            context.coverage.notes.append(
                QStringLiteral("回顾检索不完整：不能把未命中解释为没有历史"));
        }
    } else {
        context.coverage.setSource(QStringLiteral("review_retrieval"),
                                   QStringLiteral("not_queried"));
    }
}

void CognitiveContextBuilder::ingestReceipts(CognitiveContext &context,
                                             const CognitiveContextRequest &request) const
{
    if (request.receipts.isEmpty()) {
        context.coverage.setSource(QStringLiteral("execution_receipt"),
                                   QStringLiteral("not_queried"),
                                   QStringLiteral("本切片不额外读投递账本；只在调用方提供回执材料时纳入"));
        return;
    }
    for (const auto &value : request.receipts) {
        const auto row = value.toObject();
        QString ref = row.value(QStringLiteral("ref")).toString(
            row.value(QStringLiteral("action_key")).toString());
        if (ref.isEmpty()) continue;
        if (ref.startsWith(QStringLiteral("delivery/")))
            ref = ref.mid(int(QStringLiteral("delivery/").size()));
        CognitiveEvidence evidence;
        evidence.identity = evidenceIdentityForRef(ref, QStringLiteral("delivery"));
        evidence.kind = EvidenceKind::ExecutionResult;
        evidence.authority = EvidenceAuthority::ExecutionResult;
        evidence.privacy = QStringLiteral("private");
        evidence.purpose = QStringLiteral("execution_receipt");
        evidence.usages.append(QStringLiteral("receipt:outcome"));
        evidence.text = truncateText(row.value(QStringLiteral("outcome")).toString(), 120);
        evidence.time.reportedAtUtc =
            parseUtcFlexible(row.value(QStringLiteral("settled_at")).toString());
        evidence.time.eventTimeUnknown = !evidence.time.reportedAtUtc.isValid();
        addEvidence(context, evidence, nullptr);
        auto &view = context.domain(QStringLiteral("execution_receipt"));
        view.queriedDomain = true;
        view.refs.append(evidence.identity.displayRef());
        ++view.serialized;
    }
    auto &view = context.domain(QStringLiteral("execution_receipt"));
    view.status = QStringLiteral("complete");
    context.coverage.setSource(QStringLiteral("execution_receipt"), QStringLiteral("complete"));
}

void CognitiveContextBuilder::applyProjection(CognitiveContext &context,
                                              ShadowComparison *comparison) const
{
    // owner 私有域：群聊/第三方入口不携带，且不因“统一上下文”而扩权。
    static const QStringList ownerPrivateDomains{
        QStringLiteral("motivations"), QStringLiteral("commitments"),
        QStringLiteral("activities"), QStringLiteral("recent_activity_results"),
        QStringLiteral("internal_wakes"), QStringLiteral("memory_data"),
        QStringLiteral("concern"), QStringLiteral("activity"),
        QStringLiteral("execution_receipt"), QStringLiteral("private_evidence"),
        QStringLiteral("review_thread")};
    const bool ownerPrivate = context.scene.ownerPrivate;
    QList<CognitiveEvidence> kept;
    kept.reserve(context.evidence.size());
    int filtered = 0;
    int leaks = 0;
    for (auto evidence : context.evidence) {
        bool deny = false;
        QString reason;
        if (evidence.sensitive) {
            deny = true;
            reason = QStringLiteral("sensitive");
        }
        if (!ownerPrivate && (evidence.privacy == QStringLiteral("private")
                              || evidence.privacy == QStringLiteral("sensitive"))) {
            deny = true;
            reason = QStringLiteral("private_in_non_owner_projection");
        }
        if (!ownerPrivate) {
            for (const QString &usage : evidence.usages) {
                const QString domain = usage.section(QLatin1Char(':'), -1);
                const QString module = usage.section(QLatin1Char(':'), 0, 0);
                if (ownerPrivateDomains.contains(domain)
                    || (module == QStringLiteral("activity") && !ownerPrivate)
                    || (module == QStringLiteral("reminder") && !ownerPrivate
                        && evidence.channel != context.scene.channel)) {
                    deny = true;
                    reason = QStringLiteral("owner_private_domain:%1").arg(domain);
                    break;
                }
            }
        }
        // 群聊入口不携带其它私聊会话的原始内容（即使材料已过滤，这里再守一层）。
        if (context.scene.channel == Channel::kQQGroup && !evidence.channel.isEmpty()
            && evidence.channel != context.scene.channel
            && evidence.channel != Channel::kQQGroup) {
            deny = true;
            reason = QStringLiteral("cross_channel_in_group");
        }
        if (deny) {
            ++filtered;
            if (!reason.isEmpty() && !context.deniedDomains.contains(reason))
                context.deniedDomains.append(reason);
            if (evidence.privacy == QStringLiteral("private") && !ownerPrivate) {
                // 被正确过滤，不是 leak。
            }
            continue;
        }
        // 防御性 leak 检查：非 owner 私有投影中不应残留 private 证据。
        if (!ownerPrivate && evidence.privacy == QStringLiteral("private")) ++leaks;
        kept.append(evidence);
    }
    context.evidence = kept;
    if (comparison) {
        comparison->privacyFiltered += filtered;
        comparison->privacyLeaks += leaks;
    }
    context.visibleDomains.clear();
    for (auto it = context.domains.constBegin(); it != context.domains.constEnd(); ++it) {
        const QString &state = it.value().status;
        if (state == QStringLiteral("denied") || state == QStringLiteral("not_queried")
            || state == QStringLiteral("empty")) {
            continue;
        }
        context.visibleDomains.append(it.key());
    }
    context.visibleDomains.sort();
}

void CognitiveContextBuilder::finalizeBudget(CognitiveContext &context,
                                             const CognitiveContextRequest &request) const
{
    context.budget.snapshotBudgetBytes = CognitiveSnapshotBuilder::promptByteBudget();
    context.budget.evidenceCount = int(context.evidence.size());
    context.budget.uniqueEventCount = context.uniqueEventCount();
    context.budget.evidenceBytes = 0;
    context.budget.domainBytes.clear();
    for (const auto &evidence : context.evidence) {
        context.budget.evidenceBytes += evidence.text.toUtf8().size();
        auto &bytes = context.budget.domainBytes[evidence.purpose];
        bytes += evidence.text.toUtf8().size();
    }
    context.budget.duplicateMaterialBytes =
        qMax(0, context.budget.totalMaterialBytes - context.budget.uniqueEvidenceBytes);
    context.budget.legacyPromptBytes = request.legacyPromptBytes;
    context.budget.legacyDataBytes = request.legacyDataBytes;
    const QJsonObject payload = context.toShadowJson();
    const QByteArray compact = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    context.budget.contextBytes = compact.size();
    context.budget.contextChars = QString::fromUtf8(compact).size();
    context.budget.estimatedTokens = (context.budget.contextChars + 1) / 2;
    context.budget.overLegacyPrompt = request.legacyPromptBytes > 0
        && context.budget.contextBytes > request.legacyPromptBytes;
}

CognitiveContext CognitiveContextBuilder::buildShadow(const CognitiveContextRequest &request,
                                                      ShadowComparison *comparisonOut) const
{
    ShadowComparison localComparison;
    ShadowComparison *comparison = comparisonOut ? comparisonOut : &localComparison;
    *comparison = ShadowComparison{};
    m_comparison = comparison;

    CognitiveContext context;
    context.runId = request.runId.isEmpty() ? request.meta.runId : request.runId;
    context.triggerRef = request.triggerRef;
    context.purpose = request.purpose;
    context.scene.entry = request.entry;
    context.scene.subjectId = request.meta.subject;
    context.scene.actorId = request.meta.actorId;
    context.scene.speaker = request.meta.speaker;
    context.scene.speakerRole = request.meta.subject == QStringLiteral("user")
        ? QStringLiteral("owner") : QStringLiteral("other");
    context.scene.channel = request.meta.channel;
    context.scene.sessionId = request.meta.sessionId;
    const bool blindZone = ConfigManager::instance().isBlindZone(
        request.meta.channel, request.meta.sessionId);
    context.scene.blindZone = blindZone;
    context.scene.ownerPrivate = isOwnerPrivateScene(request.meta, blindZone);
    if (context.scene.ownerPrivate) context.scene.audienceKind = QStringLiteral("owner_private");
    else if (request.meta.channel == Channel::kQQGroup)
        context.scene.audienceKind = QStringLiteral("public_group");
    else context.scene.audienceKind = QStringLiteral("third_party_private");
    context.scene.privacy = context.scene.ownerPrivate ? QStringLiteral("private")
                                                       : QStringLiteral("public");
    context.scene.longTermMemoryEnabled = request.meta.longTermMemoryEnabled;
    context.scene.capabilityScope = context.scene.audienceKind;

    context.time.nowLocal = QDateTime::currentDateTime();
    context.time.nowUtc = context.time.nowLocal.toUTC();
    context.time.timezone = QString::fromUtf8(context.time.nowLocal.timeZone().id());
    context.time.localIso = context.time.nowLocal.toString(Qt::ISODate);
    context.time.utcIso = context.time.nowUtc.toString(Qt::ISODate);
    context.currentInput = request.currentInput;
    context.inputEventId = request.eventId;
    context.inputEvents = request.inputEvents;
    context.capabilities = request.capabilities;

    for (const QString &source : request.missingSources) {
        if (!context.coverage.sourceStates.contains(source))
            context.coverage.setSource(source, QStringLiteral("not_queried"));
    }

    ingestCurrentInput(context, request);
    ingestSnapshot(context, request);
    ingestMemoryContext(context, request);
    ingestUnderstanding(context, request);
    ingestConcerns(context, request);
    ingestActivities(context, request);
    ingestReminders(context, request);
    ingestHistory(context, request);
    ingestRetrieval(context, request);
    ingestRecentContext(context, request);
    ingestPrivateEvidence(context, request);
    ingestToolItems(context, request);
    ingestConsulted(context, request);
    ingestReviewThread(context, request);
    ingestReceipts(context, request);

    applyProjection(context, comparison);
    context.coverage.finalize();
    finalizeBudget(context, request);

    ShadowComparison legacy = compare(context, request.legacyEvidence);
    comparison->legacyRefCount = legacy.legacyRefCount;
    comparison->legacyStableRefCount = legacy.legacyStableRefCount;
    comparison->matchedRefs = legacy.matchedRefs;
    comparison->missingInUnified = legacy.missingInUnified;
    comparison->extraInUnified = legacy.extraInUnified;
    for (const QString &note : legacy.notes) {
        if (!comparison->notes.contains(note)) comparison->notes.append(note);
    }

    // 去重/版本/跨模块统计。
    QHash<QString, QSet<QString>> facetsByEvent;
    QHash<QString, QSet<int>> versionsByRef;
    int duplicates = 0;
    for (const auto &evidence : context.evidence) {
        facetsByEvent[evidence.identity.eventKey()].insert(evidence.identity.facet);
        versionsByRef[evidence.identity.sourceKind + QLatin1Char('|') + evidence.identity.ref]
            .insert(evidence.identity.version);
        QSet<QString> modules;
        for (const QString &usage : evidence.usages)
            modules.insert(usage.section(QLatin1Char(':'), 0, 0));
        if (modules.size() >= 2) ++duplicates;
    }
    comparison->duplicateAcrossModules = duplicates;
    comparison->repeatedEventFacets = 0;
    for (auto it = facetsByEvent.constBegin(); it != facetsByEvent.constEnd(); ++it)
        if (it.value().size() > 1) ++comparison->repeatedEventFacets;
    comparison->crossVersionGroups = 0;
    for (auto it = versionsByRef.constBegin(); it != versionsByRef.constEnd(); ++it)
        if (it.value().size() > 1) ++comparison->crossVersionGroups;
    comparison->unifiedEvidenceCount = context.evidenceCount();

    // 原始证据覆盖：chat/raw/source/delivery/receipt 等原始事件。
    QSet<QString> present;
    for (const auto &evidence : context.evidence) present.insert(evidence.identity.eventKey());
    int rawTotal = 0;
    int rawCovered = 0;
    for (auto it = request.legacyEvidence.constBegin();
         it != request.legacyEvidence.constEnd(); ++it) {
        const QString key = it.key();
        if (key.startsWith(QStringLiteral("ref:")) || isRunScopedHandle(key)) continue;
        const EvidenceIdentity identity = evidenceIdentityForRef(key);
        if (!identity.isValid() || !identity.stable) continue;
        const bool raw = identity.sourceKind == QStringLiteral("chat")
            || identity.sourceKind == QStringLiteral("raw")
            || identity.sourceKind == QStringLiteral("source")
            || identity.sourceKind == QStringLiteral("delivery")
            || identity.sourceKind == QStringLiteral("receipt")
            || identity.sourceKind == QStringLiteral("current_input");
        if (!raw) continue;
        ++rawTotal;
        if (present.contains(identity.eventKey())
            || [&] {
                   for (const auto &evidence : context.evidence)
                       if (evidence.modelHandles.contains(key)) return true;
                   return false;
               }()) {
            ++rawCovered;
        }
    }
    comparison->rawEvidenceTotal = rawTotal;
    comparison->rawEvidenceCovered = rawCovered;
    comparison->rawEvidenceCoverage = rawTotal > 0
        ? double(rawCovered) / double(rawTotal) : 0.0;

    if (context.coverage.sourceStates.value(QStringLiteral("retrieval"))
        == QStringLiteral("degraded")) {
        comparison->notes.append(QStringLiteral("retrieval_degraded_not_empty"));
    }
    if (comparison->privacyLeaks > 0)
        comparison->notes.append(QStringLiteral("privacy_leak_detected"));
    m_comparison = nullptr;
    return context;
}

ShadowComparison CognitiveContextBuilder::compare(const CognitiveContext &context,
                                                  const QJsonObject &legacyEvidence)
{
    ShadowComparison comparison;
    comparison.unifiedEvidenceCount = context.evidenceCount();
    QSet<QString> eventKeys;
    QSet<QString> stableKeys;
    QHash<QString, QStringList> handles;
    for (const auto &evidence : context.evidence) {
        eventKeys.insert(evidence.identity.eventKey());
        stableKeys.insert(evidence.identity.stableKey());
        for (const QString &handle : evidence.modelHandles)
            handles[handle].append(evidence.identity.stableKey());
    }
    int runScoped = 0;
    QSet<QString> matchedKeys;
    for (auto it = legacyEvidence.constBegin(); it != legacyEvidence.constEnd(); ++it) {
        const QString key = it.key();
        ++comparison.legacyRefCount;
        if (key.startsWith(QStringLiteral("ref:")) || isRunScopedHandle(key)) {
            ++runScoped;
            continue;
        }
        const EvidenceIdentity identity = evidenceIdentityForRef(key);
        if (!identity.isValid()) continue;
        if (!identity.stable) {
            ++runScoped;
            continue;
        }
        ++comparison.legacyStableRefCount;
        if (eventKeys.contains(identity.eventKey())
            || stableKeys.contains(identity.stableKey())
            || handles.contains(key)) {
            ++comparison.matchedRefs;
            matchedKeys.insert(identity.eventKey());
        } else {
            ++comparison.missingInUnified;
            if (comparison.notes.size() < 8)
                comparison.notes.append(QStringLiteral("legacy_ref_missing:%1").arg(key));
        }
    }
    comparison.extraInUnified = qMax(0, eventKeys.size() - matchedKeys.size());
    if (runScoped > 0)
        comparison.notes.append(
            QStringLiteral("legacy_run_scoped_refs=%1（ref:N 等不是 stable_ref，不计入覆盖）")
                .arg(runScoped));
    if (comparison.legacyStableRefCount == 0) {
        comparison.notes.append(
            QStringLiteral("legacy_evidence_empty_or_unstable：没有可比较的稳定引用"));
    }
    return comparison;
}
