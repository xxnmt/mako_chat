#ifndef ACTIONORIGIN_H
#define ACTIONORIGIN_H

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QtGlobal>

// R12-2a：通用动作来源身份（header-only）。
//
// 主动外发动作（contact / activity_share）以前各自用裸 turn/id/version 拼 action_key，
// 通用提案层需要一份可序列化的来源身份，把"这个动作是从哪次唤醒/回顾/关注/活动来的"
// 说清楚，并由服务端校验身份字段，避免调用方伪造。
//
// 字段语义（服务端生成，模型只能原样回显）：
//   * kind          来源类型：wake / reflection / concern / thought / activity；
//   * ref           规范引用：wake/<id>、reflection/<turn>、concern/<turn>、
//                   thought/<id>（legacy 存量活动）、activity/<id>（internal 活动）；
//   * id            来源领域行 id（必须是正整数，禁止负数/0）；
//   * version       **来源/成果版本，不是随机数**：
//                     - wake：open_wake.version（领取时 CAS 版本）；
//                     - reflection/concern：private_reflection.version（本次回顾版本）；
//                     - thought/activity：成果 artifact_version（完成时持久化的成果版本）；
//   * runId         OpenWake 的 open_wake.run_id（其它来源可空；wake 必须非空）；
//   * scope         OpenWake 的 scope_key（其它来源默认 owner；wake 必须非空）；
//   * contextEpoch  OpenWake 的 current_context_epoch（其它来源为 0）；
//   * targetChannel/targetSession  本次动作的目标渠道/会话（服务端生成，不允许调用方替换）。
//
// R12-2c-2：Action Context Fence 三维语义（本片只补充说明，字段沿用既有约定）：
//   * origin_version    来源/成果版本（上面的 version；文档、成果版本是"证据修订"）；
//   * evidence_revision 真实证据修订（artifact_version/hash 或来源修订，在上面的 version 中体现）；
//   * action_context_epoch 当前行动相关语境版本：**不是** origin 里的 contextEpoch。
//     它由服务端在 attention_state 中单独持久化（MemoryManager::actionContextEpoch /
//     bumpActionContextEpoch），提案落库时快照到 action_proposal.action_context_epoch；
//    主人私聊输入/成果撤回等使旧提案失效，提交与 Outbox 发送前都要逐字比较。
//    这里的 contextEpoch 只是 OpenWake 的来源 epoch（open_wake.current_context_epoch），
//     不能替代行动语境 fence。
//
// action_key 约定（与既有 ActionCommitter 完全一致）：
//   contact：       wake:<id>:<version>:contact
//                   reflection:<turn>:<version>:contact
//                   concern:<turn>:<version>:contact
//   activity_share：thought:<id>:<artifact_version>:share
//                   activity:<id>:<artifact_version>:share
//
// 本文件不依赖记忆层/配置层，只做纯数据结构与 JSON 编解码，运行时与测试共用。
struct ActionOrigin
{
    enum Kind { Wake, Reflection, Concern, Thought, Activity };

    Kind kind = Reflection;
    QString ref;
    qlonglong id = 0;
    int version = 0;
    QString runId;
    QString scope = QStringLiteral("owner");
    qint64 contextEpoch = 0;
    QString targetChannel;
    QString targetSession;

    static QString kindNameOf(Kind kind)
    {
        switch (kind) {
        case Wake: return QStringLiteral("wake");
        case Reflection: return QStringLiteral("reflection");
        case Concern: return QStringLiteral("concern");
        case Thought: return QStringLiteral("thought");
        case Activity: return QStringLiteral("activity");
        }
        return QStringLiteral("reflection");
    }

    QString kindName() const { return kindNameOf(kind); }

    static bool kindFromName(const QString &name, Kind *kind)
    {
        if (!kind) return false;
        const QString trimmed = name.trimmed();
        if (trimmed == QStringLiteral("wake")) { *kind = Wake; return true; }
        if (trimmed == QStringLiteral("reflection")) { *kind = Reflection; return true; }
        if (trimmed == QStringLiteral("concern")) { *kind = Concern; return true; }
        if (trimmed == QStringLiteral("thought")) { *kind = Thought; return true; }
        if (trimmed == QStringLiteral("activity")) { *kind = Activity; return true; }
        return false;
    }

