#include "memorymanager.h"
#include "debuglog.h"

#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

#include "historyturn.h"

namespace {

RawEvent rawEventFromQuery(const QSqlQuery &query)
{
    RawEvent event;
    event.id = query.value("id").toLongLong();
    event.platform = query.value("platform").toString();
    event.channel = query.value("channel").toString();
    event.sessionId = query.value("session_id").toString();
    event.messageId = query.value("message_id").toString();
    event.direction = query.value("direction").toString();
    event.senderId = query.value("sender_id").toString();
    event.senderName = query.value("sender_name").toString();
    event.eventType = query.value("event_type").toString();
    event.content = query.value("content").toString();
    event.payloadJson = query.value("payload").toString();
    event.occurredAt = query.value("occurred_at").toDateTime();
    event.sensitive = query.value("sensitive").toInt() != 0;
    event.visibility = query.value("visibility").toString();
    event.processed = query.value("processed").toInt();
    return event;
}

QString rawEventColumns()
{
    return QStringLiteral(
        "id, platform, channel, session_id, message_id, direction, sender_id, "
        "sender_name, event_type, content, payload, occurred_at, sensitive, "
        "visibility, processed");
}

} // namespace

MemoryManager::MemoryManager(const QString &dbFilePath,QObject *parent)
    : QObject{parent}
{
    initDatabase(dbFilePath);
}

MemoryManager::~MemoryManager()
{
    if(m_db.isOpen()){
        m_db.close();

        qDebug()<<"[MemoryManager]:Sqlite已关闭";
    }
}

bool MemoryManager::saveQATurn(const QString &userInput, const QString &rawReply, const QList<SentenceText> &sentences, const RequestMeta &meta)
{
    if(!m_db.isOpen()){
        qDebug()<<"[MemoryManager]:数据库打开失败";
        return false;
    }

    QJsonObject rootObj;
    QJsonArray sentencesArray;
    for (const SentenceText &sentence : sentences) {
        QJsonObject sentenceObj;
        sentenceObj["zh_text"] = sentence.zhText;
        sentenceObj["ja_text"] = sentence.jaText;

        QJsonObject tagsObj;
        for (QMap<QString, QString>::const_iterator it=sentence.rawTags.constBegin();
             it!=sentence.rawTags.constEnd(); it++) {
            tagsObj[it.key()] = it.value();
        }
        sentenceObj["tags"] = tagsObj;
        sentencesArray.append(sentenceObj);
    }
    rootObj["sentences"]=sentencesArray;
    QString JsonStr = QJsonDocument(rootObj).toJson(QJsonDocument::Compact);

    // 执行原子行入库（渠道分桶 + 轮次语义：user_input 可为 NULL，TD-034/v3）
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO chat_history "
                  "(user_input, raw_reply, channel, session_id, speaker, sensitive, turn_kind, trigger_event_ids, subject) "
                  "VALUES (:user_input, :raw_reply, :channel, :session_id, :speaker, :sensitive, :turn_kind, :trigger_event_ids, :subject)");
    // 主动/感知轮次没有用户输入：绑定 SQL NULL，而不是空串
    query.bindValue(":user_input", userInput.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(userInput));
    query.bindValue(":raw_reply", JsonStr);
    query.bindValue(":channel", meta.channel);
    query.bindValue(":session_id", meta.sessionId);
    query.bindValue(":speaker", meta.speaker);
    query.bindValue(":sensitive", meta.sensitive ? 1 : 0);
    query.bindValue(":turn_kind", meta.turnKind.isEmpty() ? QStringLiteral("reply") : meta.turnKind);
    query.bindValue(":trigger_event_ids", meta.triggerEventIds.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(meta.triggerEventIds));
    query.bindValue(":subject", meta.subject.isEmpty() ? QStringLiteral("user") : meta.subject);

    if (!query.exec()) {
        qDebug()<<"[MemoryManager]:本轮对话保存失败:"<<query.lastError().text();
        return false;
    }

    qDebug()<<"[MemoryManager]:成功保存本轮对话"<<"channel="<<meta.channel<<"session="<<meta.sessionId;
    return true;

}

QList<HistoryTurn> MemoryManager::getHistoryTurn(int N, const QString &channel, const QString &sessionId)
{
    QList<HistoryTurn> historyTurnList;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]:数据库打开失败"<<m_db.lastError();
        return historyTurnList;
    }
    if(N<=0){
        return historyTurnList;
    }

    QSqlQuery query(m_db);
    QString sql = R"(
        SELECT user_input, raw_reply, timestamp, turn_kind, subject, speaker
        FROM (
            SELECT id, user_input, raw_reply, timestamp, turn_kind, subject, speaker
            FROM chat_history
            WHERE channel = :channel AND session_id = :session_id
            ORDER BY id DESC
            LIMIT :limit
        ) ORDER BY id ASC;
    )";
    query.prepare(sql);
    query.bindValue(":limit",N);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);
    if(!query.exec()){
        qDebug()<<"[MemoryManager]获取历史记录失败:"<<query.lastError();
    }
    while(query.next()){
        HistoryTurn turn;
        turn.userInput = query.value("user_input").toString();
        turn.rawReply = query.value("raw_reply").toString();
        turn.timestamp = query.value("timestamp").toDateTime();
        turn.turnKind = query.value("turn_kind").toString();
        turn.subject = query.value("subject").toString();
        turn.speaker = query.value("speaker").toString();
        historyTurnList.append(turn);
    }
    qDebug()<<"[MemoryManager]获取历史记录成功";
    return historyTurnList;
}

QList<HistoryTurn> MemoryManager::getHistoryTurn(int offset, int limit)
{
    QList<HistoryTurn> historyList;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return historyList;
    }
    QSqlQuery query(m_db);
    query.prepare(R"(
        SELECT id, user_input, raw_reply, timestamp, turn_kind, subject, speaker
        FROM chat_history
        ORDER BY id DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);
    if(!query.exec()){
        qDebug()<<"[MemoryManager]分页查询失败:"<<query.lastError();
        return historyList;
    }
    while (query.next()) {
        HistoryTurn turn;
        turn.id = query.value("id").toInt();
        turn.userInput = query.value("user_input").toString();
        turn.rawReply = query.value("raw_reply").toString();
        turn.timestamp = query.value("timestamp").toDateTime();
        turn.turnKind = query.value("turn_kind").toString();
        turn.subject = query.value("subject").toString();
        turn.speaker = query.value("speaker").toString();
        historyList.append(turn);
    }
    qDebug()<<"[MemoryManager]分页查询成功";
    return historyList;

}

qlonglong MemoryManager::getTotalHistoryCount()
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return 0;
    }
    QSqlQuery query(m_db);
    if (query.exec("SELECT COUNT(*) FROM chat_history")) {
        if (query.next()) {
                qDebug()<<"[MemoryManager]获取历史总记录数成功";
            return query.value(0).toInt();
        }
    }
    qDebug()<<"[MemoryManager]获取历史总记录数（0）成功";
    return 0;
}

bool MemoryManager::deleteTurnByID(int id)
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM chat_history WHERE id = :id");
    query.bindValue(":id", id);

    if (!query.exec()) {
        qDebug()<<"[MemoryManager]删除id:"<<id<<"的记录失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]删除id:"<<id<<"的记录成功";
    return true;
}

bool MemoryManager::clearAllHistory()
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM chat_history");
    if (!query.exec()) {
        qDebug()<<"[MemoryManager]清空记录失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]清空记录成功";
    return true;
}

