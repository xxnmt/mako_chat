#include "memoryagent.h"

#include "memoryexecutor.h"

#include <QDebug>
#include <QJsonDocument>
#include <QSet>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>

#include "memorymanager.h"
#include "memorysourcepolicy.h"
#include "memoryretrieval.h"
#include "memoryembedding.h"
#include "memorydashscope.h"
#include "configmanager.h"
#include "memorytextutils.h"
#include "memorytimerange.h"

namespace {

void configureRetrievalService(MemoryRetrievalService &service,
                               const MemoryRetrievalConfig &config)
{
    if (!config.embeddingEnabled)
        return;
    if (config.embeddingProvider == QStringLiteral("local_hash_test")) {
        const int dimension = config.embeddingDimension > 0
            ? config.embeddingDimension : 64;
        service.setEmbeddingProvider(
            std::make_shared<LocalHashEmbeddingProvider>(dimension));
        return;
    }
    if (config.embeddingProvider != QStringLiteral("dashscope"))
        return;
    const QString apiKey = !config.embeddingApiKey.isEmpty()
        ? config.embeddingApiKey
        : qEnvironmentVariable(config.embeddingApiKeyEnv.toUtf8().constData());
    if (apiKey.isEmpty() || config.embeddingEndpoint.isEmpty())
        return;
    const int dimension = config.embeddingDimension > 0
        ? config.embeddingDimension : 1024;
    service.setEmbeddingProvider(std::make_shared<DashScopeEmbeddingProvider>(
        QUrl(config.embeddingEndpoint), apiKey,
        config.embeddingModel.isEmpty() ? QStringLiteral("text-embedding-v4")
                                        : config.embeddingModel,
        dimension));
    if (config.rerankEnabled && !config.rerankEndpoint.isEmpty()) {
        service.setReranker(std::make_shared<DashScopeReranker>(
            QUrl(config.rerankEndpoint), apiKey,
            config.rerankModel.isEmpty() ? QStringLiteral("qwen3-rerank")
                                         : config.rerankModel));
    }
}

const int kMaxQueryLength = 200;
const int kMaxToolIds = 8;

bool hasOnlyKeys(const QJsonObject &args, const QStringList &allowed)
{
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            return false;
        }
    }
    return true;
}

QString sourceLabelForSession(const MemoryAgentScope &scope,
                              const QString &sourceSessionId)
{
    if (!sourceSessionId.isEmpty() && sourceSessionId == scope.sessionId) {
        return QStringLiteral("current_group");
    }
    return QStringLiteral("allowed_group");
}

QString queryTextFromArgs(const QJsonObject &args)
{
    return args.value(QStringLiteral("query")).toString().trimmed();
}

int boundedInt(const QJsonValue &value, int fallback, int minimum, int maximum)
{
    if (!value.isDouble()) {
        return fallback;
    }
    return qBound(minimum, value.toInt(fallback), maximum);
}

bool parseIsoDateTime(const QString &value, QDateTime *out)
{
    if (!out || value.trimmed().isEmpty()) {
        return false;
    }
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODate);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    }
    if (!parsed.isValid()) {
        return false;
    }
    *out = parsed;
    return true;
}

bool readAgentTimeRange(const QJsonObject &args,
                        MemoryTimeRange *range,
                        QString *error)
{
    const bool hasFrom = args.contains(QStringLiteral("time_from"));
    const bool hasTo = args.contains(QStringLiteral("time_to"));
    if (!hasFrom && !hasTo) {
        return true;
    }
    if (hasFrom != hasTo) {
        if (error) {
            *error = QStringLiteral("invalid_arguments");
        }
        return false;
    }
    QDateTime from;
    QDateTime to;
    if (!parseIsoDateTime(args.value(QStringLiteral("time_from")).toString(), &from)
        || !parseIsoDateTime(args.value(QStringLiteral("time_to")).toString(), &to)
        || from >= to
        || from.secsTo(to) > 31LL * 24 * 3600) {
        if (error) {
            *error = QStringLiteral("invalid_time_range");
        }
        return false;
    }
    range->from = from;
    range->to = to;
    range->label = QStringLiteral("agent");
    range->valid = true;
    return true;
}

