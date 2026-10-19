#include "configmanager.h"
#include "utterance.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonParseError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

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
        qDebug()<<"[ConfigManger]配置文件损坏，已初始化";
        return saveSetting();
    }

    QByteArray fileData=file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(fileData, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qDebug()<<"[ConfigManager]配置文件损坏，已初始化";
        return saveSetting();
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

    if (rootObj.contains("memory") && rootObj["memory"].isObject()) {
        QJsonObject memoryObj = rootObj["memory"].toObject();
        m_shortMemoryLength = memoryObj.value("short_term_length").toInt(15);
    }
    else
    {
        qDebug()<<"[ConfigManger]:memory配置缺失，使用默认值";
        m_shortMemoryLength = 15;
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

    QJsonObject rootObj;
    rootObj.insert("api", apiObj);
    rootObj.insert("memory", memoryObj);

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
