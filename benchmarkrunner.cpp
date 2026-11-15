#include "benchmarkrunner.h"

#include "configmanager.h"
#include "llmservice.h"
#include "memorymanager.h"
#include "memorydashscope.h"
#include "memoryretrieval.h"
#include "recentcontext.h"

#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace {

const QString kTurn = QStringLiteral("\u8F6E\u6B21");
const QString kLogicalTime = QStringLiteral("\u903B\u8F91\u65F6\u95F4");
const QString kUserMessage = QStringLiteral("\u7528\u6237\u6D88\u606F");
const QString kAssistantReply = QStringLiteral("\u52A9\u624B\u56DE\u590D");

struct BenchTurn {
    int turn = 0;
    QDateTime logicalTime;
    QString user;
    QString assistant;
};

bool loadHistory(const QString &path, int cutoff, QList<BenchTurn> *turns, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open history: %1").arg(path);
        return false;
    }
    QList<BenchTurn> result;
    int line = 0;
    while (!file.atEnd()) {
        ++line;
        const QByteArray raw = file.readLine().trimmed();
        if (raw.isEmpty())
            continue;
        const auto object = QJsonDocument::fromJson(raw).object();
        BenchTurn turn;
        turn.turn = object.value(kTurn).toInt();
        turn.logicalTime = QDateTime::fromString(
            object.value(kLogicalTime).toString(), Qt::ISODate);
        turn.user = object.value(kUserMessage).toString();
        turn.assistant = object.value(kAssistantReply).toString();
        if (turn.turn <= 0 || !turn.logicalTime.isValid() || turn.user.isEmpty()) {
            if (error) *error = QStringLiteral("bad history line %1").arg(line);
            return false;
        }
        if (turn.turn <= cutoff)
            result.append(turn);
    }
    *turns = result;
    return !result.isEmpty();
}

