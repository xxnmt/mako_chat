#ifndef QQREPLYPOLICY_H
#define QQREPLYPOLICY_H

#include <QString>
#include <QStringList>
#include <QList>

#include "qqmessageparser.h"

// QQ 回复策略（纯逻辑，便于单测）：决定一条入站消息是「丢弃 / 只记录 / 记录并回复」
struct QQReplyPolicy
{
    QString privateMode = QStringLiteral("master"); // none / master / allowlist / all
    QString groupMode = QStringLiteral("none");     // none / mention / all
    QList<qlonglong> privateAllow;
    QList<qlonglong> groupAllow;
    bool masterAnyGroup = false;
};

enum class QQReplyDecision {
    Drop,       // 不记录、不回复
    RecordOnly, // 只写 raw_events，不进大脑
    Reply       // 记录并回复
};

class QQReplyPolicyEngine
{
public:
    static QQReplyDecision decide(const QQInboundMessage &message,
                                  const QQReplyPolicy &policy,
                                  const QStringList &masterQqs);
};

#endif // QQREPLYPOLICY_H
