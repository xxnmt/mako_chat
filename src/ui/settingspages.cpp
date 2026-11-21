#include "settingspages.h"
#include "configmanager.h"
#include "ttsreferencemanager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimeEdit>
#include <QToolButton>
#include <algorithm>
#include <memory>

namespace {
struct Field {
    QStringList path;
    QString label;
    QJsonValue fallback;
    QString type;
};
struct Binding {
    Field field;
    QWidget *row;
    QLabel *label;
    std::function<QJsonValue()> value;
    QJsonValue initial;
};

QString settingHelp(const Field &field)
{
    static const QHash<QString, QString> explanations{
        {"memory.retrieval.index_enabled", "维护本地片段索引，供词面检索和向量召回使用。"},
        {"memory.retrieval.auto_backfill", "启用后，在启动或应用配置时从已有记录回填空的索引。"},
        {"memory.retrieval.candidate_limit", "进入后续排序的候选数量上限，调高可能增加检索耗时。"},
        {"memory.retrieval.rerank_enabled", "开启：将候选正文和查询交给配置的重排服务重新排序。关闭：保留本地检索排序，跳过重排服务。"},
        {"memory.retrieval.rerank_limit", "限制送入重排环节的候选数量。值越高，服务能比较的候选越多，调用耗时和输入量也可能增加。关闭重排时该项不生效。"},
        {"memory.retrieval.embedding_enabled", "使用向量查找语义相近的文本，启用后后台会补齐缺失向量。"},
        {"memory.retrieval.embedding_provider", "未指定：尚未选定向量服务。阿里云百炼：调用已配置端点和凭据计算语义向量。本地测试：用哈希向量验证索引流程，无法替代真实语义检索。"},
        {"memory.retrieval.embedding_model", "留空时，百炼使用 text-embedding-v4。"},
        {"memory.retrieval.embedding_dimension", "向量长度须与服务模型及索引一致。0 使用提供方默认值。"},
        {"memory.retrieval.rerank_model", "重排服务接受的模型名称。百炼默认使用 qwen3-rerank，使用自定义服务时填写该服务支持的模型名称。"},
        {"memory.retrieval.embedding_api_key_env", "未配置百炼密钥时，用此名称读取环境变量。填写的是变量名。"},
        {"memory.retrieval.embedding_endpoint", "可填写完整向量接口地址。留空时从百炼服务地址推导。"},
        {"memory.retrieval.rerank_endpoint", "可填写完整重排接口地址。留空时从百炼服务地址推导。"},
        {"memory.agent_mode", "模型工具：对话模型决定何时调用记忆工具。独立规划：先单独规划检索，再把找到的材料交给对话模型。"},
        {"memory.agent_protocol", "原生调用使用模型的工具接口，JSON 信封通过结构化正文表达工具调用。"},
        {"memory.agent_trigger", "回忆类问题：独立规划模式按回忆意图决定是否启动。每轮：独立规划模式在每轮都尝试规划。模型工具模式由模型按需发起。"},
        {"memory.agent_plan_v2", "开启后，独立规划使用按查找目标组织的新版计划。关闭后使用旧版检索规划协议。"},
        {"memory.agent_max_rounds", "单轮对话允许的记忆补查轮数上限。前台仍受共享补查预算和重复调用限制。"},
        {"memory.agent_timeout_ms", "独立规划模式的等待预算，单位是毫秒。到时返回已取得的资料。"},
        {"memory.agent_max_results", "记忆补查返回的结果数量限制。详细内容可按工具支持的方式进一步核对。"},
        {"memory.owner_cross_channel_private", "允许在主人私密会话中使用其他私密渠道的个人记忆。"},
        {"memory.privacy_rules", "添加规则名称、匹配方式、匹配内容及处理方式。匹配支持关键字、原文和正则。处理支持脱敏或拦截。"},
        {"memory.blind_zones", "逐行填写渠道和会话标识。桌宠会话使用 default，QQ 使用目标账号或群号。"},
        {"qq.reply.master_any_group", "允许主人在回复群列表之外触发回应。实际触发仍按渠道回复策略判断。"},
        {"qq.reply.group", "关闭：不回群消息。仅被 @ 时：消息必须包含指向茉子的 QQ @ 消息段。全部：无需 @，仍检查群及发送者权限。"},
        {"qq.reply.private", "关闭：不回私聊。仅主人：只回复已绑定主人账号。允许列表：回复主人及表中勾选允许回复的账号。全部：可回复所有账号，是否记录按记录权限判断。"},
        {"qq.reply.restrict_empty_group_list", "开启：群回复表没有勾选项时，按空白名单处理。关闭：空名单沿用原有兼容行为，所有群可进入后续回复策略。主人例外、群触发方式和发送者权限仍分别检查。"},
        {"qq.group_permissions", "允许记录：该群可参与长期群聊记忆，仍需启用群聊记忆，并遵守下方的群记忆模式。允许回复：该群进入回复名单，仍受 @ 方式和发送者权限约束。短期回复历史由短期记忆开关单独控制。"},
        {"qq.private_permissions", "允许记录：该账号可进入记忆，仍受长短期记忆开关控制。允许回复：在私聊回应范围为允许列表时纳入回复名单。主人和全部模式有各自的全局规则。"},
        {"qq.group_memory_mode", "严格白名单 strict_whitelist：只记录主人和该群发送者白名单。群授权 group_authorized：开启群聊记忆且群允许记录时，可记录普通成员；回复仍检查发送者权限。"},
        {"qq.group_sender_allowlist", "按群填写允许记录的发送者账号。多个账号用空格或逗号分隔。"},
        {"qq.membership_max_age_minutes", "判断已缓存群成员信息是否过期的时间。"},
        {"qq.membership_refresh_minutes", "刷新群成员缓存的间隔。0 关闭定时刷新。"},
        {"qq.group_retention_days", "配置保留期限，单位为天。当前只保存此值，自动到期清理尚未接入。"},
        {"memory.interaction_gap_seconds", "相邻消息间隔超过此值时，可切为新的互动片段。显式引用关系仍参与分段。"},
        {"memory.interaction_max_events", "一段互动最多容纳的消息数量。达到上限后开始下一段。"},
        {"memory.interaction_max_minutes", "一段互动从首条消息开始计算的最长分钟数。达到上限后开始下一段。"},
        {"tts.mode", "模拟播放：按文字长度模拟节奏，不合成音频。GPT-SoVITS：调用语音服务生成音频，失败时仍播放文字。"},
        {"tts.reference_audio_dir", "默认使用 app_data/reference_audio。其他文件夹需包含 tts_map.json 和对应的音频。音频、台词和语言随情绪从映射中选择，缺失时使用 happyIdle。"},
        {"proactive.private_writing", "允许空闲时回顾最近的主人私下交流，留下自己的想法或休息，也可以继续到期的文字活动。两者共用每日上限和回顾间隔。主动联系另行设置。"},
        {"proactive.targets", "填写可主动联系的渠道及会话标识，桌宠会话填 default。"},
        {"proactive.idle_seconds", "从最近一次会话活动开始计时。达到该秒数后才允许进行后台创作回顾或尝试主动联系。0 表示没有额外空闲等待。"},
        {"proactive.daily_limit", "每天最多领取多少次后台创作回顾任务。0 停止领取。这个上限按回顾任务计数。"},
        {"proactive.cooldown_seconds", "两次后台创作回顾之间的间隔，单位为秒。运行时至少等待 60 秒。"},
        {"proactive.schedule", "可填写 HH:mm 及 fixed（固定）或 random（随机）。当前只保存，定时调度尚未接入。"},
        {"logging.directory", "默认保存到程序目录的 app_data/logs。更换位置后，新日志从保存设置时起写到新目录。"},
        {"logging.separate_runs", "开启后每次运行生成独立文件。关闭后按本地日期追加，跨午夜自动换文件。"},
        {"memory.debug_full_logs", "开启后记录对话正文和模型请求，用于诊断。"},
    };
    const auto path = field.path.join('.');
    QString help = explanations.value(path);
    if (help.isEmpty() && path.startsWith("memory.recent_group_context."))
        help = QStringLiteral("控制群聊近期上下文可以纳入模型的范围或数量。");
    if (help.isEmpty()) return {};
    if (field.type.startsWith("num:")) {
        const auto bounds = field.type.split(':');
        help += QStringLiteral("\n\n可配置范围 %1 至 %2").arg(bounds[1], bounds[2]);
    } else if (field.type.startsWith("choice:")) {
        QStringList choices;
        for (const auto &entry : field.type.mid(7).split('|'))
            choices.append(entry.section('=', -1));
        help += QStringLiteral("\n\n可选值 %1").arg(choices.join(QStringLiteral("、")));
    } else if (field.type == "bool") {
        help += QStringLiteral("\n\n可选值 开启、关闭");
    }
    return help;
}

QJsonValue readPath(const QJsonObject &object, const QStringList &path)
{
    QJsonValue value(object);
    for (const auto &part : path) value = value.toObject().value(part);
    return value;
}

void setPath(QJsonObject &object, const QStringList &path, const QJsonValue &value, qsizetype offset = 0)
{
    const auto &key = path.at(offset);
    if (offset + 1 == path.size()) {
        object.insert(key, value);
    } else {
        auto child = object.value(key).toObject();
        setPath(child, path, value, offset + 1);
        object.insert(key, child);
    }
}

bool readFile(const QString &name, QByteArray &contents)
{
    QFile file(QDir(ConfigManager::instance().getConfigDirPath()).filePath(name));
    contents.clear();
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) return false;
    contents = file.readAll();
    return file.error() == QFileDevice::NoError;
}

