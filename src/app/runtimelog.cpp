#include "runtimelog.h"
#include <QCoreApplication>
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
    if (!error.isEmpty()) qWarning().noquote() << "[RuntimeLog] requested directory unavailable:" << error;
    qInfo().noquote() << "[RuntimeLog] started pid=" << QCoreApplication::applicationPid();
}

void RuntimeLog::stop()
{
    auto &s = sink();
    QMutexLocker lock(&s.mutex);
    qInstallMessageHandler(s.previous);
    s.file.reset();
}
