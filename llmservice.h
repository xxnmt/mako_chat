#ifndef LLMSERVICE_H
#define LLMSERVICE_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopedPointer>
#include <QPointer>
#include <QSet>

#include "sentencedata.h"
#include "historyturn.h"
#include "utterance.h"
#include "memoryagenttypes.h"
#include "memoryrecalltool.h"

class MemoryManager;
class MemoryAgent;

class LLMService : public QObject
{
    Q_OBJECT
public:
    explicit LLMService(const QString& apiKey,QObject *parent = nullptr,
                        QUrl endpoint = QUrl(QStringLiteral("https://api.deepseek.com/chat/completions")));
    //config
    void setApiKey(const QString &apiKey);
    //chat
    void askDeepSeek(const QString& userInput,const QList<HistoryTurn> &historyQA=QList<HistoryTurn>(),
                     const RequestMeta &meta = RequestMeta());
    void registerStateProvider(std::function<QString()> provider);
    void setMemoryManager(MemoryManager *manager);
    void reviewDueThought();
    void processObservation();
    //Extract
    void extractMemoryAsync(const QList<HistoryTurn> &turns, qlonglong lastEndId, const QList<qlonglong> &sourceIds,
                            const QList<CharacterProfile> &existingProfiles = {},
                            const QList<EpisodicMemory> &existingMemories = {},
                            const RequestMeta &meta = RequestMeta());

    // PR-2: 为一条 interaction 生成同群 safe summary
    void summarizeInteractionAsync(qlonglong interactionId,
                                   const QList<RawEvent> &events);

signals:
    void replyQuiet(const QString &action, const QString &userInput, const RequestMeta &meta);
    //chat
    void sentencesReady(const QList<SentenceText> &sentences, const QString &rawReply,
                        const QString &userInput, const RequestMeta &meta);
    void draftReadyToShare(const QString &activityRef, int version, const QString &draft);
    void internetErrorSignal(const QString &errorMessage);
    //chat 回复内容异常（空/乱码/缺中文/缺日文/缺标签），不入库不走TTS
    void replyAbnormal(const QString &reason);
    //Extract
    void memoryExtractionReady(const QJsonObject &extractionResult, qlonglong lastEndId,
                               const QString &sourceIdsJson, const RequestMeta &meta);
    void interactionSummaryReady(qlonglong interactionId, const QString &summary,
                                 const QString &summaryFormat,
                                 const QString &segmentsJson,
                                 const QJsonArray &claims);
    void interactionSummaryFailed(qlonglong interactionId, const QString &reason);
private slots:
    //finish AI 回复
    void onReplyFinished(QNetworkReply* reply);
    //extract
    // void onExtractReplyFinished(QNetworkReply* reply);
    // PR-4: MemoryAgent 完成/失败后继续主回复或回退 top-k
    void onMemoryAgentFinished(const MemoryAgentResult &result);
    void onMemoryAgentFailed(const QString &reason, const QString &requestId);

private:
    QJsonArray observationHistory(const QString &subject, const QString &channel, const QString &session,
                                  const QString &before, const QStringList &terms, bool includeLocal) const;
    void proposeObservation(const QJsonObject &job, const QJsonArray &history = {},
                            const QStringList &searchTerms = {});
    //处理提示词的两个函数
    void prepareReply(const QString &userInput, const QList<HistoryTurn> &historyQA,
                      const RequestMeta &meta, const QJsonArray &privateEvidence,
                      const QString &socialGuidance);
    void considerEvidence(const QJsonArray &background, const QJsonArray &personal,
                          const QJsonArray &matters, const QJsonArray &chain = {});
    bool takeLookup(const QString &purpose, const QJsonObject &arguments);
    bool expressionBasisCurrent() const;
    void initializePromptFile();
    QString loadSystemPrompt();
    void sendChatRequest(const QJsonArray &messages,
                         bool withMemoryTool = false,
                         bool jsonResponse = true);
    bool refreshConsultation();
    QPointer<QNetworkReply> m_consultationReply;
    void registerToolEvidence(const QJsonObject &payload, const QJsonObject &arguments);
    bool consultMatters(const QJsonObject &decision);
    QJsonObject readConsultedMatters() const;
    void generateWithMatters(const QJsonObject &records);
    void sendSafeRetry();
    void sendEmptyReplyRetry();
    void sendPendingReply(const QJsonObject &agentMessage = QJsonObject(),
                          bool partial = false);

    // 主 Conversation Agent 的记忆工具路径。
    void startMemoryToolPath(const QJsonArray &messages,
                             const MemoryAgentScope &scope,
                             const QString &protocol);
    bool handleMemoryToolReply(const QJsonObject &message);
    void handleNativeToolCalls(const QJsonObject &message,
                               const QJsonArray &toolCalls);
    void handleEnvelopeRecall(const QJsonObject &content);
    void sendMemoryToolFinal(bool withTools);
    void switchMemoryToolToEnvelope();
    QString envelopeToolInstruction() const;

    QPair<QList<SentenceText>, QString> parseJsonReply(const QString &replyText, QJsonObject &decision);

    //回复内容合法性校验（异常回复整轮抛弃）
    static bool containsHan(const QString &text);      //是否含中文（CJK汉字）
    static bool containsKana(const QString &text);     //是否含日文（假名）
    static bool isSentenceValid(const QJsonObject &sentenceObj, QString &reason);


    bool m_observationActive = false;
    bool m_reviewActive = false;
    QNetworkAccessManager* m_networkManager;
    QNetworkAccessManager* m_extractManager;
    QNetworkAccessManager* m_interactionManager;
    QString m_apiKey;
    QUrl m_endpoint;

    QString m_localPromptPath;
    QString m_systemPromptCache;
    std::function<QString()> m_stateProvider;

    MemoryManager *m_memoryManager = nullptr;
    MemoryAgent *m_memoryAgent = nullptr;
    struct ForegroundRun {
        RequestMeta meta;
        QString input;
        QJsonObject evidence;
        QJsonObject beforeConsultation;
        QStringList catalogKeys;
        QStringList consultedKeys;
        QJsonObject consultedRecords;
        QJsonArray consultationMessages;
        bool refreshed = false;
        QList<HistoryTurn> history;
        QJsonArray expression;
        QJsonObject expressionBases;
        QSet<QByteArray> lookups;
        QSet<QByteArray> retrievedItems;
        int lookupsWithoutNewItems = 0;
        QSet<QString> expandedMatters;
        bool lookupFinalRequested = false;
        bool expressionRefreshed = false;
        QString projectionError;
    } m_run;
    QJsonArray m_lastRequestMessages;
    QJsonArray m_pendingMessages; // legacy：等待 MemoryAgent 的主请求
    quint64 m_agentGeneration = 0;
    bool m_agentPending = false;

    // tool 模式：同一个主模型、同一份消息历史，工具结果只追加到会话。
    QJsonArray m_toolMessages;
    MemoryAgentScope m_toolScope;
    QScopedPointer<MemoryRecallTool> m_recallTool;
    QString m_toolProtocol;       // native / json_envelope
    int m_toolRound = 0;
    bool m_memoryToolActive = false;
    bool m_toolFallbackUsed = false;

    int m_outputRetryCount = 0;
    int m_emptyRetryCount = 0;
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
