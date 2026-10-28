#include <QtTest>
#include <algorithm>

#include "recentcontext.h"

class TestRecentContext : public QObject
{
    Q_OBJECT

private:
    static RecentContextItem item(qlonglong id, const QDateTime &time,
                                  const QString &text)
    {
        RecentContextItem i;
        i.rawEventId = id;
        i.sourceGroupId = QStringLiteral("738559702");
        i.senderId = QStringLiteral("20002");
        i.senderDisplayName = QStringLiteral("群友");
        i.occurredAt = time;
        i.sanitizedText = text;
        i.messageId = QString::number(id);
        return i;
    }

    static QList<RecentContextItem> fifty(const QDateTime &start)
    {
        QList<RecentContextItem> items;
        for (int i = 1; i <= 50; ++i) {
            items.append(item(i, start.addSecs(i * 10),
                              i == 35 ? QStringLiteral("我提议吃火锅")
                                      : QStringLiteral("日常闲聊")));
        }
        return items;
    }

    static RecentContextQuery query(RecentContextMode mode, const QString &text)
    {
        RecentContextQuery q;
        q.mode = mode;
        q.queryText = text;
        q.itemLimit = 10;
        q.itemLimitOverview = 50;
        q.charBudget = 1200;
        q.overviewCharBudget = 5000;
        q.hotWindowItems = 10;
        q.warmLineMaxChars = 24;
        return q;
    }

private slots:
    void detectsOverviewAndTargeted()
    {
        QCOMPARE(RecentContextPlanner::detectMode(QStringLiteral("刚才群里聊什么了")),
                 RecentContextMode::RecentOverview);
        QCOMPARE(RecentContextPlanner::detectMode(
                     QStringLiteral("西汸忆落刚才群里聊了什么？")),
                 RecentContextMode::RecentOverview);
        QCOMPARE(RecentContextPlanner::detectMode(QStringLiteral("谁提议吃火锅")),
                 RecentContextMode::TargetedLookup);
        QVERIFY(RecentContextPlanner::isRecentCue(QStringLiteral("刚才聊了什么")));
        QVERIFY(RecentContextPlanner::isHistoricalCue(QStringLiteral("上次聊了什么")));
    }

