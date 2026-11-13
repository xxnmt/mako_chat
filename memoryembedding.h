#ifndef MEMORYEMBEDDING_H
#define MEMORYEMBEDDING_H

#include <QString>
#include <QVector>

// 向量 Provider 只负责“文本 -> 向量”，不参与权限或证据裁剪。
class MemoryEmbeddingProvider
{
public:
    virtual ~MemoryEmbeddingProvider() = default;
    virtual QString modelId() const = 0;
    virtual int dimension() const = 0;
    virtual bool embed(const QString &text, QVector<float> *vector,
                       QString *error = nullptr) const = 0;
};

// 确定性的本地测试/降级 Provider：只做字符 bigram 哈希，不是语义模型。
// 它用于在没有真实 Embedding 服务时验证索引、RRF、降级路径；
// 文档和状态必须把它标成 local_hash_test，不能冒充语义召回。
class LocalHashEmbeddingProvider : public MemoryEmbeddingProvider
{
public:
    explicit LocalHashEmbeddingProvider(int dimension = 64);
    QString modelId() const override;
    int dimension() const override;
    bool embed(const QString &text, QVector<float> *vector,
               QString *error = nullptr) const override;

private:
    int m_dimension = 64;
};

#endif // MEMORYEMBEDDING_H
