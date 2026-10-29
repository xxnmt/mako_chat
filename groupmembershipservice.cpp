#include "groupmembershipservice.h"

#include "configmanager.h"
#include "memorymanager.h"
#include "memorypolicy.h"
#include "qqprotocolclient.h"

#include <QDateTime>
#include <QDebug>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

GroupMembershipService::GroupMembershipService(MemoryManager *memoryManager,
                                               QQProtocolClient *qqClient,
                                               QObject *parent)
    : QObject(parent),
      m_memoryManager(memoryManager),
      m_qqClient(qqClient)
{
    if (m_qqClient) {
        connect(m_qqClient, &QQProtocolClient::connected,
                this, &GroupMembershipService::onConnected);
        connect(m_qqClient, &QQProtocolClient::actionResponse,
                this, &GroupMembershipService::onActionResponse);
    }
}

void GroupMembershipService::start()
{
    if (m_qqClient && m_qqClient->isConnected()) {
        onConnected();
    }
}

void GroupMembershipService::onConnected()
{
    refreshAllowedGroups();
}

void GroupMembershipService::refreshAllowedGroups()
{
    if (!m_memoryManager || !m_qqClient || !m_qqClient->isConnected()) {
        return;
    }
    const QQGroupMemoryConfig groups =
        ConfigManager::instance().getQQGroupMemoryConfig();
    for (qlonglong groupId : groups.groupAllow) {
        requestGroup(QString::number(groupId));
    }
}

void GroupMembershipService::requestGroup(const QString &groupId)
{
    if (!m_qqClient || groupId.isEmpty()) {
        return;
    }
    const QString echo = QStringLiteral("membership:") + groupId;
    m_pendingGroups.insert(echo, groupId);
    m_qqClient->sendAction(QStringLiteral("get_group_member_list"),
                           QJsonObject{{QStringLiteral("group_id"),
                                        groupId}}, echo);
    QPointer<GroupMembershipService> self(this);
    QTimer::singleShot(10000, this, [this, self, echo, groupId]() {
        if (self.isNull() || !m_pendingGroups.contains(echo)) {
            return;
        }
        failPending(echo, groupId, QStringLiteral("timeout"));
    });
}

void GroupMembershipService::onActionResponse(const QString &echo,
                                              const QJsonValue &data,
                                              int retcode)
{
    if (!m_pendingGroups.contains(echo)) {
        return;
    }
    const QString groupId = m_pendingGroups.take(echo);
    if (retcode != 0) {
        failPending(echo, groupId,
                    QStringLiteral("retcode=%1").arg(retcode));
        return;
    }

    const QJsonArray members = data.toArray();
    if (members.isEmpty()) {
        failPending(echo, groupId, QStringLiteral("empty_member_list"));
        return;
    }

    QList<QPair<QString, QString>> parsed;
    for (const QJsonValue &value : members) {
        const QJsonObject obj = value.toObject();
        const QString userId = obj.value(QStringLiteral("user_id"))
                                   .toVariant().toString();
        if (userId.isEmpty()) {
            continue;
        }
        QString display = obj.value(QStringLiteral("card")).toString();
        if (display.isEmpty()) {
            display = obj.value(QStringLiteral("nickname")).toString();
        }
        if (display.isEmpty()) {
            display = userId;
        }
        parsed.append(qMakePair(userId, display));
    }
    if (parsed.isEmpty()) {
        failPending(echo, groupId, QStringLiteral("empty_parsed_members"));
        return;
    }

    if (!m_memoryManager->replaceGroupMembershipSnapshot(
            groupId, parsed, QDateTime::currentDateTime())) {
        failPending(echo, groupId, QStringLiteral("db_write_failed"));
        return;
    }
    qDebug() << "[Membership] 群成员快照成功:" << groupId
             << "members=" << parsed.size();
}

void GroupMembershipService::failPending(const QString &echo,
                                         const QString &group,
                                         const QString &reason)
{
    m_pendingGroups.remove(echo);
    if (m_memoryManager) {
        m_memoryManager->markGroupMembershipSnapshotFailed(
            group, reason, QDateTime::currentDateTime());
    }
    qDebug() << "[Membership] 群成员快照失败:" << group << reason;
}