    void targetedFindsOlderMessage()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const RecentContextQuery q = query(
            RecentContextMode::TargetedLookup, QStringLiteral("谁提议吃火锅"));
        const RecentContextBundle bundle =
            RecentContextPlanner::build(fifty(start), q);
        QVERIFY(!bundle.items.isEmpty());
        QVERIFY(bundle.items.size() <= 10);
        QVERIFY(std::any_of(bundle.items.cbegin(), bundle.items.cend(),
                            [](const RecentContextItem &i) {
            return i.rawEventId == 35;
        }));
        QVERIFY(std::any_of(bundle.items.cbegin(), bundle.items.cend(),
                            [](const RecentContextItem &i) {
            return i.rawEventId == 33;
        }));
    }

    void overviewCoversOlderMessage()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const RecentContextQuery q = query(
            RecentContextMode::RecentOverview, QStringLiteral("刚才群里聊什么了"));
        const RecentContextBundle bundle =
            RecentContextPlanner::build(fifty(start), q);
        QVERIFY(!bundle.items.isEmpty());
        bool found = false;
        for (const RecentContextItem &i : bundle.items) {
            if (i.rawEventId == 35) {
                found = true;
                QVERIFY(i.warm);
                QVERIFY(i.sanitizedText.size() <= 24);
            }
        }
        QVERIFY(found);
    }

    void allowedGroupsRespectScope()
    {
        const QList<qlonglong> groups = {738559702LL, 2151040806LL};

        QCOMPARE(RecentContextPlanner::allowedGroupIdsForRequest(
                     true, false, QStringLiteral("738559702"), true, groups),
                 (QStringList{QStringLiteral("738559702")}));
        QVERIFY(RecentContextPlanner::allowedGroupIdsForRequest(
                     true, false, QStringLiteral("999999"), true, groups).isEmpty());
        QCOMPARE(RecentContextPlanner::allowedGroupIdsForRequest(
                     false, true, QStringLiteral("2014366825"), true, groups).size(),
                 2);
        QVERIFY(RecentContextPlanner::allowedGroupIdsForRequest(
                     false, false, QStringLiteral("2014366825"), true, groups).isEmpty());
        QVERIFY(RecentContextPlanner::allowedGroupIdsForRequest(
                     false, true, QStringLiteral("2014366825"), false, groups).isEmpty());
    }

    void targetsSenderNickname()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        QList<RecentContextItem> items;
        RecentContextItem a = item(1, start, QStringLiteral("晚上吃了没"));
        a.senderDisplayName = QStringLiteral("在线蹲个网名");
        RecentContextItem b = item(2, start.addSecs(10), QStringLiteral("随便聊聊"));
        b.senderDisplayName = QStringLiteral("原头帽叠一世");
        items << a << b;

        RecentContextQuery q = query(
            RecentContextMode::TargetedLookup, QStringLiteral("在线蹲个网名是谁"));
        const RecentContextBundle bundle =
            RecentContextPlanner::build(items, q);
        QVERIFY(!bundle.items.isEmpty());
        QCOMPARE(bundle.items.first().senderDisplayName,
                 QStringLiteral("在线蹲个网名"));
    }

    void overviewDefaultBudgetStillCoversOlderMessage()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        RecentContextQuery q = query(
            RecentContextMode::RecentOverview, QStringLiteral("刚才群里聊什么了"));
        q.overviewCharBudget = 2200;
        const RecentContextBundle bundle =
            RecentContextPlanner::build(fifty(start), q);
        QVERIFY(std::any_of(bundle.items.cbegin(), bundle.items.cend(),
                            [](const RecentContextItem &i) {
            return i.rawEventId == 35;
        }));
    }

    void emptyTargetedFallsBackForOwnerOrGroup()
    {
        QVERIFY(RecentContextPlanner::shouldFallbackToOverview(
            RecentContextMode::TargetedLookup, true, false, true));
        QVERIFY(RecentContextPlanner::shouldFallbackToOverview(
            RecentContextMode::TargetedLookup, true, true, false));
        QVERIFY(!RecentContextPlanner::shouldFallbackToOverview(
            RecentContextMode::TargetedLookup, false, false, true));
        QVERIFY(!RecentContextPlanner::shouldFallbackToOverview(
            RecentContextMode::TargetedLookup, true, false, false));
        QVERIFY(!RecentContextPlanner::shouldFallbackToOverview(
            RecentContextMode::RecentOverview, true, true, true));
    }

    void budgetsAreIndependent()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        RecentContextQuery targeted = query(
            RecentContextMode::TargetedLookup, QStringLiteral("谁提议吃火锅"));
        targeted.charBudget = 20;
        const RecentContextBundle targetedBundle =
            RecentContextPlanner::build(fifty(start), targeted);
        QVERIFY(targetedBundle.items.size() <= 1);
        QVERIFY(targetedBundle.truncated);

        RecentContextQuery overview = query(
            RecentContextMode::RecentOverview, QStringLiteral("刚才群里聊什么了"));
        overview.overviewCharBudget = 100;
        const RecentContextBundle overviewBundle =
            RecentContextPlanner::build(fifty(start), overview);
        QVERIFY(overviewBundle.items.size() < 50);
        QVERIFY(overviewBundle.truncated);
    }

    void noMatchDoesNotInvent()
    {
        const QDateTime start(QDate(2026, 10, 28), QTime(12, 0));
        const RecentContextQuery q = query(
            RecentContextMode::TargetedLookup, QStringLiteral("不存在的关键词"));
        const RecentContextBundle bundle =
            RecentContextPlanner::build(fifty(start), q);
        QVERIFY(bundle.items.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRecentContext)
#include "tst_recentcontext.moc"
