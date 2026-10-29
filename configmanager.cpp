#include "configmanager.h"
#include "debuglog.h"
#include "utterance.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonParseError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace {

QList<qlonglong> readIdArray(const QJsonArray &array)
{
    QList<qlonglong> ids;
    for (const QJsonValue &value : array) {
        ids.append(value.isDouble()
                       ? static_cast<qlonglong>(value.toDouble())
                       : value.toString().toLongLong());
    }
    return ids;
}

} // namespace

bool ConfigManager::loadSetting()
{
    QFile file(m_configFilePath);
    if(!file.exists()){
        qDebug()<<"[ConfigManger]配置文件不存在，已初始化";
        // m_apiKey="sk-placeholder-key";
        // m_ttsUrl="http://127.0.0.1:9880";
        return saveSetting();
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug()<<"[ConfigManger]配置文件无法读取，保留原文件";
        return false;
    }

    QByteArray fileData=file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(fileData, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qDebug()<<"[ConfigManager]配置文件 JSON 损坏，保留原文件，不自动覆盖";
        return false;
    }

    QJsonObject rootObj = doc.object();

    // 读取 api 模块下的参数
    if (rootObj.contains("api") && rootObj["api"].isObject()) {
        QJsonObject apiObj = rootObj["api"].toObject();
        m_apiKey = apiObj.value("deepseek_api_key").toString("sk-placeholder-key");
        m_ttsUrl = apiObj.value("gpt_sovits_url").toString("http://127.0.0.1:9880");
    }
    else
    {
        qDebug()<<"[ConfigManger]:api配置缺失，使用默认值";
        m_apiKey = "sk-placeholder-key";
        m_ttsUrl = "http://127.0.0.1:9880";
    }

    m_memoryPolicy = MemoryPolicyConfig{};
    m_debugFullLogs = false;
    m_shortMemoryLength = 15;
    m_summaryMemoryLength = 15;
    if (rootObj.contains("memory") && rootObj["memory"].isObject()) {
        QJsonObject memoryObj = rootObj["memory"].toObject();
        m_shortMemoryLength = memoryObj.value("short_term_length").toInt(15);
        m_summaryMemoryLength = memoryObj.value("summary_length").toInt(15);
        m_debugFullLogs = memoryObj.value("debug_full_logs").toBool(false);

        m_memoryPolicy.shortContextEnabled =
            memoryObj.value("short_context_enabled").toBool(true);
        m_memoryPolicy.longTermMemoryEnabled =
            memoryObj.value("long_term_memory_enabled").toBool(true);
        m_memoryPolicy.interaction.groupIdleMinutes =
            qBound(1, memoryObj.value("qq_group_idle_minutes").toInt(15), 1440);
        m_memoryPolicy.interaction.groupBatchSize =
            qBound(1, memoryObj.value("qq_group_batch_size").toInt(50), 500);
        m_memoryPolicy.interaction.gapSeconds =
            qBound(10, memoryObj.value("interaction_gap_seconds").toInt(90), 3600);
        m_memoryPolicy.interaction.maxEvents =
            qBound(1, memoryObj.value("interaction_max_events").toInt(20), 100);
        m_memoryPolicy.interaction.maxMinutes =
            qBound(1, memoryObj.value("interaction_max_minutes").toInt(10), 120);

        const QJsonObject recentObj =
            memoryObj.value("recent_group_context").toObject();
        m_memoryPolicy.recentContext.enabled =
            recentObj.value("enabled").toBool(true);
        m_memoryPolicy.recentContext.maxAgeMinutes =
            qBound(1, recentObj.value("max_age_minutes").toInt(30), 1440);
        m_memoryPolicy.recentContext.candidateLimitPerGroup =
            qBound(1, recentObj.value("candidate_limit_per_group").toInt(50), 500);
        m_memoryPolicy.recentContext.maxOwnerGroups =
            qBound(1, recentObj.value("max_owner_groups").toInt(2), 20);
        m_memoryPolicy.recentContext.targetedItemLimit =
            qBound(1, recentObj.value("targeted_item_limit").toInt(10), 100);
        m_memoryPolicy.recentContext.targetedCharBudget =
            qBound(100, recentObj.value("targeted_char_budget").toInt(1200), 8000);
        m_memoryPolicy.recentContext.overviewItemLimit =
            qBound(1, recentObj.value("overview_item_limit").toInt(50), 500);
        m_memoryPolicy.recentContext.overviewCharBudget =
            qBound(100, recentObj.value("overview_char_budget").toInt(2200), 12000);
        m_memoryPolicy.recentContext.hotWindowItems =
            qBound(1, recentObj.value("hot_window_items").toInt(10), 100);
        m_memoryPolicy.recentContext.warmLineMaxChars =
            qBound(8, recentObj.value("warm_line_max_chars").toInt(24), 200);

        m_memoryPolicy.policyVersion =
            memoryObj.value("privacy_policy_version").toString(QStringLiteral("r3"));
        if (m_memoryPolicy.policyVersion.isEmpty()) {
            m_memoryPolicy.policyVersion = QStringLiteral("r3");
        }

        const QJsonArray rules = memoryObj.value("privacy_rules").toArray();
        int index = 0;
        for (const QJsonValue &value : rules) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject ruleObj = value.toObject();
            PrivacyRule rule;
            rule.type = ruleObj.value("type").toString().toLower();
            rule.value = ruleObj.value("value").toString();
            rule.action = ruleObj.value("action").toString(QStringLiteral("redact")).toLower();
            rule.id = ruleObj.value("id").toString(
                QStringLiteral("rule-%1").arg(index));
            ++index;

            if (rule.value.isEmpty()
                || (rule.type != QStringLiteral("exact")
                    && rule.type != QStringLiteral("keyword")
                    && rule.type != QStringLiteral("pattern"))
                || (rule.action != QStringLiteral("redact")
                    && rule.action != QStringLiteral("block"))) {
                qDebug() << "[ConfigManager]忽略无效隐私规则:" << rule.id;
                continue;
            }
            m_memoryPolicy.privacyRules.append(rule);
        }
    }
    else
    {
        qDebug()<<"[ConfigManger]:memory配置缺失，使用默认值";
    }
    if (m_summaryMemoryLength < 1) {
        m_summaryMemoryLength = 15;
    }
    setDebugFullLogsEnabled(m_debugFullLogs);

    // 时间管理（TD-037；缺省值与 TimeManager 构造默认值保持一致）
    m_nightStart = QTime(22, 0);
    m_nightEnd = QTime(7, 0);
    m_blushLasting = 30;
    m_emotionLasting = 15;
    m_distanceLasting = 15;
    if (rootObj.contains("time") && rootObj["time"].isObject()) {
        QJsonObject timeObj = rootObj["time"].toObject();
        const QTime nightStart = QTime::fromString(
            timeObj.value("night_start").toString("22:00"), "HH:mm");
        const QTime nightEnd = QTime::fromString(
            timeObj.value("night_end").toString("07:00"), "HH:mm");
        if (nightStart.isValid()) {
            m_nightStart = nightStart;
        }
        if (nightEnd.isValid()) {
            m_nightEnd = nightEnd;
        }
        m_blushLasting = qBound(1, timeObj.value("blush_lasting").toInt(30), 3600);
        m_emotionLasting = qBound(1, timeObj.value("emotion_lasting").toInt(15), 3600);
        m_distanceLasting = qBound(1, timeObj.value("distance_lasting").toInt(15), 3600);
    }
    // QQ 协议端（OneBot WS）
    m_qqEnabled = false;
    m_qqWsUrl = QStringLiteral("ws://127.0.0.1:3001");
    m_qqAccessToken.clear();
    m_qqReply = QQReplyPolicy{};
    m_qqGroupMemory = QQGroupMemoryConfig{};
    if (rootObj.contains("qq") && rootObj["qq"].isObject()) {
        QJsonObject qqObj = rootObj["qq"].toObject();
        m_qqEnabled = qqObj.value("enabled").toBool(false);
        m_qqWsUrl = qqObj.value("ws_url").toString(QStringLiteral("ws://127.0.0.1:3001"));
        m_qqAccessToken = qqObj.value("access_token").toString();

        if (qqObj.contains("reply") && qqObj.value("reply").isObject()) {
            const QJsonObject replyObj = qqObj.value("reply").toObject();
            m_qqReply.privateMode = replyObj.value("private").toString(m_qqReply.privateMode);
            m_qqReply.groupMode = replyObj.value("group").toString(m_qqReply.groupMode);
            m_qqReply.masterAnyGroup = replyObj.value("master_any_group").toBool(false);
            m_qqReply.privateAllow = readIdArray(replyObj.value("private_allow").toArray());
            m_qqReply.groupAllow = readIdArray(replyObj.value("group_allow").toArray());
        }

        m_qqGroupMemory.enabled = qqObj.value("group_memory_enabled").toBool(false);
        m_qqGroupMemory.groupAllow =
            readIdArray(qqObj.value("group_memory_allow").toArray());
        m_qqGroupMemory.retentionDays =
            qBound(1, qqObj.value("group_retention_days").toInt(30), 3650);
        m_qqGroupMemory.membershipRefreshMinutes =
            qBound(0, qqObj.value("membership_refresh_minutes").toInt(30), 1440);
        m_qqGroupMemory.membershipMaxAgeMinutes =
            qBound(1, qqObj.value("membership_max_age_minutes").toInt(1440), 10080);

        const QJsonObject modes = qqObj.value("group_memory_mode").toObject();
        for (auto it = modes.constBegin(); it != modes.constEnd(); ++it) {
            bool ok = false;
            const QQGroupMemoryMode mode =
                qqGroupMemoryModeFromString(it.value().toString(), &ok);
            if (!ok) {
                qDebug() << "[ConfigManager]忽略无效群记忆模式:" << it.key()
                         << it.value().toString();
                continue;
            }
            m_qqGroupMemory.modeByGroup.insert(it.key(), mode);
        }

        const QJsonObject senderAllow =
            qqObj.value("group_sender_allowlist").toObject();
        for (auto it = senderAllow.constBegin(); it != senderAllow.constEnd(); ++it) {
            if (!it.value().isArray()) {
                continue;
            }
            m_qqGroupMemory.senderAllowByGroup.insert(
                it.key(), readIdArray(it.value().toArray()));
        }
    }

    //tts
    if (rootObj.contains("tts") && rootObj["tts"].isObject()) {
        QJsonObject ttsObj = rootObj["tts"].toObject();
        m_ttsMode = ttsObj.value("mode").toString("api");
        m_ttsRef.audioFilePath = ttsObj.value("ref_audio_path").toString("");
        m_ttsRef.promptText = ttsObj.value("prompt_text").toString("");
        m_ttsRef.promptLanguage = ttsObj.value("prompt_lang").toString("ja");
        m_gptWeightsPath = ttsObj.value("gpt_weights_path").toString("");
        m_sovitsWeightsPath = ttsObj.value("sovits_weights_path").toString("");
        m_gptSovitsRootPath = ttsObj.value("gpt_sovits_root_path").toString("");
        m_gptModelDir = ttsObj.value("gpt_model_dir").toString("");
        m_sovitsModelDir = ttsObj.value("sovits_model_dir").toString("");
         m_pythonPath = ttsObj.value("python_path").toString("");
    }

    qDebug() << "[ConfigManager] 配置成功自本地加载:" << m_configFilePath;
    return true;

}

