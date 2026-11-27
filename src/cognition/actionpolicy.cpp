#include <limits>
#include "actionpolicy.h"

#include "activityref.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include "utterance.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTime>
#include <utility>

namespace {

const QString kReportedFact = QStringLiteral("reported_fact");
const QString kUserCommitment = QStringLiteral("user_commitment");
const QString kExecutionResult = QStringLiteral("execution_result");
const QString kInference = QStringLiteral("inference");
const QString kArtifactResult = QStringLiteral("artifact_result");

const QString kSelfDesire = QStringLiteral("self_desire");
const QString kConcernFollowup = QStringLiteral("concern_followup");
const QString kActivityShare = QStringLiteral("activity_share");

const QString kProposalKindContact = QStringLiteral("contact");
const QString kProposalKindActivityShare = QStringLiteral("activity_share");

// R12-2a：CurrentState 在没有来源时序基准时要求证据落在最近 N 小时内（"当前/较新"）。
const int kCurrentStateFreshHours = 6;

// 纯时间比较（与收口语义无关）：later 是否严格晚于 earlier。
// 用于"证据必须晚于关注来源"的来源时序检查。
bool timeAfter(const QSqlDatabase &db,const QString &later,const QString &earlier)
{
    if (later.isEmpty() || earlier.isEmpty()) return false;
    QSqlQuery compare(db);
    compare.prepare(QStringLiteral("SELECT julianday(?)>julianday(?)"));
    compare.addBindValue(later);
    compare.addBindValue(earlier);
    return compare.exec() && compare.next() && compare.value(0).toInt()==1;
}

// 证据是否落在最近 hours 小时内（用于没有来源时序基准时的 CurrentState）。
bool withinFreshWindow(const QSqlDatabase &db, const QString &reported, int hours)
{
    if (reported.isEmpty()) return false;
    QSqlQuery compare(db);
    compare.prepare(QStringLiteral("SELECT julianday(?)>=julianday('now',?)"));
    compare.addBindValue(reported);
    compare.addBindValue(QStringLiteral("-%1 hours").arg(hours));
    return compare.exec() && compare.next() && compare.value(0).toInt()==1;
}

// claim 类型 → 用途。模型不能自定义用途：服务端按 type 判定。
// 旧类型映射保持兼容：reported_fact 是"相对本次关注来源的新证据"（保留 stale_ref 语义），
// 新类型可以显式表达历史事实/当前状态/成果/内部状态。
// （映射的唯一实现是 ActionPolicy::purposeForClaimType，static 入口供所有推断复用。）

// 事实类别（需要语义蕴含核验的 claim）；内部状态只证明"她内部有过记录"，不算事实声称。
bool isFactClaimType(const QString &type)
{
    EvidencePurpose purpose = EvidencePurpose::HistoricalFact;
    if (!ActionPolicy::purposeForClaimType(type, &purpose)) return false;
    return purpose != EvidencePurpose::InternalState;
}

// 动作核心：用户承诺/执行结果/成果本身就是对外动作的事实核心；
// reported_fact/inference 只有被标成 core（reason/message 依赖它）时才算核心。
bool claimIsCoreByType(const QString &type)
{
    return type == kUserCommitment || type == kExecutionResult || type == kArtifactResult;
}

bool claimMarkedCore(const QJsonObject &claim, const QJsonObject &contact)
{
    if (claim.value(QStringLiteral("core")).toBool()) return true;
    const auto id = claim.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) return false;
    for (const auto &value : contact.value(QStringLiteral("core_claim_ids")).toArray())
        if (value.toString() == id) return true;
    return false;
}

// 与 privatereflection.cpp / canContactNow 相同的渠道可见性口径：
// pet 与 qq_private（主人账号）视为同一段私聊关系，其它渠道只看同渠道同会话。
bool visibleSession(const QString &channel, const QString &session,
                    const QString &viewerChannel, const QString &viewerSession)
{
    if (channel.isEmpty() || session.isEmpty()) return false;
    auto &cfg = ConfigManager::instance();
    if (cfg.isBlindZone(channel, session)) return false;
    if (channel == viewerChannel && session == viewerSession) return true;
    return cfg.getMemoryPolicyConfig().agent.ownerCrossChannelPrivate
           && ((channel == Channel::kPet && session == Channel::kDefaultSession)
               || (channel == Channel::kQQPrivate && cfg.isMasterQQ(session)));
}

bool channelVisibleFromOwnerPrivate(const QString &channel, const QString &session)
{
    return visibleSession(channel, session, Channel::kPet, Channel::kDefaultSession);
}

// 从 chat_history.raw_reply（JSON sentences 或纯文本）里提取助手说过的文字。
QString replyText(const QString &rawReply)
{
    const QString trimmed = rawReply.trimmed();
    if (trimmed.isEmpty()) return {};
    const auto document = QJsonDocument::fromJson(trimmed.toUtf8());
    if (document.isObject()) {
        QStringList texts;
        for (const auto &value : document.object().value(QStringLiteral("sentences")).toArray())
            texts.append(value.toObject().value(QStringLiteral("zh_text")).toString());
        if (!texts.isEmpty()) return texts.join(QString());
    }
    return trimmed;
}

struct RefText
{
    bool found = false;
    QString text;
    QString channel;
    QString session;
    QString reported;
    QString reason;
};

// 统一的"这条 ref 现在还能不能当主人发言"判定。subject/sensitive/可见性/时序与
// v29 validateContactBasis 的口径保持一致，只是拆成可复用的 reason 码。
class EvidenceReader
{
public:
    EvidenceReader(const QSqlDatabase &db, const QString &channel, const QString &session,
                   qlonglong concernTurn)
        : m_db(db), m_channel(channel), m_session(session)
    {
        if (concernTurn <= 0) return;
        QSqlQuery origin(db);
        origin.prepare("SELECT h.channel,h.session_id,coalesce(s.reported,h.timestamp),"
                       "h.subject,h.sensitive,h.turn_kind "
                       "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
                       "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
                       "AND s.input=h.user_input WHERE h.id=?");
        origin.addBindValue(concernTurn);
        if (!origin.exec() || !origin.next()) {
            m_originReason = QStringLiteral("invalid_concern");
            return;
        }
        if (origin.value(3).toString() != QStringLiteral("user") || origin.value(4).toBool()
            || origin.value(5).toString() != QStringLiteral("reply")) {
            m_originReason = QStringLiteral("invalid_concern");
            return;
        }
        m_originReported = origin.value(2).toString();
        origin.finish();
    }

    // 关注来源本身不合法时，任何 claim 都不能通过（"在谁的记忆里说的"必须可定位）。
    QString originReason() const { return m_originReason; }
    QString originReported() const { return m_originReported; }

    // 关注之后的主语/时序检查。返回空串表示通过。
    QString checkOwnerStatementTiming(const QString &reported) const
    {
        if (!m_originReason.isEmpty()) return m_originReason;
        if (!m_originReported.isEmpty() && !timeAfter(m_db, reported, m_originReported))
            return QStringLiteral("stale_ref");
        return {};
    }
    // chat/<id>：主人真实回复 / 她自己的表达 / 主动消息都从同一行区分。
    // desired: 0 = 任意来源（仅取文本），1 = 必须主人发言，2 = 必须助手发送回执。
    RefText chatRef(qlonglong turn, int desired) const
    {
        RefText out;
        QSqlQuery row(m_db);
        row.prepare("SELECT h.channel,h.session_id,coalesce(s.reported,h.timestamp),"
                    "h.subject,h.sensitive,h.turn_kind,coalesce(h.user_input,''),"
                    "coalesce(h.raw_reply,''),coalesce(h.turn_state,'') "
                    "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
                    "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
                    "AND s.input=h.user_input WHERE h.id=?");
        row.addBindValue(turn);
        if (!row.exec() || !row.next()) {
            out.reason = QStringLiteral("invalid_ref");
            return out;
        }
        out.found = true;
        out.channel = row.value(0).toString();
        out.session = row.value(1).toString();
        out.reported = row.value(2).toString();
        const bool sensitive = row.value(4).toBool();
        const QString turnKind = row.value(5).toString();
        const QString userInput = row.value(6).toString();
        const QString rawReply = row.value(7).toString();
        const QString turnState = row.value(8).toString();
        const QString subject = row.value(3).toString();
        // 注意：QSqlQuery::finish() 之后 value() 已失效，必须读完全部列再 finish。
        row.finish();
        if (sensitive) {
            out.reason = QStringLiteral("sensitive_ref");
            return out;
        }
        if (!channelVisibleFromOwnerPrivate(out.channel, out.session)) {
            out.reason = QStringLiteral("invisible_ref");
            return out;
        }
        if (desired == 2) {
            // 执行回执：必须是她已经交给渠道的一条消息（handed_off），不是"打算说"。
            if (rawReply.trimmed().isEmpty() || turnState != QStringLiteral("handed_off")) {
                out.reason = QStringLiteral("execution_result_not_found");
                return out;
            }
            out.text = replyText(rawReply).left(512);
            return out;
        }
        const bool ownerStatement = subject == QStringLiteral("user")
            && turnKind == QStringLiteral("reply") && !userInput.trimmed().isEmpty();
        if (desired == 1) {
            if (!ownerStatement) {
                out.reason = turnKind == QStringLiteral("proactive")
                    ? QStringLiteral("assistant_expression_not_user_commitment")
                    : QStringLiteral("not_owner_statement");
                return out;
            }
        } else {
            // 只是取文本：主人说过的优先，其次是她自己的表达（例如执行回执）。
            if (ownerStatement) out.text = userInput.left(512);
            else out.text = replyText(rawReply).left(512);
            return out;
        }
        out.text = userInput.left(512);
        out.reason = checkOwnerStatementTiming(out.reported);
        return out;
    }

private:
    QSqlDatabase m_db;
    QString m_channel;
    QString m_session;
    QString m_originReported;
    QString m_originReason;
};

// ===== R12-2a：按 purpose 的确定性证据读取 =====
// 这里只做"引用是否存在 + authority/可见性/时序/成果真实性"，不做语义蕴含判断。
struct RefEvidence
{
    bool ok = false;
    QString reason;
    QString text;
    QString reported;
    int artifactVersion = 0;
    QString artifactHash;
};

// 成果正文哈希：与 memorymanager.cpp 的 activityContentHash 同一算法（SHA-256 前 16 hex）。
QString artifactDraftHash(const QString &text)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(text.trimmed().toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}

bool containsAny(const QString &text, const QStringList &markers)
{
    for (const auto &marker : markers)
        if (text.contains(marker)) return true;
    return false;
}

// 明确的不确定表达（CurrentState 允许 unknown，而不是把历史相似记录当成当前事实）。
bool uncertainExpression(const QString &text)
{
    static const QStringList markers{
        QStringLiteral("不确定"), QStringLiteral("可能"), QStringLiteral("好像"),
        QStringLiteral("也许"), QStringLiteral("或许"), QStringLiteral("记不清"),
        QStringLiteral("不知道"), QStringLiteral("无法确认"), QStringLiteral("不清楚")};
    return containsAny(text, markers);
}

