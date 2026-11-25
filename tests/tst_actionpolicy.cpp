#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "actionpolicy.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "utterance.h"

namespace {

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
    bool enable()
    {
        QJsonObject target{{QStringLiteral("channel"), Channel::kPet},
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
        QVERIFY(memory.saveQATurn(QStringLiteral("你想做点什么"),
                                  QStringLiteral(R"({"personal_interest":"写一首关于雨的诗"})"), {}));
        const auto thought = memory.nextThought(false);
        const auto activityId = thought.value(QStringLiteral("id")).toInteger();
        const int version = thought.value(QStringLiteral("version")).toInt();
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
};

QTEST_MAIN(TestActionPolicy)
#include "tst_actionpolicy.moc"
