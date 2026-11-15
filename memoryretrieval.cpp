#include "memoryretrieval.h"

#include "memorymanager.h"
#include "configmanager.h"

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QtMath>

#include <algorithm>
#include <cstring>

namespace {

bool sourceAllowed(const RetrievalRequest &request, const MemoryEvidence &item)
{
    if (!request.sourceKinds.isEmpty()
        && !request.sourceKinds.contains(item.sourceType))
        return false;
    if (!request.includeOthers && !item.own && !request.accounts.contains(item.speakerId)
        && !(item.speakerId.startsWith("qq:") && request.accounts.contains(item.speakerId.mid(3))))
        return false;
    const auto time = item.eventTime.isValid() ? item.eventTime : item.speechAt;
    if ((request.from.isValid() && (!time.isValid() || time < request.from))
        || (request.until.isValid() && (!time.isValid() || time >= request.until)))
        return false;
    if (request.accept && !request.accept(item))
        return false;
    if (request.allowedSources.isEmpty())
        return true;
    for (const auto &source : request.allowedSources) {
        const QString channel = source.section(QLatin1Char('|'), 0, 0);
        const QString session = source.section(QLatin1Char('|'), 1, 1);
        if (channel == item.channel
            && (session == QStringLiteral("*") || session == item.session))
            return true;
    }
    return false;
}

QByteArray normalizedEvidenceText(const MemoryEvidence &item)
{
    QByteArray key = item.text.toUtf8();
    key = key.simplified();
    return key;
}

QList<int> rankOrder(const QVector<double> &scores)
{
    QList<int> order;
    for (int i = 0; i < scores.size(); ++i)
        order.append(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return scores.at(a) > scores.at(b);
    });
    return order;
}

int roleOrder(const MemoryEvidence &item)
{
    if (item.speakerRole == QStringLiteral("user"))
        return 0;
    if (item.speakerRole == QStringLiteral("assistant"))
        return 1;
    return 2;
}

// 检索按 turn 聚合，但返回的仍是带 speaker/role/source 的 message 列表。
QList<MemoryEvidence> groupByTurn(const QList<MemoryEvidence> &input,
                                  const QHash<QString, double> &turnScore)
{
    if (input.isEmpty())
        return {};
    QHash<QString, QList<MemoryEvidence>> groups;
    QStringList firstSeen;
    for (const auto &item : input) {
        const QString key = item.turnKey();
        if (!groups.contains(key))
            firstSeen.append(key);
        groups[key].append(item);
    }
    std::stable_sort(firstSeen.begin(), firstSeen.end(), [&](const QString &a, const QString &b) {
        if (turnScore.contains(a) || turnScore.contains(b)) {
            const double sa = turnScore.value(a);
            const double sb = turnScore.value(b);
            if (sa != sb)
                return sa > sb;
        }
        return false;
    });
    QList<MemoryEvidence> result;
    for (const auto &key : firstSeen) {
        auto items = groups.value(key);
        std::stable_sort(items.begin(), items.end(), [](const MemoryEvidence &a,
                                                         const MemoryEvidence &b) {
            const int oa = roleOrder(a);
            const int ob = roleOrder(b);
            if (oa != ob)
                return oa < ob;
            return false;
        });
        result.append(items);
    }
    return result;
}