int countMatchingTerms(const QString &text, const QStringList &terms)
{
    int score = 0;
    for (const QString &term : terms) {
        if (text.contains(term, Qt::CaseInsensitive)) {
            ++score;
        }
    }
    return score;
}

} // namespace

MemoryAgentTools::MemoryAgentTools(MemoryManager *memoryManager,
                                   const MemoryAgentScope &scope,
                                   bool useQueryTimeFallback)
    : m_memoryManager(memoryManager)
    , m_scope(scope)
    , m_useQueryTimeFallback(useQueryTimeFallback)
{
}

QJsonObject MemoryAgentTools::errorObject(const QString &code,
                                          const QString &message) const
{
    return QJsonObject{
        {QStringLiteral("error"), code},
        {QStringLiteral("message"), message}
    };
}

QJsonObject MemoryAgentTools::itemToJson(const MemoryAgentItem &item) const
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), item.id);
    object.insert(QStringLiteral("kind"), item.kind);
    object.insert(QStringLiteral("text"), item.text);
    if (item.time.isValid()) {
        object.insert(QStringLiteral("time"),
                      item.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }
    object.insert(QStringLiteral("source"), item.source);
    if (!item.personId.isEmpty())
        object.insert(QStringLiteral("person_id"), item.personId);
    if (!item.speaker.isEmpty())
        object.insert(QStringLiteral("speaker"), item.speaker);
    if (!item.speakerRole.isEmpty())
        object.insert(QStringLiteral("speaker_role"), item.speakerRole);
    if (!item.derived.isEmpty())
        object.insert(QStringLiteral("derived"), item.derived);
    if (!item.groupLabel.isEmpty()) {
        object.insert(QStringLiteral("group"), item.groupLabel);
    }
    object.insert(QStringLiteral("is_viewer"), item.isViewer);
    return object;
}

QString MemoryAgentTools::groupLabelFor(const QString &groupId)
{
    if (groupId.isEmpty()) {
        return {};
    }
    if (m_groupNames.contains(groupId)) {
        return m_groupNames.value(groupId);
    }
    const QString raw = m_memoryManager
                            ? m_memoryManager->getGroupDisplayName(groupId)
                            : QString();
    const QString label = MemoryTextUtils::sanitizeIdentityText(
        raw, QStringLiteral("群聊"));
    m_groupNames.insert(groupId, label);
    return label;
}

void MemoryAgentTools::remember(const MemoryAgentItem &item)
{
    if (item.id.isEmpty() || m_cache.contains(item.id)) {
        return;
    }
    if (m_ordered.size() >= qMax(1, m_scope.maxResults)) {
        m_truncated = true;
        return;
    }
    m_cache.insert(item.id, item);
    m_ordered.append(item);
}

QJsonObject MemoryAgentTools::execute(const QString &name,
                                      const QJsonObject &args,
                                      QString *error)
{
    if (error) {
        error->clear();
    }
    if (!m_memoryManager) {
        if (error) {
            *error = QStringLiteral("memory_unavailable");
        }
        return errorObject(QStringLiteral("error"),
                           QStringLiteral("memory manager unavailable"));
    }
    if (name == QStringLiteral("memory_search")) {
        return search(args, error);
    }
    if (name == QStringLiteral("memory_read")) {
        return read(args, error);
    }
    if (name == QStringLiteral("memory_related")) {
        return related(args, error);
    }
    if (error) {
        *error = QStringLiteral("unknown_tool");
    }
    return errorObject(QStringLiteral("unknown_tool"),
                       QStringLiteral("unknown memory tool"));
}

