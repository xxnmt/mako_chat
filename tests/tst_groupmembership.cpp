#include <QtTest>
#include <QTemporaryDir>

#include "groupmembershipservice.h"
#include "memorymanager.h"

class TestGroupMembership : public QObject
{
    Q_OBJECT

private slots:
    void pendingTimeoutMarksFailedAndKeepsPeriods()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager mm(dir.filePath("membership_timeout.db"));
        mm.seedIdentity({QStringLiteral("20001")});

        const QDateTime now = QDateTime::currentDateTime();
        QVERIFY(mm.replaceGroupMembershipSnapshot(
            QStringLiteral("40010"),
            {{QStringLiteral("20001"), QStringLiteral("群主")}},
            now));
        QVERIFY(mm.hasValidGroupMembershipSnapshot(QStringLiteral("40010"), 60, now));

        GroupMembershipService service(&mm, nullptr);
        service.handlePendingTimeout(QStringLiteral("test"),
                                     QStringLiteral("40010"));

        QVERIFY(!mm.hasValidGroupMembershipSnapshot(QStringLiteral("40010"), 60, now));
        QVERIFY(mm.getOpenGroupMemberIds(QStringLiteral("40010"))
                    .contains(QStringLiteral("20001")));
    }
};

QTEST_GUILESS_MAIN(TestGroupMembership)
#include "tst_groupmembership.moc"
