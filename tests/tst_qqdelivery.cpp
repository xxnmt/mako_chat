#include <QtTest>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QJsonDocument>
#include "qqchanneladapter.h"
#include "qqprotocolclient.h"

class TestQQDelivery : public QObject {
    Q_OBJECT
private slots:
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
        QQConfig config;config.enabled=true;
        config.wsUrl=QString("ws://127.0.0.1:%1").arg(server.serverPort());
        client.setConfig(config);client.start();
        QTRY_VERIFY(client.isConnected() && peer);
        QQChannelAdapter adapter;adapter.setClient(&client);
        Utterance u;u.channel=channel;u.sessionId="12345";
        for(const QString &text:{QString("第一句。还有同单元的内容。"),QString("第二句"),QString("第三句")}) {
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
        QCOMPARE(content(sends[2]),QString("第二句"));
        QVERIFY(sends[2]["echo"]!=sends[0]["echo"]);
        ack(sends[0],true,0); // Duplicate must not advance the second sentence.
        QTest::qWait(50);QCOMPARE(sends.size(),3);
        ack(sends[2],false,1404); // Explicit terminal failure does not replay the batch.
        QTRY_COMPARE(sends.size(),4);
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
