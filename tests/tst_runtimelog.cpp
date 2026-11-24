#include "runtimelog.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <cstdio>

class TestRuntimeLog : public QObject {
    Q_OBJECT
private slots:
    void restoresSelectedDirectoryAndAppendsAcrossStarts()
    {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkdir("config"));
        QFile config(root.filePath("config/setting.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(R"({"logging":{"directory":"selected","separate_runs":false}})");
        config.close();
        for (int n = 0; n < 2; ++n) {
            RuntimeLog::start(root.path());
            qInfo() << "session-marker" << n;
            RuntimeLog::stop();
        }
        const QDir logs(root.filePath("selected"));
        QCOMPARE(logs.entryList({"logs_*.txt"}).size(),1);
        QFile log(logs.filePath(logs.entryList({"logs_*.txt"}).first()));
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count("session-marker"),2);
    }
    void savesImmediatelyAndChangesDirectory()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        RuntimeLog::start(root.path());
        auto guard = qScopeGuard([] { RuntimeLog::stop(); });
        const QDir initial(root.filePath("logs"));
        QCOMPARE(initial.entryList({"logs_*.txt"}).size(), 1);
        qInfo().noquote() << QStringLiteral("第一条消息");
        QFile file(initial.filePath(initial.entryList({"logs_*.txt"}).first()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains(QStringLiteral("[INFO] 第一条消息").toUtf8()));
        const QDir selected(root.filePath("selected"));
        QVERIFY(RuntimeLog::configure(selected.path(), false));
        const QString name = selected.entryList({"logs_*.txt"}).value(0);
        QCOMPARE(name, "logs_" + QDate::currentDate().toString("yyyyMMdd") + ".txt");
        QFile daily(selected.filePath(name));
        QVERIFY(!RuntimeLog::configure(daily.fileName(), true));
        qWarning() << "after-switch";
        QVERIFY(daily.open(QIODevice::ReadOnly));
        QVERIFY(daily.readAll().contains("[WARN] after-switch"));
        QVERIFY(!file.readAll().contains("after-switch"));
    }

    void sessionHeaderDescribesRunAndConfig()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkdir("config"));
        const QByteArray settings = R"({"logging":{"separate_runs":true}})";
        const QByteArray identity = R"({"identity":{"master":{"qq":["10001"]}}})";
        QFile setting(root.filePath("config/setting.json"));
        QVERIFY(setting.open(QIODevice::WriteOnly));
        QVERIFY(setting.write(settings) == settings.size());
        setting.close();
        QFile identityFile(root.filePath("config/identity.json"));
        QVERIFY(identityFile.open(QIODevice::WriteOnly));
        QVERIFY(identityFile.write(identity) == identity.size());
        identityFile.close();

        RuntimeLog::start(root.path());
        auto guard = qScopeGuard([] { RuntimeLog::stop(); });
        qInfo().noquote() << QStringLiteral("header-marker");

        const QDir logs(root.filePath("logs"));
        QCOMPARE(logs.entryList({"logs_*.txt"}).size(), 1);
        QFile log(logs.filePath(logs.entryList({"logs_*.txt"}).first()));
        QVERIFY(log.open(QIODevice::ReadOnly));
        // 会话头必须是新文件的第一条记录。
        const QString header = QString::fromUtf8(log.readLine());
        QVERIFY(header.contains("[INFO]"));
        QVERIFY(header.contains("pid="));
        QVERIFY(header.contains("qt="));
        QVERIFY(header.contains("config_hash="));
        QVERIFY(header.contains("app="));
        QVERIFY(header.contains("config="));
        // 指纹只读配置字节：setting.json + identity.json 的 SHA-256 前 12 位。
        const QByteArray expected =
            QCryptographicHash::hash(settings + identity, QCryptographicHash::Sha256)
                .toHex().left(12);
        QVERIFY(header.contains(QStringLiteral("config_hash=") + QString::fromLatin1(expected)));
        QVERIFY(log.readAll().contains("header-marker"));
    }

    void newRunAndProcessTermination()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QDir logs(root.filePath("logs"));
        for (int run = 1; run <= 2; ++run) {
            QProcess child;
            // 不依赖 QProcess 管道读取子进程 stdout（部分受限环境管道不可用）：
            // 把输出重定向到文件，断言的属性仍是"子进程启动后写出 ready"。
            const QString readyFile = root.filePath(QStringLiteral("child-ready-%1.txt").arg(run));
            child.setStandardOutputFile(readyFile);
            child.start(QCoreApplication::applicationFilePath(), {"--child-log",root.path()});
            QVERIFY(child.waitForStarted());
            auto childReady = [&readyFile] {
                QFile file(readyFile);
                return file.open(QIODevice::ReadOnly)
                    && file.readAll().trimmed().contains("ready");
            };
            QTRY_VERIFY_WITH_TIMEOUT(childReady(), 10000);
            child.kill();
            QVERIFY(child.waitForFinished());
            QCOMPARE(logs.entryList({"*.txt"}).size(),run);
        }
        for (const auto &name : logs.entryList({"*.txt"})) {
            QFile file(logs.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.readAll().contains("[INFO] last-before-termination"));
        }
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size()==3 && args[1]=="--child-log") {
        RuntimeLog::start(args[2]);
        qInfo() << "last-before-termination";
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }
    TestRuntimeLog test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_runtimelog.moc"
