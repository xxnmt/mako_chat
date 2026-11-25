#include "actioncommitter.h"
#include "memorymanager.h"

#include <QDebug>

ActionCommitter::ActionCommitter(MemoryManager *memory)
    : m_memory(memory)
{
}

QString ActionCommitter::contactActionKey(qlonglong turn,int version)
{
    return QStringLiteral("reflection:%1:%2:contact").arg(turn).arg(version);
}

QString ActionCommitter::reminderActionKey(qlonglong reminderId)
{
    return QStringLiteral("reminder:%1:delivery").arg(reminderId);
}

QString ActionCommitter::shareActionKey(qlonglong activityId,int version)
{
    return QStringLiteral("thought:%1:%2:share").arg(activityId).arg(version);
}

QJsonObject ActionCommitter::commitReflectionContact(qlonglong turn,int version,
                                                     const QString &policyReason)
{
    if (!m_memory) return QJsonObject{{QStringLiteral("error"),QStringLiteral("no_store")}};
    const auto result=m_memory->commitReflectionContactAction(turn,version,policyReason);
    if (result.contains(QStringLiteral("error")))
        qInfo()<<"[ActionCommitter] contact rejected turn="<<turn<<"version="<<version
               <<"error="<<result.value(QStringLiteral("error")).toString()
               <<"detail="<<result.value(QStringLiteral("detail")).toString();
    return result;
}

QJsonObject ActionCommitter::commitReminderDelivery(qlonglong reminderId)
{
    if (!m_memory) return QJsonObject{{QStringLiteral("error"),QStringLiteral("no_store")}};
    const auto result=m_memory->commitReminderDeliveryAction(reminderId);
    if (result.contains(QStringLiteral("error")))
        qInfo()<<"[ActionCommitter] reminder rejected id="<<reminderId
               <<"error="<<result.value(QStringLiteral("error")).toString();
    return result;
}

QJsonObject ActionCommitter::commitActivityShare(qlonglong activityId,int version,
                                                 const QString &draft)
{
    if (!m_memory) return QJsonObject{{QStringLiteral("error"),QStringLiteral("no_store")}};
    const auto result=m_memory->commitActivityShareAction(activityId,version,draft);
    if (result.contains(QStringLiteral("error")))
        qInfo()<<"[ActionCommitter] share rejected activity="<<activityId<<"version="<<version
               <<"error="<<result.value(QStringLiteral("error")).toString();
    return result;
}