QString idsText(const QJsonArray &list)
{
    QStringList ids;
    for (const auto &id : list) ids.append(id.toVariant().toString());
    return ids.join(QStringLiteral("、"));
}

QJsonArray idList(const QString &text)
{
    QJsonArray ids;
    const auto parts = text.split(QRegularExpression(QStringLiteral("[\\s,，、]+")), Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        bool valid = false;
        const auto id = part.toLongLong(&valid);
        if (!valid || id <= 0) throw QStringLiteral("账号或群号必须为正整数");
        ids.append(QString::number(id));
    }
    return ids;
}

bool isPermissionTable(const Field &field)
{
    return field.type == "group_permissions" || field.type == "private_permissions";
}

QStringList permissionPath(const Field &field, bool record)
{
    if (field.type == "group_permissions")
        return record ? QStringList{"qq", "group_memory_allow"} : QStringList{"qq", "reply", "group_allow"};
    return {"qq", "reply", record ? "private_memory_allow" : "private_allow"};
}

QJsonObject permissionLists(const QJsonObject &root, const Field &field)
{
    const auto replies = readPath(root, permissionPath(field, false)).toArray();
    auto records = readPath(root, permissionPath(field, true));
    if (field.type == "private_permissions" && !records.isArray()) records = replies;
    return {{"record", records.toArray()}, {"reply", replies}};
}

