#include "sessionrouter.h"
#include "memorymanager.h"
#include "configmanager.h"
#include <QDebug>

SessionRouter::SessionRouter(MemoryManager *memoryManager, QObject *parent)
    : QObject(parent), m_memoryManager(memoryManager)
{
    // 启动时显式加载 identity.json：配置错误尽早暴露，而不是等第一条 QQ 消息
    ConfigManager::instance().loadIdentityConfig();
}

QString SessionRouter::resolveSubject(const QString &channel, const QString &senderId)
{
    // 桌宠 / 无稳定身份 → 主人（'user' 即主人，E2 决策）
    if (channel == Channel::kPet || senderId.isEmpty()) {
        return QStringLiteral("user");
    }

    ConfigManager &cfg = ConfigManager::instance();

    // 自己的账号：丢弃，避免自触发
    if (cfg.getSelfQQAccounts().contains(senderId)) {
        return QString();
    }
    // 主人账号（多账号都归一到 'user'）
    if (cfg.isMasterQQ(senderId)) {
        return QStringLiteral("user");
    }
    // 未知主体：QQ 渠道用 qq:<uin>，其他平台用 <channel>:<id>
    if (channel.startsWith(QStringLiteral("qq"))) {
        return QStringLiteral("qq:%1").arg(senderId);
    }
    return QStringLiteral("%1:%2").arg(channel, senderId);
}

bool SessionRouter::submitInput(const QString &channel, const QString &sessionId,
                                const QString &speaker, const QString &text,
                                const QString &senderId,
                                bool useShortContext,
                                bool longTermMemory,
                                bool sensitive)
{
    if (text.trimmed().isEmpty()) {
        qDebug() << "[SessionRouter]空输入，忽略";
        return false;
    }

    const QString subject = resolveSubject(channel, senderId);
    if (subject.isEmpty()) {
        qDebug() << "[SessionRouter]自身账号消息，丢弃:" << channel << senderId;
        return false;
    }

    RequestMeta meta;
    meta.channel = channel;
    meta.sessionId = sessionId;
    meta.speaker = speaker;
    meta.subject = subject;
    meta.actorId = senderId;
    meta.sensitive = sensitive;
    meta.persistTurn = useShortContext;
    meta.longTermMemoryEnabled = longTermMemory;
    // 盲区/脱敏标记由渠道侧传入；MemoryManager 保持中立

    // 组装短期上下文（channel+session_id 双键隔离，B1 决策）
    QList<HistoryTurn> history;
    if (useShortContext) {
        const int memoryLength = ConfigManager::instance().getShortMemoryLength();
        history = m_memoryManager->getHistoryTurn(memoryLength, channel, sessionId);
    }

    qDebug() << "[SessionRouter]路由输入:" << channel << "/" << sessionId
             << "speaker=" << speaker << "subject=" << subject
             << "历史轮数:" << history.size();
    emit requestReply(text, history, meta);
    return true;
}