QString replyJson(const QString &assistantText)
{
    QJsonObject sentence;
    sentence.insert(QStringLiteral("zh_text"), assistantText);
    sentence.insert(QStringLiteral("ja_text"), QString());
    QJsonObject root;
    root.insert(QStringLiteral("sentences"), QJsonArray{sentence});
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

RequestMeta turnMeta(const BenchTurn &turn, int index)
{
    RequestMeta meta;
    meta.eventId = QStringLiteral("bench-turn-%1").arg(index);
    meta.runId = QStringLiteral("bench-import-%1").arg(index);
    meta.receivedAtUtc = turn.logicalTime.toUTC();
    meta.occurredAtUtc = turn.logicalTime.toUTC();
    meta.channel = Channel::kQQPrivate;
    meta.sessionId = QStringLiteral("bench");
    meta.speaker = QStringLiteral("\u4E3B\u4EBA");
    meta.subject = QStringLiteral("user");
    meta.actorId = QStringLiteral("owner");
    meta.personId = QStringLiteral("owner");
    meta.persistTurn = true;
    meta.longTermMemoryEnabled = true;
    meta.turnKind = QStringLiteral("reply");
    return meta;
}

QJsonArray auditEvidence(const QList<MemoryEvidence> &items)
{
    QJsonArray result;
    for (int i = 0; i < items.size(); ++i) {
        const auto &item = items.at(i);
        const auto hash = QCryptographicHash::hash(
            item.text.toUtf8(), QCryptographicHash::Sha256).toHex();
        result.append(QJsonObject{
            {"rank", i + 1},
            {"handle", item.handle},
            {"source_ref", item.sourceRef},
            {"source_type", item.sourceType},
            {"speaker_role", item.speakerRole},
            {"derived", item.derived},
            {"turn_id", item.turnKey()},
            {"authority", item.toJson().value("authority").toString()},
            {"channel", item.channel},
            {"session", item.session},
            {"content_hash", QString::fromLatin1(hash)},
            {"text_length", item.text.size()}});
    }
    return result;
}

void saveExtraction(MemoryManager &memory, const QJsonObject &extraction,
                    qlonglong lastEndId, const QString &sourceIdsJson,
                    const RequestMeta &meta)
{    const auto updates = extraction.value(QStringLiteral("character_updates")).toArray();
    for (const auto &value : updates) {
        const auto object = value.toObject();
        const QString subject = object.value(QStringLiteral("subject")).toString(meta.subject);
        const QString key = object.value(QStringLiteral("key")).toString();
        const QString text = object.value(QStringLiteral("value")).toString();
        if (!subject.isEmpty() && !key.isEmpty() && !text.isEmpty())
            memory.upsertCharacterProfile(subject, key, text);
    }

    const QString visibility = meta.channel == Channel::kPet
        ? QStringLiteral("private") : QStringLiteral("channel");
    const auto episodes = extraction.value(QStringLiteral("episodic_memories")).toArray();
    for (const auto &value : episodes) {
        const auto object = value.toObject();
        const QString content = object.value(QStringLiteral("content")).toString();
        if (content.isEmpty())
            continue;
        const QDateTime eventTime = QDateTime::fromString(
            object.value(QStringLiteral("event_time")).toString(),
            QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        memory.addEpisodicMemory(
            content,
            object.value(QStringLiteral("type")).toString(QStringLiteral("event")),
            object.value(QStringLiteral("importance")).toDouble(0.5),
            eventTime, sourceIdsJson, meta.channel, visibility,
            object.value(QStringLiteral("subject")).toString(meta.subject),
            meta.sessionId);
    }

    const QString summary = extraction.value(QStringLiteral("working_summary")).toString();
    if (!summary.isEmpty())
        memory.addLongTermSummary(summary, lastEndId, sourceIdsJson,
                                  meta.channel, meta.sessionId);

    for (const auto &value : extraction.value(QStringLiteral("relationship_updates")).toArray()) {
        const auto object = value.toObject();
        const QString dimension = object.value(QStringLiteral("dimension")).toString();
        const double delta = object.value(QStringLiteral("delta")).toDouble();
        if (!dimension.isEmpty() && delta != 0.0)
            memory.upsertRelationshipState(dimension, delta, meta.subject);
    }
}

} // namespace

int runBenchmarkImport(const QStringList &args)
{
    if (args.size() < 5) {
        qCritical() << "usage: --bench-import <history.jsonl> <db> <cutoff>";
        return 2;
    }
    const QString historyPath = args.at(2);
    const QString dbPath = args.at(3);
    const int cutoff = args.at(4).toInt();
    QList<BenchTurn> turns;
    QString error;
    if (!loadHistory(historyPath, cutoff, &turns, &error)) {
        qCritical() << "[Bench] history:" << error;
        return 2;
    }

    MemoryManager memory(dbPath);
    LLMService llm(ConfigManager::instance().getApiKey());
    llm.setMemoryManager(&memory);

    for (int i = 0; i < turns.size(); ++i) {
        const auto meta = turnMeta(turns.at(i), i + 1);
        SentenceText sentence;
        sentence.zhText = turns.at(i).assistant;
        const QList<SentenceText> sentences{sentence};
        if (!memory.saveQATurn(turns.at(i).user, replyJson(turns.at(i).assistant),
                               sentences, meta, 0)) {
            qCritical() << "[Bench] save failed at turn" << turns.at(i).turn;
            return 2;
        }
    }

    int summaryThreshold = ConfigManager::instance().getSummaryMemoryLength();
    if (summaryThreshold <= 0)
        summaryThreshold = 15;

    while (true) {
        qlonglong lastEndId = -1;
        QList<qlonglong> sourceIds;
        QList<HistoryTurn> pending = memory.getUnsummarizedTurns(
            lastEndId, sourceIds, Channel::kQQPrivate, QStringLiteral("bench"), 0);
        if (pending.size() < summaryThreshold)
            break;
        pending = pending.mid(0, summaryThreshold);
        sourceIds = sourceIds.mid(0, summaryThreshold);
        lastEndId = pending.last().id;
        const auto meta = turnMeta(turns.at(qMin(turns.size() - 1,
                                                 sourceIds.size() - 1)), 0);
        QList<CharacterProfile> profiles = memory.getCharacterProfiles(meta.subject);
        profiles.append(memory.getCharacterProfiles(QStringLiteral("mako")));
        QList<EpisodicMemory> episodes = memory.getActiveEpisodicMemories(
            0.2, 20, meta.channel, QString(), meta.sessionId);

        QEventLoop loop;
        bool completed = false;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&llm, &LLMService::memoryExtractionReady, &loop,
                         [&](const QJsonObject &extraction, qlonglong endId,
                             const QString &sourceIdsJson, const RequestMeta &meta) {
            saveExtraction(memory, extraction, endId, sourceIdsJson, meta);
            completed = true;
            loop.quit();
        });
        timer.start(180000);
        llm.extractMemoryAsync(pending, lastEndId, sourceIds, profiles, episodes, meta);
        loop.exec();
        if (!completed) {
            qWarning() << "[Bench] extraction timeout/error at turn" << lastEndId;
            break;
        }
    }

    if (!memory.rebuildSearchIndex(&error)) {
        qCritical() << "[Bench] rebuild failed:" << error;
        return 2;
    }
    const auto retrieval = ConfigManager::instance().getMemoryPolicyConfig().retrieval;
    if (retrieval.embeddingProvider == QStringLiteral("dashscope")) {
        const QString key = !retrieval.embeddingApiKey.isEmpty()
            ? retrieval.embeddingApiKey
            : qEnvironmentVariable(retrieval.embeddingApiKeyEnv.toUtf8().constData());
        if (!key.isEmpty() && !retrieval.embeddingEndpoint.isEmpty()) {
            DashScopeEmbeddingProvider provider(
                QUrl(retrieval.embeddingEndpoint), key,
                retrieval.embeddingModel.isEmpty()
                    ? QStringLiteral("text-embedding-v4") : retrieval.embeddingModel,
                retrieval.embeddingDimension > 0 ? retrieval.embeddingDimension : 1024);
            if (!memory.embedSearchChunks(&provider, 2000, &error)) {
                qCritical() << "[Bench] embedding failed:" << error;
                return 2;
            }
        }
    }
    qInfo() << "[Bench] imported" << turns.size() << "turns, cutoff" << cutoff;
    return 0;
}