QList<HistoryTurn> MemoryManager::getUnsummarizedTurns(qlonglong &outLastEndId, QList<qlonglong> &outSourceIds, const QString &channel, const QString &sessionId)
{
    QList<HistoryTurn> turns;
    outSourceIds.clear();
    outLastEndId = -1;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return turns;
    }
    QSqlQuery query(m_db);
    // 摘要游标按 channel+session_id 双键取（与摘要分桶一致）
    QString sql = R"(
        SELECT id, user_input, raw_reply, turn_kind, subject, speaker
        FROM chat_history
        WHERE channel = :channel AND session_id = :session_id
          AND id > (SELECT COALESCE(MAX(covered_turn_end_id), 0) FROM long_term_summary
                    WHERE channel = :channel2 AND session_id = :session_id2)
        ORDER BY id ASC;
    )";
    query.prepare(sql);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);
    query.bindValue(":channel2", channel);
    query.bindValue(":session_id2", sessionId);
    if(query.exec()){
        while(query.next()){
            qlonglong id=query.value("id").toLongLong();
            HistoryTurn turn;
            turn.id = id;
            turn.userInput = query.value("user_input").toString();
            turn.rawReply = query.value("raw_reply").toString();
            turn.turnKind = query.value("turn_kind").toString();
            turn.subject = query.value("subject").toString();
            turn.speaker = query.value("speaker").toString();

            turns.append(turn);
            outSourceIds.append(id);
            outLastEndId = id;
        }
    }
    else{
        qDebug()<<"[MemoryManager]获取未摘要历史失败"<<query.lastError();
    }
    qDebug()<<"[MemoryManager]获取未摘要历史成功";
    return turns;
}

// ============== v4: 角色档案 CRUD ==============

bool MemoryManager::upsertCharacterProfile(const QString &subject, const QString &key, const QString &value)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    // key 归一化：同 subject 内用编辑距离匹配已有 key，避免语义重复
    QString normalizedKey = normalizeProfileKey(subject, key);
    // 先尝试查询已有记录（同 subject + normalizedKey）
    query.prepare("SELECT id, value FROM character_profile WHERE subject=? AND key=?");
    query.addBindValue(subject);
    query.addBindValue(normalizedKey);
    if (query.exec() && query.next()) {
        int existingId = query.value(0).toInt();
        QString existingValue = query.value(1).toString();
        // 合并 value：去重合并
        QString mergedValue = mergeProfileValue(normalizedKey, existingValue, value);
        query.prepare("UPDATE character_profile SET value=?, updated_at=? WHERE id=?");
        query.addBindValue(mergedValue);
        query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        query.addBindValue(existingId);
        if (!query.exec()) {
            qDebug() << "[MemoryManager]更新角色档案失败:" << query.lastError();
            return false;
        }
        qDebug() << "[MemoryManager]角色档案已更新:" << subject << normalizedKey << mergedValue;
        return true;
    } else {
        // 新增
        query.prepare("INSERT OR REPLACE INTO character_profile (subject, key, value, updated_at) VALUES (?, ?, ?, ?)");
        query.addBindValue(subject);
        query.addBindValue(normalizedKey);
        query.addBindValue(value);
        query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        if (!query.exec()) {
            qDebug() << "[MemoryManager]新增角色档案失败:" << query.lastError();
            return false;
        }
        qDebug() << "[MemoryManager]角色档案已新增:" << subject << normalizedKey
                 << "valueLen=" << value.size();
        return true;
    }
}

QList<CharacterProfile> MemoryManager::getCharacterProfiles(const QString &subject)
{
    QList<CharacterProfile> profiles;
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return profiles;
    }
    QSqlQuery query(m_db);
    if (subject.isEmpty()) {
        query.prepare("SELECT id, subject, key, value, updated_at FROM character_profile ORDER BY subject, key");
    } else {
        query.prepare("SELECT id, subject, key, value, updated_at FROM character_profile WHERE subject=? ORDER BY key");
        query.addBindValue(subject);
    }
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询角色档案失败:" << query.lastError();
        return profiles;
    }
    while (query.next()) {
        CharacterProfile p;
        p.id = query.value(0).toLongLong();
        p.subject = query.value(1).toString();
        p.key = query.value(2).toString();
        p.value = query.value(3).toString();
        p.updatedAt = query.value(4).toDateTime();
        profiles.append(p);
    }
    qDebug() << "[MemoryManager]获取角色档案成功，共" << profiles.size() << "条";
    return profiles;
}

bool MemoryManager::deleteCharacterProfile(qlonglong id)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM character_profile WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]删除角色档案失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]角色档案已删除 id=" << id;
    return true;
}

// ============== v4: 情景记忆 CRUD ==============

bool MemoryManager::addEpisodicMemory(const QString &content, const QString &type, double importance,
                                      const QDateTime &eventTime, const QString &sourceIds,
                                      const QString &origin, const QString &visibility, const QString &subject,
                                      const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO episodic_memory "
                  "(content, event_time, importance, type, status, last_accessed, last_decay_at, source_ids, origin, visibility, subject, session_id) "
                  "VALUES (?, ?, ?, ?, 'active', ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(content);
    QDateTime et = eventTime.isValid() ? eventTime : QDateTime::currentDateTime();
    query.addBindValue(et.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(importance);
    query.addBindValue(type.isEmpty() ? "event" : type);
    query.addBindValue(et.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(et.toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(sourceIds);
    query.addBindValue(origin.isEmpty() ? QStringLiteral("pet") : origin);
    query.addBindValue(visibility.isEmpty() ? QStringLiteral("private") : visibility);
    query.addBindValue(subject.isEmpty() ? QStringLiteral("user") : subject);
    query.addBindValue(sessionId.isEmpty() ? QStringLiteral("default") : sessionId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]新增情景记忆失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]情景记忆已新增: chars=" << content.size()
             << "type=" << type << "importance=" << importance
             << "origin=" << origin << "visibility=" << visibility
             << "subject=" << subject;
    if (debugFullLogsEnabled()) {
        qDebug().noquote() << "[MemoryManager][debug] episodic:" << content;
    }
    return true;
}

QList<EpisodicMemory> MemoryManager::getActiveEpisodicMemories(double minImportance, int limit,
                                                              const QString &origin, const QString &visibility,
                                                              const QString &sessionId)
{
    QList<EpisodicMemory> memories;
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return memories;
    }
    QSqlQuery query(m_db);
    QString sql = "SELECT id, content, event_time, importance, type, status, last_accessed, source_ids, origin, visibility, subject, session_id "
                  "FROM episodic_memory WHERE status='active' AND importance >= :min";
    if (!origin.isEmpty()) {
        sql += " AND origin = :origin";
    }
    if (!visibility.isEmpty()) {
        sql += " AND visibility = :visibility";
    }
    if (!sessionId.isEmpty()) {
        sql += " AND session_id = :session_id";
    }
    sql += " ORDER BY importance DESC, event_time DESC LIMIT :limit";
    query.prepare(sql);
    query.bindValue(":min", minImportance);
    query.bindValue(":limit", limit);
    if (!origin.isEmpty()) {
        query.bindValue(":origin", origin);
    }
    if (!visibility.isEmpty()) {
        query.bindValue(":visibility", visibility);
    }
    if (!sessionId.isEmpty()) {
        query.bindValue(":session_id", sessionId);
    }
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询情景记忆失败:" << query.lastError();
        return memories;
    }
    while (query.next()) {
        EpisodicMemory m;
        m.id = query.value(0).toLongLong();
        m.content = query.value(1).toString();
        m.eventTime = query.value(2).toDateTime();
        m.importance = query.value(3).toDouble();
        m.type = query.value(4).toString();
        m.status = query.value(5).toString();
        m.lastAccessed = query.value(6).toDateTime();
        m.sourceIds = query.value(7).toString();
        m.origin = query.value(8).toString();
        m.visibility = query.value(9).toString();
        m.subject = query.value(10).toString();
        m.sessionId = query.value(11).toString();
        memories.append(m);
    }

    // 更新 last_accessed
    if (!memories.isEmpty()) {
        QStringList ids;
        for (const auto &m : memories) ids << QString::number(m.id);
        QSqlQuery upd(m_db);
        upd.prepare(QString("UPDATE episodic_memory SET last_accessed=? WHERE id IN (%1)")
                        .arg(ids.join(",")));
        upd.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        upd.exec();
    }

    qDebug() << "[MemoryManager]获取情景记忆成功，共" << memories.size() << "条";
    return memories;
}

