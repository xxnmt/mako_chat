#include <QtTest>
#include <QSqlQuery>
#include <QJsonDocument>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include "llmservice.h"
#include "cognitivecoordinator.h"

#include "memoryagent.h"
#include "memoryexecutor.h"
#include "memorymanager.h"
#include "memoryrecalltool.h"
#include "memoryrecalltypes.h"
#include "memorytimerange.h"
#include "memorytimeresolver.h"
#include "memorytimeframe.h"
#include "timeframeresolver.h"
#include "memoryoverview.h"

namespace {

void serveCompletions(QTcpServer &server,QList<QByteArray> &bodies,std::function<QString(int)> respond)
{
    QObject::connect(&server,&QTcpServer::newConnection,&server,[&server,&bodies,respond] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,&bodies,respond] {
            const auto bytes = socket->peek(socket->bytesAvailable());
            const auto header = bytes.indexOf("\r\n\r\n");
            if (header < 0)
                return;
            const auto length = QRegularExpression("content-length: (\\d+)",
                QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(bytes.left(header)));
            if (!length.hasMatch() || bytes.size()-header-4 < length.captured(1).toInt())
                return;
            socket->readAll();
            bodies.append(bytes.mid(header+4));
            const QString answer = respond(bodies.size());
            QJsonObject msg{{"message",QJsonObject{{"content",answer}}}};
            const auto body = QJsonDocument(QJsonObject{{"choices",QJsonArray{msg}}}).toJson();
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
}

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

MemoryAgentScope ownerPrivateScope(const QDateTime &now, bool crossChannel)
{
    MemoryAgentScope scope;
    scope.requestId = QStringLiteral("test-private");
    scope.channel = Channel::kQQPrivate;
    scope.sessionId = QStringLiteral("2014366825");
    scope.viewerPersonId = QStringLiteral("owner");
    scope.viewerIsOwner = true;
    scope.allowedGroupIds = {QStringLiteral("30001")};
    scope.membershipVerified = true;
    scope.maxResults = 8;
    scope.timeoutMs = 500;
    scope.maxRounds = 2;
    scope.ownerCrossChannelPrivate = crossChannel;
    scope.access.viewerPersonId = QStringLiteral("owner");
    scope.access.channel = Channel::kQQPrivate;
    scope.access.sessionId = scope.sessionId;
    scope.access.allowedGroupIds = scope.allowedGroupIds;
    scope.access.viewerIsOwner = true;
    scope.access.membershipVerified = true;
    scope.recentQueryTemplate.viewerIds = {QStringLiteral("20001")};
    scope.recentQueryTemplate.viewerPersonId = QStringLiteral("owner");
    scope.recentQueryTemplate.locationChannel = Channel::kQQPrivate;
    scope.recentQueryTemplate.locationSessionId = scope.sessionId;
    scope.recentQueryTemplate.allowedGroupIds = scope.allowedGroupIds;
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
    void conversationDecision_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"direct_pending", "index_ref", "consult", "related", "unrelated", "churn", "quiet", "invalid_ref"})
            QTest::newRow(name) << QString(name);
    }

    void conversationDecision()
    {
        QFETCH(QString, scenario);
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("decision.db"));
        RequestMeta meta;
        meta.runId = "foreground";
        int event = 0;
        const auto observe = [&](const QString &text, const QJsonObject &head, bool commit) {
            meta.eventId = QString::number(++event);
            if (!db.enqueueObservation(meta, text)) return false;
            if (!commit) return true;
            const QJsonObject change{{"matter",head["matter"].toString()},{"version",head["version"].toInteger()},
                {"topic","预约"},{"quote",text},{"status","tentative"},{"understanding",text},{"kind","self_report"}};
            return db.commitObservation(db.pendingObservation()["source"].toInteger(),
                {{"outcome","updated"},{"changes",QJsonArray{change}}});
        };
        QVERIFY(observe("初始时间", {}, true));
        if (scenario == "direct_pending") QVERIFY(observe("待整理", {}, false));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        bool updatesOk = true;
        serveCompletions(server, bodies, [&](int count) -> QString {
            if (count == 1) {
                if (scenario == "index_ref") return R"({"evidence_decision":{"used_evidence":["ref:1","ref:999"]},"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})";
                if (scenario == "quiet") return R"({"conversation_action":"defer","sentences":[]})";
                if (scenario == "invalid_ref") return R"({"action":"consult","refs":[100]})";
                if (scenario != "direct_pending") return R"({"action":"consult","refs":[1]})";
            }
            if (count == 2 && scenario == "unrelated")
                updatesOk &= observe("另一事项", {}, true);
            if ((count == 2 && scenario == "related") || (count >= 2 && scenario == "churn"))
                updatesOk &= observe(QString("更新%1").arg(count), db.matterHeads("user").first().toObject(), true);
            return QString(R"({"sentences":[{"zh_text":"答复%1","ja_text":"いいよ","tags":{"emotion":"happy"}}]})").arg(count);
        });
        LLMService service("test",nullptr,QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        QSignalSpy spoken(&service,&LLMService::sentencesReady);
        QSignalSpy quiet(&service,&LLMService::replyQuiet);
        QSignalSpy failed(&service,&LLMService::replyAbnormal);
        service.askDeepSeek("继续吗",{},meta);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size()+quiet.size()+failed.size(),1,5000);
        QVERIFY(updatesOk);
        if (scenario == "index_ref") {
            QCOMPARE(spoken.size(),1);
            QSqlQuery audit(QSqlDatabase::database("memory_db_connection"));
            QVERIFY(audit.exec("SELECT data FROM cognitive_audit WHERE kind='answer_decision'"));
            QVERIFY(audit.next());
            const auto decision = QJsonDocument::fromJson(audit.value(0).toByteArray()).object();
            QCOMPARE(decision["unknown_evidence_refs"].toArray(), QJsonArray{"ref:999"});
            QCOMPARE(decision["cited_evidence"].toObject()["ref:1"].toObject()["scope"].toString(),QString("index"));
            return;
        }
        if (scenario == "quiet") {
            QCOMPARE(quiet.size(),1);
            QCOMPARE(bodies.size(),1);
        } else if (scenario == "invalid_ref" || scenario == "churn") {
            QCOMPARE(failed.size(),1);
            QCOMPARE(failed.first().first().toString(), scenario == "churn" ?
                QString("referenced_matter_changed") : QString("unknown_matter_ref"));
        } else {
            QCOMPARE(spoken.size(),1);
            const int requests = scenario == "direct_pending" ? 1 : scenario == "related" ? 3 : 2;
            QCOMPARE(bodies.size(),requests);
            QVERIFY(spoken.first().at(1).toString().contains(QString("答复%1").arg(requests)));
            if (requests >= 2) QVERIFY(bodies.at(1).contains(QString("初始时间").toUtf8()));
            if (requests == 3) QVERIFY(bodies.last().contains(QString("更新2").toUtf8()));
        }
    }

    void understandingSnapshotAcrossReflection_data()
    {
        QTest::addColumn<bool>("updateDuringReflection");
        QTest::newRow("visible update before final prompt") << true;
        QTest::newRow("hidden update during final generation") << false;
    }

    void understandingSnapshotAcrossReflection()
    {
        QFETCH(bool, updateDuringReflection);
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("snapshot.db"));
        RequestMeta report;
        report.eventId = "private-report";
        QVERIFY(db.enqueueObservation(report, "预约延期"));
        const auto source = db.pendingObservation()["source"].toInteger();

        RequestMeta group;
        group.channel = Channel::kQQGroup;
        group.sessionId = "group";
        group.actorId = "owner";
        group.eventId = "group-message";
        group.runId = "snapshot";
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        bool committed = false;
        serveCompletions(server, bodies, [&](int count) -> QString {
            if (count == (updateDuringReflection ? 1 : 2)) {
                const QJsonObject change{{"matter",""},{"version",0},{"topic","预约"},{"quote","预约延期"},
                    {"status","tentative"},{"understanding","预约日期待定"},{"kind","self_report"}};
                committed = db.commitObservation(source, {{"outcome","updated"},{"changes",QJsonArray{change}}});
            }
            if (count == 1) return R"({"decision":"normal"})";
            return R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})";
        });
        LLMService service("test", nullptr, QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        QSignalSpy spoken(&service, &LLMService::sentencesReady);
        QSignalSpy quiet(&service, &LLMService::replyQuiet);
        service.askDeepSeek(updateDuringReflection ? "预约" : "最近如何", {}, group);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size() + quiet.size(), 1, 5000);
        QVERIFY(committed);
        QCOMPARE(bodies.size(), 2);
        QVERIFY(bodies.first().contains(QStringLiteral("预约延期").toUtf8()));
        QVERIFY(!bodies.last().contains(QStringLiteral("预约延期").toUtf8()));
        QCOMPARE(bodies.last().contains("CURRENT_UNDERSTANDING\\n"), updateDuringReflection);
        QCOMPARE(spoken.size(), 1);
    }

    void privateReflectionDoesNotEnterPublicReply_data()
    {
        QTest::addColumn<QString>("secret");
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("followup");
        QFile file(QFINDTESTDATA("fixtures/cognitive_replay.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto scenarios = QJsonDocument::fromJson(file.readAll()).array();
        QVERIFY(!scenarios.isEmpty());
        for (auto scenario : scenarios) {
            const auto row = scenario.toObject();
            QTest::newRow(qPrintable(row["name"].toString())) << row["report"].toString()
                << row["status"].toString() << row["followup"].toString();
        }
    }

    void privateReflectionDoesNotEnterPublicReply()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("reflection.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = "10101";
        QFETCH(QString, secret);
        QFETCH(QString, status);
        QFETCH(QString, followup);
        const QJsonObject change{{"matter",""},{"version",0},{"topic","近况"},{"status",status},
            {"kind","self_report"},{"quote",secret},{"understanding",secret}};
        const QString update = QString::fromUtf8(QJsonDocument(QJsonObject{{"outcome","updated"},
            {"changes",QJsonArray{change}}}).toJson(QJsonDocument::Compact));
        meta.eventId = "replay-report";
        meta.runId = "replay-private";
        meta.occurredAtUtc = QDateTime::currentDateTimeUtc();
        const bool needsHistory = status == "tentative";
        const QString earlier = QStringLiteral("上次我们谈到同一件事");
        if (needsHistory) {
            auto previous = meta;
            previous.eventId = "earlier-event";
            previous.occurredAtUtc = meta.occurredAtUtc.addSecs(-60);
            QVERIFY(db.enqueueObservation(previous,earlier));
            QVERIFY(db.saveQATurn(earlier,"{}",{},previous));
            QVERIFY(db.commitObservation(db.pendingObservation()["source"].toInteger(),
                QJsonObject{{"outcome","no_change"},{"changes",QJsonArray{}}}));
        }
        QVERIFY(db.enqueueObservation(meta, secret));
        QVERIFY(db.saveQATurn(secret, "{}", {}, meta));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server,bodies,[&](int count) {
                const int step = count-int(needsHistory);
                return step==0 ? QStringLiteral(R"({"outcome":"needs_evidence","changes":[]})")
                    : step==1 ? update : step==2 ? QStringLiteral(R"({"decision":"skip_old_concern"})")
                    : QStringLiteral(R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})");
        });
        LLMService service("test",nullptr,QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        service.processObservation();
        QTRY_COMPARE_WITH_TIMEOUT(db.matterHeads(meta.subject).size(),1,5000);
        if (needsHistory) {
            QCOMPARE(bodies.size(),2);
            QVERIFY(!bodies.first().contains(earlier.toUtf8()));
            QVERIFY(bodies.last().contains(earlier.toUtf8()));
        }
        QSignalSpy replies(&service,&LLMService::sentencesReady);
        meta.channel = Channel::kQQGroup;
        meta.sessionId = "20202";
        meta.runId = "group";
        service.askDeepSeek(followup,{},meta);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(),1,5000);
        QCOMPARE(bodies.size(),3+int(needsHistory));
        QCOMPARE(replies.at(0).at(2).toString(), followup);
        QCOMPARE(replies.at(0).at(3).value<RequestMeta>().runId, meta.runId);
        QCOMPARE(replies.at(0).at(3).value<RequestMeta>().sessionId, meta.sessionId);
        QVERIFY(bodies[bodies.size()-2].contains(secret.toUtf8()));
        QVERIFY(!bodies.last().contains(secret.toUtf8()));
        QVERIFY(bodies.last().contains(QString("别按以前的情况主动追问").toUtf8()));
        service.askDeepSeek("我刚才说了什么",{},RequestMeta{});
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(),2,5000);
        QCOMPARE(bodies.size(),4+int(needsHistory));
        QVERIFY(bodies.last().contains(secret.toUtf8()));
        if (needsHistory) {
            QVERIFY(db.deleteTurnByID(1));
            QVERIFY(db.matterHeads(meta.subject).isEmpty());
            QCOMPARE(db.pendingObservation()["input"].toString(),secret);
        }
    }

    void privatePromiseKeepsSpeakerAndCareScope()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("attribution.db"));
        RequestMeta seed;
        seed.channel = Channel::kQQPrivate;
        seed.sessionId = seed.actorId = QStringLiteral("2014366825");
        seed.runId = seed.eventId = QStringLiteral("seed");
        const QString assistantReply = QStringLiteral(
            R"({"sentences":[{"zh_text":"今晚能不能试着早一点点睡呀？","ja_text":"いいよ","tags":{"emotion":"caring"}}]})");
        QList<SentenceText> seedSentences;
        SentenceText seedSentence;
        seedSentence.zhText = QStringLiteral("今晚能不能试着早一点点睡呀？");
        seedSentence.jaText = QStringLiteral("いいよ");
        seedSentence.rawTags.insert(QStringLiteral("emotion"), QStringLiteral("caring"));
        seedSentences.append(seedSentence);
        QVERIFY(db.saveQATurn("我答应茉子，会努力调整作息", assistantReply, seedSentences, seed));
        QVERIFY(db.enqueueObservation(seed, "我答应茉子，会努力调整作息"));
        const auto source = db.pendingObservation()["source"].toInteger();
        const QJsonObject promise{{"matter",""},{"version",0},{"topic","作息"},
            {"quote","我答应茉子，会努力调整作息"},{"status","confirmed"},
            {"understanding","答应努力调整作息"},{"kind","self_report"}};
        QVERIFY(db.commitObservation(source,
            QJsonObject{{"outcome","updated"},{"changes",QJsonArray{promise}}}));

        RawEventWrite groupEvent;
        groupEvent.platform = QStringLiteral("qq");
        groupEvent.channel = Channel::kQQGroup;
        groupEvent.sessionId = QStringLiteral("999");
        groupEvent.senderId = QStringLiteral("999");
        groupEvent.senderName = QStringLiteral("群友");
        groupEvent.eventType = QStringLiteral("message");
        groupEvent.content = QStringLiteral("已经躺下");
        groupEvent.occurredAt = QDateTime::currentDateTime();
        QVERIFY(db.appendRawEvent(groupEvent));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [&](int) -> QString {
            if (bodies.last().contains("speaker_is_owner"))
                return QStringLiteral(
                    R"({"decision":"normal","care":[{"topic":"上床","user_response":"done","care_action":"acknowledge"}]})");
            return QStringLiteral(
                R"({"sentences":[{"zh_text":"你答应过要早点睡","ja_text":"いいよ","tags":{"emotion":"caring"}}],"evidence_decision":{"used_evidence":["reply/1"],"claims":[{"sentence_index":0,"refs":["reply/1"],"relation":"reported_fact"}],"evidence_status":"sufficient","basis":"test"}})");
        });
        LLMService service("test", nullptr,
            QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        RequestMeta meta = seed;
        meta.runId = meta.eventId = QStringLiteral("final");
        const auto history = db.getHistoryTurn(10, meta.channel, meta.sessionId);
        QSignalSpy spoken(&service, &LLMService::sentencesReady);
        QSignalSpy failed(&service, &LLMService::replyAbnormal);
        service.askDeepSeek("已经躺下了", history, meta);
        QTRY_VERIFY_WITH_TIMEOUT(spoken.size() + failed.size() > 0, 5000);
        if (spoken.isEmpty())
            qDebug() << "[test] attribution bodies" << bodies.size() << "failed" << failed.size()
                     << (failed.isEmpty() ? QString{} : failed.first().first().toString());
        QCOMPARE(spoken.size(), 1);

        const QString finalRequest = QString::fromUtf8(bodies.last());
        QVERIFY(finalRequest.contains(QStringLiteral("当前场景")));
        QVERIFY(finalRequest.contains(QStringLiteral("self_report")));
        QVERIFY(finalRequest.contains(QStringLiteral("用户已经回应")));
        QVERIFY(finalRequest.contains(QStringLiteral("不要推断其他事项已完成")));

        QSqlQuery audit(QSqlDatabase::database("memory_db_connection"));
        QVERIFY(audit.exec("SELECT data FROM cognitive_audit WHERE kind='answer_decision' ORDER BY id DESC LIMIT 1"));
        QVERIFY(audit.next());
        const auto decision = QJsonDocument::fromJson(audit.value(0).toByteArray()).object();
        const auto attribution = decision["attribution_check"].toObject();
        QCOMPARE(attribution["speaker_check"].toString(), QStringLiteral("assistant_expression"));
        QVERIFY(attribution["issues"].toArray().contains(
            QJsonValue(QStringLiteral("assistant_suggestion_as_user_promise"))));
    }

    void projectionUsesCheckedOwnReport()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("projection.db"));
        RequestMeta seed;
        seed.eventId = "private";
        const QString original = "我的陶艺课取消了，细节以后再讲";
        QVERIFY(db.recordInput(original,seed,true)>0);
        const QJsonObject change{{"matter",""},{"version",0},{"topic","兴趣课"},{"quote","陶艺课取消了"},
            {"status","cancelled"},{"understanding","课程已经取消"},{"kind","self_report"}};
        QVERIFY(db.commitObservation(db.pendingObservation()["source"].toInteger(),
            {{"outcome","updated"},{"changes",QJsonArray{change}}}));
        seed.eventId = "question";
        QVERIFY(db.recordInput("那个安排呢",seed,true)>0);
        auto inference = change;
        const auto first = db.internalMatters(seed,{}).first().toObject();
        inference["matter"] = first["matter"];
        inference["version"] = first["version"];
        inference["quote"] = "那个安排呢";
        inference["understanding"] = "可能在追问之前的安排";
        inference["kind"] = "inference";
        QVERIFY(db.commitObservation(db.pendingObservation()["source"].toInteger(),
            {{"outcome","updated"},{"changes",QJsonArray{inference}}}));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server,bodies,[](int count) -> QString {
            if (count==1 || count>4) return QStringLiteral("{}");
            if (count<4) return R"({"decision":"respond","intent":"self_recall","disclosure":"ordinary_self_report","certainty":"reported","refs":[1],"evidence_versions":{"1":1},"reference_status":"plausible","excerpts":{"1":"陶艺课取消了"}})";
            return R"({"sentences":[{"zh_text":"记得，你说课取消了。","ja_text":"そうだね","tags":{"emotion":"happy"}}]})";
        });
        LLMService model("test",nullptr,QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        RequestMeta group;
        group.channel = Channel::kQQGroup;
        group.sessionId = "group";
        group.actorId = "owner";
        group.runId = group.eventId = "followup";
        QSignalSpy spoken(&model,&LLMService::sentencesReady);
        model.askDeepSeek("还记得我那个安排后来怎么样了吗",{},group);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.count(),1,5000);
        QCOMPARE(bodies.size(),4);
        QVERIFY(bodies.at(1).contains("missing_decision"));
        QVERIFY(bodies.at(2).contains(original.toUtf8()));
        QVERIFY(bodies.last().contains("context/1"));
        QVERIFY(!bodies.last().contains(QStringLiteral("课程已经取消").toUtf8()));
        QVERIFY(bodies.last().contains(QStringLiteral("陶艺课取消了").toUtf8()));
        QVERIFY(bodies.last().contains("answer_and_confirm"));
        QVERIFY(!bodies.last().contains(original.toUtf8()));
        QSignalSpy interrupted(&model,&LLMService::replyAbnormal);
        group.runId = group.eventId = "invalid-repeat";
        model.askDeepSeek("再想想",{},group);
        QTRY_COMPARE_WITH_TIMEOUT(interrupted.size(),1,5000);
        QCOMPARE(interrupted.first().first().toString(),QString("expression_missing_decision"));
        QCOMPARE(bodies.size(),6);
        QCOMPARE(spoken.size(),1);
        group.subject = "qq:other";
        QVERIFY(db.internalMatters(group,{}).isEmpty());
    }

    void presentFrameMustIncludeRequestTime()
    {
        const auto now = QDateTime::fromString("2026-11-11T22:52:00+08:00",Qt::ISODate);
        MemoryTimeFrame frame;
        frame.anchor = MemoryTimeFrame::Anchor::RelativeDay;
        frame.period = "night";
        const auto invalid = TimeFrameResolver::resolve("现在",MemoryTimeTarget::MessageTime,frame,now,"Asia/Shanghai");
        QCOMPARE(invalid.error,QString("present_outside_requested_frame"));
        const auto dawn = TimeFrameResolver::resolve("今天凌晨",MemoryTimeTarget::MessageTime,frame,now,"Asia/Shanghai");
        QVERIFY(dawn.valid);
        frame.anchor = MemoryTimeFrame::Anchor::RecentWindow;
        frame.offset = 30;
        frame.period.clear();
        const auto recent = TimeFrameResolver::resolve("现在",MemoryTimeTarget::MessageTime,frame,now,"Asia/Shanghai");
        QVERIFY(recent.valid && recent.from <= now && recent.to > now);
    }

    void coordinatorCompletesBeforeAdvancing()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("coordinator.db"));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server,bodies,[](int count) -> QString {
            if (count==1) return R"({"conversation_action":"close","sentences":[]})";
            return R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})";
        });
        LLMService model("test",nullptr,QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        CognitiveCoordinator coordinator(db,model,nullptr);
        connect(&model,&LLMService::sentencesReady,this,[&](const QList<SentenceText> &s,const QString &raw,
            const QString &input,const RequestMeta &meta){ QVERIFY(coordinator.handedOff(input,raw,s,meta)); });
        RequestMeta meta;
        meta.longTermMemoryEnabled=false;
        meta.eventId=meta.runId="first";
        coordinator.submit("下次聊",meta);
        meta.eventId=meta.runId="second";
        coordinator.submit("还有一件事",meta);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.busy(),5000);
        const auto rows=db.getHistoryTurn(0,10);
        QCOMPARE(rows.size(),2);
        QCOMPARE(rows.first().state,QString("handed_off"));
        QCOMPARE(rows.last().state,QString("quiet"));
        QCOMPARE(bodies.size(),2);
    }

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

    void toolGateDoesNotReadUserText()
    {
        MemoryAgentConfig config;
        RequestMeta group;
        group.channel = Channel::kQQGroup;
        group.sessionId = QStringLiteral("30003");
        group.turnKind = QStringLiteral("reply");
        QVERIFY(MemoryAgentPlanner::canExposeTools(group, config));
        // tool 模式下旧关键词门控必须失效；是否调用由主模型决定。
        QVERIFY(!MemoryAgentPlanner::shouldRun(
            group, QStringLiteral("你还记得昨天吗"), config));

        RequestMeta pet;
        pet.channel = Channel::kPet;
        pet.turnKind = QStringLiteral("reply");
        QVERIFY(!MemoryAgentPlanner::canExposeTools(pet, config));

        RequestMeta proactive;
        proactive.channel = Channel::kQQPrivate;
        proactive.turnKind = QStringLiteral("proactive");
        QVERIFY(!MemoryAgentPlanner::canExposeTools(proactive, config));

        config.mode = QStringLiteral("legacy");
        QVERIFY(!MemoryAgentPlanner::canExposeTools(group, config));

        config.mode = QStringLiteral("tool");
        config.enabled = false;
        QVERIFY(!MemoryAgentPlanner::canExposeTools(group, config));
    }

    void recallToolRejectsScopeFieldsAndKeepsPerNeedStatus()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("recall_tool.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(makeInteraction(mm, QStringLiteral("30009"),
                                QStringLiteral("今晚吃火锅"), QStringLiteral("20001"),
                                QStringLiteral("主人")) >= 0);

        const MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30009"), now);
        MemoryRecallTool tool(&mm, scope);

        const QJsonObject definition = MemoryRecallTool::definition();
        const QString serialized = QString::fromUtf8(
            QJsonDocument(definition).toJson(QJsonDocument::Compact));
        QVERIFY(serialized.contains(QStringLiteral("memory_recall")));
        QVERIFY(serialized.contains(QStringLiteral("needs")));
        QVERIFY(!serialized.contains(QStringLiteral("viewer_id")));
        QVERIFY(!serialized.contains(QStringLiteral("allowed_groups")));
        QVERIFY(!serialized.contains(QStringLiteral("group_id")));
        QVERIFY(!serialized.contains(QStringLiteral("\"from\"")));
        QVERIFY(!serialized.contains(QStringLiteral("\"to\"")));

        const QJsonObject args{
            {QStringLiteral("needs"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("id"), QStringLiteral("need-1")},
                             {QStringLiteral("query"), QStringLiteral("火锅")}},
                 QJsonObject{{QStringLiteral("id"), QStringLiteral("need-2")},
                             {QStringLiteral("query"), QStringLiteral("完全不存在的词")}}}}};
        QString error;
        const QJsonObject payload = tool.execute(args, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(payload.value(QStringLiteral("status")).toString(),
                 QStringLiteral("complete"));
        QVERIFY(payload.value(QStringLiteral("has_empty")).toBool());
        QCOMPARE(payload.value(QStringLiteral("needs")).toArray().size(), 2);
        QCOMPARE(payload.value(QStringLiteral("needs")).toArray().at(0)
                     .toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("success"));
        QCOMPARE(payload.value(QStringLiteral("needs")).toArray().at(1)
                     .toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("empty"));

        const QJsonObject forged{
            {QStringLiteral("needs"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("need-1")},
                 {QStringLiteral("query"), QStringLiteral("火锅")},
                 {QStringLiteral("viewer_id"), QStringLiteral("owner")}}}}};
        error.clear();
        const QJsonObject rejected = tool.execute(forged, &error);
        QCOMPARE(error, QStringLiteral("invalid_arguments"));
        QCOMPARE(rejected.value(QStringLiteral("status")).toString(),
                 QStringLiteral("failed"));

        QJsonArray tooMany;
        for (int i = 0; i < 5; ++i) {
            tooMany.append(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("need-%1").arg(i)},
                {QStringLiteral("query"), QStringLiteral("火锅")}});
        }
        error.clear();
        tool.execute(QJsonObject{{QStringLiteral("needs"), tooMany}}, &error);
        QCOMPARE(error, QStringLiteral("invalid_arguments"));
    }

    void episodicAndLongSummaryRespectOwnerCrossChannelPolicy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("cross_channel.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        QVERIFY(mm.addEpisodicMemory(
            QStringLiteral("七夕节用户与茉子约定永远在一起"),
            QStringLiteral("event"), 0.8,
            QDateTime(QDate(2026, 8, 19), QTime(23, 0)),
            QStringLiteral("[]"), QStringLiteral("pet"),
            QStringLiteral("private"), QStringLiteral("user"),
            QStringLiteral("default")));
        QVERIFY(mm.addLongTermSummary(
            QStringLiteral("用户与茉子互道晚安，七夕互表爱意"),
            1, QStringLiteral("[]"), Channel::kPet,
            Channel::kDefaultSession));
        RequestMeta petMeta;
        petMeta.channel = Channel::kPet;
        petMeta.sessionId = Channel::kDefaultSession;
        petMeta.speaker = QStringLiteral("主人");
        petMeta.subject = QStringLiteral("user");
        QVERIFY(mm.saveQATurn(QStringLiteral("七夕那天我们说好要一直在一起"),
                              QStringLiteral("{\"sentences\":[]}"), {},
                              petMeta));

        const QDateTime now = QDateTime::currentDateTime();
        const QJsonObject args{
            {QStringLiteral("needs"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("need-1")},
                 {QStringLiteral("query"), QStringLiteral("七夕")},
                 {QStringLiteral("kinds"),
                  QJsonArray{QStringLiteral("episodic"),
                             QStringLiteral("long_term_summary"),
                             QStringLiteral("chat_history")}}}}}};

        QString error;
        MemoryRecallTool enabled(&mm, ownerPrivateScope(now, true));
        const QJsonObject found = enabled.execute(args, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(found.value(QStringLiteral("items")).toArray().size() >= 2);

        MemoryRecallTool disabled(&mm, ownerPrivateScope(now, false));
        const QJsonObject hidden = disabled.execute(args, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(hidden.value(QStringLiteral("items")).toArray().isEmpty());

        MemoryRecallTool group(&mm, ownerGroupScope(QStringLiteral("30001"), now));
        const QJsonObject groupResult = group.execute(args, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(groupResult.value(QStringLiteral("items")).toArray().isEmpty());
    }

    void timeFrameResolvesFestivalAndRelativeExpressions()
    {
        MemoryTimeFrame festival;
        QString error;
        QVERIFY(MemoryTimeFrame::fromJson(
            QJsonObject{
                {QStringLiteral("anchor"),
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("festival")},
                             {QStringLiteral("name"), QStringLiteral("七夕")}}},
                {QStringLiteral("occurrence"),
                 QJsonObject{{QStringLiteral("type"),
                              QStringLiteral("most_recent_past")}}},
                {QStringLiteral("span"),
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("day")}}}},
            &festival, &error));
        MemoryResolvedTime resolved = TimeFrameResolver::resolve(
            QStringLiteral("七夕"), MemoryTimeTarget::MessageTime, festival,
            QDateTime(QDate(2026, 11, 2), QTime(12, 0)),
            QStringLiteral("Asia/Shanghai"));
        QVERIFY2(resolved.valid, qPrintable(resolved.error));
        QCOMPARE(resolved.from.date(), QDate(2026, 8, 19));
        QCOMPARE(resolved.from.time(), QTime(0, 0));
        QCOMPARE(resolved.to.date(), QDate(2026, 8, 20));

        MemoryTimeFrame yesterdayAfternoon;
        QVERIFY(MemoryTimeFrame::fromJson(
            QJsonObject{
                {QStringLiteral("anchor"),
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("relative_day")},
                             {QStringLiteral("offset"), -1}}},
                {QStringLiteral("period"), QStringLiteral("afternoon")}},
            &yesterdayAfternoon, &error));
        MemoryResolvedTime yesterday = TimeFrameResolver::resolve(
            QStringLiteral("昨天下午"), MemoryTimeTarget::MessageTime,
            yesterdayAfternoon,
            QDateTime(QDate(2026, 11, 2), QTime(12, 0)),
            QStringLiteral("Asia/Shanghai"));
        QVERIFY2(yesterday.valid, qPrintable(yesterday.error));
        QCOMPARE(yesterday.from.time(), QTime(12, 0));
        QCOMPARE(yesterday.to.time(), QTime(18, 0));

        MemoryTimeFrame unknown;
        QVERIFY(MemoryTimeFrame::fromJson(
            QJsonObject{
                {QStringLiteral("anchor"),
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("festival")},
                             {QStringLiteral("name"), QStringLiteral("不存在的节日")}}}},
            &unknown, &error));
        const MemoryResolvedTime unresolved = TimeFrameResolver::resolve(
            QStringLiteral("不存在的节日"), MemoryTimeTarget::MessageTime,
            unknown, QDateTime(QDate(2026, 11, 2), QTime(12, 0)),
            QStringLiteral("Asia/Shanghai"));
        QVERIFY(!unresolved.valid);
    }

    void toolPathRequiresFrameAndAuditsResolution()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("timeframe.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        MemoryAgentScope scope =
            ownerGroupScope(QStringLiteral("30001"), QDateTime::currentDateTime());
        scope.requireTimeFrame = true;
        scope.accessScope = QStringLiteral("current_group");
        scope.otherChannelsVisible = false;

        MemoryRecallPlan plan;
        MemoryRecallNeed need;
        need.id = QStringLiteral("need-1");
        need.query = QStringLiteral("脚踝");
        need.kinds = {QStringLiteral("group_recent")};
        need.timeExpression = QStringLiteral("两天前");
        need.hasTime = true;
        plan.needs.append(need);

        MemoryExecutor executor(&mm, scope);
        MemoryAgentResult missingFrame = executor.execute(plan);
        QCOMPARE(missingFrame.needs.size(), 1);
        QCOMPARE(missingFrame.needs.first().status,
                 QStringLiteral("frame_required"));
        QCOMPARE(missingFrame.needs.first().resolverStatus,
                 QStringLiteral("frame_required"));
        QCOMPARE(missingFrame.needs.first().accessScopeStatus,
                 QStringLiteral("current_group"));
        QVERIFY(!missingFrame.needs.first().framePresent);
        QVERIFY(!missingFrame.needs.first().expressionHash.isEmpty());

        plan.needs[0].hasTimeFrame = true;
        plan.needs[0].timeFrame.anchor = MemoryTimeFrame::Anchor::RelativeDay;
        plan.needs[0].timeFrame.offset = -2;
        plan.needs[0].timeFrame.span = MemoryTimeFrame::Span::Day;
        MemoryAgentResult resolved = executor.execute(plan);
        QCOMPARE(resolved.needs.size(), 1);
        QCOMPARE(resolved.needs.first().resolverType,
                 QStringLiteral("time_frame"));
        QCOMPARE(resolved.needs.first().resolverStatus, QStringLiteral("ok"));
        QCOMPARE(resolved.needs.first().anchorType,
                 QStringLiteral("relative_day"));
        QCOMPARE(resolved.needs.first().frameOffset, -2);
        QVERIFY(!resolved.needs.first().resolvedFrom.isEmpty());
        QVERIFY(resolved.needs.first().timeConstraintApplied);
    }

    void envelopePlanPreservesModeOverviewAndFrame()
    {
        QJsonObject frame;
        frame.insert(QStringLiteral("anchor"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("relative_day")},
                                 {QStringLiteral("offset"), -2}});
        frame.insert(QStringLiteral("span"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("day")}});
        QJsonObject time;
        time.insert(QStringLiteral("expression"), QStringLiteral("两天前"));
        time.insert(QStringLiteral("target"), QStringLiteral("message_time"));
        time.insert(QStringLiteral("frame"), frame);
        QJsonObject need;
        need.insert(QStringLiteral("id"), QStringLiteral("need-1"));
        need.insert(QStringLiteral("query"), QStringLiteral("脚踝"));
        need.insert(QStringLiteral("time"), time);
        QJsonObject content;
        content.insert(QStringLiteral("action"), QStringLiteral("recall"));
        content.insert(QStringLiteral("mode"), QStringLiteral("overview"));
        content.insert(QStringLiteral("overview"),
                       QJsonObject{{QStringLiteral("bucket_minutes"), 180},
                                   {QStringLiteral("max_buckets"), 8}});
        content.insert(QStringLiteral("needs"), QJsonArray{need});

        MemoryRecallPlan plan;
        QString error;
        QVERIFY2(MemoryRecallPlan::fromEnvelope(content, &plan, &error),
                 qPrintable(error));
        QCOMPARE(plan.mode, QStringLiteral("overview"));
        QCOMPARE(plan.overview.value(QStringLiteral("max_buckets")).toInt(), 8);
        QCOMPARE(plan.needs.size(), 1);
        QVERIFY(plan.needs.first().hasTimeFrame);
        QCOMPARE(plan.needs.first().timeFrame.anchor,
                 MemoryTimeFrame::Anchor::RelativeDay);
        QCOMPARE(plan.needs.first().timeFrame.offset, -2);
    }

    void overviewLongRangeKeepsBucketLabelsHonest()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("overview-long.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDate monday = QDate::currentDate()
                                 .addDays(1 - QDate::currentDate().dayOfWeek())
                                 .addDays(-7);
        for (int i = 0; i < 6; ++i) {
            const QDateTime at(monday.addDays(i), QTime(12, 0));
            const qlonglong id = makeInteractionAt(
                mm, QStringLiteral("30001"), at,
                QStringLiteral("上一周群聊主题%1").arg(i),
                QStringLiteral("20001"), QStringLiteral("主人"));
            QVERIFY(id >= 0);
            QVERIFY(mm.saveInteractionSummary(
                id, QStringLiteral("上一周群聊摘要%1").arg(i),
                QStringLiteral("ready")));
        }

        MemoryAgentScope scope =
            ownerGroupScope(QStringLiteral("30001"), QDateTime::currentDateTime());
        scope.requireTimeFrame = true;
        scope.accessScope = QStringLiteral("current_group");

        QJsonObject frame;
        frame.insert(QStringLiteral("anchor"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("relative_day")},
                                 {QStringLiteral("offset"), -7}});
        frame.insert(QStringLiteral("span"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("week_containing")}});
        QJsonObject time;
        time.insert(QStringLiteral("expression"), QStringLiteral("上一周"));
        time.insert(QStringLiteral("target"), QStringLiteral("message_time"));
        time.insert(QStringLiteral("frame"), frame);
        QJsonObject need;
        need.insert(QStringLiteral("id"), QStringLiteral("overview-1"));
        need.insert(QStringLiteral("query"), QStringLiteral("上一周群聊"));
        need.insert(QStringLiteral("time"), time);
        QJsonObject overview;
        overview.insert(QStringLiteral("bucket_minutes"), 180);
        overview.insert(QStringLiteral("max_buckets"), 8);
        QJsonObject planObject;
        planObject.insert(QStringLiteral("mode"), QStringLiteral("overview"));
        planObject.insert(QStringLiteral("needs"), QJsonArray{need});
        planObject.insert(QStringLiteral("overview"), overview);

        MemoryRecallPlan plan;
        QString error;
        QVERIFY2(MemoryRecallPlan::fromJson(planObject, &plan, &error),
                 qPrintable(error));
        const MemoryOverviewResult result =
            buildMemoryOverview(&mm, scope, plan, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonObject coverage =
            result.payload.value(QStringLiteral("coverage")).toObject();
        QCOMPARE(coverage.value(QStringLiteral("total_interactions")).toInt(), 6);
        QCOMPARE(coverage.value(QStringLiteral("scanned")).toInt(), 6);
        const QJsonArray buckets =
            result.payload.value(QStringLiteral("buckets")).toArray();
        QCOMPARE(buckets.size(), 8);
        QCOMPARE(buckets.last().toObject().value(QStringLiteral("to")).toString(),
                 coverage.value(QStringLiteral("to")).toString());
    }

    void overviewRejectsMultipleNeedsAndAmbiguousGroups()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("overview-shape.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        MemoryAgentScope scope =
            ownerGroupScope(QStringLiteral("30001"), QDateTime::currentDateTime());
        scope.requireTimeFrame = true;
        scope.accessScope = QStringLiteral("current_group");

        QJsonObject frame;
        frame.insert(QStringLiteral("anchor"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("relative_day")},
                                 {QStringLiteral("offset"), 0}});
        frame.insert(QStringLiteral("span"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("day")}});
        QJsonObject time;
        time.insert(QStringLiteral("expression"), QStringLiteral("今天"));
        time.insert(QStringLiteral("target"), QStringLiteral("message_time"));
        time.insert(QStringLiteral("frame"), frame);
        QJsonObject need;
        need.insert(QStringLiteral("id"), QStringLiteral("overview-1"));
        need.insert(QStringLiteral("query"), QStringLiteral("今天群聊"));
        need.insert(QStringLiteral("time"), time);
        QJsonObject secondNeed = need;
        secondNeed.insert(QStringLiteral("id"), QStringLiteral("overview-2"));
        secondNeed.insert(QStringLiteral("query"), QStringLiteral("今天群聊二"));
        QJsonObject planObject;
        planObject.insert(QStringLiteral("mode"), QStringLiteral("overview"));
        planObject.insert(QStringLiteral("needs"), QJsonArray{need, secondNeed});
        MemoryRecallPlan plan;
        QString error;
        QVERIFY2(MemoryRecallPlan::fromJson(planObject, &plan, &error),
                 qPrintable(error));
        MemoryOverviewResult result =
            buildMemoryOverview(&mm, scope, plan, &error);
        QCOMPARE(error, QStringLiteral("invalid_overview_plan"));
        QVERIFY(result.payload.value(QStringLiteral("error")).isString());

        MemoryAgentScope privateScope =
            ownerPrivateScope(QDateTime::currentDateTime(), true);
        privateScope.allowedGroupIds = {QStringLiteral("30001"),
                                        QStringLiteral("30002")};
        privateScope.requireTimeFrame = true;
        privateScope.accessScope = QStringLiteral("owner_private_cross_channel");
        privateScope.access.allowedGroupIds = privateScope.allowedGroupIds;
        planObject.insert(QStringLiteral("needs"), QJsonArray{need});
        QVERIFY2(MemoryRecallPlan::fromJson(planObject, &plan, &error),
                 qPrintable(error));
        result = buildMemoryOverview(&mm, privateScope, plan, &error);
        QCOMPARE(error, QStringLiteral("overview_group_ambiguous"));
        QVERIFY(result.payload.value(QStringLiteral("error")).isString());
    }

    void overviewScansRangeAndExposesBuckets()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("overview.db"));
        mm.seedIdentity({QStringLiteral("20001")});
        const QDate today = QDate::currentDate();
        for (int i = 0; i < 6; ++i) {
            const QDateTime at(today, QTime(8 + i, 0));
            const qlonglong id = makeInteractionAt(
                mm, QStringLiteral("30001"), at,
                QStringLiteral("今天群聊主题%1").arg(i),
                QStringLiteral("20001"), QStringLiteral("主人"));
            QVERIFY(id >= 0);
            QVERIFY(mm.saveInteractionSummary(
                id, QStringLiteral("今天群聊摘要%1").arg(i),
                QStringLiteral("ready")));
        }

        const QDateTime now = QDateTime::currentDateTime();
        MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30001"), now);
        scope.requireTimeFrame = true;
        scope.accessScope = QStringLiteral("current_group");
        QJsonObject frame;
        frame.insert(QStringLiteral("anchor"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("relative_day")},
                                 {QStringLiteral("offset"), 0}});
        frame.insert(QStringLiteral("span"),
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("day")}});
        QJsonObject time;
        time.insert(QStringLiteral("expression"), QStringLiteral("今天"));
        time.insert(QStringLiteral("target"), QStringLiteral("message_time"));
        time.insert(QStringLiteral("frame"), frame);
        QJsonObject needJson;
        needJson.insert(QStringLiteral("id"), QStringLiteral("overview-1"));
        needJson.insert(QStringLiteral("query"), QStringLiteral("今天群聊"));
        needJson.insert(QStringLiteral("time"), time);
        QJsonObject overview;
        overview.insert(QStringLiteral("bucket_minutes"), 180);
        overview.insert(QStringLiteral("max_buckets"), 8);
        QJsonObject planObject;
        planObject.insert(QStringLiteral("mode"), QStringLiteral("overview"));
        planObject.insert(QStringLiteral("needs"), QJsonArray{needJson});
        planObject.insert(QStringLiteral("overview"), overview);

        MemoryRecallPlan plan;
        QString error;
        QVERIFY2(MemoryRecallPlan::fromJson(planObject, &plan, &error),
                 qPrintable(error));
        const MemoryOverviewResult result =
            buildMemoryOverview(&mm, scope, plan, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QJsonObject coverage =
            result.payload.value(QStringLiteral("coverage")).toObject();
        QCOMPARE(coverage.value(QStringLiteral("total_interactions")).toInt(), 6);
        QCOMPARE(coverage.value(QStringLiteral("ready_summaries")).toInt(), 6);
        QCOMPARE(coverage.value(QStringLiteral("scanned")).toInt(), 6);
        QCOMPARE(coverage.value(QStringLiteral("aggregated")).toInt(), 6);
        QCOMPARE(result.payload.value(QStringLiteral("scope"))
                     .toObject()
                     .value(QStringLiteral("access_scope"))
                     .toString(),
                 QStringLiteral("current_group"));
        QVERIFY(!result.handles.isEmpty());
        QVERIFY(!result.payload.value(QStringLiteral("buckets")).toArray().isEmpty());

        MemoryRecallTool tool(&mm, scope);
        const QJsonObject planJson = planObject;
        const QJsonObject toolOverview = tool.execute(planJson, &error);
        QVERIFY2(toolOverview.value(QStringLiteral("coverage"))
                     .toObject().value(QStringLiteral("scanned")).toInt() >= 6,
                 qPrintable(error));
        QCOMPARE(toolOverview.value(QStringLiteral("scope"))
                     .toObject().value(QStringLiteral("access_scope"))
                     .toString(),
                 QStringLiteral("current_group"));
        QStringList handles = result.handles.keys();
        std::sort(handles.begin(), handles.end());
        QJsonArray handleArray;
        for (const QString &handle : handles) {
            handleArray.append(handle);
        }
        const QJsonObject readResult = tool.read(
            QJsonObject{{QStringLiteral("handles"), handleArray}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(readResult.value(QStringLiteral("items")).toArray().size() >= 6);
    }

    void planAcceptsFrameAndOverviewMode()
    {
        MemoryRecallPlan plan;
        QString error;
        QJsonObject anchor;
        anchor.insert(QStringLiteral("type"), QStringLiteral("festival"));
        anchor.insert(QStringLiteral("name"), QStringLiteral("七夕"));
        QJsonObject occurrence;
        occurrence.insert(QStringLiteral("type"), QStringLiteral("most_recent_past"));
        QJsonObject span;
        span.insert(QStringLiteral("type"), QStringLiteral("day"));
        QJsonObject frame;
        frame.insert(QStringLiteral("anchor"), anchor);
        frame.insert(QStringLiteral("occurrence"), occurrence);
        frame.insert(QStringLiteral("span"), span);
        QJsonObject time;
        time.insert(QStringLiteral("expression"), QStringLiteral("七夕"));
        time.insert(QStringLiteral("target"), QStringLiteral("message_time"));
        time.insert(QStringLiteral("frame"), frame);
        QJsonObject need;
        need.insert(QStringLiteral("id"), QStringLiteral("need-1"));
        need.insert(QStringLiteral("query"), QStringLiteral("七夕约定"));
        need.insert(QStringLiteral("time"), time);
        QJsonObject overview;
        overview.insert(QStringLiteral("bucket_minutes"), 180);
        overview.insert(QStringLiteral("max_buckets"), 8);
        QJsonObject object;
        object.insert(QStringLiteral("mode"), QStringLiteral("overview"));
        object.insert(QStringLiteral("overview"), overview);
        object.insert(QStringLiteral("needs"), QJsonArray{need});
        QVERIFY2(MemoryRecallPlan::fromJson(object, &plan, &error),
                 qPrintable(error));
        QCOMPARE(plan.mode, QStringLiteral("overview"));
        QCOMPARE(plan.needs.size(), 1);
        QVERIFY(plan.needs.first().hasTimeFrame);
        QCOMPARE(plan.needs.first().timeFrame.anchor,
                 MemoryTimeFrame::Anchor::Festival);
        QCOMPARE(plan.needs.first().timeFrame.festival, QStringLiteral("七夕"));
    }

    void plannerOnlyRunsForQQMemoryQuestions()
    {
        MemoryAgentConfig config;
        config.mode = QStringLiteral("legacy");
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
