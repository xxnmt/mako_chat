#include <QtTest>
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QJsonDocument>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>
#include "llmservice.h"
#include "configmanager.h"
#include "cognitivecoordinator.h"
#include "cognitivesnapshot.h"
#include "memorydashscope.h"

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
#include "evidenceaudit.h"
#include "expressioncontract.h"
#include "memoryresultjson.h"
#include <QImage>

namespace {

// MemoryManager 之外的独立连接：用于伪造"当前活动"的 thought_result（只测试需要）。
class RawSqlConnection
{
public:
    RawSqlConnection(const QString &path, const QString &tag)
        : m_conn(QStringLiteral("tst_agent_raw_") + tag)
    {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_conn);
        m_db.setDatabaseName(path);
    }
    ~RawSqlConnection()
    {
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();
        if (QSqlDatabase::contains(m_conn)) QSqlDatabase::removeDatabase(m_conn);
    }
    bool open() { return m_db.open(); }
    bool exec(const QString &sql, const QString &text = {})
    {
        QSqlQuery q(m_db);
        q.prepare(sql);
        if (!text.isEmpty()) q.addBindValue(text);
        return q.exec();
    }

private:
    QString m_conn;
    QSqlDatabase m_db;
};

void serveCompletions(QTcpServer &server,QList<QByteArray> &bodies,std::function<QString(int)> respond,
                      std::function<QString(int)> finish=[](int){return QString("stop");})
{
    QObject::connect(&server,&QTcpServer::newConnection,&server,[&server,&bodies,respond,finish] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,&bodies,respond,finish] {
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
            QJsonObject msg{{"finish_reason",finish(bodies.size())},{"message",QJsonObject{{"content",answer}}}};
            const auto body = QJsonDocument(QJsonObject{{"choices",QJsonArray{msg}}}).toJson();
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
}

// 从请求体里取出"能力清单"那条 system 提示词，返回前缀之后的 JSON 文本。
// 找不到时返回空串，便于断言"没有注入"。
QString capabilityPromptText(const QByteArray &body)
{
    const QString prefix = QStringLiteral(
        "【你当前被授予的能力（服务端核验，只读）。不要声称未列出的能力。】");
    const QJsonArray messages = QJsonDocument::fromJson(body).object()
                                    .value(QStringLiteral("messages")).toArray();
    for (const auto &value : messages) {
        const auto message = value.toObject();
        const QString content = message.value(QStringLiteral("content")).toString();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("system")
            && content.startsWith(prefix)) {
            return content.mid(prefix.size());
        }
    }
    return {};
}

// 语义核验/主动联系用例的配置守卫：写 setting.json + identity.json，析构时恢复原文件。
// 与 tst_actionoutbox 的 ProactiveIdentityGuard 同思路，但额外要求长期记忆与语义核验开启。
class SemanticReviewConfigGuard
{
public:
    SemanticReviewConfigGuard()
    {
        auto &cfg = ConfigManager::instance();
        QFile setting(cfg.getConfigFilePath());
        m_settingExisted = setting.open(QIODevice::ReadOnly);
        if (m_settingExisted) m_setting = setting.readAll();
        setting.close();
        const QString identityPath = cfg.getConfigDirPath() + "/identity.json";
        QFile identity(identityPath);
        m_identityExisted = identity.open(QIODevice::ReadOnly);
        if (m_identityExisted) m_identity = identity.readAll();
        identity.close();
        m_identityPath = identityPath;
    }
    ~SemanticReviewConfigGuard()
    {
        auto &cfg = ConfigManager::instance();
        cfg.writeConfigDocument(QStringLiteral("setting.json"),
                                m_settingExisted ? m_setting : QByteArray("{}"), nullptr);
        if (m_identityExisted) {
            cfg.writeConfigDocument(QStringLiteral("identity.json"), m_identity, nullptr);
        } else {
            QFile::remove(m_identityPath);
            cfg.reloadIdentity();
        }
    }
    bool enable()
    {
        auto &cfg = ConfigManager::instance();
        if (!cfg.writeConfigDocument(QStringLiteral("setting.json"),
                QJsonDocument(QJsonObject{{QStringLiteral("memory"),
                                           QJsonObject{{QStringLiteral("long_term_memory_enabled"), true}}}})
                    .toJson(),
                nullptr))
            return false;
        // 主动联系目标可达、安静时段置空、冷却 0、语义核验开启，避免用例受执行时刻影响。
        return cfg.writeConfigDocument(QStringLiteral("identity.json"), QByteArray(
            R"({"proactive":{"enabled":true,"private_writing":true,"cooldown_seconds":0,)"
            R"("quiet_hours":{"start":"00:00","end":"00:00"},)"
            R"("targets":[{"channel":"pet","session_id":"default"}]},)"
            R"("cognition":{"semantic_verifier":{"enabled":true,"max_verifier_calls":1,"max_repairs":1}}})"),
            nullptr);
    }

private:
    QString m_identityPath;
    QByteArray m_setting;
    QByteArray m_identity;
    bool m_settingExisted = false;
    bool m_identityExisted = false;
};

// 造一个到期关注（来源轮次 + close 收口 + consider），再把关注重新领取到 pending。
// close 仍作为前台状态记录，但不再抑制主动联系。
// 返回 turn，*version 为本次到期回顾版本；失败返回 0。
qlonglong seedDueConcernForSemanticReview(MemoryManager &db, const QString &path,
                                          const QString &tag, int *version)
{
    RequestMeta meta;
    meta.channel = Channel::kPet;
    meta.sessionId = Channel::kDefaultSession;
    meta.subject = QStringLiteral("user");
    meta.turnKind = QStringLiteral("reply");
    meta.eventId = meta.runId = QStringLiteral("semantic-coverage-%1").arg(tag);
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    const auto turn = db.recordInput(QStringLiteral("明天想去看海"), meta, true);
    if (turn <= 0 || !db.recordQuiet(turn, QStringLiteral("close"))) return 0;
    const auto claim = db.claimPrivateReflection(12, 60);
    if (claim.value(QStringLiteral("id")).toInteger() != turn) return 0;
    if (!db.finishPrivateReflection(turn, 1, QStringLiteral("consider"),
                                    QStringLiteral("看海"), QJsonObject{}, QJsonArray{},
                                    QJsonObject{}, QString{}, QJsonObject{}))
        return 0;
    {
        RawSqlConnection raw(path, tag);
        if (!raw.open()) return 0;
        if (!raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                     "WHERE turn_id=%1").arg(turn)))
            return 0;
        if (!raw.exec(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120")))
            return 0;
    }
    const auto context = db.claimDueConcern(12, 60);
    if (context.value(QStringLiteral("id")).toInteger() != turn) return 0;
    if (version) *version = context.value(QStringLiteral("version")).toInt();
    return turn;
}

// R12-1a：OpenWake 运行时用例的配置守卫。setting.json 打开长期记忆；identity.json
// 保持 privateWriting=false / proactive=false，只启用 cognition.open_wake，用来验证
// OpenWake 是独立的内部认知入口。析构时恢复原文件，避免影响其它用例。
class OpenWakeConfigGuard
{
public:
    OpenWakeConfigGuard()
    {
        auto &cfg = ConfigManager::instance();
        QFile setting(cfg.getConfigFilePath());
        m_settingExisted = setting.open(QIODevice::ReadOnly);
        if (m_settingExisted) m_setting = setting.readAll();
        setting.close();
        m_identityPath = cfg.getConfigDirPath() + "/identity.json";
        QFile identity(m_identityPath);
        m_identityExisted = identity.open(QIODevice::ReadOnly);
        if (m_identityExisted) m_identity = identity.readAll();
        identity.close();
    }
    ~OpenWakeConfigGuard()
    {
        auto &cfg = ConfigManager::instance();
        cfg.writeConfigDocument(QStringLiteral("setting.json"),
                                m_settingExisted ? m_setting : QByteArray("{}"), nullptr);
        if (m_identityExisted) {
            cfg.writeConfigDocument(QStringLiteral("identity.json"), m_identity, nullptr);
        } else {
            QFile::remove(m_identityPath);
            cfg.reloadIdentity();
        }
    }
    // dailyLimit<0 时不写 daily_limit，沿用配置默认值 3。
    bool write(const QJsonObject &openWake, const QJsonObject &proactiveOverrides = {},
               int dailyLimit = -1)
    {
        auto &cfg = ConfigManager::instance();
        if (!cfg.writeConfigDocument(QStringLiteral("setting.json"),
                QJsonDocument(QJsonObject{{QStringLiteral("memory"),
                                           QJsonObject{{QStringLiteral("long_term_memory_enabled"), true}}}})
                    .toJson(),
                nullptr))
            return false;
        QJsonObject proactive{{QStringLiteral("enabled"), false},
                              {QStringLiteral("private_writing"), false},
                              {QStringLiteral("idle_seconds"), 0}};
        for (auto it = proactiveOverrides.constBegin(); it != proactiveOverrides.constEnd(); ++it)
            proactive.insert(it.key(), it.value());
        if (dailyLimit >= 0) proactive.insert(QStringLiteral("daily_limit"), dailyLimit);
        const QJsonObject root{
            {QStringLiteral("proactive"), proactive},
            {QStringLiteral("cognition"), QJsonObject{{QStringLiteral("open_wake"), openWake}}}};
        return cfg.writeConfigDocument(QStringLiteral("identity.json"),
                                       QJsonDocument(root).toJson(QJsonDocument::Compact), nullptr);
    }

private:
    QString m_identityPath;
    QByteArray m_setting;
    QByteArray m_identity;
    bool m_settingExisted = false;
    bool m_identityExisted = false;
};

QJsonObject openWakeTestJson(bool enabled, int maxPerDay, int maxNonOpen = 3,
                             int sharedBudgetPercent = 30)
{
    return QJsonObject{{QStringLiteral("enabled"), enabled},
                       {QStringLiteral("allow_contact"), false},
                       {QStringLiteral("initial_delay_seconds"), 60},
                       {QStringLiteral("min_interval_seconds"), 300},
                       {QStringLiteral("default_interval_seconds"), 300},
                       {QStringLiteral("max_interval_seconds"), 86400},
                       {QStringLiteral("max_non_open_jobs_when_due"), maxNonOpen},
                       {QStringLiteral("max_model_steps_per_wake"), 3},
                       {QStringLiteral("max_per_day"), maxPerDay},
                       {QStringLiteral("shared_budget_percent"), sharedBudgetPercent}};
}

// OpenWake rest/reflect 的合法 proposal（_origin 由服务端回显，测试里显式给出）。
QJsonObject openWakeProposal(const QJsonObject &origin, const QString &decision,
                             const QString &note = {}, int afterMinutes = 30)
{
    QJsonObject proposal{{QStringLiteral("schema_version"), 1},
                         {QStringLiteral("decision"), decision},
                         {QStringLiteral("next_wake"),
                          QJsonObject{{QStringLiteral("mode"), QStringLiteral("after")},
                                      {QStringLiteral("after_minutes"), afterMinutes}}},
                         {QStringLiteral("_origin"), origin}};
    if (!note.isEmpty()) proposal.insert(QStringLiteral("note"), note);
    return proposal;
}

bool execMemorySql(const QString &sql)
{
    QSqlQuery query(QSqlDatabase::database("memory_db_connection"));
    return query.exec(sql);
}

qint64 scalarMemorySql(const QString &sql)
{
    QSqlQuery query(QSqlDatabase::database("memory_db_connection"));
    if (!query.exec(sql) || !query.next()) return -1;
    return query.value(0).toLongLong();
}

QString textMemorySql(const QString &sql)
{
    QSqlQuery query(QSqlDatabase::database("memory_db_connection"));
    if (!query.exec(sql) || !query.next()) return {};
    return query.value(0).toString();
}

