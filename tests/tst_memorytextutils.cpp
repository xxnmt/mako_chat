#include <QtTest>

#include "memorytextutils.h"

class TestMemoryTextUtils : public QObject
{
    Q_OBJECT

private slots:
    void replacesKnownSpeakerLabels()
    {
        const QHash<QString, QString> labels = {
            {QStringLiteral("P1"), QStringLiteral("在线蹲个网名")},
            {QStringLiteral("P2"), QStringLiteral("原头帽叠一世")}
        };
        const QString text = MemoryTextUtils::replaceSpeakerLabels(
            QStringLiteral("P1 说了什么，P2 回应 P1。"), labels);
        QCOMPARE(text, QStringLiteral("在线蹲个网名 说了什么，原头帽叠一世 回应 在线蹲个网名。"));
        QVERIFY(!MemoryTextUtils::containsSpeakerLabel(text));
    }

    void unknownSpeakerLabelsBecomeAnonymous()
    {
        const QString text = MemoryTextUtils::replaceSpeakerLabels(
            QStringLiteral("P7 说了什么"), QHash<QString, QString>());
        QCOMPARE(text, QStringLiteral("某位成员 说了什么"));
    }

    void doesNotReplacePInsideWords()
    {
        const QHash<QString, QString> labels = {
            {QStringLiteral("P1"), QStringLiteral("小明")}
        };
        const QString text = MemoryTextUtils::replaceSpeakerLabels(
            QStringLiteral("API1 P1"), labels);
        QCOMPARE(text, QStringLiteral("API1 小明"));
    }
};

QTEST_GUILESS_MAIN(TestMemoryTextUtils)
#include "tst_memorytextutils.moc"
