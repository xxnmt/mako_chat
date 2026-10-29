#ifndef MEMORYTEXTUTILS_H
#define MEMORYTEXTUTILS_H

#include <QHash>
#include <QRegularExpression>
#include <QString>

namespace MemoryTextUtils {

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
