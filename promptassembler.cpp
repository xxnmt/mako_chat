#include "promptassembler.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

namespace {

// 群聊中不注入的“亲密档案” key 关键字（C3：群聊 prompt 不注入关系数值与亲密档案）
const QStringList &groupSensitiveProfileKeys()
{
    static const QStringList keys = {
        QStringLiteral("关系"),
        QStringLiteral("称呼"),
        QStringLiteral("情感"),
        QStringLiteral("依赖"),
        QStringLiteral("性格"),
    };
    return keys;
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
    if (!isGroupChannel(meta)) {
        return true;
    }
    for (const QString &sensitiveKey : groupSensitiveProfileKeys()) {
        if (profile.key.contains(sensitiveKey)) {
            return false;
        }
    }
    return true;
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
    QString prompt = basePrompt;

    // 1. 角色档案：茉子人设始终注入；主人档案在群聊按 C3 过滤
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
        prompt += "\n# 关于欧尼酱\n";
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

    // 4. 关系状态：群聊不注入数值（C3）
    if (!isGroupChannel(meta) && !context.relationships.isEmpty()) {
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
