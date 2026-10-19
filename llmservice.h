#ifndef LLMSERVICE_H
#define LLMSERVICE_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include "sentencedata.h"
#include "historyturn.h"
#include "utterance.h"

class MemoryManager;

class LLMService : public QObject
{
    Q_OBJECT
public:
    explicit LLMService(const QString& apiKey,QObject *parent = nullptr);
    //config
    void setApiKey(const QString &apiKey);
    //chat
    void askDeepSeek(const QString& userInput,const QList<HistoryTurn> &historyQA=QList<HistoryTurn>(),
                     const RequestMeta &meta = RequestMeta());
    void registerStateProvider(std::function<QString()> provider);
    void setMemoryManager(MemoryManager *manager);
    //Extract
    void extractMemoryAsync(const QList<HistoryTurn> &turns, qlonglong lastEndId, const QList<qlonglong> &sourceIds,
                            const QList<CharacterProfile> &existingProfiles = {},
                            const QList<EpisodicMemory> &existingMemories = {});

signals:
    //chat
    void sentencesReady(const QList<SentenceText> &sentences,const QString &rawReply);
    void internetErrorSignal(const QString &errorMessage);
    //chat 回复内容异常（空/乱码/缺中文/缺日文/缺标签），不入库不走TTS
    void replyAbnormal(const QString &reason);
    //Extract
    void memoryExtractionReady(const QJsonObject &extractionResult, qlonglong lastEndId, const QString &sourceIdsJson);
private slots:
    //finish AI 回复
    void onReplyFinished(QNetworkReply* reply);
    //extract
    // void onExtractReplyFinished(QNetworkReply* reply);

private:
    //处理提示词的两个函数
    void initializePromptFile();
    QString loadSystemPrompt();

    QPair<QList<SentenceText>, QString> parseJsonReply(const QString &replyText);

    //回复内容合法性校验（异常回复整轮抛弃）
    static bool containsHan(const QString &text);      //是否含中文（CJK汉字）
    static bool containsKana(const QString &text);     //是否含日文（假名）
    static bool isSentenceValid(const QJsonObject &sentenceObj, QString &reason);


    QNetworkAccessManager* m_networkManager;
    QNetworkAccessManager* m_extractManager;
    QString m_apiKey;

    QString m_localPromptPath;
    QString m_systemPromptCache;
    std::function<QString()> m_stateProvider;

    MemoryManager *m_memoryManager = nullptr;
    RequestMeta m_currentMeta;   // 当前轮次元数据（askDeepSeek 设置，prompt 组装与记忆注入使用）
};

class TagValidator{
public:
    TagValidator(const TagValidator &) = default;
    TagValidator(TagValidator &&) = default;
    TagValidator &operator=(const TagValidator &) = default;
    TagValidator &operator=(TagValidator &&) = default;
    static int levenshteinDistance(const QString &s1, const QString &s2);
    static QString validateTag(const QString &tagName, const QString &rawValue,
                               const QStringList &validValues,
                               const QString &contextText = "",
                               const QString &prevValue = "");

    static const QStringList VALID_EMOTIONS;
    static const QStringList VALID_BLUSH;
    static const QStringList VALID_DISTANCE;
    static const QStringList VALID_CLOTHING;
    static int getEditDistanceThreshold(const QString &value);


};

#endif // LLMSERVICE_H