// 把 OpenWake 调度拨到"现在到期"，并回拨回顾冷却，模拟等待期已过。
bool rewindOpenWakeClock()
{
    return execMemorySql(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120"))
        && execMemorySql(QStringLiteral("UPDATE open_wake_state SET next_due_at=unixepoch()-1"));
}

// 造一个可领取的新来源回顾（recordInput + close 收口），返回 turn。
qlonglong seedRuntimeReflectionSource(MemoryManager &db, const QString &tag)
{
    RequestMeta meta;
    meta.eventId = meta.runId = QStringLiteral("runtime-source-%1").arg(tag);
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    const auto turn = db.recordInput(QStringLiteral("今天把窗台擦干净了"), meta, true);
    if (turn <= 0 || !db.recordQuiet(turn, QStringLiteral("close"))) return 0;
    return turn;
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

// R12-0 日志降噪断言用：捕获 qInfo/qDebug 行（安装期间不转发到默认处理器）。
// 只在单线程测试中使用，作用域退出时恢复之前的处理器。
class MessageCapture
{
public:
    struct Line {
        QtMsgType type;
        QString message;
    };
    MessageCapture()
    {
        s_active = this;
        m_previous = qInstallMessageHandler(&MessageCapture::handle);
    }
    ~MessageCapture()
    {
        qInstallMessageHandler(m_previous);
        s_active = nullptr;
    }
    QList<Line> lines() const { return m_lines; }

private:
    static void handle(QtMsgType type, const QMessageLogContext &, const QString &message)
    {
        if (!s_active) return;
        s_active->m_lines.append({type, message});
    }
    static MessageCapture *s_active;
    QtMessageHandler m_previous = nullptr;
    QList<Line> m_lines;
};
MessageCapture *MessageCapture::s_active = nullptr;

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

    void understandingSnapshotAtDispatch_data()
    {
        QTest::addColumn<bool>("updateBeforeRequest");
        QTest::newRow("committed before dispatch") << true;
        QTest::newRow("committed after dispatch") << false;
    }

    void understandingSnapshotAtDispatch()
    {
        QFETCH(bool, updateBeforeRequest);
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
        const auto commit = [&] {
            const QJsonObject change{{"matter",""},{"version",0},{"topic","预约"},{"quote","预约延期"},
                {"status","tentative"},{"understanding","预约日期待定"},{"kind","self_report"}};
            return db.commitObservation(source, {{"outcome","updated"},{"changes",QJsonArray{change}}});
        };
        bool committed = updateBeforeRequest && commit();
        serveCompletions(server, bodies, [&](int) -> QString {
            if (!updateBeforeRequest) committed = commit();
            return R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})";
        });
        LLMService service("test", nullptr, QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        QSignalSpy spoken(&service, &LLMService::sentencesReady);
        QSignalSpy quiet(&service, &LLMService::replyQuiet);
        service.askDeepSeek(updateBeforeRequest ? "预约" : "最近如何", {}, group);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size() + quiet.size(), 1, 5000);
        QVERIFY(committed);
        QCOMPARE(bodies.size(), 1);
        QVERIFY(bodies.first().contains(QStringLiteral("预约延期").toUtf8()));
        // P3：表达模型保留相关背景；是否说出由听众与保密语义决定。
        QVERIFY(bodies.last().contains(QStringLiteral("预约延期").toUtf8()));
        QCOMPARE(bodies.last().contains("CURRENT_UNDERSTANDING\\n"), updateBeforeRequest);
        QCOMPARE(spoken.size(), 1);
    }

    // 切片 A：前台 owner-private 请求体带服务端认知快照（事实分层、internal_wakes 等域）；
    // 群聊请求体不注入，避免把私人来源与自发动机当成群内公开事实。
    void foregroundPromptCarriesCognitiveSnapshot()
    {
        auto &cfg = ConfigManager::instance();
        const QString settingPath = cfg.getConfigFilePath();
        QFile setting(settingPath);
        const bool settingExisted = setting.open(QIODevice::ReadOnly);
        const QByteArray settingOriginal = settingExisted ? setting.readAll() : QByteArray{};
        setting.close();
        auto restoreSetting = qScopeGuard([&] {
            cfg.writeConfigDocument(QStringLiteral("setting.json"),
                settingExisted ? settingOriginal : QByteArray("{}"), nullptr);
            if (!settingExisted) QFile::remove(settingPath);
        });
        QVERIFY(cfg.writeConfigDocument(
            QStringLiteral("setting.json"),
            QJsonDocument(QJsonObject{{QStringLiteral("memory"),
                QJsonObject{{QStringLiteral("long_term_memory_enabled"), true}}}}).toJson(),
            nullptr));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("foreground-snapshot.db")));
        RequestMeta ownerMeta;
        ownerMeta.eventId = ownerMeta.runId = QStringLiteral("snapshot-private");
        QVERIFY(db.recordInput(QStringLiteral("主人私聊里的一句话"), ownerMeta, true) > 0);

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [](int) -> QString {
            return QStringLiteral(
                R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})");
        });
        LLMService service("test", nullptr,
            QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        QSignalSpy spoken(&service, &LLMService::sentencesReady);
        QSignalSpy failed(&service, &LLMService::replyAbnormal);

        service.askDeepSeek(QStringLiteral("在吗"), {}, ownerMeta);
        QTRY_VERIFY_WITH_TIMEOUT(!bodies.isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(spoken.size() + failed.size() > 0, 5000);
        QCOMPARE(spoken.size(), 1);

        const QString prefix = CognitiveSnapshotBuilder::promptPrefix();
        QString snapshotContent;
        QString snapshotFixedRules;
        for (const auto &value : QJsonDocument::fromJson(bodies.first()).object()
                                     .value(QStringLiteral("messages")).toArray()) {
            const auto message = value.toObject();
            const QString content = message.value(QStringLiteral("content")).toString();
            if (message.value(QStringLiteral("role")).toString() == QStringLiteral("system")
                && content.startsWith(prefix)) {
                snapshotContent = content.mid(prefix.size());
            }
        }
        QVERIFY2(!snapshotContent.isEmpty(),
                 "owner-private foreground request must carry cognitive snapshot");
        // R12-1a：固定说明（事实分层/只读边界）在 system content 的固定规则行里，
        // 不占快照数据 JSON 预算；JSON 只含数据域，不再有 note/internal_wake_note 静态键。
        const qsizetype jsonStart = snapshotContent.indexOf(QLatin1Char('{'));
        QVERIFY2(jsonStart > 0, qPrintable(snapshotContent.left(120)));
        snapshotFixedRules = snapshotContent.left(jsonStart);
        QVERIFY2(snapshotFixedRules.contains(QStringLiteral("服务端事实分层")),
                 qPrintable(snapshotFixedRules));
        QVERIFY2(snapshotFixedRules.contains(QStringLiteral("内部唤醒不是主人约定")),
                 qPrintable(snapshotFixedRules));
        const QJsonObject snapshotJson =
            QJsonDocument::fromJson(snapshotContent.mid(jsonStart).toUtf8()).object();
        QVERIFY(snapshotJson.contains(QStringLiteral("internal_wakes")));
        QVERIFY(snapshotJson.contains(QStringLiteral("observations")));
        QVERIFY(snapshotJson.contains(QStringLiteral("commitments")));
        QVERIFY(!snapshotJson.contains(QStringLiteral("note")));
        QVERIFY(!snapshotJson.contains(QStringLiteral("internal_wake_note")));

        RequestMeta groupMeta;
        groupMeta.channel = Channel::kQQGroup;
        groupMeta.sessionId = QStringLiteral("30001");
        groupMeta.actorId = QStringLiteral("10001");
        groupMeta.eventId = groupMeta.runId = QStringLiteral("snapshot-group");
        const int groupBodiesStart = bodies.size();
        service.askDeepSeek(QStringLiteral("大家好"), {}, groupMeta);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size(), 2, 5000);
        QVERIFY(bodies.size() > groupBodiesStart);
        for (int i = groupBodiesStart; i < bodies.size(); ++i) {
            const auto messages = QJsonDocument::fromJson(bodies.at(i)).object()
                                      .value(QStringLiteral("messages")).toArray();
            for (const auto &value : messages)
                QVERIFY(!value.toObject().value(QStringLiteral("content")).toString()
                             .contains(prefix));
        }
    }

    void privateBackgroundIsAvailableButReplyStaysPublic_data()
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

    void privateBackgroundIsAvailableButReplyStaysPublic()
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
            {"kind","self_report"},{"quote",secret},{"understanding",secret},
            {"semantics",QJsonObject{{"lifecycle",status=="ended" ? "ended" : "unknown"},
                {"certainty",status=="tentative" ? "tentative" : "confirmed"},
                {"temporal_scope","unknown"},{"completion_condition",""},{"time_expression",""}}}};
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
                    : step==1 ? update
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
        QCOMPARE(bodies.size(),2+int(needsHistory));
        QCOMPARE(replies.at(0).at(2).toString(), followup);
        QCOMPARE(replies.at(0).at(3).value<RequestMeta>().runId, meta.runId);
        QCOMPARE(replies.at(0).at(3).value<RequestMeta>().sessionId, meta.sessionId);
        QVERIFY(bodies.last().contains(secret.toUtf8()));
        const auto requestMessages=QJsonDocument::fromJson(bodies.last()).object()["messages"].toArray();
        QCOMPARE(requestMessages.last().toObject()["content"].toString(),expressionOutputContract());
        int contracts=0;
        for(const auto &message:requestMessages)
            contracts+=message.toObject()["content"].toString()==expressionOutputContract();
        QCOMPARE(contracts,1);
        QVERIFY(bodies.last().contains(QString("按听众决定如何表达").toUtf8()));
        service.askDeepSeek("我刚才说了什么",{},RequestMeta{});
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(),2,5000);
        QCOMPARE(bodies.size(),3+int(needsHistory));
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

    void groupExpressionReceivesAttributedRevisions()
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
        QCOMPARE(bodies.size(),1);
        QVERIFY(bodies.last().contains("revision/"));
        QVERIFY(bodies.last().contains("self_report"));
        QVERIFY(bodies.last().contains("inference"));
        QVERIFY(bodies.last().contains(original.toUtf8()));
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

    void coordinatorArbitratesWritingAndIncomingTurn_data()
    {
        QTest::addColumn<bool>("interrupt");
        QTest::addColumn<bool>("reflection");
        QTest::newRow("private-writing-with-contact-disabled") << false << false;
        QTest::newRow("message-during-writing-request") << true << false;
        QTest::newRow("idle-reflection-without-writing-or-contact") << false << true;
    }

    void privateContactCanOnlyBeHandedOffOnce()
    {
        auto &cfg = ConfigManager::instance();
        const QString path = cfg.getConfigDirPath() + "/identity.json";
        QFile old(path);
        const bool existed = old.open(QIODevice::ReadOnly);
        const auto original = old.readAll();
        old.close();
        const auto restore = qScopeGuard([&] {
            if (existed) cfg.writeConfigDocument("identity.json",original,nullptr);
            else { QFile::remove(path); cfg.reloadIdentity(); }
        });
        QVERIFY(cfg.writeConfigDocument("identity.json",
            R"({"proactive":{"private_writing":true,"enabled":true,"targets":[{"channel":"pet","session_id":"default"}]}})",nullptr));
        QTemporaryDir dir;
        {
            MemoryManager before(dir.filePath("contact.db"));
            RequestMeta request;
            request.eventId=request.runId="restart-source";
            const auto id=before.recordInput("a later task",request,false);
            const auto now=QDateTime::currentDateTimeUtc();
            QJsonObject entry{{"at",now.addSecs(60).toString(Qt::ISODate)},
                {"until",now.addSecs(120).toString(Qt::ISODate)},{"message","resume after restart"}};
            QVERIFY(before.applyReminders(id,request,{{"add",QJsonArray{entry}}}));
        }
        MemoryManager db(dir.filePath("contact.db"));
        RequestMeta meta;
        meta.eventId = meta.runId = "conversation";
        const auto turn = db.recordInput("our unfinished conversation",meta,true);
        const auto restored=db.pendingReminders(meta);
        QCOMPARE(restored.size(),1);
        QVERIFY(db.applyReminders(turn,meta,{{"cancel",QJsonArray{restored.first().toObject()["id"]}}}));
        const auto now=QDateTime::currentDateTimeUtc();
        const auto entry=[&](int offset) {
            return QJsonObject{{"at",now.addSecs(offset).toString(Qt::ISODate)},
                {"until",now.addSecs(3600).toString(Qt::ISODate)},{"message","please check in"}};
        };
        QVERIFY(!db.applyReminders(turn,meta,{{"add",QJsonArray{entry(60),entry(-60)}}}));
        QVERIFY(db.pendingReminders(meta).isEmpty());
        QVERIFY(db.applyReminders(turn,meta,{{"add",QJsonArray{entry(60),entry(180)}}}));
        const auto planned=db.pendingReminders(meta);
        QCOMPARE(planned.size(),2);
        QVERIFY(db.claimDueReminder("pet","default").isEmpty());
        QVERIFY(db.applyReminders(turn,meta,{{"cancel",QJsonArray{planned.first().toObject()["id"]}}}));
        QSqlQuery clock(QSqlDatabase::database("memory_db_connection"));
        QVERIFY(clock.exec("UPDATE reminder SET due=unixepoch()-1 WHERE state='pending'"));
        const auto due=db.claimDueReminder("pet","default");
        QCOMPARE(due["id"],planned.last().toObject()["id"]);
        QVERIFY(db.claimDueReminder("pet","default").isEmpty());
        db.reminderOutcome(due["id"].toInteger(),"handed_off");
        QVERIFY(db.recordQuiet(turn,"close"));
        const auto reflection=db.claimPrivateReflection(2,60);
        QCOMPARE(reflection["id"].toInteger(),turn);
        QCOMPARE(reflection["turn_outcome"].toString(),QString("quiet"));
        const auto actions=reflection["source_reminders"].toArray();
        QCOMPARE(actions.size(),2);
        QStringList outcomes;
        for (const auto &action : actions) outcomes.append(action.toObject()["state"].toString());
        QVERIFY(outcomes.contains("cancelled"));
        QVERIFY(outcomes.contains("handed_off"));
        // contact now 必须带可核验依据：v28 起时间委托只由提醒链执行，回顾不能拿提醒当依据；
        // 这里用"关注之后主人真实说过的一句话"（new_owner_evidence）。
        RequestMeta evidenceMeta;
        evidenceMeta.eventId = evidenceMeta.runId = QStringLiteral("owner-evidence-after-contact");
        evidenceMeta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(3600);
        const qlonglong evidenceTurn = db.recordInput("later owner words", evidenceMeta, true);
        QVERIFY(evidenceTurn > 0);
        QVERIFY(db.finishPrivateReflection(turn,1,"consider","own question",
            {{"timing","now"},{"reason","follow up"},{"message","shall we continue?"},
             {"basis",QJsonObject{{"type","new_owner_evidence"},
                                  {"refs",QJsonArray{QStringLiteral("chat/%1").arg(evidenceTurn)}}}}}));
        QVERIFY(db.claimReflectionContact(turn,1,"qq_group","other").isEmpty());
        QCOMPARE(db.claimReflectionContact(turn,1,"pet","default"),QString("shall we continue?"));
        QVERIFY(db.claimReflectionContact(turn,1,"pet","default").isEmpty());
        QVERIFY(db.recordContactOutcome(turn,1,"handed_off"));
        {
            // 依据字段与联系意图同一行落库，供审计"凭什么现在联系"。
            QSqlQuery basisQuery(QSqlDatabase::database("memory_db_connection"));
            basisQuery.prepare("SELECT basis_type,basis_refs FROM reflection_contact WHERE turn_id=?");
            basisQuery.addBindValue(turn);
            QVERIFY(basisQuery.exec() && basisQuery.next());
            QCOMPARE(basisQuery.value(0).toString(),QString("new_owner_evidence"));
            QVERIFY(basisQuery.value(1).toString().contains(QStringLiteral("chat/%1").arg(evidenceTurn)));
        }
        QCOMPARE(db.recentPrivateReflection(meta)["handed_off_message"].toString(),QString("shall we continue?"));
        QVERIFY(db.recordContactOutcome(turn,1,"unknown"));
        QVERIFY(!db.recordContactOutcome(turn,1,"protocol_accepted"));
        QVERIFY(!db.recentPrivateReflection(meta).contains("handed_off_message"));
        QVERIFY(db.deleteTurnByID(turn));
        QVERIFY(db.recentPrivateReflection(meta).isEmpty());
    }

    void truncatedObservationReleasesQueue_data()
    {
        QTest::addColumn<bool>("persistent");
        QTest::newRow("retry-with-output-budget")<<false;
        QTest::newRow("retain-failure-and-advance")<<true;
    }

    void truncatedObservationReleasesQueue()
    {
        QFETCH(bool,persistent);
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("observation.db"));
        RequestMeta meta;
        meta.eventId=meta.runId="first-observation";
        meta.receivedAtUtc=QDateTime::currentDateTimeUtc();
        QVERIFY(db.enqueueObservation(meta,"first experience"));
        meta.eventId=meta.runId="second-observation";
        QVERIFY(db.enqueueObservation(meta,"later experience"));
        const auto source=db.pendingObservation()["source"].toInteger();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        const int failures=persistent ? 4 : 1;
        serveCompletions(server,bodies,[](int){return QString(R"({"outcome":"no_change","changes":[]})");},
            [failures](int round){return round<=failures ? QString("length") : QString("stop");});
        LLMService service("test",nullptr,QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())));
        service.setMemoryManager(&db);
        QSqlQuery q(QSqlDatabase::database("memory_db_connection"));
        auto attempts=[&] {
            q.prepare("SELECT attempts FROM cognitive_source WHERE id=?");
            q.addBindValue(source);
            const int count=q.exec() && q.next() ? q.value(0).toInt() : -1;
            q.finish();
            return count;
        };
        for (int round=1;round<=failures;++round) {
            service.processObservation();
            QTRY_COMPARE_WITH_TIMEOUT(attempts(),round,5000);
            QVERIFY(q.exec("UPDATE cognitive_source SET due=0"));
        }
        service.processObservation();
        auto completed=[&] {
            q.exec("SELECT count(*) FROM cognitive_source WHERE outcome IS NOT NULL");
            const int count=q.next() ? q.value(0).toInt() : 0;
            q.finish();
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(completed(),2,5000);
        QCOMPARE(bodies.size(),persistent ? 5 : 3);
        QVERIFY(!QJsonDocument::fromJson(bodies.first()).object().contains("thinking"));
        QCOMPARE(QJsonDocument::fromJson(bodies[1]).object()["thinking"].toObject()["type"].toString(),QString("disabled"));
        QVERIFY(!QJsonDocument::fromJson(bodies.last()).object().contains("thinking"));
        QVERIFY(q.exec("SELECT outcome FROM cognitive_source ORDER BY id"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(),persistent ? QString("review_required") : QString("no_change"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(),QString("no_change"));
    }

    void visionEntersOnlyItsCurrentEvent_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto name : {"pet","qq","qq_sticker","unreadable","untrusted","disabled","filtered","malformed"})
            QTest::newRow(name)<<QString(name);
    }

    void visionEntersOnlyItsCurrentEvent()
    {
        QFETCH(QString,mode);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto base=QString("http://127.0.0.1:%1").arg(server.serverPort());
        auto &cfg=ConfigManager::instance();
        QFile config(cfg.getConfigFilePath());
        const bool existed=config.open(QIODevice::ReadOnly);
        const auto original=existed ? config.readAll() : QByteArray{};
        config.close();
        const auto restore=qScopeGuard([&] {
            cfg.writeConfigDocument("setting.json",existed ? original : QByteArray("{}"),nullptr);
            if (!existed) QFile::remove(config.fileName());
        });
        const QJsonObject api{{"dashscope_base_url",base},{"dashscope_api_key","vision-test"},{"vision_enabled",mode!="disabled"}};
        QVERIFY(cfg.writeConfigDocument("setting.json",QJsonDocument(QJsonObject{{"api",api}}).toJson(),nullptr));
        QTemporaryDir dir;
        const auto path=dir.filePath("private-local-file.png");
        QImage image(32,32,QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(path));
        const auto expected=mode=="pet" || mode=="qq" || mode=="qq_sticker" || mode=="malformed";
        QList<QByteArray> bodies;
        serveCompletions(server,bodies,[&](int n) -> QString {
            if (expected && n==1) return mode=="malformed" ? "not json" :
                R"({"images":[{"id":1,"observation":"synthetic green square"}]})";
            return R"({"conversation_action":"close","sentences":[]})";
        });
        RequestMeta meta;
        meta.eventId=meta.runId="test-image";
        meta.sensitive=mode=="filtered";
        meta.channel=mode.startsWith("qq") || mode=="unreadable" || mode=="untrusted" ? Channel::kQQPrivate : Channel::kPet;
        meta.inputEvents={QJsonObject{{"event","first"},{"text","hello"}},
                          QJsonObject{{"event",meta.eventId},{"text","image caption"},{"image_segments",QJsonArray{2}}}};
        QString locator=path;
        if (mode=="qq") locator="https://multimedia.nt.qq.com.cn/download?rkey=transient";
        if (mode=="qq_sticker") locator="https://gxh.vip.qq.com/item";
        if (mode=="untrusted") locator="https://multimedia.nt.qq.com.cn.example.org/item";
        meta.imageSources[meta.eventId].insert(2,locator);
        LLMService model("chat-test",nullptr,QUrl(base+"/chat"));
        model.askDeepSeek("hello\nimage caption",{},meta);
        QTRY_VERIFY_WITH_TIMEOUT(bodies.size()>=(expected ? 2 : 1),5000);
        const auto chat=bodies[expected ? 1 : 0];
        QVERIFY(!chat.contains(path.toUtf8()));
        QVERIFY(!chat.contains("data:image"));
        QVERIFY(!chat.contains("transient"));
        QCOMPARE(chat.contains("synthetic green square"),mode=="pet" || mode.startsWith("qq"));
        if (expected) {
            const auto frames=QJsonDocument::fromJson(bodies.first()).object()["messages"].toArray().last().toObject()["content"].toArray();
            const auto url=frames.last().toObject()["image_url"].toObject()["url"].toString();
            if (mode.startsWith("qq")) QCOMPARE(url,locator);
            else QVERIFY(url.startsWith("data:image/png;base64,"));
            QVERIFY(bodies.first().contains("image caption"));
            QVERIFY(bodies.first().contains("hello"));
        }
    }

    void coordinatorArbitratesWritingAndIncomingTurn()
    {
        QFETCH(bool, interrupt);
        QFETCH(bool, reflection);
        auto &cfg = ConfigManager::instance();
        const QString path = cfg.getConfigDirPath() + "/identity.json";
        QFile old(path);
        const bool existed = old.open(QIODevice::ReadOnly);
        const auto original = existed ? old.readAll() : QByteArray{};
        old.close();
        const auto restore = qScopeGuard([&] {
            if (existed) cfg.writeConfigDocument("identity.json",original,nullptr);
            else { QFile::remove(path); cfg.reloadIdentity(); }
        });
        QVERIFY(cfg.writeConfigDocument("identity.json",
            R"({"proactive":{"private_writing":true,"enabled":false,"idle_seconds":0}})",nullptr));
        QTemporaryDir dir;
        const auto dbPath = dir.filePath("coordinator-writing.db");
        qint64 earlierTurn=0;
        {
            MemoryManager beforeRestart(dbPath);
            if (reflection) {
                RequestMeta meta;
                meta.eventId=meta.runId="earlier-message";
                meta.receivedAtUtc=QDateTime::currentDateTimeUtc().addSecs(-180);
                earlierTurn=beforeRestart.recordInput("previous street sounds",meta,true);
                QVERIFY(beforeRestart.recordQuiet(earlierTurn,"close"));
                QCOMPARE(beforeRestart.claimPrivateReflection(3,60)["id"].toInteger(),earlierTurn);
                QVERIFY(beforeRestart.finishPrivateReflection(earlierTurn,1,"consider","unanswered question about the street"));
                QSqlQuery clock(QSqlDatabase::database("memory_db_connection"));
                QVERIFY(clock.exec("UPDATE thought_review_log SET started_at=started_at-3600"));
                meta.receivedAtUtc=QDateTime::currentDateTimeUtc().addSecs(-60);
                meta.eventId=meta.runId="ordinary-message";
                const auto turn=beforeRestart.recordInput("the street sounds different today",meta,true);
                QVERIFY(beforeRestart.recordQuiet(turn,"close"));
            } else {
                QVERIFY(beforeRestart.saveQATurn("write", R"({"personal_interest":"quiet rain"})", {}));
                const auto initial = beforeRestart.nextThought(true);
                QVERIFY(beforeRestart.finishThoughtReview(initial["id"].toInteger(),initial["version"].toInt(),
                    {{"status","active"},{"reflection","opening"},{"draft","first line"},{"next_step","add a sound"}}));
                QSqlQuery clock(QSqlDatabase::database("memory_db_connection"));
                QVERIFY(clock.exec("UPDATE chat_history SET review_after=0"));
            }
        }
        MemoryManager db(dbPath);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        LLMService model("test",nullptr,QUrl(QString("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        CognitiveCoordinator coordinator(db,model,nullptr);
        QList<QByteArray> bodies;
        serveCompletions(server,bodies,[&](int count) -> QString {
            if (count == 1) {
                if (reflection) return R"({"choice":"consult","terms":["street"]})";
                if (interrupt) {
                    RequestMeta meta;
                    meta.runId = meta.eventId = "interrupt-writing";
                    meta.persistTurn = false;
                    meta.longTermMemoryEnabled = false;
                    coordinator.submit("下次聊",meta);
                    coordinator.reviewIfDue();
                }
                return R"({"status":"active","reflection":"continued","draft":"first line and a sound"})";
            }
            if (reflection && count==2) return R"({"choice":"consider","note":"I wonder which sound changed.","prior_action":"revise"})";
            return R"({"conversation_action":"close","sentences":[]})";
        });
        coordinator.reviewIfDue();
        coordinator.reviewIfDue();
        QTRY_COMPARE_WITH_TIMEOUT(bodies.size(),interrupt || reflection ? 2 : 1,5000);
        if (reflection) {
            RequestMeta viewer;
            QTRY_COMPARE_WITH_TIMEOUT(db.recentPrivateReflection(viewer)["own_reflection"].toString(),QString("I wonder which sound changed."),5000);
            QVERIFY(bodies.first().contains("unanswered question about the street"));
            QVERIFY(bodies.first().contains("previous street sounds"));
            QVERIFY(bodies.first().contains("the street sounds different today"));
            // 回顾提示词末行带一行能力摘要（联系目标/安静时段/写作开关），不是整段 JSON。
            QVERIFY(bodies.first().contains(QStringLiteral("服务端核验的当前授权").toUtf8()));
            QVERIFY(bodies.first().contains(QStringLiteral("私下写作开关").toUtf8()));
            // R12-0：新提示不再要求"必须有新主人证据/活动结果并写 contact.basis"，
            // 改为共享契约（自己的意愿可以成为动机、外部事实走 claims 核验）+ 服务端时间上下文；
            // legacy basis 的兼容读取仍由 memory 层测试覆盖。
            QVERIFY(!bodies.first().contains(QStringLiteral("必须把依据写进 contact.basis").toUtf8()));
            QVERIFY(!bodies.first().contains(QStringLiteral("有新的主人证据、或活动有了结果").toUtf8()));
            QVERIFY(bodies.first().contains(QStringLiteral("不需要新的主人证据").toUtf8()));
            QVERIFY(bodies.first().contains("claims"));
            QVERIFY(bodies.first().contains("now_local"));
            QVERIFY(bodies.first().contains("last_owner_interaction_at"));
            QVERIFY(bodies.at(1).contains("previous street sounds"));
            QVERIFY(db.nextThought(false).isEmpty());
            QVERIFY(cfg.writeConfigDocument("identity.json",
                R"({"proactive":{"private_writing":false,"enabled":false,"idle_seconds":0}})",nullptr));
            viewer.eventId=viewer.runId="follow-up";
            coordinator.submit("hello",viewer);
            QTRY_VERIFY_WITH_TIMEOUT(bodies.size()>=2,5000);
            QTRY_VERIFY_WITH_TIMEOUT(!coordinator.busy(),5000);
            bool readBack=false;
            for (const auto &body : bodies.mid(1))
                readBack |= body.contains("I wonder which sound changed.");
            QVERIFY(readBack);
            QVERIFY(db.deleteTurnByID(earlierTurn));
            QVERIFY(db.recentPrivateReflection(viewer).isEmpty());
            return;
        }
        QVERIFY(bodies.first().contains("quiet rain"));
        QVERIFY(bodies.first().contains("first line"));
        QVERIFY(bodies.first().contains("add a sound"));
        if (interrupt) {
            QTRY_VERIFY_WITH_TIMEOUT(!coordinator.busy(),5000);
            const auto review = db.nextThought(true)["last_review"].toObject();
            QCOMPARE(review["deferred_reason"].toString(),QString("incoming_message"));
            QCOMPARE(review["draft"].toString(),QString("first line"));
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(db.nextThought(false)["last_review"].toObject()["draft"].toString(),
                                      QString("first line and a sound"),5000);
        }
    }

    void coordinatorCompletesBeforeAdvancing_data()
    {
        QTest::addColumn<bool>("sameStream");
        QTest::addColumn<QString>("secondActor");
        QTest::addColumn<QString>("secondQuote");
        QTest::newRow("same-speaker-burst") << true << QString("123") << QString();
        QTest::newRow("different-speakers") << false << QString("456") << QString();
        QTest::newRow("changed-reply-target") << false << QString("123") << QString("987");
    }

    void coordinatorCompletesBeforeAdvancing()
    {
        QFETCH(bool,sameStream);
        QFETCH(QString,secondActor);
        QFETCH(QString,secondQuote);
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
        meta.channel=Channel::kQQGroup;
        meta.actorId="123";
        meta.longTermMemoryEnabled=false;
        meta.eventId=meta.runId="first";
        meta.inputEvents={QJsonObject{{"event","first"},{"text","下次聊"}}};
        coordinator.submit("下次聊",meta);
        meta.eventId=meta.runId="second";
        meta.actorId=secondActor;
        meta.replyToMessageId=secondQuote;
        meta.inputEvents={QJsonObject{{"event","second"},{"text","还有一件事"}}};
        coordinator.submit("还有一件事",meta);
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.busy(),5000);
        const auto rows=db.getHistoryTurn(0,10);
        QCOMPARE(rows.size(),2);
        QCOMPARE(rows.first().state,sameStream ? QString("quiet") : QString("handed_off"));
        QCOMPARE(rows.last().state,sameStream ? QString("grouped") : QString("quiet"));
        QCOMPARE(bodies.size(),sameStream ? 1 : 2);
        if (sameStream) {
            QCOMPARE(rows.last().detail,QString::number(rows.first().id));
            QCOMPARE(rows.last().userInput,QString("下次聊"));
            QCOMPARE(rows.first().userInput,QString("还有一件事"));
            QVERIFY(bodies.first().contains("下次聊"));
            QVERIFY(bodies.first().contains("还有一件事"));
            const auto messages=QJsonDocument::fromJson(bodies.first()).object()["messages"].toArray();
            QString content;
            for (const auto &message : messages) {
                const auto item=message.toObject();
                if (item["role"]=="user" && item["content"].toString().startsWith("当前输入按接收顺序"))
                    content=item["content"].toString();
            }
            QVERIFY(content.indexOf("下次聊") >= 0);
            QVERIFY(content.indexOf("还有一件事") > content.indexOf("下次聊"));
        }
    }

    // B1：启动时把崩溃遗留的 received 轮次登记为 recovery；reviewIfDue 先恢复它再考虑回顾。
    void reviewIfDueRecoversInterruptedTurn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto dbPath = dir.filePath("coordinator-recovery.db");
        qint64 turn = 0;
        {
            MemoryManager beforeRestart(dbPath);
            RequestMeta meta;
            meta.eventId = meta.runId = QStringLiteral("interrupted-before-restart");
            meta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(-300);
            turn = beforeRestart.recordInput(QStringLiteral("这一轮在崩溃前没得到回复"), meta, true);
            QVERIFY(turn > 0);
            // timestamp 是本地时间；挪到 3 分钟前，模拟上次进程没来得及回写。
            QSqlQuery clock(QSqlDatabase::database("memory_db_connection"));
            QVERIFY(clock.exec(QStringLiteral(
                "UPDATE chat_history SET timestamp=datetime('now','localtime','-180 seconds') WHERE id=%1").arg(turn)));
        }
        MemoryManager db(dbPath);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server,bodies,[](int){ return QStringLiteral(R"({"sentences":[]})"); });
        LLMService model("test",nullptr,QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        CognitiveCoordinator coordinator(db,model,nullptr);
        // 构造函数已把遗留轮次入队为 recovery（最高优先级）。
        QCOMPARE(db.attentionBacklog()["pending"].toObject()["recovery"].toInt(), 1);
        coordinator.reviewIfDue();
        QSqlQuery check(QSqlDatabase::database("memory_db_connection"));
        QVERIFY(check.exec(QStringLiteral(
            "SELECT turn_state,turn_detail FROM chat_history WHERE id=%1").arg(turn)));
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toString(), QStringLiteral("interrupted"));
        QCOMPARE(check.value(1).toString(), QStringLiteral("restart_recovery"));
        // recovery 事件已落终态：pending 里不再有它。
        QCOMPARE(db.attentionBacklog()["pending"].toObject()["recovery"].toInt(), 0);
        QCOMPARE(db.attentionBacklog()["by_state"].toObject()["done"].toInt(), 1);
        // 恢复优先于回顾：本轮没有启动新的回顾请求。
        QCOMPARE(bodies.size(), 0);
    }

    // 能力自感知：清单由当前配置生成，默认未授予的项给出 enabled=false 与简短原因。
    void capabilityManifestReflectsConfig()
    {
        auto &cfg = ConfigManager::instance();
        QFile setting(cfg.getConfigFilePath());
        const bool settingExisted = setting.open(QIODevice::ReadOnly);
        const QByteArray settingOriginal = settingExisted ? setting.readAll() : QByteArray{};
        setting.close();
        auto restoreSetting = qScopeGuard([&] {
            cfg.writeConfigDocument("setting.json",
                settingExisted ? settingOriginal : QByteArray("{}"), nullptr);
            if (!settingExisted) QFile::remove(cfg.getConfigFilePath());
        });
        const QString identityPath = cfg.getConfigDirPath() + "/identity.json";
        QFile identity(identityPath);
        const bool identityExisted = identity.open(QIODevice::ReadOnly);
        const QByteArray identityOriginal = identityExisted ? identity.readAll() : QByteArray{};
        identity.close();
        auto restoreIdentity = qScopeGuard([&] {
            if (identityExisted) cfg.writeConfigDocument("identity.json", identityOriginal, nullptr);
            else { QFile::remove(identityPath); cfg.reloadIdentity(); }
        });

        QVERIFY(cfg.writeConfigDocument("setting.json",
            QJsonDocument(QJsonObject{{"memory", QJsonObject{{"long_term_memory_enabled", true}}}}).toJson(),
            nullptr));
        QVERIFY(cfg.writeConfigDocument("identity.json", QByteArray("{}"), nullptr));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("manifest.db"));
        LLMService service("test", nullptr, QUrl(QStringLiteral("http://127.0.0.1:1")));
        service.setMemoryManager(&db);

        RequestMeta privateMeta; // 桌宠默认会话：pet/default
        QJsonObject manifest = service.capabilityManifest(privateMeta);
        QVERIFY(manifest.value("memory_recall").toObject().value("enabled").toBool());
        const auto defaultContact = manifest.value("private_contact").toObject();
        QVERIFY(!defaultContact.value("enabled").toBool());
        QVERIFY(!defaultContact.value("reason").toString().isEmpty());
        QVERIFY(!manifest.value("private_review").toObject().value("enabled").toBool());
        QVERIFY(!manifest.value("reminder").toObject().value("enabled").toBool());
        QVERIFY(manifest.value("tts").toObject().value("enabled").toBool());

        // 群聊没有私下提醒，即使目标列表覆盖了当前会话也不能列入。
        RequestMeta groupMeta;
        groupMeta.channel = Channel::kQQGroup;
        groupMeta.sessionId = QStringLiteral("30001");
        groupMeta.actorId = QStringLiteral("10001");
        QVERIFY(!service.capabilityManifest(groupMeta).value("reminder")
                    .toObject().value("enabled").toBool());

        // 开启主动联系 + 目标后：联系/提醒/写作真正可用，并带目标与安静时段。
        QVERIFY(cfg.writeConfigDocument("identity.json", QByteArray(
            R"({"proactive":{"enabled":true,"private_writing":true,"idle_seconds":600,)"
            R"("quiet_hours":{"start":"22:30","end":"07:30"},)"
            R"("targets":[{"channel":"pet","session_id":"default"}]}})"), nullptr));
        manifest = service.capabilityManifest(privateMeta);
        const auto contact = manifest.value("private_contact").toObject();
        QVERIFY(contact.value("enabled").toBool());
        QVERIFY(contact.value("targets").toArray().contains(QJsonValue(QStringLiteral("pet"))));
        QCOMPARE(contact.value("quiet_hours").toObject().value("start").toString(),
                 QStringLiteral("22:30"));
        QCOMPARE(contact.value("idle_seconds").toInt(), 600);
        const auto reminder = manifest.value("reminder").toObject();
        QVERIFY(reminder.value("enabled").toBool());
        QCOMPARE(reminder.value("channel").toString(), QStringLiteral("pet"));
        QCOMPARE(reminder.value("quiet_hours").toObject().value("end").toString(),
                 QStringLiteral("07:30"));
        QVERIFY(manifest.value("private_review").toObject().value("enabled").toBool());

        // 关闭长期记忆：memory_recall 不再出现，其余记忆相关能力也收回。
        QVERIFY(cfg.writeConfigDocument("setting.json",
            QJsonDocument(QJsonObject{{"memory", QJsonObject{{"long_term_memory_enabled", false}}}}).toJson(),
            nullptr));
        manifest = service.capabilityManifest(privateMeta);
        QVERIFY(!manifest.contains(QStringLiteral("memory_recall")));
        QVERIFY(!manifest.value("private_contact").toObject().value("enabled").toBool());
        QVERIFY(!manifest.value("private_review").toObject().value("enabled").toBool());
        QVERIFY(!manifest.value("reminder").toObject().value("enabled").toBool());
    }

    // 切片 C：回顾模型给出新契约 contact 时，LLMService 必须先做确定性策略复核，
    // 再做一次语义核验（coverage + entailment）；核心事实声称不可修复 → 整项联系标记 withheld；
    // 纯 self_desire（claims 为空）也要过 coverage 门禁，完整且 entailed 才保留并写 entailed_coverage。
    void privateReviewVerifiesClaims()
    {
        auto &cfg = ConfigManager::instance();
        // setting.json：私下回顾要求长期记忆开启，否则回顾回调直接报 writing_disabled。
        QFile setting(cfg.getConfigFilePath());
        const bool settingExisted = setting.open(QIODevice::ReadOnly);
        const QByteArray settingOriginal = settingExisted ? setting.readAll() : QByteArray{};
        setting.close();
        // identity.json：主动联系目标可达、安静时段置空、冷却 0，避免用例受执行时刻影响。
        const QString identityPath = cfg.getConfigDirPath() + "/identity.json";
        QFile identity(identityPath);
        const bool identityExisted = identity.open(QIODevice::ReadOnly);
        const QByteArray identityOriginal = identityExisted ? identity.readAll() : QByteArray{};
        identity.close();
        const auto restore = qScopeGuard([&] {
            cfg.writeConfigDocument("setting.json", settingExisted ? settingOriginal : QByteArray("{}"), nullptr);
            if (identityExisted) cfg.writeConfigDocument("identity.json", identityOriginal, nullptr);
            else { QFile::remove(identityPath); cfg.reloadIdentity(); }
        });
        QVERIFY(cfg.writeConfigDocument("setting.json",
            QJsonDocument(QJsonObject{{"memory", QJsonObject{{"long_term_memory_enabled", true}}}}).toJson(),
            nullptr));
        QVERIFY(cfg.writeConfigDocument("identity.json", QByteArray(
            R"({"proactive":{"enabled":true,"private_writing":true,"cooldown_seconds":0,)"
            R"("quiet_hours":{"start":"00:00","end":"00:00"},)"
            R"("targets":[{"channel":"pet","session_id":"default"}]},)"
            R"("cognition":{"semantic_verifier":{"enabled":true,"max_verifier_calls":1,"max_repairs":1}}})"),
            nullptr));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("semantic-review.db");
        MemoryManager db(path);
        const auto proactiveTurn = db.recordProactiveMessage(
            Channel::kPet, Channel::kDefaultSession, QStringLiteral("我先睡啦"),
            QStringLiteral("contact"), QStringLiteral("semantic/1"));
        QVERIFY(proactiveTurn > 0);

        // 造一个开放关注（quiet/close 收口之后回顾会看到它）。
        RequestMeta concernMeta;
        concernMeta.channel = Channel::kPet;
        concernMeta.sessionId = Channel::kDefaultSession;
        concernMeta.subject = QStringLiteral("user");
        concernMeta.turnKind = QStringLiteral("reply");
        concernMeta.eventId = concernMeta.runId = QStringLiteral("semantic-concern");
        concernMeta.receivedAtUtc = QDateTime::currentDateTimeUtc();
        const auto turn = db.recordInput(QStringLiteral("明天想去看海"), concernMeta, true);
        QVERIFY(turn > 0);
        QVERIFY(db.recordQuiet(turn, QStringLiteral("close")));
        auto claim = db.claimPrivateReflection(12, 60);
        QCOMPARE(claim.value(QStringLiteral("id")).toInteger(), turn);
        QVERIFY(db.finishPrivateReflection(turn, 1, QStringLiteral("consider"),
                                           QStringLiteral("看海"), QJsonObject{}, QJsonArray{},
                                           QJsonObject{}, QString{}, QJsonObject{}));
        // 关注之后主人的一条真实发言（reported_fact 的合法 ref）。
        RequestMeta evidenceMeta;
        evidenceMeta.channel = Channel::kPet;
        evidenceMeta.sessionId = Channel::kDefaultSession;
        evidenceMeta.subject = QStringLiteral("user");
        evidenceMeta.turnKind = QStringLiteral("reply");
        evidenceMeta.eventId = evidenceMeta.runId = QStringLiteral("semantic-evidence");
        evidenceMeta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(3600);
        const auto evidence = db.recordInput(QStringLiteral("后天应该可以去"), evidenceMeta, true);
        QVERIFY(evidence > 0);

        // 回顾/核验同一个 mock 端点：按请求体里的固定前缀区分三种请求。
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        int reviewRound = 0;
        int verifierCalls = 0;
        serveCompletions(server, bodies, [&](int) -> QString {
            const QByteArray body = bodies.last();
            if (body.contains("【语义核验】")) {
                ++verifierCalls;
                // 新契约：verifier 一次请求同时给出 claim_coverage 与 entailment 结论。
                // 从请求体里解析出 verifier 输入，按 declared_claims 是否为空区分轮次
                // （HTTP body 里输入 JSON 是转义字符串，不能直接做子串匹配）。
                QJsonArray declaredClaims;
                for (const auto &value : QJsonDocument::fromJson(body).object()
                                             .value(QStringLiteral("messages")).toArray()) {
                    const auto message = value.toObject();
                    if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user"))
                        continue;
                    declaredClaims = QJsonDocument::fromJson(
                        message.value(QStringLiteral("content")).toString().toUtf8()).object()
                                         .value(QStringLiteral("declared_claims")).toArray();
                }
                if (!declaredClaims.isEmpty())
                    return R"({"claim_coverage":"complete","missing_claims":[],)"
                           R"("result":"insufficient","supporting_refs":[],"reason_code":"unrelated"})";
                // 纯 self_desire：claims 为空、无外部事实 → coverage 完整且 entailed。
                return R"({"claim_coverage":"complete","missing_claims":[],)"
                       R"("result":"entailed","supporting_refs":[],"reason_code":"pure_self_desire"})";
            }
            if (body.contains("【约束修复】"))
                return R"({"contact":{"reason":"想问问","message":"后来去成了吗","claims":[]}})";
            ++reviewRound;
            if (reviewRound == 1) {
                // 编造的承诺：refs 指向主动消息（assistant 表达），确定性校验必须先拒绝。
                return QString(R"({"choice":"consider","concern_action":"contact","note":"想确认约定",)"
                               R"("contact":{"timing":"now","reason":"想起你答应过的事",)"
                               R"("message":"你答应过今晚早点睡吗",)"
                               R"("motivation":{"type":"self_desire","summary":"想确认一下"},)"
                               R"("claims":[{"id":"c1","type":"user_commitment",)"
                               R"("text":"主人答应过今晚早点睡","refs":["chat/%1"]}],)"
                               R"("core_claim_ids":["c1"]}})").arg(proactiveTurn);
            }
            if (reviewRound == 2) {
                // 真实存在但无关的资料：确定性通过，语义核验判 insufficient，动作核心 → withheld。
                return QString(R"({"choice":"consider","concern_action":"contact","note":"想聊聊看海",)"
                               R"("contact":{"timing":"now","reason":"想起看海的事",)"
                               R"("message":"你后来去成海边了吗",)"
                               R"("motivation":{"type":"self_desire","summary":"想接着聊"},)"
                               R"("claims":[{"id":"c1","type":"reported_fact",)"
                               R"("text":"主人说后天可以去","refs":["chat/%1"]}],)"
                               R"("core_claim_ids":["c1"]}})").arg(evidence);
            }
            // 纯 self_desire：claims 为空也要过 coverage 门禁（verifier 返回 complete/entailed）。
            return QString(R"({"choice":"consider","concern_action":"contact","note":"想问候",)"
                           R"("contact":{"timing":"now","reason":"自己想到",)"
                           R"("message":"在忙吗",)"
                           R"("motivation":{"type":"self_desire","summary":"想问候主人"},)"
                           R"("claims":[]}})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        QSignalSpy finished(&model, &LLMService::privateReviewFinished);

        // 每轮：把关注恢复为到期并重新领取（版本 +1），回顾冷却往前挪。
        const auto nextReview = [&](int version) -> QJsonObject {
            RawSqlConnection raw(path, QStringLiteral("semantic-due-%1").arg(version));
            if (!raw.open()) return {};
            if (!raw.exec(QStringLiteral("UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                                         "WHERE turn_id=%1").arg(turn))) return {};
            if (!raw.exec(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120"))) return {};
            return db.claimDueConcern(12, 60);
        };

        // 第 1 轮：assistant 轮 ref 的 user_commitment → 确定性拒绝，不消耗 verifier。
        QJsonObject context = nextReview(1);
        QVERIFY(!context.isEmpty());
        const int versionA = context.value(QStringLiteral("version")).toInt();
        model.requestPrivateReview(turn, versionA, context);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        auto proposal = finished.takeFirst().at(2).toJsonObject();
        auto contact = proposal.value(QStringLiteral("contact")).toObject();
        QCOMPARE(contact.value(QStringLiteral("semantic_status")).toString(), QStringLiteral("withheld"));
        QVERIFY(contact.value(QStringLiteral("withhold_reason")).toString()
                    .startsWith(QStringLiteral("deterministic:")));
        QCOMPARE(verifierCalls, 0);
        QVERIFY(db.finishConcernReview(turn, versionA, proposal));
        {
            QSqlQuery row(QSqlDatabase::database("memory_db_connection"));
            row.prepare("SELECT outcome,semantic_status,coalesce(claims_json,''),coalesce(motivation_json,'') "
                        "FROM reflection_contact WHERE turn_id=?");
            row.addBindValue(turn);
            QVERIFY(row.exec() && row.next());
            QCOMPARE(row.value(0).toString(), QStringLiteral("withheld"));
            QCOMPARE(row.value(1).toString(), QStringLiteral("withheld"));
            QVERIFY(!row.value(2).toString().isEmpty());
            QVERIFY(!row.value(3).toString().isEmpty());
        }

        // 第 2 轮：真实但无关的 reported_fact → verifier 判 insufficient，核心 → withheld。
        context = nextReview(2);
        QVERIFY(!context.isEmpty());
        const int versionB = context.value(QStringLiteral("version")).toInt();
        model.requestPrivateReview(turn, versionB, context);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        proposal = finished.takeFirst().at(2).toJsonObject();
        contact = proposal.value(QStringLiteral("contact")).toObject();
        QCOMPARE(verifierCalls, 1);
        QCOMPARE(contact.value(QStringLiteral("semantic_status")).toString(), QStringLiteral("withheld"));
        QCOMPARE(contact.value(QStringLiteral("withhold_reason")).toString(),
                 QStringLiteral("verifier_insufficient"));
        QVERIFY(db.finishConcernReview(turn, versionB, proposal));
        {
            QSqlQuery row(QSqlDatabase::database("memory_db_connection"));
            row.prepare("SELECT outcome,semantic_status FROM reflection_contact WHERE turn_id=?");
            row.addBindValue(turn);
            QVERIFY(row.exec() && row.next());
            QCOMPARE(row.value(0).toString(), QStringLiteral("withheld"));
            QCOMPARE(row.value(1).toString(), QStringLiteral("withheld"));
        }

        // 第 3 轮：纯 self_desire 且 claims 为空 → 仍必须走一次 verifier（coverage=complete/entailed），
        // 联系保留为 proposed，语义结论写新值 entailed_coverage。
        context = nextReview(3);
        QVERIFY(!context.isEmpty());
        const int versionC = context.value(QStringLiteral("version")).toInt();
        model.requestPrivateReview(turn, versionC, context);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        proposal = finished.takeFirst().at(2).toJsonObject();
        contact = proposal.value(QStringLiteral("contact")).toObject();
        QCOMPARE(verifierCalls, 2); // 第 2 轮（有声明事实）+ 第 3 轮（claims 为空）各一次
        QCOMPARE(contact.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("entailed_coverage"));
        QVERIFY(db.finishConcernReview(turn, versionC, proposal));
        QVERIFY(db.hasProposedContact(turn, versionC));
        {
            QSqlQuery row(QSqlDatabase::database("memory_db_connection"));
            row.prepare("SELECT outcome,semantic_status FROM reflection_contact WHERE turn_id=?");
            row.addBindValue(turn);
            QVERIFY(row.exec() && row.next());
            QCOMPARE(row.value(0).toString(), QStringLiteral("proposed"));
            QCOMPARE(row.value(1).toString(), QStringLiteral("entailed_coverage"));
        }
        QCOMPARE(reviewRound, 3);
    }

    // P0-1：模型回顾 claims=[] 却把主人承诺写进 message。单次 verifier 同时做 coverage：
    // 判 claim_coverage=incomplete → 整项 withhold（claim_coverage_incomplete，不 repair），
    // 记忆层最终关注仍 consider、没有 proposed contact。
    void coverageGateWithholdsUndeclaredCommitment()
    {
        SemanticReviewConfigGuard config;
        QVERIFY(config.enable());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("coverage-incomplete.db");
        MemoryManager db(path);
        int version = 0;
        const qlonglong turn = seedDueConcernForSemanticReview(
            db, path, QStringLiteral("coverage-incomplete"), &version);
        QVERIFY(turn > 0);
        QVERIFY(version > 0);

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        int verifierCalls = 0;
        serveCompletions(server, bodies, [&](int) -> QString {
            const QByteArray body = bodies.last();
            if (body.contains("【语义核验】")) {
                ++verifierCalls;
                // 独立判定：message 里的"你答应过我要去睡觉"没有任何 declared_claim 覆盖。
                return R"({"claim_coverage":"incomplete",)"
                       R"("missing_claims":[{"type":"user_commitment","text":"主人答应过要去睡觉"}],)"
                       R"("result":"insufficient","supporting_refs":[],)"
                       R"("reason_code":"undeclared_commitment"})";
            }
            return QString(R"({"choice":"consider","concern_action":"contact","note":"想提醒睡觉",)"
                           R"("contact":{"timing":"now","reason":"到点了",)"
                           R"("message":"半个小时到了，你答应过我要去睡觉",)"
                           R"("motivation":{"type":"self_desire","summary":"想提醒主人休息"},)"
                           R"("claims":[]}})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        QSignalSpy finished(&model, &LLMService::privateReviewFinished);

        model.requestPrivateReview(turn, version,
                                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("concern")}});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        const auto proposal = finished.takeFirst().at(2).toJsonObject();
        const auto contact = proposal.value(QStringLiteral("contact")).toObject();
        QCOMPARE(contact.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("withheld"));
        QCOMPARE(contact.value(QStringLiteral("withhold_reason")).toString(),
                 QStringLiteral("claim_coverage_incomplete"));
        QCOMPARE(verifierCalls, 1); // 单次 verifier，不做 repair
        // verifier 输入契约：message/reason/declared_claims/sources 同时给到，不能只喂声明。
        QJsonObject verifierInput;
        bool sawVerifierInput = false;
        for (const auto &body : bodies) {
            if (!body.contains("【语义核验】")) continue;
            sawVerifierInput = true;
            for (const auto &value : QJsonDocument::fromJson(body).object()
                                         .value(QStringLiteral("messages")).toArray()) {
                const auto message = value.toObject();
                if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user")) continue;
                verifierInput = QJsonDocument::fromJson(
                    message.value(QStringLiteral("content")).toString().toUtf8()).object();
            }
        }
        QVERIFY(sawVerifierInput);
        QCOMPARE(verifierInput.value(QStringLiteral("message")).toString(),
                 QStringLiteral("半个小时到了，你答应过我要去睡觉"));
        QCOMPARE(verifierInput.value(QStringLiteral("reason")).toString(), QStringLiteral("到点了"));
        QCOMPARE(verifierInput.value(QStringLiteral("declared_claims")).toArray().size(), 0);
        QCOMPARE(verifierInput.value(QStringLiteral("sources")).toArray().size(), 0);
        QVERIFY(verifierInput.value(QStringLiteral("rules")).toString()
                    .contains(QStringLiteral("覆盖")));

        // 记忆层路径：关注放回 consider（不给 proposed），审计行 outcome/semantic_status=withheld。
        QVERIFY(db.finishConcernReview(turn, version, proposal));
        QVERIFY(!db.hasProposedContact(turn, version));
        {
            QSqlQuery row(QSqlDatabase::database("memory_db_connection"));
            row.prepare("SELECT state FROM private_reflection WHERE turn_id=?");
            row.addBindValue(turn);
            QVERIFY(row.exec() && row.next());
            QCOMPARE(row.value(0).toString(), QStringLiteral("consider"));
        }
        {
            QSqlQuery row(QSqlDatabase::database("memory_db_connection"));
            row.prepare("SELECT outcome,semantic_status FROM reflection_contact "
                        "WHERE turn_id=? AND version=?");
            row.addBindValue(turn);
            row.addBindValue(version);
            QVERIFY(row.exec() && row.next());
            QCOMPARE(row.value(0).toString(), QStringLiteral("withheld"));
            QCOMPARE(row.value(1).toString(), QStringLiteral("withheld"));
        }
    }

    // P0-2：纯感受/问候（claims=[] 且 message 无外部事实）→ verifier 判
    // claim_coverage=complete 且 result=entailed；联系保留为 proposed，语义状态=entailed_coverage。
    void coverageGateAllowsPureGreeting()
    {
        SemanticReviewConfigGuard config;
        QVERIFY(config.enable());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("coverage-greeting.db");
        MemoryManager db(path);
        int version = 0;
        const qlonglong turn = seedDueConcernForSemanticReview(
            db, path, QStringLiteral("coverage-greeting"), &version);
        QVERIFY(turn > 0);
        QVERIFY(version > 0);

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        int verifierCalls = 0;
        serveCompletions(server, bodies, [&](int) -> QString {
            const QByteArray body = bodies.last();
            if (body.contains("【语义核验】")) {
                ++verifierCalls;
                return R"({"claim_coverage":"complete","missing_claims":[],)"
                       R"("result":"entailed","supporting_refs":[],"reason_code":"pure_greeting"})";
            }
            return QString(R"({"choice":"consider","concern_action":"contact","note":"想问问好",)"
                           R"("contact":{"timing":"now","reason":"突然想到",)"
                           R"("message":"突然想你了，来看看你",)"
                           R"("motivation":{"type":"self_desire","summary":"想和主人说说话"},)"
                           R"("claims":[]}})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        QSignalSpy finished(&model, &LLMService::privateReviewFinished);

        model.requestPrivateReview(turn, version,
                                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("concern")}});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        const auto proposal = finished.takeFirst().at(2).toJsonObject();
        const auto contact = proposal.value(QStringLiteral("contact")).toObject();
        QCOMPARE(verifierCalls, 1); // claims 为空也必须过一次 coverage 核验
        QCOMPARE(contact.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("entailed_coverage"));
        QVERIFY(db.finishConcernReview(turn, version, proposal));
        QVERIFY(db.hasProposedContact(turn, version));
        {
            QSqlQuery row(QSqlDatabase::database("memory_db_connection"));
            row.prepare("SELECT outcome,semantic_status FROM reflection_contact "
                        "WHERE turn_id=? AND version=?");
            row.addBindValue(turn);
            row.addBindValue(version);
            QVERIFY(row.exec() && row.next());
            QCOMPARE(row.value(0).toString(), QStringLiteral("proposed"));
            QCOMPARE(row.value(1).toString(), QStringLiteral("entailed_coverage"));
        }
    }

    // 能力清单应包含"当前正在做的事"：来自 attention_state + 活动快照。
    void capabilityManifestIncludesCurrentActivity()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("manifest-activity.db");
        MemoryManager db(path);
        LLMService service("test", nullptr, QUrl(QStringLiteral("http://127.0.0.1:1")));
        service.setMemoryManager(&db);
        RequestMeta meta;
        meta.eventId = meta.runId = QStringLiteral("manifest-activity");
        meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
        const qint64 turn = db.recordInput(QStringLiteral("我想继续写故事"), meta, true);
        QVERIFY(turn > 0);
        {
            RawSqlConnection raw(path, QStringLiteral("manifest-activity"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                        "UPDATE chat_history SET thought='写故事',thought_result=? WHERE id=%1").arg(turn),
                    QStringLiteral(R"({"status":"active","progress":"写到第二段","next_step":"继续写下去"})")));
        }
        db.setAttentionState(QStringLiteral("current_activity"), QStringLiteral("thought/%1").arg(turn));
        const auto manifest = service.capabilityManifest(meta);
        QVERIFY(manifest.contains(QStringLiteral("current_activity")));
        const auto activity = manifest.value(QStringLiteral("current_activity")).toObject();
        QCOMPARE(activity.value(QStringLiteral("ref")).toString(),
                 QStringLiteral("thought/%1").arg(turn));
        QCOMPARE(activity.value(QStringLiteral("progress")).toString(), QStringLiteral("写到第二段"));
        QCOMPARE(activity.value(QStringLiteral("next_step")).toString(), QStringLiteral("继续写下去"));
    }

    // 前台请求体必须带能力清单，且清单随配置变化；旧断言只做 contains，不依赖顺序。
    void foregroundPromptCarriesCapabilityManifest()
    {
        auto &cfg = ConfigManager::instance();
        QFile setting(cfg.getConfigFilePath());
        const bool settingExisted = setting.open(QIODevice::ReadOnly);
        const QByteArray settingOriginal = settingExisted ? setting.readAll() : QByteArray{};
        setting.close();
        auto restoreSetting = qScopeGuard([&] {
            cfg.writeConfigDocument("setting.json",
                settingExisted ? settingOriginal : QByteArray("{}"), nullptr);
            if (!settingExisted) QFile::remove(cfg.getConfigFilePath());
        });
        const QString identityPath = cfg.getConfigDirPath() + "/identity.json";
        QFile identity(identityPath);
        const bool identityExisted = identity.open(QIODevice::ReadOnly);
        const QByteArray identityOriginal = identityExisted ? identity.readAll() : QByteArray{};
        identity.close();
        auto restoreIdentity = qScopeGuard([&] {
            if (identityExisted) cfg.writeConfigDocument("identity.json", identityOriginal, nullptr);
            else { QFile::remove(identityPath); cfg.reloadIdentity(); }
        });

        QVERIFY(cfg.writeConfigDocument("setting.json",
            QJsonDocument(QJsonObject{{"memory", QJsonObject{{"long_term_memory_enabled", true}}}}).toJson(),
            nullptr));
        QVERIFY(cfg.writeConfigDocument("identity.json", QByteArray(
            R"({"proactive":{"enabled":true,"private_writing":true,)"
            R"("targets":[{"channel":"pet","session_id":"default"}]}})"), nullptr));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [](int) -> QString {
            return QStringLiteral(
                R"({"sentences":[{"zh_text":"好呀","ja_text":"いいよ","tags":{"emotion":"happy"}}]})");
        });
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("capability.db"));
        LLMService service("test", nullptr,
            QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        service.setMemoryManager(&db);
        QSignalSpy spoken(&service, &LLMService::sentencesReady);

        RequestMeta meta; // pet/default，subject=user，persistTurn=true
        meta.runId = meta.eventId = QStringLiteral("capability");
        service.askDeepSeek(QStringLiteral("你好"), {}, meta);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size(), 1, 5000);
        QCOMPARE(bodies.size(), 1);
        QVERIFY(bodies.last().contains(QStringLiteral("【你当前被授予的能力").toUtf8()));
        const QString manifestText = capabilityPromptText(bodies.last());
        QVERIFY(!manifestText.isEmpty());
        const auto manifest = QJsonDocument::fromJson(manifestText.toUtf8()).object();
        QVERIFY(manifest.value("memory_recall").toObject().value("enabled").toBool());
        QVERIFY(manifest.value("private_contact").toObject().value("enabled").toBool());
        QVERIFY(manifest.value("private_contact").toObject().value("targets").toArray()
                    .contains(QJsonValue(QStringLiteral("pet"))));
        QVERIFY(manifest.value("reminder").toObject().value("enabled").toBool());

        // 关闭长期记忆后，下一轮请求的清单不再列 memory_recall。
        QVERIFY(cfg.writeConfigDocument("setting.json",
            QJsonDocument(QJsonObject{{"memory", QJsonObject{{"long_term_memory_enabled", false}}}}).toJson(),
            nullptr));
        meta.runId = meta.eventId = QStringLiteral("capability-disabled");
        service.askDeepSeek(QStringLiteral("还记得吗"), {}, meta);
        QTRY_COMPARE_WITH_TIMEOUT(spoken.size(), 2, 5000);
        QCOMPARE(bodies.size(), 2);
        const QString disabledText = capabilityPromptText(bodies.last());
        QVERIFY(!disabledText.isEmpty());
        const auto disabledManifest = QJsonDocument::fromJson(disabledText.toUtf8()).object();
        QVERIFY(!disabledManifest.contains(QStringLiteral("memory_recall")));
    }

    // R12-0：backlog 只在 pending 非空或最老 pending>0 时 INFO；仅 by_state.done 时 DEBUG。
    void backlogLoggedOnlyWhenPending()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("backlog-log.db"));
        // 先造一条已完成事件：pending 为空、by_state.done 非空，属于正常终态。
        const auto doneEvent = db.enqueueAttentionEvent(
            QStringLiteral("message"), QStringLiteral("backlog-done"), {}, QString(), 50);
        QVERIFY(doneEvent > 0);
        QVERIFY(db.completeAttentionEvent(doneEvent, QStringLiteral("done"),
                                          QStringLiteral("test")));
        QVERIFY(db.attentionBacklog().value(QStringLiteral("pending")).toObject().isEmpty());
        QVERIFY(!db.attentionBacklog().value(QStringLiteral("by_state")).toObject().isEmpty());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        CognitiveCoordinator coordinator(db, model, nullptr);
        {
            MessageCapture capture;
            coordinator.reviewIfDue();
            int backlogLines = 0;
            int backlogInfo = 0;
            for (const auto &line : capture.lines()) {
                if (!line.message.contains(QStringLiteral("[Attention] backlog="))) continue;
                ++backlogLines;
                if (line.type == QtInfoMsg) ++backlogInfo;
            }
            QVERIFY(backlogInfo == 0);
            // qDebug 若被编译掉（QT_NO_DEBUG_OUTPUT），只校验"不是 INFO"。
            if (backlogLines > 0) QCOMPARE(backlogLines, 1);
        }
        // 有 pending 事件时，backlog 行必须升为 INFO。
        QVERIFY(db.enqueueAttentionEvent(QStringLiteral("message"),
                                         QStringLiteral("backlog-pending"), {}, QString(), 50) > 0);
        {
            MessageCapture capture;
            coordinator.reviewIfDue();
            int backlogLines = 0;
            int backlogInfo = 0;
            for (const auto &line : capture.lines()) {
                if (!line.message.contains(QStringLiteral("[Attention] backlog="))) continue;
                ++backlogLines;
                if (line.type == QtInfoMsg) ++backlogInfo;
            }
            QCOMPARE(backlogLines, 1);
            QVERIFY(backlogInfo >= 1);
        }
    }

    // R12-0：reflect 新的回顾请求体使用共享契约与时间上下文，不再强制 legacy contact.basis。
    void reviewPromptSharedContract()
    {
        SemanticReviewConfigGuard guard;
        QVERIFY(guard.enable());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("review-contract.db"));
        RequestMeta meta;
        meta.channel = Channel::kPet;
        meta.sessionId = Channel::kDefaultSession;
        meta.subject = QStringLiteral("user");
        meta.eventId = meta.runId = QStringLiteral("review-contract-source");
        meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
        const auto turn = db.recordInput(QStringLiteral("今天整理了一下书架"), meta, true);
        QVERIFY(turn > 0);
        QVERIFY(db.recordQuiet(turn, QStringLiteral("close")));
        const auto context = db.claimPrivateReflection(12, 60);
        QCOMPARE(context.value(QStringLiteral("id")).toInteger(), turn);
        QCOMPARE(context.value(QStringLiteral("kind")).toString(), QStringLiteral("reflection"));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [](int) -> QString {
            return QStringLiteral(R"({"choice":"rest"})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        QSignalSpy finished(&model, &LLMService::privateReviewFinished);
        const int version = context.value(QStringLiteral("version")).toInt();
        model.requestPrivateReview(turn, version, context);
        QTRY_COMPARE_WITH_TIMEOUT(bodies.size(), 1, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 8000);
        const QByteArray body = bodies.first();
        // 共享规则：自己的意愿可成为动机、外部事实必须走 claims、询问不施压。
        QVERIFY(body.contains(QStringLiteral("不需要新的主人证据").toUtf8()));
        QVERIFY(body.contains(QStringLiteral("claims").toUtf8()));
        QVERIFY(body.contains(QStringLiteral("不要施压").toUtf8()));
        // 服务端时间上下文：R12-1a 新增当前会话/跨渠道两组时间字段，
        // 旧字段 last_owner_interaction_at / last_confirmed_outbound_at 保留兼容。
        for (const auto &field : {QStringLiteral("now_local"), QStringLiteral("now_utc"),
                                  QStringLiteral("source_reported_at"),
                                  QStringLiteral("last_owner_session_at"),
                                  QStringLiteral("last_owner_cross_channel_at"),
                                  QStringLiteral("last_confirmed_outbound_session_at"),
                                  QStringLiteral("last_confirmed_outbound_cross_channel_at"),
                                  QStringLiteral("last_owner_interaction_at"),
                                  QStringLiteral("last_confirmed_outbound_at"),
                                  QStringLiteral("quiet_hours")})
            QVERIFY2(body.contains(field.toUtf8()), qPrintable(field));
        // 旧硬约束已删除；兼容说明的语义必须是"仅兼容旧数据/旧调用"，不再要求新提案写 basis。
        QVERIFY(!body.contains(QStringLiteral("必须把依据写进 contact.basis").toUtf8()));
        QVERIFY(!body.contains(QStringLiteral("有新的主人证据、或活动有了结果").toUtf8()));
        QVERIFY(!body.contains(QStringLiteral("旧契约 contact.basis 仍被接受").toUtf8()));
        QVERIFY(!body.contains(QStringLiteral("必须有新主人证据").toUtf8()));
        QVERIFY(body.contains(QStringLiteral("仅用于兼容旧数据").toUtf8()));
        QVERIFY(body.contains(QStringLiteral("contact.motivation+claims").toUtf8()));
        QVERIFY(body.contains(QStringLiteral("self_desire 不需要主人有任何新证据").toUtf8()));
    }

    // ===== R12-1a OpenWake 运行时（rest/reflect + 类型化 origin + 持久失败）=====

    // 1) 没有用户消息也能完成一次内部 rest：领取一次 OpenWake → 模型返回 rest →
    //    open_wake.state='committed'，不写 chat_history/proactive/outbox，下一次 due 前移。
    void openWakeRestCompletesWithoutUserInput()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(true, 12, 3)));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("openwake-rest.db"));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [](int) -> QString {
            return QString(R"({"schema_version":1,"decision":"rest","note":"",)"
                          R"("next_wake":{"mode":"after","after_minutes":30}})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        std::unique_ptr<CognitiveCoordinator> coordinator;
        bool sawRecoveredLog = false;
        {
            MessageCapture capture;
            coordinator = std::make_unique<CognitiveCoordinator>(db, model, nullptr);
            for (const auto &line : capture.lines())
                if (line.message.contains(QStringLiteral("[OpenWake] recovered=")))
                    sawRecoveredLog = line.type == QtInfoMsg;
        }
        QVERIFY(sawRecoveredLog);

        coordinator->reviewIfDue();
        QTRY_VERIFY_WITH_TIMEOUT(
            db.openWakeState().value(QStringLiteral("last_outcome")).toString()
                == QStringLiteral("rest"), 8000);
        QCOMPARE(bodies.size(), 1);
        // 模型请求是内部唤醒：说明没有用户消息 + origin/run_id；不是对话轮次。
        const QByteArray body = bodies.first();
        QVERIFY(body.contains("open_wake"));
        QVERIFY(body.contains(QStringLiteral("内部唤醒").toUtf8()));
        QVERIFY(body.contains("current_input"));
        QVERIFY(body.contains("run_id"));
        QVERIFY(body.contains("origin"));
        QVERIFY(body.contains("time_context"));
        // R12-1a：OpenWake prompt 明确本片没有 contact 能力，self_desire 规则不与联系冲突；
        // 不包含"新提案必须写 contact.basis / 必须有新主人证据"的旧硬约束。
        QVERIFY(body.contains(QStringLiteral("没有 contact 能力").toUtf8()));
        QVERIFY(body.contains(QStringLiteral("self_desire 等联系动机仅作为内部记录").toUtf8()));
        QVERIFY(!body.contains(QStringLiteral("必须把依据写进 contact.basis").toUtf8()));
        QVERIFY(!body.contains(QStringLiteral("旧契约 contact.basis 仍被接受").toUtf8()));
        QVERIFY(!body.contains(QStringLiteral("必须有新主人证据").toUtf8()));
        // rest 不产生任何用户可见/外部动作：无用户行、无主动消息、无 outbox。
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM chat_history")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM action_outbox")), qint64(0));
        // rest 是合法终态，commit 后下一次 due 更新（30 分钟）。
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM open_wake WHERE state='committed' AND decision='rest'")),
                 qint64(1));
        const qint64 nextDue =
            scalarMemorySql(QStringLiteral("SELECT next_due_at FROM open_wake_state WHERE scope_key='owner'"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVERIFY2(nextDue - now >= 1750 && nextDue - now <= 1850,
                 qPrintable(QString::number(nextDue - now)));
        // rest 没有 note：内部记录可读接口不会返回空说明。
        QVERIFY(db.recentOpenWakeNotes(2).isEmpty());
    }

    // 2) reflect 的 note 成为可读的内部记录，但不做 UserStatement、不写用户行；
    //    下一轮不会立即重领（finish 已更新 next_due）。
    void openWakeReflectRecordReadableNextRound()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(true, 12, 3)));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("openwake-reflect.db"));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [](int) -> QString {
            return QString(R"({"schema_version":1,"decision":"reflect",)"
                          R"("note":"今晚的雨声让我想起窗台",)"
                          R"("next_wake":{"mode":"after","after_minutes":10}})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        CognitiveCoordinator coordinator(db, model, nullptr);
        coordinator.reviewIfDue();
        QTRY_VERIFY_WITH_TIMEOUT(
            db.openWakeState().value(QStringLiteral("last_outcome")).toString()
                == QStringLiteral("reflect"), 8000);

        const auto notes = db.recentOpenWakeNotes(2);
        QCOMPARE(notes.size(), 1);
        const auto note = notes.first().toObject();
        QCOMPARE(note.value(QStringLiteral("note")).toString(),
                 QStringLiteral("今晚的雨声让我想起窗台"));
        QCOMPARE(note.value(QStringLiteral("decision")).toString(), QStringLiteral("reflect"));
        QVERIFY(note.value(QStringLiteral("ref")).toString().startsWith(QStringLiteral("wake/")));
        // 不是用户陈述：没有用户行 / 主动消息 / outbox。
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM chat_history")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM chat_history WHERE turn_kind='proactive'")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM action_outbox")), qint64(0));
        const qint64 nextDue =
            scalarMemorySql(QStringLiteral("SELECT next_due_at FROM open_wake_state WHERE scope_key='owner'"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QVERIFY2(nextDue - now >= 550 && nextDue - now <= 650,
                 qPrintable(QString::number(nextDue - now)));
        // 下一轮（due 未来）不领取、不调模型。
        coordinator.reviewIfDue();
        QTest::qWait(100);
        QCOMPARE(bodies.size(), 1);
        QCOMPARE(db.openWakeState().value(QStringLiteral("last_outcome")).toString(),
                 QStringLiteral("reflect"));
    }

    // 3) disabled / daily_limit=0 / 独立每日上限 / 低预算取整：都不产生模型调用或越界领取。
    void openWakeDisabledAndBudget()
    {
        OpenWakeConfigGuard config;
        // (1) enabled=false：协调器不创建唤醒、不调用模型（即使调度行已到期）。
        QVERIFY(config.write(openWakeTestJson(false, 12, 3)));
        {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("openwake-disabled.db"));
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost));
            QList<QByteArray> bodies;
            serveCompletions(server, bodies, [](int) { return QStringLiteral("{}"); });
            LLMService model(QStringLiteral("test"), nullptr,
                             QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
            model.setMemoryManager(&db);
            CognitiveCoordinator coordinator(db, model, nullptr);
            coordinator.reviewIfDue();
            QTest::qWait(100);
            QCOMPARE(bodies.size(), 0);
            QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM open_wake")), qint64(0));
        }
        // (2) daily_limit=0：所有后台认知停止，OpenWake 到期也不领取。
        QVERIFY(config.write(openWakeTestJson(true, 12, 3), {}, 0));
        {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("openwake-daily-zero.db"));
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            QTcpServer server;
            QVERIFY(server.listen(QHostAddress::LocalHost));
            QList<QByteArray> bodies;
            serveCompletions(server, bodies, [](int) { return QStringLiteral("{}"); });
            LLMService model(QStringLiteral("test"), nullptr,
                             QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
            model.setMemoryManager(&db);
            CognitiveCoordinator coordinator(db, model, nullptr);
            coordinator.reviewIfDue();
            QTest::qWait(100);
            QCOMPARE(bodies.size(), 0);
            QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM open_wake")), qint64(0));
        }
        // (3) maxPerDay=1：第二次到期后按独立预算被挡住，不调模型。
        QVERIFY(config.write(openWakeTestJson(true, 1, 3)));
        {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("openwake-max-per-day.db"));
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            PrivateReview review(db);
            QSignalSpy requested(&review, &PrivateReview::requested);
            const auto openWake = ConfigManager::instance().getOpenWakeConfig();
            QCOMPARE(openWake.maxPerDay, 1);
            auto wake = review.wake(12, 60, openWake);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("open_wake"));
            const auto context = requested.last().at(2).toJsonObject();
            review.receive(requested.last().at(0).toLongLong(),
                           requested.last().at(1).toInt(),
                           openWakeProposal(context.value(QStringLiteral("origin")).toObject(),
                                            QStringLiteral("rest")), {});
            QCOMPARE(db.openWakeState().value(QStringLiteral("last_outcome")).toString(),
                     QStringLiteral("rest"));
            QVERIFY(rewindOpenWakeClock());
            const auto blocked = review.wake(12, 60, openWake);
            QVERIFY(!blocked.started);
            QCOMPARE(blocked.kind, QStringLiteral("open_wake"));
            QCOMPARE(blocked.reason, QStringLiteral("budget"));
            QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM open_wake")), qint64(1));
        }
        // (4) 低预算取整至少 1：dailyLimit=1 且共享预算 1% 时仍能领取一次。
        QVERIFY(config.write(openWakeTestJson(true, 12, 3, 1)));
        {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("openwake-low-budget.db"));
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            PrivateReview review(db);
            const auto openWake = ConfigManager::instance().getOpenWakeConfig();
            const auto wake = review.wake(1, 60, openWake);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("open_wake"));
        }
    }

    // 4) 模型给出 contact/activity：按 schema 违规扣除，不提交、不产生 outbox/外部动作。
    void openWakeRejectsContactOrActivity()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(true, 12, 3)));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("openwake-reject.db"));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        QSignalSpy contactSpy(&review, &PrivateReview::contactProposed);
        const auto openWake = ConfigManager::instance().getOpenWakeConfig();

        auto wake = review.wake(12, 60, openWake);
        QVERIFY(wake.started);
        auto context = requested.last().at(2).toJsonObject();
        auto origin = context.value(QStringLiteral("origin")).toObject();
        auto proposal = openWakeProposal(origin, QStringLiteral("reflect"),
                                         QStringLiteral("想继续写下去"), 20);
        proposal.insert(QStringLiteral("contact"),
                        QJsonObject{{QStringLiteral("timing"), QStringLiteral("now")},
                                    {QStringLiteral("message"), QStringLiteral("在吗")}});
        review.receive(requested.last().at(0).toLongLong(),
                       requested.last().at(1).toInt(), proposal, {});
        QCOMPARE(contactSpy.size(), 0);
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM action_outbox")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM chat_history")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM open_wake WHERE state='committed'")), qint64(0));
        QCOMPARE(textMemorySql(QStringLiteral("SELECT state FROM open_wake ORDER BY id DESC LIMIT 1")),
                 QStringLiteral("deferred"));
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM review_failure WHERE source_kind='open_wake'")), qint64(1));

        // 第二次：activity 字段同样越界；仍需可领取的第二次尝试。
        QVERIFY(rewindOpenWakeClock());
        wake = review.wake(12, 60, openWake);
        QVERIFY(wake.started);
        context = requested.last().at(2).toJsonObject();
        origin = context.value(QStringLiteral("origin")).toObject();
        proposal = openWakeProposal(origin, QStringLiteral("rest"));
        proposal.insert(QStringLiteral("activity"),
                        QJsonObject{{QStringLiteral("goal"), QStringLiteral("写点什么")}});
        review.receive(requested.last().at(0).toLongLong(),
                       requested.last().at(1).toInt(), proposal, {});
        QCOMPARE(scalarMemorySql(QStringLiteral("SELECT count(*) FROM action_outbox")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM open_wake WHERE state='committed'")), qint64(0));
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM review_failure WHERE source_kind='open_wake'")), qint64(2));
    }

    // 4b) R12-1a：OpenWake 间隔配置在提交边界执行。协议 5..1440 分钟先校验；合法值再按
    //     min/max_interval_seconds（秒→分钟换算：600s=10min、1800s=30min）夹紧成 effective。
    //     夹紧不是失败；协议非法才 schema 失败。intent 记录 requested/effective，日志可审计。
    void openWakeIntervalClampedAtCommit()
    {
        OpenWakeConfigGuard config;
        auto openWakeJson = openWakeTestJson(true, 12, 3);
        openWakeJson.insert(QStringLiteral("min_interval_seconds"), 600);      // 10 分钟
        openWakeJson.insert(QStringLiteral("max_interval_seconds"), 1800);     // 30 分钟
        openWakeJson.insert(QStringLiteral("default_interval_seconds"), 1800); // 默认 30 分钟
        QVERIFY(config.write(openWakeJson));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("openwake-interval.db"));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        const auto openWake = ConfigManager::instance().getOpenWakeConfig();
        QCOMPARE(openWake.minIntervalSeconds, 600);
        QCOMPARE(openWake.maxIntervalSeconds, 1800);
        QCOMPARE(openWake.defaultIntervalSeconds, 1800);

        const auto receiveProposal = [&review, &requested](const QJsonObject &proposal) {
            review.receive(requested.last().at(0).toLongLong(),
                           requested.last().at(1).toInt(), proposal, {});
        };
        const auto intentOf = [](qlonglong id) {
            const QString text = textMemorySql(QStringLiteral(
                "SELECT intent_json FROM open_wake WHERE id=%1").arg(id));
            return QJsonDocument::fromJson(text.toUtf8()).object();
        };
        const auto nextDueSeconds = []() {
            return scalarMemorySql(QStringLiteral(
                "SELECT next_due_at-unixepoch() FROM open_wake_state WHERE scope_key='owner'"));
        };
        const auto intervalLog = [](const MessageCapture &capture) {
            for (const auto &line : capture.lines())
                if (line.message.contains(QStringLiteral("[OpenWake] interval ")))
                    return line.message;
            return QString{};
        };

        // (1) 模型给协议下界 5 分钟：按配置 min 夹到 10 分钟 effective，成功提交。
        {
            const auto wake = review.wake(48, 60, openWake);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("open_wake"));
            const auto origin = requested.last().at(2).toJsonObject()
                                    .value(QStringLiteral("origin")).toObject();
            const qlonglong id = requested.last().at(0).toLongLong();
            MessageCapture capture;
            receiveProposal(openWakeProposal(origin, QStringLiteral("rest"), QString(), 5));
            QCOMPARE(db.openWakeState().value(QStringLiteral("last_outcome")).toString(),
                     QStringLiteral("rest"));
            const auto intent = intentOf(id);
            QCOMPARE(intent.value(QStringLiteral("requested_after_minutes")).toInt(), 5);
            QCOMPARE(intent.value(QStringLiteral("after_minutes")).toInt(), 10);
            const qint64 next = nextDueSeconds();
            QVERIFY2(next >= 590 && next <= 620, qPrintable(QString::number(next)));
            const QString logged = intervalLog(capture);
            QVERIFY2(logged.contains(QStringLiteral("requested=5 effective=10 min=10 max=30")),
                     qPrintable(logged));
        }

        // (2) 模型给 900 分钟：协议合法，按 max 夹到 30 分钟 effective，成功提交。
        {
            QVERIFY(rewindOpenWakeClock());
            const auto wake = review.wake(48, 60, openWake);
            QVERIFY(wake.started);
            const auto origin = requested.last().at(2).toJsonObject()
                                    .value(QStringLiteral("origin")).toObject();
            const qlonglong id = requested.last().at(0).toLongLong();
            MessageCapture capture;
            receiveProposal(openWakeProposal(origin, QStringLiteral("reflect"),
                                             QStringLiteral("继续想"), 900));
            const auto intent = intentOf(id);
            QCOMPARE(intent.value(QStringLiteral("requested_after_minutes")).toInt(), 900);
            QCOMPARE(intent.value(QStringLiteral("after_minutes")).toInt(), 30);
            const qint64 next = nextDueSeconds();
            QVERIFY2(next >= 1780 && next <= 1820, qPrintable(QString::number(next)));
            const QString logged = intervalLog(capture);
            QVERIFY2(logged.contains(QStringLiteral("requested=900 effective=30 min=10 max=30")),
                     qPrintable(logged));
        }

        // (3) 2000 / 3000 分钟都超出协议 5..1440：schema 失败，不夹紧成配置值，不提交。
        const QList<int> protocolInvalid{2000, 3000};
        for (const int invalidMinutes : protocolInvalid) {
            QVERIFY(rewindOpenWakeClock());
            const auto wake = review.wake(48, 60, openWake);
            QVERIFY(wake.started);
            const auto origin = requested.last().at(2).toJsonObject()
                                    .value(QStringLiteral("origin")).toObject();
            const qlonglong id = requested.last().at(0).toLongLong();
            const int beforeCommitted = int(scalarMemorySql(QStringLiteral(
                "SELECT count(*) FROM open_wake WHERE state='committed'")));
            receiveProposal(openWakeProposal(origin, QStringLiteral("rest"), QString(),
                                             invalidMinutes));
            QCOMPARE(int(scalarMemorySql(QStringLiteral(
                         "SELECT count(*) FROM open_wake WHERE state='committed'"))),
                     beforeCommitted);
            const auto failure = db.reviewFailure(QStringLiteral("open_wake"),
                                                  QStringLiteral("wake/%1").arg(id));
            QCOMPARE(failure.value(QStringLiteral("last_reason")).toString(),
                     QStringLiteral("schema"));
            QCOMPARE(textMemorySql(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(id)),
                     QStringLiteral("deferred"));
        }

        // (4) 模型没给 next_wake：默认间隔也走同一 effective 夹紧（30 分钟），并记录 requested。
        {
            QVERIFY(rewindOpenWakeClock());
            const auto wake = review.wake(48, 60, openWake);
            QVERIFY(wake.started);
            const auto origin = requested.last().at(2).toJsonObject()
                                    .value(QStringLiteral("origin")).toObject();
            const qlonglong id = requested.last().at(0).toLongLong();
            MessageCapture capture;
            receiveProposal(QJsonObject{{QStringLiteral("schema_version"), 1},
                                        {QStringLiteral("decision"), QStringLiteral("rest")},
                                        {QStringLiteral("_origin"), origin}});
            const auto intent = intentOf(id);
            QCOMPARE(intent.value(QStringLiteral("requested_after_minutes")).toInt(), 30);
            QCOMPARE(intent.value(QStringLiteral("after_minutes")).toInt(), 30);
            const QString logged = intervalLog(capture);
            QVERIFY2(logged.contains(QStringLiteral("requested=30 effective=30 min=10 max=30")),
                     qPrintable(logged));
        }
        // (5) 题述"配置下限 600 分钟"的等价配置（min=36000s / max=108000s，ConfigManager
        //     把 max 夹到 86400s=1440min）：模型给 60 分钟 → 夹到 effective=600，成功。
        //     这正是"配置 min=600、模型给 60 → effective=600"在秒→分钟单位下的语义。
        {
            auto wideJson = openWakeTestJson(true, 12, 3);
            wideJson.insert(QStringLiteral("min_interval_seconds"), 36000);      // 600 分钟
            wideJson.insert(QStringLiteral("max_interval_seconds"), 108000);     // 夹到 1440 分钟
            wideJson.insert(QStringLiteral("default_interval_seconds"), 36000);  // 默认 600 分钟
            QVERIFY(config.write(wideJson));
            const auto wideOpenWake = ConfigManager::instance().getOpenWakeConfig();
            QCOMPARE(wideOpenWake.minIntervalSeconds, 36000);
            QCOMPARE(wideOpenWake.maxIntervalSeconds, 86400);
            QVERIFY(rewindOpenWakeClock());
            const auto wake = review.wake(48, 60, wideOpenWake);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("open_wake"));
            const auto origin = requested.last().at(2).toJsonObject()
                                    .value(QStringLiteral("origin")).toObject();
            const qlonglong id = requested.last().at(0).toLongLong();
            MessageCapture capture;
            receiveProposal(openWakeProposal(origin, QStringLiteral("rest"), QString(), 60));
            const auto intent = intentOf(id);
            QCOMPARE(intent.value(QStringLiteral("requested_after_minutes")).toInt(), 60);
            QCOMPARE(intent.value(QStringLiteral("after_minutes")).toInt(), 600);
            const qint64 next = nextDueSeconds();
            QVERIFY2(next >= 35980 && next <= 36020, qPrintable(QString::number(next)));
            const QString logged = intervalLog(capture);
            QVERIFY2(logged.contains(QStringLiteral("requested=60 effective=600 min=600 max=1440")),
                     qPrintable(logged));
        }
    }

    // 5) 公平性：OpenWake 到期后先让出 3 次普通回顾；第 4 次领取 OpenWake。
    void openWakeFairnessAfterNonOpenJobs()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(true, 12, 3),
                             QJsonObject{{QStringLiteral("private_writing"), true}}));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("openwake-fairness.db"));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        const auto openWake = ConfigManager::instance().getOpenWakeConfig();

        for (int index = 0; index < 3; ++index) {
            QVERIFY(db.saveQATurn(QStringLiteral("write"),
                                  QStringLiteral(R"({"personal_interest":"interest-%1"})").arg(index),
                                  {}));
            const auto wake = review.wake(12, 60, openWake);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("activity"));
            const auto id = requested.last().at(0).toLongLong();
            const int version = requested.last().at(1).toInt();
            // R12-1a：严格来源校验要求成功 proposal 携带 requested context 的 origin。
            const auto activityOrigin = requested.last().at(2).toJsonObject()
                                            .value(QStringLiteral("origin")).toObject();
            review.receive(id, version,
                           QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                       {QStringLiteral("reflection"), QStringLiteral("done")},
                                       {QStringLiteral("draft"), QString()},
                                       {QStringLiteral("_origin"), activityOrigin}}, {});
            QVERIFY(execMemorySql(QStringLiteral(
                "UPDATE thought_review_log SET started_at=started_at-120")));
        }
        QCOMPARE(db.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 3);
        // 3 次普通任务之后，下一次优先 OpenWake（claim 成功会清零公平计数）。
        const auto wake = review.wake(12, 60, openWake);
        QVERIFY(wake.started);
        QCOMPARE(wake.kind, QStringLiteral("open_wake"));
        QVERIFY(wake.sourceRef.startsWith(QStringLiteral("wake/")));
        QCOMPARE(db.openWakeState().value(QStringLiteral("non_open_since_due")).toInt(), 0);
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT count(*) FROM open_wake WHERE state='running'")), qint64(1));
    }

    // 6) 来源校验：同 id/version 但 kind/ref/run_id/epoch 不一致的迟到回调被拒，不提交；
    //    OpenWake 必须严格携带正确 _origin，activity/reflection 同样按 kind/ref 校验。
    void originMismatchRejected()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(true, 12, 3)));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath("origin-mismatch.db"));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        const auto openWake = ConfigManager::instance().getOpenWakeConfig();

        // --- OpenWake：错误的 run_id / kind 都被拒，正确 origin 才提交 ---
        auto wake = review.wake(12, 60, openWake);
        QVERIFY(wake.started);
        QCOMPARE(wake.kind, QStringLiteral("open_wake"));
        const qlonglong wakeId = requested.last().at(0).toLongLong();
        const int wakeVersion = requested.last().at(1).toInt();
        const auto openWakeContext = requested.last().at(2).toJsonObject();
        const auto openWakeOrigin = openWakeContext.value(QStringLiteral("origin")).toObject();
        QCOMPARE(openWakeOrigin.value(QStringLiteral("kind")).toString(), QStringLiteral("open_wake"));
        QCOMPARE(openWakeOrigin.value(QStringLiteral("id")).toInteger(), wakeId);
        QVERIFY(openWakeOrigin.value(QStringLiteral("run_id")).toString().size() > 0);
        QVERIFY(openWakeOrigin.value(QStringLiteral("ref")).toString()
                    .startsWith(QStringLiteral("wake/")));
        QVERIFY(openWakeContext.value(QStringLiteral("current_input")).isNull());
        QCOMPARE(openWakeContext.value(QStringLiteral("allow_contact")).toBool(true), false);

        auto forged = openWakeOrigin;
        forged.insert(QStringLiteral("run_id"), QStringLiteral("forged-run"));
        review.receive(wakeId, wakeVersion, openWakeProposal(forged, QStringLiteral("rest")), {});
        QCOMPARE(textMemorySql(QStringLiteral("SELECT state FROM open_wake ORDER BY id DESC LIMIT 1")),
                 QStringLiteral("running"));
        QVERIFY(db.reviewFailure(QStringLiteral("open_wake"),
                                 QStringLiteral("wake/%1").arg(wakeId)).isEmpty());

        forged = openWakeOrigin;
        forged.insert(QStringLiteral("kind"), QStringLiteral("reflection"));
        forged.insert(QStringLiteral("ref"), QStringLiteral("reflection/%1").arg(wakeId));
        review.receive(wakeId, wakeVersion, openWakeProposal(forged, QStringLiteral("rest")), {});
        QCOMPARE(textMemorySql(QStringLiteral("SELECT state FROM open_wake ORDER BY id DESC LIMIT 1")),
                 QStringLiteral("running"));
        review.receive(wakeId, wakeVersion,
                       openWakeProposal(openWakeOrigin, QStringLiteral("rest")), {});
        QCOMPARE(db.openWakeState().value(QStringLiteral("last_outcome")).toString(),
                 QStringLiteral("rest"));

        // --- Activity：origin.kind/ref 不一致 -> 不提交；正确 origin 才提交 ---
        QVERIFY(rewindOpenWakeClock());
        QVERIFY(db.saveQATurn(QStringLiteral("write"),
                              QStringLiteral(R"({"personal_interest":"雨声"})"), {}));
        QVERIFY(execMemorySql(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120")));
        auto activityWake = review.wake(12, 60);
        QVERIFY(activityWake.started);
        QCOMPARE(activityWake.kind, QStringLiteral("activity"));
        const qlonglong activityId = requested.last().at(0).toLongLong();
        const int activityVersion = requested.last().at(1).toInt();
        const auto activityOrigin = requested.last().at(2).toJsonObject()
                                        .value(QStringLiteral("origin")).toObject();
        auto forgedActivity = activityOrigin;
        forgedActivity.insert(QStringLiteral("kind"), QStringLiteral("reflection"));
        forgedActivity.insert(QStringLiteral("ref"),
                              QStringLiteral("reflection/%1").arg(activityId));
        review.receive(activityId, activityVersion,
                       QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                   {QStringLiteral("reflection"), QStringLiteral("late")},
                                   {QStringLiteral("draft"), QStringLiteral("late")},
                                   {QStringLiteral("_origin"), forgedActivity}}, {});
        QCOMPARE(db.activityById(activityId).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("active"));
        review.receive(activityId, activityVersion,
                       QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                   {QStringLiteral("reflection"), QStringLiteral("done")},
                                   {QStringLiteral("draft"), QStringLiteral("done")},
                                   {QStringLiteral("_origin"), activityOrigin}}, {});
        QCOMPARE(db.activityById(activityId).value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("completed"));

        // --- Reflection：epoch 不一致被拒，正确 origin 才落库 ---
        QVERIFY(execMemorySql(QStringLiteral("UPDATE thought_review_log SET started_at=started_at-120")));
        const auto turn = seedRuntimeReflectionSource(db, QStringLiteral("origin-mismatch"));
        QVERIFY(turn > 0);
        auto reflectionWake = review.wake(12, 60);
        QVERIFY(reflectionWake.started);
        QCOMPARE(reflectionWake.kind, QStringLiteral("reflection"));
        const int reflectionVersion = requested.last().at(1).toInt();
        const auto reflectionOrigin = requested.last().at(2).toJsonObject()
                                          .value(QStringLiteral("origin")).toObject();
        auto forgedReflection = reflectionOrigin;
        forgedReflection.insert(QStringLiteral("epoch"), 7);
        review.receive(turn, reflectionVersion,
                       QJsonObject{{QStringLiteral("choice"), QStringLiteral("rest")},
                                   {QStringLiteral("_origin"), forgedReflection}}, {});
        QCOMPARE(textMemorySql(QStringLiteral(
                     "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn)),
                 QStringLiteral("pending"));
        QVERIFY(db.reviewFailure(QStringLiteral("reflection"),
                                 QStringLiteral("reflection/%1").arg(turn)).isEmpty());
        review.receive(turn, reflectionVersion,
                       QJsonObject{{QStringLiteral("choice"), QStringLiteral("rest")},
                                   {QStringLiteral("_origin"), reflectionOrigin}}, {});
        QVERIFY(textMemorySql(QStringLiteral(
                    "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn))
                != QStringLiteral("pending"));
    }

    // 6b) R12-1a：四类来源都必须携带服务端 _origin；缺失/不匹配一律 stale_callback
    //     丢弃（不提交、不记失败），匹配的 origin 正常提交。非 OpenWake 来源的 runId
    //     在领取时生成，随 requested context 下发并要求回显。
    void strictOriginRequiredForAllSources()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(true, 12, 3),
                             QJsonObject{{QStringLiteral("private_writing"), true}}));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("strict-origin.db"));
        MemoryManager db(path);
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        PrivateReview review(db);
        QSignalSpy requested(&review, &PrivateReview::requested);
        const auto openWake = ConfigManager::instance().getOpenWakeConfig();
        const auto lastContext = [&requested] {
            return requested.last().at(2).toJsonObject();
        };
        const auto lastOrigin = [&lastContext] {
            return lastContext().value(QStringLiteral("origin")).toObject();
        };
        const auto receiveAtLast = [&review, &requested](const QJsonObject &proposal,
                                                         const QString &error = {}) {
            review.receive(requested.last().at(0).toLongLong(),
                           requested.last().at(1).toInt(), proposal, error);
        };
        const auto forgedRunId = [](const QJsonObject &origin) {
            auto forged = origin;
            forged.insert(QStringLiteral("run_id"), QStringLiteral("forged-run"));
            return forged;
        };

        // --- Activity：缺失 / runId 不匹配 -> 不提交、不记失败；匹配 -> 提交 ---
        QVERIFY(db.saveQATurn(QStringLiteral("write"),
                              QStringLiteral(R"({"personal_interest":"雨声"})"), {}));
        auto wake = review.wake(12, 60, openWake);
        QVERIFY(wake.started);
        QCOMPARE(wake.kind, QStringLiteral("activity"));
        const auto activityOrigin = lastOrigin();
        QCOMPARE(activityOrigin.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("activity"));
        QVERIFY(!activityOrigin.value(QStringLiteral("run_id")).toString().isEmpty());
        const qlonglong activityId = activityOrigin.value(QStringLiteral("id")).toInteger();
        const QString activityRef = activityOrigin.value(QStringLiteral("ref")).toString();
        const auto activityStatus = [&db, activityId]() {
            return db.activityById(activityId).value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("status")).toString();
        };
        receiveAtLast(QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                  {QStringLiteral("reflection"), QStringLiteral("missing")}});
        QCOMPARE(activityStatus(), QStringLiteral("active"));
        QVERIFY(db.reviewFailure(QStringLiteral("activity"), activityRef).isEmpty());
        receiveAtLast(QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                  {QStringLiteral("reflection"), QStringLiteral("forged")},
                                  {QStringLiteral("_origin"), forgedRunId(activityOrigin)}});
        QCOMPARE(activityStatus(), QStringLiteral("active"));
        QVERIFY(db.reviewFailure(QStringLiteral("activity"), activityRef).isEmpty());
        receiveAtLast(QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                  {QStringLiteral("reflection"), QStringLiteral("done")},
                                  {QStringLiteral("_origin"), activityOrigin}});
        QCOMPARE(activityStatus(), QStringLiteral("completed"));

        // --- Reflection：缺失 / runId 不匹配 -> 保持 pending；匹配 -> 落库 ---
        QVERIFY(execMemorySql(QStringLiteral(
            "UPDATE thought_review_log SET started_at=started_at-120")));
        const auto reflectionTurn = seedRuntimeReflectionSource(db, QStringLiteral("strict-origin"));
        QVERIFY(reflectionTurn > 0);
        wake = review.wake(12, 60);
        QVERIFY(wake.started);
        QCOMPARE(wake.kind, QStringLiteral("reflection"));
        const auto reflectionOrigin = lastOrigin();
        QCOMPARE(reflectionOrigin.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("reflection"));
        QVERIFY(!reflectionOrigin.value(QStringLiteral("run_id")).toString().isEmpty());
        const QString reflectionRef = QStringLiteral("reflection/%1").arg(reflectionTurn);
        const auto reflectionState = [reflectionTurn]() {
            return textMemorySql(QStringLiteral(
                "SELECT state FROM private_reflection WHERE turn_id=%1").arg(reflectionTurn));
        };
        receiveAtLast(QJsonObject{{QStringLiteral("choice"), QStringLiteral("rest")}});
        QCOMPARE(reflectionState(), QStringLiteral("pending"));
        QVERIFY(db.reviewFailure(QStringLiteral("reflection"), reflectionRef).isEmpty());
        receiveAtLast(QJsonObject{{QStringLiteral("choice"), QStringLiteral("rest")},
                                  {QStringLiteral("_origin"), forgedRunId(reflectionOrigin)}});
        QCOMPARE(reflectionState(), QStringLiteral("pending"));
        QVERIFY(db.reviewFailure(QStringLiteral("reflection"), reflectionRef).isEmpty());
        receiveAtLast(QJsonObject{{QStringLiteral("choice"), QStringLiteral("rest")},
                                  {QStringLiteral("_origin"), reflectionOrigin}});
        QVERIFY(reflectionState() != QStringLiteral("pending"));

        // --- Concern：缺失 / runId 不匹配 -> 保持 pending；匹配 -> keep 落库 ---
        QVERIFY(execMemorySql(QStringLiteral(
            "UPDATE thought_review_log SET started_at=started_at-120")));
        const auto concernTurn =
            seedRuntimeReflectionSource(db, QStringLiteral("strict-origin-concern"));
        QVERIFY(concernTurn > 0);
        const auto concernClaim = db.claimPrivateReflection(12, 60);
        QCOMPARE(concernClaim.value(QStringLiteral("id")).toInteger(), concernTurn);
        QVERIFY(db.finishPrivateReflection(
            concernTurn, concernClaim.value(QStringLiteral("version")).toInt(),
            QStringLiteral("consider"), QStringLiteral("窗台还没擦完"),
            QJsonObject{}, QJsonArray{}, QJsonObject{}, QString{},
            QJsonObject{{QStringLiteral("topic"), QStringLiteral("窗台")},
                        {QStringLiteral("next_check_after_minutes"), 1}}));
        {
            RawSqlConnection raw(path, QStringLiteral("strict-origin-concern-due"));
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral(
                "UPDATE private_reflection SET next_check_at=unixepoch()-1 "
                "WHERE turn_id=%1").arg(concernTurn)));
        }
        QVERIFY(execMemorySql(QStringLiteral(
            "UPDATE thought_review_log SET started_at=started_at-120")));
        wake = review.wake(12, 60);
        QVERIFY(wake.started);
        QCOMPARE(wake.kind, QStringLiteral("concern"));
        const auto concernOrigin = lastOrigin();
        QCOMPARE(concernOrigin.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("concern"));
        QVERIFY(!concernOrigin.value(QStringLiteral("run_id")).toString().isEmpty());
        const QString concernRef = QStringLiteral("concern/%1").arg(concernTurn);
        const auto concernState = [concernTurn]() {
            return textMemorySql(QStringLiteral(
                "SELECT state FROM private_reflection WHERE turn_id=%1").arg(concernTurn));
        };
        const QJsonObject keepProposal{{QStringLiteral("concern_action"), QStringLiteral("keep")},
                                       {QStringLiteral("forget"),
                                        QJsonObject{{QStringLiteral("next_check_after_minutes"), 0}}}};
        receiveAtLast(keepProposal);
        QCOMPARE(concernState(), QStringLiteral("pending"));
        QVERIFY(db.reviewFailure(QStringLiteral("concern"), concernRef).isEmpty());
        auto forgedConcern = keepProposal;
        forgedConcern.insert(QStringLiteral("_origin"), forgedRunId(concernOrigin));
        receiveAtLast(forgedConcern);
        QCOMPARE(concernState(), QStringLiteral("pending"));
        QVERIFY(db.reviewFailure(QStringLiteral("concern"), concernRef).isEmpty());
        auto matchedKeep = keepProposal;
        matchedKeep.insert(QStringLiteral("_origin"), concernOrigin);
        receiveAtLast(matchedKeep);
        QCOMPARE(concernState(), QStringLiteral("consider"));
        QCOMPARE(scalarMemorySql(QStringLiteral(
                     "SELECT next_check_at FROM private_reflection WHERE turn_id=%1")
                     .arg(concernTurn)), qint64(0));

        // --- OpenWake：缺失 / runId 不匹配 -> 保持 running；匹配 -> committed ---
        QVERIFY(rewindOpenWakeClock());
        wake = review.wake(12, 60, openWake);
        QVERIFY(wake.started);
        QCOMPARE(wake.kind, QStringLiteral("open_wake"));
        const auto openWakeOrigin = lastOrigin();
        QCOMPARE(openWakeOrigin.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("open_wake"));
        const qlonglong wakeId = openWakeOrigin.value(QStringLiteral("id")).toInteger();
        const QString wakeRef = QStringLiteral("wake/%1").arg(wakeId);
        const auto wakeState = [wakeId]() {
            return textMemorySql(QStringLiteral(
                "SELECT state FROM open_wake WHERE id=%1").arg(wakeId));
        };
        // 合法 proposal 但不带 _origin：拒绝且不记失败。
        auto missingOriginWake = openWakeProposal(openWakeOrigin, QStringLiteral("rest"));
        missingOriginWake.remove(QStringLiteral("_origin"));
        receiveAtLast(missingOriginWake);
        QCOMPARE(wakeState(), QStringLiteral("running"));
        QVERIFY(db.reviewFailure(QStringLiteral("open_wake"), wakeRef).isEmpty());
        receiveAtLast(openWakeProposal(forgedRunId(openWakeOrigin), QStringLiteral("rest")));
        QCOMPARE(wakeState(), QStringLiteral("running"));
        QVERIFY(db.reviewFailure(QStringLiteral("open_wake"), wakeRef).isEmpty());
        receiveAtLast(openWakeProposal(openWakeOrigin, QStringLiteral("rest")));
        QCOMPARE(db.openWakeState().value(QStringLiteral("last_outcome")).toString(),
                 QStringLiteral("rest"));
    }

    // 6c) R12-1a：LLMService 的终态 proposal 由服务端覆盖写入 _origin，模型自填同名字段
    //     不生效；回调携带的是本次领取的服务端来源身份。
    void llmServiceOverridesModelOrigin()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("origin-echo.db")));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> bodies;
        serveCompletions(server, bodies, [](int) -> QString {
            // 模型伪造一个完全不同的 _origin；服务端必须用它自己的 origin 覆盖。
            return QStringLiteral(
                R"({"schema_version":1,"decision":"rest","next_wake":{"mode":"after",)"
                R"("after_minutes":30},"_origin":{"kind":"activity","ref":"thought/99",)"
                R"("id":99,"version":1,"run_id":"model-forged","scope":"owner","epoch":0}})");
        });
        LLMService model(QStringLiteral("test"), nullptr,
                         QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
        model.setMemoryManager(&db);
        QSignalSpy finished(&model, &LLMService::privateReviewFinished);
        const QJsonObject serverOrigin{
            {QStringLiteral("kind"), QStringLiteral("open_wake")},
            {QStringLiteral("ref"), QStringLiteral("wake/7")},
            {QStringLiteral("id"), 7},
            {QStringLiteral("version"), 1},
            {QStringLiteral("run_id"), QStringLiteral("server-run")},
            {QStringLiteral("scope"), QStringLiteral("owner")},
            {QStringLiteral("epoch"), 3}};
        const QJsonObject thread{
            {QStringLiteral("kind"), QStringLiteral("open_wake")},
            {QStringLiteral("origin"), serverOrigin},
            {QStringLiteral("scope"), QStringLiteral("owner")},
            {QStringLiteral("trigger"), QStringLiteral("due")},
            {QStringLiteral("allow_contact"), false}};
        model.requestPrivateReview(7, 1, thread);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 8000);
        const auto proposal = finished.first().at(2).toJsonObject();
        const auto echoed = proposal.value(QStringLiteral("_origin")).toObject();
        QCOMPARE(echoed.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("open_wake"));
        QCOMPARE(echoed.value(QStringLiteral("run_id")).toString(),
                 QStringLiteral("server-run"));
        QCOMPARE(echoed.value(QStringLiteral("id")).toInteger(), qint64(7));
        QCOMPARE(echoed.value(QStringLiteral("epoch")).toInteger(), qint64(3));
    }

    // 7) 失败接线：Reflection/OpenWake 失败写 MemoryManager 持久失败并按退避重试；
    //    用户 yield 不增失败计数。
    void persistentFailureWired()
    {
        OpenWakeConfigGuard config;
        QVERIFY(config.write(openWakeTestJson(false, 12, 3)));
        {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("failure-reflection.db"));
            const auto turn = seedRuntimeReflectionSource(db, QStringLiteral("failure"));
            QVERIFY(turn > 0);
            const QString ref = QStringLiteral("reflection/%1").arg(turn);
            PrivateReview review(db);
            QSignalSpy requested(&review, &PrivateReview::requested);
            auto wake = review.wake(12, 60);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("reflection"));
            // R12-1a：失败回调同样必须携带服务端 origin，否则按 stale_callback 丢弃。
            const auto firstFailureOrigin = requested.last().at(2).toJsonObject()
                                                .value(QStringLiteral("origin")).toObject();
            review.receive(requested.last().at(0).toLongLong(),
                           requested.last().at(1).toInt(),
                           QJsonObject{{QStringLiteral("_origin"), firstFailureOrigin}},
                           QStringLiteral("network_error"));
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            auto failure = db.reviewFailure(QStringLiteral("reflection"), ref);
            QCOMPARE(failure.value(QStringLiteral("consecutive_failures")).toInt(), 1);
            const qint64 retry = failure.value(QStringLiteral("next_retry_at")).toInteger();
            QVERIFY2(retry - now >= 115 && retry - now <= 125,
                     qPrintable(QString::number(retry - now)));
            QCOMPARE(textMemorySql(QStringLiteral(
                         "SELECT state FROM private_reflection WHERE turn_id=%1").arg(turn)),
                     QStringLiteral("deferred"));
            // 退避期内不立即重领。
            const auto blocked = review.wake(12, 60);
            QVERIFY(!blocked.started);
            // 模拟退避时间已过 -> 第二次失败累计到 2，退避 300 秒。
            QVERIFY(execMemorySql(QStringLiteral(
                "UPDATE private_reflection SET eligible_at=unixepoch()-1 WHERE turn_id=%1").arg(turn)));
            QVERIFY(execMemorySql(QStringLiteral(
                "UPDATE thought_review_log SET started_at=started_at-500")));
            wake = review.wake(12, 60);
            QVERIFY(wake.started);
            const auto secondFailureOrigin = requested.last().at(2).toJsonObject()
                                                 .value(QStringLiteral("origin")).toObject();
            review.receive(requested.last().at(0).toLongLong(),
                           requested.last().at(1).toInt(),
                           QJsonObject{{QStringLiteral("_origin"), secondFailureOrigin}},
                           QStringLiteral("network_error"));
            failure = db.reviewFailure(QStringLiteral("reflection"), ref);
            QCOMPARE(failure.value(QStringLiteral("consecutive_failures")).toInt(), 2);
            const qint64 retry2 = failure.value(QStringLiteral("next_retry_at")).toInteger();
            QVERIFY2(retry2 - QDateTime::currentSecsSinceEpoch() >= 295
                         && retry2 - QDateTime::currentSecsSinceEpoch() <= 305,
                     qPrintable(QString::number(retry2 - QDateTime::currentSecsSinceEpoch())));
            // 用户插话 yield：立即放回队列，不增加失败计数。
            QVERIFY(execMemorySql(QStringLiteral(
                "UPDATE private_reflection SET eligible_at=unixepoch()-1 WHERE turn_id=%1").arg(turn)));
            QVERIFY(execMemorySql(QStringLiteral(
                "UPDATE thought_review_log SET started_at=started_at-500")));
            wake = review.wake(12, 60);
            QVERIFY(wake.started);
            review.yieldAttention(QStringLiteral("incoming_message"));
            QCOMPARE(db.reviewFailure(QStringLiteral("reflection"), ref)
                         .value(QStringLiteral("consecutive_failures")).toInt(), 2);
            const qint64 immediate = scalarMemorySql(QStringLiteral(
                "SELECT eligible_at-unixepoch() FROM private_reflection WHERE turn_id=%1").arg(turn));
            QVERIFY2(immediate >= -2 && immediate <= 2, qPrintable(QString::number(immediate)));
        }
        // OpenWake：失败写入同一张 review_failure 表，源状态 deferred，调度推到 next_retry。
        QVERIFY(config.write(openWakeTestJson(true, 12, 3)));
        {
            QTemporaryDir dir;
            MemoryManager db(dir.filePath("failure-openwake.db"));
            QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
            PrivateReview review(db);
            QSignalSpy requested(&review, &PrivateReview::requested);
            const auto openWake = ConfigManager::instance().getOpenWakeConfig();
            auto wake = review.wake(12, 60, openWake);
            QVERIFY(wake.started);
            QCOMPARE(wake.kind, QStringLiteral("open_wake"));
            const qlonglong id = requested.last().at(0).toLongLong();
            const int version = requested.last().at(1).toInt();
            const auto wakeOrigin = requested.last().at(2).toJsonObject()
                                        .value(QStringLiteral("origin")).toObject();
            review.receive(id, version,
                           QJsonObject{{QStringLiteral("_origin"), wakeOrigin}},
                           QStringLiteral("network_error"));
            const QString ref = QStringLiteral("wake/%1").arg(id);
            auto failure = db.reviewFailure(QStringLiteral("open_wake"), ref);
            QCOMPARE(failure.value(QStringLiteral("consecutive_failures")).toInt(), 1);
            QVERIFY(failure.value(QStringLiteral("next_retry_at")).toInteger() > 0);
            QCOMPARE(textMemorySql(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(id)),
                     QStringLiteral("deferred"));
            const qint64 scheduled = scalarMemorySql(QStringLiteral(
                "SELECT next_due_at-unixepoch() FROM open_wake_state WHERE scope_key='owner'"));
            QVERIFY2(scheduled > 100 && scheduled <= 125,
                     qPrintable(QString::number(scheduled)));
            const auto blocked = review.wake(12, 60, openWake);
            QVERIFY(!blocked.started);
            // 退避期过后重新领取；yield（主动让出）不新增失败、不写 current_activity。
            QVERIFY(rewindOpenWakeClock());
            wake = review.wake(12, 60, openWake);
            QVERIFY(wake.started);
            const qlonglong secondId = requested.last().at(0).toLongLong();
            review.yieldAttention(QStringLiteral("incoming_message"));
            QCOMPARE(textMemorySql(QStringLiteral(
                         "SELECT state FROM open_wake WHERE id=%1").arg(secondId)),
                     QStringLiteral("deferred"));
            QVERIFY(db.reviewFailure(QStringLiteral("open_wake"),
                                     QStringLiteral("wake/%1").arg(secondId)).isEmpty());
            const qint64 immediate = scalarMemorySql(QStringLiteral(
                "SELECT next_due_at-unixepoch() FROM open_wake_state WHERE scope_key='owner'"));
            QVERIFY2(immediate >= -2 && immediate <= 2, qPrintable(QString::number(immediate)));
            QVERIFY(db.attentionState(QStringLiteral("current_activity")).isEmpty());
        }
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

    void toolSearchKeepsAssistantExpression()
    {
        QTemporaryDir dir;
        MemoryManager mm(dir.filePath("assistant-tool.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = QStringLiteral("10101");
        meta.eventId = QStringLiteral("assistant-tool");
        SentenceText answer;
        answer.zhText = QStringLiteral("好呀");
        answer.jaText = QStringLiteral("いいよ");
        QVERIFY(mm.saveQATurn(QStringLiteral("我答应你"), QStringLiteral("{}"),
                              {answer}, meta));
        MemoryAgentScope scope = ownerPrivateScope(QDateTime::currentDateTime(), false);
        scope.sessionId = QStringLiteral("10101");
        scope.access.sessionId = QStringLiteral("10101");
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const auto result = tools.execute(QStringLiteral("memory_search"),
            QJsonObject{{"query",QStringLiteral("好呀")},
                        {"kinds",QJsonArray{QStringLiteral("chat_history")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        bool foundAssistant = false;
        for (const auto &value : result.value(QStringLiteral("items")).toArray()) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("speaker_role")).toString()
                == QStringLiteral("assistant")) {
                foundAssistant = true;
                QCOMPARE(item.value(QStringLiteral("derived")).toString(),
                         QStringLiteral("assistant_expression"));
                QVERIFY(!item.value(QStringLiteral("person_id")).toString().isEmpty());
            }
        }
        QVERIFY(foundAssistant);
    }

    void toolSearchDoesNotDropOldTurns()
    {
        QTemporaryDir dir;
        MemoryManager mm(dir.filePath("old-turns.db"));
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = meta.actorId = QStringLiteral("10101");
        for (int i = 0; i < 220; ++i) {
            meta.eventId = QStringLiteral("old-%1").arg(i);
            QVERIFY(mm.saveQATurn(QStringLiteral("旧记录%1").arg(i),
                                  QStringLiteral("{}"), {}, meta));
        }
        MemoryAgentScope scope = ownerPrivateScope(QDateTime::currentDateTime(), false);
        scope.sessionId = QStringLiteral("10101");
        scope.access.sessionId = QStringLiteral("10101");
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const auto result = tools.execute(QStringLiteral("memory_search"),
            QJsonObject{{"query",QStringLiteral("旧记录0")},
                        {"kinds",QJsonArray{QStringLiteral("chat_history")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        bool found = false;
        for (const auto &value : result.value(QStringLiteral("items")).toArray()) {
            if (value.toObject().value(QStringLiteral("text")).toString()
                == QStringLiteral("旧记录0"))
                found = true;
        }
        QVERIFY(found);
    }

    void ownerGroupToolCanSeeCrossChannelPrivateChat()
    {
        QTemporaryDir dir;
        MemoryManager mm(dir.filePath("tool-cross.db"));
        RequestMeta privateMeta;
        privateMeta.channel = Channel::kQQPrivate;
        privateMeta.sessionId = privateMeta.actorId = QStringLiteral("10101");
        privateMeta.eventId = QStringLiteral("private-secret");
        QVERIFY(mm.saveQATurn(QStringLiteral("私聊秘密"), QStringLiteral("{}"), {}, privateMeta));
        RequestMeta groupMeta;
        groupMeta.channel = Channel::kQQGroup;
        groupMeta.sessionId = QStringLiteral("30001");
        groupMeta.actorId = QStringLiteral("10101");
        groupMeta.eventId = QStringLiteral("group-secret");
        QVERIFY(mm.saveQATurn(QStringLiteral("公开内容"), QStringLiteral("{}"), {}, groupMeta));

        MemoryAgentScope scope = ownerGroupScope(QStringLiteral("30001"), QDateTime::currentDateTime());
        scope.ownerCrossChannelPrivate = true;
        scope.recentQueryTemplate.viewerIds = {privateMeta.actorId};
        RequestMeta other = privateMeta;
        other.sessionId = other.actorId = "404";
        other.eventId = "unrelated-private";
        other.subject = "qq:404";
        QVERIFY(mm.saveQATurn("其他人的私聊秘密", "{}", {}, other));
        MemoryAgentTools tools(&mm, scope);
        QString error;
        const auto result = tools.execute(QStringLiteral("memory_search"),
            QJsonObject{{"query",QStringLiteral("私聊秘密")},
                        {"kinds",QJsonArray{QStringLiteral("chat_history")}}},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        bool found = false;
        for (const auto &value : result.value(QStringLiteral("items")).toArray())
            if (value.toObject().value(QStringLiteral("text")).toString()
                == QStringLiteral("私聊秘密"))
                found = true;
        QVERIFY(found);
        QVERIFY(!QJsonDocument(result).toJson().contains(QString("其他人的私聊秘密").toUtf8()));
    }

    void dashScopeProvidersParseMockResponses()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray capturedRerankRequest;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &capturedRerankRequest] {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &capturedRerankRequest] {
                const auto bytes = socket->readAll();
                const auto header = bytes.indexOf("\r\n\r\n");
                if (header < 0)
                    return;
                const bool rerankRequest = bytes.contains("\"documents\"");
                if (rerankRequest)
                    capturedRerankRequest = bytes.mid(header + 4);
                const QJsonObject response = rerankRequest
                    ? QJsonObject{{"output", QJsonObject{{"results",
                        QJsonArray{QJsonObject{{"index", 1}, {"relevance_score", 0.9}},
                                  QJsonObject{{"index", 0}, {"relevance_score", 0.2}}}}}}}
                    : QJsonObject{{"data", QJsonArray{QJsonObject{{"embedding",
                        QJsonArray{0.1, 0.2}}, {"index", 0}}}},
                        {"model", "text-embedding-v4"}};
                const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Connection: close\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        DashScopeEmbeddingProvider embedding(QUrl(base + QStringLiteral("/embeddings")),
                                             QStringLiteral("test-key"),
                                             QStringLiteral("text-embedding-v4"), 2);
        QVector<float> vector;
        QString error;
        QVERIFY2(embedding.embed(QStringLiteral("你好"), &vector, &error), qPrintable(error));
        QCOMPARE(vector.size(), 2);
        QCOMPARE(vector.at(0), 0.1f);
        QCOMPARE(vector.at(1), 0.2f);

        QList<MemoryEvidence> candidates;
        MemoryEvidence first;
        first.text = QStringLiteral("第一");
        MemoryEvidence second;
        second.text = QStringLiteral("第二");
        candidates << first << second;
        DashScopeReranker reranker(QUrl(base + QStringLiteral("/rerank")),
                                   QStringLiteral("test-key"));
        const auto ranked = reranker.rerank(QStringLiteral("查询"), candidates, 2, &error);
        QCOMPARE(ranked.size(), 2);
        QCOMPARE(ranked.first().text, QStringLiteral("第二"));
        const auto sent = QJsonDocument::fromJson(capturedRerankRequest).object();
        const auto documents = sent.value(QStringLiteral("input")).toObject()
                                   .value(QStringLiteral("documents")).toArray();
        QCOMPARE(documents.size(), 2);
        QCOMPARE(documents.at(0).toString(), QStringLiteral("第一"));
        QCOMPARE(documents.at(1).toString(), QStringLiteral("第二"));
    }

    void retrievalDatabasePathPrefersCurrentManager()
    {
        QTemporaryDir dir;
        MemoryManager configured(dir.filePath("configured.db"));
        MemoryManager current(dir.filePath("current.db"));
        LLMService model(QStringLiteral("test"));
        QCOMPARE(model.retrievalDatabasePath(),
                 ConfigManager::instance().getMemoryPath());
        model.setMemoryManager(&current);
        QCOMPARE(model.retrievalDatabasePath(), current.databasePath());
        QVERIFY(model.retrievalDatabasePath() != configured.databasePath());
    }

    void retrievalWorkerUsesOwnConnection()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("retrieval-worker.db");
        {
            MemoryManager db(path);
            RequestMeta meta;
            meta.channel = Channel::kQQPrivate;
            meta.sessionId = meta.actorId = QStringLiteral("10101");
            meta.eventId = QStringLiteral("worker-turn");
            QVERIFY(db.saveQATurn(QStringLiteral("worker 测试内容"),
                                  QStringLiteral("{}"), {}, meta));
        }
        RetrievalRequest request;
        request.query = QStringLiteral("worker");
        request.terms = {QStringLiteral("worker")};
        request.subject = QStringLiteral("user");
        request.viewerPersonId = QStringLiteral("owner");
        request.viewerIsOwner = true;
        request.currentChannel = Channel::kQQPrivate;
        request.currentSession = QStringLiteral("10101");
        request.excludeCurrent = false;
        MemoryRetrievalWorker worker(path, request, {}, {}, {}, 0, {}, {}, {});
        QSignalSpy finished(&worker, &MemoryRetrievalWorker::finished);
        worker.run();
        QCOMPARE(finished.size(), 1);
        const auto result = finished.first().first().value<RetrievalResult>();
        QVERIFY(!result.evidence.isEmpty());
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
        QVERIFY(MemoryAgentPlanner::canExposeTools(pet, config));

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
                             {QStringLiteral("query"), QStringLiteral("ZQZ_NOT_A_MEMORY_9X")},
                             {QStringLiteral("kinds"), QJsonArray{QStringLiteral("chat_history")}}}}}};
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

    void evidenceAuditRequiresSentenceAccounting()
    {
        QJsonObject decision{{"sentences",QJsonArray{QJsonObject{{"zh_text","a"}},QJsonObject{{"zh_text","b"}}}}};
        QJsonObject evidence{{"reply/1",QJsonObject{{"speaker_role","assistant"}}},
                             {"source/1",QJsonObject{{"speaker_role","user"}}}};
        auto audit = auditExpressionClaims(decision,evidence);
        QCOMPARE(audit["claim_check"].toString(),QString("not_reported"));
        QCOMPARE(audit["unaccounted_sentences"].toArray().size(),2);
        QJsonObject claim{{"sentence_index",0},{"relation","reported_fact"},{"refs",QJsonArray{"reply/1"}}};
        decision["evidence_decision"] = QJsonObject{{"claims",QJsonArray{claim}}};
        QCOMPARE(auditExpressionClaims(decision,evidence)["claim_check"].toString(),QString("unsupported"));
        claim["refs"] = QJsonArray{"source/1"};
        decision["evidence_decision"] = QJsonObject{{"claims",QJsonArray{claim}}};
        QCOMPARE(auditExpressionClaims(decision,evidence)["claim_check"].toString(),QString("incomplete"));
        QJsonObject conversational{{"sentence_index",1},{"relation","conversational"},{"refs",QJsonArray{}}};
        decision["evidence_decision"] = QJsonObject{{"claims",QJsonArray{claim,conversational}}};
        audit = auditExpressionClaims(decision,evidence);
        QCOMPARE(audit["claim_check"].toString(),QString("structural_checks_passed"));
        QCOMPARE(audit["semantic_validation"].toString(),QString("not_performed"));
        conversational["sentence_index"] = 0.5;
        decision["evidence_decision"] = QJsonObject{{"claims",QJsonArray{claim,conversational}}};
        QCOMPARE(auditExpressionClaims(decision,evidence)["claim_check"].toString(),QString("unsupported"));
    }

    void executionAndCoverageAreIndependent()
    {
        MemoryAgentResult result;
        result.status = "complete";
        result.truncated = true;
        auto json = memoryAgentResultToJson(result);
        QCOMPARE(json["execution_status"].toString(), QString("complete"));
        QCOMPARE(json["coverage_status"].toString(), QString("unknown"));
        QVERIFY(json["truncated_by_limit"].toBool());
        QVERIFY(!json["partial"].toBool());
        result.status = "partial";
        QCOMPARE(memoryAgentResultToJson(result)["execution_status"].toString(), QString("degraded"));
        result.status = "failed";
        QCOMPARE(memoryAgentResultToJson(result)["execution_status"].toString(), QString("failed"));
    }

    void calendarRangeContractThroughRecall()
    {
        QTemporaryDir dir;
        MemoryManager db(dir.filePath("calendar.db"));
        const QDateTime now = QDateTime::fromString("2026-11-16T02:00:00Z", Qt::ISODate);
        auto scope = ownerGroupScope("30001", now);
        scope.referenceTime = now;
        scope.timezone = "Asia/Shanghai";
        scope.requireTimeFrame = true;
        for (const auto &group : {QString("30001"), QString("30002")}) {
            for (const auto &date : {QString("2026-11-08T23:59:59+08:00"),
                                    QString("2026-11-09T00:00:00+08:00"),
                                    QString("2026-11-15T23:59:59+08:00"),
                                    QString("2026-11-16T00:00:00+08:00")}) {
                const auto id = makeInteractionAt(db, group, QDateTime::fromString(date, Qt::ISODate),
                                                   "group discussion", group, "participant");
                QVERIFY(id >= 0);
                QVERIFY(db.saveInteractionSummary(id, group + date, "ready"));
            }
        }
        QJsonObject frame{{"anchor", QJsonObject{{"type","relative_week"},{"offset",-1}}},
                          {"span", QJsonObject{{"type","week_containing"}}}};
        QJsonObject time{{"expression","last week"},{"frame",frame}};
        QJsonObject need{{"id","one"},{"query","group discussion"},{"time",time}};
        QJsonObject plan{{"mode","overview"},{"needs",QJsonArray{need}}};
        MemoryRecallTool tool(&db, scope);
        QString error;
        const auto result = tool.execute(plan, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto coverage = result["coverage"].toObject();
        QCOMPARE(coverage["from"].toString(), QString("2026-11-09 00:00:00"));
        QCOMPARE(coverage["to"].toString(), QString("2026-11-16 00:00:00"));
        QCOMPARE(coverage["total_interactions"].toInt(), 2);
        QCOMPARE(result["execution_status"].toString(), QString("complete"));
        MemoryTimeFrame parsed;
        for (const auto &at : {QString("2027-01-01T01:00:00+08:00"),
                               QString("2026-03-01T12:00:00+08:00")}) {
            QVERIFY(MemoryTimeFrame::fromJson(frame, &parsed, &error));
            const auto anchor = QDateTime::fromString(at, Qt::ISODate);
            const auto resolved = TimeFrameResolver::resolve("last week", MemoryTimeTarget::MessageTime,
                                                             parsed, anchor, "Asia/Shanghai");
            QVERIFY(resolved.valid);
            QCOMPARE(resolved.from.date().dayOfWeek(), 1);
            QCOMPARE(resolved.from.date().daysTo(resolved.to.date()), 7);
            QVERIFY(resolved.to <= anchor);
        }
        frame["span"] = QJsonObject{{"type","day"}};
        QVERIFY(!MemoryTimeFrame::fromJson(frame, &parsed, &error));
        QCOMPARE(error, QString("weekday_required_for_day_span"));
        frame["anchor"] = QJsonObject{{"type","relative_week"},{"offset",-1},{"weekday",3}};
        QVERIFY(MemoryTimeFrame::fromJson(frame, &parsed, &error));
        auto resolved = TimeFrameResolver::resolve("last Wednesday", MemoryTimeTarget::MessageTime,
                                                   parsed, now, "Asia/Shanghai");
        QCOMPARE(resolved.from.date(), QDate(2026,11,11));
        frame = QJsonObject{{"anchor",QJsonObject{{"type","relative_month"},{"offset",-1}}},
                            {"span",QJsonObject{{"type","month_containing"}}}};
        QVERIFY(MemoryTimeFrame::fromJson(frame, &parsed, &error));
        resolved = TimeFrameResolver::resolve("last month", MemoryTimeTarget::MessageTime, parsed,
                                               QDateTime::fromString("2024-03-01T02:00:00Z",Qt::ISODate),
                                               "Asia/Shanghai");
        QCOMPARE(resolved.from.date(), QDate(2024,2,1));
        QCOMPARE(resolved.to.date(), QDate(2024,3,1));
        frame = QJsonObject{{"anchor",QJsonObject{{"type","recent_window"},{"minutes",10080}}}};
        QVERIFY(MemoryTimeFrame::fromJson(frame, &parsed, &error));
        resolved = TimeFrameResolver::resolve("past seven days", MemoryTimeTarget::MessageTime,
                                               parsed, now, "Asia/Shanghai");
        QCOMPARE(resolved.from, now.addDays(-7));
        frame["span"] = QJsonObject{{"type","week_containing"},{"before_days",7}};
        QVERIFY(!MemoryTimeFrame::fromJson(frame, &parsed, &error));
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
