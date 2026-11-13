#include "memoryembedding.h"

#include <QHash>
#include <QtMath>

LocalHashEmbeddingProvider::LocalHashEmbeddingProvider(int dimension)
    : m_dimension(qMax(8, dimension))
{
}

QString LocalHashEmbeddingProvider::modelId() const
{
    return QStringLiteral("local_hash_test_%1").arg(m_dimension);
}

int LocalHashEmbeddingProvider::dimension() const
{
    return m_dimension;
}

bool LocalHashEmbeddingProvider::embed(const QString &text, QVector<float> *vector,
                                       QString *error) const
{
    if (!vector) {
        if (error) *error = QStringLiteral("vector is null");
        return false;
    }
    vector->fill(0.0f, m_dimension);
    const QString normalized = text.simplified().toCaseFolded();
    for (int i = 0; i < normalized.size(); ++i) {
        const QString gram = (i + 1 < normalized.size())
            ? normalized.mid(i, 2) : normalized.mid(i, 1);
        const uint hash = qHash(gram, 0x9e3779b9u);
        const int slot = int(hash % uint(m_dimension));
        const float sign = (hash & 0x80000000u) ? -1.0f : 1.0f;
        (*vector)[slot] += sign;
    }
    float norm = 0.0f;
    for (float value : *vector)
        norm += value * value;
    norm = qSqrt(norm);
    if (norm <= 0.0f) {
        if (error) *error = QStringLiteral("empty embedding");
        return false;
    }
    for (float &value : *vector)
        value /= norm;
    return true;
}
