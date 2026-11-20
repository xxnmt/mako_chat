#include <QtTest>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QDialog>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTableWidget>

#include "configmanager.h"
#include "settingspages.h"
#include "ttsreferencemanager.h"
#include <QTemporaryDir>

class TestSettingsPages : public QObject
{
    Q_OBJECT
private slots:
    void editsOnlySelectedFields()
    {
        auto &config = ConfigManager::instance();
        const QByteArray initial =
            R"({"memory":{"short_term_length":15,"retrieval":{"rerank_enabled":true,"extension":7}},"api":{"deepseek_api_key":"fixture-secret","dashscope_base_url":"https://example.test/"},"qq":{"group_memory_allow":[10001],"reply":{"group":"mention","group_allow":[10001],"private":"allowlist","private_allow":[20001]}}})";
        QVERIFY(config.writeConfigDocument("setting.json", initial, nullptr));
        const auto refDir = config.getAppDataPath() + "/reference_audio";
        QVERIFY(QDir().mkpath(refDir));
        QFile map(refDir + "/tts_map.json");
        QVERIFY(map.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(map.write(R"({"happyIdle":{"audio_file":"base.wav","prompt_text":"Sample","prompt_language":"ja"}})") > 0);
        map.close();
        int applied = 0;
        QWidget host;
        auto *history = new QLabel("History");
        auto *root = buildSettingsPages(history, [&] { ++applied; }, &host);
        root->resize(900, 640);
        host.resize(900, 640);
        host.show();
        root->show();
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        QVERIFY(search);
        auto *navigation = root->findChild<QListWidget *>();
        QVERIFY(navigation);
        QCoreApplication::processEvents();
        const auto preview = qEnvironmentVariable("MAKO_SETTINGS_PREVIEW");
        if (!preview.isEmpty()) QVERIFY(root->grab().save(preview));
        auto *secret = root->findChild<QWidget *>("api.deepseek_api_key")->findChild<QLineEdit *>();
        QCOMPARE(secret->echoMode(), QLineEdit::Password);
        auto *endpoint = root->findChild<QLineEdit *>("memory.retrieval.embedding_endpoint");
        QVERIFY(endpoint);
        QVERIFY(endpoint->text().isEmpty());
        QCOMPARE(endpoint->placeholderText(), QStringLiteral("https://example.test/compatible-mode/v1/embeddings"));
        auto *help = root->findChild<QToolButton *>("memory.retrieval.candidate_limit.help");
        QVERIFY(help);
        bool displayedRange = false;
        QTimer::singleShot(0, &host, [&] {
            if (auto *box = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                const auto *body = box->findChild<QPlainTextEdit *>();
                displayedRange = body && body->toPlainText().contains(QStringLiteral("10 至 200"));
                box->accept();
            }
        });
        help->click();
        QVERIFY(displayedRange);
        search->setText("rerank_enabled");
        QCOMPARE(navigation->currentItem()->text(), QStringLiteral("记忆与检索"));
        auto *rerank = root->findChild<QCheckBox *>("memory.retrieval.rerank_enabled");
        QVERIFY(rerank && rerank->isVisible());
        auto *length = root->findChild<QSpinBox *>("memory.short_term_length");
        QVERIFY(length && !length->isVisible());
        auto *stack = root->findChild<QStackedWidget *>();
        auto *apply = stack->currentWidget()->findChild<QPushButton *>("applySettings");
        QVERIFY(apply && apply->isVisible());
        rerank->setChecked(false);
        apply->click();
        QCOMPARE(applied, 1);
        QVERIFY(!config.getMemoryPolicyConfig().retrieval.rerankEnabled);
        QFile saved(config.getConfigFilePath());
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto bytes = saved.readAll();
        saved.close();
        QVERIFY(bytes.contains("\"extension\": 7"));
        QVERIFY(bytes.contains("fixture-secret"));
        QVERIFY(!bytes.contains("candidate_limit"));
        QVERIFY(!bytes.contains("embedding_endpoint"));
        search->setText(QStringLiteral("语音合成"));
        auto *referenceRow = root->findChild<QWidget *>("tts.reference_audio_dir");
        QVERIFY(referenceRow);
        auto *reference = referenceRow->findChild<QLineEdit *>();
        QCOMPARE(reference->text(), refDir);
        QVERIFY(reference->isReadOnly());
        QVERIFY(!root->findChild<QWidget *>("tts.reference_mode"));
        QTemporaryDir custom;
        QVERIFY(custom.isValid());
        auto *voiceApply = stack->currentWidget()->findChild<QPushButton *>("applySettings");
        reference->setText(custom.path());
        voiceApply->click();
        QCOMPARE(applied, 1); // Incomplete folders do not change the active configuration.
        QFile customMap(custom.filePath("tts_map.json"));
        QVERIFY(customMap.open(QIODevice::WriteOnly));
        QVERIFY(customMap.write(R"({"happyIdle":{"audio_file":"base.wav","prompt_text":"New","prompt_language":"zh"},"sad":{"audio_file":"sad.wav","prompt_text":"Sad","prompt_language":"ja"}})") > 0);
        customMap.close();
        for (const auto &name : {"base.wav", "sad.wav"}) {
            QFile sample(custom.filePath(name));
            QVERIFY(sample.open(QIODevice::WriteOnly));
        }
        voiceApply->click();
        QCOMPARE(applied, 2);
        const auto &references = TTSReferenceManager::instance();
        QCOMPARE(references.getReferenceForEmotion("sad").audioFilePath, custom.filePath("sad.wav"));
        QCOMPARE(references.getReferenceForEmotion("sad").promptLanguage, QStringLiteral("ja"));
        QCOMPARE(references.getReferenceForEmotion("unknown").promptLanguage, QStringLiteral("zh"));
        for (auto *button : referenceRow->findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("恢复默认")) button->click();
        voiceApply->click();
        QCOMPARE(applied, 3);
        QCOMPARE(config.ttsReferenceDirectory(), refDir);
        search->setText(QStringLiteral("QQ 渠道"));
        auto *groups = root->findChild<QWidget *>("qq.group_permissions")->findChild<QTableWidget *>();
        auto *accounts = root->findChild<QWidget *>("qq.private_permissions")->findChild<QTableWidget *>();
        QCOMPARE(groups->rowCount(), 1);
        QCOMPARE(groups->item(0, 1)->checkState(), Qt::Checked);
        QCOMPARE(groups->item(0, 2)->checkState(), Qt::Checked);
        QCOMPARE(accounts->item(0, 1)->checkState(), Qt::Checked);
        groups->item(0, 2)->setCheckState(Qt::Unchecked);
        accounts->item(0, 1)->setCheckState(Qt::Unchecked);
        stack->currentWidget()->findChild<QPushButton *>("applySettings")->click();
        QCOMPARE(applied, 4);
        const auto qq = config.getQQConfig();
        QVERIFY(qq.reply.restrictEmptyGroupList);
        QVERIFY(qq.reply.groupAllow.isEmpty());
        QVERIFY(qq.groupMemory.groupAllow.contains(10001));
        QVERIFY(qq.reply.privateMemoryAllow && qq.reply.privateMemoryAllow->isEmpty());
        QQInboundMessage message;
        message.valid = true;
        message.channel = "qq_private";
        message.senderId = "20001";
        const auto decision = QQInboundPolicyEngine::decide(message, qq.reply, qq.groupMemory,
                                                            config.getMemoryPolicyConfig(), {});
        QVERIFY(decision.allowReply && decision.ephemeralReply);
        QVERIFY(!decision.persistRaw && !decision.persistChat);
        search->setText(QStringLiteral("历史会话"));
        QCOMPARE(stack->currentWidget(), history);
    }
};

QTEST_MAIN(TestSettingsPages)
#include "tst_settingspages.moc"
