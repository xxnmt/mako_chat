#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>

#include "cognitivecontext.h"
#include "cognitivesnapshot.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "promptassembler.h"
#include "sentencedata.h"

namespace {

QDateTime nowUtc()
{
    return QDateTime::currentDateTimeUtc();
}

QString iso(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

RequestMeta ownerMeta()
{
    RequestMeta meta;
    meta.channel = Channel::kPet;
    meta.sessionId = Channel::kDefaultSession;
    meta.subject = QStringLiteral("user");
    meta.speaker = QStringLiteral("欧尼酱");
    meta.runId = QStringLiteral("shadow-run");
    meta.eventId = QStringLiteral("shadow-event");
    return meta;
}

CognitiveContextRequest baseRequest(const QString &entry = QStringLiteral("foreground_reply"))
{
    CognitiveContextRequest request;
    request.entry = entry;
    request.meta = ownerMeta();
    request.runId = request.meta.runId;
    request.triggerRef = request.meta.eventId;
    request.purpose = entry;
    return request;
}

HistoryTurn historyTurn(qlonglong id, const QString &userInput, const QString &rawReply,
                        const QDateTime &at)
{
    HistoryTurn turn;
    turn.id = id;
    turn.timestamp = at;
    turn.userInput = userInput;
    turn.rawReply = rawReply;
    turn.subject = QStringLiteral("user");
    turn.turnKind = QStringLiteral("reply");
    return turn;
}

CognitiveFactRef fact(const QString &ref, EvidenceAuthority authority, const QString &text,
                      const QString &channel, const QString &session,
                      const QDateTime &at, const QString &privacy = QStringLiteral("private"))
{
    CognitiveFactRef value;
    value.ref = ref;
    value.authority = authority;
    value.subjectId = QStringLiteral("user");
    value.text = text;
    value.channel = channel;
    value.sessionId = session;
    value.occurredAtUtc = at;
    value.privacy = privacy;
    return value;
}

QStringList usageModules(const CognitiveEvidence &evidence)
{
    QSet<QString> modules;
    for (const QString &usage : evidence.usages)
        modules.insert(usage.section(QLatin1Char(':'), 0, 0));
    QStringList result;
    result.reserve(modules.size());
    for (const QString &module : modules) result.append(module);
    result.sort();
    return result;
}

} // namespace

class CognitiveContextTest : public QObject
{
    Q_OBJECT
private slots:
    void identityNormalization();
    void refSpellingEquivalence();
    void sameEventAcrossModulesDedups();
    void differentEventsSameTextStayDistinct();
    void matterVersionsAndDerivedEvidenceDoNotMerge();
    void timeSemanticsAreServerOwned();
    void retrievalFailureIsNotEmptyHistory();
    void privacyProjectionBlocksPrivateInGroup();
    void memoryDataClaimsReuseAccessPolicy();
    void coverageDistinguishesEmptyDeniedNotQueried();
    void budgetAccountsDuplication();
    void legacyComparisonHonoursStableRefs();
    void snapshotBuilderMaterialsShareIdentity();
    void toolItemsMapToStableRefs();
    void projectionDedupesAndKeepsRefs();
    void projectionKeepsFullForUnserialized();
    void projectionBudgetTrimsExplicitly();
    void projectionGroupHasNoPrivateDomains();
};

void CognitiveContextTest::identityNormalization()
{
    const auto reply = evidenceIdentityForRef(QStringLiteral("reply/12"));
    const auto expr = evidenceIdentityForRef(QStringLiteral("chat/12:assistant"));
    const auto user = evidenceIdentityForRef(QStringLiteral("chat/12:user"));
    QCOMPARE(reply.eventKey(), QStringLiteral("chat|chat/12"));
    QCOMPARE(reply.stableKey(), expr.stableKey());
    QCOMPARE(reply.facet, QStringLiteral("assistant_expression"));
    QCOMPARE(user.facet, QStringLiteral("user_statement"));
    QCOMPARE(user.eventKey(), reply.eventKey());
    QVERIFY(user.stableKey() != reply.stableKey());

    // 不同轮次的相同文本是不同事件（身份不含文本）。
    const auto other = evidenceIdentityForRef(QStringLiteral("chat/13"));
    QVERIFY(other.eventKey() != user.eventKey());

    // episode/interaction/long_summary 是既有拼写的归一化，不是新事件。
    QCOMPARE(evidenceIdentityForRef(QStringLiteral("episode/3")).stableKey(),
             evidenceIdentityForRef(QStringLiteral("episodic/3")).stableKey());
    QCOMPARE(evidenceIdentityForRef(QStringLiteral("interaction/4")).stableKey(),
             evidenceIdentityForRef(QStringLiteral("interaction_summary/4")).stableKey());
    QCOMPARE(evidenceIdentityForRef(QStringLiteral("long_summary/5")).stableKey(),
             evidenceIdentityForRef(QStringLiteral("summary/5")).stableKey());

    // matter 认识：revision/<matter>/<v> 与 matter 名+版本必须归一成同一身份。
    const auto revision = evidenceIdentityForRef(QStringLiteral("revision/作息/2"));
    const auto matter = evidenceIdentityForMatter(QStringLiteral("作息"), 2);
    QCOMPARE(revision.sourceKind, QStringLiteral("matter"));
    QCOMPARE(revision.eventKey(), matter.eventKey());
    QCOMPARE(revision.displayRef(), QStringLiteral("revision/作息/2"));
    QVERIFY(revision.stableKey() != evidenceIdentityForMatter(QStringLiteral("作息"), 3).stableKey());

    // 原始来源/自述与事项认识不是同一类型。
    const auto source = evidenceIdentityForRef(QStringLiteral("source/9"));
    QCOMPARE(source.sourceKind, QStringLiteral("source"));
    QVERIFY(source.eventKey() != matter.eventKey());

    // 每个事件的身份与 facet 可逆展示。
    QCOMPARE(user.displayRef(), QStringLiteral("chat/12"));
    QCOMPARE(evidenceIdentityForRef(QStringLiteral("current_input")).eventKey(),
             QStringLiteral("current_input|current_input"));
}

void CognitiveContextTest::refSpellingEquivalence()
{
    // reply 与 chat:assistant 在历史/检索/工具三条路径里都可能出现，身份必须一致。
    const QStringList assistantSpellings{QStringLiteral("reply/7"),
                                         QStringLiteral("chat/7:assistant")};
    QString firstKey;
    for (const QString &ref : assistantSpellings) {
        const auto identity = evidenceIdentityForRef(ref);
        if (firstKey.isEmpty()) firstKey = identity.stableKey();
        QCOMPARE(identity.stableKey(), firstKey);
    }
    // 同一行的 user facet 与 assistant facet 共享 eventKey，但可区分。
    const auto user = evidenceIdentityForRef(QStringLiteral("chat/7:user"));
    QCOMPARE(user.eventKey(), evidenceIdentityForRef(assistantSpellings.first()).eventKey());
    QVERIFY(user.stableKey() != firstKey);
}

void CognitiveContextTest::sameEventAcrossModulesDedups()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("我来汇报");
    request.eventId = QStringLiteral("event-1");
    request.meta.receivedAtUtc = at;

