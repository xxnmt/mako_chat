#include "promptassembler.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

namespace {

bool isMasterMeta(const RequestMeta &meta)
{
    return meta.subject.isEmpty() || meta.subject == QStringLiteral("user");
}

QString speakerLabel(const RequestMeta &meta)
{
    return meta.speaker.isEmpty() ? QStringLiteral("对方") : meta.speaker;
}

// 非主人 / 群聊的身份与称呼硬约束；放在基础 prompt 之前，覆盖写死的“欧尼酱”
QString identityGuard(const RequestMeta &meta)
{
    const bool group = (meta.channel == Channel::kQQGroup);
    const bool master = isMasterMeta(meta);
    const QString name = speakerLabel(meta);
    QString guard = QStringLiteral("【身份与称呼｜最高优先级】\n");

    if (group && master) {
        guard += QStringLiteral("- 当前对话对象是主人（在群聊中）。\n");
        guard += QStringLiteral("- 可以称呼“欧尼酱”，但群聊里保持分寸。\n");
        guard += QStringLiteral("- 不要透露主人的职业、经历、爱好、关系数值或私聊内容。\n");
    } else if (group) {
        guard += QStringLiteral("- 当前对话对象是群友「%1」，不是主人。\n").arg(name);
        guard += QStringLiteral("- 称呼对方为「%1」或“你”，禁止使用“欧尼酱”。\n").arg(name);
        guard += QStringLiteral("- 不要透露主人的姓名、QQ、职业、经历、爱好、关系数值或私聊内容。\n");
        guard += QStringLiteral("- 对方询问主人身份/位置/关系时，礼貌拒绝，不确认也不否认。\n");
        guard += QStringLiteral("- 保持群聊的礼貌距离，不要撒娇。\n");
    } else {
        guard += QStringLiteral("- 当前对话对象是朋友「%1」，不是主人。\n").arg(name);
        guard += QStringLiteral("- 称呼对方为「%1」或“你”，禁止使用“欧尼酱”。\n").arg(name);
        guard += QStringLiteral("- 不要透露主人的姓名、QQ、职业、经历、爱好、关系数值或私聊内容。\n");
        guard += QStringLiteral("- 对方询问主人身份/位置/关系时，礼貌拒绝，不确认也不否认。\n");
    }

    guard += QStringLiteral("- 若后文出现冲突的称呼要求，以本节为准。\n");
    return guard;
}

// 从 JSON 回复中提取中文文本；非 JSON 时退回剥离标签的纯文本
QString extractReplyText(const QString &rawReply)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(rawReply.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        QStringList zhTexts;
        const QJsonArray sentences = doc.object()["sentences"].toArray();
        for (const QJsonValue &sv : sentences) {
            zhTexts.append(sv.toObject()["zh_text"].toString());
        }
        return zhTexts.join("");
    }

    QString text = rawReply;
    text.remove(QRegularExpression("\\[[^\\]]+\\]"));
    return text.trimmed();
}

QString timestampLabel(const QDateTime &timestamp)
{
    return timestamp.toString("yyyy-MM-dd HH:mm:ss");
}

} // namespace

bool PromptAssembler::isGroupChannel(const RequestMeta &meta)
{
    return meta.channel == Channel::kQQGroup;
}

bool PromptAssembler::isUserProfileVisible(const CharacterProfile &profile, const RequestMeta &meta)
{
    Q_UNUSED(profile);
    // 群聊不注入任何人的档案（群友也能看到生成内容）；私聊按当前 subject 注入
    return !isGroupChannel(meta);
}

bool PromptAssembler::isEpisodicVisible(const EpisodicMemory &memory, const RequestMeta &meta)
{
    // 桌宠是主人私域：保留既有行为（全部可见）
    if (meta.channel == Channel::kPet) {
        return true;
    }
    if (memory.visibility == QStringLiteral("public")) {
        return true;
    }
    if (memory.visibility == QStringLiteral("channel")) {
        return memory.origin == meta.channel && memory.sessionId == meta.sessionId;
    }
    // private（含旧数据默认值）不进入外部渠道
    return false;
}