    static QString refFor(Kind kind, qlonglong id)
    {
        if (id <= 0) return {};
        switch (kind) {
        case Wake: return QStringLiteral("wake/%1").arg(id);
        case Reflection: return QStringLiteral("reflection/%1").arg(id);
        case Concern: return QStringLiteral("concern/%1").arg(id);
        case Thought: return QStringLiteral("thought/%1").arg(id);
        case Activity: return QStringLiteral("activity/%1").arg(id);
        }
        return {};
    }

    // 严格解析规范引用 "<kind>/<正整数>"。裸 id、其它前缀、带附加段（:version 等）都拒绝。
    static bool parseRef(Kind kind, const QString &refText, qlonglong *id)
    {
        const QString prefix = kindNameOf(kind) + QLatin1Char('/');
        if (!refText.startsWith(prefix)) return false;
        bool ok = false;
        const qlonglong parsedId = refText.mid(prefix.size()).toLongLong(&ok);
        if (!ok || parsedId <= 0) return false;
        if (id) *id = parsedId;
        return true;
    }

    // 动作目标是否已由服务端填好（提案层要求 target 必须存在）。
    bool hasTarget() const
    {
        return !targetChannel.trimmed().isEmpty() && !targetSession.trimmed().isEmpty();
    }

    // 身份合法性（不含 target）：
    //   id>0、version>0、ref 与 kind+id 严格自洽；
    //   wake 额外要求 runId/scope 非空（禁止用空身份绕过来源校验）；
    //   其它来源 scope 非空时只允许 owner（当前只有主人私聊域）。
    bool isValid() const
    {
        if (id <= 0 || version <= 0 || ref.isEmpty()) return false;
        if (ref != refFor(kind, id)) return false;
        if (kind == Wake) {
            if (runId.isEmpty() || scope.isEmpty()) return false;
        } else if (scope.isEmpty()) {
            return false;
        }
        // 运行时不支持跨 scope：新提案只属于 owner（多 scope 留到后续切片）。
        if (scope != QStringLiteral("owner")) return false;
        return true;
    }

    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("kind"), kindName()},
                           {QStringLiteral("ref"), ref},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("version"), version},
                           {QStringLiteral("run_id"), runId},
                           {QStringLiteral("scope"), scope},
                           {QStringLiteral("context_epoch"), contextEpoch},
                           {QStringLiteral("target_channel"), targetChannel},
                           {QStringLiteral("target_session"), targetSession}};
    }

    // 严格解析：未知 kind、负数/0 id、ref 与 kind+id 不一致、wake 缺 run_id/scope、
    // 非 owner scope、target 冲突都返回 false；不回退成默认 kind，避免未知来源被误认。
    // error 输出：invalid_origin_json / unknown_origin_kind / invalid_origin_ref /
    //             invalid_origin_id / missing_run_id / missing_scope / invalid_scope /
    //             invalid_target / invalid_origin_version / invalid_origin。
    static bool fromJson(const QJsonObject &json, ActionOrigin *origin, QString *error = nullptr)
    {
        if (error) error->clear();
        const auto fail = [error](const QString &code) {
            if (error) *error = code;
            return false;
        };
        if (!origin || json.isEmpty()) return fail(QStringLiteral("invalid_origin_json"));
        Kind kind = Reflection;
        if (!kindFromName(json.value(QStringLiteral("kind")).toString(), &kind))
            return fail(QStringLiteral("unknown_origin_kind"));
        ActionOrigin parsed;
        parsed.kind = kind;
        const QString ref = json.value(QStringLiteral("ref")).toString().trimmed();
        qlonglong refId = 0;
        const bool hasRefId = !ref.isEmpty() && parseRef(kind, ref, &refId);
        if (!ref.isEmpty() && !hasRefId) return fail(QStringLiteral("invalid_origin_ref"));
        qlonglong id = 0;
        const QJsonValue idValue = json.value(QStringLiteral("id"));
        if (idValue.isDouble()) id = idValue.toInteger();
        else if (idValue.isString()) id = idValue.toString().toLongLong();
        // 显式负数/0 ID 先报 invalid_origin_id，避免与 ref 不一致时误报 ref 错误。
        if (!idValue.isUndefined() && !idValue.isNull() && id <= 0)
            return fail(QStringLiteral("invalid_origin_id"));
        if (hasRefId) {
            if (id != 0 && id != refId) return fail(QStringLiteral("invalid_origin_ref"));
            id = refId;
        }
        if (id <= 0) return fail(QStringLiteral("invalid_origin_id"));
        parsed.id = id;
        parsed.ref = ref.isEmpty() ? refFor(kind, id) : ref;
        if (parsed.ref != refFor(kind, id)) return fail(QStringLiteral("invalid_origin_ref"));
        parsed.version = json.value(QStringLiteral("version")).toInt();
        if (parsed.version <= 0) return fail(QStringLiteral("invalid_origin_version"));
        parsed.runId = json.value(QStringLiteral("run_id")).toString();
        parsed.scope = json.value(QStringLiteral("scope")).toString().trimmed();
        if (parsed.scope.isEmpty() && kind != Wake) parsed.scope = QStringLiteral("owner");
        parsed.contextEpoch = json.value(QStringLiteral("context_epoch")).toInteger();
        parsed.targetChannel = json.value(QStringLiteral("target_channel")).toString().trimmed();
        parsed.targetSession = json.value(QStringLiteral("target_session")).toString().trimmed();
        // 兼容嵌套 target{channel,session_id|session}；与扁平字段冲突时拒绝。
        const QJsonObject target = json.value(QStringLiteral("target")).toObject();
        if (!target.isEmpty()) {
            const QString targetChannel = target.value(QStringLiteral("channel")).toString().trimmed();
            const QString sessionId = target.value(QStringLiteral("session_id")).toString().trimmed();
            const QString targetSession = sessionId.isEmpty()
                ? target.value(QStringLiteral("session")).toString().trimmed() : sessionId;
            if (!targetChannel.isEmpty()) {
                if (!parsed.targetChannel.isEmpty() && parsed.targetChannel != targetChannel)
                    return fail(QStringLiteral("invalid_target"));
                parsed.targetChannel = targetChannel;
            }
            if (!targetSession.isEmpty()) {
                if (!parsed.targetSession.isEmpty() && parsed.targetSession != targetSession)
                    return fail(QStringLiteral("invalid_target"));
                parsed.targetSession = targetSession;
            }
        }
        if (kind == Wake && parsed.runId.isEmpty()) return fail(QStringLiteral("missing_run_id"));
        if (kind == Wake && parsed.scope.isEmpty()) return fail(QStringLiteral("missing_scope"));
        if (!parsed.isValid()) {
            if (parsed.scope != QStringLiteral("owner")) return fail(QStringLiteral("invalid_scope"));
            return fail(QStringLiteral("invalid_origin"));
        }
        *origin = parsed;
        return true;
    }

    // action_key 由来源身份与动作类型共同决定，调用方不能自造。
    // 不支持的 (kind, origin.kind) 组合返回空串（例如 thought 不能直接发 contact）。
    QString actionKey(const QString &actionKind) const
    {
        const QString trimmed = actionKind.trimmed();
        if (trimmed == QStringLiteral("contact")) {
            if (kind != Wake && kind != Reflection && kind != Concern) return {};
            return QStringLiteral("%1:%2:%3:contact").arg(kindName()).arg(id).arg(version);
        }
        if (trimmed == QStringLiteral("activity_share")) {
            if (kind != Thought && kind != Activity) return {};
            return QStringLiteral("%1:%2:%3:share").arg(kindName()).arg(id).arg(version);
        }
        return {};
    }

    // 仅校验格式（服务端生成后落库/回读时做防御性检查；语义由 actionKey 决定）。
    static bool isValidActionKeyFormat(const QString &key)
    {
        static const QRegularExpression pattern(QStringLiteral(
            "^((wake|reflection|concern):[1-9][0-9]*:[1-9][0-9]*:contact|"
            "(thought|activity):[1-9][0-9]*:[1-9][0-9]*:share)$"));
        return pattern.match(key).hasMatch();
    }

    // 按规范引用反推 id（用于从库里的 origin_ref 还原身份）；不一致返回 false。
    static bool idFromRef(Kind kind, const QString &refText, qlonglong *id)
    {
        if (!id || !parseRef(kind, refText, id)) return false;
        return refText == refFor(kind, *id);
    }

    static ActionOrigin forWake(qlonglong wakeId, int version, const QString &wakeRunId,
                                const QString &scopeKey, qint64 contextEpoch,
                                const QString &targetChannel, const QString &targetSession)
    {
        ActionOrigin origin;
        origin.kind = Wake;
        origin.id = wakeId;
        origin.version = version;
        origin.ref = refFor(Wake, wakeId);
        origin.runId = wakeRunId;
        origin.scope = scopeKey.trimmed().isEmpty() ? QString() : scopeKey.trimmed();
        origin.contextEpoch = contextEpoch;
        origin.targetChannel = targetChannel;
        origin.targetSession = targetSession;
        return origin;
    }

    static ActionOrigin forReflection(qlonglong turn, int version,
                                      const QString &scopeKey = QStringLiteral("owner"),
                                      const QString &targetChannel = QString(),
                                      const QString &targetSession = QString())
    {
        ActionOrigin origin;
        origin.kind = Reflection;
        origin.id = turn;
        origin.version = version;
        origin.ref = refFor(Reflection, turn);
        origin.scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner") : scopeKey.trimmed();
        origin.targetChannel = targetChannel;
        origin.targetSession = targetSession;
        return origin;
    }

    static ActionOrigin forConcern(qlonglong turn, int version,
                                   const QString &scopeKey = QStringLiteral("owner"),
                                   const QString &targetChannel = QString(),
                                   const QString &targetSession = QString())
    {
        ActionOrigin origin;
        origin.kind = Concern;
        origin.id = turn;
        origin.version = version;
        origin.ref = refFor(Concern, turn);
        origin.scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner") : scopeKey.trimmed();
        origin.targetChannel = targetChannel;
        origin.targetSession = targetSession;
        return origin;
    }

    static ActionOrigin forThought(qlonglong thoughtId, int artifactVersion,
                                   const QString &scopeKey = QStringLiteral("owner"),
                                   const QString &targetChannel = QString(),
                                   const QString &targetSession = QString())
    {
        ActionOrigin origin;
        origin.kind = Thought;
        origin.id = thoughtId;
        origin.version = artifactVersion;
        origin.ref = refFor(Thought, thoughtId);
        origin.scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner") : scopeKey.trimmed();
        origin.targetChannel = targetChannel;
        origin.targetSession = targetSession;
        return origin;
    }

    static ActionOrigin forActivity(qlonglong activityId, int artifactVersion,
                                    const QString &scopeKey = QStringLiteral("owner"),
                                    const QString &targetChannel = QString(),
                                    const QString &targetSession = QString())
    {
        ActionOrigin origin;
        origin.kind = Activity;
        origin.id = activityId;
        origin.version = artifactVersion;
        origin.ref = refFor(Activity, activityId);
        origin.scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner") : scopeKey.trimmed();
        origin.targetChannel = targetChannel;
        origin.targetSession = targetSession;
        return origin;
    }

    // 按统一活动引用（thought/<id> 或 activity/<id>）创建来源身份；
    // 未知格式返回 !isValid() 的 origin，由调用方拒绝。
    static ActionOrigin forActivityRef(const QString &activityRef, int artifactVersion,
                                       const QString &scopeKey = QStringLiteral("owner"),
                                       const QString &targetChannel = QString(),
                                       const QString &targetSession = QString())
    {
        ActionOrigin origin;
        origin.version = artifactVersion;
        origin.scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner") : scopeKey.trimmed();
        origin.targetChannel = targetChannel;
        origin.targetSession = targetSession;
        const QString trimmed = activityRef.trimmed();
        if (trimmed.startsWith(QStringLiteral("activity/"))) {
            origin.kind = Activity;
            if (!parseRef(Activity, trimmed, &origin.id)) return ActionOrigin{};
        } else if (trimmed.startsWith(QStringLiteral("thought/"))) {
            origin.kind = Thought;
            if (!parseRef(Thought, trimmed, &origin.id)) return ActionOrigin{};
        } else {
            return ActionOrigin{};
        }
        origin.ref = refFor(origin.kind, origin.id);
        return origin;
    }
};