Binding permissionBinding(const Field &field, const QJsonObject &lists, QLabel *label, QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *table = new QTableWidget(0, 3, container);
    table->setHorizontalHeaderLabels({
        field.type == "group_permissions" ? QStringLiteral("群号") : QStringLiteral("QQ 账号"),
        QStringLiteral("允许记录"), QStringLiteral("允许回复")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(145);
    table->setMaximumHeight(220);
    layout->addWidget(table);
    auto append = [table](const QString &id, bool record, bool reply) {
        const auto row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(id));
        for (int column : {1, 2}) {
            auto *item = new QTableWidgetItem;
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState((column == 1 ? record : reply) ? Qt::Checked : Qt::Unchecked);
            table->setItem(row, column, item);
        }
    };
    QMap<qlonglong, QPair<bool, bool>> permissions;
    for (const auto &id : lists["record"].toArray()) permissions[id.toVariant().toLongLong()].first = true;
    for (const auto &id : lists["reply"].toArray()) permissions[id.toVariant().toLongLong()].second = true;
    for (auto it = permissions.cbegin(); it != permissions.cend(); ++it)
        append(QString::number(it.key()), it.value().first, it.value().second);
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("添加"), container);
    auto *remove = new QPushButton(QStringLiteral("移除所选"), container);
    actions->addWidget(add);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addLayout(actions);
    QObject::connect(add, &QPushButton::clicked, container, [table, append] {
        append({}, false, false);
        auto *item = table->item(table->rowCount() - 1, 0);
        table->setCurrentItem(item);
        table->editItem(item);
    });
    QObject::connect(remove, &QPushButton::clicked, container, [table] {
        auto rows = table->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.row() > b.row(); });
        for (const auto &row : rows) table->removeRow(row.row());
    });
    return {field, container, label, [table] {
        QJsonArray records, replies;
        QSet<QString> seen;
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto ids = idList(table->item(row, 0)->text());
            if (ids.size() != 1 || seen.contains(ids.first().toString()))
                throw QStringLiteral("每行须填写一个唯一的号码");
            const auto id = ids.first().toString();
            seen.insert(id);
            if (table->item(row, 1)->checkState() == Qt::Checked) records.append(id);
            if (table->item(row, 2)->checkState() == Qt::Checked) replies.append(id);
        }
        return QJsonValue(QJsonObject{{"record", records}, {"reply", replies}});
    }, {}};
}

struct TableColumn { QString key; QString label; QStringList choices; };
Binding tableBinding(const Field &field, const QJsonValue &value, QLabel *label, QWidget *parent)
{
    const bool modes = field.type == "modes";
    const bool senders = field.type == "senders";
    const bool map = modes || senders;
    QList<TableColumn> columns;
    if (modes) columns = {{"group","群号",{}},{"value","记忆模式",{"strict_whitelist","group_authorized"}}};
    else if (senders) columns = {{"group","群号",{}},{"value","允许的账号",{}}};
    else if (field.type == "rules")
        columns = {{"id","规则名称",{}},{"type","匹配方式",{"keyword","exact","pattern"}},
                   {"value","匹配内容",{}},{"action","处理方式",{"redact","block"}}};
    else if (field.type == "schedule")
        columns = {{"at","时间（HH:mm）",{}},{"type","方式",{"fixed","random"}}};
    else columns = {{"channel","渠道",{"pet","qq_private","qq_group"}},{"session_id","会话",{}}};

    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *table = new QTableWidget(0, columns.size(), container);
    QStringList headers;
    for (const auto &column : columns) headers.append(column.label);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(125);
    table->setMaximumHeight(180);
    layout->addWidget(table);
    // Keep unedited extension data on existing rows.
    auto originals = std::make_shared<QList<QJsonObject>>();
    auto append = [table, columns, originals](const QJsonObject &object) {
        const int row = table->rowCount();
        table->insertRow(row);
        originals->append(object);
        for (int col = 0; col < columns.size(); ++col) {
            const auto &column = columns[col];
            const auto text = object.value(column.key).toVariant().toString();
            if (column.choices.isEmpty()) {
                table->setItem(row, col, new QTableWidgetItem(text));
            } else {
                auto *choice = new QComboBox(table);
                choice->addItems(column.choices);
                if (!text.isEmpty() && choice->findText(text) < 0) choice->addItem(text);
                if (!text.isEmpty()) choice->setCurrentText(text);
                table->setCellWidget(row, col, choice);
            }
        }
    };
    if (map) {
        const auto entries = value.toObject();
        for (auto i = entries.begin(); i != entries.end(); ++i)
            append({{"group",i.key()},{"value",senders ? idsText(i.value().toArray()) : i.value().toString()}});
    } else {
        for (const auto &item : value.toArray()) append(item.toObject());
    }
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("添加"), container);
    auto *remove = new QPushButton(QStringLiteral("移除所选"), container);
    actions->addWidget(add);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addLayout(actions);
    QObject::connect(add, &QPushButton::clicked, container, [append, table] {
        append({});
        table->setCurrentCell(table->rowCount() - 1, 0);
        if (auto *item = table->currentItem()) table->editItem(item);
    });
    QObject::connect(remove, &QPushButton::clicked, container, [table, originals] {
        auto rows = table->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.row() > b.row(); });
        for (const auto &index : rows) {
            originals->removeAt(index.row());
            table->removeRow(index.row());
        }
    });
    return {field, container, label, [=]() -> QJsonValue {
        QJsonObject result;
        QJsonArray list;
        for (int row = 0; row < table->rowCount(); ++row) {
            auto entry = originals->at(row);
            for (int col = 0; col < columns.size(); ++col) {
                auto *choice = qobject_cast<QComboBox *>(table->cellWidget(row, col));
                const auto text = choice ? choice->currentText() : table->item(row, col)->text().trimmed();
                entry.insert(columns[col].key, text);
            }
            if (map) {
                const auto group = entry.value("group").toString();
                if (idList(group).size() != 1 || result.contains(group))
                    throw QStringLiteral("每组必须填写一个唯一的群号");
                const auto text = entry.value("value").toString();
                result.insert(group, senders ? QJsonValue(idList(text)) : QJsonValue(text));
            } else {
                if (field.type == "rules" && entry.value("value").toString().isEmpty())
                    throw QStringLiteral("匹配内容不能为空");
                if (field.type == "rules" && entry.value("type") == "pattern"
                    && !QRegularExpression(entry.value("value").toString()).isValid())
                    throw QStringLiteral("正则表达式无效");
                if (field.type == "schedule" && !QTime::fromString(entry.value("at").toString(), "HH:mm").isValid())
                    throw QStringLiteral("时间应为 HH:mm");
                if ((field.type == "targets" || field.type == "zones") && entry.value("session_id").toString().isEmpty())
                    throw QStringLiteral("会话不能为空（桌宠填 default）");
                list.append(entry);
            }
        }
        return map ? QJsonValue(result) : QJsonValue(list);
    }, {}};
}

