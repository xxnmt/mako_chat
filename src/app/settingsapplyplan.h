#pragma once

// B5 设置保存的差异判定：只根据“保存前后的有效配置快照”决定哪些模块需要更新。
// 这是纯函数（无状态、无副作用），供 AppController 使用，也用于确定性测试；
// 不是第二套配置系统，不改变 ConfigManager 的读取/保存职责。

#include <QJsonObject>
#include <QJsonValue>

namespace MakoSettings {

inline QJsonValue configAt(const QJsonObject &snapshot, const QString &file,
                           const QString &path)
{
    QJsonValue value = snapshot.value(file);
    for (const QString &part : path.split(QLatin1Char('.'))) {
        if (!value.isObject()) return {};
        value = value.toObject().value(part);
    }
    return value;
}

inline bool configChanged(const QJsonObject &before, const QJsonObject &after,
                          const QString &file, const QString &path)
{
    return configAt(before, file, path) != configAt(after, file, path);
}

struct ApplyPlan {
    bool logging = false;
    bool deepseekKey = false;
    bool followExternalMood = false;
    bool masterAccounts = false;
    bool qqConfig = false;
    bool qqReconnect = false;
    bool membership = false;
    bool indexReconfigure = false;
    bool ttsReload = false;
    bool timeSettings = false;
    bool permissions = false;

    bool anyModuleUpdate() const
    {
        return logging || deepseekKey || followExternalMood || masterAccounts
            || qqConfig || qqReconnect || membership || indexReconfigure
            || ttsReload || timeSettings || permissions;
    }
};

inline ApplyPlan plan(const QJsonObject &before, const QJsonObject &after)
{
    const auto setting = [&](const QString &path) {
        return configChanged(before, after, QStringLiteral("setting"), path);
    };
    const auto identity = [&](const QString &path) {
        return configChanged(before, after, QStringLiteral("identity"), path);
    };
    ApplyPlan result;
    result.logging = setting(QStringLiteral("logging.directory"))
        || setting(QStringLiteral("logging.separate_runs"));
    result.deepseekKey = setting(QStringLiteral("api.deepseek_api_key"));
    result.followExternalMood = identity(QStringLiteral("appearance.follow_external_mood"));
    result.masterAccounts = identity(QStringLiteral("identity.master.qq"))
        || identity(QStringLiteral("identity.self.qq"));
    result.qqConfig = setting(QStringLiteral("qq"));
    result.qqReconnect = setting(QStringLiteral("qq.enabled"))
        || setting(QStringLiteral("qq.ws_url"))
        || setting(QStringLiteral("qq.access_token"));
    result.membership = result.masterAccounts
        || setting(QStringLiteral("qq.membership_refresh_minutes"))
        || setting(QStringLiteral("qq.membership_max_age_minutes"))
        || setting(QStringLiteral("qq.group_memory_allow"))
        || setting(QStringLiteral("qq.group_memory_mode"))
        || setting(QStringLiteral("qq.group_sender_allowlist"));
    // 普通候选/重排参数不打断索引；只有索引/向量服务本身变化才 reconfigure。
    result.indexReconfigure = setting(QStringLiteral("memory.retrieval.index_enabled"))
        || setting(QStringLiteral("memory.retrieval.auto_backfill"))
        || setting(QStringLiteral("memory.retrieval.embedding_enabled"))
        || setting(QStringLiteral("memory.retrieval.embedding_provider"))
        || setting(QStringLiteral("memory.retrieval.embedding_endpoint"))
        || setting(QStringLiteral("memory.retrieval.embedding_api_key_env"))
        || setting(QStringLiteral("memory.retrieval.embedding_model"))
        || setting(QStringLiteral("memory.retrieval.embedding_dimension"))
        || setting(QStringLiteral("api.dashscope_api_key"))
        || setting(QStringLiteral("api.dashscope_base_url"));
    result.ttsReload = setting(QStringLiteral("tts"));
    result.timeSettings = setting(QStringLiteral("time"));
    result.permissions = result.masterAccounts
        || identity(QStringLiteral("memory.blind_zones"))
        || setting(QStringLiteral("memory.owner_cross_channel_private"))
        || identity(QStringLiteral("proactive.enabled"))
        || identity(QStringLiteral("proactive.private_writing"))
        || identity(QStringLiteral("proactive.targets"))
        || identity(QStringLiteral("cognition.open_wake.allow_contact"))
        || setting(QStringLiteral("memory.long_term_memory_enabled"));
    return result;
}

} // namespace MakoSettings
