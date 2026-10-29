#include <QtTest>

#include "memoryaccesspolicy.h"

class TestMemoryAccessPolicy : public QObject
{
    Q_OBJECT

private:
    static MemoryClaim claim(const QString &asserted,
                             const QString &about,
                             const QString &owner,
                             const QString &groupId = QStringLiteral("30001"))
    {
        MemoryClaim c;
        c.assertedByPersonId = asserted;
        c.aboutPersonId = about;
        c.privacyOwnerPersonId = owner;
        c.statement = QStringLiteral("statement");
        c.sourceChannel = QStringLiteral("qq_group");
        c.sourceSessionId = groupId;
        c.claimStatus = QStringLiteral("active");
        return c;
    }

private slots:
    void ownerCurrentGroupIsShareable()
    {
        MemoryAccessContext context;
        context.viewerPersonId = QStringLiteral("owner");
        context.channel = QStringLiteral("qq_group");
        context.sessionId = QStringLiteral("30001");
        context.viewerIsOwner = true;
        const auto decision = MemoryAccessPolicy::evaluate(
            claim(QStringLiteral("owner"), QStringLiteral("owner"),
                  QStringLiteral("owner")),
            context);
        QCOMPARE(decision.state, MemoryAccessState::Shareable);
    }

    void ownerPrivateAllowedGroupIsShareable()
    {
        MemoryAccessContext context;
        context.viewerPersonId = QStringLiteral("owner");
        context.channel = QStringLiteral("qq_private");
        context.viewerIsOwner = true;
        context.allowedGroupIds = {QStringLiteral("30001")};
        const auto decision = MemoryAccessPolicy::evaluate(
            claim(QStringLiteral("owner"), QStringLiteral("owner"),
                  QStringLiteral("owner")),
            context);
        QCOMPARE(decision.state, MemoryAccessState::Shareable);
    }

    void thirdPartyClaimIsForbidden()
    {
        MemoryAccessContext context;
        context.viewerPersonId = QStringLiteral("owner");
        context.channel = QStringLiteral("qq_group");
        context.sessionId = QStringLiteral("30001");
        context.viewerIsOwner = true;
        const auto decision = MemoryAccessPolicy::evaluate(
            claim(QStringLiteral("qq:20002"), QStringLiteral("qq:20002"),
                  QStringLiteral("qq:20002")),
            context);
        QCOMPARE(decision.state, MemoryAccessState::Forbidden);
    }

    void nonOwnerCommonGroupIsShareable()
    {
        MemoryAccessContext context;
        context.viewerPersonId = QStringLiteral("qq:20002");
        context.channel = QStringLiteral("qq_private");
        context.membershipVerified = true;
        context.allowedGroupIds = {QStringLiteral("30001")};
        const auto decision = MemoryAccessPolicy::evaluate(
            claim(QStringLiteral("qq:20002"), QStringLiteral("qq:20002"),
                  QStringLiteral("qq:20002")),
            context);
        QCOMPARE(decision.state, MemoryAccessState::Shareable);
    }

    void membershipFailureFailsClosed()
    {
        MemoryAccessContext context;
        context.viewerPersonId = QStringLiteral("qq:20002");
        context.channel = QStringLiteral("qq_private");
        context.membershipFailed = true;
        context.allowedGroupIds = {QStringLiteral("30001")};
        const auto decision = MemoryAccessPolicy::evaluate(
            claim(QStringLiteral("qq:20002"), QStringLiteral("qq:20002"),
                  QStringLiteral("qq:20002")),
            context);
        QCOMPARE(decision.state, MemoryAccessState::Forbidden);
    }

    void quarantineAndSecretAreForbidden()
    {
        MemoryAccessContext context;
        context.viewerPersonId = QStringLiteral("owner");
        context.channel = QStringLiteral("qq_group");
        context.sessionId = QStringLiteral("30001");
        context.viewerIsOwner = true;

        MemoryClaim q = claim(QStringLiteral("owner"), QStringLiteral("owner"),
                             QStringLiteral("owner"));
        q.claimStatus = QStringLiteral("quarantine");
        QCOMPARE(MemoryAccessPolicy::evaluate(q, context).state,
                 MemoryAccessState::Forbidden);

        MemoryClaim secret = claim(QStringLiteral("owner"), QStringLiteral("owner"),
                                   QStringLiteral("owner"));
        secret.privacyLevel = 3;
        QCOMPARE(MemoryAccessPolicy::evaluate(secret, context).state,
                 MemoryAccessState::Forbidden);
    }
};

QTEST_GUILESS_MAIN(TestMemoryAccessPolicy)
#include "tst_memoryaccesspolicy.moc"