bool ConfigManager::saveSetting()
{
    QFile file(m_configFilePath);
    if(!file.open(QIODevice::WriteOnly |QIODevice::Text)){
        qDebug()<<"[ConfigManager]无法写入配置文件:"<<m_configFilePath;
        return false;

    }

    QJsonObject apiObj;
    apiObj.insert("deepseek_api_key", m_apiKey);
    apiObj.insert("gpt_sovits_url", m_ttsUrl);

    QJsonObject memoryObj;
    memoryObj.insert("short_term_length", m_shortMemoryLength);
    memoryObj.insert("summary_length", m_summaryMemoryLength);
    memoryObj.insert("short_context_enabled", m_memoryPolicy.shortContextEnabled);
    memoryObj.insert("long_term_memory_enabled", m_memoryPolicy.longTermMemoryEnabled);
    memoryObj.insert("debug_full_logs", m_debugFullLogs);
    memoryObj.insert("qq_group_idle_minutes", m_memoryPolicy.interaction.groupIdleMinutes);
    memoryObj.insert("qq_group_batch_size", m_memoryPolicy.interaction.groupBatchSize);
    memoryObj.insert("interaction_gap_seconds", m_memoryPolicy.interaction.gapSeconds);
    memoryObj.insert("interaction_max_events", m_memoryPolicy.interaction.maxEvents);
    memoryObj.insert("interaction_max_minutes", m_memoryPolicy.interaction.maxMinutes);
    QJsonObject recentContextObj;
    recentContextObj.insert("enabled", m_memoryPolicy.recentContext.enabled);
    recentContextObj.insert("max_age_minutes", m_memoryPolicy.recentContext.maxAgeMinutes);
    recentContextObj.insert("candidate_limit_per_group",
                            m_memoryPolicy.recentContext.candidateLimitPerGroup);
    recentContextObj.insert("max_owner_groups", m_memoryPolicy.recentContext.maxOwnerGroups);
    recentContextObj.insert("targeted_item_limit",
                            m_memoryPolicy.recentContext.targetedItemLimit);
    recentContextObj.insert("targeted_char_budget",
                            m_memoryPolicy.recentContext.targetedCharBudget);
    recentContextObj.insert("overview_item_limit",
                            m_memoryPolicy.recentContext.overviewItemLimit);
    recentContextObj.insert("overview_char_budget",
                            m_memoryPolicy.recentContext.overviewCharBudget);
    recentContextObj.insert("hot_window_items",
                            m_memoryPolicy.recentContext.hotWindowItems);
    recentContextObj.insert("warm_line_max_chars",
                            m_memoryPolicy.recentContext.warmLineMaxChars);
    memoryObj.insert("recent_group_context", recentContextObj);
    memoryObj.insert("privacy_policy_version", m_memoryPolicy.policyVersion);
    QJsonArray privacyRules;
    for (const PrivacyRule &rule : m_memoryPolicy.privacyRules) {
        QJsonObject ruleObj;
        ruleObj.insert("id", rule.id);
        ruleObj.insert("type", rule.type);
        ruleObj.insert("value", rule.value);
        ruleObj.insert("action", rule.action);
        privacyRules.append(ruleObj);
    }
    memoryObj.insert("privacy_rules", privacyRules);

    QJsonObject timeObj;
    timeObj.insert("night_start", m_nightStart.toString("HH:mm"));
    timeObj.insert("night_end", m_nightEnd.toString("HH:mm"));
    timeObj.insert("blush_lasting", m_blushLasting);
    timeObj.insert("emotion_lasting", m_emotionLasting);
    timeObj.insert("distance_lasting", m_distanceLasting);

    QJsonObject qqObj;
    qqObj.insert("enabled", m_qqEnabled);
    qqObj.insert("ws_url", m_qqWsUrl);
    qqObj.insert("access_token", m_qqAccessToken);

    QJsonObject replyObj;
    replyObj.insert("private", m_qqReply.privateMode);
    replyObj.insert("group", m_qqReply.groupMode);
    replyObj.insert("master_any_group", m_qqReply.masterAnyGroup);
    QJsonArray privateAllow;
    for (qlonglong id : m_qqReply.privateAllow) {
        privateAllow.append(static_cast<double>(id));
    }
    QJsonArray groupAllow;
    for (qlonglong id : m_qqReply.groupAllow) {
        groupAllow.append(static_cast<double>(id));
    }
    replyObj.insert("private_allow", privateAllow);
    replyObj.insert("group_allow", groupAllow);
    qqObj.insert("reply", replyObj);

    qqObj.insert("group_memory_enabled", m_qqGroupMemory.enabled);
    QJsonArray groupMemoryAllow;
    for (qlonglong id : m_qqGroupMemory.groupAllow) {
        groupMemoryAllow.append(static_cast<double>(id));
    }
    qqObj.insert("group_memory_allow", groupMemoryAllow);
    qqObj.insert("group_retention_days", m_qqGroupMemory.retentionDays);
    qqObj.insert("membership_refresh_minutes",
                 m_qqGroupMemory.membershipRefreshMinutes);
    qqObj.insert("membership_max_age_minutes",
                 m_qqGroupMemory.membershipMaxAgeMinutes);

    QJsonObject groupModes;
    for (auto it = m_qqGroupMemory.modeByGroup.constBegin();
         it != m_qqGroupMemory.modeByGroup.constEnd(); ++it) {
        groupModes.insert(it.key(), qqGroupMemoryModeToString(it.value()));
    }
    qqObj.insert("group_memory_mode", groupModes);

    QJsonObject senderAllowMap;
    for (auto it = m_qqGroupMemory.senderAllowByGroup.constBegin();
         it != m_qqGroupMemory.senderAllowByGroup.constEnd(); ++it) {
        QJsonArray ids;
        for (qlonglong id : it.value()) {
            ids.append(static_cast<double>(id));
        }
        senderAllowMap.insert(it.key(), ids);
    }
    qqObj.insert("group_sender_allowlist", senderAllowMap);

    QJsonObject rootObj;
    rootObj.insert("api", apiObj);
    rootObj.insert("memory", memoryObj);
    rootObj.insert("time", timeObj);
    rootObj.insert("qq", qqObj);

    QJsonObject ttsObj;
    ttsObj.insert("mode", m_ttsMode);
    ttsObj.insert("ref_audio_path", m_ttsRef.audioFilePath);
    ttsObj.insert("prompt_text", m_ttsRef.promptText);
    ttsObj.insert("prompt_lang", m_ttsRef.promptLanguage);
    ttsObj.insert("gpt_weights_path", m_gptWeightsPath);
    ttsObj.insert("sovits_weights_path", m_sovitsWeightsPath);
    ttsObj.insert("gpt_sovits_root_path", m_gptSovitsRootPath);
    ttsObj.insert("gpt_model_dir", m_gptModelDir);
    ttsObj.insert("sovits_model_dir", m_sovitsModelDir);
    ttsObj.insert("python_path", m_pythonPath);
    rootObj.insert("tts", ttsObj);


    QJsonDocument doc(rootObj);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();



    qDebug()<<"[ConfigManger]配置已保存:"<<m_configFilePath;
    return true;
}

