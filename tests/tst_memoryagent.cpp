#include <QtTest>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "memoryagent.h"
#include "memorymanager.h"

namespace {

MemoryAgentScope ownerGroupScope(const QString &groupId, const QDateTime &now)
{
    MemoryAgentScope scope;
    scope.requestId = QStringLiteral("test-agent");
    scope.channel = Channel::kQQGroup;
    scope.sessionId = groupId;
    scope.viewerPersonId = QStringLiteral("owner");
    scope.viewerIsOwner = true;
    scope.allowedGroupIds = {groupId};
    scope.membershipVerified = true;
    scope.maxResults = 8;
    scope.timeoutMs = 500;
    scope.maxRounds = 2;

    scope.access.viewerPersonId = QStringLiteral("owner");
    scope.access.channel = Channel::kQQGroup;
    scope.access.sessionId = groupId;
    scope.access.allowedGroupIds = {groupId};
    scope.access.viewerIsOwner = true;
    scope.access.membershipVerified = true;

    scope.recentQueryTemplate.viewerIds = {QStringLiteral("20001")};
    scope.recentQueryTemplate.viewerPersonId = QStringLiteral("owner");
    scope.recentQueryTemplate.locationChannel = Channel::kQQGroup;
    scope.recentQueryTemplate.locationSessionId = groupId;
    scope.recentQueryTemplate.allowedGroupIds = {groupId};
    scope.recentQueryTemplate.since = now.addSecs(-3600);
    scope.recentQueryTemplate.perGroupCandidateLimit = 50;
    scope.recentQueryTemplate.maxGroups = 2;
    scope.recentQueryTemplate.itemLimit = 10;
    scope.recentQueryTemplate.itemLimitOverview = 50;
    scope.recentQueryTemplate.charBudget = 1200;
    scope.recentQueryTemplate.overviewCharBudget = 2200;
    scope.recentQueryTemplate.hotWindowItems = 10;
    scope.recentQueryTemplate.warmLineMaxChars = 24;
    return scope;
}

qlonglong makeInteraction(MemoryManager &mm,
                          const QString &groupId,
                          const QString &message,
                          const QString &senderId,
                          const QString &senderName)
{
    const QDateTime now = QDateTime::currentDateTime();
    if (!mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup, groupId,
                           QStringLiteral("message"), message,
                           QStringLiteral("ma-%1").arg(senderId),
                           QStringLiteral("in"), senderId, senderName,
                           QString(), now.addSecs(-30))) {
        return -1;
    }
    const QList<RawEvent> events = mm.getUnprocessedGroupEvents(groupId, 10);
    if (events.isEmpty()) {
        return -1;
    }
    InteractionDraft draft;
    draft.channel = Channel::kQQGroup;
    draft.sessionId = groupId;
    draft.startedAt = events.first().occurredAt;
    draft.endedAt = events.first().occurredAt;
    draft.sourceEventIds = {QString::number(events.first().id)};
    return mm.createInteractionBatch(draft, {events.first().id});
}

} // namespace