    request.hasSnapshot = true;
    request.snapshot.runId = QStringLiteral("run");
    request.snapshot.purpose = QStringLiteral("foreground_reply");
    request.snapshot.builtAtUtc = at;
    request.snapshot.observations.append(fact(QStringLiteral("chat/1"),
                                              EvidenceAuthority::UserStatement,
                                              QStringLiteral("我来汇报"),
                                              QStringLiteral("pet"), QStringLiteral("default"), at));
    request.snapshot.selfExpressions.append(fact(QStringLiteral("chat/1"),
                                                 EvidenceAuthority::AssistantExpression,
                                                 QStringLiteral("收到"), QStringLiteral("pet"),
                                                 QStringLiteral("default"), at));
    request.snapshotStats.domains.insert(QStringLiteral("observations"),
                                         SnapshotDomainStats{1, 1, 1, 0, true,
                                                             QStringLiteral("serialized")});
    request.snapshotStats.domains.insert(QStringLiteral("self_expressions"),
                                         SnapshotDomainStats{1, 1, 1, 0, true,
                                                             QStringLiteral("serialized")});
    request.history.append(historyTurn(1, QStringLiteral("我来汇报"),
                                       QStringLiteral("收到"), at));
    MemoryEvidence retrieved;
    retrieved.sourceRef = QStringLiteral("chat/1:user");
    retrieved.sourceType = QStringLiteral("chat");
    retrieved.derived = QStringLiteral("original");
    retrieved.speakerRole = QStringLiteral("user");
    retrieved.text = QStringLiteral("我来汇报");
    retrieved.eventTime = at;
    retrieved.privacy = QStringLiteral("private");
    request.retrieved.append(retrieved);
    request.retrievalDone = true;
    request.retrievalStatus = QStringLiteral("complete");
    request.privateEvidence.append(QJsonObject{{QStringLiteral("evidence_ref"), QStringLiteral("chat/1")},
                                               {QStringLiteral("reported_at"), iso(at)},
                                               {QStringLiteral("own"), true},
                                               {QStringLiteral("text"), QStringLiteral("我来汇报")}});
    MemoryAgentItem toolItem;
    toolItem.id = QStringLiteral("recent:9");
    toolItem.historyTurnId = 1;
    toolItem.derived = QStringLiteral("original");
    toolItem.speakerRole = QStringLiteral("user");
    toolItem.text = QStringLiteral("我来汇报");
    toolItem.time = at;
    request.toolItems.append(toolItem);
    request.toolStatus = QStringLiteral("complete");
    request.legacyEvidence = QJsonObject{
        {QStringLiteral("chat/1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("historical_report")}}},
        {QStringLiteral("reply/1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("prior_assistant_expression")}}},
        {QStringLiteral("ref:1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("index")}}}};

    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);

    const auto *user = context.findByKey(QStringLiteral("chat|chat/1#user_statement"));
    const auto *assistant = context.findByKey(QStringLiteral("chat|chat/1#assistant_expression"));
    QVERIFY2(user != nullptr, "同一条 chat/1 的 user 面应存在统一身份");
    QVERIFY2(assistant != nullptr, "同一条 chat/1 的 assistant 面应存在统一身份");
    // 5 个入口（快照观测/历史/检索/私密证据/工具）归并成一个 user 证据。
    QVERIFY2(usageModules(*user).size() == 5,
             qPrintable(QStringLiteral("actual usages: %1").arg(user->usages.join(QLatin1Char(',')))));
    QCOMPARE(usageModules(*assistant).size(), 2);
    // 事件 1 = chat/1（两个 facet），事件 2 = 当前输入本身（current_input 是独立入口身份，
    // 不与历史里的 chat/1 合并，因为它是本轮新事件）。
    QCOMPARE(context.uniqueEventCount(), 2);
    QCOMPARE(comparison.repeatedEventFacets, 1);
    QCOMPARE(comparison.duplicateAcrossModules, 2);
    QCOMPARE(comparison.identityConflicts, 0);
    // reply/1 与 chat/1:assistant 是同一事件；ref:1 是 run 内目录编号，不伪装成 stable_ref。
    QCOMPARE(comparison.matchedRefs, 2);
    QCOMPARE(comparison.missingInUnified, 0);
    QVERIFY(comparison.notes.join(QLatin1Char('\n')).contains(QStringLiteral("run_scoped")));
}

void CognitiveContextTest::differentEventsSameTextStayDistinct()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.history.append(historyTurn(1, QStringLiteral("同一句话"), QString(), at));
    request.history.append(historyTurn(2, QStringLiteral("同一句话"), QString(), at.addSecs(60)));
    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);
    QCOMPARE(context.evidenceCount(), 2);
    QCOMPARE(context.uniqueEventCount(), 2);
    QCOMPARE(comparison.identityConflicts, 0);
    QCOMPARE(comparison.repeatedEventFacets, 0);
    QCOMPARE(comparison.duplicateAcrossModules, 0);
    QVERIFY(context.findByKey(QStringLiteral("chat|chat/1#user_statement")) != nullptr);
    QVERIFY(context.findByKey(QStringLiteral("chat|chat/2#user_statement")) != nullptr);
    // 文本相同不构成合并理由：两条证据各自保留来源时间。
    for (const auto &evidence : context.evidence) {
        QVERIFY(evidence.time.eventAtUtc.isValid());
    }
}

void CognitiveContextTest::matterVersionsAndDerivedEvidenceDoNotMerge()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.canReadUnderstanding = true;
    // 同一事项两个历史版本 + 相同文本的原始来源：版本必须共存，派生认识不能与原始证据合并。
    const auto makeRow = [&](const QString &evidenceRef, int revision) {
        return QJsonObject{
            {QStringLiteral("topic"), QStringLiteral("作息")},
            {QStringLiteral("quote"), QStringLiteral("今天早睡")},
            {QStringLiteral("understanding"), QStringLiteral("主人打算调整作息")},
            {QStringLiteral("reported_at"), iso(at)},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("evidence_ref"), evidenceRef},
            {QStringLiteral("self_report"), QJsonObject{
                 {QStringLiteral("quote"), QStringLiteral("今天早睡")},
                 {QStringLiteral("evidence_ref"), evidenceRef},
                 {QStringLiteral("reported_at"), iso(at)}}}};
    };
    request.understanding.append(makeRow(QStringLiteral("source/9"), 2));
    request.understanding.append(makeRow(QStringLiteral("source/10"), 3));
    request.understandingMatters = QStringList{QStringLiteral("作息"), QStringLiteral("作息")};
    // 快照侧同一个事项的 revision 拼写：必须与 understanding 归一成同一身份。
    request.hasSnapshot = true;
    request.snapshot.builtAtUtc = at;
    request.snapshot.inferences.append(fact(QStringLiteral("revision/作息/2"),
                                            EvidenceAuthority::SelfInference,
                                            QStringLiteral("主人打算调整作息"),
                                            QStringLiteral("pet"), QStringLiteral("default"), at));
    request.snapshot.inferences.append(fact(QStringLiteral("revision/作息/3"),
                                            EvidenceAuthority::SelfInference,
                                            QStringLiteral("主人打算调整作息"),
                                            QStringLiteral("pet"), QStringLiteral("default"), at));

    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);
    const auto *v2 = context.findByKey(QStringLiteral("matter|matter/作息@2"));
    const auto *v3 = context.findByKey(QStringLiteral("matter|matter/作息@3"));
    QVERIFY2(v2 != nullptr, "matter revision 2 应存在");
    QVERIFY2(v3 != nullptr, "matter revision 3 应存在");
    QVERIFY(v2->identity.stableKey() != v3->identity.stableKey());
    QCOMPARE(v2->kind, EvidenceKind::SelfInference);
    QVERIFY(usageModules(*v2).contains(QStringLiteral("understanding")));
    QVERIFY(usageModules(*v2).contains(QStringLiteral("snapshot")));
    // 原始来源是另一个身份，不与派生认识合并。
    const auto *source = context.findByKey(QStringLiteral("source|source/9"));
    QVERIFY(source != nullptr);
    QCOMPARE(source->kind, EvidenceKind::UserStatement);
    QVERIFY(source->identity.eventKey() != v2->identity.eventKey());
    QVERIFY(v2->derivedFrom.contains(QStringLiteral("source/9")));
    QCOMPARE(comparison.crossVersionGroups, 1);
    QCOMPARE(comparison.identityConflicts, 0);
}

void CognitiveContextTest::timeSemanticsAreServerOwned()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("明天再说");
    request.meta.receivedAtUtc = at;
    // 文本含相对时间，但没有 event_time：必须标记“相对时间未锚定”，不能自己推断今天/明天。
    request.history.append(historyTurn(1, QStringLiteral("明天再说"), QString(), at));
    request.hasSnapshot = true;
    request.snapshot.builtAtUtc = at;
    CognitiveFactRef unknownTime = fact(QStringLiteral("chat/4"),
                                        EvidenceAuthority::UserStatement,
                                        QStringLiteral("昨天说的那件事"), QStringLiteral("pet"),
                                        QStringLiteral("default"), QDateTime{});
    request.snapshot.observations.append(unknownTime);

    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);
    QVERIFY(context.time.nowLocal.isValid());
    QVERIFY(context.time.nowUtc.isValid());
    QVERIFY(!context.time.timezone.isEmpty());
    QVERIFY(context.time.nowUtc == context.time.nowLocal.toUTC());
    // 服务端按本次构建的 now 计算相对标签，而不是继承文本。
    QVERIFY(context.time.relativeLabel(context.time.nowUtc).startsWith(QStringLiteral("今天")));
    QVERIFY(context.time.relativeLabel(context.time.nowUtc.addDays(-1)).startsWith(QStringLiteral("昨天")));
    QVERIFY(context.time.relativeLabel(context.time.nowUtc.addDays(1)).startsWith(QStringLiteral("明天")));
    QCOMPARE(context.time.relativeLabel(QDateTime{}), QStringLiteral("unknown"));
    // 未锚定相对时间的证据被标记；已知事件时间的不被误标。
    QVERIFY(comparison.relativeTimeUnanchored >= 1);
    bool sawUnanchored = false;
    for (const auto &evidence : context.evidence) {
        if (evidence.time.relativeExpressionUnanchored) sawUnanchored = true;
    }
    QVERIFY(sawUnanchored);
}

