#ifndef PROMPTASSEMBLER_H
#define PROMPTASSEMBLER_H

#include <QString>
#include <QList>
#include <QJsonArray>
#include <QJsonObject>

#include "historyturn.h"
#include "utterance.h"
#include "recentcontext.h"

// 提示词/上下文组装（HUMANIZATION_PLAN 10.4 / Phase 1 前置项）
// 职责：
//   1. 把角色档案 / 情景记忆 / 长期摘要 / 关系状态按渠道与可见性过滤后拼进 system prompt（C3）
//   2. 把短期对话轮次转成 LLM messages（proactive/perception assistant-only、群聊说话人前缀）
// 本类不持有数据库，纯格式化 + 过滤，便于单测。
class PromptAssembler
{
public:
    struct MemoryContext {
        QList<CharacterProfile> userProfiles;
        QList<CharacterProfile> makoProfiles;
        QList<EpisodicMemory> memories;
        QList<InteractionSummary> interactionSummaries;
        QList<MemoryClaim> claims;
        QList<LongTermSummary> summaries;
        QList<RelationshipState> relationships;
    };

    // 基础 prompt + 过滤后的记忆上下文（桌宠输出与重构前保持一致）
    static QString buildSystemPrompt(const QString &basePrompt,
                                     const RequestMeta &meta,
                                     const MemoryContext &context);

    // 检索到的记忆（档案/情景/摘要/关系）作为独立不可信数据消息。
    // 无可披露内容时返回空对象。
    static QJsonObject buildMemoryDataMessage(const RequestMeta &meta,
                                               const MemoryContext &context);

    // PR-2.1：近期原始群消息作为独立不可信数据消息。
    static QJsonObject buildRecentContextMessage(const RequestMeta &meta,
                                                 const RecentContextBundle &bundle);

    // PR-2.2：当前用户身份与群昵称别名（可信 system 信息）。
    static QString buildViewerIdentityBlock(const RequestMeta &meta,
                                            const RecentContextBundle &recentContext,
                                            const QStringList &knownAliases = {});

    // 短期记忆 messages（含 history note；proactive/perception 不生成空 user 消息）
    static QJsonArray buildHistoryMessages(const QList<HistoryTurn> &history,
                                           const RequestMeta &meta);

    // 可见性规则
    static bool isEpisodicVisible(const EpisodicMemory &memory, const RequestMeta &meta);
    static bool isUserProfileVisible(const CharacterProfile &profile, const RequestMeta &meta);
    static bool isGroupChannel(const RequestMeta &meta);
};

#endif // PROMPTASSEMBLER_H
