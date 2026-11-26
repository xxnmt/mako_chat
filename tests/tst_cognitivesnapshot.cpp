#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariantList>

#include "cognitivesnapshot.h"
#include "configmanager.h"
#include "memorymanager.h"

namespace {

// 测试里需要绕过 MemoryManager 的封装造少量原始行（提醒、时间戳修改）。
// 独立连接，避免影响 MemoryManager 自己的事务连接。
class RawDb
{
public:
    explicit RawDb(const QString &path)
        : m_connection(QStringLiteral("tst_snapshot_%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
        m_db.setDatabaseName(path);
    }
    ~RawDb()
    {
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connection);
    }
    bool open() { return m_db.open(); }
    bool exec(const QString &sql, const QVariantList &binds = {})
    {
        QSqlQuery query(m_db);
        query.prepare(sql);
        for (const auto &value : binds) query.addBindValue(value);
        return query.exec();
    }
    QVariant scalar(const QString &sql)
    {
        QSqlQuery query(m_db);
        if (!query.exec(sql) || !query.next()) return {};
        return query.value(0);
    }

private:
    QString m_connection;
    QSqlDatabase m_db;
};

RequestMeta ownerMeta(int seq, const QString &channel = Channel::kPet,
                      const QString &session = Channel::kDefaultSession)
{
    RequestMeta meta;
    meta.channel = channel;
    meta.sessionId = session;
    meta.eventId = QStringLiteral("snapshot-event-%1").arg(seq);
    meta.runId = QStringLiteral("snapshot-run-%1").arg(seq);
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    return meta;
}

const CognitiveFactRef *findFact(const QList<CognitiveFactRef> &facts, const QString &needle)
{
    for (const auto &fact : facts)
        if (fact.text.contains(needle)) return &fact;
    return nullptr;
}

bool anyFactText(const QList<CognitiveFactRef> &facts, const QString &needle)
{
    return findFact(facts, needle) != nullptr;
}

bool anyRefStartsWith(const QJsonArray &rows, const QString &prefix)
{
    for (const auto &value : rows)
        if (value.toObject().value(QStringLiteral("ref")).toString().startsWith(prefix))
            return true;
    return false;
}

// R12-1a：预算/统计 notes 断言辅助。
bool anyNotePrefix(const QStringList &notes, const QString &prefix)
{
    for (const auto &note : notes)
        if (note.startsWith(prefix)) return true;
    return false;
}

bool validDomainStatus(const QString &status)
{
    return status == QStringLiteral("empty") || status == QStringLiteral("filtered")
        || status == QStringLiteral("query_cap") || status == QStringLiteral("serialized")
        || status == QStringLiteral("budget_trimmed") || status == QStringLiteral("overflow");
}

const QJsonObject findJsonObjectByRef(const QJsonArray &rows, const QString &ref)
{
    for (const auto &value : rows) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("ref")).toString() == ref) return row;
    }
    return {};
}

// saveQATurn 的 raw_reply 由 sentences 参数重建；测试必须显式给句子，
// 否则只会存出空 sentences（真实调用由解析层给出句子）。
QList<SentenceText> singleSentence(const QString &text)
{
    SentenceText sentence;
    sentence.zhText = text;
    sentence.jaText = QString();
    return {sentence};
}

// R12-1b：新 saveQATurn 不再写 chat_history.thought。快照的当前活动域读取统一视图
// （legacy thought/<id> + internal activity/<id>），需要 legacy 行的快照用例在这里显式补写。
static qlonglong promoteLatestTurnToThought(MemoryManager &db, const QString &interest)
{
    RawDb raw(db.databasePath());
    if (!raw.open()) return 0;
    const qlonglong turn = raw.scalar(QStringLiteral("SELECT max(id) FROM chat_history")).toLongLong();
    if (turn <= 0) return 0;
    const auto nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QJsonObject stored{
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("goal_kind"), QStringLiteral("interest")},
        {QStringLiteral("source_turn"), turn},
        {QStringLiteral("created_at"), nowIso},
        {QStringLiteral("updated_at"), nowIso},
        {QStringLiteral("artifact_ref"), QString{}}};
    if (!raw.exec(QStringLiteral(
                      "UPDATE chat_history SET thought=?,thought_result=?,review_after=0 "
                      "WHERE id=?"),
                  {interest,
                   QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)),
                   turn}))
        return 0;
    return turn;
}

// R12-0：快照 stats 的权限过滤用例需要临时写 identity.json（盲区），析构时恢复。
class IdentityFileGuard
{
public:
    IdentityFileGuard()
    {
        auto &cfg = ConfigManager::instance();
        m_path = QDir(cfg.getConfigDirPath()).filePath(QStringLiteral("identity.json"));
        QFile file(m_path);
        m_existed = file.open(QIODevice::ReadOnly);
        if (m_existed) m_original = file.readAll();
    }
    ~IdentityFileGuard()
    {
        auto &cfg = ConfigManager::instance();
        if (m_existed) {
            cfg.writeConfigDocument(QStringLiteral("identity.json"),
                                    m_original.isEmpty() ? QByteArray("{}") : m_original,
                                    nullptr);
        } else {
            QFile::remove(m_path);
            cfg.reloadIdentity();
        }
    }
    bool write(const QByteArray &contents)
    {
        return ConfigManager::instance().writeConfigDocument(
            QStringLiteral("identity.json"), contents, nullptr);
    }

private:
    QString m_path;
    QByteArray m_original;
    bool m_existed = false;
};

// R12-1a：通过 identity.json 写快照字节预算（ConfigManager 写入后会自动 reload）。
bool writeSnapshotBudget(IdentityFileGuard &guard, int bytes)
{
    return guard.write(QJsonDocument(QJsonObject{
        {QStringLiteral("cognition"),
         QJsonObject{{QStringLiteral("snapshot_budget_bytes"), bytes}}}})
        .toJson(QJsonDocument::Compact));
}

} // namespace

