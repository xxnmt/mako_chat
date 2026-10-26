#include "llmservice.h"
#include "configmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QPair>
#include "memorymanager.h"


LLMService::LLMService(const QString &apiKey,QObject *parent)
{
    m_networkManager = new QNetworkAccessManager(this);
    connect(m_networkManager, &QNetworkAccessManager::finished, this, &LLMService::onReplyFinished);
    m_extractManager=new QNetworkAccessManager(this);

    qRegisterMetaType<QList<SentenceText>>("QList<SentenceText>");
    m_apiKey=apiKey;

    QString configDirPath=ConfigManager::instance().getConfigDirPath();
    m_localPromptPath=QDir(configDirPath).filePath("prompt.txt");

    initializePromptFile();
    m_systemPromptCache=loadSystemPrompt();
}


void LLMService::askDeepSeek(const QString &userInput, const QList<HistoryTurn> &historyQA, const RequestMeta &meta)
{
    m_currentMeta = meta;   // 本轮元数据（渠道/会话/主体），供 prompt 组装与记忆注入使用

    QUrl url("https://api.deepseek.com/chat/completions");
    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    //define YOUR_API_KEY into AI api key
    QString aotuHeader="Bearer "+m_apiKey;
    request.setRawHeader("Authorization",aotuHeader.toUtf8());
    m_systemPromptCache=loadSystemPrompt();

    //prompt
    QString finalSystemPrompt = m_systemPromptCache;

    // QJsonArray messagesArray;
    // QJsonObject systemMessage;
    // systemMessage["role"] = "system";
    // systemMessage["content"] = finalSystemPrompt;
    // messagesArray.append(systemMessage);
    // 角色档案 + 情景记忆 + 摘要（v4 注入格式，Markdown）
    if (m_memoryManager) {
        // 1. 角色档案（用户 + 角色）
        QList<CharacterProfile> userProfiles = m_memoryManager->getCharacterProfiles("user");
        QList<CharacterProfile> makoProfiles = m_memoryManager->getCharacterProfiles("mako");
        qDebug()<<"[LLM]角色档案: user="<<userProfiles.size()<<"mako="<<makoProfiles.size();
        if (!makoProfiles.isEmpty()) {
            finalSystemPrompt += "\n\n# 茉子的人设\n";
            for (const auto &p : makoProfiles) {
                finalSystemPrompt += QString("- %1: %2\n").arg(p.key).arg(p.value);
            }
        }
        if (!userProfiles.isEmpty()) {
            finalSystemPrompt += "\n# 关于欧尼酱\n";
            for (const auto &p : userProfiles) {
                finalSystemPrompt += QString("- %1: %2\n").arg(p.key).arg(p.value);
            }
        }

        // 2. 情景记忆
        QList<EpisodicMemory> memories = m_memoryManager->getActiveEpisodicMemories(0.2, 20);
        qDebug()<<"[LLM]情景记忆数量:"<<memories.size();
        if (!memories.isEmpty()) {
            finalSystemPrompt += "\n# 我们的回忆\n";
            for (const auto &m : memories) {
                QString typeLabel;
                if (m.type == "promise") typeLabel = QString("（承诺·%1）").arg(m.status);
                else if (m.type == "conflict") typeLabel = "（冲突）";
                else if (m.type == "milestone") typeLabel = "（里程碑）";
                else typeLabel = "";
                finalSystemPrompt += QString("- [%1] %2%3\n")
                                         .arg(m.eventTime.toString("yyyy-MM-dd"))
                                         .arg(m.content)
                                         .arg(typeLabel);
            }
        }

        // 3. 长期摘要（保留兼容，按渠道分桶取）
        QList<LongTermSummary> summaries = m_memoryManager->getLatestSummaries(5, m_currentMeta.channel, m_currentMeta.sessionId);
        qDebug()<<"[LLM]摘要数量:"<<summaries.size();
        if (!summaries.isEmpty()) {
            finalSystemPrompt += "\n# 最近发生\n";
            for (const auto &s : summaries) {
                finalSystemPrompt += QString("- %1\n").arg(s.summaryText);
            }
        }

        // 4. 关系状态（量化 user 与 AI 之间的关系维度，按当前对话主体取）
        QList<RelationshipState> relStates = m_memoryManager->getRelationshipStates(m_currentMeta.subject);
        if (!relStates.isEmpty()) {
            finalSystemPrompt += "\n# 我们的关系\n";
            for (const auto &r : relStates) {
                QString label;
                if (r.dimension == "intimacy") label = "亲密度";
                else if (r.dimension == "trust") label = "信任度";
                else label = r.dimension;
                finalSystemPrompt += QString("- %1: %2/100\n").arg(label).arg(r.value, 0, 'f', 0);
            }
        }
    }

    QJsonArray messagesArray;
    QJsonObject systemMessage;
    systemMessage["role"] = "system";
    systemMessage["content"] = finalSystemPrompt;
    messagesArray.append(systemMessage);

    //短期记忆
    if (!historyQA.isEmpty()) {
        QJsonObject historyNote;
        historyNote["role"] = "system";
        historyNote["content"] = "以下内容为短期对话记录。！！！仅提供对话信息，请勿参考其输出格式！！！";
        messagesArray.append(historyNote);
    }
    for (const HistoryTurn &turn : historyQA) {
        // v3 轮次语义：proactive/perception（或没有用户输入）只注入 assistant，
        // 不生成空的 user 消息（HUMANIZATION_PLAN 10.4.2）
        const bool assistantOnly = turn.turnKind == QStringLiteral("proactive")
                                   || turn.turnKind == QStringLiteral("perception")
                                   || turn.userInput.isEmpty();
        if (!assistantOnly) {
            QJsonObject histUserMsg;
            histUserMsg["role"] = "user";
            histUserMsg["name"] = QString("用户 [%1]").arg(turn.timestamp.toString("yyyy-MM-dd HH:mm:ss"));
            histUserMsg["content"] = turn.userInput;
            messagesArray.append(histUserMsg);
        }

        // 历史茉子回复
        QJsonObject histMakoMsg;
        histMakoMsg["role"] = "assistant";
        const QString makoName = assistantOnly ? QStringLiteral("茉子（主动/感知）")
                                               : QStringLiteral("茉子");
        histMakoMsg["name"] = QString("%1 [%2]").arg(makoName, turn.timestamp.toString("yyyy-MM-dd HH:mm:ss"));
        // 从 JSON 提取中文文本作为历史回复
        QString aiContent;
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(turn.rawReply.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            QStringList zhTexts;
            QJsonArray sentences = doc.object()["sentences"].toArray();
            for (const QJsonValue &sv : sentences) {
                zhTexts.append(sv.toObject()["zh_text"].toString());
            }
            aiContent = zhTexts.join("");
        }
        else{
            QString text = turn.rawReply;
            text = text.remove(QRegularExpression("\\[[^\\]]+\\]"));
            aiContent = text.trimmed();
        }
        histMakoMsg["content"] = aiContent;  // ← 使用自然语言文本
        messagesArray.append(histMakoMsg);
    }

    if (m_stateProvider) {
        QString currentUiState = m_stateProvider();
        QJsonObject envMsg;
        envMsg["role"] = "system";
        envMsg["content"] = QString(
                                "[当前环境信息]\n"
                                "茉子当前状态：\n%1\n"
                                "现在的时间是 %2\n"
                                "请根据时间流逝合理感知。"
                                ).arg(currentUiState,
                                     QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        messagesArray.append(envMsg);
    }
    QJsonObject userMessage;
    userMessage["role"] = "user";
    QString enhancedInput = userInput + "\n\n【输出要求】请直接输出符合约定格式的JSON，不要输出任何其他文本、解释或思考内容。";
    userMessage["content"] = enhancedInput;
    messagesArray.append(userMessage);
    qDebug().noquote()<<"[LLM]最终注入的Message:"<<QJsonDocument(messagesArray)
                                                           .toJson(QJsonDocument::Indented);

    QJsonObject rootObj;
    rootObj["model"] = "deepseek-v4-flash";
    rootObj["messages"] = messagesArray;
    rootObj["temperature"] = 0.7;
    rootObj["max_tokens"] = 8192;
    rootObj["response_format"] = QJsonObject{{"type", "json_object"}};

    // QJsonObject thinkingObj;
    // thinkingObj["type"] = "disabled";
    rootObj["reasoning_effort"] = "low";
    // rootObj["thinking"] = thinkingObj;

    QByteArray postData = QJsonDocument(rootObj).toJson();
    m_networkManager->post(request, postData);

    qDebug() << "[LLM]茉子带着"<<historyQA.size()<<"轮短期记忆正在思考......";
}

void LLMService::registerStateProvider(std::function<QString ()> provider)
{
    m_stateProvider=provider;
}

void LLMService::setMemoryManager(MemoryManager *manager)
{
    m_memoryManager=manager;
}

void LLMService::extractMemoryAsync(const QList<HistoryTurn> &turns, qlonglong lastEndId, const QList<qlonglong> &sourceIds,
                                    const QList<CharacterProfile> &existingProfiles,
                                    const QList<EpisodicMemory> &existingMemories)
{
    if (turns.isEmpty()) {
        qDebug()<<"[LLM]待摘要turns为空";
        return;
    }
    QUrl url("https://api.deepseek.com/chat/completions");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());

    // 构建已有角色档案上下文
    QString existingProfilesText;
    if (!existingProfiles.isEmpty()) {
        for (const CharacterProfile &p : existingProfiles) {
            existingProfilesText += QString("- %1: %2\n").arg(p.key).arg(p.value);
        }
    } else {
        existingProfilesText = "（暂无）\n";
    }

    // 构建已有情景记忆上下文
    QString existingMemoriesText;
    if (!existingMemories.isEmpty()) {
        for (const EpisodicMemory &m : existingMemories) {
            existingMemoriesText += QString("- [%1] %2 (importance=%3, type=%4)\n")
                                        .arg(m.eventTime.toString("yyyy-MM-dd"))
                                        .arg(m.content)
                                        .arg(m.importance)
                                        .arg(m.type);
        }
    } else {
        existingMemoriesText = "（暂无）\n";
    }

    // 构建当前关系状态上下文
    QString existingRelationshipText;
    if (m_memoryManager) {
        QList<RelationshipState> relStates = m_memoryManager->getRelationshipStates(m_currentMeta.subject);
        if (!relStates.isEmpty()) {
            for (const RelationshipState &r : relStates) {
                QString label;
                if (r.dimension == "intimacy") label = "亲密度";
                else if (r.dimension == "trust") label = "信任度";
                else label = r.dimension;
                existingRelationshipText += QString("- %1: %2/100\n").arg(label).arg(r.value, 0, 'f', 0);
            }
        } else {
            existingRelationshipText = "（暂无）\n";
        }
    } else {
        existingRelationshipText = "（暂无）\n";
    }

    QString extractionPrompt = QString(R"(# 任务
你是一个陪伴类AI的记忆分析与总结专家。请从以下对话中提取五类信息：

## 1. character_updates（角色档案更新）
长期稳定的事实，跨月仍然成立。
- subject: "user" 或 "mako"
- key:
  - 优先复用"已有角色档案"中出现的 key（避免信息碎片化）
  - 若语义确实独立于所有已有 key，可新建维度；新 key 必须是长期稳定维度（非临时状态），且语义不与已有 key 重叠
  - 推荐 key 参考（非强制，可按需扩展）：
    - user 主题: 称呼 / 职业 / 性格 / 基本性格 / 爱好 / 与AI关系 / 作息 / 家庭背景 / 教育经历
    - mako 主题: 人设 / 性格 / 与用户关系
  - 维度区分说明（避免误并）：
    - 性格: 当前的心理与行为状态（可随互动演变）
    - 基本性格: 长期稳定的人格特质（跨月不变）
- value: 具体内容

## 2. episodic_memories（情景记忆）
带时间戳的共同事件。
- type: "event"(普通事件) / "promise"(承诺) / "conflict"(冲突) / "milestone"(里程碑)
- importance: 0.0-1.0（里程碑和承诺建议 0.8+）
- content: 事件描述
- event_time: 事件时间（格式 yyyy-MM-dd HH:mm:ss，若不确定可省略）

## 3. working_summary（本轮摘要）
一句话概括这段对话（50字以内）。

## 4. relationship_updates（关系状态更新）
根据对话判断 user 与 AI 之间的关系状态变化。
- dimension: "intimacy"(亲密度) 或 "trust"(信任度)
- delta: 变化量，范围 -5.0 到 +5.0（可为小数）
  - 正数表示增进：如关心、陪伴、承诺兑现、情感表达、共同经历
  - 负数表示疏远：如冷漠、失信、冲突、长时间分离、情感伤害
  - 0 或无明显变化时，该维度可不输出
- 判断依据：
  - intimacy（亲密度）：情感交流深度、肢体接触描述、亲密用语频率、共同活动意愿
  - trust（信任度）：承诺兑现、坦诚分享、依赖行为、可靠性行为

# 已有角色档案（在此基础上增量更新，未涉及的不输出）
%1
# 已有情景记忆（避免重复，已兑现的承诺请提示状态更新）
%2
# 当前关系状态（在此基础上叠加 delta）
%3
# 提取规则
- character_updates 只提取"跨月仍然成立"的稳定事实，临时状态不要放入
- key 优先复用已有档案的 key；若语义独立于所有已有 key，可新建维度
- 禁止为同类信息创造近义词（如已有"性格"则不要产出"个性"/"性格特征"）
- 注意"性格"与"基本性格"是两个独立维度，可共存：前者是当前心理状态，后者是稳定人格特质
- 若对话揭示了已有档案的新信息，输出更新后的 value
- 矛盾信息以对话中的新信息为准
- 不要简单复制已有档案，只输出有更新或有新增的
- episodic_memories 的 content 中不要包含"长期/短期"等层级标注
- relationship_updates 的 delta 要基于本轮对话的实际变化，避免无变化时输出 0

# 输出格式
严格输出合法 JSON，不要包含 markdown 代码块包裹标记（如 ```json），格式如下：
{
  "character_updates": [
    {"subject": "user", "key": "occupation", "value": "AI开发者"},
    {"subject": "mako", "key": "persona", "value": "温柔体贴的茉子"}
  ],
  "episodic_memories": [
    {"content": "答应下周完成TTS修复", "type": "promise", "importance": 0.9, "event_time": "2026-08-05 14:00:00"},
    {"content": "第一次讨论画像重构方案", "type": "event", "importance": 0.6}
  ],
  "working_summary": "用户与茉子讨论了记忆系统的重构方案",
  "relationship_updates": [
    {"dimension": "intimacy", "delta": 2.0},
    {"dimension": "trust", "delta": 1.5}
  ]
})").arg(existingProfilesText).arg(existingMemoriesText).arg(existingRelationshipText);

    QString conversationText;
    for (const HistoryTurn &turn : turns) {
        QString aiText = turn.rawReply;
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(turn.rawReply.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            QStringList zhTexts;
            QJsonArray sentences = doc.object()["sentences"].toArray();
            for (const QJsonValue &sv : sentences) {
                zhTexts.append(sv.toObject()["zh_text"].toString());
            }
            aiText = zhTexts.join("");
        }
        if (turn.turnKind == QStringLiteral("proactive")) {
            conversationText += QString("茉子（主动）: %1\n---\n").arg(aiText);
        } else if (turn.turnKind == QStringLiteral("perception")) {
            conversationText += QString("茉子（因观察到某事而开口）: %1\n---\n").arg(aiText);
        } else {
            conversationText += QString("用户: %1\nAI: %2\n---\n")
                                    .arg(turn.userInput, aiText);
        }
    }

    QJsonArray messagesArray;
    QJsonObject sysMsg; sysMsg["role"] = "system"; sysMsg["content"] = extractionPrompt;
    QJsonObject userMsg; userMsg["role"] = "user"; userMsg["content"] = conversationText;
    messagesArray.append(sysMsg);
    messagesArray.append(userMsg);

    QJsonObject rootObj;
    rootObj["model"] = "deepseek-v4-flash";
    rootObj["messages"] = messagesArray;
    rootObj["temperature"] = 0.3;
    rootObj["reasoning_effort"] = "low";
    rootObj["response_format"] = QJsonObject{{"type", "json_object"}};

    QByteArray postData = QJsonDocument(rootObj).toJson();

    QJsonArray sourceJsonArray;
    for (qlonglong id : sourceIds) sourceJsonArray.append(id);
    QString sourceIdsJson = QJsonDocument(sourceJsonArray).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_extractManager->post(request, postData);

    QPointer<LLMService> self(this);

    connect(reply, &QNetworkReply::finished, this, [this,self, reply, lastEndId, sourceIdsJson]() {
        if(self.isNull()) {
            reply->deleteLater();
            qDebug()<<"[LLM]摘要捕获异常";
            return;
        }
        if (reply->error() == QNetworkReply::NoError) {
            QByteArray responseData = reply->readAll();
            QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
            QString replyText = jsonDoc.object()["choices"].toArray()[0].toObject()["message"].toObject()["content"].toString();

            // 容错：清除 markdown 代码块标记
            replyText.replace(QRegularExpression("```json|```", QRegularExpression::CaseInsensitiveOption), "");

            QJsonDocument resultDoc = QJsonDocument::fromJson(replyText.toUtf8());
            if (resultDoc.isObject()) {
                QJsonObject result = resultDoc.object();
                qDebug()<<"[LLM]记忆提取成功:" << result;
                emit memoryExtractionReady(result, lastEndId, sourceIdsJson);
            } else {
                qDebug()<<"[LLM]记忆提取返回的JSON格式解析失败:"<<replyText;
            }
        } else {
            qDebug()<<"[LLM]记忆提取网络错误:"<<reply->errorString();
        }
        reply->deleteLater();
    });
}

void LLMService::setApiKey(const QString &apiKey)
{
    m_apiKey=apiKey;
}


void LLMService::onReplyFinished(QNetworkReply *reply)
{
    if(reply->error()==QNetworkReply::NoError){
        QByteArray responseData = reply->readAll();

        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
        QJsonObject rootObj = jsonDoc.object();
        QJsonArray choices = rootObj["choices"].toArray();
//------------
        if(!choices.isEmpty()){
            QJsonObject messageObj = choices[0].toObject()["message"].toObject();
            QString replyText = messageObj["content"].toString();
            if (replyText.trimmed().isEmpty()) {
                QString reasoning = messageObj["reasoning_content"].toString();
                qDebug() << "[LLM]content为空！reasoning_content长度:" << reasoning.length();
                if (!reasoning.isEmpty()) {
                    qDebug() << "[LLM]reasoning_content前200字:" << reasoning.left(200);
                }
                qDebug() << "[LLM]完整message字段:" << messageObj;
            }
//---------------
            qDebug()<<"[LLM]茉子回复(未处理):"<<replyText;

            QPair<QList<SentenceText>, QString> parseResult=parseJsonReply(replyText);
            QList<SentenceText> parsedSentences = parseResult.first;
            QString correctedReply = parseResult.second;

            // 异常回复拦截（空/解析失败/缺中文/缺日文/缺标签）
            // parseJsonReply 的所有异常路径统一返回空 sentences，以此作为唯一判据
            if (parsedSentences.isEmpty()) {
                qDebug()<<"[LLM]回复内容异常，抛弃本次回复（不入库，不走TTS）:"<<replyText;
                emit replyAbnormal(replyText);
            }
            else {
                qDebug()<<"[LLM]成功拆分为"<<parsedSentences.size()<<"个段落：";
                for (int i=0;i<parsedSentences.size();i++) {
                    const auto &s = parsedSentences[i];
                    qDebug()<<QString("--------- [第 %1 句] ---------").arg(i + 1);
                    qDebug()<<"[LLM]中文气泡:"<<s.zhText;
                    qDebug()<<"[LLM]日文音频目标:"<<s.jaText;
                    qDebug()<<"[LLM]四维状态变更:"<<s.rawTags;
                }
                //拆完传信号
                emit sentencesReady(parsedSentences,correctedReply);
            }
        }
        else {
            // HTTP 200 但 choices 为空，视为异常回复（原本静默无反馈）
            qDebug()<<"[LLM]choices为空，回复内容异常";
            emit replyAbnormal(QStringLiteral("choices为空"));
        }
    }
    else {
        qDebug()<<"[LLM]网络错误:"<<reply->errorString();
        emit internetErrorSignal(reply->errorString());
    }
    reply->deleteLater();
}

void LLMService::initializePromptFile()
{
    QFile localFile(m_localPromptPath);
    if(!localFile.exists()){
        qDebug()<<"[LLM]:prompt寻找失败，自动创建默认prompt";
        if (QFile::copy(":/image/default_config/prompt.txt", m_localPromptPath)) {
            QFile::setPermissions(m_localPromptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            qDebug()<<"[LLM]默认prompt成功释放至:"<<m_localPromptPath;
        }
        else{
            qDebug()<<"[LLM]默认prompt释放失败";
        }
    }
}

QString LLMService::loadSystemPrompt()
{
    QFile file(m_localPromptPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug()<<"[LLM]无法读取本地prompt，使用内置默认prompt:"<<m_localPromptPath;
        file.setFileName(":/prompts/system_prompt.txt");
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    QString content = in.readAll();
    return content;
}

QPair<QList<SentenceText>, QString> LLMService::parseJsonReply(const QString &replyText)
{

    QList<SentenceText> result;
    QString currentReply=replyText;

    // 容错处理：清除 Markdown 代码块标记
    QString cleanText = replyText;
    cleanText.replace(QRegularExpression("```json|```", QRegularExpression::CaseInsensitiveOption), "");
    cleanText = cleanText.trimmed();
    if (cleanText.isEmpty() || cleanText == "{}") {
        qDebug()<<"[LLM]收到空白或空JSON回复，丢弃";
        return {};
    }
    // 尝试直接解析
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(cleanText.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qDebug() << "[LLM] JSON 解析失败:" << parseError.errorString();

        // 尝试提取 JSON 对象（容忍前后有多余文本）
        QRegularExpression jsonRegex("\\{[\\s\\S]*\\}");
        QRegularExpressionMatch match = jsonRegex.match(cleanText);
        if (match.hasMatch()) {
            doc = QJsonDocument::fromJson(match.captured(0).toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                qDebug() << "[LLM] JSON 提取后仍无法解析";
                return qMakePair(result,currentReply);
            }
            qDebug() << "[LLM] JSON 提取成功";
        } else {
            qDebug() << "[LLM] 未找到 JSON 对象";
            return qMakePair(result,currentReply);
        }
    }

    if (!doc.isObject()) {
        qDebug() << "[LLM] JSON 根节点不是对象";
        return qMakePair(result,currentReply);
    }

    QJsonObject rootObj = doc.object();
    QJsonArray sentencesArray = rootObj.value("sentences").toArray();

    if (sentencesArray.isEmpty()) {
        qDebug() << "[LLM] sentences 数组为空";
        return qMakePair(result,currentReply);
    }

    qDebug() << "[LLM] JSON 解析成功，共" << sentencesArray.size() << "个句子";

    // SentenceText prevSentence;

    for (int i = 0; i < sentencesArray.size(); ++i) {
        QJsonObject sentenceObj = sentencesArray[i].toObject();

        // 原始内容合法性校验（必须在 TagValidator 默认值兜底之前，否则检测不出"没有标签"）
        // 任一句异常 → 整轮抛弃
        QString invalidReason;
        if (!isSentenceValid(sentenceObj, invalidReason)) {
            qDebug()<<"[LLM]第"<<(i+1)<<"句内容异常("<<invalidReason<<")，整轮抛弃";
            return {};
        }

        SentenceText sentence;
        sentence.zhText = sentenceObj.value("zh_text").toString().trimmed();
        sentence.jaText = sentenceObj.value("ja_text").toString().trimmed();
        // sentence.originalJSON = QJsonDocument(sentenceObj).toJson(QJsonDocument::Compact);

        // 解析 tags
        QJsonObject tagsObj = sentenceObj.value("tags").toObject();
        QString prevEmotion = (i > 0) ? sentencesArray[i-1].toObject()["tags"].toObject()["emotion"].toString() : "";
        QString prevBlush = (i > 0) ? sentencesArray[i-1].toObject()["tags"].toObject()["blush"].toString() : "";
        QString prevDistance = (i > 0) ? sentencesArray[i-1].toObject()["tags"].toObject()["distance"].toString() : "";
        QString prevClothing = (i > 0) ? sentencesArray[i-1].toObject()["tags"].toObject()["clothing"].toString() : "";

        tagsObj["emotion"] = TagValidator::validateTag(
            "emotion", tagsObj["emotion"].toString(), TagValidator::VALID_EMOTIONS,
            sentence.zhText, prevEmotion
            );
        tagsObj["blush"] = TagValidator::validateTag(
            "blush", tagsObj["blush"].toString(), TagValidator::VALID_BLUSH,
            "", prevBlush
            );
        tagsObj["distance"] = TagValidator::validateTag(
            "distance", tagsObj["distance"].toString(), TagValidator::VALID_DISTANCE,
            "", prevDistance
            );
        tagsObj["clothing"] = TagValidator::validateTag(
            "clothing", tagsObj["clothing"].toString(), TagValidator::VALID_CLOTHING,
            "", prevClothing
            );
        // 更新修正后的 tags
        sentenceObj["tags"] = tagsObj;
        sentencesArray[i] = sentenceObj;

        // 构建 SentenceText
        for (auto it = tagsObj.constBegin(); it != tagsObj.constEnd(); ++it) {
            sentence.rawTags.insert(it.key(), it.value().toString());
        }
        sentence.isValidated = true;
        result.append(sentence);
    }

    // 更新根对象（修正后的 JSON）
    rootObj["sentences"] = sentencesArray;

    // 保存修正后的 JSON（用于存储和历史记忆）
    currentReply = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);
    qDebug() << "[LLM] 修正后回复:" << currentReply;

    return qMakePair(result,currentReply);
}
//回复内容合法性校验
//是否包含中文（CJK统一表意文字基本区 + 扩展A）
bool LLMService::containsHan(const QString &text)
{
    for (const QChar &ch : text) {
        char32_t c = ch.unicode();
        if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) {
            return true;
        }
    }
    return false;
}

//是否包含日文假名（平假名 + 片假名）
bool LLMService::containsKana(const QString &text)
{
    for (const QChar &ch : text) {
        char32_t c = ch.unicode();
        if ((c >= 0x3040 && c <= 0x309F) || (c >= 0x30A0 && c <= 0x30FF)) {
            return true;
        }
    }
    return false;
}

//单句原始内容校验（对 TagValidator 修正前的原始 JSON 执行），任一项异常则整轮回复抛弃
bool LLMService::isSentenceValid(const QJsonObject &sentenceObj, QString &reason)
{
    //1. 中文文本：非空且至少含一个汉字
    QString zhText = sentenceObj.value("zh_text").toString().trimmed();
    if (zhText.isEmpty() || !containsHan(zhText)) {
        reason = "zh_text缺失或不含中文";
        return false;
    }
    //2. 日文文本：非空且至少含一个假名（茉子口语风格必然含假名，纯汉字视为输出错乱）
    QString jaText = sentenceObj.value("ja_text").toString().trimmed();
    if (jaText.isEmpty() || !containsKana(jaText)) {
        reason = "ja_text缺失或不含日文假名";
        return false;
    }
    //3. 标签：tags 对象存在且非空（个别标签缺失仍由 TagValidator 默认值兜底）
    QJsonObject tagsObj = sentenceObj.value("tags").toObject();
    if (tagsObj.isEmpty()) {
        reason = "tags缺失或为空";
        return false;
    }
    return true;
}

//Tags校验
int TagValidator::levenshteinDistance(const QString &s1, const QString &s2)
{
    int n=s1.length();
    int m=s2.length();
    if(m==0)return n;
    if(n==0)return m;

    QVector<QVector<int>>dp(n+1,QVector<int>(m+1));

    for (int i = 0; i <= n; i++) dp[i][0] = i;
    for (int j = 0; j <= m; j++) dp[0][j] = j;

    for(int i=1;i<=n;i++){
        for(int j =1;j<=m;j++) {
            int cost=(s1[i-1]==s2[j-1]?0:1);
            dp[i][j]=qMin(qMin(dp[i-1][j]+1,dp[i][j-1]+1),dp[i-1][j-1]+cost);
        }
    }
    return dp[n][m];
}

QString TagValidator::validateTag(
    const QString &tagName,
    const QString &rawValue,
    const QStringList &validValues,
    const QString &contextText,
    const QString &prevValue)
{
    QString value=rawValue.trimmed();
    //对
    if(validValues.contains(value)){
        qDebug()<<"[LLMTVD]标签无异常:"<<tagName<<":"<<rawValue;
        return value;

    }
    //大小写
    QString valueLower=rawValue.toLower();
    for(const QString &v:validValues){
        if(v.toLower()==valueLower){
            qDebug()<<"[LLMTVD]大小写纠正:"<<tagName<<":"<<value<<"->"<<v;
            return v;
        }
    }
    //编辑距离检测
    int threshold=getEditDistanceThreshold(value);
    QString bestMatch;
    int mindistance=threshold+1;
    for(const QString &v: validValues){
        int distance=levenshteinDistance(value,v);
        if(distance<=threshold&&distance<mindistance){
            mindistance=distance;
            bestMatch=v;
        }
    }
    if(!bestMatch.isEmpty()){
        qDebug()<<"[LLMTVD]编辑距离修正:"<<tagName<<":"<<value<<"->"<<bestMatch;
        return bestMatch;
    }
    //继承
    if(!prevValue.isEmpty()&&validValues.contains(prevValue)){
        qDebug()<<"[LLMTVD]匹配失败，继承上一状态:"<<tagName<<":"<<value<<"->"<<prevValue;
        return prevValue;
    }
    //默认值
    QString defaultValue;
    if (tagName == "emotion") defaultValue = "happyIdle";
    else if (tagName == "blush") defaultValue = "unblushing";
    else if (tagName == "distance") defaultValue = "far";
    else if (tagName == "clothing") defaultValue = "schoolUniform";
    qDebug()<<"[LLMTVD]匹配+校验失败，使用默认值:"<<tagName<<":"<<value<<"->"<<defaultValue;
    return defaultValue;
}



int TagValidator::getEditDistanceThreshold(const QString &value)
{
    int len = value.length();
    if (len <= 4) return 1;
    if (len <= 8) return 2;
    if (len <= 12) return 3;
    return 4;
}

const QStringList TagValidator::VALID_CLOTHING{
    "pajama", "schoolUniform", "schoolUniformWithoutCap", "schoolUniformWithoutCoat"
};

const QStringList TagValidator::VALID_DISTANCE{
    "far", "closer"
};

const QStringList TagValidator::VALID_BLUSH{
    "unblushing", "blushing"
};

const QStringList TagValidator::VALID_EMOTIONS{
    "happyIdle", "happyMore", "amazing", "loving", "caring", "sad", "conscientious"
};