void CognitiveContextTest::retrievalFailureIsNotEmptyHistory()
{
    {
        CognitiveContextRequest request = baseRequest();
        request.retrievalDone = true;
        request.retrievalStatus = QStringLiteral("degraded");
        CognitiveContextBuilder builder(nullptr);
        const CognitiveContext context = builder.buildShadow(request);
        QCOMPARE(context.coverage.sourceStates.value(QStringLiteral("retrieval")),
                 QStringLiteral("degraded"));
        QCOMPARE(context.coverage.overall, QStringLiteral("degraded"));
        QVERIFY(!context.coverage.emptySources.contains(QStringLiteral("retrieval")));
        QVERIFY(context.coverage.notes.join(QLatin1Char('\n')).contains(QStringLiteral("检索失败")));
    }
    {
        CognitiveContextRequest request = baseRequest();
        request.retrievalDone = false;
        CognitiveContextBuilder builder(nullptr);
        const CognitiveContext context = builder.buildShadow(request);
        QCOMPARE(context.coverage.sourceStates.value(QStringLiteral("retrieval")),
                 QStringLiteral("not_queried"));
        QVERIFY(context.coverage.sourceStates.value(QStringLiteral("retrieval"))
                != context.coverage.sourceStates.value(QStringLiteral("history")));
    }
    {
        CognitiveContextRequest request = baseRequest();
        request.retrievalDone = true;
        request.retrievalStatus = QStringLiteral("complete");
        CognitiveContextBuilder builder(nullptr);
        const CognitiveContext context = builder.buildShadow(request);
        QCOMPARE(context.coverage.sourceStates.value(QStringLiteral("retrieval")),
                 QStringLiteral("empty"));
    }
}

