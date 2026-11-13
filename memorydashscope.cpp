#include "memorydashscope.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <cmath>

namespace {

QJsonObject postJson(const QUrl &endpoint, const QString &apiKey,
                     const QJsonObject &body, QString *error, int timeoutMs = 15000)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    auto *reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (reply->isRunning())
            reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    const QByteArray payload = reply->readAll();
    const auto networkError = reply->error();
    const auto networkText = reply->errorString();
    reply->deleteLater();
    if (networkError != QNetworkReply::NoError) {
        if (error) *error = networkText;
        return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = parseError.errorString();
        return {};
    }
    return document.object();
}

} // namespace

DashScopeEmbeddingProvider::DashScopeEmbeddingProvider(const QUrl &endpoint,
                                                       const QString &apiKey,
                                                       const QString &model,
                                                       int dimension)
    : m_endpoint(endpoint), m_apiKey(apiKey), m_model(model),
      m_dimension(qMax(1, dimension))
{
}

QString DashScopeEmbeddingProvider::modelId() const
{
    return QStringLiteral("dashscope:%1:%2").arg(m_model).arg(m_dimension);
}

int DashScopeEmbeddingProvider::dimension() const
{
    return m_dimension;
}

bool DashScopeEmbeddingProvider::embed(const QString &text, QVector<float> *vector,
                                       QString *error) const
{
    QVector<QVector<float>> vectors;
    if (!embedBatch(QStringList{text}, &vectors, error) || vectors.isEmpty())
        return false;
    if (vector)
        *vector = vectors.first();
    return true;
}

bool DashScopeEmbeddingProvider::embedBatch(const QStringList &texts,
                                             QVector<QVector<float>> *vectors,
                                             QString *error) const
{
    if (!vectors || texts.isEmpty()) {
        if (error) *error = QStringLiteral("empty embedding input");
        return false;
    }
    if (m_endpoint.isEmpty() || m_apiKey.isEmpty()) {
        if (error) *error = QStringLiteral("DashScope endpoint or api key missing");
        return false;
    }
    QJsonArray input;
    for (const auto &text : texts) {
        if (text.trimmed().isEmpty()) {
            if (error) *error = QStringLiteral("empty embedding input");
            return false;
        }
        input.append(text);
    }
    const auto response = postJson(m_endpoint, m_apiKey,
        QJsonObject{{"model", m_model},
                    {"input", input},
                    {"dimensions", m_dimension},
                    {"encoding_format", QStringLiteral("float")}}, error);
    const auto data = response.value(QStringLiteral("data")).toArray();
    if (data.size() != texts.size()) {
        if (error && error->isEmpty())
            *error = QStringLiteral("DashScope embedding response count mismatch: %1 != %2")
                         .arg(data.size()).arg(texts.size());
        return false;
    }
    QVector<QVector<float>> result;
    result.reserve(data.size());
    for (const auto &item : data) {
        const auto values = item.toObject().value(QStringLiteral("embedding")).toArray();
        if (values.size() != m_dimension) {
            if (error) *error = QStringLiteral("DashScope embedding dimension mismatch: %1 != %2")
                                     .arg(values.size()).arg(m_dimension);
            return false;
        }
        QVector<float> row(m_dimension);
        for (int i = 0; i < values.size(); ++i) {
            const double value = values.at(i).toDouble();
            if (!std::isfinite(value)) {
                if (error) *error = QStringLiteral("DashScope embedding contains non-finite value");
                return false;
            }
            row[i] = float(value);
        }
        result.append(row);
    }
    *vectors = result;
    return true;
}

DashScopeReranker::DashScopeReranker(const QUrl &endpoint, const QString &apiKey,
                                     const QString &model)
    : m_endpoint(endpoint), m_apiKey(apiKey), m_model(model)
{
}

QString DashScopeReranker::id() const
{
    return QStringLiteral("dashscope:%1").arg(m_model);
}

QList<MemoryEvidence> DashScopeReranker::rerank(const QString &query,
                                                const QList<MemoryEvidence> &candidates,
                                                int limit, QString *error) const
{
    if (candidates.isEmpty() || limit <= 0)
        return {};
    if (m_endpoint.isEmpty() || m_apiKey.isEmpty()) {
        if (error) *error = QStringLiteral("DashScope rerank endpoint or api key missing");
        return {};
    }
    QJsonArray documents;
    for (const auto &candidate : candidates)
        documents.append(candidate.text);
    const auto response = postJson(m_endpoint, m_apiKey,
        QJsonObject{{"model", m_model},
                    {"input", QJsonObject{{"query", query}, {"documents", documents}}},
                    {"parameters", QJsonObject{{"top_n", limit},
                                                {"return_documents", false}}}}, error);
    const auto results = response.value(QStringLiteral("output")).toObject()
                             .value(QStringLiteral("results")).toArray();
    QList<MemoryEvidence> reranked;
    for (const auto &value : results) {
        const int index = value.toObject().value(QStringLiteral("index")).toInt(-1);
        if (index >= 0 && index < candidates.size())
            reranked.append(candidates.at(index));
        if (reranked.size() >= limit)
            break;
    }
    if (reranked.isEmpty() && error && error->isEmpty())
        *error = QStringLiteral("DashScope rerank returned no usable results");
    return reranked;
}