QJsonObject MemoryAgentTools::search(const QJsonObject &args, QString *error)
{
    if (!hasOnlyKeys(args, {QStringLiteral("query"),
                            QStringLiteral("kinds"),
                            QStringLiteral("limit"),
                            QStringLiteral("time_from"),
                            QStringLiteral("time_to"),
                            QStringLiteral("time_target")})) {
        if (error) {
            *error = QStringLiteral("invalid_arguments");
        }
        return errorObject(QStringLiteral("invalid_arguments"),
                           QStringLiteral("unsupported argument"));
    }

    const QString query = queryTextFromArgs(args);
    if (query.isEmpty() || query.size() > kMaxQueryLength) {
        if (error) {
            *error = QStringLiteral("invalid_arguments");
        }
        return errorObject(QStringLiteral("invalid_arguments"),
                           QStringLiteral("query must be 1-200 chars"));
    }

    QStringList kinds;
    if (args.contains(QStringLiteral("kinds"))) {
        const QJsonValue kindsValue = args.value(QStringLiteral("kinds"));
        if (!kindsValue.isArray()) {
            if (error) {
                *error = QStringLiteral("invalid_arguments");
            }
            return errorObject(QStringLiteral("invalid_arguments"),
                               QStringLiteral("kinds must be an array"));
        }
        for (const QJsonValue &value : kindsValue.toArray()) {
            const QString kind = value.toString();
            if (kind != QStringLiteral("recent")
                && kind != QStringLiteral("summary")
                && kind != QStringLiteral("group_recent")
                && kind != QStringLiteral("interaction_summary")
                && kind != QStringLiteral("claim")
                && kind != QStringLiteral("episodic")
                && kind != QStringLiteral("long_term_summary")
                && kind != QStringLiteral("chat_history")) {
                if (error) {
                    *error = QStringLiteral("invalid_arguments");
                }
                return errorObject(QStringLiteral("invalid_arguments"),
                                   QStringLiteral("unsupported kind"));
            }
            kinds << kind;
        }
    }
    if (kinds.isEmpty()) {
        kinds = {QStringLiteral("group_recent"),
                 QStringLiteral("interaction_summary"),
                 QStringLiteral("claim"),
                 QStringLiteral("episodic"),
                 QStringLiteral("long_term_summary")};
    }
    const bool wantsRecent = kinds.contains(QStringLiteral("recent"))
                             || kinds.contains(QStringLiteral("group_recent"));
    const bool wantsInteractionSummary =
        kinds.contains(QStringLiteral("summary"))
        || kinds.contains(QStringLiteral("interaction_summary"));

    const int limit = boundedInt(
        args.value(QStringLiteral("limit")),
        qMin(6, qMax(1, m_scope.maxResults)), 1, 10);
    MemoryTimeRange agentTimeRange;
    if (!readAgentTimeRange(args, &agentTimeRange, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("invalid_time_range");
        }
        return errorObject(QStringLiteral("invalid_time_range"),
                           QStringLiteral("invalid time_from/time_to"));
    }
    const MemoryTimeRange parsedTimeRange = m_useQueryTimeFallback
                                                ? MemoryTimeRangePlanner::parse(
                                                      query, m_scope.now())
                                                : MemoryTimeRange();
    const MemoryTimeRange timeRange = agentTimeRange.valid
                                          ? agentTimeRange
                                          : parsedTimeRange;
    const MemoryTimeTarget timeTarget = memoryTimeTargetFromString(
        args.value(QStringLiteral("time_target")).toString());

    QJsonArray resultItems;
    QJsonArray retrievals;
    QStringList terms = RecentContextPlanner::extractTerms(query);

    const auto config = ConfigManager::instance().getMemoryPolicyConfig().retrieval;
    MemoryRetrievalService service(m_memoryManager);
    configureRetrievalService(service, config);
    RetrievalRequest base;
    base.query = query;
    base.subject = m_scope.viewerIsOwner ? QStringLiteral("user") : m_scope.viewerPersonId;
    base.viewerPersonId = m_scope.viewerPersonId;
    base.viewerIsOwner = m_scope.viewerIsOwner;
    base.accounts = m_scope.recentQueryTemplate.viewerIds;
    base.terms = terms;
    base.currentChannel = m_scope.channel;
    base.currentSession = m_scope.sessionId;
    base.excludeCurrent = false;
    base.useIndex = config.indexEnabled;
    base.candidateLimit = config.candidateLimit;
    base.rerankLimit = config.rerankLimit;

    // 1) recent raw messages：复用只读时间窗检索，服务端 scope 固定。
    const bool includeSummaries = wantsInteractionSummary || timeRange.valid;
    const int recentLimit = (timeRange.valid && includeSummaries)
                                ? qMin(limit, 2)
                                : limit;
    if (wantsRecent && !m_scope.allowedGroupIds.isEmpty()) {
        auto request = base;

        request.purpose = QStringLiteral("tool.recent");
        if (timeRange.valid) {
            request.from = timeRange.from;
            request.until = timeRange.to;
        }

        request.includeOthers = !m_scope.recentQueryTemplate.onlyViewer;
        request.requireTermMatch = true;
        request.includeUnmatched = timeRange.valid;
        request.limit = recentLimit;
        request.sourceKinds = {QStringLiteral("raw")};
        QStringList allowed;
        for (const auto &groupId : m_scope.allowedGroupIds)
            allowed.append(QStringLiteral("qq_group|") + groupId);
        request.allowedSources = allowed;

        const auto unified = service.retrieve(request);
        auto retrievalReport = unified.toJson();
        retrievalReport.remove("evidence");
        retrievalReport.insert("purpose", request.purpose);
        retrievals.append(retrievalReport);
        for (const auto &evidence : unified.evidence) {
            if (resultItems.size() >= recentLimit)
                break;
            if (evidence.sourceType != QStringLiteral("raw")
                || !evidence.sourceRef.startsWith(QStringLiteral("raw/")))
                continue;
            if (timeRange.valid && evidence.speechAt.isValid()
                && (evidence.speechAt < timeRange.from
                    || evidence.speechAt >= timeRange.to))
                continue;
            const QString text = MemoryTextUtils::sanitizePromptText(evidence.text);
            if (text.isEmpty())
                continue;
            MemoryAgentItem item;
            item.id = QStringLiteral("recent:%1")
                .arg(evidence.sourceRef.mid(QStringLiteral("raw/").size()));
            item.kind = QStringLiteral("recent");
            item.text = text;
            item.time = evidence.speechAt;
            item.source = sourceLabelForSession(m_scope, evidence.session);
            item.groupLabel = groupLabelFor(evidence.session);
            item.personId = evidence.speakerId;
            item.rawEventId = evidence.sourceRef.mid(
                QStringLiteral("raw/").size()).toLongLong();
            item.sourceSessionId = evidence.session;
            item.isViewer = evidence.own;
            const int before = m_ordered.size();
            remember(item);
            if (m_ordered.size() > before) {
                resultItems.append(itemToJson(item));
            }
        }
    }

    // 2) interaction summaries：统一服务取候选，群级摘要仍需授权范围。
    const bool summariesAllowed = m_scope.channel == Channel::kQQGroup
                                  || m_scope.viewerIsOwner;
    if (summariesAllowed
        && includeSummaries
        && !m_scope.allowedGroupIds.isEmpty()
        && resultItems.size() < limit) {
        QStringList sessions = m_scope.allowedGroupIds;
        if (m_scope.channel == Channel::kQQGroup) {
            sessions = {m_scope.sessionId};
        }
        auto request = base;

        request.purpose = QStringLiteral("tool.interaction_summary");
        if (timeRange.valid) {
            request.from = timeRange.from;
            request.until = timeRange.to;
        }

        request.includeUnmatched = timeRange.valid;
        request.limit = limit;
        request.sourceKinds = {QStringLiteral("interaction")};
        QStringList allowed;
        for (const auto &session : sessions)
            allowed.append(QStringLiteral("qq_group|") + session);
        request.allowedSources = allowed;

        const auto unified = service.retrieve(request);
        auto retrievalReport = unified.toJson();
        retrievalReport.remove("evidence");
        retrievalReport.insert("purpose", request.purpose);
        retrievals.append(retrievalReport);
        for (const auto &evidence : unified.evidence) {
            if (resultItems.size() >= limit)
                break;
            if (evidence.sourceType != QStringLiteral("interaction")
                || !evidence.sourceRef.startsWith(QStringLiteral("interaction/")))
                continue;
            if (timeRange.valid && evidence.speechAt.isValid()
                && (evidence.speechAt < timeRange.from
                    || evidence.speechAt >= timeRange.to))
                continue;
            const QString text = MemoryTextUtils::sanitizePromptText(evidence.text);
            if (text.isEmpty())
                continue;
            MemoryAgentItem item;
            item.id = QStringLiteral("summary:%1")
                .arg(evidence.sourceRef.mid(QStringLiteral("interaction/").size()));
            item.kind = QStringLiteral("summary");
            item.text = text;
            item.time = evidence.speechAt;
            item.source = sourceLabelForSession(m_scope, evidence.session);
            item.groupLabel = groupLabelFor(evidence.session);
            item.interactionId = evidence.sourceRef.mid(
                QStringLiteral("interaction/").size()).toLongLong();
            item.sourceSessionId = evidence.session;
            const int before = m_ordered.size();
            remember(item);
            if (m_ordered.size() > before)
                resultItems.append(itemToJson(item));
        }
    }

    // 3) claims：统一服务取候选，仍由 MemoryAccessPolicy 做逐条授权。
    if (kinds.contains(QStringLiteral("claim"))
        && (m_scope.viewerIsOwner || m_scope.membershipVerified)
        && resultItems.size() < limit) {
        auto request = base;

        request.purpose = QStringLiteral("tool.claim");
        if (timeRange.valid) {
            request.from = timeRange.from;
            request.until = timeRange.to;
        }

        request.limit = limit;
        request.sourceKinds = {QStringLiteral("claim")};
        QStringList allowed;
        for (const auto &groupId : m_scope.allowedGroupIds)
            allowed.append(QStringLiteral("qq_group|") + groupId);
        if (m_scope.channel == Channel::kQQGroup)
            allowed.append(QStringLiteral("qq_group|") + m_scope.sessionId);
        request.allowedSources = allowed;

        QHash<qlonglong, MemoryClaim> claims;
        for (const auto &claim : m_memoryManager->getActiveClaims(-1))
            claims.insert(claim.id, claim);
        request.accept = [&](const MemoryEvidence &entry) {
            const auto it = claims.constFind(entry.sourceRef.section('/', 1).toLongLong());
            return it != claims.cend() && MemoryAccessPolicy::evaluate(*it, m_scope.access).state
                == MemoryAccessState::Shareable;
        };
        const auto unified = service.retrieve(request);
        auto retrievalReport = unified.toJson();
        retrievalReport.remove("evidence");
        retrievalReport.insert("purpose", request.purpose);
        retrievals.append(retrievalReport);
        for (const auto &evidence : unified.evidence) {
            if (resultItems.size() >= limit)
                break;
            if (evidence.sourceType != QStringLiteral("claim"))
                continue;
            const auto claimId = evidence.sourceRef.mid(QStringLiteral("claim/").size()).toLongLong();
            if (!claims.contains(claimId))
                continue;
            const MemoryClaim &claim = claims.value(claimId);
            const MemoryAccessDecision decision =
                MemoryAccessPolicy::evaluate(claim, m_scope.access);
            if (decision.state != MemoryAccessState::Shareable)
                continue;
            if (timeRange.valid && claim.validTimeStart.isValid()
                && (claim.validTimeStart < timeRange.from
                    || claim.validTimeStart >= timeRange.to))
                continue;
            MemoryAgentItem item;
            item.id = QStringLiteral("claim:%1").arg(claim.id);
            item.kind = QStringLiteral("claim");
            item.text = MemoryTextUtils::sanitizePromptText(claim.statement);
            item.time = claim.validTimeStart;
            item.source = sourceLabelForSession(m_scope, claim.sourceSessionId);
            item.groupLabel = groupLabelFor(claim.sourceSessionId);
            item.personId = claim.aboutPersonId;
            item.interactionId = claim.interactionId;
            item.claimId = claim.id;
            item.sourceSessionId = claim.sourceSessionId;
            if (item.text.isEmpty())
                continue;
            const int before = m_ordered.size();
            remember(item);
            if (m_ordered.size() > before)
                resultItems.append(itemToJson(item));
        }
    }

    // 4) episodic：统一服务取候选，MemorySourcePolicy 仍逐条授权。
    if (kinds.contains(QStringLiteral("episodic"))
        && resultItems.size() < limit && m_memoryManager) {
        QHash<qlonglong, EpisodicMemory> memories;
        for (const auto &memory : m_memoryManager->getActiveEpisodicMemories(0.0, -1, {}, {}, {}, false))
            memories.insert(memory.id, memory);
        auto request = base;

        request.purpose = QStringLiteral("tool.episodic");
        if (timeTarget == MemoryTimeTarget::EventTime && timeRange.valid) {
            request.from = timeRange.from;
            request.until = timeRange.to;
        }

        request.includeUnmatched = timeRange.valid;
        request.includeAllEpisodic = true;
        request.limit = limit;
        request.sourceKinds = {QStringLiteral("episode")};

        request.accept = [&](const MemoryEvidence &entry) {
            const auto it = memories.constFind(entry.sourceRef.section('/', 1).toLongLong());
            return it != memories.cend() && MemorySourcePolicy::canReadEpisodic(
                m_scope, it->origin, it->visibility, it->subject, it->sessionId);
        };
        const auto unified = service.retrieve(request);
        auto retrievalReport = unified.toJson();
        retrievalReport.remove("evidence");
        retrievalReport.insert("purpose", request.purpose);
        retrievals.append(retrievalReport);
        for (const auto &evidence : unified.evidence) {
            if (resultItems.size() >= limit)
                break;
            if (evidence.sourceType != QStringLiteral("episode")
                || !evidence.sourceRef.startsWith(QStringLiteral("episode/")))
                continue;
            const auto memoryId = evidence.sourceRef.mid(
                QStringLiteral("episode/").size()).toLongLong();
            if (!memories.contains(memoryId))
                continue;
            const EpisodicMemory &memory = memories.value(memoryId);
            if (!MemorySourcePolicy::canReadEpisodic(
                    m_scope, memory.origin, memory.visibility,
                    memory.subject, memory.sessionId))
                continue;
            if (timeRange.valid
                && timeTarget == MemoryTimeTarget::EventTime
                && memory.eventTime.isValid()
                && (memory.eventTime < timeRange.from
                    || memory.eventTime >= timeRange.to))
                continue;
            MemoryAgentItem item;
            item.id = QStringLiteral("episodic:%1").arg(memory.id);
            item.kind = QStringLiteral("episodic");
            item.text = MemoryTextUtils::sanitizePromptText(memory.content);
            item.time = memory.eventTime;
            item.source = memory.origin == QStringLiteral("pet")
                              ? QStringLiteral("internal")
                              : sourceLabelForSession(m_scope, memory.sessionId);
            item.personId = memory.subject;
            item.episodicId = memory.id;
            item.sourceSessionId = memory.sessionId;
            item.isViewer = memory.subject == QStringLiteral("user")
                            || memory.subject == m_scope.viewerPersonId;
            if (item.text.isEmpty())
                continue;
            const int before = m_ordered.size();
            remember(item);
            if (m_ordered.size() > before)
                resultItems.append(itemToJson(item));
        }
    }

    // 5) long-term summary：统一服务；没有可靠覆盖区间，不把 createdAt 当 message_time。
    if (kinds.contains(QStringLiteral("long_term_summary"))
        && resultItems.size() < limit && m_memoryManager) {
        QList<QPair<QString, QString>> sources;
        if (MemorySourcePolicy::canReadLongTermSummary(
                m_scope, Channel::kPet, Channel::kDefaultSession)) {
            sources.append({Channel::kPet, Channel::kDefaultSession});
        }
        if (MemorySourcePolicy::canReadLongTermSummary(
                m_scope, m_scope.channel, m_scope.sessionId)
            && !sources.contains({m_scope.channel, m_scope.sessionId})) {
            sources.append({m_scope.channel, m_scope.sessionId});
        }
        auto request = base;

        request.purpose = QStringLiteral("tool.long_term_summary");

        request.limit = limit;
        request.sourceKinds = {QStringLiteral("summary")};
        request.accept = [&](const MemoryEvidence &entry) {
            return MemorySourcePolicy::canReadLongTermSummary(m_scope, entry.channel, entry.session);
        };
        QStringList allowed;
        for (const auto &source : sources)
            allowed.append(source.first + QLatin1Char('|') + source.second);
        request.allowedSources = allowed;

        const auto unified = service.retrieve(request);
        auto retrievalReport = unified.toJson();
        retrievalReport.remove("evidence");
        retrievalReport.insert("purpose", request.purpose);
        retrievals.append(retrievalReport);
        for (const auto &evidence : unified.evidence) {
            if (resultItems.size() >= limit)
                break;
            if (evidence.sourceType != QStringLiteral("summary")
                || !evidence.sourceRef.startsWith(QStringLiteral("summary/")))
                continue;
            const QString text = MemoryTextUtils::sanitizePromptText(evidence.text);
            if (text.isEmpty())
                continue;
            MemoryAgentItem item;
            item.id = QStringLiteral("long_summary:%1")
                .arg(evidence.sourceRef.mid(QStringLiteral("summary/").size()));
            item.kind = QStringLiteral("long_term_summary");
            item.text = text;
            item.source = evidence.session == Channel::kDefaultSession
                ? QStringLiteral("internal")
                : sourceLabelForSession(m_scope, evidence.session);
            item.longTermSummaryId = evidence.sourceRef.mid(
                QStringLiteral("summary/").size()).toLongLong();
            item.sourceSessionId = evidence.session;
            const int before = m_ordered.size();
            remember(item);
            if (m_ordered.size() > before)
                resultItems.append(itemToJson(item));
        }
    }

    // 6) chat_history：用户发言与助手表达分角色提供；两者都可以被检索，
    // 但助手只证明“她曾经这样说过”，不证明用户答应或事实已发生。
    if (kinds.contains(QStringLiteral("chat_history"))
        && resultItems.size() < limit && m_memoryManager) {
        QList<QPair<QString, QString>> sources;
        if (MemorySourcePolicy::canReadChatHistory(
                m_scope, Channel::kPet, Channel::kDefaultSession)) {
            sources.append({Channel::kPet, Channel::kDefaultSession});
        }
        if (MemorySourcePolicy::canReadChatHistory(
                m_scope, m_scope.channel, m_scope.sessionId)
            && !sources.contains({m_scope.channel, m_scope.sessionId})) {
            sources.append({m_scope.channel, m_scope.sessionId});
        }
        if (m_scope.viewerIsOwner && m_scope.ownerCrossChannelPrivate) {
            sources.append({Channel::kPet, Channel::kDefaultSession});
            for (const auto &account : m_scope.recentQueryTemplate.viewerIds)
                sources.append({Channel::kQQPrivate, account});
        }
        if (!sources.isEmpty()) {
            auto request = base;

            request.sourceKinds = {"chat"};
            request.purpose = QStringLiteral("tool.chat_history");
            if (timeRange.valid) {
                request.from = timeRange.from;
                request.until = timeRange.to;
            }

            request.limit = limit;

            QStringList allowed;
            for (const auto &source : sources)
                allowed.append(source.first + QLatin1Char('|') + source.second);
            request.allowedSources = allowed;

            const auto unified = service.retrieve(request);
            auto retrievalReport = unified.toJson();
            retrievalReport.remove("evidence");
            retrievalReport.insert("purpose", request.purpose);
            retrievals.append(retrievalReport);
            for (const auto &evidence : unified.evidence) {
                if (resultItems.size() >= limit)
                    break;
                if (evidence.sourceType != QStringLiteral("chat"))
                    continue;
                if (timeRange.valid && evidence.speechAt.isValid()
                    && (evidence.speechAt < timeRange.from
                        || evidence.speechAt >= timeRange.to))
                    continue;
                const QString text = MemoryTextUtils::sanitizePromptText(evidence.text);
                if (text.isEmpty())
                    continue;
                const QString ref = evidence.sourceRef;
                const bool assistant = evidence.speakerRole == QStringLiteral("assistant");
                const QString idPart = ref.section(QLatin1Char(':'), 0, 0);
                const qlonglong turnId = idPart.section(QLatin1Char('/'), 1, 1).toLongLong();
                MemoryAgentItem item;
                item.id = assistant
                    ? QStringLiteral("reply:%1").arg(turnId)
                    : QStringLiteral("chat:%1:user").arg(turnId);
                item.kind = QStringLiteral("chat_history");
                item.text = text;
                item.time = evidence.speechAt;
                item.source = sourceLabelForSession(m_scope, evidence.session);
                item.personId = evidence.speakerId;
                item.speaker = evidence.speakerName;
                item.speakerRole = evidence.speakerRole;
                item.derived = evidence.derived;
                item.historyTurnId = turnId;
                item.sourceSessionId = evidence.session;
                item.isViewer = evidence.own && !assistant;
                const int before = m_ordered.size();
                remember(item);
                if (m_ordered.size() > before) {
                    resultItems.append(itemToJson(item));
                }
            }
        }

    }

    QJsonObject result;
    bool candidateTruncated = m_truncated || resultItems.size() >= limit;
    bool degraded = false;
    for (const auto &report : retrievals) {
        candidateTruncated |= report.toObject()["truncated_by_limit"].toBool();
        degraded |= report.toObject()["execution_status"] == "degraded";
    }
    result.insert(QStringLiteral("items"), resultItems);
    result.insert(QStringLiteral("retrievals"), retrievals);
    result.insert(QStringLiteral("execution_status"), degraded ? "degraded" : "complete");
    result.insert(QStringLiteral("coverage_status"), "unknown");
    result.insert(QStringLiteral("truncated_by_limit"), candidateTruncated);
    result.insert(QStringLiteral("truncated"), candidateTruncated);
    return result;
}