// R12-2c-final-1：服务端 VerificationBinding（核验内容绑定，header-only）。
//
// 背景：R12-2c-2 只在 proposed→verified 的事务内重新读取 DB 内容计算 verified_payload_hash，
// 这只能证明"库里这份内容有指纹"，不能证明"verifier 实际核验的就是这份内容"：
// 若持久化时同 key 已存在另一份内容（persist 冲突）、或库内容在核验返回后被改写，
// 旧实现仍会把库里的内容标成 verified。本结构把"这次核验究竟核验了哪份内容"固化成
// 随异步回调传递的服务端对象，模型输出只回显 request_id，不能签发/改写 binding。
//
// 生命周期：
//   * LLMService 在**构建 verifier 请求时**生成 binding（request_id/action_key/来源身份/
//     target/两级 epoch/payload/claims/evidence 集合摘要/verifier 输入摘要）；
//   * verifier 通过后由服务端映射终态（state=verified + semantic_status），随
//     actionProposalVerificationFinished 回到 PrivateReview；
//   * PrivateReview 把它交给 MemoryManager::markActionProposalVerified：事务内读权威提案行，
//     逐项比较 binding，再 CAS proposed→verified；任何不一致（同 key 被替换、epoch 前进、
//     evidence 集合变化）都 fail closed。
//
// 字段语义（全部服务端生成）：
//   * payloadDigest  = {message,reason,[draft],claims} 规范化 SHA-256 前 16 位
//                      （与 ActionPolicy::canonicalProposalPayloadDigest 同一算法）；
//   * claimsDigest   = claims 数组规范化摘要（ActionPolicy::canonicalProposalClaimsDigest）；
//   * evidenceDigest = verifier 输入里实际使用的 declared_claims + sources 选择集合的规范化
//                      摘要（ActionPolicy::canonicalEvidenceDigest）。持久化层会用权威提案
//                      与当前库内证据重算，变化即拒绝（fail closed）；
//   * verifierInputDigest = 实际发送给 verifier 的 user JSON 文本摘要（只做审计，不含正文）；
//   * originContextEpoch  = ActionOrigin.contextEpoch（open_wake.current_context_epoch）；
//   * actionContextEpoch  = 构建核验请求时的 Action Context Fence epoch
//                           （MemoryManager::actionContextEpoch），与提案快照比较。
struct VerificationBinding
{
    static constexpr int kSchemaVersion = 1;