void ConfigManager::initAppDataPath()
{
    QDir appDir(QCoreApplication::applicationDirPath());
    m_configDirPath=appDir.filePath("app_data/config");
    m_configFilePath=QDir(m_configDirPath).filePath("setting.json");

    if(!QDir().mkpath(m_configDirPath)){
        qDebug() << "[ConfigManager] 无法创建配置目录:" << m_configDirPath;
    }
    else{
        qDebug()<<"[ConfigManager]目录已存在，无需重复创建:"<<m_configDirPath;
    }
}

QString ConfigManager::getGPTModelDir() const
{
    return m_gptModelDir;
}

void ConfigManager::setGPTModelDir(const QString &path)
{
    m_gptModelDir=path;
}

QString ConfigManager::getSoVITSModelDir() const
{
    return m_sovitsModelDir;
}

void ConfigManager::setSoVITSModelDir(const QString &path)
{
    m_sovitsModelDir=path;
}

QString ConfigManager::getApiKey() const
{
    return m_apiKey;
}

void ConfigManager::setApiKey(const QString &apiKey)
{
    m_apiKey=apiKey;
}

QString ConfigManager::getTTSUrl() const
{
    return m_ttsUrl;
}

void ConfigManager::setTTSUrl(const QString &ttsUrl)
{
    m_ttsUrl=ttsUrl;
}

