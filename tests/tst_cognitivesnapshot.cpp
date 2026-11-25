#include <QtTest>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariantList>

#include "cognitivesnapshot.h"
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

        // (e) active thought → Activity（同时是自发动机）
        RequestMeta thoughtMeta = ownerMeta(4);
        QVERIFY(db.saveQATurn(
            QStringLiteral("你想做点什么"),
            QStringLiteral(R"({"personal_interest":"想写一首关于雨的诗","sentences":[{"zh_text":"好呀","ja_text":"","tags":{}}]})"),
            singleSentence(QStringLiteral("好呀")), thoughtMeta));
        const qlonglong activityId = db.nextThought(false).value(QStringLiteral("id")).toInteger();
        QVERIFY(activityId > 0);

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

        // 提示词 JSON：所有域都在、带 internal_wake_note、总量不超预算
        const QJsonObject prompt = builder.toPromptJson(snapshot);
        for (const auto &key : {QStringLiteral("observations"), QStringLiteral("self_expressions"),
                                QStringLiteral("inferences"), QStringLiteral("motivations"),
                                QStringLiteral("commitments"), QStringLiteral("activities"),
                                QStringLiteral("internal_wakes"),
                                QStringLiteral("policy_view")}) {
            QVERIFY2(prompt.contains(key), qPrintable(QStringLiteral("missing domain: ") + key));
        }
        QCOMPARE(prompt.value(QStringLiteral("internal_wake_note")).toString(),
                 QStringLiteral("内部唤醒不是主人约定"));
        QVERIFY(QJsonDocument(prompt).toJson(QJsonDocument::Compact).size()
                <= CognitiveSnapshotBuilder::promptCharBudget());
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
};

QTEST_GUILESS_MAIN(TestCognitiveSnapshot)
#include "tst_cognitivesnapshot.moc"