QJsonObject MemoryAgentTools::read(const QJsonObject &args, QString *error)
{
    if (!hasOnlyKeys(args, {QStringLiteral("ids")})) {
        if (error) {
            *error = QStringLiteral("invalid_arguments");
        }
        return errorObject(QStringLiteral("invalid_arguments"),
                           QStringLiteral("unsupported argument"));
    }
    const QJsonValue idsValue = args.value(QStringLiteral("ids"));
    if (!idsValue.isArray() || idsValue.toArray().isEmpty()
        || idsValue.toArray().size() > kMaxToolIds) {
        if (error) {
            *error = QStringLiteral("invalid_arguments");
        }
        return errorObject(QStringLiteral("invalid_arguments"),
                           QStringLiteral("ids must be 1-8 items"));
    }

    QJsonArray resultItems;
    for (const QJsonValue &value : idsValue.toArray()) {
        const QString id = value.toString();
        if (id.isEmpty() || !m_cache.contains(id)) {
            if (error) {
                *error = QStringLiteral("denied");
            }
            return errorObject(QStringLiteral("denied"),
                               QStringLiteral("unknown or expired id"));
        }
        resultItems.append(itemToJson(m_cache.value(id)));
    }
    QJsonObject result;
    result.insert(QStringLiteral("items"), resultItems);
    result.insert(QStringLiteral("truncated"), false);
    return result;
}

