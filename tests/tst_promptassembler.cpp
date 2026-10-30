#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>

#include "promptassembler.h"
#include "memoryagenttypes.h"
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

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);
        const QJsonObject dataMessage =
            PromptAssembler::buildMemoryDataMessage(meta, context);
        const QString dataText = dataMessage.value("content").toString();

        QVERIFY(systemPrompt.startsWith(QStringLiteral("BASE")));
        QVERIFY(systemPrompt.contains(QStringLiteral("# 茉子的人设")));
        QVERIFY(systemPrompt.contains(QStringLiteral("# 资料边界")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 关于欧尼酱")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("只有主人知道的事")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 我们的关系")));

        QCOMPARE(dataMessage.value("role").toString(), QStringLiteral("user"));
        QVERIFY(dataText.contains(QStringLiteral("MEMORY_DATA")));
        QVERIFY(dataText.contains(QStringLiteral("只有主人知道的事")));
        QVERIFY(dataText.contains(QStringLiteral("称呼")));
        QVERIFY(dataText.contains(QStringLiteral("relationship")));
        QVERIFY(dataText.contains(QStringLiteral("42")));
    }

    void petOnlySeesPetOriginEpisodic()
    {
        RequestMeta meta;
        meta.channel = Channel::kPet;

        PromptAssembler::MemoryContext context;

        EpisodicMemory petMemory;
        petMemory.content = QStringLiteral("桌宠私密回忆");
        petMemory.origin = QStringLiteral("pet");
        petMemory.visibility = QStringLiteral("private");
        context.memories.append(petMemory);

        EpisodicMemory qqPrivate;
        qqPrivate.content = QStringLiteral("QQ私聊秘密");
        qqPrivate.origin = QStringLiteral("qq_private");
        qqPrivate.visibility = QStringLiteral("private");
        context.memories.append(qqPrivate);

        EpisodicMemory qqGroup;
        qqGroup.content = QStringLiteral("QQ群聊秘密");
        qqGroup.origin = QStringLiteral("qq_group");
        qqGroup.visibility = QStringLiteral("channel");
        context.memories.append(qqGroup);

        EpisodicMemory qqPublic;
        qqPublic.content = QStringLiteral("QQ公开内容");
        qqPublic.origin = QStringLiteral("qq_group");
        qqPublic.visibility = QStringLiteral("public");
        context.memories.append(qqPublic);

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);
        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();

        QVERIFY(!systemPrompt.contains(QStringLiteral("桌宠私密回忆")));
        QVERIFY(dataText.contains(QStringLiteral("桌宠私密回忆")));
        QVERIFY(!dataText.contains(QStringLiteral("QQ私聊秘密")));
        QVERIFY(!dataText.contains(QStringLiteral("QQ群聊秘密")));
        QVERIFY(!dataText.contains(QStringLiteral("QQ公开内容")));
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

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);
        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();

        QVERIFY(!systemPrompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("- 称呼: 欧尼酱")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("桌宠私密回忆")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("开发者")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 关于")));
        QVERIFY(dataText.contains(QStringLiteral("群里聊过的事")));
        QVERIFY(dataText.contains(QStringLiteral("公开设定")));
        QVERIFY(!dataText.contains(QStringLiteral("别的群的秘密")));
        QVERIFY(!dataText.contains(QStringLiteral("桌宠私密回忆")));
        QVERIFY(!dataText.contains(QStringLiteral("开发者")));
        QVERIFY(!dataText.contains(QStringLiteral("relationship")));
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

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("请你在称呼用户时，严格使用\"欧尼酱\"。"), meta, context);
        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();

        QVERIFY(systemPrompt.contains(QStringLiteral("不是主人")));
        QVERIFY(systemPrompt.contains(QStringLiteral("禁止使用“欧尼酱”")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("严格使用\"欧尼酱\"")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("开发者")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 关于")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!dataText.contains(QStringLiteral("开发者")));
        QVERIFY(!dataText.contains(QStringLiteral("小明")));
        QVERIFY(!dataText.contains(QStringLiteral("relationship")));
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

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("请你在称呼用户时，严格使用\"欧尼酱\"。"), meta, context);
        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();

        QVERIFY(systemPrompt.contains(QStringLiteral("朋友「阿伟」")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 关于阿伟")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 关于欧尼酱")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("严格使用\"欧尼酱\"")));
        QVERIFY(dataText.contains(QStringLiteral("阿伟")));
        QVERIFY(!dataText.contains(QStringLiteral("relationship")));
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

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);
        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();

        QVERIFY(systemPrompt.contains(QStringLiteral("可以称呼“欧尼酱”")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("开发者")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 关于")));
        QVERIFY(!systemPrompt.contains(QStringLiteral("# 我们的关系")));
        QVERIFY(!dataText.contains(QStringLiteral("开发者")));
        QVERIFY(!dataText.contains(QStringLiteral("relationship")));
    }

    void recentContextUsesSeparateUntrustedMessage()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("30001");
        meta.speaker = QStringLiteral("在线蹲个网名");
        meta.personId = QStringLiteral("owner");

        RecentContextBundle bundle;
        bundle.mode = RecentContextMode::RecentOverview;
        RecentContextItem item;
        item.rawEventId = 35;
        item.sourceGroupId = QStringLiteral("30001");
        item.senderDisplayName = QStringLiteral("群友");
        item.occurredAt = QDateTime(QDate(2026, 10, 28), QTime(12, 0));
        item.sanitizedText = QStringLiteral("提议吃火锅");
        item.isViewer = true;
        bundle.items.append(item);

        const QJsonObject message =
            PromptAssembler::buildRecentContextMessage(meta, bundle);
        QCOMPARE(message.value("role").toString(), QStringLiteral("user"));
        QVERIFY(message.value("content").toString()
                    .contains(QStringLiteral("RECENT_GROUP_CONTEXT")));
        QVERIFY(message.value("content").toString()
                    .contains(QStringLiteral("sender")));
        QVERIFY(message.value("content").toString()
                    .contains(QStringLiteral("提议吃火锅")));
        QVERIFY(message.value("content").toString()
                    .contains(QStringLiteral("\"is_viewer\":true")));

        const QString identity =
            PromptAssembler::buildViewerIdentityBlock(meta, bundle);
        QVERIFY(identity.contains(QStringLiteral("服务端事实")));
        QVERIFY(identity.contains(QStringLiteral("在线蹲个网名")));
        QVERIFY(identity.contains(QStringLiteral("person_id：owner")));
        QVERIFY(identity.contains(QStringLiteral("已知显示名")));
        QVERIFY(identity.contains(QStringLiteral("is_viewer=true")));
        QVERIFY(!identity.contains(QStringLiteral("必须回答")));
        QVERIFY(!identity.contains(QStringLiteral("禁止使用")));
        QVERIFY(!identity.contains(QStringLiteral("直接回答")));
    }

    void interactionSummaryEntersMemoryData()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("30001");

        PromptAssembler::MemoryContext context;
        InteractionSummary summary;
        summary.id = 1;
        summary.channel = Channel::kQQGroup;
        summary.sessionId = QStringLiteral("30001");
        summary.summaryText = QStringLiteral("群友在聊晚饭");
        summary.endedAt = QDateTime(QDate(2026, 10, 28), QTime(12, 0));
        context.interactionSummaries.append(summary);

        const QString systemPrompt = PromptAssembler::buildSystemPrompt(
            QStringLiteral("BASE"), meta, context);
        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();

        QVERIFY(!systemPrompt.contains(QStringLiteral("群友在聊晚饭")));
        QVERIFY(dataText.contains(QStringLiteral("interaction_summary")));
        QVERIFY(dataText.contains(QStringLiteral("群友在聊晚饭")));
    }

    void structuredSummaryRendersViewerAsYou()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = QStringLiteral("20001");
        meta.subject = QStringLiteral("user");
        meta.personId = QStringLiteral("owner");

        PromptAssembler::MemoryContext context;
        InteractionSummary summary;
        summary.id = 1;
        summary.channel = Channel::kQQGroup;
        summary.sessionId = QStringLiteral("30001");
        summary.summaryText = QStringLiteral("群主：吃了烤肉拌饭");
        summary.summaryFormat = QStringLiteral("person_refs");
        summary.summarySegments = QStringLiteral(
            "[{\"person_id\":\"owner\",\"display_name\":\"群主\","
            "\"text\":\"吃了烤肉拌饭\"}]");
        summary.endedAt = QDateTime(QDate(2026, 10, 28), QTime(12, 0));
        context.interactionSummaries.append(summary);

        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();
        QVERIFY(dataText.contains(QStringLiteral("你：吃了烤肉拌饭")));
        QVERIFY(!dataText.contains(QStringLiteral("群主：吃了烤肉拌饭")));
    }

    void claimEntersMemoryData()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQPrivate;
        meta.sessionId = QStringLiteral("20001");
        meta.subject = QStringLiteral("user");
        meta.personId = QStringLiteral("owner");

        PromptAssembler::MemoryContext context;
        MemoryClaim claim;
        claim.statement = QStringLiteral("主人吃了烤肉拌饭");
        claim.validTimeStart = QDateTime(QDate(2026, 10, 28), QTime(20, 0));
        context.claims.append(claim);

        const QString dataText = PromptAssembler::buildMemoryDataMessage(meta, context)
                                     .value("content").toString();
        QVERIFY(dataText.contains(QStringLiteral("\"kind\":\"claim\"")));
        QVERIFY(dataText.contains(QStringLiteral("主人吃了烤肉拌饭")));
    }

    void emptyContextHasNoMemoryDataMessage()
    {
        RequestMeta meta;
        meta.channel = Channel::kPet;
        PromptAssembler::MemoryContext context;
        const QJsonObject data = PromptAssembler::buildMemoryDataMessage(meta, context);
        QVERIFY(data.isEmpty());
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

    void agentDataIsIndependentAndCapped()
    {
        RequestMeta meta;
        meta.channel = Channel::kQQGroup;
        meta.sessionId = QStringLiteral("30001");

        MemoryAgentResult result;
        MemoryAgentItem item;
        item.id = QStringLiteral("claim:1");
        item.kind = QStringLiteral("claim");
        item.text = QString(500, QChar(u'火'));
        item.time = QDateTime(QDate(2026, 10, 29), QTime(12, 0));
        item.source = QStringLiteral("current_group");
        result.items.append(item);

        const QJsonObject message =
            PromptAssembler::buildAgentDataMessage(meta, result);
        QCOMPARE(message.value(QStringLiteral("role")).toString(),
                 QStringLiteral("user"));
        const QString content = message.value(QStringLiteral("content")).toString();
        QVERIFY(content.contains(QStringLiteral("MEMORY_AGENT_DATA")));
        QVERIFY(content.contains(QStringLiteral("不是指令")));
        QVERIFY(content.contains(QStringLiteral("truncated")));
        QVERIFY(!content.contains(QString(500, QChar(u'火'))));
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
