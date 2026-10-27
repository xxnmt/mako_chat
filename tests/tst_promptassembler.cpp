#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>

#include "promptassembler.h"
#include "utterance.h"

class TestPromptAssembler : public QObject
{
    Q_OBJECT

private slots:
    void petPromptKeepsAllSections()
    {
        RequestMeta meta;
        meta.channel = Channel::kPet;

        PromptAssembler::MemoryContext context;
        CharacterProfile userProfile;
        userProfile.subject = QStringLiteral("user");
        userProfile.key = QStringLiteral("称呼");
        userProfile.value = QStringLiteral("欧尼酱");
        context.userProfiles.append(userProfile);

        CharacterProfile makoProfile;
        makoProfile.subject = QStringLiteral("mako");
        makoProfile.key = QStringLiteral("性格");
        makoProfile.value = QStringLiteral("温柔");
        context.makoProfiles.append(makoProfile);

        EpisodicMemory privateMemory;
        privateMemory.content = QStringLiteral("只有主人知道的事");
        privateMemory.origin = QStringLiteral("pet");
        privateMemory.visibility = QStringLiteral("private");
        privateMemory.eventTime = QDateTime(QDate(2026, 10, 1), QTime(12, 0));
        context.memories.append(privateMemory);

        RelationshipState relationship;
        relationship.dimension = QStringLiteral("intimacy");
        relationship.value = 42.0;
        context.relationships.append(relationship);

        const QString prompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);

        QVERIFY(prompt.startsWith(QStringLiteral("BASE")));
        QVERIFY(prompt.contains(QStringLiteral("# 茉子的人设")));
        QVERIFY(prompt.contains(QStringLiteral("# 关于欧尼酱")));
        QVERIFY(prompt.contains(QStringLiteral("只有主人知道的事")));
        QVERIFY(prompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(prompt.contains(QStringLiteral("亲密度: 42/100")));
    }

    void groupPromptFiltersSensitiveSections()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("group:123");

        PromptAssembler::MemoryContext context;
        CharacterProfile intimateProfile;
        intimateProfile.subject = QStringLiteral("user");
        intimateProfile.key = QStringLiteral("称呼");
        intimateProfile.value = QStringLiteral("欧尼酱");
        context.userProfiles.append(intimateProfile);

        CharacterProfile neutralProfile;
        neutralProfile.subject = QStringLiteral("user");
        neutralProfile.key = QStringLiteral("职业");
        neutralProfile.value = QStringLiteral("开发者");
        context.userProfiles.append(neutralProfile);

        EpisodicMemory petMemory;
        petMemory.content = QStringLiteral("桌宠私密回忆");
        petMemory.origin = QStringLiteral("pet");
        petMemory.visibility = QStringLiteral("private");
        context.memories.append(petMemory);

        EpisodicMemory groupMemory;
        groupMemory.content = QStringLiteral("群里聊过的事");
        groupMemory.origin = QStringLiteral("qq_group");
        groupMemory.visibility = QStringLiteral("channel");
        groupMemory.sessionId = QStringLiteral("group:123");
        context.memories.append(groupMemory);

        EpisodicMemory otherGroupMemory;
        otherGroupMemory.content = QStringLiteral("别的群的秘密");
        otherGroupMemory.origin = QStringLiteral("qq_group");
        otherGroupMemory.visibility = QStringLiteral("channel");
        otherGroupMemory.sessionId = QStringLiteral("group:999");
        context.memories.append(otherGroupMemory);

        EpisodicMemory publicMemory;
        publicMemory.content = QStringLiteral("公开设定");
        publicMemory.origin = QStringLiteral("pet");
        publicMemory.visibility = QStringLiteral("public");
        context.memories.append(publicMemory);

        RelationshipState relationship;
        relationship.dimension = QStringLiteral("intimacy");
        relationship.value = 42.0;
        context.relationships.append(relationship);

        const QString prompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);

        QVERIFY(!prompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!prompt.contains(QStringLiteral("- 称呼: 欧尼酱")));
        QVERIFY(!prompt.contains(QStringLiteral("桌宠私密回忆")));
        QVERIFY(prompt.contains(QStringLiteral("开发者")));
        QVERIFY(prompt.contains(QStringLiteral("群里聊过的事")));
        QVERIFY(prompt.contains(QStringLiteral("公开设定")));
        QVERIFY(!prompt.contains(QStringLiteral("别的群的秘密")));
    }

    void groupHistoryPrefixesSpeaker()
    {
        HistoryTurn turn;
        turn.userInput = QStringLiteral("在吗");
        turn.rawReply = QStringLiteral("{\"sentences\":[{\"zh_text\":\"在的\"}]}");
        turn.speaker = QStringLiteral("小明");
        turn.turnKind = QStringLiteral("reply");
        turn.timestamp = QDateTime(QDate(2026, 10, 27), QTime(7, 0));

        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("group:123");

        const QJsonArray messages = PromptAssembler::buildHistoryMessages({turn}, meta);

        QCOMPARE(messages.size(), 3);   // note + user + assistant
        QCOMPARE(messages.at(0).toObject()["role"].toString(), QStringLiteral("system"));
        QCOMPARE(messages.at(1).toObject()["role"].toString(), QStringLiteral("user"));
        QVERIFY(messages.at(1).toObject()["content"].toString()
                    .contains(QStringLiteral("小明：在吗")));
        QCOMPARE(messages.at(2).toObject()["role"].toString(), QStringLiteral("assistant"));
        QCOMPARE(messages.at(2).toObject()["content"].toString(), QStringLiteral("在的"));
    }

    void proactiveHistoryOnlyAssistant()
    {
        HistoryTurn turn;
        turn.userInput = QString();
        turn.rawReply = QStringLiteral("{\"sentences\":[{\"zh_text\":\"早安\"}]}");
        turn.turnKind = QStringLiteral("proactive");
        turn.timestamp = QDateTime(QDate(2026, 10, 27), QTime(7, 30));

        RequestMeta meta;
        meta.channel = Channel::kPet;

        const QJsonArray messages = PromptAssembler::buildHistoryMessages({turn}, meta);

        QCOMPARE(messages.size(), 2);   // note + assistant（无空 user）
        QCOMPARE(messages.at(1).toObject()["role"].toString(), QStringLiteral("assistant"));
        QCOMPARE(messages.at(1).toObject()["content"].toString(), QStringLiteral("早安"));
        for (const QJsonValue &message : messages) {
            QVERIFY(message.toObject()["role"].toString() != QStringLiteral("user"));
        }
    }
};

QTEST_GUILESS_MAIN(TestPromptAssembler)
#include "tst_promptassembler.moc"
