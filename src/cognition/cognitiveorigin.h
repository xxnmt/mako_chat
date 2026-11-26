#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

// R12-1a 运行时：类型化认知来源身份（header-only）。
//
// 后台回顾有四类来源：新来源回顾（reflection/<turn>）、到期关注（concern/<turn>）、
// 自己的文字活动（thought/<id>）、内部唤醒（wake/<id>）。以前 PrivateReview 只保存
// id/version/kind 三个裸字段，异步回调无法证明"这次回调确实属于当时领取的那次回顾"，
// 任何同 id/version 的迟到 proposal 都可能被落库。
//
// 这里把来源身份收拢成一个可序列化的结构，随 requested context 一起交给模型层，
// 模型层原样回显到 proposal["_origin"]，PrivateReview::receive 逐字段比对：
//   * kind：来源类型（reflection/concern/activity/open_wake）；
//   * ref：规范引用（reflection/<turn>、concern/<turn>、thought/<id>、wake/<id>）；
//   * id/version：领域行标识与 CAS 版本；
//   * runId：OpenWake 的 open_wake.run_id（其它来源为空）；
//   * scope：OpenWake 的 scope_key（其它来源为 owner）；
//   * epoch：上下文 epoch（OpenWake 用 open_wake.current_context_epoch；其它为 0）。
//
// 本文件不依赖记忆层/配置层，只做纯数据结构与 JSON 编解码，方便运行时与测试共用。
struct CognitiveOrigin {
    enum Kind { Reflection, Concern, Activity, OpenWake };

    Kind kind = Reflection;
    QString ref;                 // reflection/<turn> | concern/<turn> | thought/<id> | wake/<id>
    qlonglong id = 0;            // 领域行 id（turn 或 thought/wake id）
    int version = 0;             // 本次领取的回顾版本
    QString runId;               // OpenWake: open_wake.run_id；其它可空
    QString scope = QStringLiteral("owner");  // OpenWake: scope_key；其它 owner
    qint64 epoch = 0;            // 上下文 epoch（OpenWake 用 current_context_epoch；其它 0）

    // 日志/协议里使用的稳定名称（与 memory 层 source_kind 一致）。
    QString kindName() const { return kindNameOf(kind); }

    static QString kindNameOf(Kind kind)
    {
        switch (kind) {
        case Reflection: return QStringLiteral("reflection");
        case Concern: return QStringLiteral("concern");
        case Activity: return QStringLiteral("activity");
        case OpenWake: return QStringLiteral("open_wake");
        }
        return QStringLiteral("reflection");
    }

    static QString refFor(Kind kind, qlonglong id)
    {
        switch (kind) {
        case Reflection: return QStringLiteral("reflection/%1").arg(id);
        case Concern: return QStringLiteral("concern/%1").arg(id);
        case Activity: return QStringLiteral("thought/%1").arg(id);
        case OpenWake: return QStringLiteral("wake/%1").arg(id);
        }
        return QString{};
    }

    static bool kindFromName(const QString &name, Kind *kind)
    {
        if (!kind) return false;
        if (name == QStringLiteral("reflection")) { *kind = Reflection; return true; }
        if (name == QStringLiteral("concern")) { *kind = Concern; return true; }
        if (name == QStringLiteral("activity")) { *kind = Activity; return true; }
        if (name == QStringLiteral("open_wake")) { *kind = OpenWake; return true; }
        return false;
    }

    static CognitiveOrigin forReflection(qlonglong turn, int version,
                                         const QString &scopeKey = QStringLiteral("owner"))
    {
        CognitiveOrigin origin;
        origin.kind = Reflection;
        origin.id = turn;
        origin.version = version;
        origin.ref = refFor(Reflection, turn);
        origin.scope = scopeKey;
        return origin;
    }

    static CognitiveOrigin forConcern(qlonglong turn, int version,
                                      const QString &scopeKey = QStringLiteral("owner"))
    {
        CognitiveOrigin origin;
        origin.kind = Concern;
        origin.id = turn;
        origin.version = version;
        origin.ref = refFor(Concern, turn);
        origin.scope = scopeKey;
        return origin;
    }

    static CognitiveOrigin forActivity(qlonglong thoughtId, int version,
                                       const QString &scopeKey = QStringLiteral("owner"))
    {
        CognitiveOrigin origin;
        origin.kind = Activity;
        origin.id = thoughtId;
        origin.version = version;
        origin.ref = refFor(Activity, thoughtId);
        origin.scope = scopeKey;
        return origin;
    }

    static CognitiveOrigin forOpenWake(qlonglong wakeId, int version, const QString &wakeRunId,
                                       const QString &scopeKey, qint64 contextEpoch = 0)
    {
        CognitiveOrigin origin;
        origin.kind = OpenWake;
        origin.id = wakeId;
        origin.version = version;
        origin.ref = refFor(OpenWake, wakeId);
        origin.runId = wakeRunId;
        origin.scope = scopeKey.trimmed().isEmpty() ? QStringLiteral("owner") : scopeKey.trimmed();
        origin.epoch = contextEpoch;
        return origin;
    }

    // 合法性：OpenWake 额外要求 run_id/scope 非空（禁止用空身份绕过来源校验）。
    bool isValid() const
    {
        if (id <= 0 || version <= 0 || ref.isEmpty()) return false;
        if (ref != refFor(kind, id)) return false;
        if (kind == OpenWake) return !runId.isEmpty() && !scope.isEmpty();
        return true;
    }

    // 来源身份比对：kind/ref/id/version/runId/epoch 必须一致；scope 对 OpenWake 强制，
    // 对其它来源允许回显方省略（旧路径兼容），但非空时必须一致。
    bool sameIdentity(const CognitiveOrigin &other) const
    {
        if (kind != other.kind || ref != other.ref || id != other.id
            || version != other.version || runId != other.runId || epoch != other.epoch)
            return false;
        if (kind == OpenWake) return !scope.isEmpty() && scope == other.scope;
        return other.scope.isEmpty() || scope == other.scope;
    }

    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("kind"), kindName()},
                           {QStringLiteral("ref"), ref},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("version"), version},
                           {QStringLiteral("run_id"), runId},
                           {QStringLiteral("scope"), scope},
                           {QStringLiteral("epoch"), epoch}};
    }

    // 严格解析：字段缺失/类型不符/身份与 ref 不自洽都返回 false。
    // 注意：不回退成默认 Reflection，否则未知 kind 可能被误当成 reflection 通过校验。
    // ref 缺失时按 kind+id 推导（身份仍被 id 绑定）；ref 非空但和 kind+id 不一致则拒绝。
    static bool fromJson(const QJsonObject &json, CognitiveOrigin *origin)
    {
        if (!origin || json.isEmpty()) return false;
        Kind parsedKind = Reflection;
        if (!kindFromName(json.value(QStringLiteral("kind")).toString(), &parsedKind)) return false;
        CognitiveOrigin parsed;
        parsed.kind = parsedKind;
        parsed.ref = json.value(QStringLiteral("ref")).toString().trimmed();
        parsed.id = json.value(QStringLiteral("id")).toInteger();
        parsed.version = json.value(QStringLiteral("version")).toInt();
        parsed.runId = json.value(QStringLiteral("run_id")).toString();
        parsed.scope = json.value(QStringLiteral("scope")).toString();
        parsed.epoch = json.value(QStringLiteral("epoch")).toInteger();
        if (parsed.ref.isEmpty() && parsed.id > 0)
            parsed.ref = refFor(parsed.kind, parsed.id);
        if (!parsed.isValid()) return false;
        *origin = parsed;
        return true;
    }
};
