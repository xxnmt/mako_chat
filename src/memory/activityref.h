#ifndef ACTIVITYREF_H
#define ACTIVITYREF_H

#include <QString>

// R12-1b：活动的统一引用身份。
// legacy 活动存在 chat_history 的 thought/thought_result 行里，引用格式保持 thought/<id>；
// internal 活动存在 v31 的 internal_activity 行里，引用格式为 activity/<id>（与
// currentActivitiesUnified 的 origin/ref 约定一致）。
// 两张表可以有相同的整数 id，但身份不同：解析时必须带前缀，禁止把裸整数 id 当身份。
struct ActivityRef
{
    enum Kind {
        LegacyThought,      // chat_history.thought（存量适配）
        InternalActivity    // internal_activity（新增活动）
    };

    Kind kind = LegacyThought;
    qlonglong id = 0;

    static ActivityRef legacy(qlonglong id)
    {
        ActivityRef ref;
        ref.kind = LegacyThought;
        ref.id = id;
        return ref;
    }

    static ActivityRef internal(qlonglong id)
    {
        ActivityRef ref;
        ref.kind = InternalActivity;
        ref.id = id;
        return ref;
    }

    bool isValid() const { return id > 0; }

    // "thought/<id>" 或 "activity/<id>"；无效引用返回空串。
    QString ref() const
    {
        if (!isValid()) return QString{};
        return kind == InternalActivity
            ? QStringLiteral("activity/%1").arg(id)
            : QStringLiteral("thought/%1").arg(id);
    }

    // "legacy" / "internal"：与 currentActivitiesUnified 的 origin 字段同义。
    QString originKind() const
    {
        return kind == InternalActivity ? QStringLiteral("internal")
                                        : QStringLiteral("legacy");
    }

    // 解析统一引用。只接受带前缀的引用；裸整数一律拒绝（避免把两张表的同 id 混用）。
    // 兼容旧格式 thought/<id>:<version>：冒号后的附加段不参与身份判定。
    // error 输出：empty_ref / invalid_ref / invalid_id / unknown_prefix。
    static bool parse(const QString &refText, ActivityRef *out, QString *error = nullptr)
    {
        if (error) error->clear();
        if (out) *out = ActivityRef{};
        const auto fail = [error](const QString &code) {
            if (error) *error = code;
            return false;
        };
        const QString trimmed = refText.trimmed();
        if (trimmed.isEmpty()) return fail(QStringLiteral("empty_ref"));
        const int slash = trimmed.indexOf(QLatin1Char('/'));
        if (slash <= 0) return fail(QStringLiteral("invalid_ref"));
        const QString prefix = trimmed.left(slash);
        const QString idText = trimmed.mid(slash + 1).section(QLatin1Char(':'), 0, 0);
        bool ok = false;
        const qlonglong parsedId = idText.toLongLong(&ok);
        if (!ok || parsedId <= 0) return fail(QStringLiteral("invalid_id"));
        Kind parsedKind = LegacyThought;
        if (prefix == QStringLiteral("thought")) {
            parsedKind = LegacyThought;
        } else if (prefix == QStringLiteral("activity")) {
            parsedKind = InternalActivity;
        } else {
            return fail(QStringLiteral("unknown_prefix"));
        }
        if (out) {
            out->kind = parsedKind;
            out->id = parsedId;
        }
        return true;
    }

    // R12-1b 补充：产物引用（artifact_ref）只允许指回活动成果自身：
    // activity/<id> 或 thought/<id>。前端/模型不能写入任意路径或指向别的活动。
    bool isArtifactRefFor(const ActivityRef &activity) const
    {
        return isValid() && kind == activity.kind && id == activity.id;
    }

    // 解析产物引用文本；非法/裸 id 返回 false。语义与 parse 一致，单独命名便于读代码。
    static bool parseArtifactRef(const QString &refText, ActivityRef *out,
                                 QString *error = nullptr)
    {
        return parse(refText, out, error);
    }

    bool operator==(const ActivityRef &other) const
    {
        return kind == other.kind && id == other.id;
    }

    bool operator!=(const ActivityRef &other) const { return !(*this == other); }
};

#endif // ACTIVITYREF_H