void CognitiveContextTest::privacyProjectionBlocksPrivateInGroup()
{
    const QDateTime at = nowUtc();
    auto fill = [&](CognitiveContextRequest &request) {
        request.currentInput = QStringLiteral("在吗");
        request.meta.receivedAtUtc = at;
        request.hasSnapshot = true;
        request.snapshot.builtAtUtc = at;
        request.snapshot.motivations.append(
            SelfMotivation{QStringLiteral("concern/1"), QStringLiteral("concern"),
                           QStringLiteral("想问问主人的睡眠"), 3});
        request.snapshot.commitments.append(
            DelegatedCommitment{QStringLiteral("reminder/2"), QStringLiteral("reminder"),
                                QStringLiteral("pending"), at.addSecs(3600), QDateTime{},
                                QStringLiteral("提醒主人喝水")});
        request.snapshot.activities.append(
            QJsonObject{{QStringLiteral("ref"), QStringLiteral("activity/1")},
                        {QStringLiteral("version"), 2},
                        {QStringLiteral("summary"), QStringLiteral("写了一小段草稿")}});
        request.snapshot.internalWakes.append(
            QJsonObject{{QStringLiteral("ref"), QStringLiteral("wake/1")},
                        {QStringLiteral("version"), 1},
                        {QStringLiteral("summary"), QStringLiteral("下一次内部回顾")}});
        request.concerns.append(QJsonObject{
            {QStringLiteral("ref"), QStringLiteral("reflection/5")},
            {QStringLiteral("root_ref"), QStringLiteral("concern/5")},
            {QStringLiteral("version"), 2},
            {QStringLiteral("own_reflection"), QStringLiteral("想问问主人的睡眠")},
            {QStringLiteral("reported_at"), iso(at)}});
        request.privateEvidence.append(QJsonObject{
            {QStringLiteral("evidence_ref"), QStringLiteral("chat/9")},
            {QStringLiteral("own"), true},
            {QStringLiteral("speaker_role"), QStringLiteral("user")},
            {QStringLiteral("reported_at"), iso(at)},
            {QStringLiteral("text"), QStringLiteral("私聊里的一句话")}});
    };

    {
        CognitiveContextRequest request = baseRequest();
        fill(request);
        CognitiveContextBuilder builder(nullptr);
        ShadowComparison comparison;
        const CognitiveContext owner = builder.buildShadow(request, &comparison);
        QVERIFY(owner.scene.ownerPrivate);
        QVERIFY(owner.findByKey(QStringLiteral("concern|concern/5@2")) != nullptr);
        QVERIFY(owner.findByKey(QStringLiteral("reminder|reminder/2")) != nullptr);
        QVERIFY(owner.findByKey(QStringLiteral("activity|activity/1@2#activity")) != nullptr);
        QCOMPARE(comparison.privacyLeaks, 0);
    }
    {
        CognitiveContextRequest request = baseRequest();
        request.meta.channel = Channel::kQQGroup;
        request.meta.sessionId = QStringLiteral("30001");
        request.meta.actorId = QStringLiteral("10001");
        fill(request);
        CognitiveContextBuilder builder(nullptr);
        ShadowComparison comparison;
        const CognitiveContext group = builder.buildShadow(request, &comparison);
        QVERIFY(!group.scene.ownerPrivate);
        QVERIFY(comparison.privacyFiltered > 0);
        QCOMPARE(comparison.privacyLeaks, 0);
        QVERIFY(!group.deniedDomains.isEmpty());
        // owner 私有域与私聊证据一律不进入群聊投影。
        QVERIFY(group.findByKey(QStringLiteral("concern|concern/5@2")) == nullptr);
        QVERIFY(group.findByKey(QStringLiteral("reminder|reminder/2")) == nullptr);
        QVERIFY(group.findByKey(QStringLiteral("activity|activity/1@2#activity")) == nullptr);
        QVERIFY(group.findByKey(QStringLiteral("private_evidence|chat/9")) == nullptr);
        QVERIFY(group.findByKey(QStringLiteral("chat|chat/9")) == nullptr);
        for (const auto &evidence : group.evidence) {
            QVERIFY(evidence.privacy != QStringLiteral("private"));
        }
    }
}