bool MemoryManager::deleteEpisodicMemory(qlonglong id)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM episodic_memory WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]删除情景记忆失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]情景记忆已删除 id=" << id;
    return true;
}

bool MemoryManager::updateEpisodicMemoryStatus(qlonglong id, const QString &status)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE episodic_memory SET status=? WHERE id=?");
    query.addBindValue(status);
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]更新情景记忆状态失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]情景记忆状态已更新 id=" << id << "status=" << status;
    return true;
}

int MemoryManager::decayEpisodicMemory()
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return -1;
    }
    // 衰减规则：
    // - importance >= 0.8 的里程碑/承诺不衰减
    // - 其余按 -0.05/天 衰减
    // - importance < 0.1 删除
    m_db.transaction();
    try {
        QSqlQuery query(m_db);
        QString decaySql = R"(
            UPDATE episodic_memory
            SET importance = MAX(0, importance - 0.05 * (julianday('now', 'localtime') - julianday(last_decay_at))),
                last_decay_at = datetime('now', 'localtime')
            WHERE importance < 0.8
              AND (julianday('now', 'localtime') - julianday(last_decay_at)) >= 0.0416
        )";
        if (!query.exec(decaySql)) throw query.lastError();
        int updatedRows = query.numRowsAffected();

        QString cleanSql = "DELETE FROM episodic_memory WHERE importance < 0.1 AND status='active'";
        if (!query.exec(cleanSql)) throw query.lastError();
        int deletedRows = query.numRowsAffected();

        m_db.commit();
        if (updatedRows > 0 || deletedRows > 0) {
            qDebug()<<QString("[MemoryManager]情景记忆衰减完成:更新%1条，删除%2条。")
                            .arg(updatedRows).arg(deletedRows);
        } else {
            qDebug()<<"[MemoryManager]情景记忆保持最新，无需衰减。";
        }
        return deletedRows;
    } catch (const QSqlError &err) {
        m_db.rollback();
        qDebug()<<"[MemoryManager]情景记忆衰减失败:"<<err.text();
        return -1;
    }
}

QString MemoryManager::normalizeProfileKey(const QString &subject, const QString &rawKey)
{
    QSqlQuery query(m_db);
    query.prepare("SELECT DISTINCT key FROM character_profile WHERE subject=?");
    query.addBindValue(subject);
    if(!query.exec()){
        qDebug()<<"[MemoryManager]归一化查询失败，保留原key:"<<rawKey;
        return rawKey;
    }
    QStringList existingKeys;
    while(query.next()){
        existingKeys<<query.value(0).toString();
    }
    //全匹配
    if(existingKeys.contains(rawKey)){
        return rawKey;
    }
    //编辑距离模糊匹配（同 subject 内）
    int threshold=getEditDistanceThreshold(rawKey);
    QString bestMatch;
    int minDist=threshold+1;
    for (const QString &existingKey : existingKeys) {
        int dist =levenshteinDistance(rawKey, existingKey);
        if (dist <= threshold && dist < minDist) {
            minDist = dist;
            bestMatch = existingKey;
        }
    }
    if (!bestMatch.isEmpty()) {
        qDebug()<<"[MemoryManager]key归一化["<<subject<<"]:"<<rawKey<<"->"<<bestMatch<<"编辑距离="<<minDist;
        return bestMatch;
    }
    qDebug()<<"[MemoryManager]新key维度["<<subject<<"]:"<<rawKey;
    return rawKey;
}

QString MemoryManager::mergeProfileValue(const QString &key, const QString &oldVal, const QString &newVal)
{
    //同key的值value的合并逻辑

    if (newVal.isEmpty()) return oldVal;
    if (newVal.length() >= oldVal.length()) return newVal;
    return oldVal;
}

int MemoryManager::levenshteinDistance(const QString &s1, const QString &s2)
{
    int n=s1.length();
    int m=s2.length();
    if(m==0)return m;
    if(n==0)return n;

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

int MemoryManager::getEditDistanceThreshold(const QString &value)
{
    int len = value.length();
    if (len <= 4) return 1;
    if (len <= 8) return 2;
    if (len <= 12) return 3;
    return 4;
}

bool MemoryManager::addLongTermSummary(const QString &summaryText, qlonglong coveredEndId, const QString &sourceIdsJson, const QString &channel, const QString &sessionId)
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    QString sql = R"(
        INSERT INTO long_term_summary (summary_text, covered_turn_end_id, source_ids, is_dirty, channel, session_id)
        VALUES (:text, :end_id, :source_ids, 0, :channel, :session_id)
    )";

    query.prepare(sql);
    query.bindValue(":text", summaryText);
    query.bindValue(":end_id", coveredEndId);
    query.bindValue(":source_ids", sourceIdsJson);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);

    if (!query.exec()) {
        qDebug()<<"[MemoryManager]添加记忆摘要失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]添加记忆摘要成功";
    return true;
}

QList<LongTermSummary> MemoryManager::getLatestSummaries(int limit, const QString &channel, const QString &sessionId)
{
    QList<LongTermSummary> summaries;
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]:数据库打开失败"<<m_db.lastError();
        return summaries;
    }
    if(limit<=0){
        return summaries;
    }

    QSqlQuery query(m_db);
    QString sql = R"(
        SELECT * FROM (
            SELECT * FROM long_term_summary
            WHERE is_dirty = 0 AND channel = :channel AND session_id = :session_id
            ORDER BY id DESC
            LIMIT :limit
        ) ORDER BY id ASC
    )";

    query.prepare(sql);
    query.bindValue(":limit", limit);
    query.bindValue(":channel", channel);
    query.bindValue(":session_id", sessionId);

    if (query.exec()) {
        while (query.next()) {
            LongTermSummary s;
            s.id = query.value("id").toLongLong();
            s.summaryText = query.value("summary_text").toString();
            s.coveredTurnEndId = query.value("covered_turn_end_id").toLongLong();
            s.sourceIds = query.value("source_ids").toString();
            s.isDirty = query.value("is_dirty").toBool();
            s.createdAt = query.value("created_at").toDateTime();
            s.updatedAt = query.value("updated_at").toDateTime();
            summaries.append(s);
        }
    }
    else{
        if (!query.exec()) {
            qDebug()<<"[MemoryManager]获取记忆摘要失败:"<<query.lastError();
            return summaries;
        }
    }
    qDebug()<<"[MemoryManager]获取记忆摘要成功";
    return summaries;
}

