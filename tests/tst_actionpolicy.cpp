#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

#include "actionorigin.h"
#include "actionpolicy.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "utterance.h"

namespace {

// R12-1b：前台新活动已走 internal_activity；该旧分享策略测试显式构造 legacy 存量活动，
// 验证历史活动仍可被 ActionPolicy 读取并可完成/分享（不伪造用户聊天轮次）。
static qlonglong seedLegacyActiveThought(const QString &dbPath, const QString &interest,
                                         const QString &userText)
{
    const QString conn = QStringLiteral("tst_actionpolicy_legacy_%1")
        .arg(QDateTime::currentMSecsSinceEpoch());
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            QSqlDatabase::removeDatabase(conn);
            return 0;
        }
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO chat_history(user_input,raw_reply,channel,session_id,speaker,"
            "sensitive,turn_kind,subject,thought,thought_result,thought_version,"
            "review_after,timestamp) "
            "VALUES(?,'','pet','default','assistant',0,'reply','user',?,?,0,0,datetime('now'))"));
        q.addBindValue(userText);
        q.addBindValue(interest);
        const QJsonObject initial{{QStringLiteral("status"), QStringLiteral("active")},
                                  {QStringLiteral("artifact_ref"), QString()}};
        q.addBindValue(QString::fromUtf8(QJsonDocument(initial).toJson(QJsonDocument::Compact)));
        if (!q.exec()) {
            db.close();
            QSqlDatabase::removeDatabase(conn);
            return 0;
        }
        const qlonglong id = q.lastInsertId().toLongLong();
        db.close();
        QSqlDatabase::removeDatabase(conn);
        return id;
    }
}

// 主动联系/语义核验配置在 identity.json。测试里临时启用，析构时恢复。
class PolicyIdentityGuard
{
public:
    PolicyIdentityGuard()
    {
        auto &config = ConfigManager::instance();
        m_path = QDir(config.getConfigDirPath()).filePath(QStringLiteral("identity.json"));
        QFile file(m_path);
        m_existed = file.exists();
        if (file.open(QIODevice::ReadOnly)) m_original = file.readAll();
    }
    ~PolicyIdentityGuard()
    {
        auto &config = ConfigManager::instance();
        if (m_existed) {
            config.writeConfigDocument(QStringLiteral("identity.json"),
                                       m_original.isEmpty() ? QByteArray("{}") : m_original, nullptr);
        } else {
            QFile::remove(m_path);
            config.reloadIdentity();
        }
    }
    // R12-2a：测试内临时写入 identity（与生产 ConfigManager 同一路径），析构恢复。
    bool writeIdentity(const QByteArray &json)
    {
        auto &config = ConfigManager::instance();
        return config.writeConfigDocument(QStringLiteral("identity.json"), json, nullptr);
    }
    bool enable()
    {        QJsonObject target{{QStringLiteral("channel"), Channel::kPet},
                           {QStringLiteral("session_id"), Channel::kDefaultSession}};
        QJsonObject proactive{{QStringLiteral("enabled"), true},
                              {QStringLiteral("private_writing"), false},
                              {QStringLiteral("targets"), QJsonArray{target}},
                              // start==end 表示不设安静时段，避免用例受执行时刻影响。
                              {QStringLiteral("quiet_hours"), QJsonObject{{QStringLiteral("start"), QStringLiteral("00:00")},
                                                                          {QStringLiteral("end"), QStringLiteral("00:00")}}},
                              {QStringLiteral("cooldown_seconds"), 0}};
        QJsonObject cognition{{QStringLiteral("semantic_verifier"),
                               QJsonObject{{QStringLiteral("enabled"), true},
                                           {QStringLiteral("max_verifier_calls"), 1},
                                           {QStringLiteral("max_repairs"), 1}}}};
        const QJsonObject root{{QStringLiteral("proactive"), proactive},
                               {QStringLiteral("cognition"), cognition}};
        return ConfigManager::instance().writeConfigDocument(
            QStringLiteral("identity.json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact), nullptr);
    }

private:
    QString m_path;
    QByteArray m_original;
    bool m_existed = false;
};

RequestMeta policyMeta(const QString &event)
{
    RequestMeta meta;
    // 与真实运行一致：主人私聊轮次的 subject 规范化为 user，渠道/会话必须落库，
    // 否则 ActionPolicy 的"主人原话"判定会（正确地）把它当成非主人来源。
    meta.channel = Channel::kPet;
    meta.sessionId = Channel::kDefaultSession;
    meta.subject = QStringLiteral("user");
    meta.turnKind = QStringLiteral("reply");
    meta.eventId = meta.runId = event;
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc();
    return meta;
}

// 领取并提交一条 consider 回顾，形成一个开放关注（不带 contact）。
qlonglong seedOpenConcern(MemoryManager &memory, const QString &input, const QString &note)
{
    static qlonglong serial = 0;
    const auto turn = memory.recordInput(input, policyMeta(QStringLiteral("concern-%1").arg(++serial)), true);
    if (turn <= 0 || !memory.recordQuiet(turn, QStringLiteral("close"))) return 0;
    const auto claim = memory.claimPrivateReflection(12, 60);
    if (claim.value(QStringLiteral("id")).toInteger() != turn
        || claim.value(QStringLiteral("version")).toInt() != 1)
        return 0;
    if (!memory.finishPrivateReflection(turn, 1, QStringLiteral("consider"), note,
                                        QJsonObject{}, QJsonArray{}, QJsonObject{},
                                        QString{}, QJsonObject{}))
        return 0;
    return turn;
}

// 造一条"关注之后主人真实说过的话"（owner evidence）。
qlonglong insertOwnerEvidenceTurn(MemoryManager &memory, const QString &text, int offsetSeconds = 3600)
{
    static qlonglong serial = 0;
    auto meta = policyMeta(QStringLiteral("evidence-%1").arg(++serial));
    meta.receivedAtUtc = QDateTime::currentDateTimeUtc().addSecs(offsetSeconds);
    return memory.recordInput(text, meta, true);
}

// R12-2a：直接造一条 handed_off 的 cognitive_action 行（执行回执证据）。
bool insertHandedOffAction(const QString &dbPath, const QString &actionKey)
{
    const QString conn = QStringLiteral("tst_actionpolicy_handed_%1")
        .arg(QDateTime::currentMSecsSinceEpoch());
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "INSERT INTO cognitive_action(action_key,kind,channel,session_id,payload_json,state) "
                "VALUES(?,'SpontaneousContact','pet','default','{}','handed_off')"));
            q.addBindValue(actionKey);
            ok = q.exec();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

// R12-2a：直接造一条 running 的 open_wake 行（Wake 来源身份真实性校验用）。
qlonglong insertRunningOpenWake(const QString &dbPath, const QString &runId,
                                const QString &scope, qint64 contextEpoch)
{
    const QString conn = QStringLiteral("tst_actionpolicy_wake_%1")
        .arg(QDateTime::currentMSecsSinceEpoch());
    qlonglong id = 0;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "INSERT INTO open_wake(scope_key,run_id,version,state,trigger_reason,created_at,"
                "started_at,current_context_epoch) "
                "VALUES(?,?,1,'running','test',unixepoch(),unixepoch(),?)"));
            q.addBindValue(scope);
            q.addBindValue(runId);
            q.addBindValue(contextEpoch);
            if (q.exec()) id = q.lastInsertId().toLongLong();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return id;
}

// R12-2a：构造 claim JSON（type/text/refs，可选 core）。
QJsonObject purposeClaim(const QString &type, const QString &text, const QStringList &refs,
                         bool core = false)
{
    QJsonArray array;
    for (const auto &ref : refs) array.append(ref);
    return QJsonObject{{QStringLiteral("id"), QStringLiteral("c1")},
                       {QStringLiteral("type"), type},
                       {QStringLiteral("text"), text},
                       {QStringLiteral("refs"), array},
                       {QStringLiteral("core"), core}};
}

QJsonObject selfDesireContact(const QString &summary = QStringLiteral("想和主人说说话"),
                              const QString &message = QStringLiteral("在忙吗"))
{
    return QJsonObject{
        {QStringLiteral("timing"), QStringLiteral("now")},
        {QStringLiteral("reason"), QStringLiteral("自己想到")},
        {QStringLiteral("message"), message},
        {QStringLiteral("motivation"), QJsonObject{{QStringLiteral("type"), QStringLiteral("self_desire")},
                                                   {QStringLiteral("summary"), summary}}},
        {QStringLiteral("claims"), QJsonArray{}}};
}

