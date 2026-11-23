#include "runtimelog.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QUuid>
#include <cstdio>
#include <memory>

namespace {
struct Sink {
    QMutex mutex;
    std::unique_ptr<QFile> file;
    QString directory;
    QString sessionName;
    QDate date;
    bool separate = true;
    QtMessageHandler previous = nullptr;
};
Sink &sink()
{
    static Sink instance;
    return instance;
}

bool openFile(Sink &s, const QString &directory, bool separate, QDate date, QString *error)
{
    const auto name = separate ? s.sessionName : "logs_" + date.toString("yyyyMMdd") + ".txt";
    auto file = std::make_unique<QFile>(QDir(directory).filePath(name));
    if (!QDir().mkpath(directory) || !file->open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = file->fileName() + ": " + file->errorString();
        return false;
    }
    s.file = std::move(file);
    s.directory = directory;
    s.date = date;
    s.separate = separate;
    return true;
}

// 配置指纹：只读 setting.json + identity.json 的字节，拼起来取 SHA-256 前 12 位。
// 用途是事后把日志和当时生效的配置对上号；绝不写回配置，也不暴露密钥原文。
QString configFingerprint(const QString &configDir)
{
    QByteArray bytes;
    bool any = false;
    for (const QString &name : {QStringLiteral("setting.json"), QStringLiteral("identity.json")}) {
        QFile file(QDir(configDir).filePath(name));
        if (!file.exists() || !file.open(QIODevice::ReadOnly))
            continue;
        bytes += file.readAll();
        any = true;
    }
    if (!any) return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12));
}

void handle(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    auto &s = sink();
    QtMessageHandler previous;
    {
        QMutexLocker lock(&s.mutex);
        const auto now = QDateTime::currentDateTime();
        if (s.file && !s.separate && s.date != now.date()) {
            QString error;
            if (!openFile(s,s.directory,false,now.date(),&error))
                std::fprintf(stderr,"[RuntimeLog] %s\n",qPrintable(error));
        }
        if (s.file) {
            const QString level = type==QtDebugMsg ? "DEBUG" : type==QtInfoMsg ? "INFO" :
                type==QtWarningMsg ? "WARN" : type==QtCriticalMsg ? "ERROR" : "FATAL";
            const auto line = (now.toString(Qt::ISODateWithMs) + " [" + level + "] " + message + '\n').toUtf8();
            if (s.file->write(line)!=line.size() || !s.file->flush())
                std::fprintf(stderr,"[RuntimeLog] write failed: %s\n",qPrintable(s.file->errorString()));
        }
        previous = s.previous;
    }
    if (previous) previous(type,context,message);
    else std::fprintf(stderr,"%s\n",qPrintable(qFormatLogMessage(type,context,message)));
}
}

bool RuntimeLog::configure(const QString &directory, bool separateRuns, QString *error)
{
    auto &s = sink();
    QMutexLocker lock(&s.mutex);
    const auto path = QDir::cleanPath(QDir(directory).absolutePath());
    if (s.file && path==s.directory && separateRuns==s.separate) return true;
    return openFile(s,path,separateRuns,QDate::currentDate(),error);
}

void RuntimeLog::start(const QString &appData)
{
    QFile config(QDir(appData).filePath("config/setting.json"));
    QJsonObject settings;
    if (config.open(QIODevice::ReadOnly))
        settings = QJsonDocument::fromJson(config.readAll()).object()["logging"].toObject();
    auto &s = sink();
    s.sessionName = "logs_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz") + "_" +
                    QUuid::createUuid().toString(QUuid::Id128).left(12) + ".txt";
    const auto standard = QDir(appData).filePath("logs");
    const auto configured = settings["directory"].toString().trimmed();
    const bool separate = settings["separate_runs"].toBool(true);
    QString error;
    if (!configure(configured.isEmpty() ? standard : QDir(appData).absoluteFilePath(configured),separate,&error))
        configure(standard,separate);
    {
        QMutexLocker lock(&s.mutex);
        s.previous = qInstallMessageHandler(handle);
    }
    // 会话头：身份、版本、运行环境与配置指纹放在同一条 INFO 里，方便事后
    // 把一次运行和当时的配置对应起来。这里只读配置，不修改任何文件。
    const QString configDir = QDir(appData).filePath("config");
#ifdef QT_DEBUG
    const QString build = QStringLiteral("debug");
#else
    const QString build = QStringLiteral("release");
#endif
    qInfo().noquote() << QStringLiteral(
        "[RuntimeLog] session app=%1 version=%2 pid=%3 qt=%4 build=%5 exe_dir=%6 config=%7 config_hash=%8")
        .arg(QCoreApplication::applicationName(),
             QCoreApplication::applicationVersion(),
             QString::number(QCoreApplication::applicationPid()),
             QString::fromLatin1(qVersion()),
             build,
             QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
             QDir::toNativeSeparators(configDir),
             configFingerprint(configDir));
    if (!error.isEmpty()) qWarning().noquote() << "[RuntimeLog] requested directory unavailable:" << error;
}

void RuntimeLog::stop()
{
    auto &s = sink();
    QMutexLocker lock(&s.mutex);
    qInstallMessageHandler(s.previous);
    s.file.reset();
}