bool MemoryManager::deleteLongTermSummary(qlonglong id)
{
    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM long_term_summary WHERE id = :id");
    query.bindValue(":id", id);
    if (!query.exec()) {
        qDebug()<<"[MemoryManager]删除记忆摘要失败:"<<query.lastError();
        return false;
    }
    qDebug()<<"[MemoryManager]删除记忆摘要成功";
    return true;
}


// ============== 关系状态 CRUD ==============

bool MemoryManager::upsertRelationshipState(const QString &dimension, double delta, const QString &subject)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    // 查询当前值（subject 隔离：'user' 即主人）
    query.prepare("SELECT value FROM relationship_state WHERE subject=? AND dimension=?");
    query.addBindValue(subject);
    query.addBindValue(dimension);
    if (!query.exec() || !query.next()) {
        // 维度不存在，新建（clamp 到 [0, 100]）
        double initVal = qBound(0.0, 30.0 + delta, 100.0);
        query.prepare("INSERT OR REPLACE INTO relationship_state (subject, dimension, value, updated_at) VALUES (?, ?, ?, ?)");
        query.addBindValue(subject);
        query.addBindValue(dimension);
        query.addBindValue(initVal);
        query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
        if (!query.exec()) {
            qDebug() << "[MemoryManager]新增关系状态失败:" << query.lastError();
            return false;
        }
        qDebug() << "[MemoryManager]关系状态已新增:" << subject << dimension << "=" << initVal;
        return true;
    }
    double oldVal = query.value(0).toDouble();
    double newVal = qBound(0.0, oldVal + delta, 100.0);
    query.prepare("UPDATE relationship_state SET value=?, updated_at=? WHERE subject=? AND dimension=?");
    query.addBindValue(newVal);
    query.addBindValue(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(subject);
    query.addBindValue(dimension);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]更新关系状态失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]关系状态已更新:" << subject << dimension << oldVal << "->" << newVal << "(delta=" << delta << ")";
    return true;
}

QList<RelationshipState> MemoryManager::getRelationshipStates(const QString &subject)
{
    QList<RelationshipState> states;
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return states;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT dimension, value, updated_at FROM relationship_state WHERE subject=? ORDER BY dimension");
    query.addBindValue(subject);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询关系状态失败:" << query.lastError();
        return states;
    }
    while (query.next()) {
        RelationshipState s;
        s.subject = subject;
        s.dimension = query.value(0).toString();
        s.value = query.value(1).toDouble();
        s.updatedAt = query.value(2).toDateTime();
        states.append(s);
    }
    qDebug() << "[MemoryManager]获取关系状态成功，subject=" << subject << "共" << states.size() << "条";
    return states;
}

bool MemoryManager::initRelationshipState()
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM relationship_state");
    query.exec();
    int count = 0;
    if (query.next()) count = query.value(0).toInt();
    if (count > 0) {
        qDebug() << "[MemoryManager]关系状态已存在，无需初始化";
        return true;
    }
    query.prepare("INSERT OR IGNORE INTO relationship_state (subject, dimension, value) VALUES ('user', 'intimacy', 30.0)");
    bool ok1 = query.exec();
    query.prepare("INSERT OR IGNORE INTO relationship_state (subject, dimension, value) VALUES ('user', 'trust', 30.0)");
    bool ok2 = query.exec();
    qDebug() << "[MemoryManager]关系状态初始化:" << (ok1 && ok2 ? "成功" : "失败");
    return ok1 && ok2;
}


// 幂等加列：预检列存在性 + duplicate column 容忍，应对重复/并发启动（三重防御）
static bool ensureColumn(QSqlQuery &query, const QString &table, const QString &column, const QString &alterDdl)
{
    // 1. 预检：列已存在则跳过
    if (query.exec(QString("PRAGMA table_info(%1)").arg(table))) {
        while (query.next()) {
            if (query.value(1).toString() == column) {
                return true;
            }
        }
    }
    // 2. 执行加列
    if (query.exec(alterDdl)) {
        qDebug() << "[MemoryManager]已加列:" << table << "." << column;
        return true;
    }
    // 3. 容忍：并发窗口内另一连接已完成加列
    const QString err = query.lastError().text();
    if (err.contains("duplicate column", Qt::CaseInsensitive)) {
        qDebug() << "[MemoryManager]列已被其他连接添加(并发忽略):" << column;
        return true;
    }
    qDebug() << "[MemoryManager]加列失败:" << alterDdl << "错误:" << err;
    return false;
}

bool MemoryManager::appendRawEvent(const QString &platform, const QString &channel,
                                   const QString &sessionId, const QString &eventType,
                                   const QString &content, const QString &messageId,
                                   const QString &direction, const QString &senderId,
                                   const QString &senderName, const QString &payloadJson,
                                   const QDateTime &occurredAt, bool sensitive,
                                   const QString &visibility)
{
    RawEventWrite event;
    event.platform = platform;
    event.channel = channel;
    event.sessionId = sessionId;
    event.eventType = eventType;
    event.content = content;
    event.messageId = messageId;
    event.direction = direction;
    event.senderId = senderId;
    event.senderName = senderName;
    event.payloadJson = payloadJson;
    event.occurredAt = occurredAt;
    event.sensitive = sensitive;
    event.visibility = visibility;
    return appendRawEvent(event);
}

bool MemoryManager::appendRawEvent(const RawEventWrite &event)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("INSERT OR IGNORE INTO raw_events "
                  "(platform, channel, session_id, message_id, direction, sender_id, sender_name, "
                  " event_type, content, payload, occurred_at, sensitive, visibility, processed, "
                  " redaction_status, redaction_rules, policy_version) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(event.platform);
    query.addBindValue(event.channel);
    query.addBindValue(event.sessionId);
    query.addBindValue(event.messageId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.messageId));
    query.addBindValue(event.direction.isEmpty()
                           ? QStringLiteral("in")
                           : event.direction);
    query.addBindValue(event.senderId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.senderId));
    query.addBindValue(event.senderName.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.senderName));
    query.addBindValue(event.eventType);
    query.addBindValue(event.contentNull
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.content));
    query.addBindValue(event.payloadNull || event.payloadJson.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.payloadJson));
    query.addBindValue((event.occurredAt.isValid()
                            ? event.occurredAt
                            : QDateTime::currentDateTime())
                           .toString("yyyy-MM-dd HH:mm:ss"));
    query.addBindValue(event.sensitive ? 1 : 0);
    query.addBindValue(event.visibility.isEmpty()
                           ? QStringLiteral("private")
                           : event.visibility);
    query.addBindValue(event.processed ? 1 : 0);
    query.addBindValue(event.redactionStatus.isEmpty()
                           ? QStringLiteral("none")
                           : event.redactionStatus);
    query.addBindValue(event.redactionRules.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.redactionRules));
    query.addBindValue(event.policyVersion.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.policyVersion));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]写入raw_events失败:" << query.lastError();
        return false;
    }
    qDebug() << "[MemoryManager]raw_events写入: platform=" << event.platform
             << "channel=" << event.channel << "session=" << event.sessionId
             << "type=" << event.eventType << "affected=" << query.numRowsAffected();
    if (debugFullLogsEnabled()) {
        qDebug().noquote() << "[MemoryManager][debug] raw content:" << event.content
                           << "payload:" << event.payloadJson;
    }
    return true;
}