QJsonObject reportedFactClaim(qlonglong turn, bool core)
{
    return QJsonObject{{QStringLiteral("id"), QStringLiteral("c1")},
                       {QStringLiteral("type"), QStringLiteral("reported_fact")},
                       {QStringLiteral("text"), QStringLiteral("主人说过一件事")},
                       {QStringLiteral("refs"), QJsonArray{QStringLiteral("chat/%1").arg(turn)}},
                       {QStringLiteral("core"), core}};
}

QJsonObject contactWithClaim(const QJsonObject &claim)
{
    auto contact = selfDesireContact();
    contact[QStringLiteral("claims")] = QJsonArray{claim};
    contact[QStringLiteral("core_claim_ids")] = QJsonArray{claim.value(QStringLiteral("id")).toString()};
    return contact;
}

} // namespace

class TestActionPolicy : public QObject
{
    Q_OBJECT
private slots:
    // 1. 纯 self_desire（无 claims、无主人新证据）：allowed，且不调用语义验证器。
    void pureSelfDesireAllowed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        MemoryManager memory(dir.filePath(QStringLiteral("self-desire.db")));
        const auto turn = seedOpenConcern(memory, QStringLiteral("今天有点安静"), QStringLiteral("安静"));
        QVERIFY(turn > 0);

