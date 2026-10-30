#include <QtTest>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include "memoryagent.h"
#include "memoryexecutor.h"
#include "memorymanager.h"
#include "memoryrecalltypes.h"
#include "memorytimerange.h"
#include "memorytimeresolver.h"

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

qlonglong makeInteractionAt(MemoryManager &mm,
                            const QString &groupId,
                            const QDateTime &at,
                            const QString &message,
                            const QString &senderId,
                            const QString &senderName)
{
    if (!mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup, groupId,
                           QStringLiteral("message"), message,
                           QStringLiteral("mt-%1-%2").arg(senderId).arg(at.toMSecsSinceEpoch()),
                           QStringLiteral("in"), senderId, senderName,
                           QString(), at)) {
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

    void unsafeParticipantNamesCanBeRequeued()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_requeue.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30007"), QStringLiteral("message"),
                                  QStringLiteral("测试"), QStringLiteral("r-1"),
                                  QStringLiteral("in"), QStringLiteral("20002"),
                                  QStringLiteral("坏名字"), QString(),
                                  now.addSecs(-60)));
        const QList<RawEvent> events = mm.getUnprocessedGroupEvents(
            QStringLiteral("30007"), 10);
        QVERIFY(!events.isEmpty());
        InteractionDraft draft;
        draft.channel = Channel::kQQGroup;
        draft.sessionId = QStringLiteral("30007");
        draft.startedAt = events.first().occurredAt;
        draft.endedAt = events.first().occurredAt;
        draft.sourceEventIds = {QString::number(events.first().id)};
        draft.participants = {{QStringLiteral("20002"),
                               QStringLiteral("坏\n\x00名字")}};
        const qlonglong id = mm.createInteractionBatch(
            draft, {events.first().id});
        QVERIFY(id > 0);
        QVERIFY(mm.markInteractionSummaryFailed(id));
        QVERIFY(mm.markInteractionSummaryFailed(id));
        QVERIFY(mm.markInteractionSummaryFailed(id));

        const QList<qlonglong> ids =
            mm.getFailedInteractionsWithUnsafeParticipantNames(10);
        QVERIFY(ids.contains(id));
        QVERIFY(mm.resetInteractionSummaryForRetry(id));
        QVERIFY(mm.getPendingInteractionIds(10).contains(id));
    }

    void timeRangeParsesAfterMidnight()
    {
        const QDateTime now(QDate(2026, 10, 31), QTime(1, 3));
        const MemoryTimeRange afternoon = MemoryTimeRangePlanner::parse(
            QStringLiteral("今天下午群聊聊了什么"), now);
        QVERIFY(afternoon.valid);
        QCOMPARE(afternoon.from, QDateTime(QDate(2026, 10, 30), QTime(12, 0)));
        QCOMPARE(afternoon.to, QDateTime(QDate(2026, 10, 30), QTime(18, 0)));

        const MemoryTimeRange yesterdayMorning = MemoryTimeRangePlanner::parse(
            QStringLiteral("昨天上午聊了什么"), now);
        QVERIFY(yesterdayMorning.valid);
        QCOMPARE(yesterdayMorning.from,
                 QDateTime(QDate(2026, 10, 30), QTime(6, 0)));
        QCOMPARE(yesterdayMorning.to,
                 QDateTime(QDate(2026, 10, 30), QTime(12, 0)));

        const MemoryTimeRange dawn = MemoryTimeRangePlanner::parse(
            QStringLiteral("今天凌晨"), now);
        QVERIFY(dawn.valid);
        QCOMPARE(dawn.from, QDateTime(QDate(2026, 10, 31), QTime(0, 0)));
        QVERIFY(!MemoryTimeRangePlanner::parse(QStringLiteral("今天天气不错"), now).valid);

        const MemoryTimeRange lastNight = MemoryTimeRangePlanner::parse(
            QStringLiteral("刚刚修复了系统，昨晚吃了什么"), now);
        QVERIFY(lastNight.valid);
        QCOMPARE(lastNight.from, QDateTime(QDate(2026, 10, 30), QTime(18, 0)));
        QCOMPARE(lastNight.to, QDateTime(QDate(2026, 10, 31), QTime(0, 0)));

        const MemoryTimeRange justNow = MemoryTimeRangePlanner::parse(
            QStringLiteral("刚刚发生了什么"), now);
        QVERIFY(justNow.valid);
        QCOMPARE(justNow.from, now.addSecs(-30 * 60));
        QCOMPARE(justNow.to, now.addSecs(1));
    }

    void summarySearchUsesTimeRange()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_time_range.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        const QDateTime at(now.date().addDays(-1), QTime(14, 0));
        const qlonglong interactionId = makeInteractionAt(
            mm, QStringLiteral("30005"), at,
            QStringLiteral("冰鸟技能怎么选"), QStringLiteral("20001"),
            QStringLiteral("主人"));
        QVERIFY(interactionId > 0);
        QVERIFY(mm.saveInteractionSummary(interactionId,
                                          QStringLiteral("讨论了冰鸟技能选择"),
                                          QStringLiteral("ready")));

        MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30005"), now);
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const QJsonObject result = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("昨天下午聊了什么")},
                        {QStringLiteral("kinds"), QJsonArray{QStringLiteral("summary")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        QVERIFY(!items.isEmpty());
        QVERIFY(items.first().toObject().value(QStringLiteral("text")).toString()
                    .contains(QStringLiteral("冰鸟技能")));
    }

    void timeRangeRanksOlderMatchingSummary()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_summary_rank.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        const QDate date = now.date().addDays(-1);
        const QString groupId = QStringLiteral("30008");

        const qlonglong dinnerId = makeInteractionAt(
            mm, groupId, QDateTime(date, QTime(19, 19)),
            QStringLiteral("我晚上吃烤肉啦"), QStringLiteral("20001"),
            QStringLiteral("主人"));
        QVERIFY(dinnerId > 0);
        QVERIFY(mm.saveInteractionSummary(
            dinnerId, QStringLiteral("在线蹲个网名说晚上吃烤肉，希望有人陪自己一起吃"),
            QStringLiteral("ready")));

        for (int i = 0; i < 10; ++i) {
            const qlonglong id = makeInteractionAt(
                mm, groupId,
                QDateTime(date, QTime(22, i)),
                QStringLiteral("普通群聊内容 %1").arg(i),
                QStringLiteral("20002"), QStringLiteral("群友"));
            QVERIFY(id > 0);
            QVERIFY(mm.saveInteractionSummary(
                id, QStringLiteral("普通群聊内容 %1").arg(i),
                QStringLiteral("ready")));
        }

        MemoryAgentScope scope = ownerGroupScope(groupId, now);
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const QJsonObject result = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"),
                         QStringLiteral("昨天晚上吃了什么")},
                        {QStringLiteral("kinds"),
                         QJsonArray{QStringLiteral("summary")}},
                        {QStringLiteral("limit"), 8}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        QVERIFY(!items.isEmpty());
        QVERIFY2(items.first().toObject().value(QStringLiteral("text")).toString()
                     .contains(QStringLiteral("烤肉")),
                 "19:19 dinner summary must outrank newer non-matching summaries");
    }

    void agentTimeArgsOverrideAmbiguousNow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_time_args.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        const QDate date = now.date().addDays(-1);
        const QString groupId = QStringLiteral("30009");
        const qlonglong dinnerId = makeInteractionAt(
            mm, groupId, QDateTime(date, QTime(19, 19)),
            QStringLiteral("我晚上吃烤肉啦"), QStringLiteral("20001"),
            QStringLiteral("主人"));
        QVERIFY(dinnerId > 0);
        QVERIFY(mm.saveInteractionSummary(
            dinnerId, QStringLiteral("在线蹲个网名说晚上吃烤肉"),
            QStringLiteral("ready")));

        MemoryAgentScope scope = ownerGroupScope(groupId, now);
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const QJsonObject result = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{
                {QStringLiteral("query"),
                 QStringLiteral("刚刚修复了系统，昨晚吃了什么")},
                {QStringLiteral("kinds"), QJsonArray{QStringLiteral("summary")}},
                {QStringLiteral("time_from"),
                 QDateTime(date, QTime(18, 0)).toString(Qt::ISODate)},
                {QStringLiteral("time_to"),
                 QDateTime(date.addDays(1), QTime(0, 0)).toString(Qt::ISODate)}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonArray items = result.value(QStringLiteral("items")).toArray();
        QVERIFY(!items.isEmpty());
        QVERIFY(items.first().toObject().value(QStringLiteral("text")).toString()
                    .contains(QStringLiteral("烤肉")));

        const QJsonObject missingTo = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{{QStringLiteral("query"), QStringLiteral("昨晚")},
                        {QStringLiteral("time_from"),
                         QDateTime(date, QTime(18, 0)).toString(Qt::ISODate)}},
            &error);
        QCOMPARE(missingTo.value(QStringLiteral("error")).toString(),
                 QStringLiteral("invalid_time_range"));

        const QJsonObject reversed = tools.execute(
            QStringLiteral("memory_search"),
            QJsonObject{
                {QStringLiteral("query"), QStringLiteral("昨晚")},
                {QStringLiteral("time_from"),
                 QDateTime(date, QTime(20, 0)).toString(Qt::ISODate)},
                {QStringLiteral("time_to"),
                 QDateTime(date, QTime(18, 0)).toString(Qt::ISODate)}},
            &error);
        QCOMPARE(reversed.value(QStringLiteral("error")).toString(),
                 QStringLiteral("invalid_time_range"));
    }

    void planValidationRejectsScopeAndIsoTime()
    {
        const QJsonObject validNeed{
            {QStringLiteral("id"), QStringLiteral("need-1")},
            {QStringLiteral("query"), QStringLiteral("昨晚聊了什么")},
            {QStringLiteral("kinds"), QJsonArray{QStringLiteral("summary")}},
            {QStringLiteral("source_hint"), QStringLiteral("current_group")},
            {QStringLiteral("time"),
             QJsonObject{{QStringLiteral("expression"), QStringLiteral("昨晚")},
                         {QStringLiteral("target"), QStringLiteral("message_time")}}}};
        MemoryRecallPlan plan;
        QString error;
        QVERIFY2(MemoryRecallPlan::fromJson(
                     QJsonObject{{QStringLiteral("needs"),
                                  QJsonArray{validNeed}}},
                     &plan, &error),
                 qPrintable(error));
        QCOMPARE(plan.needs.size(), 1);
        QVERIFY(plan.needs.first().hasTime);
        QCOMPARE(plan.needs.first().timeTarget, MemoryTimeTarget::MessageTime);

        const QJsonObject forgedTime{
            {QStringLiteral("id"), QStringLiteral("need-1")},
            {QStringLiteral("query"), QStringLiteral("x")},
            {QStringLiteral("time"),
             QJsonObject{{QStringLiteral("expression"), QStringLiteral("昨晚")},
                         {QStringLiteral("from"), QStringLiteral("2026-10-30T18:00:00+08:00")}}}};
        QVERIFY(!MemoryRecallPlan::fromJson(
            QJsonObject{{QStringLiteral("needs"), QJsonArray{forgedTime}}},
            &plan, &error));

        const QJsonObject forgedScope{
            {QStringLiteral("id"), QStringLiteral("need-1")},
            {QStringLiteral("query"), QStringLiteral("x")},
            {QStringLiteral("group_id"), QStringLiteral("1094382543")}};
        QVERIFY(!MemoryRecallPlan::fromJson(
            QJsonObject{{QStringLiteral("needs"), QJsonArray{forgedScope}}},
            &plan, &error));

        QJsonArray tooMany;
        for (int i = 0; i < 5; ++i) {
            tooMany.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("need-%1").arg(i)},
                {QStringLiteral("query"), QStringLiteral("x")}});
        }
        QVERIFY(!MemoryRecallPlan::fromJson(
            QJsonObject{{QStringLiteral("needs"), tooMany}}, &plan, &error));
    }

    void resolverParsesCompositeAndUnresolved()
    {
        const QDateTime anchor(QDate(2026, 10, 31), QTime(2, 14));
        const QString zone = QStringLiteral("Asia/Shanghai");

        const MemoryResolvedTime lastNight = MemoryTimeResolver::resolve(
            QStringLiteral("昨晚"), MemoryTimeTarget::MessageTime, anchor, zone);
        QVERIFY(lastNight.valid);
        QCOMPARE(lastNight.from, QDateTime(QDate(2026, 10, 30), QTime(18, 0)));
        QCOMPARE(lastNight.to, QDateTime(QDate(2026, 10, 31), QTime(0, 0)));

        const MemoryResolvedTime lastWednesday = MemoryTimeResolver::resolve(
            QStringLiteral("上周三晚上"), MemoryTimeTarget::MessageTime, anchor, zone);
        QVERIFY(lastWednesday.valid);
        QCOMPARE(lastWednesday.from, QDateTime(QDate(2026, 10, 21), QTime(18, 0)));
        QCOMPARE(lastWednesday.to, QDateTime(QDate(2026, 10, 22), QTime(0, 0)));

        const MemoryResolvedTime afternoon = MemoryTimeResolver::resolve(
            QStringLiteral("今天下午"), MemoryTimeTarget::MessageTime, anchor, zone);
        QVERIFY(afternoon.valid);
        QCOMPARE(afternoon.from, QDateTime(QDate(2026, 10, 30), QTime(12, 0)));
        QCOMPARE(afternoon.interpretation,
                 QStringLiteral("previous_completed_period"));

        const MemoryResolvedTime justNow = MemoryTimeResolver::resolve(
            QStringLiteral("刚刚"), MemoryTimeTarget::MessageTime, anchor, zone);
        QVERIFY(justNow.valid);
        QCOMPARE(justNow.from, anchor.addSecs(-30 * 60));

        const MemoryResolvedTime unresolved = MemoryTimeResolver::resolve(
            QStringLiteral("不存在的日期"), MemoryTimeTarget::MessageTime, anchor, zone);
        QVERIFY(!unresolved.valid);
        QCOMPARE(unresolved.interpretation, QStringLiteral("unresolved"));
    }

    void executorDeduplicatesNeedsAndKeepsPartial()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_multi_need.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        const QDate date = now.date().addDays(-1);
        const QString groupId = QStringLiteral("30010");
        const qlonglong dinnerId = makeInteractionAt(
            mm, groupId, QDateTime(date, QTime(19, 19)),
            QStringLiteral("晚上吃烤肉"), QStringLiteral("20001"),
            QStringLiteral("主人"));
        QVERIFY(dinnerId > 0);
        QVERIFY(mm.saveInteractionSummary(
            dinnerId, QStringLiteral("主人说晚上吃烤肉"), QStringLiteral("ready")));

        MemoryAgentScope scope = ownerGroupScope(groupId, now);
        scope.timezone = QStringLiteral("Asia/Shanghai");
        MemoryRecallNeed first;
        first.id = QStringLiteral("need-1");
        first.query = QStringLiteral("烤肉");
        first.kinds = {QStringLiteral("summary")};
        first.hasTime = true;
        first.timeExpression = QStringLiteral("昨晚");
        MemoryRecallNeed second = first;
        second.id = QStringLiteral("need-2");
        MemoryRecallNeed third = first;
        third.id = QStringLiteral("need-3");
        third.timeExpression = QStringLiteral("不存在的日期");
        MemoryRecallNeed eventTime = first;
        eventTime.id = QStringLiteral("need-4");
        eventTime.timeTarget = MemoryTimeTarget::EventTime;
        MemoryRecallPlan plan;
        plan.needs = {first, second, third, eventTime};

        MemoryExecutor executor(&mm, scope);
        const MemoryAgentResult result = executor.execute(plan);
        QCOMPARE(result.items.size(), 1);
        QCOMPARE(result.items.first().matchedNeedIds.size(), 3);
        QCOMPARE(result.status, QStringLiteral("partial"));
        QCOMPARE(result.needs.size(), 4);
        QCOMPARE(result.needs.at(0).status, QStringLiteral("success"));
        QCOMPARE(result.needs.at(1).status, QStringLiteral("success"));
        QCOMPARE(result.needs.at(2).status, QStringLiteral("unresolved"));
        QCOMPARE(result.needs.at(3).status, QStringLiteral("fallback"));
        QVERIFY(result.needs.at(3).interpretation.contains(
            QStringLiteral("event_time_message_time_fallback")));
        QVERIFY(result.items.first().id.startsWith(QStringLiteral("item:")));
    }

    void agentUsesNeedsPlanAndRecentContext()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_plan_mode.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        const QString groupId = QStringLiteral("30011");
        const qlonglong interactionId = makeInteractionAt(
            mm, groupId, now.addSecs(-60), QStringLiteral("火锅约定"),
            QStringLiteral("20001"), QStringLiteral("主人"));
        QVERIFY(interactionId > 0);
        QVERIFY(mm.saveInteractionSummary(
            interactionId, QStringLiteral("主人提到火锅约定"), QStringLiteral("ready")));

        MemoryAgentScope scope = ownerGroupScope(groupId, now);
        scope.timezone = QStringLiteral("Asia/Shanghai");
        PlannerTurn turn;
        turn.role = QStringLiteral("user");
        turn.timestamp = now.addSecs(-120);
        turn.text = QStringLiteral("之前说火锅什么");
        scope.recentTurns = {turn};

        MemoryAgent agent;
        agent.setMemoryManager(&mm);
        QJsonArray capturedMessages;
        agent.setPlannerTransport(
            [&capturedMessages](const QJsonArray &messages,
                                MemoryAgent::PlannerCallback callback) {
                capturedMessages = messages;
                const QJsonObject response{
                    {QStringLiteral("needs"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("id"), QStringLiteral("need-1")},
                         {QStringLiteral("query"), QStringLiteral("火锅")},
                         {QStringLiteral("kinds"),
                          QJsonArray{QStringLiteral("summary")}},
                         {QStringLiteral("source_hint"),
                          QStringLiteral("current_group")}}}}};
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
        QCOMPARE(result.needs.size(), 1);
        QCOMPARE(result.needs.first().status, QStringLiteral("success"));
        QString plannerInput;
        for (const QJsonValue &message : capturedMessages) {
            plannerInput += message.toObject()
                                .value(QStringLiteral("content"))
                                .toString();
        }
        QVERIFY(plannerInput.contains(QStringLiteral("recent_turns")));
        QVERIFY(plannerInput.contains(QStringLiteral("之前说火锅什么")));
        QVERIFY(plannerInput.contains(QStringLiteral("current_time")));
        QVERIFY(plannerInput.contains(QStringLiteral("memory_capabilities")));
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

    void timeoutKeepsPartialResults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("agent_partial.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.appendRawEvent(QStringLiteral("qq"), Channel::kQQGroup,
                                  QStringLiteral("30006"), QStringLiteral("message"),
                                  QStringLiteral("火锅约定"), QStringLiteral("p-1"),
                                  QStringLiteral("in"), QStringLiteral("20001"),
                                  QStringLiteral("主人"), QString(),
                                  now.addSecs(-60)));

        MemoryAgent agent;
        agent.setMemoryManager(&mm);
        MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30006"), now);
        scope.timeoutMs = 100;
        int calls = 0;
        agent.setPlannerTransport(
            [&calls](const QJsonArray &, MemoryAgent::PlannerCallback callback) {
                ++calls;
                if (calls == 1) {
                    const QJsonObject response{
                        {QStringLiteral("tool_calls"),
                         QJsonArray{QJsonObject{
                             {QStringLiteral("name"), QStringLiteral("memory_search")},
                             {QStringLiteral("arguments"), QJsonObject{
                                  {QStringLiteral("query"), QStringLiteral("火锅")},
                                  {QStringLiteral("limit"), 5}}}}}}};
                    QTimer::singleShot(0, [callback, response]() {
                        callback(true, response, QString());
                    });
                }
                // 第二轮不回调，触发 partial timeout。
            });

        QSignalSpy finishedSpy(&agent, &MemoryAgent::finished);
        QSignalSpy failedSpy(&agent, &MemoryAgent::failed);
        agent.start(scope, QStringLiteral("记得火锅吗"));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2000);
        QCOMPARE(failedSpy.count(), 0);
        const MemoryAgentResult result =
            finishedSpy.first().first().value<MemoryAgentResult>();
        QCOMPARE(result.status, QStringLiteral("timeout"));
        QVERIFY(!result.items.isEmpty());
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