QString ConfigManager::getTTSMode() const
{
    return m_ttsMode;
}

void ConfigManager::setTTSMode(const QString &mode)
{
    m_ttsMode=mode;
}

TTSReference ConfigManager::getTTSReference() const
{
    return m_ttsRef;
}

void ConfigManager::setTTSReference(const TTSReference &ref)
{
    m_ttsRef=ref;
}

QString ConfigManager::getGPTWeightsPath() const
{
    return m_gptWeightsPath;
}

void ConfigManager::setGPTWeightsPath(const QString &path)
{
    m_gptWeightsPath=path;
}

QString ConfigManager::getSoVITSWeightsPath() const
{
    return m_sovitsWeightsPath;
}

void ConfigManager::setSoVITSWeightsPath(const QString &path)
{
    m_sovitsWeightsPath=path;
}

QString ConfigManager::getGPTSovitsRootPath() const
{
    return m_gptSovitsRootPath;
}

QString ConfigManager::getPythonPath() const
{
    return m_pythonPath;
}

void ConfigManager::setPythonPath(const QString &path)
{
    m_pythonPath=path;
}

void ConfigManager::setGPTSovitsRootPath(const QString &path)
{
    m_gptSovitsRootPath=path;
}

int ConfigManager::getShortMemoryLength()
{
    return m_shortMemoryLength;
}

