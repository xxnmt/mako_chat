#ifndef MEMORYAGENT_H
#define MEMORYAGENT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QScopedPointer>
#include <functional>

#include "memoryagenttypes.h"
#include "memorypolicy.h"
#include "utterance.h"

class MemoryManager;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// PR-4：只读记忆检索工具。所有 scope 由服务端注入；参数中不存在
// viewer/owner/location/allowed_groups 等权限字段。
class MemoryAgentTools
{
public:
    MemoryAgentTools(MemoryManager *memoryManager, const MemoryAgentScope &scope);

    static QJsonArray toolDefinitions();

    // 返回 {"items":[...],"truncated":bool} 或 {"error":"...","message":"..."}。
    QJsonObject execute(const QString &name,
                        const QJsonObject &args,
                        QString *error);

    const QList<MemoryAgentItem> &items() const { return m_ordered; }
    bool truncated() const { return m_truncated; }

private:
    QJsonObject search(const QJsonObject &args, QString *error);
    QJsonObject read(const QJsonObject &args, QString *error);
    QJsonObject related(const QJsonObject &args, QString *error);

    void remember(const MemoryAgentItem &item);
    QJsonObject itemToJson(const MemoryAgentItem &item) const;
    QJsonObject errorObject(const QString &code, const QString &message) const;

    MemoryManager *m_memoryManager = nullptr;
    MemoryAgentScope m_scope;
    QHash<QString, MemoryAgentItem> m_cache;
    QList<MemoryAgentItem> m_ordered;
    bool m_truncated = false;
};

// PR-4：只读检索规划器。先由 planner 决定调用哪些工具，工具在本地执行，
// 结果再回填给 planner；最终结果交给主回复模型，不直接生成用户回复。
class MemoryAgent : public QObject
{
    Q_OBJECT
public:
    using PlannerCallback = std::function<void(bool ok,
                                              const QJsonObject &content,
                                              const QString &error)>;
    using PlannerTransport = std::function<void(const QJsonArray &messages,
                                                PlannerCallback callback)>;

    explicit MemoryAgent(QObject *parent = nullptr);
    ~MemoryAgent() override;

    void setMemoryManager(MemoryManager *manager);
    void setApiKey(const QString &apiKey);
    void setPlannerTransport(PlannerTransport transport); // 仅测试注入
    void setMetricsEnabled(bool enabled) { m_metricsEnabled = enabled; }

    void start(const MemoryAgentScope &scope, const QString &query);
    void cancel();

    MemoryAgentMetrics metrics() const { return m_metrics; }
    const QVector<MemoryAgentAudit> &auditTrail() const { return m_audit; }

signals:
    void finished(const MemoryAgentResult &result);
    void failed(const QString &reason);
    void auditRecorded(const MemoryAgentAudit &audit);

private:
    void sendPlanner(const QJsonArray &messages);
    void handlePlannerReply(bool ok, const QJsonObject &content,
                            const QString &error);
    void executeToolCalls(const QJsonArray &toolCalls);
    void finishWithStatus(const QString &status);
    void failRun(const QString &reason, bool timedOut = false);
    void appendAudit(const QString &tool, const QJsonObject &args,
                     int resultCount, int latencyMs, const QString &status);
    QJsonArray initialPlannerMessages(const QString &query) const;
    QString plannerSystemPrompt() const;

    MemoryManager *m_memoryManager = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_deadlineTimer = nullptr;
    PlannerTransport m_transport;

    MemoryAgentScope m_scope;
    QString m_query;
    QString m_apiKey;
    QScopedPointer<MemoryAgentTools> m_tools;
    QJsonArray m_messages;
    QJsonObject m_lastContent;
    int m_round = 0;
    int m_parseRetries = 0;
    bool m_timedOut = false;
    bool m_finished = false;
    bool m_metricsEnabled = true;
    QElapsedTimer m_runTimer;
    MemoryAgentMetrics m_metrics;
    QVector<MemoryAgentAudit> m_audit;
};

class MemoryAgentPlanner
{
public:
    static bool shouldRun(const RequestMeta &meta,
                          const QString &input,
                          const MemoryAgentConfig &config);
    static bool isMemoryQuestion(const QString &input);
};

#endif // MEMORYAGENT_H