Binding buildField(const Field &field, const QJsonValue &value, QWidget *parent)
{
    auto *label = new QLabel(field.label, parent);
    label->setWordWrap(true);
    if (isPermissionTable(field)) return permissionBinding(field, value.toObject(), label, parent);
    if (field.type == "bool") {
        auto *box = new QCheckBox(parent);
        box->setChecked(value.toBool());
        return {field, box, label, [box] { return QJsonValue(box->isChecked()); }, {}};
    }
    if (field.type.startsWith("num:")) {
        const auto limits = field.type.split(':');
        auto *spin = new QSpinBox(parent);
        spin->setRange(limits[1].toInt(), limits[2].toInt());
        spin->setValue(value.toInt());
        return {field, spin, label, [spin] { return QJsonValue(spin->value()); }, {}};
    }
    if (field.type.startsWith("choice:")) {
        auto *choice = new QComboBox(parent);
        for (const auto &entry : field.type.mid(7).split('|')) {
            const auto pair = entry.split('=');
            choice->addItem(pair.last(), pair.first());
        }
        int index = choice->findData(value.toString());
        if (index < 0) {
            choice->addItem(value.toString(), value.toString());
            index = choice->count() - 1;
        }
        choice->setCurrentIndex(index);
        return {field, choice, label, [choice] { return QJsonValue(choice->currentData().toString()); }, {}};
    }
    if (field.type == "time") {
        auto *time = new QTimeEdit(QTime::fromString(value.toString(), "HH:mm"), parent);
        time->setDisplayFormat("HH:mm");
        return {field, time, label, [time] { return QJsonValue(time->time().toString("HH:mm")); }, {}};
    }
    if (field.type == "multiline") {
        auto *editor = new QPlainTextEdit(value.toString(), parent);
        editor->setMinimumHeight(120);
        return {field, editor, label, [editor] { return QJsonValue(editor->toPlainText()); }, {}};
    }
    if (field.type == "rules" || field.type == "schedule" || field.type == "targets"
        || field.type == "zones" || field.type == "modes" || field.type == "senders")
        return tableBinding(field, value, label, parent);

    const bool referenceDirectory = field.type == "reference_directory";
    const bool logDirectory = field.type == "log_directory";
    const bool managedDirectory = referenceDirectory || logDirectory;
    const auto defaultDirectory = logDirectory ? QDir(ConfigManager::instance().getAppDataPath()).filePath("logs") : referenceDirectory
        ? ConfigManager::instance().defaultTTSReferenceDirectory() : QString();
    const auto initialText = logDirectory ? ConfigManager::instance().logDirectory() : referenceDirectory
        ? ConfigManager::instance().ttsReferenceDirectory()
        : field.type == "ids" ? idsText(value.toArray()) : value.toString();
    auto *text = new QLineEdit(initialText, parent);
    if (managedDirectory) {
        text->setReadOnly(true);
        text->setToolTip(initialText);
        QObject::connect(text, &QLineEdit::textChanged, text, &QLineEdit::setToolTip);
    }
    if (field.type == "secret") text->setEchoMode(QLineEdit::Password);
    if (field.type == "ids") text->setPlaceholderText(QStringLiteral("多个号码以空格或逗号分隔"));
    QWidget *row = text;
    if (field.type == "file" || field.type == "directory" || field.type == "secret" || managedDirectory) {
        row = new QWidget(parent);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(text, 1);
        auto *button = new QPushButton(field.type == "secret" ? QStringLiteral("显示") : QStringLiteral("浏览…"), row);
        layout->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, row, [=] {
            if (field.type == "secret") {
                const bool show = text->echoMode() == QLineEdit::Password;
                text->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
                button->setText(show ? QStringLiteral("隐藏") : QStringLiteral("显示"));
                return;
            }
            const auto selected = field.type == "directory" || managedDirectory
                ? QFileDialog::getExistingDirectory(row, field.label, text->text())
                : QFileDialog::getOpenFileName(row, field.label, text->text());
            if (!selected.isEmpty()) text->setText(QDir::toNativeSeparators(selected));
        });
        if (managedDirectory) {
            auto *reset = new QPushButton(QStringLiteral("恢复默认"), row);
            layout->addWidget(reset);
            QObject::connect(reset, &QPushButton::clicked, row, [text, defaultDirectory] {
                text->setText(defaultDirectory);
            });
        }
    }
    return {field, row, label, [text, field, managedDirectory, defaultDirectory]() -> QJsonValue {
        if (managedDirectory) {
            const auto path = QDir::cleanPath(text->text());
            return path == QDir::cleanPath(defaultDirectory) ? QString() : path;
        }
        if (field.type == "ids") return idList(text->text());
        return text->text().trimmed();
    }, {}};
}

struct Group { QString title; QList<Field> fields; };
struct Page { QString title; QString detail; QString file; QList<Group> groups; };
Field field(const QString &path, const QString &label, QJsonValue fallback, const QString &type = {})
{
    return {path.split('.'), label, fallback, type};
}