class TestCognitiveSnapshot : public QObject
{
    Q_OBJECT

private slots:
    // 1. 同一库里的六类来源分别落到正确的域与 authority。
    void classifiesRefsByAuthority()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("classify.db")));

        // (a) 主人的 user reply → Observation(UserStatement)
        const qlonglong userTurn = db.recordInput(QStringLiteral("我把钥匙放在玄关了"),
                                                  ownerMeta(1), false);
        QVERIFY(userTurn > 0);

        // (b) matter_revision 最新 understanding → Inference(SelfInference)
        RequestMeta matterMeta = ownerMeta(2);
        QVERIFY(db.enqueueObservation(matterMeta, QStringLiteral("我明天下午三点去面试")));
        const qlonglong source = db.pendingObservation().value(QStringLiteral("source")).toInteger();
        QVERIFY(source > 0);
        const QJsonObject change{{QStringLiteral("matter"),QString()},
                                 {QStringLiteral("version"),0},
                                 {QStringLiteral("topic"),QStringLiteral("面试")},
                                 {QStringLiteral("quote"),QStringLiteral("明天下午三点去面试")},
                                 {QStringLiteral("status"),QStringLiteral("confirmed")},
                                 {QStringLiteral("understanding"),QStringLiteral("明天下午三点有面试")},
                                 {QStringLiteral("kind"),QStringLiteral("self_report")}};
        QVERIFY(db.commitObservation(
            source, QJsonObject{{QStringLiteral("outcome"),QStringLiteral("updated")},
                                {QStringLiteral("changes"),QJsonArray{change}}}));

        // (c) assistant reply → SelfExpression(AssistantExpression)
        RequestMeta replyMeta = ownerMeta(3);
        QVERIFY(db.saveQATurn(
            QStringLiteral("你还记得面试吗"),
            QStringLiteral(R"({"sentences":[{"zh_text":"我记住了，面试加油。","ja_text":"","tags":{}}]})"),
            singleSentence(QStringLiteral("我记住了，面试加油。")), replyMeta));

        // (d) proactive 主动消息同样只算“她说过” → SelfExpression
        QVERIFY(db.recordProactiveMessage(Channel::kPet, Channel::kDefaultSession,
                                          QStringLiteral("茉子主动说出的那句话"),
                                          QStringLiteral("contact"), QStringLiteral("ref/1")) > 0);

        // (e) active thought → Activity（同时是自发动机）。R12-1b：新代码不写 thought，
        // 这里显式补一条 legacy 活动行来验证快照的统一读取（legacy + internal）。
        RequestMeta thoughtMeta = ownerMeta(4);
        QVERIFY(db.saveQATurn(
            QStringLiteral("你想做点什么"),
            QStringLiteral(R"({"sentences":[{"zh_text":"好呀","ja_text":"","tags":{}}]})"),
            singleSentence(QStringLiteral("好呀")), thoughtMeta));
        const qlonglong activityId =
            promoteLatestTurnToThought(db, QStringLiteral("想写一首关于雨的诗"));
        QVERIFY(activityId > 0);
        QCOMPARE(db.nextThought(false).value(QStringLiteral("id")).toInteger(), activityId);

        // (f) pending reminder → Commitment（真实委托）
        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            const qint64 due = QDateTime::currentSecsSinceEpoch() + 3600;
            QVERIFY(raw.exec(
                QStringLiteral("INSERT INTO reminder(turn,channel,session,due,expires,message,state) "
                               "VALUES(?,?,?,?,?,?,'pending')"),
                {userTurn, Channel::kPet, Channel::kDefaultSession, due, due + 3600,
                 QStringLiteral("提醒我给妈妈打电话")}));
        }

        // (g) attention wake → InternalWake
        db.setAttentionState(QStringLiteral("last_wake_at"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

        RequestMeta meta = ownerMeta(9);
        CognitiveSnapshotBuilder builder(&db);
        const CognitiveSnapshot snapshot = builder.build(
            meta, QStringLiteral("run-1"), QStringLiteral("trigger-1"), QStringLiteral("test"));

        // 主人原话
        const auto *observation = findFact(snapshot.observations, QStringLiteral("钥匙"));
        QVERIFY(observation != nullptr);
        QCOMPARE(observation->ref, QStringLiteral("chat/%1").arg(userTurn));
        QVERIFY(observation->authority == EvidenceAuthority::UserStatement);

        // 她说过的话（普通回复 + 主动消息）
        QVERIFY(anyFactText(snapshot.selfExpressions, QStringLiteral("面试加油")));
        QVERIFY(anyFactText(snapshot.selfExpressions, QStringLiteral("茉子主动说出的那句话")));
        for (const auto &fact : snapshot.selfExpressions) {
            QVERIFY(fact.authority == EvidenceAuthority::AssistantExpression);
            QVERIFY(!anyFactText(snapshot.observations, fact.text));
        }

        // 她自己的理解
        const auto *inference = findFact(snapshot.inferences, QStringLiteral("明天下午三点有面试"));
        QVERIFY(inference != nullptr);
        QVERIFY(inference->authority == EvidenceAuthority::SelfInference);

        // 真实委托：只有 pending reminder
        QVERIFY(!snapshot.commitments.isEmpty());
        for (const auto &commitment : snapshot.commitments) {
            QCOMPARE(commitment.kind, QStringLiteral("reminder"));
            QVERIFY(commitment.ref.startsWith(QStringLiteral("reminder/")));
            QCOMPARE(commitment.state, QStringLiteral("pending"));
        }
        QVERIFY(anyFactText(snapshot.selfExpressions, QStringLiteral("面试加油")));
        QVERIFY(!snapshot.commitments.first().text.isEmpty());

        // 私人活动
        QVERIFY(!snapshot.activities.isEmpty());
        const auto activity = findJsonObjectByRef(
            snapshot.activities, QStringLiteral("thought/%1").arg(activityId));
        QVERIFY(!activity.isEmpty());
        QCOMPARE(activity.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        QVERIFY(activity.value(QStringLiteral("version")).isDouble());

        // 内部唤醒：attention 在后，且永远不含委托
        QVERIFY(anyRefStartsWith(snapshot.internalWakes, QStringLiteral("attention/")));
        QVERIFY(!anyRefStartsWith(snapshot.internalWakes, QStringLiteral("reminder/")));

        // 提示词 JSON：只有数据域与 policy_view，不含静态说明键（说明已移到
        // LLMService::cognitiveSnapshotMessage 的 system content），总量不超字节预算。
        const QJsonObject prompt = builder.toPromptJson(snapshot);
        for (const auto &key : {QStringLiteral("observations"), QStringLiteral("self_expressions"),
                                QStringLiteral("inferences"), QStringLiteral("motivations"),
                                QStringLiteral("commitments"), QStringLiteral("activities"),
                                QStringLiteral("internal_wakes"),
                                QStringLiteral("policy_view")}) {
            QVERIFY2(prompt.contains(key), qPrintable(QStringLiteral("missing domain: ") + key));
        }
        QVERIFY(!prompt.contains(QStringLiteral("note")));
        QVERIFY(!prompt.contains(QStringLiteral("internal_wake_note")));
        QVERIFY(QJsonDocument(prompt).toJson(QJsonDocument::Compact).size()
                <= CognitiveSnapshotBuilder::promptByteBudget());
    }

    // R12-0：每域 queried/eligible/serialized/omitted 统计与字节前后值；
    // 用权限过滤（盲区）区分 queried 与 eligible，用大文本触发预算裁剪验证 notes。
    void snapshotDomainStats()
    {
        // 先清空 identity 里的盲区等干扰配置，并显式固定快照字节预算为默认 4096，
        // 保证 queried=可读行数的基线可重复；后面的权限过滤子用例再显式写入盲区，
        // 析构时统一恢复原文件。
        IdentityFileGuard identityGuard;
        QVERIFY(identityGuard.write(
            QByteArray(R"({"cognition":{"snapshot_budget_bytes":4096}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("stats.db")));

        // 8 条较长的普通生活观察：序列化后必然超过 4096 字节预算，触发裁剪。
        const QString longObservation(160, QChar(0x666E));
        qlonglong firstObservation = 0;
        for (int index = 0; index < 8; ++index) {
            const auto turn = db.recordInput(
                longObservation + QString::number(index), ownerMeta(100 + index), false);
            QVERIFY(turn > 0);
            if (firstObservation == 0) firstObservation = turn;
        }
        // 开放关注（进入 motivations + internal_wakes 的 next_check_at）。
        {
            RequestMeta meta = ownerMeta(50);
            const auto turn = db.recordInput(QStringLiteral("我明天想去看看那家旧书店"), meta, true);
            QVERIFY(turn > 0);
            QVERIFY(db.recordQuiet(turn, QStringLiteral("close")));
            const auto claim = db.claimPrivateReflection(12, 60);
            QCOMPARE(claim.value(QStringLiteral("id")).toInteger(), turn);
            QVERIFY(db.finishPrivateReflection(
                turn, 1, QStringLiteral("consider"), QStringLiteral("旧书店还在不在"),
                QJsonObject{}, QJsonArray{}, QJsonObject{}, QString{},
                QJsonObject{{QStringLiteral("topic"), QStringLiteral("旧书店")},
                            {QStringLiteral("watch"), QStringLiteral("是否还营业")},
                            {QStringLiteral("next_check_after_minutes"), 60}}));
        }
        // 私人活动 + 真实委托 + 内部唤醒。
        QVERIFY(db.saveQATurn(
            QStringLiteral("你想做点什么"),
            QStringLiteral(R"({"sentences":[{"zh_text":"好呀","ja_text":"","tags":{}}]})"),
            singleSentence(QStringLiteral("好呀")), ownerMeta(60)));
        QVERIFY(promoteLatestTurnToThought(db, QStringLiteral("整理旧照片")) > 0);
        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            const qint64 due = QDateTime::currentSecsSinceEpoch() + 3600;
            QVERIFY(raw.exec(
                QStringLiteral("INSERT INTO reminder(turn,channel,session,due,expires,message,state) "
                               "VALUES(?,?,?,?,?,?,'pending')"),
                {firstObservation, Channel::kPet, Channel::kDefaultSession, due, due + 3600,
                 QStringLiteral("提醒我把旧照片整理完")}));
        }
        db.setAttentionState(QStringLiteral("last_wake_at"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

        RequestMeta meta = ownerMeta(90);
        CognitiveSnapshotBuilder builder(&db);
        const auto snapshot = builder.build(
            meta, QStringLiteral("run-stats"), QStringLiteral("trigger-stats"),
            QStringLiteral("test"));
        const auto buildStats = builder.stats();
        QCOMPARE(buildStats.domains.value(QStringLiteral("observations")).queried, 8);
        QCOMPARE(buildStats.domains.value(QStringLiteral("observations")).eligible, 8);
        QVERIFY(buildStats.domains.value(QStringLiteral("motivations")).eligible >= 1);
        QVERIFY(buildStats.domains.value(QStringLiteral("commitments")).eligible >= 1);
        QVERIFY(buildStats.domains.value(QStringLiteral("activities")).eligible >= 1);
        QVERIFY(buildStats.domains.value(QStringLiteral("internal_wakes")).eligible >= 1);
        for (auto it = buildStats.domains.constBegin(); it != buildStats.domains.constEnd(); ++it)
            QVERIFY2((*it).eligible <= (*it).queried, qPrintable(it.key()));

        const auto prompt = builder.toPromptJson(snapshot);
        Q_UNUSED(prompt)
        const auto &stats = builder.stats();
        const int budget = CognitiveSnapshotBuilder::promptByteBudget();
        QCOMPARE(budget, 4096);
        QVERIFY(stats.bytesBefore > budget);
        QVERIFY(stats.bytesAfter <= budget);
        int omittedTotal = 0;
        for (auto it = stats.domains.constBegin(); it != stats.domains.constEnd(); ++it) {
            QVERIFY((*it).serialized <= (*it).eligible);
            QVERIFY((*it).omitted == (*it).eligible - (*it).serialized);
            QVERIFY2(!(*it).status.isEmpty(), qPrintable(it.key()));
            omittedTotal += (*it).omitted;
        }
        QVERIFY(omittedTotal > 0);
        // 预算裁剪被明确记录，且被裁的域状态可诊断（不是静默丢数据）。
        const auto observationsStat = stats.domains.value(QStringLiteral("observations"));
        QCOMPARE(observationsStat.status, QStringLiteral("budget_trimmed"));
        QVERIFY(observationsStat.serialized >= 1);
        QVERIFY(observationsStat.serialized < observationsStat.eligible);
        bool budgetNote = false;
        for (const auto &note : stats.notes)
            budgetNote = budgetNote || note.startsWith(QStringLiteral("budget:"));
        QVERIFY2(budgetNote, qPrintable(stats.notes.join(QLatin1Char(';'))));
        // 8 条观察触达每域查询上限（LIMIT kDomainLimit），需有近似提示。
        bool queryCapNote = false;
        for (const auto &note : stats.notes)
            queryCapNote = queryCapNote
                || note == QStringLiteral("domain:query_cap:observations:approx");
        QVERIFY2(queryCapNote, qPrintable(stats.notes.join(QLatin1Char(';'))));

        // 权限过滤：盲区里的行会被查询命中（queried>0）但全部被读取策略过滤（eligible=0）。
        {
            IdentityFileGuard guard;
            QVERIFY(guard.write(QByteArray(
                R"({"memory":{"blind_zones":[{"channel":"pet","session_id":"quiet-place"}]}})")));
            RequestMeta hiddenMeta = ownerMeta(200);
            hiddenMeta.sessionId = QStringLiteral("quiet-place");
            for (int index = 0; index < 3; ++index) {
                hiddenMeta.eventId = QStringLiteral("snapshot-hidden-%1").arg(index);
                QVERIFY(db.recordInput(QStringLiteral("这是一条在盲区里的生活记录"),
                                       hiddenMeta, false) > 0);
            }
            CognitiveSnapshotBuilder hiddenBuilder(&db);
            const auto hiddenSnapshot = hiddenBuilder.build(
                hiddenMeta, QStringLiteral("run-hidden"),
                QStringLiteral("trigger-hidden"), QStringLiteral("test"));
            const auto hiddenStats = hiddenBuilder.stats();
            const auto hiddenObservations =
                hiddenStats.domains.value(QStringLiteral("observations"));
            QVERIFY(hiddenObservations.queried >= 3);
            QCOMPARE(hiddenObservations.eligible, 0);
            QCOMPARE(hiddenObservations.serialized, 0);
            // 策略过滤（盲区）与“查询为空”区分：queried>eligible 记为 filtered。
            QCOMPARE(hiddenObservations.status, QStringLiteral("filtered"));
            QVERIFY(hiddenSnapshot.policyView.value(QStringLiteral("blind_zone")).toBool());
        }
    }

    // 2. own_next_review_at 只作为内部唤醒出现；commitments 只有 reminder。
    void internalWakeNeverCommitment()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("wake.db")));

        const qlonglong turn = db.recordInput(QStringLiteral("我明天有一场考试"),
                                              ownerMeta(1), true);
        QVERIFY(turn > 0);
        QVERIFY(db.recordQuiet(turn, QStringLiteral("close")));
        const QJsonObject claim = db.claimPrivateReflection(3, 0);
        QCOMPARE(claim.value(QStringLiteral("id")).toInteger(), turn);
        const int version = claim.value(QStringLiteral("version")).toInt();
        QVERIFY(version > 0);
        const QJsonObject concern{{QStringLiteral("topic"), QStringLiteral("考试")},
                                  {QStringLiteral("watch"), QStringLiteral("他说考完")},
                                  {QStringLiteral("next_check_after_minutes"), 30}};
        QVERIFY(db.finishPrivateReflection(turn, version, QStringLiteral("consider"),
                                           QStringLiteral("我有点在意这场考试"),
                                           {}, {}, {}, {}, concern));

        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            const qint64 due = QDateTime::currentSecsSinceEpoch() + 1800;
            QVERIFY(raw.exec(
                QStringLiteral("INSERT INTO reminder(turn,channel,session,due,expires,message,state) "
                               "VALUES(?,?,?,?,?,?,'pending')"),
                {turn, Channel::kPet, Channel::kDefaultSession, due, due + 3600,
                 QStringLiteral("提醒我七点出门")}));
        }
        db.setAttentionState(QStringLiteral("last_wake_at"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

        CognitiveSnapshotBuilder builder(&db);
        const CognitiveSnapshot snapshot = builder.build(
            ownerMeta(9), QStringLiteral("run-2"), QStringLiteral("trigger-2"),
            QStringLiteral("private_review"));

        // 关注的 next_check_at 只进入 internal_wakes，并显式标注它自己的再检查时间。
        const auto concernWake = findJsonObjectByRef(
            snapshot.internalWakes, QStringLiteral("concern/%1").arg(turn));
        QVERIFY(!concernWake.isEmpty());
        QVERIFY(!concernWake.value(QStringLiteral("own_next_review_at")).toString().isEmpty());
        QCOMPARE(concernWake.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("concern_review"));
        for (const auto &commitment : snapshot.commitments)
            QVERIFY(!commitment.ref.startsWith(QStringLiteral("concern/")));

        // commitments 只有提醒；内部唤醒里没有任何提醒。
        QCOMPARE(snapshot.commitments.size(), 1);
        QCOMPARE(snapshot.commitments.first().kind, QStringLiteral("reminder"));
        QCOMPARE(snapshot.commitments.first().state, QStringLiteral("pending"));
        QVERIFY(!anyRefStartsWith(snapshot.internalWakes, QStringLiteral("reminder/")));

        // 提示词层：own_next_review_at 只能出现在 internal_wakes 条目里。
        const QJsonObject prompt = builder.toPromptJson(snapshot);
        bool wakeCarriesOwnCheck = false;
        for (const auto &value : prompt.value(QStringLiteral("internal_wakes")).toArray())
            wakeCarriesOwnCheck |= value.toObject().contains(QStringLiteral("own_next_review_at"));
        QVERIFY(wakeCarriesOwnCheck);
        for (const auto &value : prompt.value(QStringLiteral("commitments")).toArray())
            QVERIFY(!value.toObject().contains(QStringLiteral("own_next_review_at")));
        for (const auto &value : prompt.value(QStringLiteral("internal_wakes")).toArray())
            QVERIFY(!value.toObject().value(QStringLiteral("ref")).toString()
                         .startsWith(QStringLiteral("reminder/")));
    }

    // 3. 助手/主动消息永远不会变成“主人事实”，authority 各不相同。
    void assistantNeverUserFact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("authority.db")));

        const qlonglong userTurn = db.recordInput(
            QStringLiteral("主人自己说的一句话"), ownerMeta(1), false);
        QVERIFY(userTurn > 0);
        RequestMeta replyMeta = ownerMeta(2);
        QVERIFY(db.saveQATurn(
            QStringLiteral("助手回复前的主人输入"),
            QStringLiteral(R"({"sentences":[{"zh_text":"我建议你早点睡。","ja_text":"","tags":{}}]})"),
            singleSentence(QStringLiteral("我建议你早点睡。")), replyMeta));
        QVERIFY(db.recordProactiveMessage(Channel::kPet, Channel::kDefaultSession,
                                          QStringLiteral("主动发来的晚安"),
                                          QStringLiteral("contact"), QStringLiteral("ref/2")) > 0);

        CognitiveSnapshotBuilder builder(&db);
        const CognitiveSnapshot snapshot = builder.build(
            ownerMeta(9), QStringLiteral("run-3"), QStringLiteral("trigger-3"),
            QStringLiteral("foreground_reply"));

        QVERIFY(!anyFactText(snapshot.observations, QStringLiteral("我建议你早点睡")));
        QVERIFY(!anyFactText(snapshot.observations, QStringLiteral("主动发来的晚安")));
        QVERIFY(anyFactText(snapshot.observations, QStringLiteral("主人自己说的一句话")));

        const auto *ownerFact = findFact(snapshot.observations, QStringLiteral("主人自己说的一句话"));
        QVERIFY(ownerFact != nullptr);
        QVERIFY(ownerFact->authority == EvidenceAuthority::UserStatement);
        const auto *assistantFact = findFact(snapshot.selfExpressions, QStringLiteral("我建议你早点睡"));
        QVERIFY(assistantFact != nullptr);
        QVERIFY(assistantFact->authority == EvidenceAuthority::AssistantExpression);
        QVERIFY(ownerFact->authority != assistantFact->authority);

        // 反过来：她的话也不会出现在 observations；observations 里只有 user_statement。
        for (const auto &fact : snapshot.observations)
            QVERIFY(fact.authority == EvidenceAuthority::UserStatement);
    }

    // 4. 群聊 projection 不包含任何私聊来源（含敏感行、其它私聊渠道）。
    void groupProjectionExcludesPrivate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("projection.db")));

        RequestMeta privateMeta = ownerMeta(1);
        const qlonglong privateTurn = db.recordInput(
            QStringLiteral("私聊里的秘密原文"), privateMeta, false);
        QVERIFY(privateTurn > 0);
        RequestMeta sensitiveMeta = ownerMeta(2);
        sensitiveMeta.sensitive = true;
        QVERIFY(db.recordInput(QStringLiteral("敏感私聊不该出现"), sensitiveMeta, false) > 0);
        RequestMeta otherPrivateMeta = ownerMeta(3, Channel::kQQPrivate,
                                                 QStringLiteral("2014366825"));
        otherPrivateMeta.actorId = QStringLiteral("2014366825");
        QVERIFY(db.recordInput(QStringLiteral("另一个私聊渠道的话"), otherPrivateMeta, false) > 0);
        RequestMeta groupMeta = ownerMeta(4, Channel::kQQGroup, QStringLiteral("30001"));
        groupMeta.actorId = QStringLiteral("10001");
        QVERIFY(db.recordInput(QStringLiteral("群里说的话"), groupMeta, false) > 0);

        CognitiveSnapshotBuilder builder(&db);
        const CognitiveSnapshot groupSnapshot = builder.build(
            groupMeta, QStringLiteral("run-4"), QStringLiteral("trigger-4"),
            QStringLiteral("foreground_reply"));
        QVERIFY(anyFactText(groupSnapshot.observations, QStringLiteral("群里说的话")));
        QVERIFY(!anyFactText(groupSnapshot.observations, QStringLiteral("私聊里的秘密原文")));
        QVERIFY(!anyFactText(groupSnapshot.observations, QStringLiteral("敏感私聊不该出现")));
        QVERIFY(!anyFactText(groupSnapshot.observations, QStringLiteral("另一个私聊渠道的话")));

        // owner-private 视图仍能看到自己的私聊原文（敏感行除外）。
        const CognitiveSnapshot ownerSnapshot = builder.build(
            privateMeta, QStringLiteral("run-4"), QStringLiteral("trigger-4"),
            QStringLiteral("foreground_reply"));
        QVERIFY(anyFactText(ownerSnapshot.observations, QStringLiteral("私聊里的秘密原文")));
        QVERIFY(!anyFactText(ownerSnapshot.observations, QStringLiteral("敏感私聊不该出现")));
        QVERIFY(!anyFactText(ownerSnapshot.observations, QStringLiteral("群里说的话")));
    }

    // 5. 主人私聊各渠道的快照只投影本渠道事实；各渠道之间不互相串数据。
    void ownerPrivateChannelsStaySeparated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("owner-private.db")));

        const qlonglong petTurn = db.recordInput(QStringLiteral("晚安啦"), ownerMeta(1), false);
        QVERIFY(petTurn > 0);
        // close 仍作为前台状态记录；它不再参与任何主动联系门禁。
        QVERIFY(db.recordQuiet(petTurn, QStringLiteral("close")));

        RequestMeta qqMeta = ownerMeta(2, Channel::kQQPrivate, QStringLiteral("2014366825"));
        qqMeta.actorId = QStringLiteral("2014366825");
        const qlonglong qqTurn = db.recordInput(QStringLiteral("脚好点没"), qqMeta, false);
        QVERIFY(qqTurn > 0);

        CognitiveSnapshotBuilder builder(&db);
        const CognitiveSnapshot qqSnapshot = builder.build(
            qqMeta, QStringLiteral("run-5"), QStringLiteral("trigger-5"),
            QStringLiteral("foreground_reply"));
        QVERIFY(anyFactText(qqSnapshot.observations, QStringLiteral("脚好点没")));
        // pet 里的原文不会因此泄进 qq_private 的 observations。
        QVERIFY(!anyFactText(qqSnapshot.observations, QStringLiteral("晚安啦")));

        const CognitiveSnapshot petSnapshot = builder.build(
            ownerMeta(1), QStringLiteral("run-5"), QStringLiteral("trigger-5"),
            QStringLiteral("foreground_reply"));
        QVERIFY(anyFactText(petSnapshot.observations, QStringLiteral("晚安啦")));
        QVERIFY(!anyFactText(petSnapshot.observations, QStringLiteral("脚好点没")));
    }

    // 6. 同一份内容重复构建 hash 稳定；改一条 observation 后 hash 变化。
    void snapshotHashStable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("hash.db")));
        const qlonglong turn = db.recordInput(QStringLiteral("原来的观察原文"),
                                              ownerMeta(1), false);
        QVERIFY(turn > 0);

        CognitiveSnapshotBuilder builder(&db);
        const RequestMeta meta = ownerMeta(9);
        const CognitiveSnapshot first = builder.build(
            meta, QStringLiteral("run-6a"), QStringLiteral("trigger-6"),
            QStringLiteral("foreground_reply"));
        const CognitiveSnapshot second = builder.build(
            meta, QStringLiteral("run-6b"), QStringLiteral("trigger-6"),
            QStringLiteral("foreground_reply"));
        QCOMPARE(first.snapshotHash.size(), qsizetype(16));
        QCOMPARE(first.snapshotHash, second.snapshotHash);

        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            QVERIFY(raw.exec(QStringLiteral("UPDATE chat_history SET user_input=? WHERE id=?"),
                             {QStringLiteral("改过的观察原文"), turn}));
        }
        const CognitiveSnapshot changed = builder.build(
            meta, QStringLiteral("run-6a"), QStringLiteral("trigger-6"),
            QStringLiteral("foreground_reply"));
        QVERIFY(first.snapshotHash != changed.snapshotHash);
        QVERIFY(anyFactText(changed.observations, QStringLiteral("改过的观察原文")));
    }

    // 7. R12-1a：预算裁剪必须为每个非空域保留最小有效条目（ref/authority/at/摘要）；
    //    预算连最小集都放不下时显式整域 overflow 并记录，不静默假装各域齐全。
    void snapshotDomainMinimumsPreserved()
    {
        IdentityFileGuard budgetGuard;
        const QString longText(200, QChar(0x957F)); // 长文：让最小集本身也超过 2048 字节
        QVERIFY(writeSnapshotBudget(budgetGuard, 4096));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("minimums.db")));

        qlonglong firstObservation = 0;
        for (int index = 0; index < 8; ++index) {
            const qlonglong turn = db.recordInput(
                longText + QString::number(index), ownerMeta(100 + index), false);
            QVERIFY(turn > 0);
            if (firstObservation == 0) firstObservation = turn;
        }
        // 长回顾 note → inferences；长 topic 开放关注 → motivations + internal_wakes。
        const qlonglong concernTurn =
            db.recordInput(QStringLiteral("我最近一直惦记一件事"), ownerMeta(50), true);
        QVERIFY(concernTurn > 0);
        QVERIFY(db.recordQuiet(concernTurn, QStringLiteral("close")));
        const QJsonObject claim = db.claimPrivateReflection(12, 60);
        QCOMPARE(claim.value(QStringLiteral("id")).toInteger(), concernTurn);
        QVERIFY(db.finishPrivateReflection(
            concernTurn, claim.value(QStringLiteral("version")).toInt(),
            QStringLiteral("consider"), longText, QJsonObject{}, QJsonArray{}, QJsonObject{},
            QString{}, QJsonObject{{QStringLiteral("topic"), longText},
                                   {QStringLiteral("next_check_after_minutes"), 60}}));
        // 她的表达 + 私人活动（长文本；personal_interest 受 120 字符读取上限约束）。
        const QJsonObject longSentence{{QStringLiteral("zh_text"), longText},
                                       {QStringLiteral("ja_text"), QString()},
                                       {QStringLiteral("tags"), QJsonObject{}}};
        const QJsonObject replyPayload{
            {QStringLiteral("sentences"), QJsonArray{longSentence}}};
        QVERIFY(db.saveQATurn(QStringLiteral("你想做点什么"),
                              QString::fromUtf8(QJsonDocument(replyPayload)
                                                    .toJson(QJsonDocument::Compact)),
                              singleSentence(longText), ownerMeta(60)));
        QVERIFY(promoteLatestTurnToThought(db, longText.left(120)) > 0);
        // 真实委托（提醒 message 长，但 manager 会截断到读取上限）。
        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            const qint64 due = QDateTime::currentSecsSinceEpoch() + 3600;
            QVERIFY(raw.exec(
                QStringLiteral("INSERT INTO reminder(turn,channel,session,due,expires,message,state) "
                               "VALUES(?,?,?,?,?,?,'pending')"),
                {firstObservation, Channel::kPet, Channel::kDefaultSession, due, due + 3600,
                 longText}));
        }
        // 内部唤醒（attention）。
        db.setAttentionState(QStringLiteral("last_wake_at"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        // 再补一条最新的长观察：保证裁剪到“每域 1 条”时 observations 的最小条目本身
        // 足够大，使 2048 字节确实装不下最小集（显式 overflow 路径可确定触发）。
        QVERIFY(db.recordInput(longText, ownerMeta(91), false) > 0);

        const RequestMeta meta = ownerMeta(90);
        CognitiveSnapshotBuilder builder(&db);
        const auto snapshot = builder.build(
            meta, QStringLiteral("run-min"), QStringLiteral("trigger-min"),
            QStringLiteral("foreground_reply"));
        const auto prompt = builder.toPromptJson(snapshot);
        const auto stats = builder.stats();
        const QStringList domains{
            QStringLiteral("observations"), QStringLiteral("self_expressions"),
            QStringLiteral("inferences"), QStringLiteral("motivations"),
            QStringLiteral("commitments"), QStringLiteral("activities"),
            QStringLiteral("internal_wakes")};
        // 预算 4096：最小集可容纳 → 每个非空域至少保留 1 条，且不出现 overflow。
        for (const auto &domain : domains) {
            const auto stat = stats.domains.value(domain);
            QVERIFY2(stat.eligible > 0, qPrintable(domain));
            QVERIFY2(prompt.value(domain).toArray().size() >= 1, qPrintable(domain));
            QVERIFY2(validDomainStatus(stat.status),
                     qPrintable(domain + QLatin1Char(':') + stat.status));
            QVERIFY(stat.serialized <= stat.eligible);
            QCOMPARE(stat.omitted, stat.eligible - stat.serialized);
        }
        QVERIFY(stats.bytesBefore > 4096);
        QVERIFY(stats.bytesAfter <= 4096);
        QVERIFY2(anyNotePrefix(stats.notes, QStringLiteral("budget:trimmed:")),
                 qPrintable(stats.notes.join(QLatin1Char(';'))));
        QVERIFY(!anyNotePrefix(stats.notes, QStringLiteral("budget:overflow:")));
        QCOMPARE(stats.domains.value(QStringLiteral("observations")).status,
                 QStringLiteral("budget_trimmed"));
        // observations 达到每域查询上限（8 条）→ 近似 query_cap 提示。
        QVERIFY(anyNotePrefix(stats.notes,
                              QStringLiteral("domain:query_cap:observations:approx")));

        // 预算 2048：最小集装不下 → 整域 overflow，并显式记录；不静默超预算。
        QVERIFY(writeSnapshotBudget(budgetGuard, 2048));
        CognitiveSnapshotBuilder tightBuilder(&db);
        const auto tightSnapshot = tightBuilder.build(
            meta, QStringLiteral("run-tight"), QStringLiteral("trigger-tight"),
            QStringLiteral("foreground_reply"));
        tightBuilder.toPromptJson(tightSnapshot);
        const auto tightStats = tightBuilder.stats();
        QVERIFY(tightStats.bytesBefore > 2048);
        QVERIFY2(anyNotePrefix(tightStats.notes, QStringLiteral("budget:overflow:")),
                 qPrintable(tightStats.notes.join(QLatin1Char(';'))));
        QVERIFY(tightStats.bytesAfter <= 2048
                || anyNotePrefix(tightStats.notes, QStringLiteral("budget:unresolved")));
        bool sawOverflowDomain = false;
        for (const auto &domain : domains) {
            const auto stat = tightStats.domains.value(domain);
            if (stat.status != QStringLiteral("overflow")) continue;
            sawOverflowDomain = true;
            QCOMPARE(stat.serialized, 0);
            QCOMPARE(stat.omitted, stat.eligible);
        }
        QVERIFY(sawOverflowDomain);
    }

    // 8. R12-1a：OpenWake 内部记录只以 InternalState 进入 inferences（不进 observations），
    //    读取有上限（插 5 条只取 ≤2），群聊投影不注入；调度状态只进 internal_wakes。
    void snapshotOpenWakeNotesInternalState()
    {
        IdentityFileGuard budgetGuard;
        QVERIFY(writeSnapshotBudget(budgetGuard, 4096));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("openwake-notes.db")));
        QVERIFY(db.recordInput(QStringLiteral("主人白天说的一句话"),
                               ownerMeta(1), false) > 0);
        // 调度状态（open_wake_state，scope=owner）→ internal_wakes。
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            for (int index = 0; index < 5; ++index) {
                QVERIFY(raw.exec(
                    QStringLiteral(
                        "INSERT INTO open_wake(scope_key,run_id,version,state,trigger_reason,"
                        "created_at,finished_at,decision,note) "
                        "VALUES('owner',?,1,'committed','schedule',?,?,?,?)"),
                    {QStringLiteral("wake-run-%1").arg(index), now - 100, now - 10 + index,
                     index % 2 == 0 ? QStringLiteral("reflect") : QStringLiteral("rest"),
                     QStringLiteral("内部唤醒记录%1").arg(index)}));
            }
        }
        CognitiveSnapshotBuilder builder(&db);
        const auto snapshot = builder.build(
            ownerMeta(9), QStringLiteral("run-wake"), QStringLiteral("wake/5"),
            QStringLiteral("private_review"));
        int wakeFacts = 0;
        for (const auto &fact : snapshot.inferences) {
            if (!fact.ref.startsWith(QStringLiteral("wake/"))) continue;
            ++wakeFacts;
            QVERIFY(fact.authority == EvidenceAuthority::InternalState);
            QCOMPARE(fact.subjectId, QStringLiteral("mako"));
            QCOMPARE(fact.privacy, QStringLiteral("private"));
            QVERIFY(!fact.text.isEmpty());
            QVERIFY(fact.ref.contains(QLatin1Char(':'))); // wake/<id>:<version>
            // OpenWake note 绝不进 observations。
            QVERIFY(!anyFactText(snapshot.observations, fact.text));
        }
        QCOMPARE(wakeFacts, 2); // 插 5 条，读取上限 2
        for (const auto &commitment : snapshot.commitments)
            QVERIFY(!commitment.ref.startsWith(QStringLiteral("wake")));
        // 可选的同源自发动机：kind 标注为 open_wake，不是委托。
        for (const auto &motivation : snapshot.motivations)
            if (motivation.ref.startsWith(QStringLiteral("wake/")))
                QCOMPARE(motivation.kind, QStringLiteral("open_wake"));
        // 调度状态只在 internal_wakes，且不是主人约定。
        const auto schedule = findJsonObjectByRef(
            snapshot.internalWakes, QStringLiteral("wake_schedule/owner"));
        QVERIFY(!schedule.isEmpty());
        QCOMPARE(schedule.value(QStringLiteral("kind")).toString(), QStringLiteral("open_wake"));
        QVERIFY(!schedule.value(QStringLiteral("at")).toString().isEmpty());

        const auto prompt = builder.toPromptJson(snapshot);
        int promptWakeFacts = 0;
        for (const auto &value : prompt.value(QStringLiteral("inferences")).toArray())
            if (value.toObject().value(QStringLiteral("ref")).toString()
                    .startsWith(QStringLiteral("wake/")))
                ++promptWakeFacts;
        QCOMPARE(promptWakeFacts, 2);
        QVERIFY(!prompt.contains(QStringLiteral("internal_wake_note")));

        // 群聊投影：不注入任何 OpenWake 内部记录与调度状态。
        RequestMeta groupMeta = ownerMeta(10, Channel::kQQGroup, QStringLiteral("30001"));
        groupMeta.actorId = QStringLiteral("10001");
        CognitiveSnapshotBuilder groupBuilder(&db);
        const auto groupSnapshot = groupBuilder.build(
            groupMeta, QStringLiteral("run-wake-group"), QStringLiteral("trigger-group"),
            QStringLiteral("foreground_reply"));
        for (const auto &fact : groupSnapshot.inferences)
            QVERIFY(!fact.ref.startsWith(QStringLiteral("wake/")));
        QVERIFY(groupSnapshot.internalWakes.isEmpty());
        QVERIFY(anyNotePrefix(groupBuilder.stats().notes,
                              QStringLiteral("group_projection:")));
    }

    // 8b. R12-1b：internal 活动进入 owner 私人快照（ref/origin/status/version/goal/progress），
    //     completed 不进入 current，群聊/第三方投影不读取、不泄漏。
    void snapshotInternalActivityUnifiedAndIsolated()
    {
        IdentityFileGuard identityGuard;
        QVERIFY(identityGuard.write(QByteArray(
            R"({"cognition":{"snapshot_budget_bytes":4096}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("internal-activity.db")));
        QString error;
        const auto created = db.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理旧照片"), QStringLiteral("先按年份分组"),
            QStringLiteral("foreground"), QStringLiteral("chat/501"),
            QStringLiteral("run-snapshot-activity"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString ref = created.value(QStringLiteral("ref")).toString();
        QVERIFY(ref.startsWith(QStringLiteral("activity/")));

        CognitiveSnapshotBuilder ownerBuilder(&db);
        const auto ownerSnapshot = ownerBuilder.build(
            ownerMeta(1), QStringLiteral("run-activity-owner"),
            QStringLiteral("foreground"), QStringLiteral("foreground_reply"));
        const auto row = findJsonObjectByRef(ownerSnapshot.activities, ref);
        QVERIFY(!row.isEmpty());
        QCOMPARE(row.value(QStringLiteral("origin")).toString(), QStringLiteral("internal"));
        QCOMPARE(row.value(QStringLiteral("origin_kind")).toString(),
                 QStringLiteral("foreground"));
        QCOMPARE(row.value(QStringLiteral("status")).toString(), QStringLiteral("active"));
        QCOMPARE(row.value(QStringLiteral("version")).toInt(), 1);
        QCOMPARE(row.value(QStringLiteral("goal")).toString(), QStringLiteral("先按年份分组"));
        const auto ownerStats = ownerBuilder.stats();
        QVERIFY(ownerStats.domains.value(QStringLiteral("activities")).eligible >= 1);
        QVERIFY(ownerStats.domains.value(QStringLiteral("activities")).queried
                >= ownerStats.domains.value(QStringLiteral("activities")).eligible);
        const auto ownerPrompt = ownerBuilder.toPromptJson(ownerSnapshot);
        QVERIFY(QJsonDocument(ownerPrompt.value(QStringLiteral("activities")).toArray())
                    .toJson(QJsonDocument::Compact).contains(ref.toUtf8()));

        // 群聊投影：不读取 owner 私人活动，prompt 里也不出现该 ref。
        RequestMeta groupMeta = ownerMeta(2, Channel::kQQGroup, QStringLiteral("30001"));
        groupMeta.actorId = QStringLiteral("10001");
        CognitiveSnapshotBuilder groupBuilder(&db);
        const auto groupSnapshot = groupBuilder.build(
            groupMeta, QStringLiteral("run-activity-group"),
            QStringLiteral("foreground"), QStringLiteral("foreground_reply"));
        QVERIFY(groupSnapshot.activities.isEmpty());
        QVERIFY(anyNotePrefix(groupBuilder.stats().notes, QStringLiteral("group_projection:")));
        const auto groupPrompt = groupBuilder.toPromptJson(groupSnapshot);
        QVERIFY(!QJsonDocument(groupPrompt.value(QStringLiteral("activities")).toArray())
                     .toJson(QJsonDocument::Compact).contains(ref.toUtf8()));

        // 第三方（subject 非主人）：即使走 qq_private 也不读取私人活动。
        RequestMeta thirdMeta = ownerMeta(3, Channel::kQQPrivate, QStringLiteral("2014366825"));
        thirdMeta.subject = QStringLiteral("qq:10001");
        thirdMeta.actorId = QStringLiteral("10001");
        CognitiveSnapshotBuilder thirdBuilder(&db);
        const auto thirdSnapshot = thirdBuilder.build(
            thirdMeta, QStringLiteral("run-activity-third"),
            QStringLiteral("foreground"), QStringLiteral("foreground_reply"));
        QVERIFY(thirdSnapshot.activities.isEmpty());
        QVERIFY(anyNotePrefix(thirdBuilder.stats().notes,
                              QStringLiteral("third_party_projection:")));
        const auto thirdPrompt = thirdBuilder.toPromptJson(thirdSnapshot);
        QVERIFY(!QJsonDocument(thirdPrompt.value(QStringLiteral("activities")).toArray())
                     .toJson(QJsonDocument::Compact).contains(ref.toUtf8()));

        // completed 后不再进入 current（即使还留着过时的 next_step）；进入完成成果目录。
        const auto finished = db.finishActivity(
            ref, 1,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("整理完了")},
                        {QStringLiteral("draft"), QStringLiteral("按年份分好了")},
                        {QStringLiteral("next_step"), QStringLiteral("过时步骤")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        CognitiveSnapshotBuilder afterBuilder(&db);
        const auto afterSnapshot = afterBuilder.build(
            ownerMeta(4), QStringLiteral("run-activity-after"),
            QStringLiteral("foreground"), QStringLiteral("foreground_reply"));
        QVERIFY(findJsonObjectByRef(afterSnapshot.activities, ref).isEmpty());
        QVERIFY(!QJsonDocument(afterBuilder.toPromptJson(afterSnapshot)
                                   .value(QStringLiteral("activities")).toArray())
                     .toJson(QJsonDocument::Compact).contains("过时步骤"));
        // R12-1b 补充：完成成果目录进入快照/提示词，带 ref/成果版本且不含过时 next_step。
        const auto resultRow = findJsonObjectByRef(afterSnapshot.recentActivityResults, ref);
        QVERIFY(!resultRow.isEmpty());
        QCOMPARE(resultRow.value(QStringLiteral("status")).toString(),
                 QStringLiteral("completed"));
        QCOMPARE(resultRow.value(QStringLiteral("artifact_ref")).toString(), ref);
        QCOMPARE(resultRow.value(QStringLiteral("artifact_version")).toInt(),
                 finished.value(QStringLiteral("version")).toInt());
        QCOMPARE(resultRow.value(QStringLiteral("title")).toString(),
                 QStringLiteral("整理旧照片"));
        QVERIFY(!resultRow.contains(QStringLiteral("next_step")));
        const auto afterPrompt = afterBuilder.toPromptJson(afterSnapshot);
        QVERIFY(QJsonDocument(afterPrompt.value(QStringLiteral("recent_activity_results"))
                                  .toArray())
                    .toJson(QJsonDocument::Compact).contains(ref.toUtf8()));
        QVERIFY(!QJsonDocument(afterPrompt.value(QStringLiteral("recent_activity_results"))
                                   .toArray())
                     .toJson(QJsonDocument::Compact).contains("过时步骤"));
        // 群聊/第三方快照的完成目录必须为空。
        CognitiveSnapshotBuilder groupAfterBuilder(&db);
        const auto groupAfter = groupAfterBuilder.build(
            groupMeta, QStringLiteral("run-activity-after-group"),
            QStringLiteral("foreground"), QStringLiteral("foreground_reply"));
        QVERIFY(groupAfter.recentActivityResults.isEmpty());
        QVERIFY(anyNotePrefix(groupAfterBuilder.stats().notes,
                              QStringLiteral("group_projection:")));
        CognitiveSnapshotBuilder thirdAfterBuilder(&db);
        const auto thirdAfter = thirdAfterBuilder.build(
            thirdMeta, QStringLiteral("run-activity-after-third"),
            QStringLiteral("foreground"), QStringLiteral("foreground_reply"));
        QVERIFY(thirdAfter.recentActivityResults.isEmpty());
        // stats 口径：完成目录 queried>=eligible，且序列化条数不超过预算域上限。
        QVERIFY(afterBuilder.stats().domains.value(QStringLiteral("recent_activity_results"))
                    .queried
                >= afterBuilder.stats().domains.value(QStringLiteral("recent_activity_results"))
                    .eligible);
    }

    // 8b. R12-1a：OpenWake 内部记录/调度按 scope 隔离：默认只读 owner scope；
    //     其他 scope、第三方会话、群聊、盲区都不可见；owner 前台/回顾快照可见。
    void snapshotOpenWakeNotesScopeAndBlindZone()
    {
        IdentityFileGuard identityGuard;
        QVERIFY(identityGuard.write(QByteArray(
            R"({"cognition":{"snapshot_budget_bytes":4096},)"
            R"("memory":{"blind_zones":[{"channel":"pet","session_id":"quiet-place"}]}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("openwake-scope.db")));
        QVERIFY(db.ensureOpenWakeScheduled(QStringLiteral("owner"), 0));
        {
            RawDb raw(db.databasePath());
            QVERIFY(raw.open());
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            const auto insertWake = [&raw, now](const QString &scope, const QString &runId,
                                                const QString &note, int offset) {
                return raw.exec(
                    QStringLiteral(
                        "INSERT INTO open_wake(scope_key,run_id,version,state,trigger_reason,"
                        "created_at,finished_at,decision,note) "
                        "VALUES(?,?,1,'committed','schedule',?,?,?,?)"),
                    {scope, runId, now - 100, now - 10 + offset,
                     QStringLiteral("reflect"), note});
            };
            QVERIFY(insertWake(QStringLiteral("owner"), QStringLiteral("scope-owner-run"),
                               QStringLiteral("主人私聊域内部记录"), 0));
            QVERIFY(insertWake(QStringLiteral("qq_group:30001"), QStringLiteral("scope-group-run"),
                               QStringLiteral("群聊域内部记录"), 1));
            QVERIFY(insertWake(QStringLiteral("qq_private:2014366825"),
                               QStringLiteral("scope-third-run"),
                               QStringLiteral("第三方私聊域内部记录"), 2));
        }
        // 直接接口：默认 owner scope 只返回 owner 行，其它 scope 不泄漏到默认读取。
        QCOMPARE(db.recentOpenWakeNotes(20).size(), 1);
        QCOMPARE(db.recentOpenWakeNotes(20).first().toObject()
                     .value(QStringLiteral("note")).toString(),
                 QStringLiteral("主人私聊域内部记录"));

        CognitiveSnapshotBuilder builder(&db);
        // owner 前台/回顾快照都可见 owner scope 的 note 与调度；其他 scope 不可见。
        for (const auto &purpose : {QStringLiteral("foreground_reply"),
                                    QStringLiteral("private_review")}) {
            const auto snapshot = builder.build(ownerMeta(9), QStringLiteral("run-scope"),
                                                QStringLiteral("wake/1"), purpose);
            QVERIFY(anyFactText(snapshot.inferences, QStringLiteral("主人私聊域内部记录")));
            QVERIFY(!anyFactText(snapshot.inferences, QStringLiteral("群聊域内部记录")));
            QVERIFY(!anyFactText(snapshot.inferences, QStringLiteral("第三方私聊域内部记录")));
            bool ownerMotivation = false;
            for (const auto &motivation : snapshot.motivations) {
                if (!motivation.ref.startsWith(QStringLiteral("wake/"))) continue;
                ownerMotivation = true;
                QCOMPARE(motivation.kind, QStringLiteral("open_wake"));
                QVERIFY(!motivation.summary.contains(QStringLiteral("群聊域")));
                QVERIFY(!motivation.summary.contains(QStringLiteral("第三方")));
            }
            QVERIFY(ownerMotivation);
            QVERIFY(!findJsonObjectByRef(snapshot.internalWakes,
                                         QStringLiteral("wake_schedule/owner")).isEmpty());
        }
        // 群聊投影：不读取 OpenWake note/调度。
        RequestMeta groupMeta = ownerMeta(10, Channel::kQQGroup, QStringLiteral("30001"));
        groupMeta.actorId = QStringLiteral("10001");
        const auto groupSnapshot = builder.build(groupMeta, QStringLiteral("run-scope-group"),
                                                 QStringLiteral("wake/1"),
                                                 QStringLiteral("foreground_reply"));
        for (const auto &fact : groupSnapshot.inferences)
            QVERIFY(!fact.ref.startsWith(QStringLiteral("wake/")));
        QVERIFY(groupSnapshot.internalWakes.isEmpty());
        QVERIFY(anyNotePrefix(builder.stats().notes, QStringLiteral("group_projection:")));
        // 第三方（subject 非主人）：即使走 qq_private 也不读取 OpenWake note/调度。
        RequestMeta thirdMeta = ownerMeta(11, Channel::kQQPrivate,
                                          QStringLiteral("2014366825"));
        thirdMeta.subject = QStringLiteral("qq:10001");
        thirdMeta.actorId = QStringLiteral("10001");
        const auto thirdSnapshot = builder.build(thirdMeta, QStringLiteral("run-scope-third"),
                                                 QStringLiteral("wake/1"),
                                                 QStringLiteral("foreground_reply"));
        for (const auto &fact : thirdSnapshot.inferences)
            QVERIFY(!fact.ref.startsWith(QStringLiteral("wake/")));
        QVERIFY(thirdSnapshot.internalWakes.isEmpty());
        // 盲区（owner 私聊渠道命中 blind_zone）：不读取 OpenWake note/调度。
        RequestMeta blindMeta = ownerMeta(12, Channel::kPet, QStringLiteral("quiet-place"));
        const auto blindSnapshot = builder.build(blindMeta, QStringLiteral("run-scope-blind"),
                                                 QStringLiteral("wake/1"),
                                                 QStringLiteral("private_review"));
        QVERIFY(blindSnapshot.policyView.value(QStringLiteral("blind_zone")).toBool());
        for (const auto &fact : blindSnapshot.inferences)
            QVERIFY(!fact.ref.startsWith(QStringLiteral("wake/")));
        QVERIFY(blindSnapshot.internalWakes.isEmpty());
    }

    // 9. R12-1a：快照字节预算读 ConfigManager（默认 4096，2048/8192 边界）；
    //    非法值夹紧由 tst_configmanager 覆盖。
    void snapshotBudgetConfigurable()
    {
        IdentityFileGuard guard;
        QVERIFY(guard.write(QByteArray("{}")));
        QCOMPARE(CognitiveSnapshotBuilder::promptByteBudget(), 4096);
        QVERIFY(writeSnapshotBudget(guard, 2048));
        QCOMPARE(CognitiveSnapshotBuilder::promptByteBudget(), 2048);
        QVERIFY(writeSnapshotBudget(guard, 8192));
        QCOMPARE(CognitiveSnapshotBuilder::promptByteBudget(), 8192);
    }

    // 10. R12-1a：snapshotHash 把 scope（渠道/会话/主体）与 purpose 纳入规范化内容；
    //     同一输入重复构建稳定，换 scope/purpose 变化（runId 仍不参与）。
    void snapshotHashIncludesScope()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager db(dir.filePath(QStringLiteral("hash-scope.db")));
        QVERIFY(db.recordInput(QStringLiteral("同一句观察"), ownerMeta(1), false) > 0);

        CognitiveSnapshotBuilder builder(&db);
        const RequestMeta petMeta = ownerMeta(1);
        const auto petFirst = builder.build(
            petMeta, QStringLiteral("run-a"), QStringLiteral("trigger-a"),
            QStringLiteral("foreground_reply"));
        // 同输入、不同 runId 重复构建：hash 稳定。
        const auto petAgain = builder.build(
            petMeta, QStringLiteral("run-b"), QStringLiteral("trigger-a"),
            QStringLiteral("foreground_reply"));
        QCOMPARE(petFirst.snapshotHash, petAgain.snapshotHash);

        RequestMeta qqMeta = ownerMeta(1, Channel::kQQPrivate, QStringLiteral("2014366825"));
        qqMeta.actorId = QStringLiteral("2014366825");
        const auto qq = builder.build(
            qqMeta, QStringLiteral("run-a"), QStringLiteral("trigger-a"),
            QStringLiteral("foreground_reply"));
        QVERIFY(petFirst.snapshotHash != qq.snapshotHash);

        // 同一 scope、不同用途：hash 也应不同（purpose 属 scope 契约）。
        const auto review = builder.build(
            petMeta, QStringLiteral("run-a"), QStringLiteral("trigger-a"),
            QStringLiteral("private_review"));
        QVERIFY(petFirst.snapshotHash != review.snapshotHash);
    }
};

QTEST_GUILESS_MAIN(TestCognitiveSnapshot)
#include "tst_cognitivesnapshot.moc"