void ConfigManager::setShortMemoryLength(const int length)
{
    m_shortMemoryLength=length;
}

int ConfigManager::getSummaryMemoryLength() const
{
    return m_summaryMemoryLength;
}

void ConfigManager::setSummaryMemoryLength(int length)
{
    m_summaryMemoryLength = qBound(1, length, 999);
}

bool ConfigManager::getDebugFullLogs() const
{
    return m_debugFullLogs;
}

QTime ConfigManager::getNightStartTime() const
{
    return m_nightStart;
}

void ConfigManager::setNightStartTime(const QTime &time)
{
    if (time.isValid()) {
        m_nightStart = time;
    }
}

QTime ConfigManager::getNightEndTime() const
{
    return m_nightEnd;
}

void ConfigManager::setNightEndTime(const QTime &time)
{
    if (time.isValid()) {
        m_nightEnd = time;
    }
}

int ConfigManager::getBlushLasting() const
{
    return m_blushLasting;
}

void ConfigManager::setBlushLasting(int seconds)
{
    m_blushLasting = qBound(1, seconds, 3600);
}

int ConfigManager::getEmotionLasting() const
{
    return m_emotionLasting;
}

void ConfigManager::setEmotionLasting(int seconds)
{
    m_emotionLasting = qBound(1, seconds, 3600);
}

