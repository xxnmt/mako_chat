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
    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout,
            this, &GroupMembershipService::refreshAllowedGroups);
    if (m_qqClient) {
        connect(m_qqClient, &QQProtocolClient::connected,
                this, &GroupMembershipService::onConnected);
        connect(m_qqClient, &QQProtocolClient::actionResponse,
                this, &GroupMembershipService::onActionResponse);
        connect(m_qqClient, &QQProtocolClient::noticeEvent,
                this, &GroupMembershipService::onNoticeEvent);
    }
}

void GroupMembershipService::start()
{
    if (m_qqClient && m_qqClient->isConnected()) {
        onConnected();
    }
    const QQGroupMemoryConfig groups =
        ConfigManager::instance().getQQGroupMemoryConfig();
    if (groups.membershipRefreshMinutes > 0) {
        m_refreshTimer->start(groups.membershipRefreshMinutes * 60 * 1000);
    }
}

void GroupMembershipService::onConnected()
{
    const QQGroupMemoryConfig groups =
        ConfigManager::instance().getQQGroupMemoryConfig();
    if (groups.membershipRefreshMinutes > 0) {
        m_refreshTimer->start(groups.membershipRefreshMinutes * 60 * 1000);
    } else {
        m_refreshTimer->stop();
    }
    refreshAllowedGroups();
}

bool GroupMembershipService::isAllowedGroup(const QString &groupId) const
{
    const QQGroupMemoryConfig groups =
        ConfigManager::instance().getQQGroupMemoryConfig();
    return groups.enabled && groups.groupAllow.contains(groupId.toLongLong());
}

void GroupMembershipService::refreshAllowedGroups()
{
    if (!m_memoryManager || !m_qqClient || !m_qqClient->isConnected()) {
        return;
    }
    const QQGroupMemoryConfig groups =
        ConfigManager::instance().getQQGroupMemoryConfig();
    for (qlonglong groupId : groups.groupAllow) {
        const QString id = QString::number(groupId);
        const QString echo = QStringLiteral("membership:") + id;
        if (m_pendingGroups.contains(echo)) {
            continue;
        }
        requestGroup(id);
    }
}

void GroupMembershipService::requestGroup(const QString &groupId)
{
    if (!m_qqClient || groupId.isEmpty() || !isAllowedGroup(groupId)) {
        return;
    }
    const QString echo = QStringLiteral("membership:") + groupId;
    if (m_pendingGroups.contains(echo)) {
        return;
    }
    m_pendingGroups.insert(echo, groupId);
    m_qqClient->sendAction(QStringLiteral("get_group_member_list"),
                           QJsonObject{{QStringLiteral("group_id"),
                                        groupId}}, echo);
    QPointer<GroupMembershipService> self(this);
    QTimer::singleShot(10000, this, [this, self, echo, groupId]() {
        if (self.isNull() || !m_pendingGroups.contains(echo)) {
            return;
        }
        handlePendingTimeout(echo, groupId);
    });
}

void GroupMembershipService::handlePendingTimeout(const QString &echo,
                                                  const QString &groupId)
{
    if (!m_pendingGroups.contains(echo) && echo != QStringLiteral("test")) {
        return;
    }
    failPending(echo, groupId, QStringLiteral("timeout"));
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

void GroupMembershipService::onNoticeEvent(const QJsonObject &event)
{
    const QString noticeType = event.value(QStringLiteral("notice_type")).toString();
    if (noticeType != QStringLiteral("group_increase")
        && noticeType != QStringLiteral("group_decrease")) {
        return;
    }
    const QString groupId = event.value(QStringLiteral("group_id"))
                                .toVariant().toString();
    const QString userId = event.value(QStringLiteral("user_id"))
                               .toVariant().toString();
    if (groupId.isEmpty() || userId.isEmpty() || !isAllowedGroup(groupId)) {
        return;
    }
    if (!m_memoryManager) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    if (noticeType == QStringLiteral("group_increase")) {
        m_memoryManager->openGroupMembershipPeriod(
            groupId, userId, userId, QStringLiteral("group_increase"), now);
    } else {
        m_memoryManager->closeGroupMembershipPeriod(groupId, userId, now);
    }
    // 增减事件可能没有群名片，立即刷新一次补齐显示名。
    const QString echo = QStringLiteral("membership:") + groupId;
    if (!m_pendingGroups.contains(echo)) {
        requestGroup(groupId);
    }
    qDebug() << "[Membership] 群成员事件:" << noticeType
             << "group=" << groupId << "user=" << userId;
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