bool MemoryManager::appendPrivacyBlockedEvent(const QString &platform,
                                              const QString &channel,
                                              const QString &sessionId,
                                              const QString &messageId,
                                              const QString &senderId,
                                              const QString &senderName,
                                              const QDateTime &occurredAt,
                                              const QStringList &ruleIds,
                                              const QString &policyVersion)
{
    QJsonArray rulesArray;
    for (const QString &ruleId : ruleIds) {
        rulesArray.append(ruleId);
    }
    const QString rulesJson = QString::fromUtf8(
        QJsonDocument(rulesArray).toJson(QJsonDocument::Compact));

    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }

    // 同一 message_id 之前可能已以 clean 状态入库；命中 blocked 后必须清空旧原文。
    if (!messageId.isEmpty()) {
        QSqlQuery update(m_db);
        update.prepare("UPDATE raw_events SET event_type='privacy_blocked', "
                       "content=NULL, payload=NULL, sensitive=1, "
                       "redaction_status='blocked', redaction_rules=?, "
                       "policy_version=?, processed=1 "
                       "WHERE channel=? AND message_id=?");
        update.addBindValue(rulesJson);
        update.addBindValue(policyVersion);
        update.addBindValue(channel);
        update.addBindValue(messageId);
        if (!update.exec()) {
            qDebug() << "[MemoryManager]清理旧 raw_events 失败:" << update.lastError();
            return false;
        }
        if (update.numRowsAffected() > 0) {
            qDebug() << "[MemoryManager]raw_events 已转为 privacy_blocked:"
                     << channel << sessionId;
            return true;
        }
    }

    RawEventWrite event;
    event.platform = platform;
    event.channel = channel;
    event.sessionId = sessionId;
    event.eventType = QStringLiteral("privacy_blocked");
    event.messageId = messageId;
    event.senderId = senderId;
    event.senderName = senderName;
    event.occurredAt = occurredAt;
    event.sensitive = true;
    event.visibility = QStringLiteral("private");
    event.redactionStatus = QStringLiteral("blocked");
    event.redactionRules = rulesJson;
    event.policyVersion = policyVersion;
    event.contentNull = true;
    event.payloadNull = true;
    event.processed = true;
    return appendRawEvent(event);
}

QList<RawEvent> MemoryManager::getUnprocessedGroupEvents(const QString &sessionId, int limit)
{
    QList<RawEvent> events;
    if (!m_db.isOpen() && !m_db.open()) {
        return events;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM raw_events "
                                 "WHERE channel='qq_group' AND session_id=? "
                                 "AND processed=0 AND event_type='message' "
                                 "ORDER BY id ASC LIMIT ?")
                      .arg(rawEventColumns()));
    query.addBindValue(sessionId);
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询未处理群事件失败:" << query.lastError();
        return events;
    }
    while (query.next()) {
        events.append(rawEventFromQuery(query));
    }
    return events;
}

QList<QString> MemoryManager::getPendingGroupSessions()
{
    QList<QString> sessions;
    if (!m_db.isOpen() && !m_db.open()) {
        return sessions;
    }
    QSqlQuery query(m_db);
    if (!query.exec("SELECT DISTINCT session_id FROM raw_events "
                    "WHERE channel='qq_group' AND processed=0 "
                    "AND event_type='message'")) {
        qDebug() << "[MemoryManager]查询待消费群会话失败:" << query.lastError();
        return sessions;
    }
    while (query.next()) {
        sessions.append(query.value(0).toString());
    }
    return sessions;
}

int MemoryManager::countUnprocessedGroupEvents(const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return 0;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COUNT(*) FROM raw_events "
                  "WHERE channel='qq_group' AND session_id=? "
                  "AND processed=0 AND event_type='message'");
    query.addBindValue(sessionId);
    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

QDateTime MemoryManager::latestUnprocessedGroupEventTime(const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return QDateTime();
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT MAX(occurred_at) FROM raw_events "
                  "WHERE channel='qq_group' AND session_id=? "
                  "AND processed=0 AND event_type='message'");
    query.addBindValue(sessionId);
    if (query.exec() && query.next()) {
        return query.value(0).toDateTime();
    }
    return QDateTime();
}

qlonglong MemoryManager::createInteractionBatch(const InteractionDraft &draft,
                                                const QList<qlonglong> &eventIds)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return -1;
    }
    if (eventIds.isEmpty()) {
        return -1;
    }

    QJsonArray sourceIds;
    for (qlonglong id : eventIds) {
        sourceIds.append(static_cast<double>(id));
    }
    const QString sourceIdsJson = QString::fromUtf8(
        QJsonDocument(sourceIds).toJson(QJsonDocument::Compact));

    m_db.transaction();
    QSqlQuery insert(m_db);
    insert.prepare("INSERT INTO interactions "
                   "(channel, session_id, started_at, ended_at, source_event_ids, "
                   " segmentation_method, confidence, summary_status, summary_attempts) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', 0)");
    insert.addBindValue(draft.channel);
    insert.addBindValue(draft.sessionId);
    insert.addBindValue((draft.startedAt.isValid() ? draft.startedAt
                                                   : QDateTime::currentDateTime())
                            .toString("yyyy-MM-dd HH:mm:ss"));
    insert.addBindValue((draft.endedAt.isValid() ? draft.endedAt
                                                 : QDateTime::currentDateTime())
                            .toString("yyyy-MM-dd HH:mm:ss"));
    insert.addBindValue(sourceIdsJson);
    insert.addBindValue(draft.segmentationMethod);
    insert.addBindValue(draft.confidence);
    if (!insert.exec()) {
        qDebug() << "[MemoryManager]创建 interaction 失败:" << insert.lastError();
        m_db.rollback();
        return -1;
    }
    const qlonglong interactionId = insert.lastInsertId().toLongLong();

    for (const auto &participant : draft.participants) {
        QSqlQuery p(m_db);
        p.prepare("INSERT OR IGNORE INTO interaction_participants "
                  "(interaction_id, user_id, display_name_at_time) "
                  "VALUES (?, ?, ?)");
        p.addBindValue(interactionId);
        p.addBindValue(participant.first);
        p.addBindValue(participant.second);
        if (!p.exec()) {
            qDebug() << "[MemoryManager]写入 interaction participant 失败:" << p.lastError();
            m_db.rollback();
            return -1;
        }
    }

    QStringList placeholders;
    for (int i = 0; i < eventIds.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery mark(m_db);
    mark.prepare(QStringLiteral("UPDATE raw_events SET processed=1 WHERE id IN (%1)")
                     .arg(placeholders.join(',')));
    for (qlonglong id : eventIds) {
        mark.addBindValue(id);
    }
    if (!mark.exec()) {
        qDebug() << "[MemoryManager]标记 raw_events processed 失败:" << mark.lastError();
        m_db.rollback();
        return -1;
    }

    const qlonglong lastEventId = eventIds.last();
    QSqlQuery cursor(m_db);
    cursor.prepare("INSERT OR IGNORE INTO memory_cursor "
                   "(channel, session_id, last_event_id, last_turn_id, updated_at) "
                   "VALUES (?, ?, 0, 0, datetime('now','localtime'))");
    cursor.addBindValue(draft.channel);
    cursor.addBindValue(draft.sessionId);
    if (!cursor.exec()) {
        qDebug() << "[MemoryManager]初始化 memory_cursor 失败:" << cursor.lastError();
        m_db.rollback();
        return -1;
    }
    QSqlQuery updateCursor(m_db);
    updateCursor.prepare("UPDATE memory_cursor SET last_event_id=?, "
                         "updated_at=datetime('now','localtime') "
                         "WHERE channel=? AND session_id=?");
    updateCursor.addBindValue(lastEventId);
    updateCursor.addBindValue(draft.channel);
    updateCursor.addBindValue(draft.sessionId);
    if (!updateCursor.exec()) {
        qDebug() << "[MemoryManager]推进 memory_cursor 失败:" << updateCursor.lastError();
        m_db.rollback();
        return -1;
    }

    if (!m_db.commit()) {
        qDebug() << "[MemoryManager]提交 interaction 事务失败:" << m_db.lastError();
        m_db.rollback();
        return -1;
    }
    return interactionId;
}