    int schemaVersion = kSchemaVersion;
    QString requestId;          // verifier 请求的服务端唯一关联 id
    QString actionKey;          // origin+kind 派生（服务端权威）
    QString kind;               // contact | activity_share
    QString originKind;         // wake | reflection | concern | thought | activity
    QString originRef;          // wake/<id> 等规范引用
    qlonglong originId = 0;
    int originVersion = 0;      // 来源/成果版本
    QString runId;
    QString scope;
    qint64 originContextEpoch = 0;
    qint64 actionContextEpoch = 0;
    QString targetChannel;
    QString targetSession;
    QString payloadDigest;
    QString claimsDigest;
    QString evidenceIdentity = QStringLiteral("claims+sources");
    int evidenceVersion = 1;
    QString evidenceDigest;
    QString verifierInputDigest;
    QString state;              // verified | withheld（服务端映射的终态）
    QString semanticStatus;     // 服务端封闭枚举；verified 时不得为 withheld
    QString withholdReason;     // 仅 withheld 审计

    // 规范化摘要统一为 SHA-256 前 16 位小写 hex。
    static bool isDigestText(const QString &text)
    {
        if (text.size() != 16) return false;
        for (const QChar ch : text) {
            if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                  || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))))
                return false;
        }
        return true;
    }

    // 结构合法性（不含"与某份提案是否一致"——那必须由持久化层在同一事务内逐项比较）。
    // error 输出：missing_verification_request_id / invalid_action_key /
    //             invalid_binding_kind / invalid_binding_origin / invalid_binding_target /
    //             invalid_binding_digest / invalid_binding_state / invalid_binding_semantic。
    bool isValid(QString *error = nullptr) const
    {
        const auto fail = [error](const QString &code) {
            if (error) *error = code;
            return false;
        };
        if (error) error->clear();
        if (schemaVersion != kSchemaVersion) return fail(QStringLiteral("invalid_binding_schema"));
        if (requestId.trimmed().isEmpty() || requestId.size() > 120)
            return fail(QStringLiteral("missing_verification_request_id"));
        if (!ActionOrigin::isValidActionKeyFormat(actionKey))
            return fail(QStringLiteral("invalid_action_key"));
        if (kind != QStringLiteral("contact") && kind != QStringLiteral("activity_share"))
            return fail(QStringLiteral("invalid_binding_kind"));
        if (originId <= 0 || originVersion <= 0 || originRef.isEmpty())
            return fail(QStringLiteral("invalid_binding_origin"));
        ActionOrigin::Kind parsedOrigin = ActionOrigin::Reflection;
        if (!ActionOrigin::kindFromName(originKind, &parsedOrigin))
            return fail(QStringLiteral("invalid_binding_origin"));
        if (originRef != ActionOrigin::refFor(parsedOrigin, originId))
            return fail(QStringLiteral("invalid_binding_origin"));
        // 动作类型与来源类型必须自洽（contact 只来自 wake/reflection/concern；share 只来自 thought/activity）。
        if (kind == QStringLiteral("contact")) {
            if (parsedOrigin == ActionOrigin::Thought || parsedOrigin == ActionOrigin::Activity)
                return fail(QStringLiteral("invalid_binding_origin"));
            if (!actionKey.endsWith(QStringLiteral(":contact")))
                return fail(QStringLiteral("invalid_binding_origin"));
        } else {
            if (parsedOrigin != ActionOrigin::Thought && parsedOrigin != ActionOrigin::Activity)
                return fail(QStringLiteral("invalid_binding_origin"));
            if (!actionKey.endsWith(QStringLiteral(":share")))
                return fail(QStringLiteral("invalid_binding_origin"));
        }
        if (scope.trimmed().isEmpty()) return fail(QStringLiteral("invalid_binding_origin"));
        if (parsedOrigin == ActionOrigin::Wake && runId.trimmed().isEmpty())
            return fail(QStringLiteral("invalid_binding_origin"));
        if (targetChannel.trimmed().isEmpty() || targetSession.trimmed().isEmpty())
            return fail(QStringLiteral("invalid_binding_target"));
        if (originContextEpoch < 0 || actionContextEpoch < 0)
            return fail(QStringLiteral("invalid_binding_origin"));
        if (!isDigestText(payloadDigest) || !isDigestText(claimsDigest)
            || !isDigestText(evidenceDigest) || !isDigestText(verifierInputDigest))
            return fail(QStringLiteral("invalid_binding_digest"));
        if (evidenceIdentity != QStringLiteral("claims+sources") || evidenceVersion != 1)
            return fail(QStringLiteral("invalid_binding_digest"));
        if (state != QStringLiteral("verified") && state != QStringLiteral("withheld"))
            return fail(QStringLiteral("invalid_binding_state"));
        if (semanticStatus.trimmed().isEmpty()
            || (state == QStringLiteral("verified")
                && semanticStatus == QStringLiteral("withheld")))
            return fail(QStringLiteral("invalid_binding_semantic"));
        return true;
    }

    QJsonObject toJson() const
    {
        QJsonObject json{
            {QStringLiteral("schema_version"), schemaVersion},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("action_key"), actionKey},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("origin_kind"), originKind},
            {QStringLiteral("origin_ref"), originRef},
            {QStringLiteral("origin_id"), originId},
            {QStringLiteral("origin_version"), originVersion},
            {QStringLiteral("run_id"), runId},
            {QStringLiteral("scope"), scope},
            {QStringLiteral("origin_context_epoch"), originContextEpoch},
            {QStringLiteral("action_context_epoch"), actionContextEpoch},
            {QStringLiteral("target_channel"), targetChannel},
            {QStringLiteral("target_session"), targetSession},
            {QStringLiteral("payload_digest"), payloadDigest},
            {QStringLiteral("claims_digest"), claimsDigest},
            {QStringLiteral("evidence_identity"), evidenceIdentity},
            {QStringLiteral("evidence_version"), evidenceVersion},
            {QStringLiteral("evidence_digest"), evidenceDigest},
            {QStringLiteral("verifier_input_digest"), verifierInputDigest},
            {QStringLiteral("state"), state},
            {QStringLiteral("semantic_status"), semanticStatus}};
        if (!withholdReason.isEmpty())
            json.insert(QStringLiteral("withhold_reason"), withholdReason);
        return json;
    }

    // 严格解析（字段类型/缺失都失败）；只解析结构，不判断提案一致性。
    static bool fromJson(const QJsonObject &json, VerificationBinding *binding,
                         QString *error = nullptr)
    {
        if (error) error->clear();
        const auto fail = [error](const QString &code) {
            if (error) *error = code;
            return false;
        };
        if (!binding) return fail(QStringLiteral("invalid_binding_json"));
        if (json.isEmpty()) return fail(QStringLiteral("invalid_binding_json"));
        VerificationBinding parsed;
        parsed.schemaVersion = json.value(QStringLiteral("schema_version")).toInt(-1);
        const auto stringField = [&json](const char *name) {
            return json.value(QString::fromLatin1(name)).toString();
        };
        parsed.requestId = stringField("request_id");
        parsed.actionKey = stringField("action_key");
        parsed.kind = stringField("kind");
        parsed.originKind = stringField("origin_kind");
        parsed.originRef = stringField("origin_ref");
        const QJsonValue originIdValue = json.value(QStringLiteral("origin_id"));
        if (!originIdValue.isDouble()) return fail(QStringLiteral("invalid_binding_origin"));
        parsed.originId = originIdValue.toInteger();
        parsed.originVersion = json.value(QStringLiteral("origin_version")).toInt(0);
        parsed.runId = stringField("run_id");
        parsed.scope = stringField("scope");
        parsed.originContextEpoch =
            json.value(QStringLiteral("origin_context_epoch")).toInteger(-1);
        parsed.actionContextEpoch =
            json.value(QStringLiteral("action_context_epoch")).toInteger(-1);
        parsed.targetChannel = stringField("target_channel");
        parsed.targetSession = stringField("target_session");
        parsed.payloadDigest = stringField("payload_digest");
        parsed.claimsDigest = stringField("claims_digest");
        parsed.evidenceIdentity = stringField("evidence_identity");
        parsed.evidenceVersion =
            json.value(QStringLiteral("evidence_version")).toInt(0);
        parsed.evidenceDigest = stringField("evidence_digest");
        parsed.verifierInputDigest = stringField("verifier_input_digest");
        parsed.state = stringField("state");
        parsed.semanticStatus = stringField("semantic_status");
        parsed.withholdReason = stringField("withhold_reason");
        QString validationError;
        if (!parsed.isValid(&validationError))
            return fail(validationError.isEmpty() ? QStringLiteral("invalid_binding")
                                                  : validationError);
        *binding = parsed;
        return true;
    }
};

#endif // ACTIONORIGIN_H