QStringList bm25Tokens(const QString &text)
{
    static const QSet<QString> stopWords = {
        QStringLiteral("的"), QStringLiteral("了"), QStringLiteral("是"), QStringLiteral("在"),
        QStringLiteral("和"), QStringLiteral("就"), QStringLiteral("都"), QStringLiteral("而"),
        QStringLiteral("及"), QStringLiteral("与"), QStringLiteral("着"), QStringLiteral("过"),
        QStringLiteral("也"), QStringLiteral("不"), QStringLiteral("有"), QStringLiteral("这"),
        QStringLiteral("那"), QStringLiteral("我"), QStringLiteral("你"), QStringLiteral("他"),
        QStringLiteral("她"), QStringLiteral("它"), QStringLiteral("们"), QStringLiteral("上"),
        QStringLiteral("下"), QStringLiteral("里"), QStringLiteral("会"), QStringLiteral("要"),
        QStringLiteral("把"), QStringLiteral("被"), QStringLiteral("让"), QStringLiteral("给"),
        QStringLiteral("对"), QStringLiteral("从"), QStringLiteral("到"), QStringLiteral("为"),
        QStringLiteral("以"), QStringLiteral("之"), QStringLiteral("其"), QStringLiteral("或"),
        QStringLiteral("很"), QStringLiteral("太"), QStringLiteral("更"), QStringLiteral("最"),
        QStringLiteral("又"), QStringLiteral("还"), QStringLiteral("再"), QStringLiteral("只"),
        QStringLiteral("才"), QStringLiteral("能"), QStringLiteral("可"), QStringLiteral("没"),
        QStringLiteral("想"), QStringLiteral("说"), QStringLiteral("做"), QStringLiteral("一"),
        QStringLiteral("个"), QStringLiteral("什么"), QStringLiteral("怎么"), QStringLiteral("为什么")
    };
    QStringList output;
    QString hanRun;
    QString asciiRun;
    auto flushHan = [&] {
        if (hanRun.isEmpty())
            return;
        if (hanRun.size() == 1) {
            if (!stopWords.contains(hanRun))
                output.append(hanRun);
        } else {
            for (int i = 0; i < hanRun.size(); ++i) {
                const QString single(hanRun.at(i));
                if (!stopWords.contains(single))
                    output.append(single);
                if (i + 1 < hanRun.size()) {
                    const QString bigram = hanRun.mid(i, 2);
                    if (!stopWords.contains(bigram))
                        output.append(bigram);
                }
            }
        }
        hanRun.clear();
    };
    auto flushAscii = [&] {
        if (asciiRun.size() >= 2)
            output.append(asciiRun.toLower());
        asciiRun.clear();
    };
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        if (code >= 0x4E00 && code <= 0x9FFF) {
            flushAscii();
            hanRun.append(ch);
        } else if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
                   || (code >= '0' && code <= '9') || code == '_') {
            flushHan();
            asciiRun.append(ch);
        } else {
            flushHan();
            flushAscii();
        }
    }
    flushHan();
    flushAscii();
    return output;
}

QVector<double> lexicalScores(const QList<MemoryEvidence> &evidence,
                              const QStringList &terms)
{
    QVector<double> scores(evidence.size(), 0.0);
    if (evidence.isEmpty() || terms.isEmpty())
        return scores;
    const double avgLength = [&] {
        double total = 0.0;
        for (const auto &item : evidence)
            total += qMax(1, item.text.size());
        return total / evidence.size();
    }();
    QHash<QString, int> documentFrequency;
    for (const auto &term : terms) {
        int count = 0;
        for (const auto &item : evidence)
            if (item.text.contains(term, Qt::CaseInsensitive))
                ++count;
        documentFrequency.insert(term, count);
    }
    for (int i = 0; i < evidence.size(); ++i) {
        const double length = qMax(1, evidence.at(i).text.size());
        double score = 0.0;
        for (const auto &term : terms) {
            const int tf = evidence.at(i).text.count(term, Qt::CaseInsensitive);
            if (tf <= 0)
                continue;
            const int df = documentFrequency.value(term);
            const double idf = qLn(1.0 + (evidence.size() - df + 0.5) / (df + 0.5));
            const double denominator = tf + 1.2 * (1.0 - 0.75 + 0.75 * length / avgLength);
            score += idf * (tf * 2.2) / denominator;
        }
        scores[i] = score;
    }
    return scores;
}

QVector<float> blobToVector(const QByteArray &blob)
{
    if (blob.isEmpty() || blob.size() % int(sizeof(float)) != 0)
        return {};
    QVector<float> vector(blob.size() / int(sizeof(float)));
    std::memcpy(vector.data(), blob.constData(), size_t(blob.size()));
    return vector;
}

double cosine(const QVector<float> &a, const QVector<float> &b)
{
    if (a.isEmpty() || a.size() != b.size())
        return -1.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        dot += double(a.at(i)) * b.at(i);
        na += double(a.at(i)) * a.at(i);
        nb += double(b.at(i)) * b.at(i);
    }
    if (na <= 0.0 || nb <= 0.0)
        return -1.0;
    return dot / (qSqrt(na) * qSqrt(nb));
}