// message/reason 中的"外部事实/用户承诺"断言（确定性近似；真实语义 coverage 由注入器判定）。
QStringList externalFactAssertions(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(
        "(你(?:之前|上次|昨天|前天|上周|以前|曾经)?(?:说过|答应过|答应|告诉过我|提过|让我)"
        "|你(?:已经|刚刚)?(?:答应|承诺)"
        "|(?:上次|之前|昨天|前天|上周)(?:你|主人|我们)"
        "|(?:我们|咱俩)(?:上次|之前|说好|约定)"
        "|主人(?:说过|告诉过我|答应过|提过)"
        "|我(?:已经|刚刚)?(?:把|给)你(?:发|送|做|写)过)"));
    QStringList hits;
    if (text.trimmed().isEmpty()) return hits;
    auto it = pattern.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString fragment = match.captured(0).trimmed();
        if (!fragment.isEmpty() && !hits.contains(fragment)) hits.append(fragment);
        if (hits.size() >= 4) break;
    }
    return hits;
}

// 普通"主人原话/真实历史来源"类 ref：chat/、source/、raw_event/。
// requireFresh=true 时，证据时间必须严格晚于 originReported；
// originReported 为空时（旧入口 concernTurn<=0），若 requireRecentWhenNoOrigin=true
// 则要求证据落在最近 kCurrentStateFreshHours 小时内，否则跳过时序检查。
RefEvidence readOwnerSourceRef(const QSqlDatabase &db, const QString &ref,
                               const QString &originReported, const QString &staleReason,
                               bool requireFresh, bool requireRecentWhenNoOrigin)
{
    RefEvidence out;
    const auto timingReason = [&](const QString &reported) -> QString {
        if (!requireFresh) return {};
        if (!originReported.isEmpty())
            return timeAfter(db, reported, originReported) ? QString() : staleReason;
        if (requireRecentWhenNoOrigin
            && !withinFreshWindow(db, reported, kCurrentStateFreshHours))
            return staleReason;
        return {};
    };
    if (ref.startsWith(QStringLiteral("chat/"))) {
        const qlonglong turn = ref.mid(5).section(':', 0, 0).toLongLong();
        EvidenceReader reader(db, QString(), QString(), 0);
        const RefText row = reader.chatRef(turn, 1);
        if (!row.reason.isEmpty()) {
            out.reason = row.reason;
            return out;
        }
        out.reported = row.reported;
        const QString reason = timingReason(row.reported);
        if (!reason.isEmpty()) {
            out.reason = reason;
            return out;
        }
        out.ok = true;
        out.text = row.text;
        return out;
    }
    if (ref.startsWith(QStringLiteral("source/"))) {
        const qlonglong sourceId = ref.mid(QStringLiteral("source/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(db);
        row.prepare("SELECT subject,channel,session,input,reported FROM cognitive_source WHERE id=?");
        row.addBindValue(sourceId);
        if (!row.exec() || !row.next()) {
            out.reason = QStringLiteral("invalid_ref");
            return out;
        }
        const QString subject = row.value(0).toString();
        const QString channel = row.value(1).toString();
        const QString session = row.value(2).toString();
        const QString input = row.value(3).toString();
        out.reported = row.value(4).toString();
        row.finish();
        if (subject != QStringLiteral("user") || input.trimmed().isEmpty()) {
            out.reason = QStringLiteral("not_owner_statement");
            return out;
        }
        if (!channelVisibleFromOwnerPrivate(channel, session)) {
            out.reason = QStringLiteral("invisible_ref");
            return out;
        }
        const QString reason = timingReason(out.reported);
        if (!reason.isEmpty()) {
            out.reason = reason;
            return out;
        }
        out.ok = true;
        out.text = input.left(512);
        return out;
    }
    if (ref.startsWith(QStringLiteral("raw_event/"))) {
        const qlonglong eventId = ref.mid(QStringLiteral("raw_event/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(db);
        row.prepare("SELECT channel,session_id,direction,sensitive,content,visibility,occurred_at "
                    "FROM raw_events WHERE id=?");
        row.addBindValue(eventId);
        if (!row.exec() || !row.next()) {
            out.reason = QStringLiteral("invalid_ref");
            return out;
        }
        const QString channel = row.value(0).toString();
        const QString session = row.value(1).toString();
        const QString direction = row.value(2).toString();
        const bool sensitive = row.value(3).toBool();
        const QString content = row.value(4).toString();
        out.reported = row.value(6).toString();
        row.finish();
        if (sensitive) {
            out.reason = QStringLiteral("sensitive_ref");
            return out;
        }
        if (direction != QStringLiteral("in")) {
            out.reason = QStringLiteral("not_owner_statement");
            return out;
        }
        if (!channelVisibleFromOwnerPrivate(channel, session)) {
            out.reason = QStringLiteral("invisible_ref");
            return out;
        }
        const QString reason = timingReason(out.reported);
        if (!reason.isEmpty()) {
            out.reason = reason;
            return out;
        }
        out.ok = true;
        out.text = content.left(512);
        return out;
    }
    out.reason = QStringLiteral("invalid_ref");
    return out;
}

// 执行结果 ref：delivery/<action_key>（handed_off）或 chat/<turn>（handed_off 的助手消息）。
RefEvidence readExecutionRef(const QSqlDatabase &db, const QString &ref)
{
    RefEvidence out;
    if (ref.startsWith(QStringLiteral("delivery/"))) {
        const auto key = ref.mid(QStringLiteral("delivery/").size());
        QString actionState, deliveryChannel, deliverySession;
        bool found = false;
        QSqlQuery row(db);
        row.prepare("SELECT a.state,a.channel,a.session_id FROM cognitive_action a "
                    "WHERE a.action_key=? LIMIT 1");
        row.addBindValue(key);
        if (row.exec() && row.next()) {
            found = true;
            actionState = row.value(0).toString();
            deliveryChannel = row.value(1).toString();
            deliverySession = row.value(2).toString();
        }
        if (!found && key.toLongLong() > 0) {
            QSqlQuery byId(db);
            byId.prepare("SELECT a.state,a.channel,a.session_id FROM action_outbox o "
                         "JOIN cognitive_action a ON a.id=o.action_id WHERE o.id=? LIMIT 1");
            byId.addBindValue(key.toLongLong());
            if (byId.exec() && byId.next()) {
                found = true;
                actionState = byId.value(0).toString();
                deliveryChannel = byId.value(1).toString();
                deliverySession = byId.value(2).toString();
            }
        }
        if (!found || actionState != QStringLiteral("handed_off")) {
            out.reason = QStringLiteral("execution_result_not_found");
            return out;
        }
        if (!channelVisibleFromOwnerPrivate(deliveryChannel, deliverySession)) {
            out.reason = QStringLiteral("invisible_ref");
            return out;
        }
        out.ok = true;
        out.text = key;
        return out;
    }
    if (ref.startsWith(QStringLiteral("chat/"))) {
        const qlonglong turn = ref.mid(5).section(':', 0, 0).toLongLong();
        EvidenceReader reader(db, QString(), QString(), 0);
        const RefText row = reader.chatRef(turn, 2);
        if (!row.reason.isEmpty()) {
            out.reason = row.reason;
            return out;
        }
        out.ok = true;
        out.text = row.text;
        out.reported = row.reported;
        return out;
    }
    out.reason = QStringLiteral("invalid_ref");
    return out;
}

// 内部状态 ref：reflection/<turn>、matter/<name>、revision/<name>、wake/<id>。
// 只能证明"她内部有过这条记录"，不得当作用户事实/承诺（额外文本检查在 claim 层）。
RefEvidence readInternalStateRef(const QSqlDatabase &db, const QString &ref)
{
    RefEvidence out;
    if (ref.startsWith(QStringLiteral("reflection/"))) {
        const auto turn = ref.mid(QStringLiteral("reflection/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(db);
        row.prepare("SELECT h.channel,h.session_id FROM private_reflection r "
                    "JOIN chat_history h ON h.id=r.turn_id WHERE r.turn_id=? AND h.subject='user' LIMIT 1");
        row.addBindValue(turn);
        if (!row.exec() || !row.next()
            || !channelVisibleFromOwnerPrivate(row.value(0).toString(), row.value(1).toString())) {
            out.reason = QStringLiteral("internal_ref_not_found");
            return out;
        }
        out.ok = true;
        return out;
    }
    if (ref.startsWith(QStringLiteral("matter/")) || ref.startsWith(QStringLiteral("revision/"))) {
        const auto matter = ref.startsWith(QStringLiteral("matter/"))
            ? ref.mid(QStringLiteral("matter/").size()).section(':', 0, 0)
            : ref.mid(QStringLiteral("revision/").size()).section('/', 0, 0);
        QSqlQuery row(db);
        row.prepare("SELECT s.channel,s.session FROM matter_revision r "
                    "JOIN cognitive_source s ON s.id=r.source "
                    "WHERE r.matter=? AND s.subject='user' ORDER BY r.version DESC LIMIT 1");
        row.addBindValue(matter);
        if (!row.exec() || !row.next()
            || !channelVisibleFromOwnerPrivate(row.value(0).toString(), row.value(1).toString())) {
            out.reason = QStringLiteral("internal_ref_not_found");
            return out;
        }
        out.ok = true;
        return out;
    }
    if (ref.startsWith(QStringLiteral("wake/"))) {
        const auto wakeId = ref.mid(QStringLiteral("wake/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(db);
        row.prepare("SELECT 1 FROM open_wake WHERE id=? LIMIT 1");
        row.addBindValue(wakeId);
        if (!row.exec() || !row.next()) {
            out.reason = QStringLiteral("internal_ref_not_found");
            return out;
        }
        out.ok = true;
        return out;
    }
    out.reason = QStringLiteral("invalid_ref");
    return out;
}

// 成果 ref：thought/<id>（legacy）或 activity/<id>（internal）。
// 要求 completed、artifact_ref 指回成果自身、正文 draft 非空；internal 还必须带成果版本/哈希。
// legacy 旧 completed 允许缺 artifact_version/hash（只读兼容），版本按行版本兜底。
RefEvidence readArtifactRef(const QSqlDatabase &db, const QString &ref)
{
    RefEvidence out;
    ActivityRef activity;
    QString parseError;
    if (!ActivityRef::parse(ref, &activity, &parseError)) {
        out.reason = QStringLiteral("invalid_ref");
        return out;
    }
    const QString canonical = activity.ref();
    if (activity.kind == ActivityRef::LegacyThought) {
        QSqlQuery row(db);
        row.prepare("SELECT channel,session_id,sensitive,thought_version,"
                    "coalesce(json_extract(thought_result,'$.status'),'active'),"
                    "coalesce(json_extract(thought_result,'$.artifact_ref'),''),"
                    "coalesce(json_extract(thought_result,'$.draft'),''),"
                    "coalesce(json_extract(thought_result,'$.artifact_version'),0),"
                    "coalesce(json_extract(thought_result,'$.artifact_hash'),'') "
                    "FROM chat_history WHERE id=? AND thought IS NOT NULL AND thought<>''");
        row.addBindValue(activity.id);
        if (!row.exec() || !row.next()) {
            out.reason = QStringLiteral("artifact_not_found");
            return out;
        }
        const QString channel = row.value(0).toString();
        const QString session = row.value(1).toString();
        const bool sensitive = row.value(2).toBool();
        const int rowVersion = row.value(3).toInt();
        const QString status = row.value(4).toString();
        const QString artifactRef = row.value(5).toString().trimmed();
        const QString draft = row.value(6).toString().trimmed();
        int artifactVersion = row.value(7).toInt();
        const QString artifactHash = row.value(8).toString().trimmed();
        row.finish();
        if (sensitive) {
            out.reason = QStringLiteral("sensitive_ref");
            return out;
        }
        if (!channelVisibleFromOwnerPrivate(channel, session)) {
            out.reason = QStringLiteral("invisible_ref");
            return out;
        }
        if (status != QStringLiteral("completed")) {
            out.reason = QStringLiteral("artifact_not_completed");
            return out;
        }
        if (artifactRef != canonical || draft.isEmpty()) {
            out.reason = QStringLiteral("artifact_not_verifiable");
            return out;
        }
        if (artifactVersion <= 0) artifactVersion = rowVersion; // 旧 completed 只读兼容
        out.ok = true;
        out.text = draft;
        out.artifactVersion = artifactVersion;
        out.artifactHash = artifactHash;
        return out;
    }
    QSqlQuery row(db);
    row.prepare("SELECT scope_key,channel,session_id,status,version,"
                "coalesce(artifact_ref,''),coalesce(result_json,'{}') "
                "FROM internal_activity WHERE id=?");
    row.addBindValue(activity.id);
    if (!row.exec() || !row.next()) {
        out.reason = QStringLiteral("artifact_not_found");
        return out;
    }
    const QString scopeKey = row.value(0).toString();
    const QString channel = row.value(1).toString();
    const QString session = row.value(2).toString();
    const QString status = row.value(3).toString();
    const QString artifactRefColumn = row.value(5).toString().trimmed();
    const QJsonObject stored = QJsonDocument::fromJson(row.value(6).toString().toUtf8()).object();
    row.finish();
    if (scopeKey != QStringLiteral("owner")) {
        out.reason = QStringLiteral("invisible_ref");
        return out;
    }
    if (!channelVisibleFromOwnerPrivate(channel, session)) {
        out.reason = QStringLiteral("invisible_ref");
        return out;
    }
    if (status != QStringLiteral("completed")) {
        out.reason = QStringLiteral("artifact_not_completed");
        return out;
    }
    const QString artifactRef = artifactRefColumn.isEmpty()
        ? stored.value(QStringLiteral("artifact_ref")).toString().trimmed()
        : artifactRefColumn;
    const QString draft = stored.value(QStringLiteral("draft")).toString().trimmed();
    const int artifactVersion = stored.value(QStringLiteral("artifact_version")).toInt();
    const QString artifactHash = stored.value(QStringLiteral("artifact_hash")).toString().trimmed();
    // internal 新写入的成果必须可核验（ref/版本/正文齐全）；缺字段说明不是真实成果。
    if (artifactRef != canonical || draft.isEmpty() || artifactVersion <= 0) {
        out.reason = QStringLiteral("artifact_not_verifiable");
        return out;
    }
    out.ok = true;
    out.text = draft;
    out.artifactVersion = artifactVersion;
    out.artifactHash = artifactHash;
    return out;
}

// purpose → 单条 ref 的确定性读取。失败 reason 即对外可审计的原因码。
RefEvidence readPurposeRef(const QSqlDatabase &db, EvidencePurpose purpose, const QString &ref,
                           const QString &originReported)
{
    switch (purpose) {
    case EvidencePurpose::HistoricalFact:
        // 真实历史来源：不因早于本次 wake/关注时间而 stale。
        return readOwnerSourceRef(db, ref, QString(), QString(), false, false);
    case EvidencePurpose::NewEvidence:
        // 唯一保留 stale_ref 的路径：新证据必须晚于本次来源时间。
        return readOwnerSourceRef(db, ref, originReported, QStringLiteral("stale_ref"), true, false);
    case EvidencePurpose::CurrentState:
        // 当前状态：必须有当前/较新证据；没有来源基准时要求落在最近窗口内。
        return readOwnerSourceRef(db, ref, originReported,
                                  QStringLiteral("current_state_not_current"), true, true);
    case EvidencePurpose::UserCommitment:
        // 承诺只能来自真实用户原话；提醒是"已安排的执行"，不是主人原话。
        if (ref.startsWith(QStringLiteral("reminder/"))) {
            RefEvidence reminder;
            reminder.reason = QStringLiteral("reminder_not_owner_statement");
            return reminder;
        }
        if (ref.startsWith(QStringLiteral("raw_event/"))) {
            RefEvidence raw;
            raw.reason = QStringLiteral("invalid_ref");
            return raw;
        }
        return readOwnerSourceRef(db, ref, QString(), QString(), false, false);
    case EvidencePurpose::ExecutionResult:
        return readExecutionRef(db, ref);
    case EvidencePurpose::ArtifactResult:
        return readArtifactRef(db, ref);
    case EvidencePurpose::InternalState:
        return readInternalStateRef(db, ref);
    }
    RefEvidence out;
    out.reason = QStringLiteral("invalid_purpose");
    return out;
}

// 统一的 claim refs 确定性校验（purpose 已由服务端判定）。
// R12-2c-final-1：失败时把"第一个失败 ref / 它的 report 时间 / 来源基准时间"带回
// ClaimValidation（只用于诊断日志；不改变判定结果），便于区分 claim type→purpose 映射问题
// 与真正的来源缺失/时序问题。
ClaimValidation validateClaimRefsWithOrigin(const QSqlDatabase &db, EvidencePurpose purpose,
                                            const QJsonObject &claim,
                                            const QString &originReported)
{
    const auto insufficient = [&originReported](const QString &reason,
                                                const QString &evidenceRef = QString(),
                                                const QString &evidenceReported = QString()) {
        ClaimValidation validation;
        validation.result = ClaimValidation::Insufficient;
        validation.reason = reason;
        validation.evidenceRef = evidenceRef;
        validation.evidenceReported = evidenceReported;
        validation.originReported = originReported;
        return validation;
    };
    if (claim.value(QStringLiteral("text")).toString().trimmed().isEmpty())
        return insufficient(QStringLiteral("missing_claim_text"));
    const auto refsValue = claim.value(QStringLiteral("refs"));
    if (!refsValue.isArray()) return insufficient(QStringLiteral("missing_refs"));
    const auto refs = refsValue.toArray();
    if (refs.size() > 4) return insufficient(QStringLiteral("too_many_refs"));
    // CurrentState 允许明确 unknown：没有"当前证据"时不能把历史记录当现在的事实。
    if (purpose == EvidencePurpose::CurrentState
        && uncertainExpression(claim.value(QStringLiteral("text")).toString())) {
        ClaimValidation unknown;
        unknown.result = ClaimValidation::Insufficient;
        unknown.unknown = true;
        unknown.reason = QStringLiteral("explicit_unknown");
        unknown.purpose = QStringLiteral("current_state");
        unknown.originReported = originReported;
        return unknown;
    }
    if (refs.isEmpty()) {
        // 内部状态（旧 inference）不需要外部来源；但不得把外部事实伪装成内部记录。
        if (purpose == EvidencePurpose::InternalState) {
            if (!externalFactAssertions(claim.value(QStringLiteral("text")).toString()).isEmpty())
                return insufficient(QStringLiteral("internal_state_not_external"));
            ClaimValidation validation;
            validation.result = ClaimValidation::Entailed;
            validation.purpose = QStringLiteral("internal_state");
            validation.originReported = originReported;
            return validation;
        }
        return insufficient(QStringLiteral("missing_refs"));
    }
    for (const auto &value : refs) {
        if (!value.isString() || value.toString().trimmed().isEmpty())
            return insufficient(QStringLiteral("invalid_ref"));
    }
    ClaimValidation validation;
    validation.result = ClaimValidation::Entailed;
    validation.originReported = originReported;
    for (const auto &value : refs) {
        const QString ref = value.toString().trimmed();
        const RefEvidence evidence = readPurposeRef(db, purpose, ref, originReported);
        if (!evidence.ok)
            return insufficient(evidence.reason, ref, evidence.reported);
        if (purpose == EvidencePurpose::ArtifactResult) {
            // 成果版本/哈希可由 claim 显式声明，但必须与真实成果一致（不是随机数）。
            const QJsonValue versionValue = claim.value(QStringLiteral("artifact_version"));
            if (versionValue.isDouble() && versionValue.toInt() != evidence.artifactVersion)
                return insufficient(QStringLiteral("artifact_version_mismatch"), ref,
                                    evidence.reported);
            const QString claimedHash = claim.value(QStringLiteral("artifact_hash")).toString().trimmed();
            if (!claimedHash.isEmpty() && !evidence.artifactHash.isEmpty()
                && claimedHash != evidence.artifactHash)
                return insufficient(QStringLiteral("artifact_hash_mismatch"), ref,
                                    evidence.reported);
        }
        validation.supportingRefs.append(ref);
    }
    // 内部状态不能成为对外事实/承诺：即使带内部 refs，文本出现外部事实断言也拒绝。
    if (purpose == EvidencePurpose::InternalState
        && !externalFactAssertions(claim.value(QStringLiteral("text")).toString()).isEmpty())
        return insufficient(QStringLiteral("internal_state_not_external"));
    validation.purpose = evidencePurposeName(purpose);
    return validation;
}

bool quietNow(const ProactiveConfig &options)
{
    const auto start = QTime::fromString(options.quietStart, QStringLiteral("HH:mm"));
    const auto end = QTime::fromString(options.quietEnd, QStringLiteral("HH:mm"));
    if (!start.isValid() || !end.isValid()) return false;
    const auto time = QTime::currentTime();
    if (start < end) return time >= start && time < end;
    if (start > end) return time >= start || time < end;
    return false;
}

bool targetAllowed(const ProactiveConfig &options, const QString &channel, const QString &session)
{
    for (const auto &pair : options.targets)
        if (pair.first == channel && pair.second == session) return true;
    return false;
}

QJsonObject claimResultJson(const QJsonObject &claim, const QString &type, bool core,
                            const QString &result, const QString &reason,
                            const QStringList &supportingRefs)
{
    QJsonArray refs;
    for (const auto &ref : supportingRefs) refs.append(ref);
    return QJsonObject{{QStringLiteral("id"), claim.value(QStringLiteral("id")).toString()},
                       {QStringLiteral("type"), type},
                       {QStringLiteral("core"), core},
                       {QStringLiteral("result"), result},
                       {QStringLiteral("reason"), reason},
                       {QStringLiteral("supporting_refs"), refs}};
}

} // namespace

QString evidencePurposeName(EvidencePurpose purpose)
{
    switch (purpose) {
    case EvidencePurpose::HistoricalFact: return QStringLiteral("historical_fact");
    case EvidencePurpose::CurrentState: return QStringLiteral("current_state");
    case EvidencePurpose::NewEvidence: return QStringLiteral("new_evidence");
    case EvidencePurpose::UserCommitment: return QStringLiteral("user_commitment");
    case EvidencePurpose::ExecutionResult: return QStringLiteral("execution_result");
    case EvidencePurpose::ArtifactResult: return QStringLiteral("artifact_result");
    case EvidencePurpose::InternalState: return QStringLiteral("internal_state");
    }
    return QStringLiteral("internal_state");
}

bool evidencePurposeFromName(const QString &name, EvidencePurpose *purpose)
{
    if (!purpose) return false;
    const QString trimmed = name.trimmed();
    if (trimmed == QStringLiteral("historical_fact")) { *purpose = EvidencePurpose::HistoricalFact; return true; }
    if (trimmed == QStringLiteral("current_state")) { *purpose = EvidencePurpose::CurrentState; return true; }
    if (trimmed == QStringLiteral("new_evidence")) { *purpose = EvidencePurpose::NewEvidence; return true; }
    if (trimmed == QStringLiteral("user_commitment")) { *purpose = EvidencePurpose::UserCommitment; return true; }
    if (trimmed == QStringLiteral("execution_result")) { *purpose = EvidencePurpose::ExecutionResult; return true; }
    if (trimmed == QStringLiteral("artifact_result")) { *purpose = EvidencePurpose::ArtifactResult; return true; }
    if (trimmed == QStringLiteral("internal_state")) { *purpose = EvidencePurpose::InternalState; return true; }
    return false;
}

ActionPolicy::ActionPolicy(MemoryManager *memory)
    : m_memory(memory)
{
}

void ActionPolicy::setSemanticVerifier(SemanticVerifier verifier)
{
    m_verifier = std::move(verifier);
}

void ActionPolicy::setCoverageVerifier(CoverageVerifier verifier)
{
    m_coverageVerifier = std::move(verifier);
}

bool ActionPolicy::purposeForClaimType(const QString &type, EvidencePurpose *purpose)
{
    if (!purpose) return false;
    const QString trimmed = type.trimmed();
    if (trimmed == QStringLiteral("historical_fact")) { *purpose = EvidencePurpose::HistoricalFact; return true; }
    if (trimmed == QStringLiteral("current_state")) { *purpose = EvidencePurpose::CurrentState; return true; }
    if (trimmed == kReportedFact || trimmed == QStringLiteral("new_evidence")) {
        *purpose = EvidencePurpose::NewEvidence;
        return true;
    }
    if (trimmed == kUserCommitment) { *purpose = EvidencePurpose::UserCommitment; return true; }
    if (trimmed == kExecutionResult) { *purpose = EvidencePurpose::ExecutionResult; return true; }
    if (trimmed == kArtifactResult) { *purpose = EvidencePurpose::ArtifactResult; return true; }
    if (trimmed == kInference || trimmed == QStringLiteral("internal_state")) {
        *purpose = EvidencePurpose::InternalState;
        return true;
    }
    return false;
}

QString ActionPolicy::claimTypeForPurpose(EvidencePurpose purpose)
{
    switch (purpose) {
    case EvidencePurpose::HistoricalFact: return QStringLiteral("historical_fact");
    case EvidencePurpose::CurrentState: return QStringLiteral("current_state");
    case EvidencePurpose::NewEvidence: return QStringLiteral("new_evidence");
    case EvidencePurpose::UserCommitment: return QStringLiteral("user_commitment");
    case EvidencePurpose::ExecutionResult: return QStringLiteral("execution_result");
    case EvidencePurpose::ArtifactResult: return QStringLiteral("artifact_result");
    case EvidencePurpose::InternalState: return QStringLiteral("internal_state");
    }
    return QStringLiteral("internal_state");
}

QString ActionPolicy::originReportedText(const ActionOrigin &origin) const
{
    if (!m_memory || origin.id <= 0) return {};
    QSqlQuery row(m_memory->m_db);
    if (origin.kind == ActionOrigin::Wake) {
        // 唤醒时间：优先 started_at，其次 created_at（都是 unixepoch 秒）。
        row.prepare(QStringLiteral(
            "SELECT coalesce(datetime(started_at,'unixepoch'),datetime(created_at,'unixepoch')) "
            "FROM open_wake WHERE id=?"));
    } else if (origin.kind == ActionOrigin::Reflection || origin.kind == ActionOrigin::Concern) {
        row.prepare(QStringLiteral(
            "SELECT coalesce(s.reported,h.timestamp) "
            "FROM chat_history h LEFT JOIN cognitive_source s ON s.event=h.cognitive_event "
            "AND s.channel=h.channel AND s.session=h.session_id AND s.subject=h.subject "
            "AND s.input=h.user_input WHERE h.id=?"));
    } else {
        return {};
    }
    row.addBindValue(origin.id);
    if (!row.exec() || !row.next()) return {};
    return row.value(0).toString();
}

QString ActionPolicy::originSourceError(const ActionOrigin &origin, const QString &actionKind) const
{
    if (!m_memory) return QStringLiteral("source_unavailable");
    if (!origin.isValid()) return QStringLiteral("invalid_origin");
    if (origin.kind == ActionOrigin::Wake) {
        QSqlQuery row(m_memory->m_db);
        row.prepare(QStringLiteral(
            "SELECT version,coalesce(run_id,''),coalesce(scope_key,''),"
            "coalesce(current_context_epoch,0) FROM open_wake WHERE id=?"));
        row.addBindValue(origin.id);
        if (!row.exec() || !row.next()) return QStringLiteral("origin_not_found");
        if (row.value(0).toInt() != origin.version)
            return QStringLiteral("origin_version_mismatch");
        if (row.value(1).toString() != origin.runId)
            return QStringLiteral("origin_run_mismatch");
        if (row.value(2).toString() != origin.scope)
            return QStringLiteral("origin_scope_mismatch");
        if (row.value(3).toLongLong() != origin.contextEpoch)
            return QStringLiteral("context_epoch_mismatch");
        // OpenWake 的 contact 必须显式 allow_contact；enabled 不参与此判断（两者独立）。
        if (actionKind == kProposalKindContact
            && !ConfigManager::instance().openWakeAllowContact())
            return QStringLiteral("open_wake_contact_disabled");
        return {};
    }
    if (origin.kind == ActionOrigin::Reflection || origin.kind == ActionOrigin::Concern) {
        QSqlQuery row(m_memory->m_db);
        row.prepare(QStringLiteral(
            "SELECT coalesce(r.version,0),h.channel,h.session_id,h.subject,h.sensitive,h.turn_kind "
            "FROM private_reflection r JOIN chat_history h ON h.id=r.turn_id WHERE r.turn_id=?"));
        row.addBindValue(origin.id);
        if (!row.exec() || !row.next()) return QStringLiteral("origin_not_found");
        if (row.value(0).toInt() != origin.version)
            return QStringLiteral("origin_version_mismatch");
        if (row.value(3).toString() != QStringLiteral("user") || row.value(4).toBool()
            || row.value(5).toString() != QStringLiteral("reply"))
            return QStringLiteral("origin_not_owner_source");
        if (!channelVisibleFromOwnerPrivate(row.value(1).toString(), row.value(2).toString()))
            return QStringLiteral("origin_invisible");
        return {};
    }
    if (origin.kind == ActionOrigin::Thought || origin.kind == ActionOrigin::Activity) {
        const RefEvidence artifact = readArtifactRef(m_memory->m_db, origin.ref);
        if (!artifact.ok) return artifact.reason;
        // 分享版本必须是成果版本：不允许把随机数/旧版本写成 action_key。
        if (artifact.artifactVersion != origin.version)
            return QStringLiteral("artifact_version_mismatch");
        if (origin.scope != QStringLiteral("owner")) return QStringLiteral("origin_scope_mismatch");
        return {};
    }
    return QStringLiteral("origin_kind_not_allowed");
}

ClaimValidation ActionPolicy::validateClaimForPurpose(const QString &channel, const QString &session,
                                                      const ActionOrigin &origin,
                                                      EvidencePurpose purpose,
                                                      const QJsonObject &claim) const
{
    const auto insufficient = [](const QString &reason) {
        ClaimValidation validation;
        validation.result = ClaimValidation::Insufficient;
        validation.reason = reason;
        return validation;
    };
    if (!m_memory) return insufficient(QStringLiteral("db_error"));
    if (!origin.isValid()) return insufficient(QStringLiteral("invalid_origin"));
    const QString type = claim.value(QStringLiteral("type")).toString().trimmed();
    EvidencePurpose derived = EvidencePurpose::HistoricalFact;
    if (!purposeForClaimType(type, &derived))
        return insufficient(QStringLiteral("invalid_claim_type"));
    if (derived != purpose) return insufficient(QStringLiteral("purpose_mismatch"));
    const QString declaredPurpose = claim.value(QStringLiteral("purpose")).toString().trimmed();
    if (!declaredPurpose.isEmpty() && declaredPurpose != evidencePurposeName(purpose))
        return insufficient(QStringLiteral("purpose_mismatch"));
    QString originReported;
    if (purpose == EvidencePurpose::NewEvidence || purpose == EvidencePurpose::CurrentState) {
        originReported = originReportedText(origin);
        if (originReported.isEmpty()) return insufficient(QStringLiteral("origin_not_found"));
    }
    Q_UNUSED(channel);
    Q_UNUSED(session);
    ClaimValidation validation = validateClaimRefsWithOrigin(m_memory->m_db, purpose, claim,
                                                             originReported);
    if (validation.result == ClaimValidation::Entailed && validation.purpose.isEmpty())
        validation.purpose = evidencePurposeName(purpose);
    return validation;
}

ClaimValidation ActionPolicy::validateEvidenceRef(const QString &channel, const QString &session,
                                                  const ActionOrigin &origin,
                                                  EvidencePurpose purpose,
                                                  const QString &refText) const
{
    const auto insufficient = [](const QString &reason) {
        ClaimValidation validation;
        validation.result = ClaimValidation::Insufficient;
        validation.reason = reason;
        return validation;
    };
    if (!m_memory) return insufficient(QStringLiteral("db_error"));
    if (!origin.isValid()) return insufficient(QStringLiteral("invalid_origin"));
    const QString ref = refText.trimmed();
    if (ref.isEmpty()) return insufficient(QStringLiteral("invalid_ref"));
    QString originReported;
    if (purpose == EvidencePurpose::NewEvidence || purpose == EvidencePurpose::CurrentState) {
        originReported = originReportedText(origin);
        if (originReported.isEmpty()) return insufficient(QStringLiteral("origin_not_found"));
    }
    Q_UNUSED(channel);
    Q_UNUSED(session);
    const RefEvidence evidence = readPurposeRef(m_memory->m_db, purpose, ref, originReported);
    if (!evidence.ok) return insufficient(evidence.reason);
    ClaimValidation validation;
    validation.result = ClaimValidation::Entailed;
    validation.purpose = evidencePurposeName(purpose);
    validation.supportingRefs.append(ref);
    return validation;
}

ClaimValidation ActionPolicy::validateClaimDeterministic(const QString &channel, const QString &session,
                                                         qlonglong concernTurn,
                                                         const QJsonObject &claim) const
{
    const auto insufficient = [](const QString &reason) {
        ClaimValidation validation;
        validation.result = ClaimValidation::Insufficient;
        validation.reason = reason;
        return validation;
    };
    if (!m_memory || !claim.value(QStringLiteral("type")).isString())
        return insufficient(QStringLiteral("invalid_claim"));
    const QString type = claim.value(QStringLiteral("type")).toString().trimmed();
    EvidencePurpose purpose = EvidencePurpose::HistoricalFact;
    if (!purposeForClaimType(type, &purpose))
        return insufficient(QStringLiteral("invalid_claim_type"));
    // 旧入口的"关注来源"语义：concernTurn>0 时来源必须是真实主人轮次，并作为新证据时序基准。
    QString originReported;
    if (concernTurn > 0) {
        EvidenceReader reader(m_memory->m_db, channel, session, concernTurn);
        if (!reader.originReason().isEmpty()) return insufficient(reader.originReason());
        originReported = reader.originReported();
    }
    return validateClaimRefsWithOrigin(m_memory->m_db, purpose, claim, originReported);
}

QString ActionPolicy::validateMotivation(const QString &channel, const QString &session,
                                         qlonglong concernTurn, const QJsonObject &motivation) const
{
    if (!m_memory || !motivation.value(QStringLiteral("type")).isString())
        return QStringLiteral("missing_motivation");
    const auto type = motivation.value(QStringLiteral("type")).toString().trimmed();
    if (type != kSelfDesire && type != kConcernFollowup && type != kActivityShare)
        return QStringLiteral("invalid_motivation_type");
    if (motivation.value(QStringLiteral("summary")).toString().trimmed().isEmpty())
        return QStringLiteral("empty_motivation_summary");
    const auto ref = motivation.value(QStringLiteral("ref")).toString().trimmed();
    Q_UNUSED(concernTurn);
    if (type == kSelfDesire) {
        // 自发动机只需要这一次回顾里合法存在；不要求任何主人新证据。
        return {};
    }
    if (type == kConcernFollowup) {
        if (!ref.startsWith(QStringLiteral("reflection/"))
            && !ref.startsWith(QStringLiteral("concern/")))
            return QStringLiteral("invalid_motivation_ref");
        qlonglong turn = 0;
        if (ref.startsWith(QStringLiteral("reflection/")))
            turn = ref.mid(QStringLiteral("reflection/").size()).section(':', 0, 0).toLongLong();
        else
            turn = ref.mid(QStringLiteral("concern/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(m_memory->m_db);
        // 当前正在回顾的这条关注本身是 state='pending'；到期/闲置时是 'consider'。
        // 两者都算"仍未了结的开放关注"；已 resolve/drop 的不算。
        row.prepare("SELECT h.channel,h.session_id,r.state,r.turn_id FROM private_reflection r "
                    "JOIN chat_history h ON h.id=r.turn_id WHERE (r.turn_id=? OR r.root_turn=?) "
                    "AND h.subject='user' AND h.sensitive=0 "
                    "AND r.state IN ('consider','pending') LIMIT 1");
        row.addBindValue(turn);
        row.addBindValue(turn);
        if (!row.exec() || !row.next()) return QStringLiteral("motivation_concern_not_found");
        if (!visibleSession(row.value(0).toString(), row.value(1).toString(), channel, session))
            return QStringLiteral("motivation_ref_invisible");
        return {};
    }
    // activity_share：必须是 completed 且有真实成果的活动（legacy thought/<id> 与
    // internal activity/<id> 都支持）。内部想法/未完成目标不能当成果分享。
    ActivityRef activity;
    QString parseError;
    if (!ActivityRef::parse(ref, &activity, &parseError))
        return QStringLiteral("invalid_motivation_ref");
    const QJsonObject view = m_memory->activityByRef(activity.ref());
    if (view.isEmpty()) return QStringLiteral("motivation_artifact_not_found");
    if (!visibleSession(view.value(QStringLiteral("channel")).toString(),
                        view.value(QStringLiteral("session")).toString(), channel, session))
        return QStringLiteral("motivation_ref_invisible");
    if (view.value(QStringLiteral("status")).toString() != QStringLiteral("completed"))
        return QStringLiteral("motivation_artifact_not_completed");
    const QString artifactRef = view.value(QStringLiteral("artifact_ref")).toString().trimmed();
    const QString draft = view.value(QStringLiteral("draft")).toString().trimmed();
    if (artifactRef != activity.ref() || draft.isEmpty())
        return QStringLiteral("motivation_artifact_not_completed");
    return {};
}

QString ActionPolicy::refExcerpt(const QString &ref, int maxChars) const
{
    if (!m_memory || ref.isEmpty()) return {};
    const auto truncate = [maxChars](const QString &text) { return text.left(qMax(0, maxChars)); };
    if (ref.startsWith(QStringLiteral("chat/"))) {
        const auto turn = ref.mid(5).section(':', 0, 0).toLongLong();
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT coalesce(user_input,''),coalesce(raw_reply,'') FROM chat_history WHERE id=?");
        row.addBindValue(turn);
        if (!row.exec() || !row.next()) return {};
        const auto input = row.value(0).toString();
        return truncate(input.isEmpty() ? replyText(row.value(1).toString()) : input);
    }
    if (ref.startsWith(QStringLiteral("source/"))) {
        const auto id = ref.mid(QStringLiteral("source/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT input FROM cognitive_source WHERE id=?");
        row.addBindValue(id);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    if (ref.startsWith(QStringLiteral("raw_event/"))) {
        const auto id = ref.mid(QStringLiteral("raw_event/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT coalesce(content,'') FROM raw_events WHERE id=?");
        row.addBindValue(id);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    if (ref.startsWith(QStringLiteral("reflection/"))) {
        const auto turn = ref.mid(QStringLiteral("reflection/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT coalesce(note,'') FROM private_reflection WHERE turn_id=?");
        row.addBindValue(turn);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    if (ref.startsWith(QStringLiteral("matter/")) || ref.startsWith(QStringLiteral("revision/"))) {
        const auto matter = ref.startsWith(QStringLiteral("matter/"))
            ? ref.mid(QStringLiteral("matter/").size()).section(':', 0, 0)
            : ref.mid(QStringLiteral("revision/").size()).section('/', 0, 0);
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT understanding FROM matter_revision WHERE matter=? ORDER BY version DESC LIMIT 1");
        row.addBindValue(matter);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    if (ref.startsWith(QStringLiteral("thought/"))) {
        const auto id = ref.mid(QStringLiteral("thought/").size()).section(':', 0, 0).toLongLong();
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT thought FROM chat_history WHERE id=?");
        row.addBindValue(id);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    if (ref.startsWith(QStringLiteral("delivery/"))) {
        const auto key = ref.mid(QStringLiteral("delivery/").size());
        QSqlQuery row(m_memory->m_db);
        row.prepare("SELECT payload_json FROM action_outbox WHERE action_key=? OR id=? "
                    "ORDER BY action_key=? DESC LIMIT 1");
        row.addBindValue(key);
        row.addBindValue(key.toLongLong());
        row.addBindValue(key);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    return {};
}

QString ActionPolicy::proposalEvidenceDigest(const QJsonArray &claims) const
{
    QJsonArray sources;
    return proposalEvidence(claims, &sources) ? canonicalEvidenceDigest(claims, sources) : QString();
}

bool ActionPolicy::proposalEvidence(const QJsonArray &claims, QJsonArray *sources) const
{
    QMap<QString, bool> references;
    for (const auto &claim : factualClaimsForVerification(claims)) {
        const auto fields = claim.toObject();
        const bool execution = fields.value(QStringLiteral("type")).toString().trimmed() == kExecutionResult;
        for (const auto &value : fields.value(QStringLiteral("refs")).toArray()) {
            const auto ref = value.toString().trimmed();
            if (references.contains(ref) && references.value(ref) != execution)
                return false;
            references.insert(ref, execution);
        }
    }
    QJsonArray evidence;
    for (auto ref = references.cbegin(); ref != references.cend(); ++ref) {
        QString excerpt;
        if (ref.value() && ref.key().startsWith(QStringLiteral("chat/"))) {
            QSqlQuery row(m_memory->m_db);
            row.prepare("SELECT raw_reply FROM chat_history WHERE id=? AND turn_state='handed_off'");
            row.addBindValue(ref.key().mid(5).section(':', 0, 0).toLongLong());
            if (!row.exec() || !row.next()) return false;
            excerpt = replyText(row.value(0).toString());
        } else {
            excerpt = refExcerpt(ref.key(), std::numeric_limits<int>::max());
        }
        if (excerpt.trimmed().isEmpty()) return false;
        evidence.append(QJsonObject{{QStringLiteral("ref"), ref.key()},
                                    {QStringLiteral("text"), excerpt}});
    }
    *sources = evidence;
    return true;
}

QJsonArray ActionPolicy::claimSources(const QString &channel, const QString &session,
                                      qlonglong concernTurn, const QJsonArray &claims) const
{
    Q_UNUSED(channel);
    Q_UNUSED(session);
    Q_UNUSED(concernTurn);
    QJsonArray sources;
    QSet<QString> seen;
    int budget = 3000; // verifier 输入预算的字符近似：来源总计不超过 3000 字符
    for (const auto &value : claims) {
        for (const auto &refValue : value.toObject().value(QStringLiteral("refs")).toArray()) {
            const auto ref = refValue.toString().trimmed();
            if (ref.isEmpty() || seen.contains(ref)) continue;
            seen.insert(ref);
            QString text = refExcerpt(ref, 512);
            if (text.size() > budget) text = text.left(qMax(0, budget));
            budget -= text.size();
            sources.append(QJsonObject{{QStringLiteral("ref"), ref},
                                       {QStringLiteral("text"), text}});
            if (sources.size() >= 4) return sources;
            if (budget <= 0) return sources;
        }
    }
    return sources;
}

QJsonObject ActionPolicy::evaluateSpontaneousContactDeterministic(const QString &channel,
                                                                  const QString &session,
                                                                  qlonglong concernTurn,
                                                                  const QJsonObject &contact) const
{
    QJsonObject result{{QStringLiteral("allowed"), false},
                       {QStringLiteral("reason"), QStringLiteral("invalid_contact")},
                       {QStringLiteral("needs_verifier"), false},
                       {QStringLiteral("verifier_sources"), QJsonArray{}},
                       {QStringLiteral("claim_results"), QJsonArray{}},
                       {QStringLiteral("repairable_claims"), QJsonArray{}},
                       {QStringLiteral("semantic_status"), QStringLiteral("withheld")}};
    if (!m_memory || !contact.value(QStringLiteral("motivation")).isObject())
        return result;
    const auto options = ConfigManager::instance().getProactiveConfig();
    const auto motivation = contact.value(QStringLiteral("motivation")).toObject();
    const auto motivationReason = validateMotivation(channel, session, concernTurn, motivation);
    if (!motivationReason.isEmpty()) {
        result[QStringLiteral("reason")] = motivationReason;
        return result;
    }
    const auto motivationType = motivation.value(QStringLiteral("type")).toString();
    result[QStringLiteral("motivation_type")] = motivationType;

    QJsonArray claimResults;
    QJsonArray unverifiedFactual;
    bool deterministicFailure = false;
    QString failureReason;
    for (const auto &value : contact.value(QStringLiteral("claims")).toArray()) {
        const auto claim = value.toObject();
        const auto type = claim.value(QStringLiteral("type")).toString();
        const bool core = claimIsCoreByType(type) || claimMarkedCore(claim, contact);
        const auto validation = validateClaimDeterministic(channel, session, concernTurn, claim);
        if (validation.result != ClaimValidation::Entailed) {
            // 确定性校验失败（ref 不存在/authority 不允许/时序不成立）不是"措辞可修复"的问题：
            // 整条联系不能带着编造的事实发出去。
            claimResults.append(claimResultJson(claim, type, core,
                                                QStringLiteral("insufficient"),
                                                validation.reason, validation.supportingRefs));
            deterministicFailure = true;
            if (failureReason.isEmpty()) failureReason = validation.reason;
            continue;
        }
        claimResults.append(claimResultJson(claim, type, core,
                                            isFactClaimType(type) ? QStringLiteral("unverified")
                                                                  : QStringLiteral("entailed"),
                                            QString(), validation.supportingRefs));
        if (isFactClaimType(type)) unverifiedFactual.append(claim);
    }
    result[QStringLiteral("claim_results")] = claimResults;
    result[QStringLiteral("needs_verifier")] = !unverifiedFactual.isEmpty();
    result[QStringLiteral("verifier_sources")] = claimSources(channel, session, concernTurn, unverifiedFactual);
    if (deterministicFailure) {
        result[QStringLiteral("reason")] = QStringLiteral("deterministic:%1").arg(failureReason);
        return result;
    }
    if (quietNow(options)) {
        result[QStringLiteral("reason")] = QStringLiteral("quiet_hours");
        return result;
    }
    if (!options.enabled || !targetAllowed(options, channel, session)) {
        result[QStringLiteral("reason")] = QStringLiteral("no_target");
        return result;
    }
    // 冷却：上一次主动外发还没过最小间隔；只是"想联系"不能突破。
    if (options.cooldownSeconds > 0) {
        QSqlQuery last(m_memory->m_db);
        last.prepare("SELECT 1 FROM cognitive_action WHERE kind IN ('SpontaneousContact','ActivityShare') "
                     "AND state NOT IN ('withheld','unknown') "
                     "AND unixepoch()-created_at < ? LIMIT 1");
        last.addBindValue(options.cooldownSeconds);
        if (last.exec() && last.next()) {
            result[QStringLiteral("reason")] = QStringLiteral("cooldown");
            return result;
        }
    }
    // 重复 action：同一联系意图已经提交过，就不再重开一次。
    {
        qlonglong version = contact.value(QStringLiteral("version")).toInteger();
        if (version <= 0 && concernTurn > 0) {
            QSqlQuery row(m_memory->m_db);
            row.prepare("SELECT version FROM private_reflection WHERE turn_id=?");
            row.addBindValue(concernTurn);
            if (row.exec() && row.next()) version = row.value(0).toLongLong();
        }
        if (concernTurn > 0 && version > 0) {
            QSqlQuery duplicate(m_memory->m_db);
            duplicate.prepare("SELECT 1 FROM cognitive_action WHERE action_key=? AND state<>'withheld' LIMIT 1");
            duplicate.addBindValue(QStringLiteral("reflection:%1:%2:contact")
                                       .arg(concernTurn).arg(version));
            if (duplicate.exec() && duplicate.next()) {
                result[QStringLiteral("reason")] = QStringLiteral("duplicate_action");
                return result;
            }
        }
    }
    result[QStringLiteral("allowed")] = true;
    result[QStringLiteral("reason")] = QStringLiteral("ok");
    result[QStringLiteral("semantic_status")] =
        unverifiedFactual.isEmpty() ? QStringLiteral("not_required") : QStringLiteral("unverified");
    return result;
}

QJsonObject ActionPolicy::evaluateSpontaneousContact(const QString &channel, const QString &session,
                                                     qlonglong concernTurn,
                                                     const QJsonObject &contact) const
{
    auto result = evaluateSpontaneousContactDeterministic(channel, session, concernTurn, contact);
    if (!result.value(QStringLiteral("allowed")).toBool())
        return result;
    if (!result.value(QStringLiteral("needs_verifier")).toBool()) {
        result[QStringLiteral("semantic_status")] = QStringLiteral("not_required");
        return result;
    }
    const auto sources = result.value(QStringLiteral("verifier_sources")).toArray();
    QStringList claimTexts;
    QString combinedType;
    bool hasCore = false;
    for (const auto &value : contact.value(QStringLiteral("claims")).toArray()) {
        const auto claim = value.toObject();
        const auto type = claim.value(QStringLiteral("type")).toString();
        if (!isFactClaimType(type)) continue;
        claimTexts.append(claim.value(QStringLiteral("text")).toString());
        if (combinedType.isEmpty() || claimIsCoreByType(type) || claimMarkedCore(claim, contact))
            combinedType = type;
        if (claimIsCoreByType(type) || claimMarkedCore(claim, contact)) hasCore = true;
    }
    const QString claimText = claimTexts.join(QStringLiteral("\n---\n")).left(1500);
    if (!m_verifier) {
        result[QStringLiteral("allowed")] = false;
        result[QStringLiteral("reason")] = QStringLiteral("verifier_unavailable");
        result[QStringLiteral("semantic_status")] = QStringLiteral("withheld");
        return result;
    }
    if (ConfigManager::instance().getSemanticVerifierConfig().maxVerifierCalls < 1) {
        result[QStringLiteral("allowed")] = false;
        result[QStringLiteral("reason")] = QStringLiteral("verifier_unavailable");
        result[QStringLiteral("semantic_status")] = QStringLiteral("withheld");
        return result;
    }
    // 预算硬约束：一次 action 最多一次语义核验。把全部事实 claim 作为同一个核验单元，
    // 共同引用最多 4 条来源摘要，避免逐条 judge 串行放大成本。
    ClaimValidation validation;
    try {
        validation = m_verifier(combinedType.isEmpty() ? kReportedFact : combinedType,
                                claimText, sources);
    } catch (...) {
        validation = ClaimValidation{};
        validation.result = ClaimValidation::Insufficient;
        validation.reason = QStringLiteral("verifier_exception");
    }
    auto claimResults = result.value(QStringLiteral("claim_results")).toArray();
    for (int i = 0; i < claimResults.size(); ++i) {
        auto row = claimResults.at(i).toObject();
        if (row.value(QStringLiteral("result")).toString() != QStringLiteral("unverified")) continue;
        row[QStringLiteral("result")] = validation.result == ClaimValidation::Entailed
            ? QStringLiteral("entailed")
            : validation.result == ClaimValidation::Contradicted ? QStringLiteral("contradicted")
                                                                 : QStringLiteral("insufficient");
        row[QStringLiteral("reason")] = validation.reason;
        QJsonArray refs;
        for (const auto &ref : validation.supportingRefs) refs.append(ref);
        row[QStringLiteral("supporting_refs")] = refs;
        claimResults.replace(i, row);
    }
    result[QStringLiteral("claim_results")] = claimResults;
    if (validation.result == ClaimValidation::Entailed) {
        result[QStringLiteral("semantic_status")] = QStringLiteral("entailed");
        return result;
    }
    if (hasCore) {
        result[QStringLiteral("allowed")] = false;
        result[QStringLiteral("reason")] = validation.result == ClaimValidation::Contradicted
            ? QStringLiteral("verifier_contradicted") : QStringLiteral("verifier_insufficient");
        result[QStringLiteral("semantic_status")] = QStringLiteral("withheld");
        return result;
    }
    // 非核心事实 claim：联系本身可以保留，但这条夹带的事实不能当作已证实的声称。
    // 调用方（LLMService）据此做一次 constrained repair（删除或改成不确定表达）。
    result[QStringLiteral("semantic_status")] = QStringLiteral("repair_needed");
    result[QStringLiteral("repair_reason")] = validation.reason;
    result[QStringLiteral("repairable_claims")] = contact.value(QStringLiteral("claims")).toArray();
    return result;
}

CoverageCheck ActionPolicy::deterministicCoverageCheck(const QString &message,
                                                       const QString &reason,
                                                       const QJsonArray &claims) const
{
    CoverageCheck check;
    const QString text = message + QLatin1Char('\n') + reason;
    if (text.trimmed().isEmpty()) return check;
    QStringList declaredRefs;
    bool hasFactualClaim = false;
    for (const auto &value : claims) {
        const QJsonObject claim = value.toObject();
        for (const auto &ref : claim.value(QStringLiteral("refs")).toArray()) {
            const QString declared = ref.toString().trimmed();
            if (!declared.isEmpty()) declaredRefs.append(declared);
        }
        EvidencePurpose purpose = EvidencePurpose::HistoricalFact;
        if (purposeForClaimType(claim.value(QStringLiteral("type")).toString(), &purpose)
            && purpose != EvidencePurpose::InternalState)
            hasFactualClaim = true;
    }
    // 1) 自然语言里出现的显式引用必须被 declared claims 覆盖
    //    （内部记录引用 reflection/concern/wake/matter/revision 不算外部事实）。
    static const QRegularExpression refToken(QStringLiteral(
        "(chat|source|raw_event|delivery|thought|activity|reflection|concern|reminder|matter|revision)/[0-9A-Za-z_:.\\-]+"));
    auto matches = refToken.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString token = match.captured(0);
        if (token.startsWith(QStringLiteral("reflection/"))
            || token.startsWith(QStringLiteral("concern/"))
            || token.startsWith(QStringLiteral("wake/"))
            || token.startsWith(QStringLiteral("matter/"))
            || token.startsWith(QStringLiteral("revision/")))
            continue;
        if (declaredRefs.contains(token)) continue;
        check.complete = false;
        if (check.reason.isEmpty()) check.reason = QStringLiteral("undeclared_reference");
        if (!check.undeclared.contains(token)) check.undeclared.append(token);
        if (check.undeclared.size() >= 4) break;
    }
    // 2) 外部事实/承诺断言：没有任何事实 claim 覆盖时不能直接放行（claims=[] 同样适用）。
    if (!hasFactualClaim) {
        const auto assertions = externalFactAssertions(text);
        if (!assertions.isEmpty()) {
            check.complete = false;
            if (check.reason.isEmpty()) check.reason = QStringLiteral("undeclared_external_fact");
            for (const auto &fragment : assertions) {
                if (!check.undeclared.contains(fragment)) check.undeclared.append(fragment);
                if (check.undeclared.size() >= 4) break;
            }
        }
    }
    return check;
}

CoverageCheck ActionPolicy::verifyContactCoverage(const QString &message, const QString &reason,
                                                  const QJsonArray &claims,
                                                  const ActionOrigin &origin) const
{
    CoverageCheck check = deterministicCoverageCheck(message, reason, claims);
    if (!check.complete) return check;
    // 默认没有真实模型：确定性检测已完成；有注入的 coverage verifier 时只调用一次。
    if (!m_coverageVerifier) return check;
    CoverageCheck semantic;
    try {
        semantic = m_coverageVerifier(message, reason, claims, origin);
    } catch (...) {
        CoverageCheck failed;
        failed.complete = false;
        failed.reason = QStringLiteral("coverage_verifier_exception");
        return failed;
    }
    if (!semantic.complete) {
        if (semantic.reason.isEmpty()) semantic.reason = QStringLiteral("claim_coverage_incomplete");
        return semantic;
    }
    semantic.semantic = true; // 单次动作内至多一次语义 coverage 核验
    return semantic;
}

QJsonObject ActionPolicy::validateActionProposal(const QJsonObject &proposal,
                                                 QString *error) const
{
    if (error) error->clear();
    QJsonArray claimResults;
    const auto fail = [error, &claimResults](const QString &reason) {
        if (error) *error = reason;
        return QJsonObject{{QStringLiteral("allowed"), false},
                           {QStringLiteral("decision"), QStringLiteral("withheld")},
                           {QStringLiteral("reason"), reason},
                           {QStringLiteral("semantic_status"), QStringLiteral("withheld")},
                           {QStringLiteral("claims"), claimResults}};
    };
    if (!m_memory) return fail(QStringLiteral("source_unavailable"));
    if (!proposal.value(QStringLiteral("origin")).isObject())
        return fail(QStringLiteral("invalid_origin"));
    // 1) 来源身份：严格解析（服务端字段不允许伪造）。
    ActionOrigin origin;
    QString originError;
    if (!ActionOrigin::fromJson(proposal.value(QStringLiteral("origin")).toObject(),
                                &origin, &originError)) {
        QJsonObject result = fail(QStringLiteral("invalid_origin"));
        result.insert(QStringLiteral("detail"), originError);
        return result;
    }
    if (!origin.hasTarget()) return fail(QStringLiteral("invalid_target"));
    // 2) kind / action_key：action_key 必须由 origin+kind 派生。
    const QString kind = proposal.value(QStringLiteral("kind")).toString().trimmed();
    if (kind != kProposalKindContact && kind != kProposalKindActivityShare)
        return fail(QStringLiteral("invalid_kind"));
    const QString expectedKey = origin.actionKey(kind);
    if (expectedKey.isEmpty()) return fail(QStringLiteral("origin_kind_not_allowed"));
    const QString providedKey = proposal.value(QStringLiteral("action_key")).toString().trimmed();
    if (!providedKey.isEmpty() && providedKey != expectedKey)
        return fail(QStringLiteral("action_key_mismatch"));
    // 3) target 从 origin 生成；调用方给出时必须完全一致，且必须落在主人私聊域。
    const QJsonObject providedTarget = proposal.value(QStringLiteral("target")).toObject();
    if (!providedTarget.isEmpty()) {
        const QString channel = providedTarget.value(QStringLiteral("channel")).toString().trimmed();
        QString session = providedTarget.value(QStringLiteral("session_id")).toString().trimmed();
        if (session.isEmpty())
            session = providedTarget.value(QStringLiteral("session")).toString().trimmed();
        if ((!channel.isEmpty() && channel != origin.targetChannel)
            || (!session.isEmpty() && session != origin.targetSession))
            return fail(QStringLiteral("target_mismatch"));
    }
    if (!channelVisibleFromOwnerPrivate(origin.targetChannel, origin.targetSession))
        return fail(QStringLiteral("target_not_visible"));
    // 4) 来源真实身份（wake/reflection/concern/成果）+ allow_contact。
    const QString sourceError = originSourceError(origin, kind);
    if (!sourceError.isEmpty()) return fail(sourceError);
    // 5) payload / claims。
    if (!proposal.value(QStringLiteral("payload")).isObject())
        return fail(QStringLiteral("invalid_payload"));
    const QJsonObject payload = proposal.value(QStringLiteral("payload")).toObject();
    const QJsonValue claimsValue = proposal.value(QStringLiteral("claims"));
    if (!claimsValue.isUndefined() && !claimsValue.isArray())
        return fail(QStringLiteral("invalid_claims"));
    const QJsonArray claims = claimsValue.toArray();
    if (proposal.contains(QStringLiteral("expires_at"))
        && proposal.value(QStringLiteral("expires_at")).toInteger(0) < 0)
        return fail(QStringLiteral("invalid_expires_at"));
    bool needsVerifier = false;
    bool anyUnknown = false;
    // R12-2c-final-1：claim 被拒时记录 type/purpose/ref/report 时间/来源基准时间/原因码。
    // 只记字段与时间，不记 claim/message 正文；用于判断"历史事实被当成新证据"这类问题。
    const auto logClaimRejected = [this, &origin](const QJsonObject &claim, const QString &type,
                                                  EvidencePurpose purpose,
                                                  const ClaimValidation &validation) {
        QString ref = validation.evidenceRef;
        if (ref.isEmpty()) {
            for (const auto &value : claim.value(QStringLiteral("refs")).toArray()) {
                const QString candidate = value.toString().trimmed();
                if (!candidate.isEmpty()) {
                    ref = candidate;
                    break;
                }
            }
        }
        QString originReported = validation.originReported;
        if (originReported.isEmpty()) originReported = originReportedText(origin);
        const QString purposeText = validation.purpose.isEmpty()
            ? evidencePurposeName(purpose) : validation.purpose;
        qInfo().noquote()
            << QStringLiteral("[Evidence] claim_rejected type=%1 purpose=%2 ref=%3"
                              " source_reported=%4 origin_reported=%5 reason=%6")
                   .arg(type.isEmpty() ? QStringLiteral("-") : type,
                        purposeText,
                        ref.isEmpty() ? QStringLiteral("-") : ref,
                        validation.evidenceReported.isEmpty()
                            ? QStringLiteral("-") : validation.evidenceReported,
                        originReported.isEmpty() ? QStringLiteral("-") : originReported,
                        validation.reason.isEmpty()
                            ? QStringLiteral("-") : validation.reason);
    };
    for (const auto &value : claims) {
        if (!value.isObject()) return fail(QStringLiteral("invalid_claims"));
        const QJsonObject claim = value.toObject();
        const QString type = claim.value(QStringLiteral("type")).toString().trimmed();
        EvidencePurpose purpose = EvidencePurpose::HistoricalFact;
        if (!purposeForClaimType(type, &purpose)) {
            logClaimRejected(claim, type, EvidencePurpose::HistoricalFact,
                             ClaimValidation{});
            return fail(QStringLiteral("invalid_claim_type"));
        }
        const QString declaredPurpose = claim.value(QStringLiteral("purpose")).toString().trimmed();
        if (!declaredPurpose.isEmpty() && declaredPurpose != evidencePurposeName(purpose)) {
            logClaimRejected(claim, type, purpose,
                             ClaimValidation{ClaimValidation::Insufficient,
                                             QStringLiteral("purpose_mismatch"), {}, false,
                                             evidencePurposeName(purpose)});
            return fail(QStringLiteral("purpose_mismatch"));
        }
        const ClaimValidation validation = validateClaimForPurpose(
            origin.targetChannel, origin.targetSession, origin, purpose, claim);
        QJsonArray supporting;
        for (const auto &ref : validation.supportingRefs) supporting.append(ref);
        claimResults.append(QJsonObject{
            {QStringLiteral("id"), claim.value(QStringLiteral("id")).toString()},
            {QStringLiteral("type"), type},
            {QStringLiteral("purpose"), evidencePurposeName(purpose)},
            {QStringLiteral("core"), claim.value(QStringLiteral("core")).toBool()},
            {QStringLiteral("result"),
             validation.unknown ? QStringLiteral("unknown")
                 : validation.result == ClaimValidation::Entailed ? QStringLiteral("entailed")
                                                                  : QStringLiteral("insufficient")},
            {QStringLiteral("reason"), validation.reason},
            {QStringLiteral("supporting_refs"), supporting}});
        if (validation.unknown) {
            anyUnknown = true;
            continue;
        }
        if (validation.result != ClaimValidation::Entailed) {
            logClaimRejected(claim, type, purpose, validation);
            return fail(validation.reason);
        }
        // 内部状态只能证明她内部有过记录，不能成为对外动作核心/事实来源。
        if (purpose == EvidencePurpose::InternalState
            && claim.value(QStringLiteral("core")).toBool()) {
            logClaimRejected(claim, type, purpose, validation);
            return fail(QStringLiteral("internal_state_core"));
        }
        if (purpose != EvidencePurpose::InternalState
            && purpose != EvidencePurpose::ArtifactResult)
            needsVerifier = true;
    }
    // 6) kind 专属 payload 检查（只核验，不发送）。
    QString coverageMessage = payload.value(QStringLiteral("message")).toString();
    const QString coverageReason = payload.value(QStringLiteral("reason")).toString();
    if (kind == kProposalKindContact) {
        if (coverageMessage.trimmed().isEmpty()) return fail(QStringLiteral("missing_message"));
        if (coverageMessage.size() > 1000) return fail(QStringLiteral("message_too_long"));
    } else {
        const QString draft = payload.value(QStringLiteral("draft")).toString().trimmed();
        if (draft.isEmpty()) return fail(QStringLiteral("missing_artifact_draft"));
        // 分享正文必须绑定真实成果（版本/哈希由服务端落库，模型不能自造成果）。
        const QJsonObject view = m_memory->activityByRef(origin.ref);
        if (view.isEmpty()) return fail(QStringLiteral("artifact_not_found"));
        const QString storedHash = view.value(QStringLiteral("artifact_hash")).toString().trimmed();
        const QString storedDraft = view.value(QStringLiteral("draft")).toString().trimmed();
        if (!storedHash.isEmpty()) {
            if (artifactDraftHash(draft) != storedHash)
                return fail(QStringLiteral("artifact_hash_mismatch"));
        } else if (!storedDraft.isEmpty() && storedDraft != draft) {
            return fail(QStringLiteral("artifact_draft_mismatch"));
        }
    }
    // 7) coverage：claims=[] 也必须执行（确定性检测 + 至多一次语义 coverage verifier）。
    const CoverageCheck coverage = verifyContactCoverage(coverageMessage, coverageReason,
                                                         claims, origin);
    QJsonArray undeclared;
    for (const auto &item : coverage.undeclared) undeclared.append(item);
    const QJsonObject coverageJson{
        {QStringLiteral("complete"), coverage.complete},
        {QStringLiteral("semantic"), coverage.semantic},
        {QStringLiteral("reason"), coverage.reason},
        {QStringLiteral("undeclared"), undeclared}};
    if (!coverage.complete) {
        QJsonObject result = fail(coverage.reason.isEmpty()
                                      ? QStringLiteral("claim_coverage_incomplete")
                                      : coverage.reason);
        result.insert(QStringLiteral("coverage"), coverageJson);
        result.insert(QStringLiteral("needs_verifier"), needsVerifier);
        return result;
    }
    // 8) 允许：确定性/成果/coverage 已通过；真实语义蕴含留给上层（本片不接模型）。
    QString semanticStatus = QStringLiteral("not_required");
    if (needsVerifier) semanticStatus = QStringLiteral("unverified");
    else if (anyUnknown) semanticStatus = QStringLiteral("unknown");
    QJsonObject result{{QStringLiteral("allowed"), true},
                       {QStringLiteral("decision"), QStringLiteral("allowed")},
                       {QStringLiteral("reason"), QStringLiteral("ok")},
                       {QStringLiteral("action_key"), expectedKey},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("origin"), origin.toJson()},
                       {QStringLiteral("target"),
                        QJsonObject{{QStringLiteral("channel"), origin.targetChannel},
                                    {QStringLiteral("session_id"), origin.targetSession}}},
                       {QStringLiteral("action_origin_target_channel"), origin.targetChannel},
                       {QStringLiteral("action_origin_target_session"), origin.targetSession},
                       {QStringLiteral("claims"), claimResults},
                       {QStringLiteral("coverage"), coverageJson},
                       {QStringLiteral("needs_verifier"), needsVerifier},
                       {QStringLiteral("semantic_status"), semanticStatus}};
    return result;
}

// R12-2c-1：verifier 响应的严格白名单解析。
//
// 这一步只回答"模型返回的协议是否合法"，不回答"提案是否可通过"：
//   * request_id 缺失/非字符串 → schema_invalid（与请求不匹配由调用方比对后报
//     request_mismatch，两者诊断码分开）；
//   * claim_coverage/result 必须是封闭枚举字符串，未知值 → unknown_enum；
//   * supporting_refs 必须是字符串数组（可选）；missing_claims 必须是 {type,text}
//     字符串对象数组（可选），元素类型不符 → schema_invalid；
//   * 元素数量有界，避免模型输出放大内存/日志。
VerifierVerdict ActionPolicy::parseVerifierVerdict(const QJsonObject &response)
{
    VerifierVerdict verdict;
    const auto fail = [&verdict](const QString &error) {
        verdict.valid = false;
        verdict.error = error;
        return verdict;
    };
    if (response.isEmpty()) return fail(QStringLiteral("schema_invalid"));
    const QJsonValue requestIdValue = response.value(QStringLiteral("request_id"));
    if (!requestIdValue.isString() || requestIdValue.toString().trimmed().isEmpty())
        return fail(QStringLiteral("schema_invalid"));
    verdict.requestId = requestIdValue.toString().trimmed();

    const QJsonValue coverageValue = response.value(QStringLiteral("claim_coverage"));
    if (!coverageValue.isString()) return fail(QStringLiteral("schema_invalid"));
    const QString coverage = coverageValue.toString().trimmed();
    if (coverage != QStringLiteral("complete") && coverage != QStringLiteral("incomplete"))
        return fail(QStringLiteral("unknown_enum"));
    verdict.coverageComplete = coverage == QStringLiteral("complete");

    const QJsonValue resultValue = response.value(QStringLiteral("result"));
    if (!resultValue.isString()) return fail(QStringLiteral("schema_invalid"));
    const QString result = resultValue.toString().trimmed();
    if (result == QStringLiteral("entailed")) {
        verdict.result = VerifierVerdict::Entailed;
    } else if (result == QStringLiteral("contradicted")) {
        verdict.result = VerifierVerdict::Contradicted;
    } else if (result == QStringLiteral("insufficient")) {
        verdict.result = VerifierVerdict::Insufficient;
    } else {
        return fail(QStringLiteral("unknown_enum"));
    }

    // supporting_refs：可选；存在时必须是字符串数组且有界（只做审计，不参与状态）。
    const QJsonValue refsValue = response.value(QStringLiteral("supporting_refs"));
    if (!refsValue.isUndefined() && !refsValue.isNull()) {
        if (!refsValue.isArray()) return fail(QStringLiteral("schema_invalid"));
        for (const auto &ref : refsValue.toArray()) {
            if (!ref.isString()) return fail(QStringLiteral("schema_invalid"));
            verdict.supportingRefs.append(ref.toString());
            if (verdict.supportingRefs.size() > 8)
                return fail(QStringLiteral("schema_invalid"));
        }
    }

    // missing_claims：可选；存在时必须是 {type?,text?} 的字符串对象数组（只做诊断）。
    const QJsonValue missingValue = response.value(QStringLiteral("missing_claims"));
    if (!missingValue.isUndefined() && !missingValue.isNull()) {
        if (!missingValue.isArray()) return fail(QStringLiteral("schema_invalid"));
        QJsonArray normalized;
        for (const auto &value : missingValue.toArray()) {
            if (!value.isObject()) return fail(QStringLiteral("schema_invalid"));
            const auto claim = value.toObject();
            const QJsonValue typeValue = claim.value(QStringLiteral("type"));
            const QJsonValue textValue = claim.value(QStringLiteral("text"));
            if ((!typeValue.isUndefined() && !typeValue.isNull() && !typeValue.isString())
                || (!textValue.isUndefined() && !textValue.isNull()
                    && !textValue.isString())) {
                return fail(QStringLiteral("schema_invalid"));
            }
            normalized.append(QJsonObject{
                {QStringLiteral("type"), typeValue.toString()},
                {QStringLiteral("text"), textValue.toString()}});
            if (normalized.size() > 16) return fail(QStringLiteral("schema_invalid"));
        }
        verdict.missingClaims = normalized;
    }

    const QJsonValue reasonValue = response.value(QStringLiteral("reason_code"));
    if (!reasonValue.isUndefined() && !reasonValue.isNull()) {
        if (!reasonValue.isString()) return fail(QStringLiteral("schema_invalid"));
        verdict.reasonCode = reasonValue.toString().left(120);
    }

    verdict.valid = true;
    verdict.error.clear();
    return verdict;
}

bool ActionPolicy::isValidSemanticStatus(const QString &status)
{
    // 与记忆层 action_proposal 持久化约束保持一致，并保留 legacy contact 的
    // retained/repaired；空串/null 表示"尚无结论"，不允许作为终态写回。
    static const QSet<QString> allowed{
        QStringLiteral("unverified"), QStringLiteral("entailed"),
        QStringLiteral("entailed_coverage"), QStringLiteral("repaired"),
        QStringLiteral("repair_needed"), QStringLiteral("contradicted"),
        QStringLiteral("insufficient"), QStringLiteral("withheld"),
        QStringLiteral("not_required"), QStringLiteral("unknown"),
        QStringLiteral("retained")};
    return !status.trimmed().isEmpty() && allowed.contains(status.trimmed());
}

QString ActionPolicy::verifiedSemanticStatus(bool hasFactualClaims)
{
    return hasFactualClaims ? QStringLiteral("entailed")
                            : QStringLiteral("entailed_coverage");
}

// R12-2c-2：verified 内容绑定摘要。算法必须与 LLMService 的 verifierPayloadDigest
// 逐字一致（同样的字段集合、同样的 Qt JSON 紧凑序列化、同样取 SHA-256 前 16 位 hex），
// 否则持久化的"核验内容指纹"无法和 verifier 输入对齐。这里只处理规范化后的内容，
// 不记录/输出 message/reason/draft 原文。
QString ActionPolicy::canonicalProposalPayloadDigest(const QJsonObject &payload,
                                                     const QJsonArray &claims)
{
    QJsonObject normalized{
        {QStringLiteral("message"), payload.value(QStringLiteral("message"))},
        {QStringLiteral("reason"), payload.value(QStringLiteral("reason"))}};
    if (payload.contains(QStringLiteral("draft")))
        normalized.insert(QStringLiteral("draft"), payload.value(QStringLiteral("draft")));
    normalized.insert(QStringLiteral("claims"), claims);
    const QByteArray canonical = QJsonDocument(normalized).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().left(16));
}

QString ActionPolicy::canonicalProposalClaimsDigest(const QJsonArray &claims)
{
    const QByteArray canonical = QJsonDocument(claims).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().left(16));
}

// R12-2c-final-1：需要语义 verifier 判定的 claim 选择。服务端唯一实现：LLMService 构建
// verifier 请求、MemoryManager 复核 VerificationBinding 的 evidence digest 都必须用它，
// 否则两边选择集合不一致会让合法提案失败关闭（宁可 fail closed，也不接受不一致证据）。
QJsonArray ActionPolicy::factualClaimsForVerification(const QJsonArray &claims)
{
    QJsonArray factual;
    for (const auto &value : claims) {
        const QString type = value.toObject().value(QStringLiteral("type")).toString().trimmed();
        // historical_fact/current_state 同样需要 entailment（只是不需要"新证据"时序）；
        // reported_fact 是 NewEvidence 旧类型，保留在核验集合里。
        if (type == kReportedFact || type == QStringLiteral("historical_fact")
            || type == QStringLiteral("current_state") || type == kUserCommitment
            || type == kExecutionResult)
            factual.append(value);
    }
    return factual;
}

QString ActionPolicy::canonicalEvidenceDigest(const QJsonArray &claims, const QJsonArray &sources)
{
    const QJsonObject normalized{
        {QStringLiteral("evidence_identity"), QStringLiteral("claims+sources")},
        {QStringLiteral("evidence_version"), 1},
        {QStringLiteral("claims"), claims},
        {QStringLiteral("sources"), sources}};
    const QByteArray canonical = QJsonDocument(normalized).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().left(16));
}

QString ActionPolicy::canonicalJsonDigest(const QJsonValue &value)
{
    const QByteArray canonical =
        QJsonDocument(QJsonObject{{QStringLiteral("v"), value}}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().left(16));
}

