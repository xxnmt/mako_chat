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
        QVERIFY(!prompt.contains(QStringLiteral("开发者")));   // 群聊不注入人员档案
        QVERIFY(!prompt.contains(QStringLiteral("# 关于")));
        QVERIFY(prompt.contains(QStringLiteral("群里聊过的事")));
        QVERIFY(prompt.contains(QStringLiteral("公开设定")));
        QVERIFY(!prompt.contains(QStringLiteral("别的群的秘密")));
    }


    void nonMasterGroupUsesSpeakerAndHidesMasterInfo()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("group:123");
        meta.subject = QStringLiteral("qq:20002");
        meta.speaker = QStringLiteral("小明");

        PromptAssembler::MemoryContext context;
        CharacterProfile masterProfile;
        masterProfile.subject = QStringLiteral("user");
        masterProfile.key = QStringLiteral("职业");
        masterProfile.value = QStringLiteral("开发者");
        context.userProfiles.append(masterProfile);

        CharacterProfile friendProfile;
        friendProfile.subject = QStringLiteral("qq:20002");
        friendProfile.key = QStringLiteral("昵称");
        friendProfile.value = QStringLiteral("小明");
        context.userProfiles.append(friendProfile);

        RelationshipState relationship;
        relationship.dimension = QStringLiteral("intimacy");
        relationship.value = 42.0;
        context.relationships.append(relationship);

        const QString prompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("请你在称呼用户时，严格使用\"欧尼酱\"。"), meta, context);

        QVERIFY(prompt.contains(QStringLiteral("不是主人")));
        QVERIFY(prompt.contains(QStringLiteral("禁止使用“欧尼酱”")));
        QVERIFY(!prompt.contains(QStringLiteral("严格使用\"欧尼酱\"")));
        QVERIFY(!prompt.contains(QStringLiteral("开发者")));
        QVERIFY(!prompt.contains(QStringLiteral("# 关于")));
        QVERIFY(!prompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!prompt.contains(QStringLiteral("亲密度: 42/100")));
    }

    void nonMasterPrivateUsesFriendProfile()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = QStringLiteral("20002");
        meta.subject = QStringLiteral("qq:20002");
        meta.speaker = QStringLiteral("阿伟");

        PromptAssembler::MemoryContext context;
        CharacterProfile friendProfile;
        friendProfile.subject = QStringLiteral("qq:20002");
        friendProfile.key = QStringLiteral("昵称");
        friendProfile.value = QStringLiteral("阿伟");
        context.userProfiles.append(friendProfile);

        RelationshipState relationship;
        relationship.dimension = QStringLiteral("intimacy");
        relationship.value = 42.0;
        context.relationships.append(relationship);

        const QString prompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("请你在称呼用户时，严格使用\"欧尼酱\"。"), meta, context);

        QVERIFY(prompt.contains(QStringLiteral("# 关于阿伟")));
        QVERIFY(prompt.contains(QStringLiteral("朋友「阿伟」")));
        QVERIFY(!prompt.contains(QStringLiteral("# 关于欧尼酱")));
        QVERIFY(!prompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!prompt.contains(QStringLiteral("严格使用\"欧尼酱\"")));
    }

    void masterGroupKeepsAddressButHidesPrivateInfo()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("group:123");
        meta.subject = QStringLiteral("user");
        meta.speaker = QStringLiteral("主人");

        PromptAssembler::MemoryContext context;
        CharacterProfile masterProfile;
        masterProfile.subject = QStringLiteral("user");
        masterProfile.key = QStringLiteral("职业");
        masterProfile.value = QStringLiteral("开发者");
        context.userProfiles.append(masterProfile);

        RelationshipState relationship;
        relationship.dimension = QStringLiteral("intimacy");
        relationship.value = 42.0;
        context.relationships.append(relationship);

        const QString prompt = PromptAssembler::buildSystemPrompt(QStringLiteral("BASE"), meta, context);

        QVERIFY(prompt.contains(QStringLiteral("可以称呼“欧尼酱”")));
        QVERIFY(!prompt.contains(QStringLiteral("开发者")));
        QVERIFY(!prompt.contains(QStringLiteral("# 关于")));
        QVERIFY(!prompt.contains(QStringLiteral("# 我们的关系")));
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