int runBenchmarkRetrieve(const QStringList &args)
{
    if (args.size() < 5) {
        qCritical() << "usage: --bench-retrieve <db> <question.json> <out.json> [--rerank] [--candidate-limit N] [--rerank-limit N]";
        return 2;
    }
    int candidateLimit = 200;
    const int limitIndex = args.indexOf(QStringLiteral("--candidate-limit"));
    if (limitIndex >= 0 && limitIndex + 1 < args.size())
        candidateLimit = qMax(1, args.at(limitIndex + 1).toInt());
    int rerankLimit = 10;
    const int rerankLimitIndex = args.indexOf(QStringLiteral("--rerank-limit"));
    if (rerankLimitIndex >= 0 && rerankLimitIndex + 1 < args.size())
        rerankLimit = qMax(1, args.at(rerankLimitIndex + 1).toInt());
    const bool withRerank = args.contains(QStringLiteral("--rerank"));
    const bool turnAggregation = args.contains(QStringLiteral("--turn-aggregation"));
    MemoryManager memory(args.at(2));
    QFile qfile(args.at(3));
    if (!qfile.open(QIODevice::ReadOnly)) {
        qCritical() << "[Bench] cannot open question";
        return 2;
    }
    const auto question = QJsonDocument::fromJson(qfile.readAll()).object();
    qfile.close();
    const QString text = question.value(QStringLiteral("question")).toString();
    if (text.isEmpty())
        return 2;

    RetrievalRequest request;
    request.query = text;
    request.purpose = QStringLiteral("bench.retrieve");
    request.subject = QStringLiteral("user");
    request.accounts = ConfigManager::instance().getMasterQQAccounts();
    request.viewerPersonId = QStringLiteral("owner");
    request.viewerIsOwner = true;
    request.terms = RecentContextPlanner::extractTerms(text.left(100));
    request.includeOthers = true;
    request.currentChannel = Channel::kQQPrivate;
    request.currentSession = QStringLiteral("bench");
    request.excludeCurrent = false;
    request.limit = 100;
    request.useIndex = true;
    request.allowLegacyFallback = true;
    request.candidateLimit = candidateLimit;
    request.rerankLimit = rerankLimit;
    request.turnAggregation = turnAggregation;

    MemoryRetrievalService service(&memory);
    const auto retrieval = ConfigManager::instance().getMemoryPolicyConfig().retrieval;
    if (retrieval.embeddingProvider == QStringLiteral("dashscope")) {
        const QString key = !retrieval.embeddingApiKey.isEmpty()
            ? retrieval.embeddingApiKey
            : qEnvironmentVariable(retrieval.embeddingApiKeyEnv.toUtf8().constData());
        if (!key.isEmpty() && !retrieval.embeddingEndpoint.isEmpty()) {
            service.setEmbeddingProvider(std::make_shared<DashScopeEmbeddingProvider>(
                QUrl(retrieval.embeddingEndpoint), key,
                retrieval.embeddingModel.isEmpty()
                    ? QStringLiteral("text-embedding-v4") : retrieval.embeddingModel,
                retrieval.embeddingDimension > 0 ? retrieval.embeddingDimension : 1024));
        }
    }
    if (withRerank) {
        const QString key = !retrieval.embeddingApiKey.isEmpty()
            ? retrieval.embeddingApiKey
            : qEnvironmentVariable(retrieval.embeddingApiKeyEnv.toUtf8().constData());
        if (!key.isEmpty() && !retrieval.rerankEndpoint.isEmpty()) {
            service.setReranker(std::make_shared<DashScopeReranker>(
                QUrl(retrieval.rerankEndpoint), key,
                retrieval.rerankModel.isEmpty()
                    ? QStringLiteral("qwen3-rerank") : retrieval.rerankModel));
        }
    }
    const auto result = service.retrieve(request);
    QFile out(args.at(4));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCritical() << "[Bench] cannot write output";
        return 2;
    }
    QJsonObject output = result.toJson();
    output.insert(QStringLiteral("terms"), QJsonArray::fromStringList(request.terms));
    output.insert(QStringLiteral("before_rerank"), auditEvidence(result.candidatesBeforeRerank));
    output.insert(QStringLiteral("after_rerank"), auditEvidence(result.candidatesAfterRerank));
    output.insert(QStringLiteral("reranker"), result.rerankerId);
    out.write(QJsonDocument(output).toJson(QJsonDocument::Indented));
    out.close();
    return 0;
}