QList<Page> pages()
{
    return {
        {"模型与服务", "配置对话和语义检索所用的服务。", "setting.json", {
            {"DeepSeek", {
                field("api.deepseek_api_key", "API 密钥", "", "secret"),
            }},
            {"阿里云百炼", {
                field("api.dashscope_api_key", "API 密钥", "", "secret"),
                field("api.dashscope_base_url", "服务地址", "", "text"),
            }},
        }},
        {"语音合成", "配置语音方式、参考音频和本地模型。", "setting.json", {
            {"播放与服务", {
                field("tts.mode", "语音方式", "api", "choice:api=GPT-SoVITS|mock=模拟播放"),
                field("api.gpt_sovits_url", "API 地址", "http://127.0.0.1:9880", "text"),
                field("tts.python_path", "Python 可执行文件", "", "file"),
                field("tts.gpt_sovits_root_path", "GPT-SoVITS 安装目录", "", "directory"),
            }},
            {"参考音频", {
                field("tts.reference_audio_dir", "参考音频文件夹", "", "reference_directory"),
            }},
            {"模型文件", {
                field("tts.gpt_weights_path", "GPT 权重", "", "file"),
                field("tts.sovits_weights_path", "SoVITS 权重", "", "file"),
                field("tts.gpt_model_dir", "GPT 模型目录", "", "directory"),
                field("tts.sovits_model_dir", "SoVITS 模型目录", "", "directory"),
            }},
        }},
        {"记忆与检索", "管理对话上下文、检索与索引。", "setting.json", {
            {"对话记忆", {
                field("memory.short_context_enabled", "使用近期对话", true, "bool"),
                field("memory.short_term_length", "近期对话轮数", 15, "num:0:999"),
                field("memory.long_term_memory_enabled", "使用长期记忆", true, "bool"),
                field("memory.summary_length", "摘要间隔（轮）", 15, "num:1:999"),
            }},
            {"检索和索引", {
                field("memory.retrieval.index_enabled", "启用本地检索索引", true, "bool"),
                field("memory.retrieval.auto_backfill", "自动回填空索引", false, "bool"),
                field("memory.retrieval.candidate_limit", "候选数量上限", 40, "num:10:200"),
                field("memory.retrieval.rerank_enabled", "启用重排", true, "bool"),
                field("memory.retrieval.rerank_limit", "重排数量上限", 10, "num:1:40"),
                field("memory.retrieval.rerank_endpoint", "重排端点", "", "text"),
                field("memory.retrieval.rerank_model", "重排模型", "qwen3-rerank", "text"),
                field("memory.retrieval.embedding_enabled", "启用向量召回", false, "bool"),
                field("memory.retrieval.embedding_provider", "向量服务", "", "choice:=未指定|dashscope=阿里云百炼|local_hash_test=本地测试"),
                field("memory.retrieval.embedding_endpoint", "向量端点", "", "text"),
                field("memory.retrieval.embedding_api_key_env", "向量密钥环境变量", "DASHSCOPE_API_KEY", "text"),
                field("memory.retrieval.embedding_model", "向量模型", "", "text"),
                field("memory.retrieval.embedding_dimension", "向量维度（0 为自动）", 0, "num:0:8192"),
            }},
            {"主动回忆工具", {
                field("memory.agent_enabled", "启用补查工具", true, "bool"),
                field("memory.agent_mode", "回忆方式", "tool", "choice:tool=模型工具|legacy=独立规划"),
                field("memory.agent_protocol", "工具协议", "native", "choice:native=原生调用|json_envelope=JSON 信封"),
                field("memory.agent_plan_v2", "启用第二版检索规划", true, "bool"),
                field("memory.agent_trigger", "规划触发方式", "memory_question", "choice:memory_question=回忆类问题|always=每轮"),
                field("memory.agent_timeout_ms", "工具超时（毫秒）", 6000, "num:1000:15000"),
                field("memory.agent_max_rounds", "最多工具轮数", 2, "num:1:4"),
                field("memory.agent_max_results", "最多返回条数", 8, "num:1:20"),
            }},
        }},
        {"QQ 渠道", "设置 OneBot 连接、回应范围和群聊记忆。", "setting.json", {
            {"连接", {
                field("qq.enabled", "启用 QQ 渠道", false, "bool"),
                field("qq.ws_url", "WebSocket 地址", "ws://127.0.0.1:3001", "text"),
                field("qq.access_token", "访问令牌", "", "secret"),
            }},
            {"回复范围", {
                field("qq.reply.private", "私聊回应范围", "master", "choice:none=关闭|master=仅主人|allowlist=允许列表|all=全部"),
                field("qq.reply.group", "群聊回应方式", "none", "choice:none=关闭|mention=仅被 @ 时|all=全部"),
                field("qq.reply.master_any_group", "主人可在任意群触发", false, "bool"),
                field("qq.reply.restrict_empty_group_list", "空名单也限制群回复", false, "bool"),
                field("qq.group_permissions", "群权限", QJsonObject{}, "group_permissions"),
                field("qq.private_permissions", "私聊账号权限", QJsonObject{}, "private_permissions"),
            }},
            {"群聊记忆", {
                field("qq.group_memory_enabled", "启用群聊记忆", false, "bool"),
                field("qq.group_retention_days", "保留天数（清理暂未接入）", 30, "num:1:3650"),
                field("qq.membership_refresh_minutes", "成员刷新间隔（分钟）", 30, "num:0:1440"),
                field("qq.membership_max_age_minutes", "成员缓存有效期（分钟）", 1440, "num:1:10080"),
                field("qq.group_memory_mode", "各群记忆模式", QJsonObject{}, "modes"),
                field("qq.group_sender_allowlist", "各群发送者允许列表", QJsonObject{}, "senders"),
            }},
            {"群消息整理", {
                field("memory.qq_group_idle_minutes", "空闲后整理（分钟）", 15, "num:1:1440"),
                field("memory.qq_group_batch_size", "开始整理的消息数", 50, "num:1:500"),
                field("memory.interaction_gap_seconds", "分段间隔（秒）", 90, "num:10:3600"),
                field("memory.interaction_max_events", "每段最多消息数", 20, "num:1:100"),
                field("memory.interaction_max_minutes", "每段最长时间（分钟）", 10, "num:1:120"),
            }},
            {"近期群聊上下文", {
                field("memory.recent_group_context.enabled", "使用近期群聊上下文", true, "bool"),
                field("memory.recent_group_context.max_age_minutes", "回看范围（分钟）", 30, "num:1:1440"),
                field("memory.recent_group_context.candidate_limit_per_group", "每群候选上限", 50, "num:1:500"),
                field("memory.recent_group_context.max_owner_groups", "最多参考群数", 2, "num:1:20"),
                field("memory.recent_group_context.targeted_item_limit", "定向查找条数", 10, "num:1:100"),
                field("memory.recent_group_context.targeted_char_budget", "定向查找字数", 1200, "num:100:8000"),
                field("memory.recent_group_context.overview_item_limit", "概览条数", 50, "num:1:500"),
                field("memory.recent_group_context.overview_char_budget", "概览字数", 2200, "num:100:12000"),
                field("memory.recent_group_context.hot_window_items", "近期窗口条数", 10, "num:1:100"),
                field("memory.recent_group_context.warm_line_max_chars", "较旧单条消息字数", 24, "num:8:200"),
            }},
        }},
        {"隐私与日志", "设置跨渠道记忆、内容过滤和诊断日志。", "setting.json", {
            {"记忆权限", {
                field("memory.owner_cross_channel_private", "允许主人跨私聊渠道使用个人记忆", false, "bool"),
                field("memory.privacy_policy_version", "策略版本标识", "r3", "text"),
                field("memory.privacy_rules", "内容过滤规则", QJsonArray{}, "rules"),
            }},
            {"日志", {
                field("logging.directory", "日志文件夹", "", "log_directory"),
                field("logging.separate_runs", "每次启动单独创建日志", true, "bool"),
                field("memory.debug_full_logs", "记录完整对话与模型请求", false, "bool"),
            }},
        }},
        {"身份与盲区", "绑定账户，并指定不进入长期记忆的会话。", "identity.json", {
            {"身份与会话", {
                field("identity.self.qq", "茉子的 QQ 账号", QJsonArray{}, "ids"),
                field("identity.master.qq", "主人的 QQ 账号", QJsonArray{}, "ids"),
                field("memory.blind_zones", "记忆盲区", QJsonArray{}, "zones"),
            }},
            {"桌宠表现", {
                field("appearance.follow_external_mood", "桌宠跟随外部渠道情绪", true, "bool"),
            }},
        }},
        {"主动交流", "设置自主写作、联系目标和安静时段。", "identity.json", {
            {"活动和联系", {
                field("proactive.enabled", "允许主动联系", false, "bool"),
                field("proactive.private_writing", "允许私下回顾与创作", false, "bool"),
                field("proactive.daily_limit", "每日回顾上限", 3, "num:0:999"),
                field("proactive.cooldown_seconds", "回顾间隔（秒）", 900, "num:60:604800"),
                field("proactive.idle_seconds", "空闲等待（秒）", 1800, "num:0:604800"),
                field("proactive.quiet_hours.start", "安静开始", "23:00", "time"),
                field("proactive.quiet_hours.end", "安静结束", "08:00", "time"),
                field("proactive.targets", "允许联系的会话", QJsonArray{}, "targets"),
                field("proactive.schedule", "计划时间（调度暂未接入）", QJsonArray{}, "schedule"),
            }},
        }},
        {"时间与状态", "设置昼夜切换与桌宠状态持续时间。", "setting.json", {
            {"昼夜切换", {
                field("time.night_start", "夜间开始", "22:00", "time"),
                field("time.night_end", "夜间结束", "07:00", "time"),
            }},
            {"状态持续时间", {
                field("time.blush_lasting", "脸红（秒）", 30, "num:1:3600"),
                field("time.emotion_lasting", "表情（秒）", 15, "num:1:3600"),
                field("time.distance_lasting", "距离（秒）", 15, "num:1:3600"),
            }},
        }},
        {"角色设定", "修改茉子的人格与表达设定。", "prompt.txt", {
            {"角色提示词", {
                field("prompt", "角色提示词", "", "multiline"),
            }},
        }},
    };
}
struct PageView {
    QWidget *widget;
    std::shared_ptr<QList<Binding>> bindings;
    QList<QPair<QGroupBox *, QList<int>>> groups;
};