int ConfigManager::getDistanceLasting() const
{
    return m_distanceLasting;
}

void ConfigManager::setDistanceLasting(int seconds)
{
    m_distanceLasting = qBound(1, seconds, 3600);
}

QString ConfigManager::getConfigDirPath() const
{
    return m_configDirPath;
}

QString ConfigManager::getConfigFilePath() const
{
    return m_configFilePath;
}

QString ConfigManager::getAppDataPath() const
{
    QDir appPath(m_configDirPath);
    appPath.cdUp();
    return appPath.absolutePath();
}

QString ConfigManager::getMemoryPath() const
{
    return getAppDataPath()+"/memory/QianDaoMoZi_memory.db";
}

ConfigManager::ConfigManager() {
    initAppDataPath();
    loadSetting();
}

// ===== 多渠道扩展配置（identity.json，Phase 0 只读解析）=====

// 懒加载 identity.json：缺失或损坏时使用默认值并提示（不自动创建，由用户手动配置）
void ConfigManager::ensureIdentityLoaded() const
{
    if (m_identityLoaded) {
        return;
    }
    m_identityLoaded = true;

    const QString identityPath = m_configDirPath + "/identity.json";
    QFile file(identityPath);
    if (!file.exists()) {
        qDebug() << "[ConfigManager]identity.json 不存在，使用默认配置（QQ 渠道接入前请手动创建）:" << identityPath;
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "[ConfigManager]identity.json 打开失败，使用默认配置";
        return;
    }
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError) {
        qDebug() << "[ConfigManager]identity.json 解析失败:" << parseError.errorString();
        return;
    }

    QJsonObject rootObj = doc.object();

    // 身份绑定（按 uin 绑定，与昵称无关）
    QJsonObject identityObj = rootObj.value("identity").toObject();
    for (const QJsonValue &v : identityObj.value("self").toObject().value("qq").toArray()) {
        m_selfQQ.append(v.toString());
    }
    for (const QJsonValue &v : identityObj.value("master").toObject().value("qq").toArray()) {
        m_masterQQ.append(v.toString());
    }

    // 记忆盲区
    for (const QJsonValue &v : rootObj.value("memory").toObject().value("blind_zones").toArray()) {
        QJsonObject z = v.toObject();
        BlindZone zone;
        zone.channel = z.value("channel").toString();
        zone.sessionId = z.value("session_id").toString();
        if (!zone.channel.isEmpty() && !zone.sessionId.isEmpty()) {
            m_blindZones.append(zone);
        }
    }

    // 立绘跟随开关（D1 决策）
    m_followExternalMood = rootObj.value("appearance").toObject().value("follow_external_mood").toBool(true);

    // 主动对话配置（Phase 0 只解析不调度）
    QJsonObject proObj = rootObj.value("proactive").toObject();
    m_proactive.enabled = proObj.value("enabled").toBool(false);
    m_proactive.dailyLimit = proObj.value("daily_limit").toInt(3);
    m_proactive.cooldownSeconds = proObj.value("cooldown_seconds").toInt(900);
    m_proactive.idleSeconds = proObj.value("idle_seconds").toInt(1800);
    QJsonObject quietObj = proObj.value("quiet_hours").toObject();
    m_proactive.quietStart = quietObj.value("start").toString("23:00");
    m_proactive.quietEnd = quietObj.value("end").toString("08:00");
    for (const QJsonValue &v : proObj.value("targets").toArray()) {
        QJsonObject t = v.toObject();
        m_proactive.targets.append(qMakePair(t.value("channel").toString(Channel::kPet),
                                             t.value("session_id").toString(Channel::kDefaultSession)));
    }
    for (const QJsonValue &v : proObj.value("schedule").toArray()) {
        QJsonObject s = v.toObject();
        ScheduleItem item;
        item.at = s.value("at").toString();
        item.type = s.value("type").toString("fixed");
        if (!item.at.isEmpty()) {
            m_proactive.schedule.append(item);
        }
    }

    qDebug() << "[ConfigManager]identity.json 加载完成: self" << m_selfQQ.size()
             << "个 / master" << m_masterQQ.size() << "个 / 盲区" << m_blindZones.size() << "个";
}

