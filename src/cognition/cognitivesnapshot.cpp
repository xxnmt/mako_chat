#include "cognitivesnapshot.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include "configmanager.h"
#include "memorymanager.h"

namespace {

// 每个域最多给模型看的条数；manager 的默认 limit 也是 8，这里再兜底一次。
constexpr int kDomainLimit = 8;

QString isoFromUtc(const QDateTime &value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QString promptText(const QString &text, int maxChars)
{
    const QString trimmed = text.trimmed();
    if (maxChars <= 0 || trimmed.size() <= maxChars) return trimmed;
    return trimmed.left(maxChars);
}

QJsonArray limitArray(const QJsonArray &source, int maxItems)
{
    QJsonArray result;
    for (const auto &value : source) {
        if (result.size() >= maxItems) break;
        result.append(value);
    }
    return result;
}

// --- 快照哈希 --------------------------------------------------------------
// 只对“可分类内容 + 读取 scope/purpose”取哈希：排除 builtAtUtc / runId，
// 保证同一份库内容在同一渠道/会话/主体/用途下重复构建得到同一指纹。

QJsonArray factsForHash(const QList<CognitiveFactRef> &facts)
{
    QJsonArray rows;
    for (const auto &fact : facts) {
        rows.append(QJsonObject{
            {QStringLiteral("ref"), fact.ref},
            {QStringLiteral("authority"), evidenceAuthorityName(fact.authority)},
            {QStringLiteral("subject"), fact.subjectId},
            {QStringLiteral("channel"), fact.channel},
            {QStringLiteral("session"), fact.sessionId},
            {QStringLiteral("occurred_at"), isoFromUtc(fact.occurredAtUtc)},
            {QStringLiteral("text"), fact.text},
            {QStringLiteral("privacy"), fact.privacy},
            {QStringLiteral("sensitive"), fact.sensitive}});
    }
    return rows;
}

QJsonObject canonicalContent(const CognitiveSnapshot &snapshot)
{
    QJsonArray motivations;
    for (const auto &motivation : snapshot.motivations) {
        motivations.append(QJsonObject{{QStringLiteral("ref"), motivation.ref},
                                       {QStringLiteral("kind"), motivation.kind},
                                       {QStringLiteral("summary"), motivation.summary},
                                       {QStringLiteral("version"), motivation.version}});
    }
    QJsonArray commitments;
    for (const auto &commitment : snapshot.commitments) {
        commitments.append(QJsonObject{{QStringLiteral("ref"), commitment.ref},
                                       {QStringLiteral("kind"), commitment.kind},
                                       {QStringLiteral("state"), commitment.state},
                                       {QStringLiteral("due_at"), isoFromUtc(commitment.dueAtUtc)},
                                       {QStringLiteral("expires_at"), isoFromUtc(commitment.expiresAtUtc)},
                                       {QStringLiteral("text"), commitment.text}});
    }
    return QJsonObject{
        {QStringLiteral("trigger_ref"), snapshot.triggerRef},
        // R12-1a：scope/purpose 进入规范化内容，同一库内容在不同渠道/会话/主体/
        // 用途下必须得到不同指纹；builtAtUtc/runId 仍然排除。
        {QStringLiteral("purpose"), snapshot.purpose},
        {QStringLiteral("scope"),
         QJsonObject{{QStringLiteral("channel"), snapshot.currentChannel},
                     {QStringLiteral("session"), snapshot.currentSession},
                     {QStringLiteral("subject_id"), snapshot.subjectId}}},
        {QStringLiteral("observations"), factsForHash(snapshot.observations)},
        {QStringLiteral("self_expressions"), factsForHash(snapshot.selfExpressions)},
        {QStringLiteral("inferences"), factsForHash(snapshot.inferences)},
        {QStringLiteral("motivations"), motivations},
        {QStringLiteral("commitments"), commitments},
        {QStringLiteral("activities"), snapshot.activities},
        {QStringLiteral("internal_wakes"), snapshot.internalWakes},
        {QStringLiteral("policy_view"), snapshot.policyView}};
}

QString computeSnapshotHash(const CognitiveSnapshot &snapshot)
{
    const QByteArray payload =
        QJsonDocument(canonicalContent(snapshot)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().left(16));
}

// --- 提示词 JSON -----------------------------------------------------------

QJsonObject factPromptJson(const CognitiveFactRef &fact)
{
    QJsonObject row{{QStringLiteral("ref"), fact.ref},
                    {QStringLiteral("authority"), evidenceAuthorityName(fact.authority)},
                    {QStringLiteral("at"), isoFromUtc(fact.occurredAtUtc)},
                    {QStringLiteral("text"), promptText(fact.text, 160)}};
    if (fact.sensitive) row.insert(QStringLiteral("sensitive"), true);
    return row;
}

QJsonObject motivationPromptJson(const SelfMotivation &motivation)
{
    return QJsonObject{{QStringLiteral("ref"), motivation.ref},
                       {QStringLiteral("kind"), motivation.kind},
                       {QStringLiteral("summary"), promptText(motivation.summary, 160)},
                       {QStringLiteral("version"), motivation.version}};
}

QJsonObject commitmentPromptJson(const DelegatedCommitment &commitment)
{
    QJsonObject row{{QStringLiteral("ref"), commitment.ref},
                    {QStringLiteral("kind"), commitment.kind},
                    {QStringLiteral("state"), commitment.state},
                    {QStringLiteral("due_at"), isoFromUtc(commitment.dueAtUtc)},
                    {QStringLiteral("expires_at"), isoFromUtc(commitment.expiresAtUtc)}};
    const QString text = promptText(commitment.text, 120);
    if (!text.isEmpty()) row.insert(QStringLiteral("message"), text);
    return row;
}

} // namespace

QString evidenceAuthorityName(EvidenceAuthority authority)
{
    switch (authority) {
    case EvidenceAuthority::UserStatement: return QStringLiteral("user_statement");
    case EvidenceAuthority::ExternalObservation: return QStringLiteral("external_observation");
    case EvidenceAuthority::ExecutionResult: return QStringLiteral("execution_result");
    case EvidenceAuthority::AssistantExpression: return QStringLiteral("assistant_expression");
    case EvidenceAuthority::SelfInference: return QStringLiteral("self_inference");
    case EvidenceAuthority::InternalState: return QStringLiteral("internal_state");
    }
    return QStringLiteral("unknown");
}

CognitiveSnapshotBuilder::CognitiveSnapshotBuilder(MemoryManager *memory)
    : m_memory(memory)
{
}

CognitiveSnapshot CognitiveSnapshotBuilder::build(const RequestMeta &meta, const QString &runId,
                                                  const QString &triggerRef, const QString &purpose) const
{
    // R12-1a：每次 build 重置统计；queried 来自记忆层查询的原始行数（受查询 LIMIT 影响，是下界），
    // eligible 用实际进入快照的行数（= 读取策略 + 群聊投影过滤之后）。
    // status/queriedDomain 在这里先给出查询侧状态，toPromptJson 再按序列化/预算结果收敛。
    m_stats = SnapshotBuildStats{};
    const auto recordDomain = [this](const QString &domain, int queried, int eligible,
                                     bool queriedDomain) {
        SnapshotDomainStats stat;
        stat.queried = queried;
        stat.eligible = eligible;
        stat.queriedDomain = queriedDomain;
        if (!queriedDomain || (queried == 0 && eligible == 0)) {
            stat.status = QStringLiteral("empty");
        } else if (queried > eligible) {
            // 查询命中但被读取策略过滤：过滤器口径见 notes 的 domain:filtered:<domain>。
            stat.status = QStringLiteral("filtered");
        } else if (eligible >= kDomainLimit) {
            // 查询本身就带 LIMIT：只能记“疑似查询上限（近似）”，见 domain:query_cap 说明。
            stat.status = QStringLiteral("query_cap");
        } else {
            stat.status = QStringLiteral("serialized");
        }
        m_stats.domains.insert(domain, stat);
    };
    const auto domainName = [](const char *name) { return QString::fromLatin1(name); };
    CognitiveSnapshot snapshot;
    snapshot.runId = runId;
    snapshot.triggerRef = triggerRef;
    snapshot.purpose = purpose;
    snapshot.builtAtUtc = QDateTime::currentDateTimeUtc();
    snapshot.currentChannel = meta.channel;
    snapshot.currentSession = meta.sessionId;
    if (!m_memory) {
        snapshot.subjectId = QStringLiteral("user");
        snapshot.snapshotHash = computeSnapshotHash(snapshot);
        return snapshot;
    }

    // snapshotSubjectId 是“对外身份”（主人 = user，第三方 = actor），
    // 数据库里的 subject 列则保留既有约定（qq:<uin>），两者分离避免查询不到行。
    snapshot.subjectId = m_memory->snapshotSubjectId(meta);
    const QString storedSubject = meta.subject.isEmpty() ? snapshot.subjectId : meta.subject;
    // 群聊 projection 只允许当前群渠道/会话的来源：她的自发动机、私人活动、内部唤醒
    // 都属于主人私聊域，群聊快照不携带（前台也不会向群聊注入快照）。
    const bool groupAudience = meta.channel == Channel::kQQGroup;
    // R12-1a：OpenWake 内部状态只在“主人私聊域”注入：群聊/第三方会话不携带；
    // 回顾上下文可能没有显式渠道（只知道是主人私聊来源），按既有约定视为主人私聊域。
    // 盲区不注入：盲区会话的上下文不读取/不携带任何 OpenWake 内部记录或调度状态
    // （不扩大任何现有权限；盲区的 policy_view.blind_zone=true 仅作只读说明）。
    const bool blindZone =
        ConfigManager::instance().isBlindZone(meta.channel, meta.sessionId);
    const bool ownerPrivateAudience =
        !blindZone
        && (meta.subject.isEmpty() || meta.subject == QStringLiteral("user"))
        && (meta.channel == Channel::kPet || meta.channel == Channel::kQQPrivate
            || meta.channel.isEmpty());

    // R12-1a：OpenWake 自身的内部记录（rest/reflect note）只进主人私聊快照，
    // 用 InternalState 权威域进入 inferences，永远不进入 observations/用户事实/承诺。
    // 读取有上限（≤2 条），避免跨轮无界累积。
    QList<QJsonObject> wakeNoteRows;
    if (ownerPrivateAudience) {
        // 只读 owner 私聊域的 OpenWake note；非 owner scope/第三方行不进入任何视图。
        for (const auto &value : m_memory->recentOpenWakeNotes(2, QStringLiteral("owner")))
            wakeNoteRows.append(value.toObject());
    }
    const int wakeNoteQueried = int(wakeNoteRows.size());
    // 一条 OpenWake note 映射成内部状态事实：ref=wake/<id>:<version>、subject=mako、private。
    const auto wakeFact = [](const QJsonObject &row) {
        CognitiveFactRef fact;
        fact.ref = row.value(QStringLiteral("ref")).toString();
        fact.authority = EvidenceAuthority::InternalState;
        fact.subjectId = QStringLiteral("mako");
        fact.occurredAtUtc = QDateTime::fromString(
            row.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs);
        fact.text = promptText(row.value(QStringLiteral("note")).toString(), 200);
        fact.privacy = QStringLiteral("private");
        fact.sensitive = false;
        return fact;
    };

    int observationsQueried = 0;
    for (const auto &value :
         m_memory->recentOwnerObservations(storedSubject, meta.channel, meta.sessionId, kDomainLimit,
                                           &observationsQueried, nullptr)) {
        const auto row = value.toObject();
        CognitiveFactRef fact;
        fact.ref = row.value(QStringLiteral("ref")).toString();
        // “owner observations” 查询按当前 subject 取：主人本人原话才是 user_statement，
        // 其它 subject（第三方会话）只能算渠道观察。
        fact.authority = storedSubject == QStringLiteral("user")
            ? EvidenceAuthority::UserStatement : EvidenceAuthority::ExternalObservation;
        fact.subjectId = row.value(QStringLiteral("subject")).toString();
        fact.channel = row.value(QStringLiteral("channel")).toString();
        fact.sessionId = row.value(QStringLiteral("session")).toString();
        fact.occurredAtUtc = QDateTime::fromString(
            row.value(QStringLiteral("occurred_at")).toString(), Qt::ISODateWithMs);
        fact.text = row.value(QStringLiteral("text")).toString();
        fact.privacy = meta.channel == Channel::kQQGroup
            ? QStringLiteral("public") : QStringLiteral("private");
        fact.sensitive = row.value(QStringLiteral("sensitive")).toBool();
        snapshot.observations.append(fact);
    }
    recordDomain(domainName("observations"), observationsQueried,
                 int(snapshot.observations.size()), true);

    int selfExpressionsQueried = 0;
    for (const auto &value :
         m_memory->recentSelfExpressions(storedSubject, meta.channel, meta.sessionId, kDomainLimit,
                                         &selfExpressionsQueried, nullptr)) {
        const auto row = value.toObject();
        CognitiveFactRef fact;
        fact.ref = row.value(QStringLiteral("ref")).toString();
        fact.authority = EvidenceAuthority::AssistantExpression;
        fact.subjectId = QStringLiteral("mako");
        fact.channel = row.value(QStringLiteral("channel")).toString();
        fact.sessionId = row.value(QStringLiteral("session")).toString();
        fact.occurredAtUtc = QDateTime::fromString(
            row.value(QStringLiteral("occurred_at")).toString(), Qt::ISODateWithMs);
        fact.text = row.value(QStringLiteral("text")).toString();
        fact.privacy = meta.channel == Channel::kQQGroup
            ? QStringLiteral("public") : QStringLiteral("private");
        snapshot.selfExpressions.append(fact);
    }
    recordDomain(domainName("self_expressions"), selfExpressionsQueried,
                 int(snapshot.selfExpressions.size()), true);

    int inferencesQueried = 0;
    // OpenWake 内部记录放在最前：按“每个非空域至少保留 1 条”裁剪时优先生效，
    // 保证后续前台/后台快照总能看到她最近的内部唤醒记录（仅 owner-private 构建）。
    for (const auto &row : wakeNoteRows) {
        if (snapshot.inferences.size() >= kDomainLimit) break;
        snapshot.inferences.append(wakeFact(row));
    }
    for (const auto &value : m_memory->recentInferences(storedSubject, kDomainLimit,
                                                        &inferencesQueried, nullptr)) {
        if (snapshot.inferences.size() >= kDomainLimit) break;
        const auto row = value.toObject();
        CognitiveFactRef fact;
        fact.ref = row.value(QStringLiteral("ref")).toString();
        fact.authority = EvidenceAuthority::SelfInference;
        fact.subjectId = QStringLiteral("mako");
        fact.channel = row.value(QStringLiteral("channel")).toString();
        fact.sessionId = row.value(QStringLiteral("session")).toString();
        fact.occurredAtUtc = QDateTime::fromString(
            row.value(QStringLiteral("occurred_at")).toString(), Qt::ISODateWithMs);
        fact.text = row.value(QStringLiteral("text")).toString();
        fact.privacy = QStringLiteral("private");
        if (groupAudience
            && (fact.channel != meta.channel || fact.sessionId != meta.sessionId)) {
            continue;
        }
        snapshot.inferences.append(fact);
    }
    // wake 记录来自额外的只读查询：把它的行数并进 queried（仍是下界），
    // 这样 eligible/omitted 能解释“为什么 inferences 有条目没序列化”。
    recordDomain(domainName("inferences"), inferencesQueried + wakeNoteQueried,
                 int(snapshot.inferences.size()), true);

    if (!groupAudience) {
        int motivationsQueried = 0;
        // OpenWake 的自发动机（可选）：与 inferences 的 note 同源，只表示“她自己想继续/休息”，
        // 不是对主人的承诺；放最前保证最小保留时可见。
        for (const auto &row : wakeNoteRows) {
            if (snapshot.motivations.size() >= kDomainLimit) break;
            SelfMotivation motivation;
            motivation.ref = row.value(QStringLiteral("ref")).toString();
            motivation.kind = QStringLiteral("open_wake");
            motivation.summary = promptText(row.value(QStringLiteral("note")).toString(), 160);
            motivation.version = row.value(QStringLiteral("version")).toInt();
            snapshot.motivations.append(motivation);
        }
        for (const auto &value : m_memory->currentMotivations(kDomainLimit,
                                                              &motivationsQueried, nullptr)) {
            if (snapshot.motivations.size() >= kDomainLimit) break;
            const auto row = value.toObject();
            SelfMotivation motivation;
            motivation.ref = row.value(QStringLiteral("ref")).toString();
            motivation.kind = row.value(QStringLiteral("kind")).toString();
            motivation.summary = row.value(QStringLiteral("summary")).toString();
            motivation.version = row.value(QStringLiteral("version")).toInt();
            snapshot.motivations.append(motivation);
        }
        recordDomain(domainName("motivations"), motivationsQueried + wakeNoteQueried,
                     int(snapshot.motivations.size()), true);
    } else {
        // 群聊投影不读取自发动机/私人活动/内部唤醒：记 0 并在 notes 里说明口径。
        recordDomain(domainName("motivations"), 0, 0, false);
        m_stats.notes.append(QStringLiteral("group_projection:motivations_not_queried"));
    }

    int commitmentsQueried = 0;
    for (const auto &value : m_memory->activeCommitments(meta.channel, meta.sessionId, kDomainLimit,
                                                         &commitmentsQueried, nullptr)) {
        const auto row = value.toObject();
        DelegatedCommitment commitment;
        commitment.ref = row.value(QStringLiteral("ref")).toString();
        commitment.kind = row.value(QStringLiteral("kind")).toString();
        commitment.state = row.value(QStringLiteral("state")).toString();
        commitment.dueAtUtc = QDateTime::fromString(
            row.value(QStringLiteral("due_at")).toString(), Qt::ISODateWithMs);
        commitment.expiresAtUtc = QDateTime::fromString(
            row.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
        commitment.text = row.value(QStringLiteral("text")).toString();
        if (commitment.text.isEmpty())
            commitment.text = row.value(QStringLiteral("message")).toString();
        snapshot.commitments.append(commitment);
    }
    recordDomain(domainName("commitments"), commitmentsQueried,
                 int(snapshot.commitments.size()), true);

    // 私人活动与内部唤醒只属于主人私聊域；群聊快照不读取，避免把私聊来源带进群投影。
    if (!groupAudience) {
        int activitiesQueried = 0;
        snapshot.activities = limitArray(
            m_memory->currentActivities(kDomainLimit, &activitiesQueried, nullptr), kDomainLimit);
        recordDomain(domainName("activities"), activitiesQueried,
                     int(snapshot.activities.size()), true);

        int wakesQueried = 0;
        const QJsonArray wakeRows =
            m_memory->internalWakes(kDomainLimit, &wakesQueried, nullptr);
        // OpenWake 调度状态：来自 open_wake_state(scope=owner)，只表示她自己的内部调度，
        // 永远不是主人约定；只在主人私聊域注入，放在最前保证预算裁剪时优先保留。
        const QJsonObject wakeState = ownerPrivateAudience
            ? m_memory->openWakeState(QStringLiteral("owner")) : QJsonObject{};
        if (!wakeState.isEmpty()) {
            const QString nextDueIso =
                wakeState.value(QStringLiteral("next_due_iso")).toString();
            snapshot.internalWakes.append(QJsonObject{
                {QStringLiteral("ref"), QStringLiteral("wake_schedule/owner")},
                {QStringLiteral("kind"), QStringLiteral("open_wake")},
                {QStringLiteral("at"), nextDueIso},
                // 与既有 own_next_review_at 同义：她自己的下一次内部唤醒时间。
                {QStringLiteral("own_next_review_at"), nextDueIso}});
            ++wakesQueried; // 调度状态也是本次查询到的一行内部状态
        }
        for (const auto &value : wakeRows) {
            if (snapshot.internalWakes.size() >= kDomainLimit) break;
            snapshot.internalWakes.append(value);
        }
        recordDomain(domainName("internal_wakes"), wakesQueried,
                     int(snapshot.internalWakes.size()), true);
    } else {
        recordDomain(domainName("activities"), 0, 0, false);
        recordDomain(domainName("internal_wakes"), 0, 0, false);
        m_stats.notes.append(QStringLiteral("group_projection:activities_not_queried"));
        m_stats.notes.append(QStringLiteral("group_projection:internal_wakes_not_queried"));
    }

    QJsonObject policyView{
        {QStringLiteral("purpose"), purpose},
        {QStringLiteral("channel"), meta.channel},
        {QStringLiteral("session"), meta.sessionId},
        {QStringLiteral("subject"), snapshot.subjectId},
        {QStringLiteral("owner_private"),
         (meta.subject == QStringLiteral("user")
          && (meta.channel == Channel::kPet || meta.channel == Channel::kQQPrivate))},
        {QStringLiteral("long_term_memory"),
         ConfigManager::instance().getMemoryPolicyConfig().longTermMemoryEnabled},
        {QStringLiteral("turn_long_term_memory"), meta.longTermMemoryEnabled},
        {QStringLiteral("sensitive"), meta.sensitive},
        {QStringLiteral("blind_zone"),
         ConfigManager::instance().isBlindZone(meta.channel, meta.sessionId)},
        {QStringLiteral("cross_channel_owner_private"),
         ConfigManager::instance().getMemoryPolicyConfig().agent.ownerCrossChannelPrivate}};
    snapshot.policyView = policyView;

    snapshot.snapshotHash = computeSnapshotHash(snapshot);
    return snapshot;
}

QJsonObject CognitiveSnapshotBuilder::toPromptJson(const CognitiveSnapshot &snapshot) const
{
    // 保留 build 已记录的 queried/eligible/queriedDomain；本函数重新记录
    // serialized/omitted、每域最终状态、字节数与预算相关 notes。
    m_stats.bytesBefore = 0;
    m_stats.bytesAfter = 0;
    for (auto it = m_stats.domains.begin(); it != m_stats.domains.end(); ++it) {
        (*it).serialized = 0;
        (*it).omitted = 0;
    }
    for (int i = int(m_stats.notes.size()) - 1; i >= 0; --i) {
        const QString &note = m_stats.notes.at(i);
        if (note.startsWith(QStringLiteral("budget:"))
            || note.startsWith(QStringLiteral("domain:")))
            m_stats.notes.removeAt(i);
    }

    const QStringList domainOrder{
        QStringLiteral("observations"), QStringLiteral("self_expressions"),
        QStringLiteral("inferences"), QStringLiteral("motivations"),
        QStringLiteral("commitments"), QStringLiteral("activities"),
        QStringLiteral("internal_wakes")};
    // R12-1a：提示词 JSON 只放数据域和只读的 policy_view；固定说明已移到
    // LLMService::cognitiveSnapshotMessage 的 system content，不占本数据预算。
    QJsonObject root;

    QJsonArray observations;
    for (const auto &fact : snapshot.observations) {
        if (observations.size() >= kDomainLimit) break;
        observations.append(factPromptJson(fact));
    }
    root.insert(QStringLiteral("observations"), observations);

    QJsonArray selfExpressions;
    for (const auto &fact : snapshot.selfExpressions) {
        if (selfExpressions.size() >= kDomainLimit) break;
        selfExpressions.append(factPromptJson(fact));
    }
    root.insert(QStringLiteral("self_expressions"), selfExpressions);

    QJsonArray inferences;
    for (const auto &fact : snapshot.inferences) {
        if (inferences.size() >= kDomainLimit) break;
        inferences.append(factPromptJson(fact));
    }
    root.insert(QStringLiteral("inferences"), inferences);

    QJsonArray motivations;
    for (const auto &motivation : snapshot.motivations) {
        if (motivations.size() >= kDomainLimit) break;
        motivations.append(motivationPromptJson(motivation));
    }
    root.insert(QStringLiteral("motivations"), motivations);

    QJsonArray commitments;
    for (const auto &commitment : snapshot.commitments) {
        if (commitments.size() >= kDomainLimit) break;
        commitments.append(commitmentPromptJson(commitment));
    }
    root.insert(QStringLiteral("commitments"), commitments);

    root.insert(QStringLiteral("activities"), limitArray(snapshot.activities, kDomainLimit));
    root.insert(QStringLiteral("internal_wakes"), limitArray(snapshot.internalWakes, kDomainLimit));
    root.insert(QStringLiteral("policy_view"), snapshot.policyView);

    const int budgetBytes = promptByteBudget();
    const auto serializedSize = [&root]() {
        return QJsonDocument(root).toJson(QJsonDocument::Compact).size();
    };
    m_stats.bytesBefore = serializedSize();

    // 裁剪规则（R12-1a）：
    //   1) policy_view 不是认知域，先整体让出预算；
    //   2) 再从“多于 1 条”的域按低优先→高优先删到每域只剩 1 条（最小有效条目）；
    //   3) 最小集仍装不下才按同一顺序整域 overflow，并显式记录 budget:overflow:<domain>；
    //   4) 最终仍超预算时记录 budget:unresolved，绝不静默假装各域齐全。
    const QStringList trimPriority{
        QStringLiteral("inferences"), QStringLiteral("activities"),
        QStringLiteral("motivations"), QStringLiteral("self_expressions"),
        QStringLiteral("internal_wakes"), QStringLiteral("commitments"),
        QStringLiteral("observations")};
    QStringList trimmedDomains;
    QStringList overflowDomains;
    QStringList budgetNotes;
    const auto addBudgetNote = [&budgetNotes](const QString &note) {
        if (!budgetNotes.contains(note)) budgetNotes.append(note);
    };
    if (serializedSize() > budgetBytes
        && !root.value(QStringLiteral("policy_view")).toObject().isEmpty()) {
        root.insert(QStringLiteral("policy_view"), QJsonObject{});
        addBudgetNote(QStringLiteral("budget:trimmed:policy_view"));
    }
    while (serializedSize() > budgetBytes) {
        bool changed = false;
        for (const auto &key : trimPriority) {
            QJsonArray values = root.value(key).toArray();
            if (values.size() <= 1) continue;
            values.removeLast();
            root.insert(key, values);
            changed = true;
            if (!trimmedDomains.contains(key)) trimmedDomains.append(key);
            addBudgetNote(QStringLiteral("budget:trimmed:") + key);
            break;
        }
        if (!changed) break;
    }
    for (const auto &key : trimPriority) {
        if (serializedSize() <= budgetBytes) break;
        QJsonArray values = root.value(key).toArray();
        if (values.isEmpty()) continue;
        root.insert(key, QJsonArray{});
        overflowDomains.append(key);
        addBudgetNote(QStringLiteral("budget:overflow:") + key);
    }
    if (serializedSize() > budgetBytes) addBudgetNote(QStringLiteral("budget:unresolved"));
    m_stats.bytesAfter = serializedSize();
    for (const auto &note : budgetNotes) m_stats.notes.append(note);

    // 每域最终状态：serialized/omitted 是提示词数据 JSON 里的真实行数；
    // 策略过滤/查询上限单独留 domain: notes（避免被“预算裁剪”的最终状态覆盖后无法追溯）。
    for (const auto &domain : domainOrder) {
        auto it = m_stats.domains.find(domain);
        if (it == m_stats.domains.end()) continue;
        (*it).serialized = int(root.value(domain).toArray().size());
        (*it).omitted = qMax(0, (*it).eligible - (*it).serialized);
        const bool overflowed = overflowDomains.contains(domain);
        const bool trimmed = trimmedDomains.contains(domain)
            || (*it).serialized < (*it).eligible;
        if (overflowed) {
            (*it).status = QStringLiteral("overflow");
        } else if (trimmed) {
            (*it).status = QStringLiteral("budget_trimmed");
        } else if ((*it).eligible == 0) {
            (*it).status = (*it).queried > 0 ? QStringLiteral("filtered")
                                             : QStringLiteral("empty");
        } else if ((*it).queried > (*it).eligible) {
            (*it).status = QStringLiteral("filtered");
        } else if ((*it).eligible >= kDomainLimit) {
            (*it).status = QStringLiteral("query_cap");
        } else {
            (*it).status = QStringLiteral("serialized");
        }
        if ((*it).queried > (*it).eligible) {
            const QString note = QStringLiteral("domain:filtered:") + domain;
            if (!m_stats.notes.contains(note)) m_stats.notes.append(note);
        }
        if ((*it).eligible >= kDomainLimit) {
            // 查询 SQL 自带 LIMIT，拿不到真实总数：明确标注为近似提示。
            const QString note =
                QStringLiteral("domain:query_cap:") + domain + QStringLiteral(":approx");
            if (!m_stats.notes.contains(note)) m_stats.notes.append(note);
        }
    }
    return root;
}

QString CognitiveSnapshotBuilder::promptPrefix()
{
    return QStringLiteral("【认知快照｜服务端事实分层｜只读】");
}

int CognitiveSnapshotBuilder::promptByteBudget()
{
    // 配置层已把值夹在 2048..8192；这里再兜底一次，避免异常配置把预算变成 0。
    return qBound(2048, ConfigManager::instance().getSnapshotBudgetBytes(), 8192);
}
