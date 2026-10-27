#include <QtTest>

#include "globalmoodstate.h"
#include "utterance.h"

class TestGlobalMoodState : public QObject
{
    Q_OBJECT

private slots:
    void defaultState()
    {
        GlobalMoodState mood;
        QCOMPARE(mood.getEmotion(), QStringLiteral("happyIdle"));
        QCOMPARE(mood.getBlush(), QStringLiteral("unblushing"));
        QCOMPARE(mood.getDistance(), QStringLiteral("far"));
        QVERIFY(mood.followExternalMood());
    }

    void petTagsSyncWithoutExternalSignal()
    {
        GlobalMoodState mood;
        int externalSignals = 0;
        connect(&mood, &GlobalMoodState::externalMoodApplied, this,
                [&externalSignals](const QMap<QString, QString> &) { ++externalSignals; });

        QMap<QString, QString> tags;
        tags["emotion"] = "loving";
        tags["blush"] = "blushing";
        tags["distance"] = "closer";
        tags["clothing"] = "pajama";   // 服装不属于情绪，全局状态忽略
        mood.applyTags(tags, Channel::kPet);

        QCOMPARE(mood.getEmotion(), QStringLiteral("loving"));
        QCOMPARE(mood.getBlush(), QStringLiteral("blushing"));
        QCOMPARE(mood.getDistance(), QStringLiteral("closer"));
        QCOMPARE(externalSignals, 0);
    }

    void externalHonorsFollowSwitch()
    {
        GlobalMoodState mood;
        QMap<QString, QString> tags;
        tags["emotion"] = "sad";
        tags["clothing"] = "pajama";

        // D1 关闭：只更新全局情绪，不驱动桌宠立绘
        mood.setFollowExternalMood(false);
        int signalCount = 0;
        QMap<QString, QString> lastTags;
        connect(&mood, &GlobalMoodState::externalMoodApplied, this,
                [&signalCount, &lastTags](const QMap<QString, QString> &t) {
                    ++signalCount;
                    lastTags = t;
                });

        mood.applyTags(tags, Channel::kQQGroup);
        QCOMPARE(mood.getEmotion(), QStringLiteral("sad"));
        QCOMPARE(signalCount, 0);

        // D1 打开：外部情绪驱动立绘（只带情绪三维）
        mood.setFollowExternalMood(true);
        QMap<QString, QString> tags2;
        tags2["emotion"] = "caring";
        tags2["distance"] = "closer";
        tags2["clothing"] = "pajama";
        mood.applyTags(tags2, Channel::kQQGroup);
        QCOMPARE(signalCount, 1);
        QCOMPARE(lastTags.value("emotion"), QStringLiteral("caring"));
        QCOMPARE(lastTags.value("distance"), QStringLiteral("closer"));
        QVERIFY(!lastTags.contains("clothing"));
    }

    void partialTagsOnlyUpdateProvided()
    {
        GlobalMoodState mood;
        QMap<QString, QString> first;
        first["emotion"] = "happyMore";
        first["blush"] = "blushing";
        mood.applyTags(first, Channel::kPet);

        QMap<QString, QString> second;
        second["distance"] = "closer";
        mood.applyTags(second, Channel::kPet);

        QCOMPARE(mood.getEmotion(), QStringLiteral("happyMore"));
        QCOMPARE(mood.getBlush(), QStringLiteral("blushing"));
        QCOMPARE(mood.getDistance(), QStringLiteral("closer"));
    }

    void descriptionMatchesAppearanceFormat()
    {
        GlobalMoodState mood;
        QMap<QString, QString> tags;
        tags["emotion"] = "loving";
        tags["blush"] = "blushing";
        tags["distance"] = "closer";
        mood.applyTags(tags, Channel::kPet);

        QCOMPARE(mood.getCurrentStateDescription("pajama"),
                 QStringLiteral("距离:closer\n服装:pajama\n脸红状态:blushing\n当前表情:loving"));
    }
};

QTEST_GUILESS_MAIN(TestGlobalMoodState)
#include "tst_globalmoodstate.moc"
