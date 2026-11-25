#include "actionpolicy.h"

#include "configmanager.h"
#include "memorymanager.h"
#include "memoryreadpolicy.h"
#include "utterance.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
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

const QString kSelfDesire = QStringLiteral("self_desire");
const QString kConcernFollowup = QStringLiteral("concern_followup");
const QString kActivityShare = QStringLiteral("activity_share");

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

bool isFactClaimType(const QString &type)
{
    return type == kReportedFact || type == kUserCommitment || type == kExecutionResult;
}

// 动作核心：用户承诺/执行结果本身就是对外动作的事实核心；
// reported_fact/inference 只有被标成 core（reason/message 依赖它）时才算核心。
bool claimIsCoreByType(const QString &type)
{
    return type == kUserCommitment || type == kExecutionResult;
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
    if (channel == viewerChannel && session == viewerSession) return true;
    auto &cfg = ConfigManager::instance();
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

ActionPolicy::ActionPolicy(MemoryManager *memory)
    : m_memory(memory)
{
}

void ActionPolicy::setSemanticVerifier(SemanticVerifier verifier)
{
    m_verifier = std::move(verifier);
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
    const auto type = claim.value(QStringLiteral("type")).toString().trimmed();
    if (!isFactClaimType(type) && type != kInference)
        return insufficient(QStringLiteral("invalid_claim_type"));
    if (claim.value(QStringLiteral("text")).toString().trimmed().isEmpty())
        return insufficient(QStringLiteral("missing_claim_text"));
    const auto refsValue = claim.value(QStringLiteral("refs"));
    if (!refsValue.isArray()) return insufficient(QStringLiteral("missing_refs"));
    const auto refs = refsValue.toArray();
    if (refs.size() > 4) return insufficient(QStringLiteral("too_many_refs"));
    if (refs.isEmpty()) {
        // inference 不需要事实来源；事实/承诺/执行结果没有 refs 就是无法证明。
        if (type == kInference) {
            ClaimValidation validation;
            validation.result = ClaimValidation::Entailed;
            return validation;
        }
        return insufficient(QStringLiteral("missing_refs"));
    }
    EvidenceReader reader(m_memory->m_db, channel, session, concernTurn);
    for (const auto &value : refs) {
        if (!value.isString() || value.toString().trimmed().isEmpty())
            return insufficient(QStringLiteral("invalid_ref"));
    }
    if (!reader.originReason().isEmpty()) return insufficient(reader.originReason());

    ClaimValidation validation;
    validation.result = ClaimValidation::Entailed;
    for (const auto &value : refs) {
        const auto ref = value.toString().trimmed();
        if (type == kInference) {
            // 自己的理解：reflection/<turn> 或 matter/<名称>；只要求"她确实留下过这条理解"。
            bool ok = false;
            if (ref.startsWith(QStringLiteral("reflection/"))) {
                const auto turn = ref.mid(QStringLiteral("reflection/").size()).section(':', 0, 0).toLongLong();
                QSqlQuery row(m_memory->m_db);
                row.prepare("SELECT h.channel,h.session_id FROM private_reflection r "
                            "JOIN chat_history h ON h.id=r.turn_id WHERE r.turn_id=? AND h.subject='user' LIMIT 1");
                row.addBindValue(turn);
                ok = row.exec() && row.next()
                    && channelVisibleFromOwnerPrivate(row.value(0).toString(), row.value(1).toString());
            } else if (ref.startsWith(QStringLiteral("matter/"))
                       || ref.startsWith(QStringLiteral("revision/"))) {
                const auto matter = ref.startsWith(QStringLiteral("matter/"))
                    ? ref.mid(QStringLiteral("matter/").size()).section(':', 0, 0)
                    : ref.mid(QStringLiteral("revision/").size()).section('/', 0, 0);
                QSqlQuery row(m_memory->m_db);
                row.prepare("SELECT s.channel,s.session FROM matter_revision r "
                            "JOIN cognitive_source s ON s.id=r.source "
                            "WHERE r.matter=? AND s.subject='user' ORDER BY r.version DESC LIMIT 1");
                row.addBindValue(matter);
                ok = row.exec() && row.next()
                    && channelVisibleFromOwnerPrivate(row.value(0).toString(), row.value(1).toString());
            }
            if (!ok) return insufficient(QStringLiteral("inference_ref_not_found"));
            validation.supportingRefs.append(ref);
            continue;
        }
        if (type == kReportedFact) {
            QString reason;
            if (ref.startsWith(QStringLiteral("chat/"))) {
                const auto turn = ref.mid(5).section(':', 0, 0).toLongLong();
                const auto row = reader.chatRef(turn, 1);
                reason = row.reason;
                if (reason.isEmpty())
                    reason = reader.checkOwnerStatementTiming(row.reported);
            } else if (ref.startsWith(QStringLiteral("source/"))) {
                const auto sourceId = ref.mid(QStringLiteral("source/").size()).section(':', 0, 0).toLongLong();
                QSqlQuery row(m_memory->m_db);
                row.prepare("SELECT subject,channel,session,input,reported FROM cognitive_source WHERE id=?");
                row.addBindValue(sourceId);
                if (!row.exec() || !row.next()) reason = QStringLiteral("invalid_ref");
                else if (row.value(0).toString() != QStringLiteral("user")
                         || row.value(3).toString().trimmed().isEmpty())
                    reason = QStringLiteral("not_owner_statement");
                else if (!channelVisibleFromOwnerPrivate(row.value(1).toString(), row.value(2).toString()))
                    reason = QStringLiteral("invisible_ref");
                else
                    reason = reader.checkOwnerStatementTiming(row.value(4).toString());
            } else if (ref.startsWith(QStringLiteral("raw_event/"))) {
                const auto eventId = ref.mid(QStringLiteral("raw_event/").size()).section(':', 0, 0).toLongLong();
                QSqlQuery row(m_memory->m_db);
                row.prepare("SELECT channel,session_id,direction,sensitive,content,visibility,occurred_at "
                            "FROM raw_events WHERE id=?");
                row.addBindValue(eventId);
                if (!row.exec() || !row.next()) reason = QStringLiteral("invalid_ref");
                else if (row.value(3).toBool()) reason = QStringLiteral("sensitive_ref");
                else if (row.value(2).toString() != QStringLiteral("in"))
                    reason = QStringLiteral("not_owner_statement");
                else if (!channelVisibleFromOwnerPrivate(row.value(0).toString(), row.value(1).toString()))
                    reason = QStringLiteral("invisible_ref");
                else
                    reason = reader.checkOwnerStatementTiming(row.value(6).toString());
            } else {
                reason = QStringLiteral("invalid_ref");
            }
            if (!reason.isEmpty()) return insufficient(reason);
        } else if (type == kUserCommitment) {
            QString reason;
            if (ref.startsWith(QStringLiteral("chat/"))) {
                const auto turn = ref.mid(5).section(':', 0, 0).toLongLong();
                const auto row = reader.chatRef(turn, 1);
                reason = row.reason;
                if (reason.isEmpty())
                    reason = reader.checkOwnerStatementTiming(row.reported);
            } else if (ref.startsWith(QStringLiteral("source/"))) {
                const auto sourceId = ref.mid(QStringLiteral("source/").size()).section(':', 0, 0).toLongLong();
                QSqlQuery row(m_memory->m_db);
                row.prepare("SELECT subject,channel,session,input,reported FROM cognitive_source WHERE id=?");
                row.addBindValue(sourceId);
                if (!row.exec() || !row.next()) reason = QStringLiteral("invalid_ref");
                else if (row.value(0).toString() != QStringLiteral("user")
                         || row.value(3).toString().trimmed().isEmpty())
                    reason = QStringLiteral("not_owner_statement");
                else if (!channelVisibleFromOwnerPrivate(row.value(1).toString(), row.value(2).toString()))
                    reason = QStringLiteral("invisible_ref");
                else
                    reason = reader.checkOwnerStatementTiming(row.value(4).toString());
            } else if (ref.startsWith(QStringLiteral("reminder/"))) {
                // 提醒是"已安排的执行"，不是主人此刻的原话；提醒链自己执行，不能拿来支持承诺。
                reason = QStringLiteral("reminder_not_owner_statement");
            } else {
                reason = QStringLiteral("invalid_ref");
            }
            if (!reason.isEmpty()) return insufficient(reason);
        } else { // execution_result
            QString reason;
            if (ref.startsWith(QStringLiteral("delivery/"))) {
                const auto key = ref.mid(QStringLiteral("delivery/").size());
                QString actionState, deliveryChannel, deliverySession;
                bool found = false;
                QSqlQuery row(m_memory->m_db);
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
                    QSqlQuery byId(m_memory->m_db);
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
                if (!found) reason = QStringLiteral("execution_result_not_found");
                else if (actionState != QStringLiteral("handed_off"))
                    reason = QStringLiteral("execution_result_not_found");
                else if (!channelVisibleFromOwnerPrivate(deliveryChannel, deliverySession))
                    reason = QStringLiteral("invisible_ref");
            } else if (ref.startsWith(QStringLiteral("chat/"))) {
                const auto turn = ref.mid(5).section(':', 0, 0).toLongLong();
                reason = reader.chatRef(turn, 2).reason;
            } else {
                reason = QStringLiteral("invalid_ref");
            }
            if (!reason.isEmpty()) return insufficient(reason);
        }
        validation.supportingRefs.append(ref);
    }
    return validation;
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
    // activity_share：必须是 completed 且有产物的活动，分享去重由切片 B 的投递层负责。
    if (!ref.startsWith(QStringLiteral("thought/")))
        return QStringLiteral("invalid_motivation_ref");
    const auto activityId = ref.mid(QStringLiteral("thought/").size()).section(':', 0, 0).toLongLong();
    QSqlQuery row(m_memory->m_db);
    row.prepare("SELECT channel,session_id,sensitive,"
                "coalesce(json_extract(thought_result,'$.status'),''),"
                "coalesce(json_extract(thought_result,'$.artifact_ref'),'') "
                "FROM chat_history WHERE id=? AND thought IS NOT NULL AND thought<>''");
    row.addBindValue(activityId);
    if (!row.exec() || !row.next()) return QStringLiteral("motivation_artifact_not_found");
    if (row.value(2).toBool() || !visibleSession(row.value(0).toString(), row.value(1).toString(),
                                                 channel, session))
        return QStringLiteral("motivation_ref_invisible");
    if (row.value(3).toString() != QStringLiteral("completed")
        || row.value(4).toString().trimmed().isEmpty())
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
        row.prepare("SELECT payload_json FROM action_outbox WHERE action_key=? LIMIT 1");
        row.addBindValue(key);
        return row.exec() && row.next() ? truncate(row.value(0).toString()) : QString();
    }
    return {};
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