class TestMemoryAgent : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<MemoryAgentResult>("MemoryAgentResult");
        qRegisterMetaType<MemoryAgentAudit>("MemoryAgentAudit");
    }

    void toolDefinitionsDoNotExposeScope()
    {
        const QString text = QString::fromUtf8(
            QJsonDocument(MemoryAgentTools::toolDefinitions())
                .toJson(QJsonDocument::Compact));
        QVERIFY(text.contains(QStringLiteral("memory_search")));
        QVERIFY(text.contains(QStringLiteral("memory_read")));
        QVERIFY(text.contains(QStringLiteral("memory_related")));
        QVERIFY(!text.contains(QStringLiteral("viewer")));
        QVERIFY(!text.contains(QStringLiteral("allowed_groups")));
        QVERIFY(!text.contains(QStringLiteral("session_id")));
    }

    void searchReadAndRejectUnknownHandle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_search.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();

        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30001"), QStringLiteral("message"),
                                  QStringLiteral("我提议吃火锅"), QStringLiteral("a-1"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(),
                                  now.addSecs(-120)));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30001"), QStringLiteral("message"),
                                  QStringLiteral("火锅不错"), QStringLiteral("a-2"),
                                  QStringLiteral("in"), QStringLiteral("20001"),
                                  QStringLiteral("主人"), QString(),
                                  now.addSecs(-60)));
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("39999"), QStringLiteral("message"),
                                  QStringLiteral("另一个群的火锅"), QStringLiteral("a-3"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("小明"), QString(),
                                  now.addSecs(-30)));

        MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30001"), now);
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const QJsonObject result = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("火锅")},
                        {QStringLiteral("kinds"), QJsonArray{QStringLiteral("recent")}},
                        {QStringLiteral("limit"), 5}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        QVERIFY(!items.isEmpty());
        const QString id = items.first().toObject().value(QStringLiteral("id")).toString();
        QVERIFY(id.startsWith(QStringLiteral("recent:")));

        const QJsonObject forgedScope = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("火锅")},
                        {QStringLiteral("viewer"), QStringLiteral("owner")}},
            &error);
        QCOMPARE(forgedScope.value(QStringLiteral("error")).toString(),
                 QStringLiteral("invalid_arguments"));

        const QJsonObject otherGroup = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("另一个群")},
                        {QStringLiteral("kinds"), QJsonArray{QStringLiteral("recent")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(otherGroup.value(QStringLiteral("items")).toArray().isEmpty());

        const QJsonObject readResult = tools.execute(
            QStringLiteral("memory_read"),
            QJsonObject{{QStringLiteral("ids"), QJsonArray{id}}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(readResult.value(QStringLiteral("items")).toArray().size(), 1);

        const QJsonObject denied = tools.execute(
            QStringLiteral("memory_read"),
            QJsonObject{{QStringLiteral("ids"), QJsonArray{QStringLiteral("claim:99999")}}},
            &error);
        QCOMPARE(denied.value(QStringLiteral("error")).toString(),
                 QStringLiteral("denied"));
    }

    void claimsAreScopedAndRelatedStaysInSession()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_claims.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        const qlonglong interactionId = makeInteraction(
            mm, QStringLiteral("30002"),
            QStringLiteral("我喜欢吃火锅"), QStringLiteral("20001"),
            QStringLiteral("主人"));
        QVERIFY(interactionId > 0);

        ClaimDraft ownerClaim;
        ownerClaim.assertedByPersonId = QStringLiteral("owner");
        ownerClaim.aboutPersonId = QStringLiteral("owner");
        ownerClaim.privacyOwnerPersonId = QStringLiteral("owner");
        ownerClaim.statement = QStringLiteral("我喜欢吃火锅");
        ownerClaim.privacyLevel = 1;

        ClaimDraft thirdPartyClaim;
        thirdPartyClaim.assertedByPersonId = QStringLiteral("qq:20002");
        thirdPartyClaim.aboutPersonId = QStringLiteral("qq:20002");
        thirdPartyClaim.privacyOwnerPersonId = QStringLiteral("qq:20002");
        thirdPartyClaim.statement = QStringLiteral("小明喜欢火锅");
        thirdPartyClaim.privacyLevel = 1;

        QVERIFY(mm.saveInteractionSummaryWithClaims(
            interactionId, QStringLiteral("关于火锅的交流"),
            QStringLiteral("legacy"), QString(),
            {ownerClaim, thirdPartyClaim}, QStringLiteral("ready")));

        MemoryAgentScope ownerScope = ownerGroupScope(QStringLiteral("30002"), now);
        MemoryAgentTools tools(&mm, ownerScope);
        QString error;
        const QJsonObject result = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("火锅")},
                        {QStringLiteral("kinds"),
                         QJsonArray{QStringLiteral("summary"), QStringLiteral("claim")}},
                        {QStringLiteral("limit"), 8}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        QVERIFY(!items.isEmpty());
        bool sawOwnerClaim = false;
        bool sawThirdParty = false;
        QString summaryId;
        for (const QJsonValue &value : items) {
            const QJsonObject item = value.toObject();
            const QString text = item.value(QStringLiteral("text")).toString();
            if (item.value(QStringLiteral("kind")).toString()
                == QStringLiteral("claim")) {
                if (text.contains(QStringLiteral("我喜欢吃火锅"))) {
                    sawOwnerClaim = true;
                }
                if (text.contains(QStringLiteral("小明喜欢火锅"))) {
                    sawThirdParty = true;
                }
            }
            if (item.value(QStringLiteral("kind")).toString()
                == QStringLiteral("summary")) {
                summaryId = item.value(QStringLiteral("id")).toString();
            }
        }
        QVERIFY(sawOwnerClaim);
        QVERIFY(!sawThirdParty);
        QVERIFY(!summaryId.isEmpty());

        const QJsonObject related = tools.execute(
            QStringLiteral("memory_related"),
            QJsonObject{{QStringLiteral("id"), summaryId},
                        {QStringLiteral("limit"), 8}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (const QJsonValue &value :
             related.value(QStringLiteral("items")).toArray()) {
            QVERIFY(!value.toObject().value(QStringLiteral("text")).toString()
                         .contains(QStringLiteral("小明喜欢火锅")));
        }

        // 非 Owner 私聊：不得注入群级 summary；只能看到 viewer 自己的 claim。
        MemoryAgentScope viewerScope = ownerGroupScope(QStringLiteral("30002"), now);
        viewerScope.channel = Channel::kQQPrivate;
        viewerScope.sessionId = QStringLiteral("20002");
        viewerScope.viewerPersonId = QStringLiteral("qq:20002");
        viewerScope.viewerIsOwner = false;
        viewerScope.access.channel = Channel::kQQPrivate;
        viewerScope.access.sessionId = QStringLiteral("20002");
        viewerScope.access.viewerPersonId = QStringLiteral("qq:20002");
        viewerScope.access.viewerIsOwner = false;
        viewerScope.access.membershipVerified = true;
        MemoryAgentTools viewerTools(&mm, viewerScope);
        const QJsonObject viewerSummary = viewerTools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("火锅")},
                        {QStringLiteral("kinds"), QJsonArray{QStringLiteral("summary")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(viewerSummary.value(QStringLiteral("items")).toArray().isEmpty());

        const QJsonObject viewerClaims = viewerTools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("火锅")},
                        {QStringLiteral("kinds"), QJsonArray{QStringLiteral("claim")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray viewerItems =
            viewerClaims.value(QStringLiteral("items")).toArray();
        bool sawViewerClaim = false;
        for (const QJsonValue &value : viewerItems) {
            const QString text = value.toObject().value(QStringLiteral("text")).toString();
            QVERIFY(!text.contains(QStringLiteral("我喜欢吃火锅")));
            if (text.contains(QStringLiteral("小明喜欢火锅"))) {
                sawViewerClaim = true;
            }
        }
        QVERIFY(sawViewerClaim);
    }

    void plannerOnlyRunsForQQMemoryQuestions()
    {
        MemoryAgentConfig config;
        RequestMeta group;
        group.channel = Channel::kQQGroup;
        group.sessionId = QStringLiteral("30003");
        group.turnKind = QStringLiteral("reply");

        QVERIFY(MemoryAgentPlanner::shouldRun(
            group, QStringLiteral("你还记得我们之前聊过什么吗"), config));
        QVERIFY(!MemoryAgentPlanner::shouldRun(
            group, QStringLiteral("今天天气不错"), config));

        RequestMeta pet;
        pet.channel = Channel::kPet;
        QVERIFY(!MemoryAgentPlanner::shouldRun(
            pet, QStringLiteral("你记得上次吗"), config));

        config.trigger = QStringLiteral("always");
        QVERIFY(MemoryAgentPlanner::shouldRun(group, QStringLiteral("随便聊聊"),
                                              config));
        config.enabled = false;
        QVERIFY(!MemoryAgentPlanner::shouldRun(group, QStringLiteral("你记得吗"),
                                               config));
    }

    void agentLoopFinishesAndTimeoutFallsBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_loop.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30004"), QStringLiteral("message"),
                                  QStringLiteral("火锅约定"), QStringLiteral("l-1"),
                                  QStringLiteral("in"), QStringLiteral("20001"),
                                  QStringLiteral("主人"), QString(),
                                  now.addSecs(-60)));

        MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30004"), now);
        MemoryAgent agent;
        agent.setMemoryManager(&mm);
        agent.setApiKey(QStringLiteral("test-key"));
        int calls = 0;
        agent.setPlannerTransport(
            [&calls](const QJsonArray &, MemoryAgent::PlannerCallback callback) {
                ++calls;
                const QJsonObject response = calls == 1
                    ? QJsonObject{{QStringLiteral("tool_calls"),
                                   QJsonArray{QJsonObject{
                                       {QStringLiteral("name"), QStringLiteral("memory_search")},
                                       {QStringLiteral("arguments"), QJsonObject{
                                            {QStringLiteral("query"), QStringLiteral("火锅")},
                                            {QStringLiteral("limit"), 5}}}}}}}
                    : QJsonObject{{QStringLiteral("done"), true}};
                QTimer::singleShot(0, [callback, response]() {
                    callback(true, response, QString());
                });
            });

        QSignalSpy finishedSpy(&agent, &MemoryAgent::finished);
        QSignalSpy failedSpy(&agent, &MemoryAgent::failed);
        agent.start(scope, QStringLiteral("我们之前说火锅什么"));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 3000);
        QCOMPARE(failedSpy.count(), 0);
        const MemoryAgentResult result =
            finishedSpy.first().first().value<MemoryAgentResult>();
        QVERIFY(!result.items.isEmpty());
        QVERIFY(agent.metrics().totalRuns >= 1);
        QVERIFY(agent.metrics().p95() >= 0);
        QVERIFY(!agent.auditTrail().isEmpty());
        bool sawSearchAudit = false;
        for (const MemoryAgentAudit &audit : agent.auditTrail()) {
            QVERIFY(!audit.argsHash.contains(QStringLiteral("火锅")));
            if (audit.tool == QStringLiteral("memory_search")) {
                sawSearchAudit = true;
                QVERIFY(audit.queryLength > 0);
                QVERIFY(audit.resultCount >= 1);
            }
        }
        QVERIFY(sawSearchAudit);

        MemoryAgentMetrics metrics;
        for (int i = 1; i <= 100; ++i) {
            metrics.recordRun(true, false, i);
        }
        QCOMPARE(metrics.p50(), 50);
        QCOMPARE(metrics.p95(), 95);

        MemoryAgent timeoutAgent;
        timeoutAgent.setMemoryManager(&mm);
        MemoryAgentScope timeoutScope = ownerGroupScope(QStringLiteral("30004"), now);
        timeoutScope.timeoutMs = 50;
        timeoutAgent.setPlannerTransport(
            [](const QJsonArray &, MemoryAgent::PlannerCallback) {
                // 故意不回调，触发 deadline。
            });
        QSignalSpy timeoutFailed(&timeoutAgent, &MemoryAgent::failed);
        timeoutAgent.start(timeoutScope, QStringLiteral("还记得什么"));
        QTRY_COMPARE_WITH_TIMEOUT(timeoutFailed.count(), 1, 2000);
        QVERIFY(timeoutAgent.metrics().timeoutRuns >= 1);
    }
};

QTEST_GUILESS_MAIN(TestMemoryAgent)
#include "tst_memoryagent.moc"
