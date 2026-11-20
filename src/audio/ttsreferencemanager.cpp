#include "ttsreferencemanager.h"
#include "configmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
QJsonObject readReferenceMap(const QDir &directory)
{
    QFile file(directory.filePath("tts_map.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

TTSReference resolveReference(const QDir &directory, const QJsonObject &entry)
{
    const auto audio = entry.value("audio_file").toString().trimmed();
    if (audio.isEmpty()) return {};
    const auto path = directory.absoluteFilePath(audio);
    if (!QFileInfo(path).isFile()) return {};
    return {path, entry.value("prompt_text").toString(),
            entry.value("prompt_language").toString("ja")};
}
} // namespace

bool TTSReferenceManager::validateDirectory(const QString &path, QString *error)
{
    const QDir directory(path);
    const auto map = readReferenceMap(directory);
    if (map.isEmpty()) {
        if (error) *error = QStringLiteral("所选文件夹需要一个可读取的 tts_map.json。");
        return false;
    }
    if (resolveReference(directory, map.value("happyIdle").toObject()).audioFilePath.isEmpty()) {
        if (error) *error = QStringLiteral("tts_map.json 需要 happyIdle 条目，并指向存在的参考音频。");
        return false;
    }
    return true;
}

TTSReference TTSReferenceManager::getReferenceForEmotion(const QString &emotion) const
{
    const QDir directory(ConfigManager::instance().ttsReferenceDirectory());
    const auto map = readReferenceMap(directory);
    auto reference = resolveReference(directory, map.value(emotion).toObject());
    if (reference.audioFilePath.isEmpty())
        reference = resolveReference(directory, map.value("happyIdle").toObject());
    return reference;
}