void CognitiveContextTest::memoryDataClaimsReuseAccessPolicy()
{
    PromptAssembler::MemoryContext material;
    MemoryClaim claim;
    claim.id = 3;
    claim.statement = QStringLiteral("群里说周末去爬山");
    claim.claimStatus = QStringLiteral("active");
    claim.privacyLevel = 1;
    claim.sourceChannel = QStringLiteral("qq_group");
    claim.sourceSessionId = QStringLiteral("g1");
    claim.aboutPersonId = QStringLiteral("owner");
    claim.privacyOwnerPersonId = QStringLiteral("owner");
    claim.assertedByPersonId = QStringLiteral("owner");
    material.claims.append(claim);

    auto request = baseRequest();
    request.hasMemoryContext = true;
    request.memoryContext = material;
    request.access.viewerPersonId = QStringLiteral("owner");
    request.access.viewerIsOwner = true;
    request.access.channel = QStringLiteral("pet");
    request.access.allowedGroupIds = QStringList{QStringLiteral("g1")};
    {
        CognitiveContextBuilder builder(nullptr);
        const CognitiveContext context = builder.buildShadow(request);
        QVERIFY(context.findByKey(QStringLiteral("claim|claim/3")) != nullptr);
        QCOMPARE(context.coverage.sourceStates.value(QStringLiteral("memory_data")),
                 QStringLiteral("complete"));
    }
    // 同一份 claim 在无授权访问上下文里不得进入统一目录（复用 MemoryAccessPolicy 结果）。
    request.access.allowedGroupIds.clear();
    {
        CognitiveContextBuilder builder(nullptr);
        const CognitiveContext context = builder.buildShadow(request);
        QVERIFY(context.findByKey(QStringLiteral("claim|claim/3")) == nullptr);
    }
}

void CognitiveContextTest::coverageDistinguishesEmptyDeniedNotQueried()
{
    CognitiveContextRequest request = baseRequest();
    request.canReadUnderstanding = false;
    CognitiveContextBuilder builder(nullptr);
    const CognitiveContext context = builder.buildShadow(request);
    const auto &states = context.coverage.sourceStates;
    QCOMPARE(states.value(QStringLiteral("history")), QStringLiteral("empty"));
    QCOMPARE(states.value(QStringLiteral("current_understanding")), QStringLiteral("denied"));
    QCOMPARE(states.value(QStringLiteral("retrieval")), QStringLiteral("not_queried"));
    QCOMPARE(states.value(QStringLiteral("memory_tool")), QStringLiteral("not_queried"));
    QCOMPARE(states.value(QStringLiteral("execution_receipt")), QStringLiteral("not_queried"));
    QCOMPARE(states.value(QStringLiteral("snapshot")), QStringLiteral("not_queried"));
    QCOMPARE(context.coverage.overall, QStringLiteral("partial"));
    QVERIFY(context.coverage.emptySources.contains(QStringLiteral("history")));
    QVERIFY(context.coverage.deniedSources.contains(QStringLiteral("current_understanding")));
    QVERIFY(context.coverage.notQueriedSources.contains(QStringLiteral("retrieval")));

    // 明确“没有读取”的入口不能伪装成 empty。
    CognitiveContextRequest skipped = baseRequest();
    skipped.missingSources = QStringList{QStringLiteral("recent")};
    CognitiveContextBuilder skippedBuilder(nullptr);
    const CognitiveContext skippedContext = skippedBuilder.buildShadow(skipped);
    QCOMPARE(skippedContext.coverage.sourceStates.value(QStringLiteral("recent_group_context")),
             QStringLiteral("not_queried"));

    // 读到了但确实为空时是 empty（不是 denied / error）。
    CognitiveContextRequest emptyUnderstanding = baseRequest();
    emptyUnderstanding.canReadUnderstanding = true;
    CognitiveContextBuilder emptyBuilder(nullptr);
    const CognitiveContext emptyContext = emptyBuilder.buildShadow(emptyUnderstanding);
    QCOMPARE(emptyContext.coverage.sourceStates.value(QStringLiteral("current_understanding")),
             QStringLiteral("empty"));
}

