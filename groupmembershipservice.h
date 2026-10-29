#ifndef GROUPMEMBERSHIPSERVICE_H
#define GROUPMEMBERSHIPSERVICE_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>

class QTimer;

class MemoryManager;
class QQProtocolClient;

// PR-3.2a：拉取允许群的成员列表并维护 group_membership_periods。
class GroupMembershipService : public QObject
{
    Q_OBJECT
public:
    GroupMembershipService(MemoryManager *memoryManager,
                           QQProtocolClient *qqClient,
                           QObject *parent = nullptr);

    void start();
    void refreshAllowedGroups();

public slots:
    // 由 10 秒 pending 定时器调用；独立出来便于确定性测试。
    void handlePendingTimeout(const QString &echo, const QString &groupId);

private slots:
    void onConnected();
    void onActionResponse(const QString &echo,
                          const QJsonValue &data,
                          int retcode);
    void onNoticeEvent(const QJsonObject &event);

private:
    bool isAllowedGroup(const QString &groupId) const;
    void requestGroup(const QString &groupId);
    void failPending(const QString &echo, const QString &group,
                     const QString &reason);

    MemoryManager *m_memoryManager = nullptr;
    QQProtocolClient *m_qqClient = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QHash<QString, QString> m_pendingGroups; // echo -> group_id
};

#endif // GROUPMEMBERSHIPSERVICE_H
