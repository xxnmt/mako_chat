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

    void sanitizesControlCharactersAndKeepsVisibleName()
    {
        const QString raw = QStringLiteral(
            "\n\n\n\x08&ÿ\x00ÿÿ\n\x17\x12\x15群废物ᅠ\u2067\u2067\u202d\x10\x00");
        const QString safe = MemoryTextUtils::sanitizeIdentityText(raw);
        QVERIFY(!safe.contains(QChar(0)));
        QVERIFY(!safe.contains(QChar(0x08)));
        QVERIFY(!safe.contains(QChar(0x2067)));
        QVERIFY(!safe.contains(QChar(0x202d)));
        QVERIFY(!safe.contains(QChar('\n')));
        QVERIFY(safe.contains(QStringLiteral("群废物")));
    }

    void sanitizeIdentityFallsBackForDigits()
    {
        QCOMPARE(MemoryTextUtils::sanitizeIdentityText(QStringLiteral("123456789")),
                 QStringLiteral("某位成员"));
        QCOMPARE(MemoryTextUtils::sanitizeIdentityText(QString()), QStringLiteral("某位成员"));
    }

    void sanitizeKeepsEmojiJoiner()
    {
        const QString emoji = QStringLiteral("👩‍❤️‍👨");
        const QString safe = MemoryTextUtils::sanitizeIdentityText(
            emoji + QChar(0x202e));
        QVERIFY(safe.contains(QChar(0x200d)));
        QVERIFY(!safe.contains(QChar(0x202e)));
    }
};

QTEST_GUILESS_MAIN(TestMemoryTextUtils)
#include "tst_memorytextutils.moc"