QList<RawEvent> MemoryManager::getRawEventsByIds(const QList<qlonglong> &ids)
{
    QList<RawEvent> events;
    if (ids.isEmpty() || (!m_db.isOpen() && !m_db.open())) {
        return events;
    }
    QStringList placeholders;
    for (int i = 0; i < ids.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM raw_events WHERE id IN (%2) ORDER BY id ASC")
                      .arg(rawEventColumns(), placeholders.join(',')));
    for (qlonglong id : ids) {
        query.addBindValue(id);
    }
    if (!query.exec()) {
        qDebug() << "[MemoryManager]按 id 查询 raw_events 失败:" << query.lastError();
        return events;
    }
    while (query.next()) {
        events.append(rawEventFromQuery(query));
    }
    return events;
}

QList<qlonglong> MemoryManager::getPendingInteractionIds(int limit)
{
    QList<qlonglong> ids;
    if (!m_db.isOpen() && !m_db.open()) {
        return ids;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id FROM interactions "
                  "WHERE summary_status='pending' AND summary_attempts < 3 "
                  "ORDER BY id ASC LIMIT ?");
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询待摘要 interaction 失败:" << query.lastError();
        return ids;
    }
    while (query.next()) {
        ids.append(query.value(0).toLongLong());
    }
    return ids;
}

QList<RawEvent> MemoryManager::getInteractionSourceEvents(qlonglong interactionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return {};
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT source_event_ids FROM interactions WHERE id=?");
    query.addBindValue(interactionId);
    if (!query.exec() || !query.next()) {
        return {};
    }
    const QJsonArray ids = QJsonDocument::fromJson(
        query.value(0).toString().toUtf8()).array();
    QList<qlonglong> eventIds;
    for (const QJsonValue &value : ids) {
        eventIds.append(static_cast<qlonglong>(value.toDouble()));
    }
    return getRawEventsByIds(eventIds);
}

bool MemoryManager::saveInteractionSummary(qlonglong id, const QString &summary,
                                           const QString &status)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE interactions SET safe_summary=?, summary_status=?, "
                  "summary_attempts=CASE WHEN ?='ready' THEN 0 ELSE summary_attempts END "
                  "WHERE id=?");
    query.addBindValue(summary.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(summary));
    query.addBindValue(status);
    query.addBindValue(status);
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]保存 interaction summary 失败:" << query.lastError();
        return false;
    }
    return true;
}

bool MemoryManager::markInteractionSummaryFailed(qlonglong id)
{
    if (!m_db.isOpen() && !m_db.open()) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("UPDATE interactions SET "
                  "summary_attempts=summary_attempts+1, "
                  "summary_status=CASE WHEN summary_attempts+1 >= 3 "
                  "THEN 'failed' ELSE 'pending' END "
                  "WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]标记 interaction summary 失败失败:" << query.lastError();
        return false;
    }
    return true;
}

