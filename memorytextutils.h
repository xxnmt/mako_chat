#ifndef MEMORYTEXTUTILS_H
#define MEMORYTEXTUTILS_H

#include <QChar>
#include <QHash>
#include <QRegularExpression>
#include <QString>

namespace MemoryTextUtils {

// 清理会破坏 prompt 结构的不可见字符；保留可见昵称、中文、日文和 emoji。
// 只用于 prompt/展示视图，不覆盖数据库原文。
inline QString sanitizePromptText(const QString &raw)
{
    QString result;
    result.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const QChar ch = raw.at(i);
        if (ch.isHighSurrogate() && i + 1 < raw.size()
            && raw.at(i + 1).isLowSurrogate()) {
            result.append(ch);
            result.append(raw.at(++i));
            continue;
        }

        const ushort code = ch.unicode();
        if (code == u'\n' || code == u'\r' || code == u'\t') {
            result.append(QChar(u' '));
            continue;
        }
        if (code == 0x200B || code == 0x200E || code == 0x200F
            || code == 0xFEFF
            || (code >= 0x202A && code <= 0x202E)
            || (code >= 0x2066 && code <= 0x2069)) {
            continue;
        }
        if (ch.category() == QChar::Other_Control
            || ch.category() == QChar::Other_Surrogate) {
            continue;
        }
        if (ch.category() == QChar::Other_Format) {
            // ZWNJ / ZWJ 是 emoji 组合和部分文字的一部分，保留。
            if (code == 0x200C || code == 0x200D) {
                result.append(ch);
            }
            continue;
        }
        if (ch.category() == QChar::Separator_Line
            || ch.category() == QChar::Separator_Paragraph) {
            result.append(QChar(u' '));
            continue;
        }
        result.append(ch);
    }
    return result.simplified();
}

inline QString sanitizeIdentityText(const QString &raw,
                                    const QString &fallback = QStringLiteral("某位成员"))
{
    QString result = sanitizePromptText(raw);
    if (result.isEmpty()
        || QRegularExpression(QStringLiteral("^\\d{5,}$")).match(result).hasMatch()) {
        result = fallback.isEmpty() ? QStringLiteral("某位成员") : fallback;
    }
    if (result.size() > 64) {
        result = result.left(64);
        if (!result.isEmpty() && result.at(result.size() - 1).isHighSurrogate()) {
            result.chop(1);
        }
    }
    return result;
}

// 生成阶段的 P1/P2 只是内部标签，不能进入持久化文本。
inline QString replaceSpeakerLabels(
    const QString &text,
    const QHash<QString, QString> &labelToDisplay)
{
    QString result = text;
    for (auto it = labelToDisplay.cbegin(); it != labelToDisplay.cend(); ++it) {
        const QString display = it.value().trimmed();
        if (display.isEmpty()) {
            continue;
        }
        result.replace(
            QRegularExpression(QStringLiteral("\\b%1\\b").arg(
                QRegularExpression::escape(it.key()))),
            display);
    }
    result.replace(QRegularExpression(QStringLiteral("\\bP\\d+\\b")),
                   QStringLiteral("某位成员"));
    return result;
}

inline bool containsSpeakerLabel(const QString &text)
{
    return QRegularExpression(QStringLiteral("\\bP\\d+\\b")).match(text).hasMatch();
}

} // namespace MemoryTextUtils

#endif // MEMORYTEXTUTILS_H
