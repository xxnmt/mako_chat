#ifndef HISTORYTURN_H
#define HISTORYTURN_H
#include <QString>
#include <QDateTime>

//聊天记录
struct HistoryTurn {
    qlonglong id = -1;//id
    QDateTime timestamp;//时间戳
    QString userInput;//用户输入；主动/感知轮次为 SQL NULL（读取后为空串）
    QString rawReply;//未处理回复
    QString turnKind = QStringLiteral("reply");// reply / proactive / perception
    QString subject = QStringLiteral("user");// 本轮对象：user / qq:<uin> / group:<群号>
};

//角色档案（长期稳定特质，user 或 mako）
struct CharacterProfile {
    qlonglong id = -1;
    QString subject;    // 'user' 或 'mako'
    QString key;        // 如 'nickname'/'occupation'/'persona'
    QString value;
    QDateTime updatedAt;
};

//情景记忆（事件、承诺、冲突、里程碑）
struct EpisodicMemory {
    qlonglong id = -1;
    QString content;    // 事件描述
    QDateTime eventTime;
    double importance = 0.5;   // 0.0-1.0
    QString type;       // 'event'/'promise'/'conflict'/'milestone'
    QString status;     // 'active'/'resolved'/'broken'
    QDateTime lastAccessed;
    QString sourceIds;
    QString origin = QStringLiteral("pet");          // 渠道溯源：pet / qq_private / qq_group / bilibili / screen
    QString visibility = QStringLiteral("private");  // private / channel / public
    QString subject = QStringLiteral("user");        // 记忆所属主体
};

//记忆摘要（保留兼容）
struct LongTermSummary {
    qlonglong id = -1;//id
    QString summaryText;//摘要内容
    qlonglong coveredTurnEndId = -1;//摘要末尾
    QString sourceIds; //摘要id
    bool isDirty = false;//摘要状态 0 正常 1 待重建
    QDateTime createdAt;//创建于
    QDateTime updatedAt;//更新于
};

//关系状态（量化 AI 与各对话主体之间的关系维度）
struct RelationshipState {
    QString subject = QStringLiteral("user");  // 'user'(主人) / 'qq:<uin>' / 'group:<群号>'
    QString dimension;   // 'intimacy'(亲密度) / 'trust'(信任度)
    double value = 0.0;  // 0-100
    QDateTime updatedAt;
};


#endif // HISTORYTURN_H
