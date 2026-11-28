#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "configmanager.h"
#include "settingspages.h"
#include "settingsapplyplan.h"
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
        search->setText(QStringLiteral("认知与自主行为"));
        // 执行端约束：联系每日上限、关注默认间隔与保留天数都要在设置页可编辑。
        struct NumericField {
            QString path;
            int minimum;
            int maximum;
        };
        for (const auto &item : {NumericField{QStringLiteral("proactive.contact_daily_limit"), 0, 99},
                                 NumericField{QStringLiteral("proactive.concern_default_check_minutes"), 0, 10080},
                                 NumericField{QStringLiteral("proactive.concern_retention_days"), 1, 365}}) {
            auto *box = root->findChild<QSpinBox *>(item.path);
            QVERIFY2(box, qPrintable(item.path));
            QCOMPARE(box->minimum(), item.minimum);
            QCOMPARE(box->maximum(), item.maximum);
        }
        search->setText(QStringLiteral("历史会话"));
        QCOMPARE(stack->currentWidget(), history);
    }
    void cognitionPageDefaultsWhenIdentityHasNoCognition()
    {
        auto &config = ConfigManager::instance();
        QVERIFY(config.writeConfigDocument(QStringLiteral("identity.json"),
            R"({"identity":{"self":{"qq":[10001]},"master":{"qq":[20001]}},"memory":{"long_term_memory_enabled":true},"proactive":{"daily_limit":288,"contact_daily_limit":0,"cooldown_seconds":60,"idle_seconds":120}})",
            nullptr));
        QWidget host;
        auto *root = buildSettingsPages(new QLabel("History"), [] {}, &host);
        host.resize(900, 700);
        host.show();
        root->show();
        QCoreApplication::processEvents();
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        search->setText(QStringLiteral("认知与自主行为"));
        QCOMPARE(root->findChild<QCheckBox *>("cognition.open_wake.enabled")->isChecked(), false);
        QCOMPARE(root->findChild<QCheckBox *>("cognition.open_wake.allow_contact")->isChecked(), false);
        QCOMPARE(root->findChild<QSpinBox *>("cognition.open_wake.max_per_day")->value(), 12);
        QCOMPARE(root->findChild<QSpinBox *>("cognition.open_wake.shared_budget_percent")->value(), 30);
        QCOMPARE(root->findChild<QSpinBox *>("cognition.snapshot_budget_bytes")->value(), 4096);
        QCOMPARE(root->findChild<QCheckBox *>("cognition.context_actual_enabled")->isChecked(), false);
        auto *status = root->findChild<QLabel *>("cognition.context_actual_enabled.status");
        QVERIFY(status && status->text().contains(QStringLiteral("配置")));
        QVERIFY(status->text().contains(QStringLiteral("关")));
        // 每天三个上限分别显示，且沿用现有配置值。
        QCOMPARE(root->findChild<QSpinBox *>("proactive.daily_limit")->value(), 288);
        QCOMPARE(root->findChild<QSpinBox *>("proactive.contact_daily_limit")->value(), 0);
    }

    void settingsNavigationConsolidatedWithoutDuplicateLimits()
    {
        auto &config = ConfigManager::instance();
        QVERIFY(config.writeConfigDocument(QStringLiteral("identity.json"),
            R"({"proactive":{"daily_limit":288,"contact_daily_limit":0,"cooldown_seconds":60,"idle_seconds":120},"cognition":{"open_wake":{"max_per_day":12}}})",
            nullptr));
        QWidget host;
        auto *root = buildSettingsPages(new QLabel("History"), [] {}, &host);
        host.resize(900, 700);
        host.show();
        root->show();
        QCoreApplication::processEvents();
        auto *navigation = root->findChild<QListWidget *>();
        QVERIFY(navigation);
        QStringList titles;
        for (int i = 0; i < navigation->count(); ++i) titles.append(navigation->item(i)->text());
        QVERIFY(titles.contains(QStringLiteral("认知与自主行为")));
        QVERIFY(!titles.contains(QStringLiteral("主动交流")));
        QVERIFY(titles.contains(QStringLiteral("高级与诊断")));
        // 三个上限只在认知页各有一个主要编辑入口。
        QCOMPARE(root->findChildren<QWidget *>(QStringLiteral("proactive.daily_limit")).size(), 1);
        QCOMPARE(root->findChildren<QWidget *>(QStringLiteral("proactive.contact_daily_limit")).size(), 1);
        QCOMPARE(root->findChildren<QWidget *>(QStringLiteral("cognition.open_wake.max_per_day")).size(), 1);
        // 高级诊断字段不再出现在记忆与检索主要入口。
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        auto *stack = root->findChild<QStackedWidget *>();
        search->setText(QStringLiteral("记忆与检索"));
        QVERIFY(!stack->currentWidget()->findChild<QWidget *>("memory.retrieval.embedding_provider"));
        search->setText(QStringLiteral("高级与诊断"));
        QVERIFY(stack->currentWidget()->findChild<QWidget *>("memory.retrieval.embedding_provider"));
        QVERIFY(!stack->currentWidget()->findChild<QWidget *>("memory.agent_plan_v2"));
        search->setText(QStringLiteral("日志")); // 日志已移入高级与诊断
        QVERIFY(stack->currentWidget()->findChild<QWidget *>("logging.directory"));
    }

    void cognitionPageRejectsInvalidIntervalCombination()
    {
        auto &config = ConfigManager::instance();
        const QByteArray identity =
            R"({"proactive":{"daily_limit":288,"contact_daily_limit":0},"cognition":{"open_wake":{"min_interval_seconds":300,"default_interval_seconds":1800,"max_interval_seconds":86400}}})";
        QVERIFY(config.writeConfigDocument("identity.json", identity, nullptr));
        QWidget host;
        auto *root = buildSettingsPages(new QLabel("History"), [] {}, &host);
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        search->setText(QStringLiteral("认知与自主行为"));
        root->findChild<QCheckBox *>("showAdvancedSettings")->setChecked(true);
        QCoreApplication::processEvents();
        auto *minimum = root->findChild<QSpinBox *>(
            "cognition.open_wake.min_interval_seconds");
        auto *preferred = root->findChild<QSpinBox *>(
            "cognition.open_wake.default_interval_seconds");
        auto *apply = root->findChild<QStackedWidget *>()->currentWidget()
                          ->findChild<QPushButton *>("applySettings");
        QVERIFY(minimum && preferred && apply);
        minimum->setValue(7200);
        preferred->setValue(1800);
        apply->click();
        auto *feedback = root->findChild<QStackedWidget *>()->currentWidget()
                             ->findChild<QLabel *>("settingsFeedback");
        QVERIFY(feedback);
        QVERIFY2(feedback->text().contains(QStringLiteral("最小唤醒间隔")),
                 qPrintable(feedback->text()));
        QFile file(config.getConfigDirPath() + "/identity.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(saved.value("cognition").toObject().value("open_wake").toObject()
                     .value("min_interval_seconds").toInt(), 300);
    }

    void cognitionPageShowsNormalizedEffectiveValues()
    {
        auto &config = ConfigManager::instance();
        QVERIFY(config.writeConfigDocument(QStringLiteral("identity.json"),
            R"({"cognition":{"open_wake":{"min_interval_seconds":7200,"default_interval_seconds":1800,"max_interval_seconds":3600},"snapshot_budget_bytes":99999}})",
            nullptr));
        QWidget host;
        auto *root = buildSettingsPages(new QLabel("History"), [] {}, &host);
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        search->setText(QStringLiteral("认知与自主行为"));
        root->findChild<QCheckBox *>("showAdvancedSettings")->setChecked(true);
        QCoreApplication::processEvents();
        // ConfigManager 归一后：最小=最大=默认=7200，快照预算夹到 8192。
        QCOMPARE(root->findChild<QSpinBox *>(
                     "cognition.open_wake.min_interval_seconds")->value(), 7200);
        QCOMPARE(root->findChild<QSpinBox *>(
                     "cognition.open_wake.default_interval_seconds")->value(), 7200);
        QCOMPARE(root->findChild<QSpinBox *>(
                     "cognition.open_wake.max_interval_seconds")->value(), 7200);
        QCOMPARE(root->findChild<QSpinBox *>("cognition.snapshot_budget_bytes")->value(), 8192);
    }

    void settingsDiffPlanOnlyMarksAffectedModules()
    {
        const QJsonObject base{
            {QStringLiteral("setting"), QJsonObject{
                 {QStringLiteral("api"), QJsonObject{{QStringLiteral("deepseek_api_key"), QStringLiteral("k")}}},
                 {QStringLiteral("tts"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("api")}}},
                 {QStringLiteral("logging"), QJsonObject{{QStringLiteral("directory"), QString()}}},
                 {QStringLiteral("qq"), QJsonObject{{QStringLiteral("enabled"), true},
                                                    {QStringLiteral("ws_url"), QStringLiteral("ws://a")},
                                                    {QStringLiteral("reply"), QJsonObject{{QStringLiteral("private"), QStringLiteral("master")}}}}},
                 {QStringLiteral("memory"), QJsonObject{
                     {QStringLiteral("retrieval"),
                      QJsonObject{{QStringLiteral("candidate_limit"), 40},
                                  {QStringLiteral("embedding_model"), QString()}}},
                     {QStringLiteral("owner_cross_channel_private"), false}}}}},
            {QStringLiteral("identity"), QJsonObject{
                 {QStringLiteral("proactive"), QJsonObject{{QStringLiteral("daily_limit"), 288}}},
                 {QStringLiteral("appearance"), QJsonObject{{QStringLiteral("follow_external_mood"), true}}},
                 {QStringLiteral("cognition"), QJsonObject{{QStringLiteral("snapshot_budget_bytes"), 4096}}}}}};
        const auto unchanged = base;
        QVERIFY(!MakoSettings::plan(unchanged, base).anyModuleUpdate());
        auto changed = base;
        auto identity = base.value(QStringLiteral("identity")).toObject();
        identity.insert(QStringLiteral("cognition"), QJsonObject{
            {QStringLiteral("snapshot_budget_bytes"), 4096},
            {QStringLiteral("context_actual_enabled"), true}});
        changed.insert(QStringLiteral("identity"), identity);
        QVERIFY(!MakoSettings::plan(base, changed).anyModuleUpdate());
        changed = base;
        auto setting = changed.value(QStringLiteral("setting")).toObject();
        setting.insert(QStringLiteral("api"), QJsonObject{{QStringLiteral("deepseek_api_key"), QStringLiteral("new")}});
        changed.insert(QStringLiteral("setting"), setting);
        auto plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.deepseekKey && !plan.ttsReload && !plan.indexReconfigure && !plan.qqConfig);
        changed = base;
        setting = changed.value(QStringLiteral("setting")).toObject();
        setting.insert(QStringLiteral("tts"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("mock")}});
        changed.insert(QStringLiteral("setting"), setting);
        plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.ttsReload && !plan.deepseekKey && !plan.indexReconfigure);
        changed = base;
        setting = changed.value(QStringLiteral("setting")).toObject();
        setting.insert(QStringLiteral("logging"), QJsonObject{{QStringLiteral("directory"), QStringLiteral("logs2")}});
        changed.insert(QStringLiteral("setting"), setting);
        plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.logging && !plan.deepseekKey && !plan.permissions);
        // 普通检索参数不打断索引；向量模型变化才 reconfigure。
        changed = base;
        setting = changed.value(QStringLiteral("setting")).toObject();
        auto memory = setting.value(QStringLiteral("memory")).toObject();
        memory.insert(QStringLiteral("retrieval"), QJsonObject{
            {QStringLiteral("candidate_limit"), 55},
            {QStringLiteral("embedding_model"), QString()}});
        setting.insert(QStringLiteral("memory"), memory);
        changed.insert(QStringLiteral("setting"), setting);
        plan = MakoSettings::plan(base, changed);
        QVERIFY(!plan.indexReconfigure);
        changed = base;
        setting = changed.value(QStringLiteral("setting")).toObject();
        memory = setting.value(QStringLiteral("memory")).toObject();
        memory.insert(QStringLiteral("retrieval"), QJsonObject{
            {QStringLiteral("candidate_limit"), 40},
            {QStringLiteral("embedding_model"), QStringLiteral("text-embedding-v4")}});
        setting.insert(QStringLiteral("memory"), memory);
        changed.insert(QStringLiteral("setting"), setting);
        plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.indexReconfigure);
        // QQ 普通回应权限变化只更新配置，不重连。
        changed = base;
        setting = changed.value(QStringLiteral("setting")).toObject();
        auto qq = setting.value(QStringLiteral("qq")).toObject();
        qq.insert(QStringLiteral("reply"), QJsonObject{{QStringLiteral("private"), QStringLiteral("all")}});
        setting.insert(QStringLiteral("qq"), qq);
        changed.insert(QStringLiteral("setting"), setting);
        plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.qqConfig && !plan.qqReconnect);
        // 连接地址变化才重连。
        setting = changed.value(QStringLiteral("setting")).toObject();
        qq = setting.value(QStringLiteral("qq")).toObject();
        qq.insert(QStringLiteral("ws_url"), QStringLiteral("ws://b"));
        setting.insert(QStringLiteral("qq"), qq);
        changed.insert(QStringLiteral("setting"), setting);
        plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.qqConfig && plan.qqReconnect);
        // 外观只影响情绪跟随；权限变化才走撤回。
        changed = base;
        changed["identity"] = QJsonObject{{QStringLiteral("proactive"),
            QJsonObject{{QStringLiteral("daily_limit"), 288}}},
            {QStringLiteral("appearance"), QJsonObject{{QStringLiteral("follow_external_mood"), false}}},
            {QStringLiteral("cognition"), QJsonObject{{QStringLiteral("snapshot_budget_bytes"), 4096}}}};
        plan = MakoSettings::plan(base, changed);
        QVERIFY(plan.followExternalMood && !plan.permissions);
        changed = base;
        changed["identity"] = QJsonObject{{QStringLiteral("proactive"),
            QJsonObject{{QStringLiteral("daily_limit"), 288},
                        {QStringLiteral("contact_daily_limit"), 0}}},
            {QStringLiteral("appearance"), QJsonObject{{QStringLiteral("follow_external_mood"), true}}},
            {QStringLiteral("cognition"), QJsonObject{{QStringLiteral("snapshot_budget_bytes"), 4096}}}};
        plan = MakoSettings::plan(base, changed);
        QVERIFY(!plan.permissions); // 未涉及权限字段
        // 跨渠道权限位于 setting.json：单独修改必须触发权限撤回/差异更新。
        auto withCrossChannel = [&base](bool enabled) {
            QJsonObject snapshot = base;
            auto setting = snapshot.value(QStringLiteral("setting")).toObject();
            auto memory = setting.value(QStringLiteral("memory")).toObject();
            memory.insert(QStringLiteral("owner_cross_channel_private"), enabled);
            setting.insert(QStringLiteral("memory"), memory);
            snapshot.insert(QStringLiteral("setting"), setting);
            return snapshot;
        };
        plan = MakoSettings::plan(withCrossChannel(false), withCrossChannel(true));
        QVERIFY2(plan.permissions, "开启跨渠道权限要走权限差异");
        plan = MakoSettings::plan(withCrossChannel(true), withCrossChannel(false));
        QVERIFY2(plan.permissions, "关闭跨渠道权限必须走权限撤回");
    }

    void noChangeSaveDoesNotApplyOrRewrite()
    {
        auto &config = ConfigManager::instance();
        QVERIFY(config.writeConfigDocument(QStringLiteral("setting.json"),
            R"({"memory":{"short_term_length":15}})", nullptr));
        QFile before(config.getConfigFilePath());
        QVERIFY(before.open(QIODevice::ReadOnly));
        const auto beforeBytes = before.readAll();
        before.close();
        int applied = 0;
        QWidget host;
        auto *root = buildSettingsPages(new QLabel("History"), [&] { ++applied; }, &host);
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        search->setText(QStringLiteral("记忆与检索"));
        auto *stack = root->findChild<QStackedWidget *>();
        auto *apply = stack->currentWidget()->findChild<QPushButton *>("applySettings");
        QVERIFY(apply);
        apply->click();
        QCOMPARE(applied, 0);
        QFile after(config.getConfigFilePath());
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), beforeBytes);
        after.close();
    }

    void cognitionPageLoadsSavesAndPreservesUnknownFields()
    {
        auto &config = ConfigManager::instance();
        const QByteArray identity =
            R"({"identity":{"self":{"qq":[10001]},"master":{"qq":[20001]}},"memory":{"long_term_memory_enabled":true},"proactive":{"daily_limit":288,"contact_daily_limit":0,"cooldown_seconds":60,"idle_seconds":120},"cognition":{"open_wake":{"enabled":false,"allow_contact":false,"initial_delay_seconds":900,"default_interval_seconds":1800,"min_interval_seconds":300,"max_interval_seconds":86400,"max_per_day":12,"shared_budget_percent":30,"max_model_steps_per_wake":3,"max_non_open_jobs_when_due":3},"snapshot_budget_bytes":4096,"review_backoff_seconds":[120,300,900],"max_review_failures":4,"context_actual_enabled":false,"future_extension":{"keep":true}}})";
        QVERIFY(config.writeConfigDocument("identity.json", identity, nullptr));
        QCOMPARE(config.getOpenWakeConfig().maxPerDay, 12);
        QCOMPARE(config.getSnapshotBudgetBytes(), 4096);
        QVERIFY(!config.contextActualEnabled());
        int applied = 0;
        QWidget host;
        auto *history = new QLabel("History");
        auto *root = buildSettingsPages(history, [&] { ++applied; }, &host);
        root->resize(900, 700);
        host.resize(900, 700);
        host.show();
        root->show();
        QCoreApplication::processEvents();
        auto *search = root->findChild<QLineEdit *>("settingsSearch");
        auto *navigation = root->findChild<QListWidget *>();
        auto *stack = root->findChild<QStackedWidget *>();
        QVERIFY(search && navigation && stack);
        search->setText(QStringLiteral("认知与自主行为"));
        QCOMPARE(navigation->currentItem()->text(), QStringLiteral("认知与自主行为"));
        search->clear(); // 回到完整页面：高级设置仍默认收起
        QCoreApplication::processEvents();
        auto *enabled = root->findChild<QCheckBox *>("cognition.open_wake.enabled");
        auto *allow = root->findChild<QCheckBox *>("cognition.open_wake.allow_contact");
        auto *actual = root->findChild<QCheckBox *>("cognition.context_actual_enabled");
        auto *budget = root->findChild<QSpinBox *>("cognition.snapshot_budget_bytes");
        auto *wakeLimit = root->findChild<QSpinBox *>("cognition.open_wake.max_per_day");
        auto *reviewLimit = root->findChild<QSpinBox *>("proactive.daily_limit");
        auto *contactLimit = root->findChild<QSpinBox *>("proactive.contact_daily_limit");
        auto *status = root->findChild<QLabel *>("cognition.context_actual_enabled.status");
        QVERIFY(enabled && allow && actual && budget && wakeLimit && reviewLimit && contactLimit && status);
        QVERIFY(!enabled->isChecked());
        QVERIFY(!actual->isChecked());
        QCOMPARE(budget->value(), 4096);
        QCOMPARE(wakeLimit->value(), 12);
        QCOMPARE(reviewLimit->value(), 288);
        QCOMPARE(contactLimit->value(), 0);
        QVERIFY(status->text().contains(QStringLiteral("配置")));
        QVERIFY(status->text().contains(QStringLiteral("关")));
        // 调度/退避参数在高级设置里，默认收起。
        auto *advancedToggle = root->findChild<QCheckBox *>("showAdvancedSettings");
        QVERIFY(advancedToggle && !advancedToggle->isChecked());
        auto *initialDelay =
            root->findChild<QWidget *>("cognition.open_wake.initial_delay_seconds");
        QVERIFY(initialDelay && !initialDelay->isVisible());
        advancedToggle->setChecked(true);
        QCoreApplication::processEvents();
        QVERIFY(initialDelay->isVisible());
        auto *backoff = root->findChild<QLineEdit *>("cognition.review_backoff_seconds");
        QVERIFY(backoff);
        QCOMPARE(backoff->text(), QStringLiteral("120, 300, 900"));
        // 修改常用参数 + 高级参数 + actual 开关。
        enabled->setChecked(true);
        allow->setChecked(true);
        actual->setChecked(true);
        budget->setValue(8192);
        wakeLimit->setValue(5);
        static_cast<QSpinBox *>(root->findChild<QWidget *>(
            "cognition.open_wake.shared_budget_percent"))->setValue(40);
        static_cast<QSpinBox *>(root->findChild<QWidget *>(
            "cognition.open_wake.initial_delay_seconds"))->setValue(600);
        static_cast<QSpinBox *>(root->findChild<QWidget *>(
            "cognition.open_wake.default_interval_seconds"))->setValue(1200);
        QVERIFY(!root->findChild<QWidget *>("cognition.open_wake.max_model_steps_per_wake"));
        static_cast<QSpinBox *>(root->findChild<QWidget *>(
            "cognition.open_wake.max_non_open_jobs_when_due"))->setValue(2);
        backoff->setText(QStringLiteral("60, 180, 600"));
        static_cast<QSpinBox *>(root->findChild<QWidget *>(
            "cognition.max_review_failures"))->setValue(5);
        stack->currentWidget()->findChild<QPushButton *>("applySettings")->click();
        QCOMPARE(applied, 1);
        // 重新读取文件：展示字段保存、未展示字段仍在。
        QFile identityFile(config.getConfigDirPath() + "/identity.json");
        QVERIFY(identityFile.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(identityFile.readAll()).object();
        identityFile.close();
        const auto savedOpenWake = saved.value("cognition").toObject()
                                       .value("open_wake").toObject();
        QCOMPARE(savedOpenWake.value("enabled").toBool(), true);
        QCOMPARE(savedOpenWake.value("allow_contact").toBool(), true);
        QCOMPARE(savedOpenWake.value("initial_delay_seconds").toInt(), 600);
        QCOMPARE(savedOpenWake.value("default_interval_seconds").toInt(), 1200);
        QCOMPARE(savedOpenWake.value("max_per_day").toInt(), 5);
        QCOMPARE(savedOpenWake.value("shared_budget_percent").toInt(), 40);
        QCOMPARE(savedOpenWake.value("max_model_steps_per_wake").toInt(), 3);
        auto *cap = root->findChild<QLabel *>("openWakeEffectiveDailyCap");
        QVERIFY(cap && cap->text().contains(QStringLiteral("5 次")));
        QCOMPARE(savedOpenWake.value("max_non_open_jobs_when_due").toInt(), 2);
        const auto savedCognition = saved.value("cognition").toObject();
        QCOMPARE(savedCognition.value("snapshot_budget_bytes").toInt(), 8192);
        QCOMPARE(savedCognition.value("context_actual_enabled").toBool(), true);
        QCOMPARE(savedCognition.value("max_review_failures").toInt(), 5);
        QCOMPARE(savedCognition.value("review_backoff_seconds").toArray(),
                 (QJsonArray{60, 180, 600}));
        QCOMPARE(savedCognition.value("future_extension").toObject()
                     .value("keep").toBool(), true);
        const auto savedProactive = saved.value("proactive").toObject();
        QCOMPARE(savedProactive.value("daily_limit").toInt(), 288);
        QCOMPARE(savedProactive.value("contact_daily_limit").toInt(), 0);
        QCOMPARE(savedProactive.value("cooldown_seconds").toInt(), 60);
        QCOMPARE(savedProactive.value("idle_seconds").toInt(), 120);
        QVERIFY(saved.value("identity").toObject().value("master").toObject()
                    .value("qq").toArray().contains(20001));
        const auto openWake = config.getOpenWakeConfig();
        QVERIFY(openWake.enabled);
        QVERIFY(openWake.allowContact);
        QCOMPARE(openWake.initialDelaySeconds, 600);
        QCOMPARE(openWake.defaultIntervalSeconds, 1200);
        QCOMPARE(openWake.maxPerDay, 5);
        QCOMPARE(openWake.sharedBudgetPercent, 40);
        QCOMPARE(config.getSnapshotBudgetBytes(), 8192);
        QVERIFY(config.contextActualEnabled());
        QCOMPARE(config.getProactiveConfig().reviewBackoffSeconds,
                 (QList<int>{60, 180, 600}));
        QCOMPARE(config.getProactiveConfig().maxReviewFailures, 5);
        // 重新打开界面，数值一致。
        auto *root2 = buildSettingsPages(new QLabel("History"), [] {}, &host);
        auto *search2 = root2->findChild<QLineEdit *>("settingsSearch");
        search2->setText(QStringLiteral("认知与自主行为"));
        QCOMPARE(root2->findChild<QCheckBox *>("cognition.open_wake.enabled")->isChecked(), true);
        QCOMPARE(root2->findChild<QCheckBox *>("cognition.open_wake.allow_contact")->isChecked(), true);
        QCOMPARE(root2->findChild<QCheckBox *>("cognition.context_actual_enabled")->isChecked(), true);
        QCOMPARE(root2->findChild<QSpinBox *>("cognition.open_wake.max_per_day")->value(), 5);
        QCOMPARE(root2->findChild<QSpinBox *>("cognition.snapshot_budget_bytes")->value(), 8192);
        root2->findChild<QCheckBox *>("showAdvancedSettings")->setChecked(true);
        QCOMPARE(root2->findChild<QLineEdit *>("cognition.review_backoff_seconds")->text(),
                 QStringLiteral("60, 180, 600"));
        // 环境变量显式设置时优先于配置（含 "0"）；界面区分配置值与实际运行状态。
        qputenv("MAKO_COGNITIVE_CONTEXT_ACTUAL", "0");
        auto *root3 = buildSettingsPages(new QLabel("History"), [] {}, &host);
        auto *search3 = root3->findChild<QLineEdit *>("settingsSearch");
        search3->setText(QStringLiteral("认知与自主行为"));
        auto *status3 = root3->findChild<QLabel *>("cognition.context_actual_enabled.status");
        QVERIFY(status3);
        QVERIFY(status3->text().contains(QStringLiteral("环境变量")));
        QVERIFY(status3->text().contains(QStringLiteral("关")));
        QCOMPARE(root3->findChild<QCheckBox *>("cognition.context_actual_enabled")->isChecked(), true);
        qunsetenv("MAKO_COGNITIVE_CONTEXT_ACTUAL");
    }
};

QTEST_MAIN(TestSettingsPages)
#include "tst_settingspages.moc"