int runBenchmarkAsk(const QStringList &args)
{
    if (args.size() < 5) {
        qCritical() << "usage: --bench-ask <db> <question.json> <out.json>";
        return 2;
    }
    const QString dbPath = args.at(2);
    const QString questionPath = args.at(3);
    const QString outputPath = args.at(4);

    QFile qfile(questionPath);
    if (!qfile.open(QIODevice::ReadOnly)) {
        qCritical() << "[Bench] cannot open question";
        return 2;
    }
    const auto question = QJsonDocument::fromJson(qfile.readAll()).object();
    qfile.close();
    const QString id = question.value(QStringLiteral("id")).toString();
    const QString text = question.value(QStringLiteral("question")).toString();
    if (id.isEmpty() || text.isEmpty()) {
        qCritical() << "[Bench] bad question";
        return 2;
    }

    MemoryManager memory(dbPath);
    LLMService llm(ConfigManager::instance().getApiKey());
    llm.setMemoryManager(&memory);
    llm.setBenchmarkChatOnly(args.contains(QStringLiteral("--strict-memory")));
    int maxTurn = question.value(QStringLiteral("cutoff_turn")).toInt();
    const int maxTurnIndex = args.indexOf(QStringLiteral("--max-turn"));
    if (maxTurnIndex >= 0 && maxTurnIndex + 1 < args.size())
        maxTurn = args.at(maxTurnIndex + 1).toInt();
    llm.setBenchmarkMaxTurn(maxTurn);

    RequestMeta meta;
    meta.eventId = QStringLiteral("bench-question-%1").arg(id);
    meta.runId = QStringLiteral("bench-answer-%1").arg(id);
    meta.receivedAtUtc = QDateTime::fromString(
        question.value(QStringLiteral("now")).toString(), Qt::ISODate).toUTC();
    meta.occurredAtUtc = meta.receivedAtUtc;
    meta.channel = QStringLiteral("qq_private");
    meta.sessionId = QStringLiteral("bench");
    meta.speaker = QStringLiteral("\u4E3B\u4EBA");
    meta.subject = QStringLiteral("user");
    meta.actorId = QStringLiteral("owner");
    meta.personId = QStringLiteral("owner");
    const int channelIndex = args.indexOf(QStringLiteral("--channel"));
    if (channelIndex >= 0 && channelIndex + 1 < args.size())
        meta.channel = args.at(channelIndex + 1);
    const int sessionIndex = args.indexOf(QStringLiteral("--session"));
    if (sessionIndex >= 0 && sessionIndex + 1 < args.size())
        meta.sessionId = args.at(sessionIndex + 1);
    const int subjectIndex = args.indexOf(QStringLiteral("--subject"));
    if (subjectIndex >= 0 && subjectIndex + 1 < args.size())
        meta.subject = args.at(subjectIndex + 1);
    const int actorIndex = args.indexOf(QStringLiteral("--actor"));
    if (actorIndex >= 0 && actorIndex + 1 < args.size()) {
        meta.actorId = args.at(actorIndex + 1);
        meta.personId = meta.actorId;
    }
    const int speakerIndex = args.indexOf(QStringLiteral("--speaker"));
    if (speakerIndex >= 0 && speakerIndex + 1 < args.size())
        meta.speaker = args.at(speakerIndex + 1);
    meta.persistTurn = false;
    meta.longTermMemoryEnabled = true;
    meta.turnKind = QStringLiteral("reply");

    QString answer;
    QString error;
    bool timedOut = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(240000);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    QObject::connect(&llm, &LLMService::sentencesReady, &loop,
                     [&](const QList<SentenceText> &sentences, const QString &,
                         const QString &, const RequestMeta &) {
        QStringList parts;
        for (const auto &sentence : sentences)
            parts.append(sentence.zhText);
        answer = parts.join(QString());
        loop.quit();
    });
    QObject::connect(&llm, &LLMService::replyAbnormal, &loop,
                     [&](const QString &reason) {
        error = reason;
        loop.quit();
    });
    QObject::connect(&llm, &LLMService::internetErrorSignal, &loop,
                     [&](const QString &message) {
        error = message;
        loop.quit();
    });
    llm.askDeepSeek(text, {}, meta);
    loop.exec();

    QJsonObject output;
    output.insert(QStringLiteral("id"), id);
    output.insert(QStringLiteral("question"), text);
    output.insert(QStringLiteral("answer"), answer);
    output.insert(QStringLiteral("error"), error);
    output.insert(QStringLiteral("timed_out"), timedOut);
    const auto retrieval = llm.lastRetrievalResult();
    output.insert(QStringLiteral("retrieval"), retrieval.toJson());
    output.insert(QStringLiteral("before_rerank"), auditEvidence(retrieval.candidatesBeforeRerank));
    output.insert(QStringLiteral("after_rerank"), auditEvidence(retrieval.candidatesAfterRerank));
    output.insert(QStringLiteral("reranker"), retrieval.rerankerId);
    output.insert(QStringLiteral("usage"), llm.lastResponseUsage());
    output.insert(QStringLiteral("last_request"), llm.lastRequestMessages());
    QFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCritical() << "[Bench] cannot write output";
        return 2;
    }
    out.write(QJsonDocument(output).toJson(QJsonDocument::Indented));
    out.close();
    return answer.isEmpty() ? 3 : 0;
}

