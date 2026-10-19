#include "sessionrouter.h"
#include "memorymanager.h"
#include "configmanager.h"
#include <QDebug>

SessionRouter::SessionRouter(MemoryManager *memoryManager, QObject *parent)
    : QObject(parent), m_memoryManager(memoryManager)
{
}

QString SessionRouter::resolveSubject(const QString &channel, const QString &senderId)
{
    // Phase 0：桌宠对话即主人（'user' 即主人，E2 决策）
    // Phase 1：QQ 渠道接入后改为查 ConfigManager 的 identity 配置表
    Q_UNUSED(channel);
    Q_UNUSED(senderId);
    return QStringLiteral("user");
}

void SessionRouter::submitInput(const QString &channel, const QString &sessionId,
                                const QString &speaker, const QString &text)
{
    if (text.trimmed().isEmpty()) {
        qDebug() << "[SessionRouter]空输入，忽略";
        return;
    }

    RequestMeta meta;
    meta.channel = channel;
    meta.sessionId = sessionId;
    meta.speaker = speaker;
    meta.subject = resolveSubject(channel, speaker);
    // meta.sensitive 由渠道侧查盲区配置后设置（桌宠路径默认 false）

    // 组装短期上下文（channel+session_id 双键隔离，B1 决策）
    int memoryLength = ConfigManager::instance().getShortMemoryLength();
    QList<HistoryTurn> history = m_memoryManager->getHistoryTurn(memoryLength, channel, sessionId);

    qDebug() << "[SessionRouter]路由输入:" << channel << "/" << sessionId
             << "speaker=" << speaker << "历史轮数:" << history.size();
    emit requestReply(text, history, meta);
}