void CognitiveContextTest::budgetAccountsDuplication()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("在吗");
    request.meta.receivedAtUtc = at;
    request.legacyPromptBytes = 4321;
    request.legacyDataBytes = 1234;
    request.history.append(historyTurn(1, QStringLiteral("重复的一句话"), QString(), at));
    request.hasSnapshot = true;
    request.snapshot.builtAtUtc = at;
    // 与历史相同的原始事件：归一化后只存一份文本。
    request.snapshot.observations.append(fact(QStringLiteral("chat/1"),
                                              EvidenceAuthority::UserStatement,
                                              QStringLiteral("重复的一句话"),
                                              QStringLiteral("pet"), QStringLiteral("default"), at));
    request.legacyEvidence = QJsonObject{
        {QStringLiteral("chat/1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("historical_report")}}}};

    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);
    QVERIFY(context.budget.contextBytes > 0);
    QVERIFY(context.budget.contextChars > 0);
    QVERIFY(context.budget.estimatedTokens > 0);
    QVERIFY(context.budget.totalMaterialBytes > context.budget.uniqueEvidenceBytes);
    QVERIFY(context.budget.duplicateMaterialBytes > 0);
    QCOMPARE(context.budget.legacyPromptBytes, 4321);
    QCOMPARE(context.budget.legacyDataBytes, 1234);
    QCOMPARE(context.budget.snapshotBudgetBytes, CognitiveSnapshotBuilder::promptByteBudget());
    QVERIFY(context.budget.snapshotBudgetBytes >= 2048);
    QVERIFY(context.budget.snapshotBudgetBytes <= 8192);
    QCOMPARE(context.budget.evidenceCount, context.evidenceCount());
    QVERIFY(context.budget.uniqueEventCount <= context.budget.evidenceCount);
    QCOMPARE(context.budget.duplicateMaterialBytes,
             context.budget.totalMaterialBytes - context.budget.uniqueEvidenceBytes);
}

void CognitiveContextTest::legacyComparisonHonoursStableRefs()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("在吗");
    request.meta.receivedAtUtc = at;
    request.history.append(historyTurn(1, QStringLiteral("在吗"), QString(), at));
    request.canReadUnderstanding = true;
    request.understanding.append(QJsonObject{
        {QStringLiteral("topic"), QStringLiteral("作息")},
        {QStringLiteral("quote"), QStringLiteral("早睡")},
        {QStringLiteral("understanding"), QStringLiteral("想调整作息")},
        {QStringLiteral("reported_at"), iso(at)},
        {QStringLiteral("revision"), 2},
        {QStringLiteral("evidence_ref"), QStringLiteral("source/5")}});
    request.understandingMatters = QStringList{QStringLiteral("作息")};
    request.legacyEvidence = QJsonObject{
        {QStringLiteral("chat/1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("historical_report")}}},
        {QStringLiteral("reply/1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("prior_assistant_expression")}}},
        {QStringLiteral("revision/作息/2"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("current_understanding")}}},
        {QStringLiteral("source/5"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("current_understanding")}}},
        {QStringLiteral("current_input"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("current_utterance")}}},
        {QStringLiteral("ref:1"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("index")}}},
        {QStringLiteral("chunk/77"), QJsonObject{{QStringLiteral("scope"), QStringLiteral("retrieval")}}}};

    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);
    QCOMPARE(comparison.missingInUnified, 0);
    QVERIFY(comparison.matchedRefs >= 5);
    QVERIFY(context.findByKey(QStringLiteral("matter|matter/作息@2")) != nullptr);
    QCOMPARE(comparison.rawEvidenceTotal, 4); // chat/1、reply/1、source/5、current_input
    QCOMPARE(comparison.rawEvidenceCovered, 4);
    QCOMPARE(comparison.rawEvidenceCoverage, 1.0);
    const QString notes = comparison.notes.join(QLatin1Char('\n'));
    QVERIFY(notes.contains(QStringLiteral("run_scoped")));
}

void CognitiveContextTest::snapshotBuilderMaterialsShareIdentity()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MemoryManager db(dir.filePath(QStringLiteral("shadow-shared.db")));
    RequestMeta meta = ownerMeta();
    QDateTime at = QDateTime::currentDateTime();
    meta.receivedAtUtc = at.toUTC();
    meta.occurredAtUtc = at.toUTC();
    meta.eventId = QStringLiteral("shared-1");
    meta.runId = QStringLiteral("shared-run-1");
    const qlonglong turn = db.recordInput(QStringLiteral("周末一起去看展"), meta, true);
    QVERIFY(turn > 0);
    SentenceText sentence;
    sentence.zhText = QStringLiteral("好呀");
    sentence.jaText = QStringLiteral("いいよ");
    QVERIFY(db.saveQATurn(QStringLiteral("周末一起去看展"),
                          QStringLiteral("{\"sentences\":[{\"zh_text\":\"好呀\"}]}"),
                          {sentence}, meta, turn));

    CognitiveSnapshotBuilder snapshotBuilder(&db);
    const CognitiveSnapshot snapshot = snapshotBuilder.build(meta, meta.runId,
                                                             meta.eventId,
                                                             QStringLiteral("foreground_reply"));
    QVERIFY(!snapshot.observations.isEmpty());

    // 入口 A：前台（快照 + 短期历史 + 当前输入）。
    CognitiveContextRequest foreground = baseRequest();
    foreground.meta = meta;
    foreground.currentInput = QStringLiteral("周末一起去看展");
    foreground.meta.receivedAtUtc = meta.receivedAtUtc;
    foreground.hasSnapshot = true;
    foreground.snapshot = snapshot;
    foreground.snapshotStats = snapshotBuilder.stats();
    foreground.history.append(historyTurn(turn, QStringLiteral("周末一起去看展"),
                                          QStringLiteral("{\"sentences\":[{\"zh_text\":\"好呀\"}]}"), at));
    foreground.legacyEvidence = QJsonObject{
        {QStringLiteral("chat/%1").arg(turn), QJsonObject{{QStringLiteral("scope"), QStringLiteral("historical_report")}}}};

    // 入口 B：私下回顾（同一快照 + 回顾 thread 的来源发言/历史）。
    CognitiveContextRequest review = baseRequest(QStringLiteral("private_review"));
    review.meta = meta;
    review.hasSnapshot = true;
    review.snapshot = snapshot;
    review.snapshotStats = snapshotBuilder.stats();
    review.reviewThread = QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("reflection")},
        {QStringLiteral("owner_statement"), QStringLiteral("周末一起去看展")},
        {QStringLiteral("reported_at"), iso(meta.occurredAtUtc)},
        {QStringLiteral("history"), QJsonArray{QJsonObject{
             {QStringLiteral("turn"), turn},
             {QStringLiteral("text"), QStringLiteral("周末一起去看展")},
             {QStringLiteral("reported_at"), iso(meta.occurredAtUtc)}}}}};
    review.reviewSourceTurn = turn;

    CognitiveContextBuilder builder(&db);
    ShadowComparison foregroundComparison;
    const CognitiveContext foregroundContext = builder.buildShadow(foreground, &foregroundComparison);
    ShadowComparison reviewComparison;
    const CognitiveContext reviewContext = builder.buildShadow(review, &reviewComparison);

    // 两条入口对同一 chat 轮次得到同一个证据身份（不同投影，但身份共享）。
    const QString chatKey = QStringLiteral("chat|chat/%1#user_statement").arg(turn);
    const auto *foregroundChat = foregroundContext.findByKey(chatKey);
    const auto *reviewChat = reviewContext.findByKey(chatKey);
    QVERIFY2(foregroundChat != nullptr, "前台入口应包含来源 chat 轮次");
    QVERIFY2(reviewChat != nullptr, "回顾入口应包含同一来源 chat 轮次");
    QCOMPARE(foregroundChat->identity.displayRef(), reviewChat->identity.displayRef());
    QCOMPARE(foregroundChat->kind, reviewChat->kind);
    QVERIFY(foregroundComparison.rawEvidenceCovered >= 1);
    QCOMPARE(foregroundComparison.privacyLeaks, 0);
    QCOMPARE(reviewComparison.privacyLeaks, 0);
}