QStringList ConfigManager::getSelfQQAccounts()
{
    ensureIdentityLoaded();
    return m_selfQQ;
}

QStringList ConfigManager::getMasterQQAccounts()
{
    ensureIdentityLoaded();
    return m_masterQQ;
}

bool ConfigManager::isMasterQQ(const QString &uin)
{
    ensureIdentityLoaded();
    return m_masterQQ.contains(uin);
}

bool ConfigManager::isBlindZone(const QString &channel, const QString &sessionId)
{
    ensureIdentityLoaded();
    for (const BlindZone &z : m_blindZones) {
        if (z.channel == channel && z.sessionId == sessionId) {
            return true;
        }
    }
    return false;
}

bool ConfigManager::getFollowExternalMood()
{
    ensureIdentityLoaded();
    return m_followExternalMood;
}

ProactiveConfig ConfigManager::getProactiveConfig()
{
    ensureIdentityLoaded();
    return m_proactive;
}

QQConfig ConfigManager::getQQConfig() const
{
    QQConfig config;
    config.enabled = m_qqEnabled;
    config.wsUrl = m_qqWsUrl;
    config.accessToken = m_qqAccessToken;
    config.reply = m_qqReply;
    config.groupMemory = m_qqGroupMemory;
    return config;
}

MemoryPolicyConfig ConfigManager::getMemoryPolicyConfig() const
{
    return m_memoryPolicy;
}

QQGroupMemoryConfig ConfigManager::getQQGroupMemoryConfig() const
{
    return m_qqGroupMemory;
}

void ConfigManager::reloadIdentity()
{
    m_identityLoaded = false;
    m_selfQQ.clear();
    m_masterQQ.clear();
    m_blindZones.clear();
    m_followExternalMood = true;
    m_proactive = ProactiveConfig();
    ensureIdentityLoaded();
}

void ConfigManager::loadIdentityConfig()
{
    ensureIdentityLoaded();
}