QList<InteractionSummary> MemoryManager::getRecentInteractionSummaries(
    const QString &channel, const QString &sessionId, int limit)
{
    QList<InteractionSummary> summaries;
    if (!m_db.isOpen() && !m_db.open()) {
        return summaries;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT id, channel, session_id, safe_summary, ended_at "
                  "FROM interactions "
                  "WHERE summary_status='ready' AND channel=? AND session_id=? "
                  "ORDER BY ended_at DESC LIMIT ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(qMax(1, limit));
    if (!query.exec()) {
        qDebug() << "[MemoryManager]查询 interaction summary 失败:" << query.lastError();
        return summaries;
    }
    while (query.next()) {
        InteractionSummary summary;
        summary.id = query.value(0).toLongLong();
        summary.channel = query.value(1).toString();
        summary.sessionId = query.value(2).toString();
        summary.summaryText = query.value(3).toString();
        summary.endedAt = query.value(4).toDateTime();
        summaries.append(summary);
    }
    return summaries;
}

bool MemoryManager::updateMemoryCursor(const QString &channel, const QString &sessionId,
                                       qlonglong lastEventId, qlonglong lastTurnId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare("INSERT OR REPLACE INTO memory_cursor "
                  "(channel, session_id, last_event_id, last_turn_id, updated_at) "
                  "VALUES (?, ?, ?, ?, datetime('now', 'localtime'))");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    query.addBindValue(lastEventId);
    query.addBindValue(lastTurnId);
    if (!query.exec()) {
        qDebug() << "[MemoryManager]更新memory_cursor失败:" << query.lastError();
        return false;
    }
    return true;
}

qlonglong MemoryManager::getMemoryCursorEventId(const QString &channel, const QString &sessionId)
{
    if (!m_db.isOpen() && !m_db.open()) {
        qDebug() << "[MemoryManager]数据库打开失败:" << m_db.lastError();
        return 0;
    }
    QSqlQuery query(m_db);
    query.prepare("SELECT COALESCE(last_event_id, 0) FROM memory_cursor "
                  "WHERE channel = ? AND session_id = ?");
    query.addBindValue(channel);
    query.addBindValue(sessionId);
    if (query.exec() && query.next()) {
        return query.value(0).toLongLong();
    }
    return 0;
}

void MemoryManager::initDatabase(const QString &dbFilePath)
{
    QDir dbDir = QFileInfo(dbFilePath).dir();
    if (!dbDir.exists()) {
        if (!dbDir.mkpath(".")) {
            qCritical() << "[MemoryManager] 无法创建数据库目录:" << dbDir.absolutePath();
            return;
        }
    }



    const QString connName = "memory_db_connection";
    if (QSqlDatabase::contains(connName)) {
        // 复用连接时不自动 open（旧文件可能已不存在），改由下方 setDatabaseName 后统一打开
        m_db = QSqlDatabase::database(connName, false);
        if (m_db.isOpen()) {
            m_db.close();
        }
    }
    else
    {
        m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
    }
    m_db.setDatabaseName(dbFilePath);


    if(!m_db.open()){
        qDebug()<<"[MemoryManager]数据库打开失败:"<<m_db.lastError();
        return;
    }

    qDebug()<<"[MemoryManager]数据库连接成功:"<<m_db;

    QSqlQuery query(m_db);
    int currentVersion = 0;
    if (query.exec("PRAGMA user_version")) {
        if (query.next()) {
            currentVersion = query.value(0).toInt();
        }
    }
    qDebug()<<"[MemoryManager]当前数据库版本:"<<currentVersion;
    m_db.transaction();

    try {
        if (currentVersion<1) {
            qDebug()<<"[MemoryManager]正在初始化数据库表结构:";

            // 1. 对话历史表
            QString createChatHistorySql = R"(
                CREATE TABLE IF NOT EXISTS chat_history (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp DATETIME DEFAULT (datetime('now', 'localtime')),
                    user_input TEXT NOT NULL,
                    raw_reply TEXT NOT NULL
                );
            )";
            if (!query.exec(createChatHistorySql)) {
                qDebug()<<"[MemoryManager]创建chat_history异常:"<<query.lastError();
            }

            // 2. 长期摘要表
            QString createSummarySql = R"(
                CREATE TABLE IF NOT EXISTS long_term_summary (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    summary_text TEXT NOT NULL,
                    covered_turn_end_id INTEGER NOT NULL,
                    source_ids TEXT NOT NULL,
                    is_dirty INTEGER DEFAULT 0,
                    created_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime'))
                );
            )";
            if (!query.exec(createSummarySql)) {
                qDebug()<<"[MemoryManager]创建long_term_summary异常:"<<query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_summary_covered_end ON long_term_summary(covered_turn_end_id);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_summary_is_dirty ON long_term_summary(is_dirty);");

            // 3. 角色档案表（取代已废弃的 user_profile）
            QString createCharProfileSql = R"(
                CREATE TABLE IF NOT EXISTS character_profile (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    subject TEXT NOT NULL,
                    key TEXT NOT NULL,
                    value TEXT NOT NULL,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    UNIQUE(subject, key)
                );
            )";
            if (!query.exec(createCharProfileSql)) {
                qDebug()<<"[MemoryManager]创建character_profile异常:"<<query.lastError();
            }

            // 4. 情景记忆表
            QString createEpisodicSql = R"(
                CREATE TABLE IF NOT EXISTS episodic_memory (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    content TEXT NOT NULL,
                    event_time DATETIME DEFAULT (datetime('now', 'localtime')),
                    importance REAL DEFAULT 0.5,
                    type TEXT DEFAULT 'event',
                    status TEXT DEFAULT 'active',
                    last_accessed DATETIME DEFAULT (datetime('now', 'localtime')),
                    last_decay_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    source_ids TEXT
                );
            )";
            if (!query.exec(createEpisodicSql)) {
                qDebug()<<"[MemoryManager]创建episodic_memory异常:"<<query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_importance ON episodic_memory(importance);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_type ON episodic_memory(type);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_status ON episodic_memory(status);");

            // 5. 关系状态表（亲密度/信任度等关系维度）
            QString createRelStateSql = R"(
                CREATE TABLE IF NOT EXISTS relationship_state (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    dimension TEXT NOT NULL UNIQUE,
                    value REAL NOT NULL,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime'))
                );
            )";
            if (!query.exec(createRelStateSql)) {
                qDebug()<<"[MemoryManager]创建relationship_state异常:"<<query.lastError();
            }
            // 首次创建时初始化默认关系维度
            query.prepare("SELECT COUNT(*) FROM relationship_state");
            query.exec();
            int relCount = 0;
            if (query.next()) relCount = query.value(0).toInt();
            if (relCount == 0) {
                query.prepare("INSERT OR IGNORE INTO relationship_state (dimension, value) VALUES ('intimacy', 30.0)");
                query.exec();
                query.prepare("INSERT OR IGNORE INTO relationship_state (dimension, value) VALUES ('trust', 30.0)");
                query.exec();
                qDebug()<<"[MemoryManager]关系状态已初始化: intimacy=30, trust=30";
            }

            if (!query.exec("PRAGMA user_version = 1")) {
                qDebug()<<"[MemoryManager]设置数据库版本异常:"<<query.lastError();
            }
            currentVersion = 1;
        }

        // ============== v2: 多渠道记忆扩展（Phase 0，见 docs/HUMANIZATION_PLAN.md 8.5） ==============
        if (currentVersion < 2) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v2(多渠道记忆扩展):";

            // 1. chat_history 渠道分桶列
            ensureColumn(query, "chat_history", "channel",
                         "ALTER TABLE chat_history ADD COLUMN channel TEXT NOT NULL DEFAULT 'pet'");
            ensureColumn(query, "chat_history", "session_id",
                         "ALTER TABLE chat_history ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'");
            ensureColumn(query, "chat_history", "speaker",
                         "ALTER TABLE chat_history ADD COLUMN speaker TEXT");
            ensureColumn(query, "chat_history", "sensitive",
                         "ALTER TABLE chat_history ADD COLUMN sensitive INTEGER NOT NULL DEFAULT 0");
            query.exec("CREATE INDEX IF NOT EXISTS idx_history_session ON chat_history(channel, session_id);");

            // 2. long_term_summary 渠道分桶列（摘要与未摘要游标按双键，审查修订 A1）
            ensureColumn(query, "long_term_summary", "channel",
                         "ALTER TABLE long_term_summary ADD COLUMN channel TEXT NOT NULL DEFAULT 'pet'");
            ensureColumn(query, "long_term_summary", "session_id",
                         "ALTER TABLE long_term_summary ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'");

            // 3. episodic_memory 渠道溯源列
            ensureColumn(query, "episodic_memory", "origin",
                         "ALTER TABLE episodic_memory ADD COLUMN origin TEXT NOT NULL DEFAULT 'pet'");

            // 4. relationship_state 重建：旧结构 dimension 单列 UNIQUE → 新结构 UNIQUE(subject, dimension)
            //    存量行复制为 subject='user'（'user' 即主人，E2 决策：永久保留不迁移）
            QString createRelV2Sql = R"(
                CREATE TABLE IF NOT EXISTS relationship_state_v2 (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    subject TEXT NOT NULL DEFAULT 'user',
                    dimension TEXT NOT NULL,
                    value REAL NOT NULL,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    UNIQUE(subject, dimension)
                );
            )";
            if (!query.exec(createRelV2Sql)) {
                qDebug() << "[MemoryManager]创建relationship_state_v2异常:" << query.lastError();
            }
            if (!query.exec("INSERT OR IGNORE INTO relationship_state_v2 (subject, dimension, value) "
                            "SELECT 'user', dimension, value FROM relationship_state")) {
                qDebug() << "[MemoryManager]关系状态数据迁移异常:" << query.lastError();
            }
            if (!query.exec("DROP TABLE relationship_state")) {
                qDebug() << "[MemoryManager]删除旧relationship_state表异常:" << query.lastError();
            }
            if (!query.exec("ALTER TABLE relationship_state_v2 RENAME TO relationship_state")) {
                qDebug() << "[MemoryManager]relationship_state重命名异常:" << query.lastError();
            }

            // 5. raw_messages 群聊原始消息表（Phase 0 只建不写，Phase 1 接数据源）
            QString createRawSql = R"(
                CREATE TABLE IF NOT EXISTS raw_messages (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    speaker TEXT,
                    content TEXT NOT NULL,
                    timestamp DATETIME DEFAULT (datetime('now', 'localtime')),
                    sensitive INTEGER NOT NULL DEFAULT 0,
                    consolidated INTEGER NOT NULL DEFAULT 0
                );
            )";
            if (!query.exec(createRawSql)) {
                qDebug() << "[MemoryManager]创建raw_messages异常:" << query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_session ON raw_messages(session_id, timestamp);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_consolidated ON raw_messages(consolidated);");

            if (!query.exec("PRAGMA user_version = 2")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 2;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v3: 记忆库调整（HUMANIZATION_PLAN 10.4） ==============
        if (currentVersion < 3) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v3(记忆库调整):";

            // 1. chat_history 重建：user_input 放开 NOT NULL + 轮次字段
            query.exec("DROP TABLE IF EXISTS chat_history_v3");
            QString createChatV3 = R"(
                CREATE TABLE chat_history_v3 (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp DATETIME DEFAULT (datetime('now', 'localtime')),
                    user_input TEXT,
                    raw_reply TEXT NOT NULL,
                    channel TEXT NOT NULL DEFAULT 'pet',
                    session_id TEXT NOT NULL DEFAULT 'default',
                    speaker TEXT,
                    sensitive INTEGER NOT NULL DEFAULT 0,
                    turn_kind TEXT NOT NULL DEFAULT 'reply',
                    trigger_event_ids TEXT,
                    subject TEXT NOT NULL DEFAULT 'user'
                );
            )";
            if (!query.exec(createChatV3)) throw query.lastError();
            if (!query.exec("INSERT INTO chat_history_v3 "
                            "(id, timestamp, user_input, raw_reply, channel, session_id, speaker, sensitive) "
                            "SELECT id, timestamp, user_input, raw_reply, channel, session_id, speaker, sensitive "
                            "FROM chat_history")) throw query.lastError();
            if (!query.exec("DROP TABLE chat_history")) throw query.lastError();
            if (!query.exec("ALTER TABLE chat_history_v3 RENAME TO chat_history")) throw query.lastError();
            query.exec("CREATE INDEX IF NOT EXISTS idx_history_session ON chat_history(channel, session_id);");

            // 2. raw_messages → raw_events（全量事件流水）
            bool hasLegacyRaw = false;
            if (query.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='raw_messages'")
                && query.next()) {
                hasLegacyRaw = true;
            }
            QString createRawEvents = R"(
                CREATE TABLE IF NOT EXISTS raw_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    platform TEXT NOT NULL,
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    message_id TEXT,
                    direction TEXT NOT NULL DEFAULT 'in',
                    sender_id TEXT,
                    sender_name TEXT,
                    event_type TEXT NOT NULL,
                    content TEXT,
                    payload TEXT,
                    occurred_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    sensitive INTEGER NOT NULL DEFAULT 0,
                    visibility TEXT NOT NULL DEFAULT 'private',
                    processed INTEGER NOT NULL DEFAULT 0,
                    UNIQUE(channel, message_id)
                );
            )";
            if (!query.exec(createRawEvents)) throw query.lastError();
            if (hasLegacyRaw) {
                if (!query.exec("INSERT OR IGNORE INTO raw_events "
                                "(platform, channel, session_id, sender_name, event_type, content, "
                                " occurred_at, sensitive, visibility, processed, direction) "
                                "SELECT CASE WHEN channel LIKE 'qq%' THEN 'qq' ELSE 'pet' END, "
                                "       channel, session_id, speaker, 'message', content, "
                                "       timestamp, sensitive, 'private', consolidated, 'in' "
                                "FROM raw_messages")) throw query.lastError();
                if (!query.exec("DROP TABLE raw_messages")) throw query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_events_session ON raw_events(channel, session_id, occurred_at);");
            query.exec("CREATE INDEX IF NOT EXISTS idx_raw_events_processed ON raw_events(processed);");

            // 3. episodic_memory 可见性/主体
            if (!ensureColumn(query, "episodic_memory", "visibility",
                              "ALTER TABLE episodic_memory ADD COLUMN visibility TEXT NOT NULL DEFAULT 'private'")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "episodic_memory", "subject",
                              "ALTER TABLE episodic_memory ADD COLUMN subject TEXT NOT NULL DEFAULT 'user'")) {
                throw query.lastError();
            }

            // 4. 蒸馏游标（每 channel+session 一条）
            QString createCursor = R"(
                CREATE TABLE IF NOT EXISTS memory_cursor (
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    last_event_id INTEGER NOT NULL DEFAULT 0,
                    last_turn_id INTEGER NOT NULL DEFAULT 0,
                    updated_at DATETIME DEFAULT (datetime('now', 'localtime')),
                    PRIMARY KEY(channel, session_id)
                );
            )";
            if (!query.exec(createCursor)) throw query.lastError();

            if (!query.exec("PRAGMA user_version = 3")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 3;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v4: episodic 会话隔离（C3 三重隔离） ==============
        if (currentVersion < 4) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v4(episodic 会话隔离):";
            if (!ensureColumn(query, "episodic_memory", "session_id",
                              "ALTER TABLE episodic_memory ADD COLUMN session_id TEXT NOT NULL DEFAULT 'default'")) {
                throw query.lastError();
            }
            query.exec("CREATE INDEX IF NOT EXISTS idx_episodic_origin_session "
                       "ON episodic_memory(origin, session_id);");
            if (!query.exec("PRAGMA user_version = 4")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 4;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        // ============== v6: interactions（PR-2） ==============
        if (currentVersion < 6) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v6(interactions):";
            const QString createInteractions = R"(
                CREATE TABLE IF NOT EXISTS interactions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    channel TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    started_at DATETIME NOT NULL,
                    ended_at DATETIME NOT NULL,
                    source_event_ids TEXT NOT NULL,
                    segmentation_method TEXT NOT NULL,
                    confidence REAL NOT NULL DEFAULT 0.0,
                    safe_summary TEXT,
                    summary_status TEXT NOT NULL DEFAULT 'pending',
                    summary_attempts INTEGER NOT NULL DEFAULT 0,
                    created_at DATETIME DEFAULT (datetime('now','localtime'))
                );
            )";
            if (!query.exec(createInteractions)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interactions_session_time "
                            "ON interactions(channel, session_id, started_at)")) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interactions_summary_status "
                            "ON interactions(summary_status, summary_attempts)")) {
                throw query.lastError();
            }

            const QString createParticipants = R"(
                CREATE TABLE IF NOT EXISTS interaction_participants (
                    interaction_id INTEGER NOT NULL,
                    user_id TEXT NOT NULL,
                    display_name_at_time TEXT,
                    participation_kind TEXT NOT NULL DEFAULT 'message',
                    PRIMARY KEY(interaction_id, user_id),
                    FOREIGN KEY(interaction_id) REFERENCES interactions(id) ON DELETE CASCADE
                );
            )";
            if (!query.exec(createParticipants)) {
                throw query.lastError();
            }
            if (!query.exec("CREATE INDEX IF NOT EXISTS idx_interaction_participants_user "
                            "ON interaction_participants(user_id, interaction_id)")) {
                throw query.lastError();
            }

        }

        // ============== v5: raw event 脱敏审计（PR-1a） ==============
        if (currentVersion < 5) {
            qDebug() << "[MemoryManager]正在升级数据库结构至 v5(raw event 脱敏审计):";
            if (!ensureColumn(query, "raw_events", "redaction_status",
                              "ALTER TABLE raw_events ADD COLUMN redaction_status TEXT NOT NULL DEFAULT 'none'")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "raw_events", "redaction_rules",
                              "ALTER TABLE raw_events ADD COLUMN redaction_rules TEXT")) {
                throw query.lastError();
            }
            if (!ensureColumn(query, "raw_events", "policy_version",
                              "ALTER TABLE raw_events ADD COLUMN policy_version TEXT")) {
                throw query.lastError();
            }
            if (!query.exec("PRAGMA user_version = 5")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 5;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        if (currentVersion < 6) {
            if (!query.exec("PRAGMA user_version = 6")) {
                qDebug() << "[MemoryManager]设置数据库版本异常:" << query.lastError();
            }
            currentVersion = 6;
            qDebug() << "[MemoryManager]数据库结构升级完成,版本:" << currentVersion;
        }

        m_db.commit();
        qDebug()<<"[MemoryManager]数据库初始化完成,结构版本为:"<<currentVersion;
    }
    catch(const QSqlError &error){
        m_db.rollback();
        qDebug()<<"[MemoryManager]数据库更新失败:"<<error;
    }
}