void CognitiveContextTest::toolItemsMapToStableRefs()
{
    CognitiveContextRequest request = baseRequest(QStringLiteral("memory_recall"));
    request.toolStatus = QStringLiteral("complete");
    MemoryAgentItem chatted;
    chatted.id = QStringLiteral("chat:1");
    chatted.historyTurnId = 1;
    chatted.speakerRole = QStringLiteral("user");
    chatted.text = QStringLiteral("工具命中的同一轮");
    chatted.time = nowUtc();
    request.toolItems.append(chatted);
    MemoryAgentItem raw;
    raw.id = QStringLiteral("recent:9");
    raw.rawEventId = 9;
    raw.speakerRole = QStringLiteral("other");
    raw.text = QStringLiteral("群消息");
    raw.time = nowUtc();
    request.toolItems.append(raw);
    CognitiveContextBuilder builder(nullptr);
    const CognitiveContext context = builder.buildShadow(request);
    QVERIFY(context.findByKey(QStringLiteral("chat|chat/1#user_statement")) != nullptr);
    QVERIFY(context.findByKey(QStringLiteral("raw|raw/9")) != nullptr);
    QCOMPARE(context.coverage.sourceStates.value(QStringLiteral("memory_tool")),
             QStringLiteral("complete"));
}

void CognitiveContextTest::projectionDedupesAndKeepsRefs()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("周末一起去看展");
    request.meta.receivedAtUtc = at;
    request.history.append(historyTurn(1, QStringLiteral("周末一起去看展"),
                                       QStringLiteral("好呀"), at));
    request.hasSnapshot = true;
    request.snapshot.builtAtUtc = at;
    request.snapshot.observations.append(fact(QStringLiteral("chat/1"),
                                              EvidenceAuthority::UserStatement,
                                              QStringLiteral("周末一起去看展"),
                                              QStringLiteral("pet"), QStringLiteral("default"), at));
    request.snapshot.selfExpressions.append(fact(QStringLiteral("chat/1"),
                                                 EvidenceAuthority::AssistantExpression,
                                                 QStringLiteral("好呀"), QStringLiteral("pet"),
                                                 QStringLiteral("default"), at));
    MemoryEvidence retrieved;
    retrieved.sourceRef = QStringLiteral("chat/1:user");
    retrieved.sourceType = QStringLiteral("chat");
    retrieved.derived = QStringLiteral("original");
    retrieved.speakerRole = QStringLiteral("user");
    retrieved.text = QStringLiteral("周末一起去看展");
    retrieved.eventTime = at;
    request.retrieved.append(retrieved);
    request.retrievalDone = true;
    request.retrievalStatus = QStringLiteral("complete");
    CognitiveContextBuilder builder(nullptr);
    const CognitiveContext context = builder.buildShadow(request);

    CognitiveProjectionOptions options;
    options.entry = QStringLiteral("foreground_reply");
    options.ownerPrivate = context.scene.ownerPrivate;
    options.alreadySerialized.insert(
        evidenceIdentityForRef(QStringLiteral("chat/1"), QString(), 0,
                               QStringLiteral("user_statement")).stableKey());
    options.alreadySerialized.insert(
        evidenceIdentityForRef(QStringLiteral("reply/1")).stableKey());
    options.alreadySerialized.insert(
        evidenceIdentityForRef(QStringLiteral("current_input")).stableKey());
    const CognitiveProjection projection = projectModelInput(context, options);
    // 历史已完整序列化的 user/assistant 面在快照与检索块里只给 ref，不重复正文。
    QVERIFY(projection.stats.duplicateEntriesOmitted >= 2);
    QVERIFY(projection.stats.effectiveEvidenceCount >= 1
            || projection.stats.duplicateEntriesOmitted >= 2);
    const QString snapshotJson = QString::fromUtf8(
        QJsonDocument(projection.snapshotJson).toJson(QJsonDocument::Compact));
    QVERIFY(snapshotJson.contains(QStringLiteral("chat/1")));
    bool sawRefOnly = false;
    for (const auto &value : projection.snapshotJson.value(QStringLiteral("observations")).toArray()) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("ref")).toString() == QStringLiteral("chat/1")
            && row.value(QStringLiteral("dedup")).toBool()) {
            sawRefOnly = true;
            QVERIFY2(!row.contains(QStringLiteral("text")),
                     "去重项不得再携带完整正文");
        }
    }
    QVERIFY(sawRefOnly);
    bool sawDedupEvidence = false;
    for (const auto &value : projection.privateEvidenceItems) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("dedup")).toBool()) {
            sawDedupEvidence = true;
            QVERIFY(!row.contains(QStringLiteral("text")));
        }
    }
    QVERIFY(sawDedupEvidence);
    const QStringList refs = projectionRefs(projection);
    QVERIFY(refs.contains(QStringLiteral("chat/1")));
    QVERIFY(projection.unifiedTimeBlock.contains(QStringLiteral("now_local")));
    QVERIFY(projection.unifiedTimeBlock.contains(QStringLiteral("不是当前时间")));
    // 引用仍按既有 stable_ref 解析回目录（去重不丢来源）。
    bool resolvable = false;
    for (const auto &evidence : context.evidence) {
        if (evidence.identity.displayRef() == QStringLiteral("chat/1")) resolvable = true;
    }
    QVERIFY(resolvable);
}