PageView buildPage(const Page &page, const std::function<void()> &applied, QWidget *parent)
{
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(24, 20, 24, 16);
    auto *title = new QLabel(page.title, panel);
    auto font = title->font();
    font.setPointSize(font.pointSize() + 8);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto *description = new QLabel(page.detail, panel);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *scroll = new QScrollArea(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *contents = new QWidget(scroll);
    auto *cards = new QVBoxLayout(contents);
    cards->setContentsMargins(0, 8, 8, 8);
    cards->setSpacing(14);
    scroll->setWidget(contents);
    layout->addWidget(scroll, 1);

    const bool prompt = page.file == "prompt.txt";
    QByteArray loaded;
    const bool read = readFile(page.file, loaded);
    auto snapshot = std::make_shared<QJsonObject>();
    auto promptSnapshot = std::make_shared<QByteArray>(loaded);
    const auto document = QJsonDocument::fromJson(loaded);
    const bool readable = read && (prompt || loaded.isEmpty() || document.isObject());
    if (document.isObject()) *snapshot = document.object();
    const auto effective = page.file == "setting.json"
        ? ConfigManager::instance().effectiveSettings() : QJsonObject{};
    QString promptText = QString::fromUtf8(loaded);
    if (prompt && loaded.isEmpty()) {
        QFile bundled(":/image/default_config/prompt.txt");
        if (bundled.open(QIODevice::ReadOnly)) promptText = QString::fromUtf8(bundled.readAll());
    }
    PageView view{panel, std::make_shared<QList<Binding>>(), {}};

    for (const auto &group : page.groups) {
        auto *card = new QGroupBox(group.title, contents);
        card->setStyleSheet(QStringLiteral(
            "QGroupBox { border: 1px solid palette(mid); border-radius: 8px; margin-top: 10px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }"));
        auto *form = new QFormLayout(card);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setContentsMargins(16, 20, 16, 14);
        form->setVerticalSpacing(12);
        QList<int> indexes;
        for (const auto &field : group.fields) {
            const auto configured = isPermissionTable(field)
                ? QJsonValue(permissionLists(*snapshot, field)) : readPath(*snapshot, field.path);
            auto value = prompt ? QJsonValue(promptText) : configured;
            const auto key = field.path.join('.');
            const bool derivedEndpoint = key == "memory.retrieval.embedding_endpoint"
                || key == "memory.retrieval.rerank_endpoint";
            const auto runtime = readPath(effective, field.path);
            if (!derivedEndpoint && !runtime.isUndefined() && !runtime.isArray() && !runtime.isObject()
                && (configured.isUndefined() || configured.type() != runtime.type()))
                value = runtime;
            if (value.isUndefined()) value = field.fallback;
            auto binding = buildField(field, value, card);
            binding.row->setObjectName(key);
            binding.row->setAccessibleName(field.label);
            binding.label->setBuddy(binding.row);
            if (derivedEndpoint && value.toString().isEmpty() && !runtime.toString().isEmpty()) {
                auto *endpoint = qobject_cast<QLineEdit *>(binding.row);
                endpoint->setPlaceholderText(runtime.toString());
                endpoint->setToolTip(QStringLiteral("当前自动使用 %1").arg(runtime.toString()));
            }
            const auto provider = readPath(effective, {"memory", "retrieval", "embedding_provider"}).toString();
            if (key == "memory.retrieval.embedding_model" && provider == "dashscope")
                qobject_cast<QLineEdit *>(binding.row)->setPlaceholderText(QStringLiteral("自动使用 text-embedding-v4"));
            if (key == "memory.retrieval.embedding_dimension") {
                auto *dimension = qobject_cast<QSpinBox *>(binding.row);
                if (provider == "dashscope") dimension->setSpecialValueText(QStringLiteral("自动（1024）"));
                else if (provider == "local_hash_test") dimension->setSpecialValueText(QStringLiteral("自动（64）"));
            }
            try { binding.initial = binding.value(); }
            catch (const QString &) { binding.initial = value; }
            QWidget *label = binding.label;
            const auto help = settingHelp(field);
            if (!help.isEmpty()) {
                label = new QWidget(card);
                auto *labelLayout = new QHBoxLayout(label);
                labelLayout->setContentsMargins(0, 0, 0, 0);
                labelLayout->addWidget(binding.label);
                auto *button = new QToolButton(label);
                button->setText(QStringLiteral("?"));
                button->setAutoRaise(true);
                button->setAccessibleName(field.label + QStringLiteral("说明"));
                button->setObjectName(key + ".help");
                button->setToolTip(QStringLiteral("查看说明和允许的值"));
                labelLayout->addWidget(button);
                labelLayout->addStretch();
                QObject::connect(button, &QToolButton::clicked, card, [card, title = field.label, help] {
                    QDialog dialog(card);
                    dialog.setWindowTitle(title);
                    dialog.resize(470, 280);
                    auto *layout = new QVBoxLayout(&dialog);
                    auto *body = new QPlainTextEdit(help, &dialog);
                    body->setReadOnly(true);
                    layout->addWidget(body);
                    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
                    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                    layout->addWidget(close);
                    dialog.exec();
                });
            }
            form->addRow(label, binding.row);
            indexes.append(view.bindings->size());
            view.bindings->append(std::move(binding));
        }
        cards->addWidget(card);
        view.groups.append({card, indexes});
    }
    cards->addStretch();
    auto *footer = new QHBoxLayout;
    auto *feedback = new QLabel(panel);
    feedback->setWordWrap(true);
    auto *apply = new QPushButton(QStringLiteral("保存并应用"), panel);
    apply->setObjectName("applySettings");
    apply->setMinimumWidth(115);
    apply->setEnabled(readable);
    if (!readable) feedback->setText(QStringLiteral("配置无法读取，请检查本地文件后重新打开设置。"));
    footer->addWidget(feedback, 1);
    footer->addWidget(apply);
    layout->addLayout(footer);

    QObject::connect(apply, &QPushButton::clicked, panel,
        [=, bindings = view.bindings] {
            QByteArray latest;
            if (!readFile(page.file, latest)) {
                feedback->setText(QStringLiteral("无法读取当前配置。"));
                return;
            }
            const auto fresh = QJsonDocument::fromJson(latest);
            if (!prompt && !latest.isEmpty() && !fresh.isObject()) {
                feedback->setText(QStringLiteral("配置文件格式无效，保存已取消。"));
                return;
            }
            auto merged = fresh.object();
            QByteArray updated;
            bool restrictedAfterRemoval = false;
            QList<QPair<int, QJsonValue>> edits;
            for (int index = 0; index < bindings->size(); ++index) {
                const auto &binding = bindings->at(index);
                QJsonValue value;
                try { value = binding.value(); }
                catch (const QString &err) {
                    feedback->setText(binding.field.label + QStringLiteral("：") + err);
                    return;
                }
                if (value == binding.initial) continue;
                const bool permissions = isPermissionTable(binding.field);
                const bool changedExternally = permissions
                    ? permissionLists(fresh.object(), binding.field) != permissionLists(*snapshot, binding.field)
                    : readPath(fresh.object(), binding.field.path) != readPath(*snapshot, binding.field.path);
                if (prompt ? latest != *promptSnapshot : changedExternally) {
                    feedback->setText(QStringLiteral("此设置已在其他位置修改，请重新打开设置后再编辑。"));
                    return;
                }
                if (prompt) updated = value.toString().toUtf8();
                else if (permissions) {
                    const auto lists = value.toObject();
                    setPath(merged, permissionPath(binding.field, true), lists["record"]);
                    setPath(merged, permissionPath(binding.field, false), lists["reply"]);
                    // A list edited down to zero must not open replies to every group.
                    if (binding.field.type == "group_permissions" && lists["reply"].toArray().isEmpty()) {
                        setPath(merged, {"qq", "reply", "restrict_empty_group_list"}, true);
                        restrictedAfterRemoval = true;
                    }
                } else setPath(merged, binding.field.path, value);
                edits.append({index, value});
            }
            if (edits.isEmpty()) {
                feedback->setText(QStringLiteral("没有需要保存的修改。"));
                return;
            }
            for (const auto &[index, value] : edits) {
                if (bindings->at(index).field.type != "reference_directory" || value.toString().isEmpty())
                    continue;
                QString error;
                if (!TTSReferenceManager::validateDirectory(value.toString(), &error)) {
                    feedback->setText(error);
                    return;
                }
            }
            if (!prompt) updated = QJsonDocument(merged).toJson();
            QString error;
            if (!ConfigManager::instance().writeConfigDocument(page.file, updated, &error)) {
                feedback->setText(error);
                return;
            }
            for (const auto &[index, value] : edits) {
                auto &binding = (*bindings)[index];
                binding.initial = value;
                if (!prompt && isPermissionTable(binding.field)) {
                    setPath(*snapshot, permissionPath(binding.field, true), value.toObject()["record"]);
                    setPath(*snapshot, permissionPath(binding.field, false), value.toObject()["reply"]);
                } else if (!prompt) setPath(*snapshot, binding.field.path, value);
            }
            if (prompt) *promptSnapshot = updated;
            if (restrictedAfterRemoval) {
                const QStringList restriction{"qq", "reply", "restrict_empty_group_list"};
                setPath(*snapshot, restriction, true);
                for (auto &binding : *bindings) {
                    if (binding.field.path != restriction) continue;
                    qobject_cast<QCheckBox *>(binding.row)->setChecked(true);
                    binding.initial = true;
                }
            }
            applied();
            feedback->setText(restrictedAfterRemoval
                ? QStringLiteral("已保存。回复名单为空，已启用空名单限制。")
                : QStringLiteral("已保存并应用。"));
        });
    return view;
}
} // namespace

