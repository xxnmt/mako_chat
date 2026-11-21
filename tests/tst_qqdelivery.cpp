#include <QtTest>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QJsonDocument>
#include "qqchanneladapter.h"
#include "qqprotocolclient.h"
#include "textplaybacktiming.h"

class TestQQDelivery : public QObject {
    Q_OBJECT
private slots:
    void textPacingAndSlowAcknowledgement() {
        QCOMPARE(simulatedTextPlaybackMs(""),1000);
        QCOMPARE(simulatedTextPlaybackMs("12345678"),2000);
        QWebSocketServer server("pacing-test",QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QJsonObject> sends;
        QList<qint64> times;
        QElapsedTimer clock;clock.start();
        QWebSocket *peer=nullptr;
        connect(&server,&QWebSocketServer::newConnection,&server,[&] {
            peer=server.nextPendingConnection();peer->setParent(&server);
            connect(peer,&QWebSocket::textMessageReceived,&server,[&](const QString &text) {
                const auto obj=QJsonDocument::fromJson(text.toUtf8()).object();
                if(obj["action"].toString().startsWith("send_")) {sends.append(obj);times.append(clock.elapsed());}
            });
        });
        QQProtocolClient client;QQConfig cfg;cfg.enabled=true;
        cfg.wsUrl=QString("ws://127.0.0.1:%1").arg(server.serverPort());client.setConfig(cfg);client.start();
        QTRY_VERIFY(client.isConnected() && peer);
        QQChannelAdapter adapter;adapter.setClient(&client);
        Utterance u;u.channel=Channel::kQQPrivate;u.sessionId="12345";u.text="四个文字";
        QVERIFY(adapter.sendUtterance(u));QVERIFY(adapter.sendUtterance(u));QVERIFY(adapter.sendUtterance(u));
        QTRY_COMPARE(sends.size(),1);
        auto ack=[&](int i) {peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"echo",sends[i]["echo"]},{"status","ok"},{"retcode",0}}).toJson()));};
        ack(0);QTest::qWait(350);QCOMPARE(sends.size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(sends.size(),2,2000);
        QVERIFY(times[1]-times[0]>=950);
        QTest::qWait(1100);QCOMPARE(sends.size(),2); // Clock alone cannot advance without receipt.
        const auto ackAt=clock.elapsed();ack(1);
        QTRY_COMPARE_WITH_TIMEOUT(sends.size(),3,700);
        QVERIFY(times[2]-ackAt<700); // Slow receipt does not add a second full delay.
        QVERIFY(adapter.sendUtterance(u));ack(2);QTest::qWait(100);
        client.stop();QTest::qWait(1100);QCOMPARE(sends.size(),3);
    }
    void orderedSentences_data() {
        QTest::addColumn<QString>("channel");
        QTest::newRow("private") << Channel::kQQPrivate;
        QTest::newRow("group") << Channel::kQQGroup;
    }
    void orderedSentences() {
        QFETCH(QString,channel);
        QWebSocketServer server("local-test",QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QJsonObject> sends;
        QWebSocket *peer=nullptr;
        connect(&server,&QWebSocketServer::newConnection,&server,[&] {
            peer=server.nextPendingConnection(); peer->setParent(&server);
            connect(peer,&QWebSocket::textMessageReceived,&server,[&](const QString &text) {
                const auto obj=QJsonDocument::fromJson(text.toUtf8()).object();
                if(obj["action"].toString().startsWith("send_")) sends.append(obj);
            });
        });
        QQProtocolClient client;
        QSignalSpy receipts(&client, &QQProtocolClient::sendResult);
        QQConfig config;config.enabled=true;
        config.wsUrl=QString("ws://127.0.0.1:%1").arg(server.serverPort());
        client.setConfig(config);client.start();
        QTRY_VERIFY(client.isConnected() && peer);
        QQChannelAdapter adapter;adapter.setClient(&client);
        Utterance u;u.channel=channel;u.sessionId="12345";
        for(const QString &text:{QString("第一句。还有同单元的内容。"),QString("第二句"),QString("第三句")}) {
            u.deliveryEcho = text == "第一句。还有同单元的内容。" ? QStringLiteral("reflection:12:1") : QString();
            u.text=text;QVERIFY(adapter.sendUtterance(u));
        }
        QTRY_COMPARE(sends.size(),1);
        auto ack=[&](const QJsonObject &sent,bool ok,int code) {
            peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"echo",sent["echo"]},{"status",ok ? "ok":"failed"},{"retcode",code}}).toJson()));
        };
        auto content=[](const QJsonObject &s) {
            return s["params"].toObject()["message"].toArray().first().toObject()["data"].toObject()["text"].toString();
        };
        QCOMPARE(content(sends[0]),QString("第一句。还有同单元的内容。"));
        QCOMPARE(sends[0]["action"].toString(),channel==Channel::kQQGroup ? "send_group_msg":"send_private_msg");
        ack(sends[0],false,100);
        QTest::qWait(100);QCOMPARE(sends.size(),1); // Retry blocks later sentences.
        ack(sends[0],false,100); // Duplicate failure while waiting is not a second attempt.
        QTRY_COMPARE_WITH_TIMEOUT(sends.size(),2,6000);
        QCOMPARE(sends[1],sends[0]);
        ack(sends[1],true,0);
        QTRY_COMPARE(sends.size(),3);
        QCOMPARE(receipts.size(),1);
        QCOMPARE(receipts.first().at(0).toString(),QString("reflection:12:1"));
        QCOMPARE(receipts.first().at(1).toString(),QString("protocol_accepted"));
        QCOMPARE(content(sends[2]),QString("第二句"));
        QVERIFY(sends[2]["echo"]!=sends[0]["echo"]);
        ack(sends[0],true,0); // Duplicate must not advance the second sentence.
        QTest::qWait(50);QCOMPARE(sends.size(),3);
        ack(sends[2],false,1404); // Explicit terminal failure does not replay the batch.
        QTRY_COMPARE(sends.size(),4);
        QCOMPARE(receipts.size(),2);
        QCOMPARE(receipts.last().at(1).toString(),QString("rejected"));
        QCOMPARE(content(sends[3]),QString("第三句"));
        QVERIFY(sends[3]["echo"]!=sends[2]["echo"]);
        u.text="不应在重连后补发";QVERIFY(adapter.sendUtterance(u));
        peer->close();
        QTRY_VERIFY(!client.isConnected());
        QVERIFY(!adapter.sendUtterance(u));
        QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(),5000);
        QTest::qWait(50);QCOMPARE(sends.size(),4);
        client.stop();
    }
};
QTEST_GUILESS_MAIN(TestQQDelivery)
#include "tst_qqdelivery.moc"
