#ifndef LLMSERVICE_H
#define LLMSERVICE_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QScopedPointer>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>

#include "sentencedata.h"
#include "historyturn.h"
#include "utterance.h"
#include "memoryagenttypes.h"
#include "memoryrecalltool.h"
#include "memoryretrieval.h"
#include "imageperception.h"
#include "memoryretrievalworker.h"

class MemoryManager;
class MemoryAgent;

class LLMService : public QObject
{
    Q_OBJECT
public:
    explicit LLMService(const QString& apiKey,QObject *parent = nullptr,
                        QUrl endpoint = QUrl(QStringLiteral("https://api.deepseek.com/chat/completions")));
    ~LLMService() override;
    //config
    void setApiKey(const QString &apiKey);
    //chat
    void askDeepSeek(const QString& userInput,const QList<HistoryTurn> &historyQA=QList<HistoryTurn>(),
                     const RequestMeta &meta = RequestMeta());
    void registerStateProvider(std::function<QString()> provider);
    void setMemoryManager(MemoryManager *manager);
    QString retrievalDatabasePath() const;
    void setBenchmarkChatOnly(bool enabled) { m_benchmarkChatOnly = enabled; }
    QJsonArray lastRequestMessages() const { return m_lastRequestMessages; }
    RetrievalResult lastRetrievalResult() const { return m_lastRetrievalResult; }
    QJsonObject lastResponseUsage() const { return m_lastResponseUsage; }
    // 当前被授予的能力清单：由 ConfigManager 配置与服务端状态生成（只读）。
    // 前台提示词与私下回顾共用同一份数据源，避免两处各说一套。
    QJsonObject capabilityManifest(const RequestMeta &meta) const;
    void requestPrivateReview(qlonglong id, int version, const QJsonObject &thread);
    // R12-2b：内部活动分享等通用动作提案的统一语义核验入口。request 由服务端组装
    // （origin/target/payload/claims）；R12-2c-1 起每次核验由服务端生成唯一 request_id，
    // 核验链：确定性 validateActionProposal →（一次）coverage/entailment verifier。
    // repair 默认关闭（fail closed）。终态通过 actionProposalVerificationFinished 发出；
    // 不写库、不发送。
    void requestActionProposalVerification(const QJsonObject &request);
    void cancelPrivateReview();
    bool hasWritingProvider() const { return !m_apiKey.isEmpty(); }
    void processObservation();
    // ===== R12-2c-1：语义核验输入预算（按完整证据项分配，绝不截断序列化 JSON）=====
    // 预算单位=输入 JSON 的紧凑序列化字符数（近似 token 预算）。
    // identity/core 为必要核心：identity（request_id/action_key/来源身份/payload_digest 等）
    // 与 core（message/reason/draft + rules）必须完整容纳。requiredClaims/requiredSources
    // 是"核验成立所必需"的完整证据项，必须全部容纳，否则返回 evidence_budget_exceeded；
    // 当前生产路径把全部 declared claims 与 sources 都作为必选项（丢弃任一 claim 会让
    // entailed 无法覆盖它，丢弃 source 会让 entailed 失去依据）。
    // optionalClaims/optionalSources 是预算助手的整项取舍能力（先 claim 后 source），
    // 供以后能证明"非必需的低优先证据"时使用；绝不做字符串截断。
    // 预算诊断（selected/omitted/bytes）写进 input["budget"]，同时回传结构体供日志/测试。
    struct VerifierInputBudget
    {
        bool ok = false;
        QString error;              // evidence_budget_exceeded / invalid_evidence
        QJsonObject input;          // 完整 verifier 输入（每项都是完整对象）
        int bytes = 0;              // 最终 input 紧凑序列化字符数
        int selectedClaimCount = 0;
        int omittedClaimCount = 0;
        int selectedSourceCount = 0;
        int omittedSourceCount = 0;
    };
    static constexpr int kVerifierInputBudget = 3000;
    static VerifierInputBudget allocateVerifierInput(const QJsonObject &identity,
                                                     const QJsonObject &core,
                                                     const QJsonArray &requiredClaims,
                                                     const QJsonArray &requiredSources,
                                                     const QJsonArray &optionalClaims,
                                                     const QJsonArray &optionalSources,
                                                     int budget = kVerifierInputBudget);
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
    // 模型决定 close/defer 且不出声时，关注写回仍然要落地（否则"这轮就算聊完了"会丢掉答案）。
    void concernUpdatesReady(const QString &runId, const QJsonArray &updates, const RequestMeta &meta);
    //chat
    void sentencesReady(const QList<SentenceText> &sentences, const QString &rawReply,
                        const QString &userInput, const RequestMeta &meta);
    void privateReviewFinished(qlonglong id, int version, const QJsonObject &proposal, const QString &error);
    // R12-2b：通用 action proposal 核验终态（state=verified|withheld + semantic_status/
    // withhold_reason）。订阅方（PrivateReview）负责 persist/state CAS/commit。
    void actionProposalVerificationFinished(const QJsonObject &result);
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
                      const RequestMeta &meta, const QJsonArray &privateEvidence);
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
    quint64 m_reviewGeneration = 0;
    void generatePrivateReview(qlonglong id, int version, const QJsonObject &thread);
    // 切片 C：私人回顾输出 contact.motivation/claims 后的验证链。
    // 先做确定性策略复核（ActionPolicy），再对 timing=="now" 的新契约联系做一次语义核验；
    // 该次核验同时完成 claim coverage（message/reason 中的外部事实是否都被 declared_claims
    // 覆盖）与 entailment（已声明事实是否被 sources 支持）。coverage 不完整直接 withhold
    // （claim_coverage_incomplete）；complete+entailed 在无已声明事实 claim 时记
    // entailed_coverage（有事实 claim 时沿用记忆层门禁接受的 entailed）；complete+
    // insufficient/contradicted 一律 fail closed（R12-2c-1：不再发送自由重写 repair）。
    // 全部在异步回调里完成，最后才 emit privateReviewFinished。
    void finalizePrivateReview(qlonglong id, int version, const QJsonObject &thread,
                               const QJsonObject &proposal, const QString &error);
    void verifyPrivateReviewContact(qlonglong id, int version, const QJsonObject &thread,
                                    const QJsonObject &proposal);
    void emitPrivateReviewResult(qlonglong id, int version, const QJsonObject &proposal,
                                 const QString &error);
    static QJsonObject withholdContactProposal(QJsonObject proposal, QJsonObject contact,
                                               const QString &reason);
    // ===== R12-2b/R12-2c-1：通用行动提案核验链（OpenWake contact / internal activity share）=====
    using ActionVerificationCallback = std::function<void(const QJsonObject &result)>;
    // OpenWake contact：构建服务端 ActionOrigin/计划，走统一核验链后把 _action 附到 proposal。
    void verifyOpenWakeContactProposal(qlonglong id, int version, const QJsonObject &thread,
                                       const QJsonObject &proposal);
    void verifyActionProposal(const QJsonObject &request, ActionVerificationCallback done);
    void verifyActionProposalSemantics(const QJsonObject &request,
                                       const QJsonObject &deterministic,
                                       ActionVerificationCallback done);
    // R12-2c-1：在途 verifier 请求按 request_id/action_key 管理，迟到/旧请求直接丢弃。
    // verifier 与 repair 共用单指针会在并发 contact/share 核验时互相覆盖，这里彻底去掉。
    struct InFlightVerifier
    {
        QPointer<QNetworkReply> reply;
        QString requestId;
        QString actionKey;
        quint64 generation = 0;     // private-review 链的 generation fence；action 链为 0
        bool privateReview = false; // true=legacy contact 核验（受 m_reviewGeneration 约束）
    };
    void registerVerifierRequest(const InFlightVerifier &info);
    // 取出并移除在途请求；返回 true 仅当它仍是该 action_key 的最新请求。
    // 旧/迟到/被取代的请求返回 false，调用方不得再走任何终态写回。
    bool takeInFlightVerifier(const QString &requestId, QNetworkReply *reply,
                              InFlightVerifier *info);
    void discardPrivateReviewVerifiers();
    QHash<QString, InFlightVerifier> m_verifierRequests;     // request_id -> 在途请求
    QHash<QString, QString> m_latestVerifierRequestByAction;  // action_key -> 最新 request_id
    void retrieveMemory(const RetrievalRequest &request, std::function<void(const RetrievalResult &)> done);
    ImagePerception m_imagePerception;
    void finishRetrieval(const RetrievalResult &result);

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
    QPointer<QNetworkReply> m_privateReviewReply;
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
    std::unique_ptr<MemoryRetrievalService> m_retrievalService;
    struct ForegroundRun {
        RequestMeta meta;
        QString input;
        MemorySituation situation;
        QString retrievalStatus;
        QString retrievalBackend;
        QString retrievalDegradedReason;
        int retrievalCandidates = 0;
        int retrievalAuxiliary = 0;
        qint64 retrievalLatencyMs = 0;
        bool retrievalTruncated = false;
        QStringList retrievalTerms;
        QJsonObject evidence;
        QJsonObject beforeConsultation;
        QStringList catalogKeys;
        QStringList consultedKeys;
        QJsonObject consultedRecords;
        QJsonArray consultationMessages;
        bool refreshed = false;
        QList<HistoryTurn> history;
        QJsonObject expressionBases;
        QSet<QByteArray> lookups;
        QSet<QByteArray> retrievedItems;
        int lookupsWithoutNewItems = 0;
        QSet<QString> expandedMatters;
        bool lookupFinalRequested = false;
        bool expressionRefreshed = false;
    } m_run;
    QJsonArray m_lastRequestMessages;
    RetrievalResult m_lastRetrievalResult;
    QJsonObject m_lastResponseUsage;
    bool m_benchmarkChatOnly = false;
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