QWidget *buildSettingsPages(QWidget *history, std::function<void()> applied, QWidget *parent)
{
    auto *root = new QWidget(parent);
    auto *layout = new QHBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *rail = new QWidget(root);
    rail->setObjectName("settingsNavigation");
    rail->setFixedWidth(186);
    rail->setAutoFillBackground(true);
    auto palette = rail->palette();
    palette.setColor(QPalette::Window, palette.color(QPalette::AlternateBase));
    rail->setPalette(palette);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(12, 18, 12, 12);
    auto *heading = new QLabel(QStringLiteral("设置"), rail);
    auto font = heading->font();
    font.setPointSize(font.pointSize() + 4);
    font.setBold(true);
    heading->setFont(font);
    railLayout->addWidget(heading);
    auto *search = new QLineEdit(rail);
    search->setObjectName("settingsSearch");
    search->setPlaceholderText(QStringLiteral("搜索设置"));
    search->setAccessibleName(QStringLiteral("搜索设置"));
    search->setClearButtonEnabled(true);
    railLayout->addWidget(search);
    auto *navigation = new QListWidget(rail);
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setSpacing(2);
    navigation->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; }"
        "QListWidget::item { padding: 7px 9px; border-radius: 4px; }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"));
    railLayout->addWidget(navigation, 1);
    layout->addWidget(rail);

    auto *stack = new QStackedWidget(root);
    layout->addWidget(stack, 1);
    const auto definitions = pages();
    QList<PageView> views;
    for (const auto &page : definitions) {
        navigation->addItem(page.title);
        auto view = buildPage(page, applied, stack);
        stack->addWidget(view.widget);
        views.append(view);
    }
    navigation->addItem(QStringLiteral("历史会话"));
    stack->addWidget(history);
    auto *empty = new QLabel(QStringLiteral("没有找到匹配的设置。"), stack);
    empty->setAlignment(Qt::AlignCenter);
    stack->addWidget(empty);
    QObject::connect(navigation, &QListWidget::currentRowChanged, stack, [stack](int row) {
        if (row >= 0) stack->setCurrentIndex(row);
    });
    QObject::connect(search, &QLineEdit::textChanged, root, [=](const QString &text) {
        const auto query = text.trimmed();
        int first = -1;
        for (int i = 0; i < views.size(); ++i) {
            const auto &view = views[i];
            const bool all = query.isEmpty() || definitions[i].title.contains(query, Qt::CaseInsensitive);
            bool pageMatches = all;
            for (const auto &[card, indexes] : view.groups) {
                const bool groupMatches = all || card->title().contains(query, Qt::CaseInsensitive);
                bool any = false;
                for (const auto index : indexes) {
                    const auto &binding = view.bindings->at(index);
                    const bool show = groupMatches || binding.field.label.contains(query, Qt::CaseInsensitive)
                        || binding.field.path.join('.').contains(query, Qt::CaseInsensitive);
                    static_cast<QFormLayout *>(card->layout())->setRowVisible(binding.row, show);
                    any |= show;
                }
                card->setVisible(any);
                pageMatches |= any;
            }
            navigation->item(i)->setHidden(!pageMatches);
            if (first < 0 && pageMatches) first = i;
        }
        const int historyIndex = views.size();
        const bool historyMatches = QStringLiteral("历史会话").contains(query);
        navigation->item(historyIndex)->setHidden(!historyMatches);
        if (first < 0 && historyMatches) first = historyIndex;
        const int current = navigation->currentRow();
        if (first < 0) {
            navigation->setCurrentRow(-1);
            stack->setCurrentWidget(empty);
        } else if (current < 0 || navigation->item(current)->isHidden()) {
            navigation->setCurrentRow(first);
        }
    });
    navigation->setCurrentRow(0);
    return root;
}