void CognitiveContextTest::projectionKeepsFullForUnserialized()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("在吗");
    request.meta.receivedAtUtc = at;
    MemoryEvidence older;
    older.sourceRef = QStringLiteral("chat/40:user");
    older.sourceType = QStringLiteral("chat");
    older.derived = QStringLiteral("original");
    older.speakerRole = QStringLiteral("user");
    older.text = QStringLiteral("很久以前说过的一句话");
    older.eventTime = at.addDays(-30);
    older.handle = QStringLiteral("chunk/40");
    request.retrieved.append(older);
    request.retrievalDone = true;
    request.retrievalStatus = QStringLiteral("complete");
    CognitiveContextBuilder builder(nullptr);
    const CognitiveContext context = builder.buildShadow(request);
    CognitiveProjectionOptions options;
    options.entry = QStringLiteral("foreground_reply");
    options.ownerPrivate = true;
    options.alreadySerialized.insert(
        evidenceIdentityForRef(QStringLiteral("current_input")).stableKey());
    const CognitiveProjection projection = projectModelInput(context, options);
    bool sawFull = false;
    for (const auto &value : projection.privateEvidenceItems) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("ref")).toString() == QStringLiteral("chat/40")) {
            sawFull = true;
            QVERIFY(!row.value(QStringLiteral("dedup")).toBool());
            QVERIFY(row.value(QStringLiteral("text")).toString().contains(
                QStringLiteral("很久以前")));
        }
    }
    QVERIFY2(sawFull, "未被其它模块完整序列化的旧证据必须保留正文");
}

void CognitiveContextTest::projectionBudgetTrimsExplicitly()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("在吗");
    request.meta.receivedAtUtc = at;
    request.hasSnapshot = true;
    request.snapshot.builtAtUtc = at;
    for (int i = 0; i < 8; ++i) {
        request.snapshot.observations.append(fact(
            QStringLiteral("chat/%1").arg(100 + i), EvidenceAuthority::UserStatement,
            QString(160, QLatin1Char('x')), QStringLiteral("pet"), QStringLiteral("default"), at));
        request.snapshot.motivations.append(SelfMotivation{
            QStringLiteral("concern/%1").arg(200 + i), QStringLiteral("concern"),
            QString(160, QLatin1Char('y')), 1});
    }
    CognitiveContextBuilder builder(nullptr);
    const CognitiveContext context = builder.buildShadow(request);
    CognitiveProjectionOptions options;
    options.entry = QStringLiteral("foreground_reply");
    options.ownerPrivate = true;
    options.snapshotBudgetBytes = 900; // 故意压到旧快照 4096 以下，验证显式裁剪
    const CognitiveProjection projection = projectModelInput(context, options);
    QVERIFY(projection.stats.snapshotBudgetTrimmed);
    const int bytes = QJsonDocument(projection.snapshotJson)
                          .toJson(QJsonDocument::Compact).size();
    QVERIFY2(bytes <= options.snapshotBudgetBytes,
             qPrintable(QStringLiteral("snapshot projection bytes=%1").arg(bytes)));
    QVERIFY(projection.stats.snapshotBudgetBytes == 900);
}

void CognitiveContextTest::projectionGroupHasNoPrivateDomains()
{
    const QDateTime at = nowUtc();
    CognitiveContextRequest request = baseRequest();
    request.currentInput = QStringLiteral("大家好");
    request.meta.channel = Channel::kQQGroup;
    request.meta.sessionId = QStringLiteral("30001");
    request.meta.actorId = QStringLiteral("10001");
    request.meta.receivedAtUtc = at;
    request.hasSnapshot = true;
    request.snapshot.builtAtUtc = at;
    request.snapshot.motivations.append(SelfMotivation{
        QStringLiteral("concern/1"), QStringLiteral("concern"), QStringLiteral("私事"), 2});
    request.snapshot.commitments.append(DelegatedCommitment{
        QStringLiteral("reminder/2"), QStringLiteral("reminder"), QStringLiteral("pending"),
        at.addSecs(3600), QDateTime{}, QStringLiteral("私密提醒")});
    request.privateEvidence.append(QJsonObject{
        {QStringLiteral("evidence_ref"), QStringLiteral("chat/9")},
        {QStringLiteral("own"), true}, {QStringLiteral("reported_at"), iso(at)},
        {QStringLiteral("text"), QStringLiteral("私聊内容")}});
    CognitiveContextBuilder builder(nullptr);
    ShadowComparison comparison;
    const CognitiveContext context = builder.buildShadow(request, &comparison);
    QVERIFY(!context.scene.ownerPrivate);
    QVERIFY(comparison.privacyLeaks == 0);
    CognitiveProjectionOptions options;
    options.entry = QStringLiteral("foreground_reply");
    options.ownerPrivate = context.scene.ownerPrivate;
    const CognitiveProjection projection = projectModelInput(context, options);
    const QString json = QString::fromUtf8(
        QJsonDocument(projection.snapshotJson).toJson(QJsonDocument::Compact));
    QVERIFY(!json.contains(QStringLiteral("私事")));
    QVERIFY(!json.contains(QStringLiteral("私密提醒")));
    for (const auto &value : projection.privateEvidenceItems)
        QVERIFY(!value.toObject().value(QStringLiteral("text")).toString().contains(
            QStringLiteral("私聊内容")));
    QVERIFY(projection.unifiedTimeBlock.contains(QStringLiteral("owner_private=false")));
}

QTEST_MAIN(CognitiveContextTest)
#include "tst_cognitivecontext.moc"