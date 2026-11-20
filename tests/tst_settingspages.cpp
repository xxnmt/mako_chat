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
#include <QMessageBox>
#include <QToolButton>

#include "configmanager.h"
#include "settingspages.h"

class TestSettingsPages : public QObject
{
    Q_OBJECT
private slots:
    void editsOnlySelectedFields()
    {
        auto &config = ConfigManager::instance();
        const QByteArray initial =
            R"({"memory":{"short_term_length":15,"retrieval":{"rerank_enabled":true,"extension":7}},"api":{"deepseek_api_key":"fixture-secret","dashscope_base_url":"https://example.test/"}})";
        QVERIFY(config.writeConfigDocument("setting.json", initial, nullptr));
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
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                displayedRange = box->text().contains(QStringLiteral("10 至 200"));
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
        search->setText(QStringLiteral("历史会话"));
        QCOMPARE(stack->currentWidget(), history);
    }
};

QTEST_MAIN(TestSettingsPages)
#include "tst_settingspages.moc"