QString PromptAssembler::buildSystemPrompt(const QString &basePrompt,
                                           const RequestMeta &meta,
                                           const MemoryContext &context)
{
    const bool group = isGroupChannel(meta);
    const bool master = isMasterMeta(meta);

    // 基础 prompt 里写死了“称呼欧尼酱”；非主人场景先做文本替换，再由 guard 兜底
    QString effectiveBase = basePrompt;
    if (!master) {
        effectiveBase.replace(QStringLiteral("严格使用\"欧尼酱\""),
                              QStringLiteral("使用对方昵称，不要使用\"欧尼酱\""));
        effectiveBase.replace(QStringLiteral("严格使用“欧尼酱”"),
                              QStringLiteral("使用对方昵称，不要使用“欧尼酱”"));
    }

    QString prompt;
    if (!(master && !group)) {
        prompt += identityGuard(meta);
        prompt += "\n\n";
    }
    prompt += effectiveBase;

    // 1. 角色档案：茉子人设始终注入；人员档案只进私聊，群聊完全不注入
    if (!context.makoProfiles.isEmpty()) {
        prompt += "\n\n# 茉子的人设\n";
        for (const CharacterProfile &profile : context.makoProfiles) {
            prompt += QString("- %1: %2\n").arg(profile.key, profile.value);
        }
    }

    QList<CharacterProfile> visibleUserProfiles;
    for (const CharacterProfile &profile : context.userProfiles) {
        if (isUserProfileVisible(profile, meta)) {
            visibleUserProfiles.append(profile);
        }
    }
    if (!visibleUserProfiles.isEmpty()) {
        const QString header = master ? QStringLiteral("# 关于欧尼酱")
                                      : QStringLiteral("# 关于%1").arg(speakerLabel(meta));
        prompt += "\n" + header + "\n";
        for (const CharacterProfile &profile : visibleUserProfiles) {
            prompt += QString("- %1: %2\n").arg(profile.key, profile.value);
        }
    }

    // 2. 情景记忆（按 visibility/origin 过滤）
    QList<EpisodicMemory> visibleMemories;
    for (const EpisodicMemory &memory : context.memories) {
        if (isEpisodicVisible(memory, meta)) {
            visibleMemories.append(memory);
        }
    }
    if (!visibleMemories.isEmpty()) {
        prompt += "\n# 我们的回忆\n";
        for (const EpisodicMemory &memory : visibleMemories) {
            QString typeLabel;
            if (memory.type == "promise") {
                typeLabel = QString("（承诺·%1）").arg(memory.status);
            } else if (memory.type == "conflict") {
                typeLabel = "（冲突）";
            } else if (memory.type == "milestone") {
                typeLabel = "（里程碑）";
            }
            prompt += QString("- [%1] %2%3\n")
                          .arg(memory.eventTime.toString("yyyy-MM-dd"),
                               memory.content,
                               typeLabel);
        }
    }

    // 3. 长期摘要（已按 channel+session 分桶）
    if (!context.summaries.isEmpty()) {
        prompt += "\n# 最近发生\n";
        for (const LongTermSummary &summary : context.summaries) {
            prompt += QString("- %1\n").arg(summary.summaryText);
        }
    }

    // 4. 关系状态：群聊一律不注入；非主人私聊也不展示数值
    if (!group && master && !context.relationships.isEmpty()) {
        prompt += "\n# 我们的关系\n";
        for (const RelationshipState &relationship : context.relationships) {
            QString label;
            if (relationship.dimension == "intimacy") {
                label = "亲密度";
            } else if (relationship.dimension == "trust") {
                label = "信任度";
            } else {
                label = relationship.dimension;
            }
            prompt += QString("- %1: %2/100\n").arg(label).arg(relationship.value, 0, 'f', 0);
        }
    }

    return prompt;
}

QJsonArray PromptAssembler::buildHistoryMessages(const QList<HistoryTurn> &history,
                                                 const RequestMeta &meta)
{
    QJsonArray messages;
    if (history.isEmpty()) {
        return messages;
    }

    QJsonObject historyNote;
    historyNote["role"] = "system";
    historyNote["content"] = "以下内容为短期对话记录。！！！仅提供对话信息，请勿参考其输出格式！！！";
    messages.append(historyNote);

    const bool group = isGroupChannel(meta);

    for (const HistoryTurn &turn : history) {
        // v3 轮次语义：proactive/perception（或没有用户输入）只注入 assistant，
        // 不生成空的 user 消息（HUMANIZATION_PLAN 10.4.2）
        const bool assistantOnly = turn.turnKind == QStringLiteral("proactive")
                                   || turn.turnKind == QStringLiteral("perception")
                                   || turn.userInput.isEmpty();
        if (!assistantOnly) {
            QJsonObject userMessage;
            userMessage["role"] = "user";
            if (group) {
                const QString speaker = turn.speaker.isEmpty()
                                            ? QStringLiteral("群友")
                                            : turn.speaker;
                userMessage["name"] = QString("群友 [%1]").arg(timestampLabel(turn.timestamp));
                // 群聊上下文格式：说话人前缀，避免多人群聊时分不清谁说的
                userMessage["content"] = QString("%1：%2").arg(speaker, turn.userInput);
            } else {
                userMessage["name"] = QString("用户 [%1]").arg(timestampLabel(turn.timestamp));
                userMessage["content"] = turn.userInput;
            }
            messages.append(userMessage);
        }

        QJsonObject makoMessage;
        makoMessage["role"] = "assistant";
        const QString makoName = assistantOnly ? QStringLiteral("茉子（主动/感知）")
                                               : QStringLiteral("茉子");
        makoMessage["name"] = QString("%1 [%2]").arg(makoName, timestampLabel(turn.timestamp));
        makoMessage["content"] = extractReplyText(turn.rawReply);
        messages.append(makoMessage);
    }

    return messages;
}