QJsonObject MemoryAgentTools::related(const QJsonObject &args, QString *error)
{
    if (!hasOnlyKeys(args, {QStringLiteral("id"), QStringLiteral("limit")})) {
        if (error) {
            *error = QStringLiteral("invalid_arguments");
        }
        return errorObject(QStringLiteral("invalid_arguments"),
                           QStringLiteral("unsupported argument"));
    }
    const QString id = args.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || !m_cache.contains(id)) {
        if (error) {
            *error = QStringLiteral("denied");
        }
        return errorObject(QStringLiteral("denied"),
                           QStringLiteral("unknown or expired id"));
    }
    const int limit = boundedInt(args.value(QStringLiteral("limit")), 6, 1, 10);
    const MemoryAgentItem source = m_cache.value(id);
    QJsonArray resultItems;
    for (const MemoryAgentItem &candidate : m_ordered) {
        if (candidate.id == id) {
            continue;
        }
        // 同会话，或同 interaction；不按 person_id 自由反查。
        const bool sameSession = !source.sourceSessionId.isEmpty()
                                 && candidate.sourceSessionId == source.sourceSessionId;
        const bool sameInteraction = source.interactionId > 0
                                     && candidate.interactionId == source.interactionId;
        if (!sameSession && !sameInteraction) {
            continue;
        }
        resultItems.append(itemToJson(candidate));
        if (resultItems.size() >= limit) {
            break;
        }
    }
    QJsonObject result;
    result.insert(QStringLiteral("items"), resultItems);
    result.insert(QStringLiteral("truncated"), resultItems.size() >= limit);
    return result;
}

bool MemoryAgentTools::canExposeTools(const RequestMeta &meta,
                                        const MemoryAgentConfig &config)
{
    if (!config.enabled || config.mode != QStringLiteral("tool")) {
        return false;
    }
    if (meta.channel != Channel::kPet
        && meta.channel != Channel::kQQGroup
        && meta.channel != Channel::kQQPrivate) {
        return false;
    }
    return meta.turnKind == QStringLiteral("reply");
}