        int verifierCalls = 0;
        ActionPolicy policy(&memory);
        policy.setSemanticVerifier([&verifierCalls](const QString &, const QString &, const QJsonArray &) {
            ++verifierCalls;
            ClaimValidation entailed;
            entailed.result = ClaimValidation::Entailed;
            return entailed;
        });
        const auto decision = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, selfDesireContact());
        QVERIFY2(decision.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(decision.value(QStringLiteral("reason")).toString()));
        QCOMPARE(verifierCalls, 0);
        QCOMPARE(decision.value(QStringLiteral("needs_verifier")).toBool(), false);
        QCOMPARE(decision.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("not_required"));
    }

    // 2. 编造承诺：user_commitment 引用 assistant/主动轮 → deterministic insufficient；
    //    refs 为空 → missing_refs；提醒不能被当作主人原话。
    void fabricatedCommitmentWithheld()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager memory(dir.filePath(QStringLiteral("fabricated.db")));
        ActionPolicy policy(&memory);
        const auto proactiveTurn = memory.recordProactiveMessage(
            Channel::kPet, Channel::kDefaultSession, QStringLiteral("我先睡啦"),
            QStringLiteral("contact"), QStringLiteral("ref/1"));
        QVERIFY(proactiveTurn > 0);

        const QJsonObject assistantRefClaim{
            {QStringLiteral("id"), QStringLiteral("c1")},
            {QStringLiteral("type"), QStringLiteral("user_commitment")},
            {QStringLiteral("text"), QStringLiteral("你答应过我今晚早点睡")},
            {QStringLiteral("refs"), QJsonArray{QStringLiteral("chat/%1").arg(proactiveTurn)}}};
        const auto assistantRef = policy.validateClaimDeterministic(
            Channel::kPet, Channel::kDefaultSession, 0, assistantRefClaim);
        QCOMPARE(static_cast<int>(assistantRef.result),
                 static_cast<int>(ClaimValidation::Insufficient));
        QCOMPARE(assistantRef.reason, QStringLiteral("assistant_expression_not_user_commitment"));

        auto missing = assistantRefClaim;
        missing[QStringLiteral("refs")] = QJsonArray{};
        QCOMPARE(policy.validateClaimDeterministic(Channel::kPet, Channel::kDefaultSession, 0, missing).reason,
                 QStringLiteral("missing_refs"));

        auto reminderRef = assistantRefClaim;
        reminderRef[QStringLiteral("refs")] = QJsonArray{QStringLiteral("reminder/7")};
        QCOMPARE(policy.validateClaimDeterministic(Channel::kPet, Channel::kDefaultSession, 0, reminderRef).reason,
                 QStringLiteral("reminder_not_owner_statement"));

        // 动作核心的确定性失败：整项联系 withheld（reason=deterministic:*）。
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        const auto turn = seedOpenConcern(memory, QStringLiteral("又想起一件事"), QStringLiteral("想起"));
        QVERIFY(turn > 0);
        auto contact = selfDesireContact();
        contact[QStringLiteral("claims")] = QJsonArray{assistantRefClaim};
        const auto decision = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, contact);
        QCOMPARE(decision.value(QStringLiteral("allowed")).toBool(), false);
        QVERIFY2(decision.value(QStringLiteral("reason")).toString().startsWith(
                     QStringLiteral("deterministic:")),
                 qPrintable(decision.value(QStringLiteral("reason")).toString()));
    }

    // 3. 真实但无关的证据：verifier 判 insufficient，核心声称 → withheld；
    //    同一来源若改成纯 self_desire（无 claims）→ allowed。
    void truthfulButUnrelatedInsufficient()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        MemoryManager memory(dir.filePath(QStringLiteral("unrelated.db")));
        const auto turn = seedOpenConcern(memory, QStringLiteral("想聊聊面试"), QStringLiteral("面试"));
        QVERIFY(turn > 0);
        const auto evidence = insertOwnerEvidenceTurn(memory, QStringLiteral("今天天气不错"));
        QVERIFY(evidence > 0);

        int verifierCalls = 0;
        ActionPolicy policy(&memory);
        policy.setSemanticVerifier([&verifierCalls](const QString &, const QString &, const QJsonArray &) {
            ++verifierCalls;
            ClaimValidation insufficient;
            insufficient.result = ClaimValidation::Insufficient;
            insufficient.reason = QStringLiteral("unrelated");
            return insufficient;
        });
        const auto withheld = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, contactWithClaim(reportedFactClaim(evidence, true)));
        QCOMPARE(verifierCalls, 1);
        QCOMPARE(withheld.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(withheld.value(QStringLiteral("reason")).toString(), QStringLiteral("verifier_insufficient"));
        QCOMPARE(withheld.value(QStringLiteral("semantic_status")).toString(), QStringLiteral("withheld"));

        // 去掉事实声称后，同一段自我动机不需要核验即可保留。
        ActionPolicy second(&memory);
        const auto allowed = second.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, selfDesireContact());
        QVERIFY(allowed.value(QStringLiteral("allowed")).toBool());
        QCOMPARE(allowed.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("not_required"));
    }

    // 4. repair 预算：非核心事实判 insufficient 时联系可保留（返回 repair_needed 供调用方修复）；
    //    修复后仍带着无法支持的事实（且已成为动作核心）→ withheld。
    void repairKeepsSpontaneous()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        MemoryManager memory(dir.filePath(QStringLiteral("repair.db")));
        const auto turn = seedOpenConcern(memory, QStringLiteral("想起了上次的约定"), QStringLiteral("约定"));
        QVERIFY(turn > 0);
        const auto evidence = insertOwnerEvidenceTurn(memory, QStringLiteral("随手拍的照片"));
        QVERIFY(evidence > 0);

        int verifierCalls = 0;
        ActionPolicy policy(&memory);
        policy.setSemanticVerifier([&verifierCalls](const QString &, const QString &, const QJsonArray &) {
            ++verifierCalls;
            ClaimValidation insufficient;
            insufficient.result = ClaimValidation::Insufficient;
            insufficient.reason = QStringLiteral("unrelated");
            return insufficient;
        });
        // 非核心：夹带的事实可以按 repair 删除，联系本身保留。
        auto dangling = reportedFactClaim(evidence, false);
        auto contact = selfDesireContact();
        contact[QStringLiteral("claims")] = QJsonArray{dangling};
        const auto repairable = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, contact);
        QVERIFY2(repairable.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(repairable.value(QStringLiteral("reason")).toString()));
        QCOMPARE(repairable.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("repair_needed"));
        QCOMPARE(repairable.value(QStringLiteral("repairable_claims")).toArray().size(), 1);
        QCOMPARE(verifierCalls, 1);

        // 修复后仍保留该事实，且 reason/message 依赖它（core=true）→ withheld。
        ActionPolicy second(&memory);
        second.setSemanticVerifier([&verifierCalls](const QString &, const QString &, const QJsonArray &) {
            ++verifierCalls;
            ClaimValidation insufficient;
            insufficient.result = ClaimValidation::Insufficient;
            insufficient.reason = QStringLiteral("still_unrelated");
            return insufficient;
        });
        const auto stillBad = second.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, contactWithClaim(reportedFactClaim(evidence, true)));
        QCOMPARE(stillBad.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(stillBad.value(QStringLiteral("reason")).toString(), QStringLiteral("verifier_insufficient"));
    }

    // 5. verifier 不可用：有事实 claims 时保守 withheld；无 claims 的纯 self_desire 不受影响。
    void verifierUnavailableConservative()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        MemoryManager memory(dir.filePath(QStringLiteral("unavailable.db")));
        const auto turn = seedOpenConcern(memory, QStringLiteral("想起一件事"), QStringLiteral("事"));
        QVERIFY(turn > 0);
        const auto evidence = insertOwnerEvidenceTurn(memory, QStringLiteral("路上看到一只猫"));
        QVERIFY(evidence > 0);

        ActionPolicy policy(&memory); // 不注入 verifier：等价于 provider 不可用
        const auto withheld = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, contactWithClaim(reportedFactClaim(evidence, true)));
        QCOMPARE(withheld.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(withheld.value(QStringLiteral("reason")).toString(), QStringLiteral("verifier_unavailable"));
        QCOMPARE(withheld.value(QStringLiteral("semantic_status")).toString(), QStringLiteral("withheld"));

        const auto allowed = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, turn, selfDesireContact());
        QVERIFY(allowed.value(QStringLiteral("allowed")).toBool());
    }

    // 6. 收口机制已删除：刚 close/defer 过也不抑制自发联系。
    void closureDoesNotSuppressSpontaneous()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        MemoryManager memory(dir.filePath(QStringLiteral("closure.db")));
        // seedOpenConcern 会把来源轮次 recordQuiet("close")：收口确实发生过。
        const auto turn = seedOpenConcern(memory, QStringLiteral("说了晚安"), QStringLiteral("晚安"));
        QVERIFY(turn > 0);
        QCOMPARE(memory.conversationPhase(policyMeta(QStringLiteral("closure-phase"))),
                 QStringLiteral("close"));

        ActionPolicy policy(&memory);
        const auto decision = policy.evaluateSpontaneousContactDeterministic(
            Channel::kPet, Channel::kDefaultSession, turn, selfDesireContact());
        QVERIFY2(decision.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(decision.value(QStringLiteral("reason")).toString()));
        QCOMPARE(decision.value(QStringLiteral("reason")).toString(), QStringLiteral("ok"));
    }

    // 7. activity_share 必须引用 completed 且有产物的活动。
    void activityShareNeedsCompletedArtifact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PolicyIdentityGuard identity;
        QVERIFY(identity.enable());
        MemoryManager memory(dir.filePath(QStringLiteral("activity.db")));
        const auto activityId = seedLegacyActiveThought(
            dir.filePath(QStringLiteral("activity.db")),
            QStringLiteral("写一首关于雨的诗"), QStringLiteral("你想做点什么"));
        const int version = 0;
        QVERIFY(activityId > 0);

        ActionPolicy policy(&memory);
        QJsonObject motivation{{QStringLiteral("type"), QStringLiteral("activity_share")},
                               {QStringLiteral("ref"), QStringLiteral("thought/%1").arg(activityId)},
                               {QStringLiteral("summary"), QStringLiteral("写好了想分享")}};
        auto contact = selfDesireContact(QStringLiteral("写好了"), QStringLiteral("写了一首短诗"));
        contact[QStringLiteral("motivation")] = motivation;
        const auto notCompleted = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, activityId, contact);
        QCOMPARE(notCompleted.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(notCompleted.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("motivation_artifact_not_completed"));

        QVERIFY(memory.finishThoughtReview(activityId, version,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写好了两句")},
                        {QStringLiteral("draft"), QStringLiteral("雨落在窗台")}}));
        const auto completed = policy.evaluateSpontaneousContact(
            Channel::kPet, Channel::kDefaultSession, activityId, contact);
        QVERIFY2(completed.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(completed.value(QStringLiteral("reason")).toString()));
    }

    // ===== R12-2a：ActionOrigin 身份/版本/隔离/action_key =====
    void actionOriginIdentityIsolated()
    {
        const auto concern = ActionOrigin::forConcern(5, 1, QStringLiteral("owner"),
                                                      Channel::kPet, Channel::kDefaultSession);
        const auto reflection = ActionOrigin::forReflection(5, 1, QStringLiteral("owner"),
                                                            Channel::kPet, Channel::kDefaultSession);
        const auto thought = ActionOrigin::forThought(5, 2, QStringLiteral("owner"),
                                                      Channel::kPet, Channel::kDefaultSession);
        const auto activity = ActionOrigin::forActivity(5, 2, QStringLiteral("owner"),
                                                        Channel::kPet, Channel::kDefaultSession);
        QVERIFY(concern.isValid());
        QVERIFY(reflection.isValid());
        QVERIFY(thought.isValid());
        QVERIFY(activity.isValid());
        // 同一整数 id 不同 kind 不串线：ref 与 action_key 都带来源前缀。
        QCOMPARE(concern.ref, QStringLiteral("concern/5"));
        QCOMPARE(reflection.ref, QStringLiteral("reflection/5"));
        QCOMPARE(thought.ref, QStringLiteral("thought/5"));
        QCOMPARE(activity.ref, QStringLiteral("activity/5"));
        QCOMPARE(concern.actionKey(QStringLiteral("contact")), QStringLiteral("concern:5:1:contact"));
        QCOMPARE(reflection.actionKey(QStringLiteral("contact")),
                 QStringLiteral("reflection:5:1:contact"));
        QCOMPARE(thought.actionKey(QStringLiteral("activity_share")),
                 QStringLiteral("thought:5:2:share"));
        QCOMPARE(activity.actionKey(QStringLiteral("activity_share")),
                 QStringLiteral("activity:5:2:share"));
        // 不支持的组合返回空 key（thought 不能直接发 contact）。
        QVERIFY(concern.actionKey(QStringLiteral("activity_share")).isEmpty());
        QVERIFY(thought.actionKey(QStringLiteral("contact")).isEmpty());
        // action_key 稳定：同身份派生结果一致。
        QCOMPARE(concern.actionKey(QStringLiteral("contact")),
                 ActionOrigin::forConcern(5, 1, QStringLiteral("owner"), Channel::kPet,
                                          Channel::kDefaultSession)
                     .actionKey(QStringLiteral("contact")));
        // JSON 往返保留身份字段。
        ActionOrigin parsed;
        QString error;
        QVERIFY2(ActionOrigin::fromJson(concern.toJson(), &parsed, &error), qPrintable(error));
        QCOMPARE(static_cast<int>(parsed.kind),
                 static_cast<int>(ActionOrigin::Concern));
        QCOMPARE(parsed.ref, concern.ref);
        QCOMPARE(parsed.id, qlonglong(5));
        QCOMPARE(parsed.version, 1);
        QCOMPARE(parsed.targetChannel, QString(Channel::kPet));
        QCOMPARE(parsed.targetSession, QString(Channel::kDefaultSession));

        // 非法身份：负数 id / 伪造 chat ref / kind 与 ref 不一致 / 空 ref。
        ActionOrigin out;
        QJsonObject bad = concern.toJson();
        bad[QStringLiteral("id")] = -1;
        error.clear();
        QVERIFY(!ActionOrigin::fromJson(bad, &out, &error));
        QCOMPARE(error, QStringLiteral("invalid_origin_id"));
        bad = concern.toJson();
        bad[QStringLiteral("ref")] = QStringLiteral("chat/5");
        bad.remove(QStringLiteral("id"));
        error.clear();
        QVERIFY(!ActionOrigin::fromJson(bad, &out, &error));
        QCOMPARE(error, QStringLiteral("invalid_origin_ref"));
        bad = concern.toJson();
        bad[QStringLiteral("kind")] = QStringLiteral("thought");
        error.clear();
        QVERIFY(!ActionOrigin::fromJson(bad, &out, &error));
        QCOMPARE(error, QStringLiteral("invalid_origin_ref"));
        // 空 ref 且没有 id：没有可定位的来源身份。
        bad = concern.toJson();
        bad.remove(QStringLiteral("id"));
        bad[QStringLiteral("ref")] = QString();
        error.clear();
        QVERIFY(!ActionOrigin::fromJson(bad, &out, &error));
        QCOMPARE(error, QStringLiteral("invalid_origin_id"));
        // wake：缺 run_id / 缺 scope / 非 owner scope 都拒绝。
        const auto wake = ActionOrigin::forWake(3, 1, QStringLiteral("run-a"),
                                                QStringLiteral("owner"), 7,
                                                Channel::kPet, Channel::kDefaultSession);
        QVERIFY(wake.isValid());
        QCOMPARE(wake.actionKey(QStringLiteral("contact")), QStringLiteral("wake:3:1:contact"));
        QJsonObject wakeJson = wake.toJson();
        wakeJson[QStringLiteral("run_id")] = QString();
        error.clear();
        QVERIFY(!ActionOrigin::fromJson(wakeJson, &out, &error));
        QCOMPARE(error, QStringLiteral("missing_run_id"));
        wakeJson = wake.toJson();
        wakeJson[QStringLiteral("scope")] = QString();
        error.clear();
        QVERIFY(!ActionOrigin::fromJson(wakeJson, &out, &error));
        QCOMPARE(error, QStringLiteral("missing_scope"));
        const auto otherScope = ActionOrigin::forReflection(8, 1, QStringLiteral("other"),
                                                            Channel::kPet, Channel::kDefaultSession);
        QVERIFY(!otherScope.isValid());
        // 格式校验兜底。
        QVERIFY(ActionOrigin::isValidActionKeyFormat(QStringLiteral("reflection:5:1:contact")));
        QVERIFY(ActionOrigin::isValidActionKeyFormat(QStringLiteral("activity:5:2:share")));
        QVERIFY(!ActionOrigin::isValidActionKeyFormat(QStringLiteral("reflection:-5:1:contact")));
        QVERIFY(!ActionOrigin::isValidActionKeyFormat(QStringLiteral("concern:5:0:contact")));
        QVERIFY(!ActionOrigin::isValidActionKeyFormat(QStringLiteral("thought:5:1:contact")));
    }

    // R12-2a：HistoricalFact 不因早于本次 wake stale；NewEvidence 是唯一保留 stale_ref 的路径。
    void evidencePurposeHistoricalAndNewEvidence()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager memory(dir.filePath(QStringLiteral("purpose-history.db")));
        const auto concernTurn = seedOpenConcern(memory, QStringLiteral("想起一件事"),
                                                 QStringLiteral("事"));
        QVERIFY(concernTurn > 0);
        const auto earlier = insertOwnerEvidenceTurn(memory, QStringLiteral("昨天说过要早点睡"), -3600);
        const auto later = insertOwnerEvidenceTurn(memory, QStringLiteral("今天去了海边"), 3600);
        QVERIFY(earlier > 0);
        QVERIFY(later > 0);
        ActionPolicy policy(&memory);
        const auto origin = ActionOrigin::forConcern(concernTurn, 1, QStringLiteral("owner"),
                                                     Channel::kPet, Channel::kDefaultSession);
        QVERIFY(origin.isValid());

        // HistoricalFact：真实历史来源早于本次关注时间也不算 stale。
        const auto historical = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::HistoricalFact,
            purposeClaim(QStringLiteral("historical_fact"), QStringLiteral("主人以前说过要早点睡"),
                         {QStringLiteral("chat/%1").arg(earlier)}));
        QCOMPARE(static_cast<int>(historical.result), static_cast<int>(ClaimValidation::Entailed));
        QCOMPARE(historical.purpose, QStringLiteral("historical_fact"));
        // NewEvidence：早于来源 → stale_ref（旧 reported_fact 同语义）。
        const auto stale = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::NewEvidence,
            purposeClaim(QStringLiteral("new_evidence"), QStringLiteral("主人今天说要去海边"),
                         {QStringLiteral("chat/%1").arg(earlier)}));
        QCOMPARE(static_cast<int>(stale.result), static_cast<int>(ClaimValidation::Insufficient));
        QCOMPARE(stale.reason, QStringLiteral("stale_ref"));
        const auto legacyStale = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::NewEvidence,
            purposeClaim(QStringLiteral("reported_fact"), QStringLiteral("主人今天说要去海边"),
                         {QStringLiteral("chat/%1").arg(earlier)}));
        QCOMPARE(legacyStale.reason, QStringLiteral("stale_ref"));
        // 晚于来源 → 通过。
        const auto fresh = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::NewEvidence,
            purposeClaim(QStringLiteral("new_evidence"), QStringLiteral("主人今天说要去海边"),
                         {QStringLiteral("chat/%1").arg(later)}));
        QCOMPARE(static_cast<int>(fresh.result), static_cast<int>(ClaimValidation::Entailed));
        // 用途由服务端判定：claim 自填冲突 purpose → 拒绝。
        auto mismatched = purposeClaim(QStringLiteral("historical_fact"),
                                       QStringLiteral("主人以前说过要早点睡"),
                                       {QStringLiteral("chat/%1").arg(earlier)});
        mismatched[QStringLiteral("purpose")] = QStringLiteral("current_state");
        const auto mismatch = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin,
            EvidencePurpose::HistoricalFact, mismatched);
        QCOMPARE(mismatch.reason, QStringLiteral("purpose_mismatch"));
    }

    // R12-2a：CurrentState 不能靠历史相似记录；UserCommitment 仅真实用户原话。
    void evidencePurposeCurrentStateAndCommitment()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager memory(dir.filePath(QStringLiteral("purpose-current.db")));
        const auto concernTurn = seedOpenConcern(memory, QStringLiteral("想起一件事"),
                                                 QStringLiteral("事"));
        QVERIFY(concernTurn > 0);
        const auto earlier = insertOwnerEvidenceTurn(memory, QStringLiteral("昨天说过要早点睡"), -3600);
        const auto later = insertOwnerEvidenceTurn(memory, QStringLiteral("今天去了海边"), 3600);
        QVERIFY(earlier > 0);
        QVERIFY(later > 0);
        const auto proactiveTurn = memory.recordProactiveMessage(
            Channel::kPet, Channel::kDefaultSession, QStringLiteral("我先睡啦"),
            QStringLiteral("contact"), QStringLiteral("ref/1"));
        QVERIFY(proactiveTurn > 0);
        ActionPolicy policy(&memory);
        const auto origin = ActionOrigin::forConcern(concernTurn, 1, QStringLiteral("owner"),
                                                     Channel::kPet, Channel::kDefaultSession);
        // 当前状态：只有历史证据（早于来源）→ 不能确认。
        const auto staleState = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::CurrentState,
            purposeClaim(QStringLiteral("current_state"), QStringLiteral("她正在忙"),
                         {QStringLiteral("chat/%1").arg(earlier)}));
        QCOMPARE(staleState.reason, QStringLiteral("current_state_not_current"));
        // 较新证据 → 通过。
        const auto currentState = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::CurrentState,
            purposeClaim(QStringLiteral("current_state"), QStringLiteral("她正在忙"),
                         {QStringLiteral("chat/%1").arg(later)}));
        QCOMPARE(static_cast<int>(currentState.result),
                 static_cast<int>(ClaimValidation::Entailed));
        // 明确 unknown：允许以 unknown 通过，而不是当历史事实确认。
        const auto unknown = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::CurrentState,
            purposeClaim(QStringLiteral("current_state"), QStringLiteral("她现在可能还在路上"),
                         {}));
        QCOMPARE(unknown.unknown, true);
        QCOMPARE(unknown.reason, QStringLiteral("explicit_unknown"));

        // UserCommitment：历史承诺不因早于 wake stale（真实用户原话仍然成立）。
        const auto commitment = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::UserCommitment,
            purposeClaim(QStringLiteral("user_commitment"), QStringLiteral("主人答应过要早点睡"),
                         {QStringLiteral("chat/%1").arg(earlier)}));
        QCOMPARE(static_cast<int>(commitment.result), static_cast<int>(ClaimValidation::Entailed));
        // 助手主动消息/旧回复不能当承诺。
        const auto assistant = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::UserCommitment,
            purposeClaim(QStringLiteral("user_commitment"), QStringLiteral("主人答应过要早点睡"),
                         {QStringLiteral("chat/%1").arg(proactiveTurn)}));
        QCOMPARE(assistant.reason, QStringLiteral("assistant_expression_not_user_commitment"));
        // 提醒是"已安排的执行"，不是用户原话。
        const auto reminder = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::UserCommitment,
            purposeClaim(QStringLiteral("user_commitment"), QStringLiteral("主人答应过要早点睡"),
                         {QStringLiteral("reminder/7")}));
        QCOMPARE(reminder.reason, QStringLiteral("reminder_not_owner_statement"));
    }

    // R12-2a：ExecutionResult 必须真实回执；ArtifactResult 必须真实可核验成果。
    void evidencePurposeExecutionAndArtifact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("purpose-exec.db"));
        MemoryManager memory(path);
        ActionPolicy policy(&memory);
        const auto origin = ActionOrigin::forReflection(1, 1, QStringLiteral("owner"),
                                                        Channel::kPet, Channel::kDefaultSession);
        // 执行回执：只有 handed_off 的 action 才可作数。
        const QString actionKey = QStringLiteral("reflection:1:1:contact");
        QVERIFY(insertHandedOffAction(path, actionKey));
        const auto delivered = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::ExecutionResult,
            purposeClaim(QStringLiteral("execution_result"), QStringLiteral("她已经把消息发出去了"),
                         {QStringLiteral("delivery/%1").arg(actionKey)}));
        QCOMPARE(static_cast<int>(delivered.result),
                 static_cast<int>(ClaimValidation::Entailed));
        const auto missingDelivery = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::ExecutionResult,
            purposeClaim(QStringLiteral("execution_result"), QStringLiteral("她已经把消息发出去了"),
                         {QStringLiteral("delivery/reflection:1:9:contact")}));
        QCOMPARE(missingDelivery.reason, QStringLiteral("execution_result_not_found"));

        // 成果：legacy thought/<id>，completed 且有真实 draft/版本/哈希。
        const auto activityId = seedLegacyActiveThought(
            path, QStringLiteral("写一首关于雨的诗"), QStringLiteral("你想做点什么"));
        QVERIFY(activityId > 0);
        const auto unfinished = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::ArtifactResult,
            purposeClaim(QStringLiteral("artifact_result"), QStringLiteral("她写了首诗"),
                         {QStringLiteral("thought/%1").arg(activityId)}));
        QCOMPARE(unfinished.reason, QStringLiteral("artifact_not_completed"));
        QVERIFY(memory.finishThoughtReview(
            activityId, 0,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写好了两句")},
                        {QStringLiteral("draft"), QStringLiteral("雨落在窗台")}}));
        const auto thoughtOrigin = ActionOrigin::forThought(
            activityId, 1, QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession);
        const auto artifact = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, thoughtOrigin,
            EvidencePurpose::ArtifactResult,
            purposeClaim(QStringLiteral("artifact_result"), QStringLiteral("她写了首诗"),
                         {QStringLiteral("thought/%1").arg(activityId)}));
        QCOMPARE(static_cast<int>(artifact.result), static_cast<int>(ClaimValidation::Entailed));
        // 成果版本必须是真实版本，不能自造。
        auto wrongVersion = purposeClaim(QStringLiteral("artifact_result"),
                                         QStringLiteral("她写了首诗"),
                                         {QStringLiteral("thought/%1").arg(activityId)});
        wrongVersion[QStringLiteral("artifact_version")] = 2;
        const auto versionMismatch = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, thoughtOrigin,
            EvidencePurpose::ArtifactResult, wrongVersion);
        QCOMPARE(versionMismatch.reason, QStringLiteral("artifact_version_mismatch"));

        // internal activity：completed 成果可核验；active 的内部想法不能当成果。
        QString error;
        const auto created = memory.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("整理旧照片"), QStringLiteral("按年份分组"),
            QStringLiteral("foreground"), QStringLiteral("chat/101"),
            QStringLiteral("run-r12-2a-artifact"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString internalRef = created.value(QStringLiteral("ref")).toString();
        const qlonglong internalId = created.value(QStringLiteral("id")).toInteger();
        const auto finished = memory.finishActivity(
            internalRef, created.value(QStringLiteral("version")).toInt(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("整理好了")},
                        {QStringLiteral("draft"), QStringLiteral("春天在海边")},
                        {QStringLiteral("activity_kind"), QStringLiteral("writing")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        const int artifactVersion = finished.value(QStringLiteral("artifact_version")).toInt();
        QVERIFY(artifactVersion > 0);
        const auto internalOrigin = ActionOrigin::forActivity(
            internalId, artifactVersion, QStringLiteral("owner"),
            Channel::kPet, Channel::kDefaultSession);
        const auto internalArtifact = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, internalOrigin,
            EvidencePurpose::ArtifactResult,
            purposeClaim(QStringLiteral("artifact_result"), QStringLiteral("她整理好了照片"),
                         {QStringLiteral("activity/%1").arg(internalId)}));
        QCOMPARE(static_cast<int>(internalArtifact.result),
                 static_cast<int>(ClaimValidation::Entailed));
        // 未完成的 internal 想法（active）不能当成果。
        const auto active = memory.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("想一个句子"), QStringLiteral("想到再写"),
            QStringLiteral("foreground"), QStringLiteral("chat/102"),
            QStringLiteral("run-r12-2a-active"), &error);
        QVERIFY2(!active.isEmpty(), qPrintable(error));
        const QString activeRef = active.value(QStringLiteral("ref")).toString();
        const auto activeOrigin = ActionOrigin::forActivity(
            active.value(QStringLiteral("id")).toInteger(), 1, QStringLiteral("owner"),
            Channel::kPet, Channel::kDefaultSession);
        const auto activeArtifact = policy.validateClaimForPurpose(
            Channel::kPet, Channel::kDefaultSession, activeOrigin,
            EvidencePurpose::ArtifactResult,
            purposeClaim(QStringLiteral("artifact_result"), QStringLiteral("她想好了一个句子"),
                         {activeRef}));
        QCOMPARE(activeArtifact.reason, QStringLiteral("artifact_not_completed"));
        // 动机层同样拒绝内部想法；新旧 ActivityRef 的 completed 成果都可分享。
        QCOMPARE(policy.validateMotivation(
                     Channel::kPet, Channel::kDefaultSession, 0,
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("activity_share")},
                                 {QStringLiteral("ref"), activeRef},
                                 {QStringLiteral("summary"), QStringLiteral("想分享")}}),
                 QStringLiteral("motivation_artifact_not_completed"));
        QCOMPARE(policy.validateMotivation(
                     Channel::kPet, Channel::kDefaultSession, 0,
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("activity_share")},
                                 {QStringLiteral("ref"), internalRef},
                                 {QStringLiteral("summary"), QStringLiteral("想分享")}}),
                 QString());
        QCOMPARE(policy.validateMotivation(
                     Channel::kPet, Channel::kDefaultSession, 0,
                     QJsonObject{{QStringLiteral("type"), QStringLiteral("activity_share")},
                                 {QStringLiteral("ref"), QStringLiteral("thought/%1").arg(activityId)},
                                 {QStringLiteral("summary"), QStringLiteral("想分享")}}),
                 QString());
    }

    // R12-2a：统一核验入口 validateActionProposal（contact）：身份/action_key/target/
    // purposes/coverage；不发送、不落库。
    void validateActionProposalContactUsesPurpose()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager memory(dir.filePath(QStringLiteral("proposal-contact.db")));
        const auto concernTurn = seedOpenConcern(memory, QStringLiteral("想起一件事"),
                                                 QStringLiteral("事"));
        QVERIFY(concernTurn > 0);
        const auto later = insertOwnerEvidenceTurn(memory, QStringLiteral("今天去了海边"), 3600);
        QVERIFY(later > 0);
        ActionPolicy policy(&memory);
        const auto origin = ActionOrigin::forConcern(concernTurn, 1, QStringLiteral("owner"),
                                                     Channel::kPet, Channel::kDefaultSession);
        const QString expectedKey = QStringLiteral("concern:%1:1:contact").arg(concernTurn);
        QJsonObject proposal{
            {QStringLiteral("origin"), origin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("contact")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("message"), QStringLiteral("在忙吗")},
                         {QStringLiteral("reason"), QStringLiteral("自己想到")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        QString error;
        const auto allowed = policy.validateActionProposal(proposal, &error);
        QVERIFY2(allowed.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(allowed.value(QStringLiteral("reason")).toString()));
        QCOMPARE(allowed.value(QStringLiteral("action_key")).toString(), expectedKey);
        QCOMPARE(allowed.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("not_required"));
        QCOMPARE(allowed.value(QStringLiteral("origin")).toObject()
                     .value(QStringLiteral("id")).toInteger(), concernTurn);
        QCOMPARE(error, QString());

        // 来源真实身份：version 必须等于 private_reflection 版本。
        auto badVersion = proposal;
        auto badOrigin = origin.toJson();
        badOrigin[QStringLiteral("version")] = 2;
        badVersion[QStringLiteral("origin")] = badOrigin;
        QCOMPARE(policy.validateActionProposal(badVersion, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("origin_version_mismatch"));
        // 伪造一个不存在的关注轮次（concernTurn）不能通过来源身份校验。
        auto fakeOrigin = origin.toJson();
        fakeOrigin[QStringLiteral("id")] = 999999;
        fakeOrigin[QStringLiteral("ref")] = QStringLiteral("concern/999999");
        auto fakeProposal = proposal;
        fakeProposal[QStringLiteral("origin")] = fakeOrigin;
        QCOMPARE(policy.validateActionProposal(fakeProposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("origin_not_found"));
        // action_key 必须由 origin+kind 派生。
        auto badKey = proposal;
        badKey[QStringLiteral("action_key")] =
            QStringLiteral("reflection:%1:1:contact").arg(concernTurn);
        QCOMPARE(policy.validateActionProposal(badKey, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("action_key_mismatch"));
        // target 必须落在主人私聊域。
        const auto groupOrigin = ActionOrigin::forConcern(
            concernTurn, 1, QStringLiteral("owner"), Channel::kQQGroup,
            QStringLiteral("group:123"));
        auto groupProposal = proposal;
        groupProposal[QStringLiteral("origin")] = groupOrigin.toJson();
        QCOMPARE(policy.validateActionProposal(groupProposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("target_not_visible"));
        // claims 用途：历史事实（真实旧来源）通过但需要语义核验；当前状态用旧证据被拒绝。
        auto factClaim = purposeClaim(QStringLiteral("historical_fact"),
                                      QStringLiteral("主人以前说想去海边"),
                                      {QStringLiteral("chat/%1").arg(later)}, true);
        auto withClaim = proposal;
        withClaim[QStringLiteral("claims")] = QJsonArray{factClaim};
        const auto verified = policy.validateActionProposal(withClaim, &error);
        QVERIFY2(verified.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(verified.value(QStringLiteral("reason")).toString()));
        QCOMPARE(verified.value(QStringLiteral("semantic_status")).toString(),
                 QStringLiteral("unverified"));
        QCOMPARE(verified.value(QStringLiteral("claims")).toArray().first().toObject()
                     .value(QStringLiteral("purpose")).toString(),
                 QStringLiteral("historical_fact"));
        // 内部状态不能成为动作核心。
        auto internalCore = purposeClaim(QStringLiteral("internal_state"),
                                         QStringLiteral("她想多说两句"),
                                         {QStringLiteral("reflection/%1").arg(concernTurn)}, true);
        auto internalProposal = proposal;
        internalProposal[QStringLiteral("claims")] = QJsonArray{internalCore};
        QCOMPARE(policy.validateActionProposal(internalProposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("internal_state_core"));
        // 内部状态不能把外部事实伪装成"她自己的想法"。
        auto internalFact = purposeClaim(QStringLiteral("internal_state"),
                                         QStringLiteral("主人答应过今晚早点睡"), {});
        auto internalFactProposal = proposal;
        internalFactProposal[QStringLiteral("claims")] = QJsonArray{internalFact};
        QCOMPARE(policy.validateActionProposal(internalFactProposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("internal_state_not_external"));
    }

    // R12-2a：activity_share 统一核验：新旧 ActivityRef、真实成果、内部想法/未完成拒绝。
    void validateActionProposalActivityShareUsesArtifact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("proposal-share.db"));
        MemoryManager memory(path);
        ActionPolicy policy(&memory);
        // legacy thought/<id>：completed 后有真实成果。
        const auto legacyId = seedLegacyActiveThought(
            path, QStringLiteral("写一首关于雨的诗"), QStringLiteral("你想做点什么"));
        QVERIFY(legacyId > 0);
        QVERIFY(memory.finishThoughtReview(
            legacyId, 0,
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("写好了两句")},
                        {QStringLiteral("draft"), QStringLiteral("雨落在窗台")}}));
        const auto legacyOrigin = ActionOrigin::forThought(
            legacyId, 1, QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession);
        QJsonObject legacyProposal{
            {QStringLiteral("origin"), legacyOrigin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("activity_share")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("draft"), QStringLiteral("雨落在窗台")},
                         {QStringLiteral("message"), QStringLiteral("写好了")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        QString error;
        const auto legacyAllowed = policy.validateActionProposal(legacyProposal, &error);
        QVERIFY2(legacyAllowed.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(legacyAllowed.value(QStringLiteral("reason")).toString()));
        QCOMPARE(legacyAllowed.value(QStringLiteral("action_key")).toString(),
                 QStringLiteral("thought:%1:1:share").arg(legacyId));
        // 分享正文必须与服务端成果一致（哈希/正文核对）。
        auto wrongDraft = legacyProposal;
        wrongDraft[QStringLiteral("payload")] =
            QJsonObject{{QStringLiteral("draft"), QStringLiteral("另一个版本")}};
        QCOMPARE(policy.validateActionProposal(wrongDraft, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("artifact_hash_mismatch"));
        // 成果版本必须是真实版本。
        auto wrongVersion = legacyProposal;
        auto badOrigin = legacyOrigin.toJson();
        badOrigin[QStringLiteral("version")] = 2;
        wrongVersion[QStringLiteral("origin")] = badOrigin;
        QCOMPARE(policy.validateActionProposal(wrongVersion, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("artifact_version_mismatch"));

        // internal activity/<id>：completed 成果同样可分享。
        const auto created = memory.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("海边速写"), QStringLiteral("画出海面"),
            QStringLiteral("foreground"), QStringLiteral("chat/201"),
            QStringLiteral("run-r12-2a-share"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString internalRef = created.value(QStringLiteral("ref")).toString();
        const auto finished = memory.finishActivity(
            internalRef, created.value(QStringLiteral("version")).toInt(),
            QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                        {QStringLiteral("reflection"), QStringLiteral("画完了")},
                        {QStringLiteral("draft"), QStringLiteral("海面泛着光")},
                        {QStringLiteral("activity_kind"), QStringLiteral("writing")}},
            &error);
        QVERIFY2(!finished.isEmpty(), qPrintable(error));
        const int artifactVersion = finished.value(QStringLiteral("artifact_version")).toInt();
        const auto internalOrigin = ActionOrigin::forActivity(
            created.value(QStringLiteral("id")).toInteger(), artifactVersion,
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession);
        QJsonObject internalProposal{
            {QStringLiteral("origin"), internalOrigin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("activity_share")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("draft"), QStringLiteral("海面泛着光")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        const auto internalAllowed = policy.validateActionProposal(internalProposal, &error);
        QVERIFY2(internalAllowed.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(internalAllowed.value(QStringLiteral("reason")).toString()));
        QCOMPARE(internalAllowed.value(QStringLiteral("action_key")).toString(),
                 QStringLiteral("activity:%1:%2:share")
                     .arg(created.value(QStringLiteral("id")).toInteger())
                     .arg(artifactVersion));

        // 未完成目标/内部想法不能当成果分享。
        const auto active = memory.createActivity(
            QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession,
            QStringLiteral("想一个句子"), QStringLiteral("想到再写"),
            QStringLiteral("foreground"), QStringLiteral("chat/202"),
            QStringLiteral("run-r12-2a-share-active"), &error);
        QVERIFY2(!active.isEmpty(), qPrintable(error));
        const auto activeOrigin = ActionOrigin::forActivity(
            active.value(QStringLiteral("id")).toInteger(), 1, QStringLiteral("owner"),
            Channel::kPet, Channel::kDefaultSession);
        QJsonObject activeProposal{
            {QStringLiteral("origin"), activeOrigin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("activity_share")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("draft"), QStringLiteral("还没想好")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        QCOMPARE(policy.validateActionProposal(activeProposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("artifact_not_completed"));
        // 不存在的成果 id → artifact_not_found。
        const auto missingOrigin = ActionOrigin::forActivity(
            999999, 1, QStringLiteral("owner"), Channel::kPet, Channel::kDefaultSession);
        QJsonObject missingProposal{
            {QStringLiteral("origin"), missingOrigin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("activity_share")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("draft"), QStringLiteral("不存在")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        QCOMPARE(policy.validateActionProposal(missingProposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("artifact_not_found"));
    }

    // R12-2a：coverage 核验 —— claims=[] 也不能直接放行；语义 coverage 至多一次。
    void validateActionProposalCoverageEvenWhenClaimsEmpty()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MemoryManager memory(dir.filePath(QStringLiteral("proposal-coverage.db")));
        const auto concernTurn = seedOpenConcern(memory, QStringLiteral("想起一件事"),
                                                 QStringLiteral("事"));
        QVERIFY(concernTurn > 0);
        ActionPolicy policy(&memory);
        const auto origin = ActionOrigin::forConcern(concernTurn, 1, QStringLiteral("owner"),
                                                     Channel::kPet, Channel::kDefaultSession);
        QJsonObject cleanProposal{
            {QStringLiteral("origin"), origin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("contact")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("message"), QStringLiteral("在忙吗")},
                         {QStringLiteral("reason"), QStringLiteral("自己想到")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        QString error;
        QVERIFY(policy.validateActionProposal(cleanProposal, &error)
                    .value(QStringLiteral("allowed")).toBool());
        // claims 为空但 message 里有未声明的外部事实断言 → 不能放行。
        auto undeclaredFact = cleanProposal;
        undeclaredFact[QStringLiteral("payload")] =
            QJsonObject{{QStringLiteral("message"), QStringLiteral("你上次说过要早点睡")},
                        {QStringLiteral("reason"), QStringLiteral("自己想到")}};
        const auto factBlocked = policy.validateActionProposal(undeclaredFact, &error);
        QCOMPARE(factBlocked.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(factBlocked.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("undeclared_external_fact"));
        QCOMPARE(factBlocked.value(QStringLiteral("coverage")).toObject()
                     .value(QStringLiteral("complete")).toBool(), false);
        // 自然语言里出现的显式外部 ref 也必须被 declared claims 覆盖。
        auto undeclaredRef = cleanProposal;
        undeclaredRef[QStringLiteral("payload")] =
            QJsonObject{{QStringLiteral("message"), QStringLiteral("之前聊到 chat/1 那件事")}};
        QCOMPARE(policy.validateActionProposal(undeclaredRef, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("undeclared_reference"));
        // 即使 claims=[]，注入的语义 coverage verifier 也会被调用一次并能否决。
        int verifierCalls = 0;
        ActionPolicy semantic(&memory);
        semantic.setCoverageVerifier(
            [&verifierCalls](const QString &, const QString &, const QJsonArray &,
                             const ActionOrigin &) {
                ++verifierCalls;
                CoverageCheck check;
                check.complete = false;
                check.reason = QStringLiteral("claim_coverage_incomplete");
                return check;
            });
        const auto semanticBlocked = semantic.validateActionProposal(cleanProposal, &error);
        QCOMPARE(verifierCalls, 1);
        QCOMPARE(semanticBlocked.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(semanticBlocked.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("claim_coverage_incomplete"));
        // 语义 coverage 通过：allowed 且 coverage.semantic=true（单次动作内至多一次）。
        int passCalls = 0;
        ActionPolicy semanticOk(&memory);
        semanticOk.setCoverageVerifier(
            [&passCalls](const QString &, const QString &, const QJsonArray &,
                         const ActionOrigin &) {
                ++passCalls;
                return CoverageCheck{};
            });
        const auto semanticAllowed = semanticOk.validateActionProposal(cleanProposal, &error);
        QCOMPARE(passCalls, 1);
        QVERIFY2(semanticAllowed.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(semanticAllowed.value(QStringLiteral("reason")).toString()));
        QCOMPARE(semanticAllowed.value(QStringLiteral("coverage")).toObject()
                     .value(QStringLiteral("semantic")).toBool(), true);
        // 确定性检测先于语义核验：确定性已判 incomplete 时不浪费一次模型调用。
        int skippedCalls = 0;
        ActionPolicy semanticSkipped(&memory);
        semanticSkipped.setCoverageVerifier(
            [&skippedCalls](const QString &, const QString &, const QJsonArray &,
                            const ActionOrigin &) {
                ++skippedCalls;
                return CoverageCheck{};
            });
        QCOMPARE(semanticSkipped.validateActionProposal(undeclaredFact, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("undeclared_external_fact"));
        QCOMPARE(skippedCalls, 0);
    }

    // R12-2a：OpenWake contact 必须 allow_contact=true 才能通过核验；enabled 独立。
    void validateActionProposalOpenWakeAllowContact()
    {
        PolicyIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"cognition":{"open_wake":{"enabled":false,"allow_contact":false}}})")));
        QCOMPARE(ConfigManager::instance().openWakeAllowContact(), false);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("proposal-wake.db"));
        MemoryManager memory(path);
        const auto wakeId = insertRunningOpenWake(
            path, QStringLiteral("run-wake-r12-2a"), QStringLiteral("owner"), 7);
        QVERIFY(wakeId > 0);
        const auto origin = ActionOrigin::forWake(
            wakeId, 1, QStringLiteral("run-wake-r12-2a"), QStringLiteral("owner"), 7,
            Channel::kPet, Channel::kDefaultSession);
        QVERIFY(origin.isValid());
        QJsonObject proposal{
            {QStringLiteral("origin"), origin.toJson()},
            {QStringLiteral("kind"), QStringLiteral("contact")},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("message"), QStringLiteral("在忙吗")},
                         {QStringLiteral("reason"), QStringLiteral("自己想到")}}},
            {QStringLiteral("claims"), QJsonArray{}}};
        ActionPolicy policy(&memory);
        QString error;
        // 默认 allow_contact=false：OpenWake contact 必须拒绝。
        const auto disabled = policy.validateActionProposal(proposal, &error);
        QCOMPARE(disabled.value(QStringLiteral("allowed")).toBool(), false);
        QCOMPARE(disabled.value(QStringLiteral("reason")).toString(),
                 QStringLiteral("open_wake_contact_disabled"));
        // 来源身份伪造：run_id / context_epoch / version 不一致都拒绝。
        auto badRun = proposal;
        auto badOrigin = origin.toJson();
        badOrigin[QStringLiteral("run_id")] = QStringLiteral("run-other");
        badRun[QStringLiteral("origin")] = badOrigin;
        QCOMPARE(policy.validateActionProposal(badRun, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("origin_run_mismatch"));
        auto badEpoch = proposal;
        auto badEpochOrigin = origin.toJson();
        badEpochOrigin[QStringLiteral("context_epoch")] = 8;
        badEpoch[QStringLiteral("origin")] = badEpochOrigin;
        QCOMPARE(policy.validateActionProposal(badEpoch, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("context_epoch_mismatch"));
        auto badVersion = proposal;
        auto badVersionOrigin = origin.toJson();
        badVersionOrigin[QStringLiteral("version")] = 2;
        badVersion[QStringLiteral("origin")] = badVersionOrigin;
        QCOMPARE(policy.validateActionProposal(badVersion, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("origin_version_mismatch"));

        // allow_contact=true 且 enabled=false：允许（enabled 与 allow_contact 独立）。
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"cognition":{"open_wake":{"enabled":false,"allow_contact":true}}})")));
        const auto config = ConfigManager::instance().getOpenWakeConfig();
        QCOMPARE(config.enabled, false);
        QCOMPARE(config.allowContact, true);
        const auto allowed = policy.validateActionProposal(proposal, &error);
        QVERIFY2(allowed.value(QStringLiteral("allowed")).toBool(),
                 qPrintable(allowed.value(QStringLiteral("reason")).toString()));
        QCOMPARE(allowed.value(QStringLiteral("action_key")).toString(),
                 QStringLiteral("wake:%1:1:contact").arg(wakeId));
        // enabled=true 但 allow_contact=false：仍然拒绝（allow_contact 是唯一许可）。
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"cognition":{"open_wake":{"enabled":true,"allow_contact":false}}})")));
        QCOMPARE(policy.validateActionProposal(proposal, &error)
                     .value(QStringLiteral("reason")).toString(),
                 QStringLiteral("open_wake_contact_disabled"));
    }

    // R12-2c-final-1（V12/V13/V14）：OpenWake 场景下 claim type→EvidencePurpose 的边界。
    // 真实日志里 wake/3 self_desire 引用较早的 chat/589 被判 stale_ref：旧输出协议只给出
    // reported_fact（→NewEvidence，保留新证据时序），没有 historical_fact 这个可表达的
    // "真实历史事实"类型。这里验证：历史事实（早于 wake）通过；被声明为新证据的旧引用
    // 仍按时序拒绝（不删时序检查、不按关键词降级）；self_desire 无外部事实不要求新证据。
    void openWakeEvidencePurposeHistoricalVsNewEvidence()
    {
        PolicyIdentityGuard identity;
        QVERIFY(identity.writeIdentity(QByteArray(
            R"({"proactive":{"enabled":true,"private_writing":false,"cooldown_seconds":0,)"
            R"("quiet_hours":{"start":"00:00","end":"00:00"},)"
            R"("targets":[{"channel":"pet","session_id":"default"}]},)"
            R"("cognition":{"open_wake":{"enabled":false,"allow_contact":true}}})")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath(QStringLiteral("purpose-open-wake.db"));
        MemoryManager memory(path);
        // 早于本次 wake 的主人真实发言（happened before wake by construction）。
        const auto earlier = insertOwnerEvidenceTurn(
            memory, QStringLiteral("以前说过喜欢下雨天"), -7200);
        QVERIFY(earlier > 0);
        const auto wakeId = insertRunningOpenWake(
            path, QStringLiteral("run-purpose-wake"), QStringLiteral("owner"), 0);
        QVERIFY(wakeId > 0);
        const auto origin = ActionOrigin::forWake(
            wakeId, 1, QStringLiteral("run-purpose-wake"), QStringLiteral("owner"), 0,
            Channel::kPet, Channel::kDefaultSession);
        QVERIFY(origin.isValid());
        ActionPolicy policy(&memory);
        const auto buildProposal = [&origin](const QJsonArray &claims,
                                             const QString &message) {
            QJsonArray coreIds;
            for (const auto &value : claims)
                coreIds.append(value.toObject().value(QStringLiteral("id")).toString());
            return QJsonObject{
                {QStringLiteral("origin"), origin.toJson()},
                {QStringLiteral("kind"), QStringLiteral("contact")},
                {QStringLiteral("payload"),
                 QJsonObject{{QStringLiteral("message"), message},
                             {QStringLiteral("reason"), QStringLiteral("自己想到")},
                             {QStringLiteral("core_claim_ids"), coreIds}}},
                {QStringLiteral("claims"), claims}};
        };
        const QString ref = QStringLiteral("chat/%1").arg(earlier);

        // V12：historical_fact（真实历史事实）早于 OpenWake 通过；不需要"新证据"时序。
        {
            const auto claim = purposeClaim(
                QStringLiteral("historical_fact"), QStringLiteral("主人以前说过喜欢下雨天"),
                {ref}, true);
            QString error;
            const auto result = policy.validateActionProposal(
                buildProposal(QJsonArray{claim}, QStringLiteral("还记得你说过喜欢下雨天吗")),
                &error);
            QVERIFY2(result.value(QStringLiteral("allowed")).toBool(),
                     qPrintable(result.value(QStringLiteral("reason")).toString()));
            QCOMPARE(result.value(QStringLiteral("needs_verifier")).toBool(), true);
            const auto claimResult = result.value(QStringLiteral("claims")).toArray()
                                         .first().toObject();
            QCOMPARE(claimResult.value(QStringLiteral("purpose")).toString(),
                     QStringLiteral("historical_fact"));
            QCOMPARE(claimResult.value(QStringLiteral("result")).toString(),
                     QStringLiteral("entailed"));
        }
        // V13：同一份旧引用若被声明为 reported_fact（new_evidence 旧语义）→ stale_ref 拒绝；
        // 诊断里保留 claim type/purpose/ref/report 时间/wake 时间（不含正文）。
        {
            const auto claim = purposeClaim(
                QStringLiteral("reported_fact"), QStringLiteral("主人今天说喜欢下雨天"),
                {ref}, true);
            QString error;
            const auto result = policy.validateActionProposal(
                buildProposal(QJsonArray{claim}, QStringLiteral("你刚说喜欢下雨天")), &error);
            QCOMPARE(result.value(QStringLiteral("allowed")).toBool(), false);
            QCOMPARE(result.value(QStringLiteral("reason")).toString(),
                     QStringLiteral("stale_ref"));
            const auto validation = policy.validateClaimForPurpose(
                Channel::kPet, Channel::kDefaultSession, origin, EvidencePurpose::NewEvidence,
                claim);
            QCOMPARE(validation.reason, QStringLiteral("stale_ref"));
            QCOMPARE(validation.evidenceRef, ref);
            QVERIFY(!validation.evidenceReported.isEmpty());
            // wake 时间基准：早于 wake 的证据被时序拒绝，而不是被当成新证据。
            QVERIFY(!validation.originReported.isEmpty());
        }
        // V14：self_desire 无外部事实（claims=[]）不要求任何新证据，也不触发语义核验。
        {
            QString error;
            const auto result = policy.validateActionProposal(
                buildProposal(QJsonArray{}, QStringLiteral("在忙吗")), &error);
            QVERIFY2(result.value(QStringLiteral("allowed")).toBool(),
                     qPrintable(result.value(QStringLiteral("reason")).toString()));
            QCOMPARE(result.value(QStringLiteral("needs_verifier")).toBool(), false);
            QCOMPARE(result.value(QStringLiteral("semantic_status")).toString(),
                     QStringLiteral("not_required"));
        }
    }

    // R12-2c-1：verifier 响应严格白名单解析（request_id/封闭枚举/类型检查），
    // semantic_status 只认服务端正式枚举；模型输出不能决定 verified 终态。
    void verifierVerdictStrictWhitelist()
    {
        const QJsonObject base{
            {QStringLiteral("request_id"), QStringLiteral("req-1")},
            {QStringLiteral("claim_coverage"), QStringLiteral("complete")},
            {QStringLiteral("missing_claims"), QJsonArray{}},
            {QStringLiteral("result"), QStringLiteral("entailed")},
            {QStringLiteral("supporting_refs"), QJsonArray{QStringLiteral("chat/1")}},
            {QStringLiteral("reason_code"), QStringLiteral("ok")}};
        const auto valid = ActionPolicy::parseVerifierVerdict(base);
        QVERIFY(valid.valid);
        QCOMPARE(valid.requestId, QStringLiteral("req-1"));
        QCOMPARE(valid.coverageComplete, true);
        QCOMPARE(static_cast<int>(valid.result),
                 static_cast<int>(VerifierVerdict::Entailed));
        QCOMPARE(valid.supportingRefs.size(), 1);

        // 缺 request_id → schema_invalid（调用方 withheld；不得猜测为匹配）。
        auto noRequestId = base;
        noRequestId.remove(QStringLiteral("request_id"));
        auto verdict = ActionPolicy::parseVerifierVerdict(noRequestId);
        QCOMPARE(verdict.valid, false);
        QCOMPARE(verdict.error, QStringLiteral("schema_invalid"));
        // 未知枚举 → unknown_enum。
        auto badCoverage = base;
        badCoverage[QStringLiteral("claim_coverage")] = QStringLiteral("partial");
        verdict = ActionPolicy::parseVerifierVerdict(badCoverage);
        QCOMPARE(verdict.valid, false);
        QCOMPARE(verdict.error, QStringLiteral("unknown_enum"));
        auto badResult = base;
        badResult[QStringLiteral("result")] = QStringLiteral("maybe");
        verdict = ActionPolicy::parseVerifierVerdict(badResult);
        QCOMPARE(verdict.valid, false);
        QCOMPARE(verdict.error, QStringLiteral("unknown_enum"));
        // 字段类型检查：supporting_refs 必须是字符串数组，missing_claims 必须是对象数组。
        auto badRefs = base;
        badRefs[QStringLiteral("supporting_refs")] = QStringLiteral("chat/1");
        QCOMPARE(ActionPolicy::parseVerifierVerdict(badRefs).error,
                 QStringLiteral("schema_invalid"));
        auto badMissing = base;
        badMissing[QStringLiteral("missing_claims")] = QJsonArray{1};
        QCOMPARE(ActionPolicy::parseVerifierVerdict(badMissing).error,
                 QStringLiteral("schema_invalid"));
        auto badMissingText = base;
        badMissingText[QStringLiteral("missing_claims")] =
            QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("reported_fact")},
                                   {QStringLiteral("text"), 7}}};
        QCOMPARE(ActionPolicy::parseVerifierVerdict(badMissingText).error,
                 QStringLiteral("schema_invalid"));
        // 合法但非通过的结果：contradicted/insufficient 解析成功，终态仍由调用方 withheld。
        auto contradicted = base;
        contradicted[QStringLiteral("result")] = QStringLiteral("contradicted");
        verdict = ActionPolicy::parseVerifierVerdict(contradicted);
        QVERIFY(verdict.valid);
        QCOMPARE(static_cast<int>(verdict.result),
                 static_cast<int>(VerifierVerdict::Contradicted));
        auto insufficient = base;
        insufficient[QStringLiteral("result")] = QStringLiteral("insufficient");
        QVERIFY(ActionPolicy::parseVerifierVerdict(insufficient).valid);

        // semantic_status 白名单：只接受正式枚举；空串/任意字符串都不合法。
        for (const auto &allowed :
             {QStringLiteral("unverified"), QStringLiteral("entailed"),
              QStringLiteral("entailed_coverage"), QStringLiteral("repaired"),
              QStringLiteral("repair_needed"), QStringLiteral("contradicted"),
              QStringLiteral("insufficient"), QStringLiteral("withheld"),
              QStringLiteral("not_required"), QStringLiteral("unknown"),
              QStringLiteral("retained")}) {
            QVERIFY2(ActionPolicy::isValidSemanticStatus(allowed),
                     qPrintable(allowed));
        }
        for (const auto &invalid :
             {QString(), QStringLiteral("verified"), QStringLiteral("complete"),
              QStringLiteral("ok"), QStringLiteral("ENTAILED")}) {
            QVERIFY2(!ActionPolicy::isValidSemanticStatus(invalid),
                     qPrintable(invalid));
        }
        QCOMPARE(ActionPolicy::verifiedSemanticStatus(true), QStringLiteral("entailed"));
        QCOMPARE(ActionPolicy::verifiedSemanticStatus(false),
                 QStringLiteral("entailed_coverage"));
    }
};

QTEST_MAIN(TestActionPolicy)
#include "tst_actionpolicy.moc"