qlonglong chunkId(const QString &handle)
{
    if (!handle.startsWith(QStringLiteral("chunk/")))
        return 0;
    return handle.mid(QStringLiteral("chunk/").size()).toLongLong();
}

} // namespace

QList<MemoryEvidence> IdentityMemoryReranker::rerank(const QString &,
                                                     const QList<MemoryEvidence> &candidates,
                                                     int limit, QString *) const
{
    return candidates.mid(0, qMax(0, limit));
}

QList<MemoryEvidence> HeuristicMemoryReranker::rerank(const QString &query,
                                                      const QList<MemoryEvidence> &candidates,
                                                      int limit, QString *) const
{
    if (candidates.isEmpty() || limit <= 0)
        return {};
    const QStringList rawTerms = bm25Tokens(query);
    QStringList terms;
    QSet<QString> seenTerms;
    for (const auto &term : rawTerms) {
        if (term.isEmpty() || seenTerms.contains(term))
            continue;
        seenTerms.insert(term);
        terms.append(term);
    }
    QList<QPair<double, int>> scored;
    const QDateTime now = QDateTime::currentDateTime();
    const QString exactQuery = query.trimmed();
    for (int i = 0; i < candidates.size(); ++i) {
        double score = 1.0 / (60.0 + i + 1.0);
        if (!exactQuery.isEmpty()
            && candidates.at(i).text.contains(exactQuery, Qt::CaseInsensitive)) {
            // 原样命中优先，避免旧消息被近因/词项噪声挤掉。
            score += 1.0;
        }
        for (const auto &term : terms) {
            if (term.size() >= 2) {
                if (candidates.at(i).text.contains(term, Qt::CaseInsensitive))
                    score += 0.08;
            } else if (candidates.at(i).text.contains(term, Qt::CaseInsensitive)) {
                // 单字兜底：比 bigram 弱，但比完全没有词面信号强。
                score += 0.02;
            }
        }
        if (candidates.at(i).speechAt.isValid()) {
            const double days = qMax(0.0, candidates.at(i).speechAt.secsTo(now) / 86400.0);
            score += 0.02 * qExp(-days / 30.0);
        }
        scored.append({score, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    QList<MemoryEvidence> result;
    for (const auto &entry : scored) {
        if (result.size() >= limit)
            break;
        result.append(candidates.at(entry.second));
    }
    return result;
}

MemoryRetrievalService::MemoryRetrievalService(MemoryManager *manager,
                                               std::shared_ptr<MemoryEmbeddingProvider> embedding,
                                               std::shared_ptr<MemoryReranker> reranker)
    : m_embedding(std::move(embedding)), m_reranker(std::move(reranker))
{
    if (!m_reranker)
        m_reranker = std::make_shared<HeuristicMemoryReranker>();
    setMemoryManager(manager);
}

void MemoryRetrievalService::setMemoryManager(MemoryManager *manager)
{
    m_manager = manager;
}

void MemoryRetrievalService::setEmbeddingProvider(
    std::shared_ptr<MemoryEmbeddingProvider> embedding)
{
    m_embedding = std::move(embedding);
}

void MemoryRetrievalService::setReranker(std::shared_ptr<MemoryReranker> reranker)
{
    m_reranker = std::move(reranker);
}

QList<MemoryEvidence> MemoryRetrievalService::retrieveIndexed(const RetrievalRequest &request,
                                                             RetrievalResult *result) const
{
    QList<MemoryEvidence> candidates;
    if (!m_manager)
        return candidates;
    QHash<QString, int> indexByHandle;
    auto addCandidate = [&](const MemoryEvidence &evidence) {
        if (evidence.handle.isEmpty() || evidence.text.trimmed().isEmpty())
            return false;
        if (!sourceAllowed(request, evidence))
            return false;
        if (request.excludeCurrent && evidence.channel == request.currentChannel
            && evidence.session == request.currentSession)
            return false;
        if (indexByHandle.contains(evidence.handle))
            return false;
        indexByHandle.insert(evidence.handle, candidates.size());
        candidates.append(evidence);
        return true;
    };

    const bool original = request.sourceKinds.isEmpty() || request.sourceKinds.contains("chat")
        || request.sourceKinds.contains("raw");
    const auto allRows = original && request.useIndex ? m_manager->memoryChunksForEmbedding(0) : QJsonArray{};
    QJsonArray lexicalRows = allRows;
    if (request.includeAuxiliarySources) {
        const auto structured = m_manager->structuredMemoryRows(request.sourceKinds, request.subject,
                                                               request.includeOthers, request.includeAllEpisodic);
        for (const auto &row : structured)
            lexicalRows.append(row);
    }
    result->backend = QStringLiteral("index");
    result->truncated = false;

    QStringList queryTokens = bm25Tokens(request.query);
    if (queryTokens.isEmpty())
        queryTokens = request.terms;
    QSet<QString> queryTokenSet;
    QStringList uniqueQueryTokens;
    for (const auto &token : queryTokens) {
        if (!token.isEmpty() && !queryTokenSet.contains(token)) {
            queryTokenSet.insert(token);
            uniqueQueryTokens.append(token);
        }
    }

    struct Bm25Doc {
        MemoryEvidence evidence;
        int length = 1;
        QHash<QString, int> tf;
        double score = 0.0;
    };
    QList<Bm25Doc> docs;
    QHash<QString, int> documentFrequency;
    for (const auto &row : lexicalRows) {
        const auto evidence = MemoryEvidence::fromChunk(
            row.toObject(), request.viewerPersonId, request.viewerIsOwner);
        if (evidence.handle.isEmpty() || evidence.text.trimmed().isEmpty())
            continue;
        if (!sourceAllowed(request, evidence))
            continue;
        if (request.excludeCurrent && evidence.channel == request.currentChannel
            && evidence.session == request.currentSession)
            continue;
        Bm25Doc doc;
        doc.evidence = evidence;
        doc.length = qMax(1, evidence.text.size());
        if (!uniqueQueryTokens.isEmpty()) {
            const auto tokens = bm25Tokens(evidence.text);
            for (const auto &token : tokens)
                doc.tf[token] += 1;
            for (const auto &token : uniqueQueryTokens) {
                if (doc.tf.value(token) > 0)
                    documentFrequency[token] += 1;
            }
        }
        docs.append(doc);
    }

    double averageLength = 1.0;
    if (!docs.isEmpty()) {
        double totalLength = 0.0;
        for (const auto &doc : docs)
            totalLength += doc.length;
        averageLength = totalLength / docs.size();
    }
    const double k1 = 1.2;
    const double b = 0.75;
    for (auto &doc : docs) {
        const double lengthNorm = 1.0 - b + b * (doc.length / averageLength);
        double score = 0.0;
        for (const auto &token : uniqueQueryTokens) {
            const int tf = doc.tf.value(token);
            if (tf <= 0)
                continue;
            const int df = documentFrequency.value(token);
            const double idf = qLn(1.0 + (docs.size() - df + 0.5) / (df + 0.5));
            score += idf * (tf * (k1 + 1.0)) / (tf + k1 * lengthNorm);
        }
        doc.score = score;
        if (score > 0.0)
            ++result->lexicalCount;
    }
    std::stable_sort(docs.begin(), docs.end(), [](const Bm25Doc &a, const Bm25Doc &b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.evidence.speechAt != b.evidence.speechAt)
            return a.evidence.speechAt > b.evidence.speechAt;
        return a.evidence.handle < b.evidence.handle;
    });
    QHash<QString, double> lexicalScore;
    const int lexicalCap = qMax(1, request.candidateLimit);
    for (int i = 0; i < docs.size() && i < lexicalCap; ++i) {
        if (request.requireTermMatch && docs.at(i).score <= 0 && !request.includeUnmatched)
            continue;
        if (addCandidate(docs.at(i).evidence))
            lexicalScore.insert(docs.at(i).evidence.handle, docs.at(i).score);
    }

    QHash<QString, double> vectorScore;
    bool vectorUsed = false;
    if (m_embedding && !allRows.isEmpty()) {
        QVector<float> query;
        QString embedError;
        if (!m_embedding->embed(request.query, &query, &embedError)) {
            result->status = QStringLiteral("degraded");
        } else {
            vectorUsed = true;
            // 独立的向量召回：不以 LIKE 命中为前提，扫描当前活跃 chunk 的向量。
            for (const auto &row : allRows) {
                const auto value = row.toObject();
                const auto id = value.value(QStringLiteral("id")).toInteger();
                const auto blob = m_manager->chunkEmbedding(id, m_embedding->modelId());
                const auto stored = blobToVector(blob);
                if (stored.size() != query.size())
                    continue;
                const auto evidence = MemoryEvidence::fromChunk(
                    value, request.viewerPersonId, request.viewerIsOwner);
                if (evidence.handle.isEmpty())
                    continue;
                if (!sourceAllowed(request, evidence))
                    continue;
                const double score = cosine(query, stored);
                if (score < 0.0)
                    continue;
                vectorScore.insert(evidence.handle, score);
                ++result->vectorCount;
                addCandidate(evidence);
            }
        }
    }

    QList<QString> lexicalHandles = lexicalScore.keys();
    std::stable_sort(lexicalHandles.begin(), lexicalHandles.end(),
                     [&](const QString &a, const QString &b) {
                         if (lexicalScore.value(a) == lexicalScore.value(b)) return a < b;
                         return lexicalScore.value(a) > lexicalScore.value(b);
                     });
    lexicalHandles = lexicalHandles.mid(0, qMax(1, request.candidateLimit));

    QList<QString> vectorHandles = vectorScore.keys();
    std::stable_sort(vectorHandles.begin(), vectorHandles.end(),
                     [&](const QString &a, const QString &b) {
                         if (vectorScore.value(a) == vectorScore.value(b)) return a < b;
                         return vectorScore.value(a) > vectorScore.value(b);
                     });
    vectorHandles = vectorHandles.mid(0, qMax(1, request.candidateLimit));

    QHash<QString, double> rrf;
    const double k = 60.0;
    for (int rank = 0; rank < lexicalHandles.size(); ++rank) {
        if (lexicalScore.value(lexicalHandles.at(rank)) > 0.0)
            rrf[lexicalHandles.at(rank)] += 1.0 / (k + rank + 1.0);
    }
    if (vectorUsed) {
        for (int rank = 0; rank < vectorHandles.size(); ++rank)
            rrf[vectorHandles.at(rank)] += 1.0 / (k + rank + 1.0);
    }

    QList<int> finalOrder;
    for (int i = 0; i < candidates.size(); ++i)
        finalOrder.append(i);
    std::stable_sort(finalOrder.begin(), finalOrder.end(), [&](int a, int b) {
        const double sa = rrf.value(candidates.at(a).handle);
        const double sb = rrf.value(candidates.at(b).handle);
        if (sa != sb)
            return sa > sb;
        return lexicalScore.value(candidates.at(a).handle)
            > lexicalScore.value(candidates.at(b).handle);
    });
    QList<MemoryEvidence> ranked;
    for (int index : finalOrder)
        ranked.append(candidates.at(index));

    QHash<QString, double> turnScore;
    QHash<QString, QList<MemoryEvidence>> turnItems;
    QStringList turnOrder;
    if (request.turnAggregation) {
        for (const auto &item : ranked) {
            const QString key = item.turnKey();
            turnScore[key] = qMax(turnScore.value(key), rrf.value(item.handle));
        }
        ranked = groupByTurn(ranked, turnScore);
        for (const auto &item : ranked) {
            const QString key = item.turnKey();
            if (!turnItems.contains(key))
                turnOrder.append(key);
            turnItems[key].append(item);
        }
    }
    const int rerankCap = qMax(1, request.candidateLimit);
    QList<MemoryEvidence> rerankInput;
    QSet<QByteArray> seenText;
    if (request.turnAggregation) {
        for (const auto &key : turnOrder) {
            const auto items = turnItems.value(key);
            if (items.isEmpty())
                continue;
            MemoryEvidence representative = items.first();
            for (const auto &item : items) {
                if (item.speakerRole == QStringLiteral("user")) {
                    representative = item;
                    break;
                }
            }
            const QByteArray textKey = normalizedEvidenceText(representative);
            if (textKey.isEmpty() || seenText.contains(textKey))
                continue;
            seenText.insert(textKey);
            rerankInput.append(representative);
            if (rerankInput.size() >= rerankCap)
                break;
        }
    } else {
        for (const auto &item : ranked) {
            const QByteArray textKey = normalizedEvidenceText(item);
            if (textKey.isEmpty() || seenText.contains(textKey))
                continue;
            seenText.insert(textKey);
            rerankInput.append(item);
            if (rerankInput.size() >= rerankCap)
                break;
        }
    }
    result->candidateCount = ranked.size();
    result->truncated = ranked.size() > qMin(request.limit, request.rerankLimit);
    result->candidatesBeforeRerank = rerankInput;
    result->rerankerId = m_reranker ? m_reranker->id() : QString();

    if (m_reranker && !rerankInput.isEmpty()) {
        QString rerankError;
        const auto reranked = m_reranker->rerank(request.query, rerankInput,
                                                 request.rerankLimit, &rerankError);
        if (!rerankError.isEmpty() || reranked.isEmpty()) {
            result->status = QStringLiteral("degraded");
            result->degradedReason = rerankError.isEmpty()
                ? QStringLiteral("rerank returned no usable results")
                : rerankError;
            // 重排是增强项，失败时保留重排前可用的候选。
            result->candidatesAfterRerank = rerankInput.mid(0, qMax(0, request.rerankLimit));
            return result->candidatesAfterRerank;
        }
        QList<MemoryEvidence> expanded;
        QSet<QString> expandedHandles;
        if (request.turnAggregation) {
            for (const auto &item : reranked) {
                const auto group = turnItems.value(item.turnKey(), {item});
                for (const auto &message : group) {
                    if (expandedHandles.contains(message.handle))
                        continue;
                    expandedHandles.insert(message.handle);
                    expanded.append(message);
                }
            }
        } else {
            expanded = reranked;
        }
        result->candidatesAfterRerank = expanded.mid(0, qMax(0, request.rerankLimit));
    } else {
        result->candidatesAfterRerank = rerankInput.mid(0, qMax(0, request.rerankLimit));
    }
    if (request.includeUnmatched) {
        MemoryEvidence newest, newestMatch;
        const auto terms = request.terms.isEmpty() ? QStringList{request.query} : request.terms;
        for (const auto &doc : docs) {
            const auto &item = doc.evidence;
            if (!item.speechAt.isValid()) continue;
            if (!newest.speechAt.isValid() || newest.speechAt < item.speechAt) newest = item;
            if ((!newestMatch.speechAt.isValid() || newestMatch.speechAt < item.speechAt)
                && std::any_of(terms.begin(), terms.end(), [&](const QString &term) {
                    return !term.isEmpty() && item.text.contains(term, Qt::CaseInsensitive);
                })) newestMatch = item;
        }
        const int budget = qMin(request.rerankLimit, request.limit);
        if (budget >= 2) {
            auto &items = result->candidatesAfterRerank;
            QList<MemoryEvidence> overview;
            for (const auto &item : {newestMatch, newest})
                if (!item.handle.isEmpty() && (overview.isEmpty() || overview.first().handle != item.handle))
                    overview.append(item);
            for (const auto &item : items)
                if (item.handle != newest.handle && item.handle != newestMatch.handle) overview.append(item);
            items = overview.mid(0, budget);
        }
    }
    return result->candidatesAfterRerank;
}

RetrievalResult MemoryRetrievalService::retrieve(const RetrievalRequest &request) const
{
    RetrievalResult result;
    QElapsedTimer timer;
    timer.start();
    if (!m_manager) {
        result.status = QStringLiteral("degraded");
        result.degradedReason = QStringLiteral("database unavailable");
        return result;
    }
    auto scoped = request;
    const auto rules = ConfigManager::instance().getMemoryPolicyConfig().privacyRules;
    const auto blindZones = ConfigManager::instance().blindZones();
    scoped.accept = [&](const MemoryEvidence &entry) {
        for (const auto &zone : blindZones)
            if (zone.channel == entry.channel && zone.sessionId == entry.session)
                return false;
        const auto scan = SensitiveContentFilter::sanitize(entry.text, {}, rules);
        return !scan.blocked && !scan.redacted && (!request.accept || request.accept(entry));
    };
    auto ranked = retrieveIndexed(scoped, &result);
    result.truncated |= ranked.size() > request.limit;
    result.evidence = ranked.mid(0, qMax(0, request.limit));
    if (result.truncated && result.status == "complete") result.status = "partial";
    for (const auto &item : result.evidence)
        if (!item.handle.startsWith("chunk/"))
            ++result.auxiliaryCount;
    result.latencyMs = timer.elapsed();
    return result;
}
