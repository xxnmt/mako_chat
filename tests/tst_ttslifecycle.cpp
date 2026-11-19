#include <QtTest>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "ttsservice.h"
#include "configmanager.h"
#include "iaudiosink.h"

class RecordingSink : public IAudioSink
{
public:
    using IAudioSink::IAudioSink;
    QStringList texts;
    void beginStream(const SentenceText &, int) override {}
    void writePcm(const QByteArray &) override {}
    void updateSampleRate(int) override {}
    void endStream(const SentenceText &) override {}
    void abortPendingStream() override {}
    void playFile(const QString &, const SentenceText &s, int = 24000) override { texts << s.zhText; }
    bool isIdle() const override { return true; }
    void stop() override {}
};

class TestTtsLifecycle : public QObject
{
    Q_OBJECT
private slots:
    void mockPreservesTextAcrossReload()
    {
        auto &config = ConfigManager::instance();
        const auto original = config.getTTSMode();
        const auto restore = qScopeGuard([&]{config.setTTSMode(original);});
        config.setTTSMode("mock");
        TTSService service;
        RecordingSink sink;
        service.setAudioSink(&sink);
        SentenceText first, second;
        first.zhText = "first";
        second.zhText = "second";
        service.enqueueSentences({first, second});
        config.setTTSMode("text");
        service.reloadProvider();
        QTRY_COMPARE(sink.texts.size(), 2);
        QCOMPARE(sink.texts, (QStringList{"first", "second"}));
        QVERIFY(service.findChildren<TTSProcessManager *>().isEmpty());
        service.setAudioSink(nullptr);
    }

    void occupiedPortIsNotOwned()
    {
        QTcpServer external;
        QVERIFY(external.listen(QHostAddress::LocalHost));
        TTSProcessManager manager("nonexistent.exe", ".", {}, external.serverPort());
        QSignalSpy failed(&manager, &TTSProcessManager::apiFailed);
        manager.apiStart();
        QCOMPARE(failed.size(), 1);
        manager.apiStop();
        QVERIFY(external.isListening());
        QTcpSocket client;
        client.connectToHost("127.0.0.1", external.serverPort());
        QVERIFY(client.waitForConnected(500));
    }

    void failedStartCanBeStoppedAndRetried()
    {
        QTcpServer freePort;
        QVERIFY(freePort.listen(QHostAddress::LocalHost));
        const auto port = freePort.serverPort();
        freePort.close();
        TTSProcessManager manager("does-not-exist.exe", ".", {}, port);
        QSignalSpy failed(&manager, &TTSProcessManager::apiFailed);
        manager.apiStart();
        QTRY_VERIFY(!failed.isEmpty());
        manager.apiStop();
        const int before = failed.size();
        manager.apiStart();
        QTRY_VERIFY(failed.size() > before);
        manager.apiStop();
        QVERIFY(!manager.isApiReady());
    }

    void ownedProcessStopsAndRestarts()
    {
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost));
        const auto port = reserve.serverPort();
        reserve.close();
        QTemporaryDir dir;
        TTSProcessManager manager(QCoreApplication::applicationFilePath(), dir.path(),
                                  {"--test-port", QString::number(port)}, port);
        QSignalSpy ready(&manager, &TTSProcessManager::apiReady);
        manager.apiStart();
        QTRY_COMPARE(ready.size(), 1);
        manager.apiStart();
        QCOMPARE(ready.size(), 1);
        manager.apiStop();
        QVERIFY(!manager.isApiReady());
        manager.apiStart();
        QTRY_COMPARE(ready.size(), 2);
        manager.apiStop();
    }

    void apiFailureStillDeliversQueuedText()
    {
        auto &config = ConfigManager::instance();
        const auto original = config.getTTSMode();
        const auto restore = qScopeGuard([&]{config.setTTSMode(original);});
        const auto python = config.getPythonPath();
        const auto restorePython = qScopeGuard([&]{config.setPythonPath(python);});
        config.setPythonPath("missing-tts-test.exe");
        config.setTTSMode("api");
        TTSService service;
        RecordingSink sink;
        service.setAudioSink(&sink);
        SentenceText sentence;
        sentence.zhText = "fallback text";
        service.enqueueSentences({sentence});
        QTRY_COMPARE(sink.texts, (QStringList{"fallback text"}));
        service.setAudioSink(nullptr);
    }

    void switchingPendingApiToMockPreservesOrder()
    {
        auto &config = ConfigManager::instance();
        const auto original = config.getTTSMode();
        const auto python = config.getPythonPath();
        const auto restore = qScopeGuard([&]{
            config.setTTSMode(original);
            config.setPythonPath(python);
        });
        config.setPythonPath("missing-tts-test.exe");
        config.setTTSMode("api");
        TTSService service;
        RecordingSink sink;
        service.setAudioSink(&sink);
        SentenceText first, second;
        first.zhText = "first";
        second.zhText = "second";
        service.enqueueSentences({first, second});
        config.setTTSMode("mock");
        service.reloadProvider();
        QTRY_COMPARE(sink.texts, (QStringList{"first", "second"}));
        QVERIFY(service.findChildren<TTSProcessManager *>().isEmpty());
        service.setAudioSink(nullptr);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto index = args.indexOf("--test-port");
    if (index >= 0) {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, args.value(index+1).toUShort())) return 2;
        return app.exec();
    }
    TestTtsLifecycle test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_ttslifecycle.moc"
