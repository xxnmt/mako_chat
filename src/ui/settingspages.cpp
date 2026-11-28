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
        {"memory.retrieval.rerank_limit", "限制融合排序后保留的结果数量。开重排时也是请求返回上限，关闭重排仍裁剪结果。候选数量上限另设。"},
        {"memory.retrieval.embedding_enabled", "使用向量查找语义相近的文本，启用后后台会补齐缺失向量。"},
        {"memory.retrieval.embedding_provider", "未指定：尚未选定向量服务。阿里云百炼：调用已配置端点和凭据计算语义向量。本地测试：用哈希向量验证索引流程，无法替代真实语义检索。"},
        {"memory.retrieval.embedding_model", "留空时，百炼使用 text-embedding-v4。"},
        {"memory.retrieval.embedding_dimension", "须与已建索引的向量长度一致。0 表示按提供方自动使用 DashScope 1024 维或本地测试 64 维。切换后旧向量不会自动重建。"},
        {"memory.retrieval.rerank_model", "重排服务接受的模型名称。百炼默认使用 qwen3-rerank，使用自定义服务时填写该服务支持的模型名称。"},
        {"memory.retrieval.embedding_api_key_env", "未配置百炼密钥时，用此名称读取环境变量。填写的是变量名。"},
        {"memory.retrieval.embedding_endpoint", "可填写完整向量接口地址。留空时从百炼服务地址推导。"},
        {"memory.retrieval.rerank_endpoint", "可填写完整重排接口地址。留空时从百炼服务地址推导。"},
        {"memory.agent_mode", "模型工具：对话模型决定何时调用记忆工具。独立规划：先单独规划检索，再把找到的材料交给对话模型。"},
        {"memory.agent_protocol", "原生调用使用模型的工具接口，JSON 信封通过结构化正文表达工具调用。"},
        {"memory.agent_trigger", "回忆类问题：独立规划模式按回忆意图决定是否启动。每轮：独立规划模式在每轮都尝试规划。模型工具模式由模型按需发起。"},
        {"memory.agent_plan_v2", "开启后，独立规划使用按查找目标组织的新版计划。关闭后使用旧版检索规划协议。"},
        {"memory.agent_max_rounds", "单轮对话允许的记忆补查轮数上限。前台仍受共享补查预算和重复调用限制。"},
        {"memory.agent_timeout_ms", "独立 Planner 使用此值作为整轮期限，共享检索执行器也使用此值。模型工具的定向检索受此预算约束，概览走独立查询。它不是主模型请求或 HTTP 超时。"},
        {"memory.agent_max_results", "记忆补查返回的结果数量限制。详细内容可按工具支持的方式进一步核对。"},
        {"memory.owner_cross_channel_private", "允许在主人私密会话中使用其他私密渠道的个人记忆。"},
        {"memory.privacy_rules", "管理员配置的内容规则。exact 使用字面子字符串匹配，pattern 使用正则。规则作用于可见资料及相应读写阶段。"},
        {"memory.blind_zones", "逐行填写渠道和会话标识。桌宠会话使用 default，QQ 使用目标账号或群号。"},
        {"qq.reply.master_any_group", "允许主人在回复群列表之外触发回应。实际触发仍按渠道回复策略判断。"},
        {"qq.reply.group", "关闭：不回群消息。仅被 @ 时：消息必须包含指向茉子的 QQ @ 消息段。全部：无需 @，仍检查群及发送者权限。"},
        {"qq.reply.private", "关闭：不回私聊。仅主人：只回复已绑定主人账号。允许列表：回复主人及表中勾选允许回复的账号。全部：可回复所有账号，是否记录按记录权限判断。"},
        {"qq.reply.restrict_empty_group_list", "开启：群回复表没有勾选项时，按空白名单处理。关闭：空名单沿用原有兼容行为，所有群可进入后续回复策略。主人例外、群触发方式和发送者权限仍分别检查。"},
        {"qq.group_permissions", "分别配置允许记录和允许回复的群。记录还受总开关、群模式和发送者策略限制。回复受回复模式和触发条件限制。检索另检查访问权限。"},
        {"qq.private_permissions", "分别配置私聊记录与回复账号。主人身份、私聊回复模式和已有记忆的检索权限各自检查。"},
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
        {"proactive.daily_limit", "每日共享回顾额度，包含普通回顾、活动、关注与 OpenWake。0 停止后台回顾领取。用户委托提醒独立。"},
        {"proactive.contact_daily_limit", "主动联系投递共享上限。0 表示不限。保持安静时段、授权和语义核验。"},
        {"proactive.concern_default_check_minutes", "关注默认再检查间隔，0 表示只在新证据时。"},
        {"proactive.concern_retention_days", "限制已留存关注的定时复查与失败重试。到期后，仍可由新证据命中或读到已有历史。"},
        {"proactive.cooldown_seconds", "两次后台创作回顾之间的间隔，单位为秒。运行时至少等待 60 秒。"},
        {"proactive.schedule", "可填写 HH:mm 及 fixed（固定）或 random（随机）。当前只保存，定时调度尚未接入。"},
        {"logging.directory", "默认保存到程序目录的 app_data/logs。更换位置后，新日志从保存设置时起写到新目录。"},
        {"api.vision_enabled","开启后，回复带图的消息时，会把所选桌宠图片或 QQ 图片地址交给配置的百炼视觉模型分析。每轮最多分析四张。"},
        {"api.vision_model","填写百炼服务支持的图文对话模型标识；使用上方 API 密钥和服务地址。"},
        {"conversation.input_pause_ms", "同一会话、发送者和引用目标的连续输入可一起回答。每条新输入重新计时，从第一条起最多等待 6 秒。0 表示逐条处理。"},
        {"logging.separate_runs", "开启后每次运行生成独立文件。关闭后按本地日期追加，跨午夜自动换文件。"},
        {"memory.debug_full_logs", "开启后记录对话正文和模型请求，用于诊断。"},
        {"cognition.open_wake.enabled", "允许系统按计划创建内部唤醒（OpenWake）。关闭后不再创建新的唤醒；已有的提醒、回顾和主动联系仍按各自开关执行。"},
        {"cognition.open_wake.allow_contact", "允许内部唤醒在符合条件时主动联系主人。与启用唤醒独立：只开启唤醒、不开启此项时，唤醒只做内部回顾。"},
        {"cognition.open_wake.max_per_day", "0 停止 OpenWake 领取。正值同时受每日共享额度与共享预算占比限制。"},
        {"cognition.open_wake.shared_budget_percent", "内部唤醒与普通回顾共享每日预算时，允许占用的百分比。调低会让唤醒更少占用回顾额度。"},
        {"cognition.snapshot_budget_bytes", "仅限制认知状态快照的 JSON 字节数。角色提示、工具协议、当前输入和其他上下文使用各自预算。"},
        {"cognition.context_actual_enabled", "B5 实验功能。保存后下一次独立认知运行读取。显式测试 setter 优先，其次是环境变量 MAKO_COGNITIVE_CONTEXT_ACTUAL（含 0），最后是此配置。"},
        {"cognition.open_wake.initial_delay_seconds", "仅在尚无有效唤醒计划时用于首次调度。已保存的下一次唤醒不会因保存此值重排。后续时间由模型提议与允许的间隔共同确定。"},
        {"cognition.open_wake.default_interval_seconds", "模型没有给出下次唤醒时间时使用的默认间隔。"},
        {"cognition.open_wake.min_interval_seconds", "允许模型提议的最小唤醒间隔；小于此值会在提交边界被夹紧。"},
        {"cognition.open_wake.max_interval_seconds", "允许模型提议的最大唤醒间隔。"},
        {"cognition.open_wake.max_model_steps_per_wake", "单次内部唤醒最多允许的模型步骤数。"},
        {"cognition.open_wake.max_non_open_jobs_when_due", "内部唤醒到期后，先让出多少次普通回顾任务再执行唤醒。"},
        {"cognition.review_backoff_seconds", "回顾连续失败后的退避秒数列表（最多 5 个，每个 30 至 86400）。例如 120, 300, 900。"},
        {"cognition.max_review_failures", "连续失败多少次后暂停回顾；达到后退避列表用尽即 suspended。"},
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

// identity.json 的字段由 ConfigManager 夹紧/归一；界面优先显示“当前有效值”。
// 未覆盖的路径返回 Undefined，调用方回退到原始文件值。
QString idsText(const QJsonArray &list);

QString validateCognitionConfig(const QJsonObject &merged)
{
    const QJsonObject cognition = merged.value(QStringLiteral("cognition")).toObject();
    const QJsonObject openWake = cognition.value(QStringLiteral("open_wake")).toObject();
    const auto number = [&openWake](const QString &key, int fallback) {
        return openWake.contains(key) ? openWake.value(key).toInt(fallback) : fallback;
    };
    const int minimum = number(QStringLiteral("min_interval_seconds"), 300);
    const int preferred = number(QStringLiteral("default_interval_seconds"), 1800);
    const int maximum = number(QStringLiteral("max_interval_seconds"), 86400);
    if (!(minimum >= 300 && minimum <= 86400)
        || !(preferred >= 300 && preferred <= 86400)
        || !(maximum >= 300 && maximum <= 86400))
        return QStringLiteral("唤醒间隔应为 300 至 86400 秒");
    if (minimum > preferred)
        return QStringLiteral("最小唤醒间隔不能大于默认唤醒间隔");
    if (preferred > maximum)
        return QStringLiteral("默认唤醒间隔不能大于最大唤醒间隔");
    if (cognition.contains(QStringLiteral("review_backoff_seconds"))) {
        const auto backoff = cognition.value(QStringLiteral("review_backoff_seconds"));
        if (!backoff.isArray() || backoff.toArray().isEmpty() || backoff.toArray().size() > 5)
            return QStringLiteral("回顾失败退避应为 1 至 5 个整数秒");
        for (const auto &value : backoff.toArray()) {
            const int seconds = value.toInt(-1);
            if (seconds < 30 || seconds > 86400)
                return QStringLiteral("回顾失败退避每个值应为 30 至 86400 秒");
        }
    }
    if (cognition.contains(QStringLiteral("max_review_failures"))) {
        const int failures = cognition.value(QStringLiteral("max_review_failures")).toInt(-1);
        if (failures < 1 || failures > 10)
            return QStringLiteral("连续失败暂停阈值应为 1 至 10");
    }
    if (cognition.contains(QStringLiteral("snapshot_budget_bytes"))) {
        const int budget = cognition.value(QStringLiteral("snapshot_budget_bytes")).toInt(-1);
        if (budget < 2048 || budget > 8192)
            return QStringLiteral("认知快照预算应为 2048 至 8192 字节");
    }
    return {};
}

bool applyValueToBinding(Binding &binding, const QJsonValue &value)
{
    const QString &type = binding.field.type;
    QWidget *row = binding.row;
    if (type == "bool") {
        if (auto *box = qobject_cast<QCheckBox *>(row))
            box->setChecked(value.toBool());
        else if (auto *widget = row->findChild<QCheckBox *>())
            widget->setChecked(value.toBool());
        else return false;
    } else if (type.startsWith("num:")) {
        auto *spin = qobject_cast<QSpinBox *>(row);
        if (!spin) return false;
        spin->setValue(value.toInt());
    } else if (type.startsWith("choice:")) {
        auto *choice = qobject_cast<QComboBox *>(row);
        if (!choice) return false;
        int index = choice->findData(value.toString());
        if (index < 0) {
            choice->addItem(value.toString(), value.toString());
            index = choice->count() - 1;
        }
        choice->setCurrentIndex(index);
    } else if (type == "time") {
        auto *time = qobject_cast<QTimeEdit *>(row);
        if (!time) return false;
        time->setTime(QTime::fromString(value.toString(), QStringLiteral("HH:mm")));
    } else if (type == "ints") {
        auto *text = row->findChild<QLineEdit *>();
        if (!text) return false;
        QStringList values;
        for (const auto &entry : value.toArray()) values.append(QString::number(entry.toInt()));
        text->setText(values.join(QStringLiteral(", ")));
    } else if (type == "text" || type == "secret" || type == "ids") {
        auto *text = row->findChild<QLineEdit *>();
        if (!text) return false;
        text->setText(type == "ids" ? idsText(value.toArray()) : value.toString());
    } else {
        return false;
    }
    return true;
}

QJsonValue effectiveIdentityValue(const QString &path)
{
    auto &config = ConfigManager::instance();
    if (path.startsWith(QStringLiteral("cognition.open_wake."))) {
        const auto openWake = config.getOpenWakeConfig();
        const QString key = path.section(QLatin1Char('.'), -1);
        if (key == QStringLiteral("enabled")) return openWake.enabled;
        if (key == QStringLiteral("allow_contact")) return openWake.allowContact;
        if (key == QStringLiteral("initial_delay_seconds")) return openWake.initialDelaySeconds;
        if (key == QStringLiteral("default_interval_seconds")) return openWake.defaultIntervalSeconds;
        if (key == QStringLiteral("min_interval_seconds")) return openWake.minIntervalSeconds;
        if (key == QStringLiteral("max_interval_seconds")) return openWake.maxIntervalSeconds;
        if (key == QStringLiteral("max_per_day")) return openWake.maxPerDay;
        if (key == QStringLiteral("shared_budget_percent")) return openWake.sharedBudgetPercent;
        if (key == QStringLiteral("max_model_steps_per_wake")) return openWake.maxModelStepsPerWake;
        if (key == QStringLiteral("max_non_open_jobs_when_due")) return openWake.maxNonOpenJobsWhenDue;
        return {};
    }
    if (path == QStringLiteral("cognition.snapshot_budget_bytes"))
        return config.getSnapshotBudgetBytes();
    if (path == QStringLiteral("cognition.context_actual_enabled"))
        return config.contextActualEnabled();
    if (path == QStringLiteral("cognition.review_backoff_seconds")) {
        QJsonArray values;
        for (const int seconds : config.getProactiveConfig().reviewBackoffSeconds)
            values.append(seconds);
        return values;
    }
    if (path == QStringLiteral("cognition.max_review_failures"))
        return config.getProactiveConfig().maxReviewFailures;
    if (path.startsWith(QStringLiteral("proactive."))) {
        const auto proactive = config.getProactiveConfig();
        if (path == QStringLiteral("proactive.enabled")) return proactive.enabled;
        if (path == QStringLiteral("proactive.private_writing")) return proactive.privateWriting;
        if (path == QStringLiteral("proactive.daily_limit")) return proactive.dailyLimit;
        if (path == QStringLiteral("proactive.contact_daily_limit")) return proactive.contactDailyLimit;
        if (path == QStringLiteral("proactive.concern_default_check_minutes"))
            return proactive.concernDefaultCheckMinutes;
        if (path == QStringLiteral("proactive.concern_retention_days"))
            return proactive.concernRetentionDays;
        if (path == QStringLiteral("proactive.cooldown_seconds")) return proactive.cooldownSeconds;
        if (path == QStringLiteral("proactive.idle_seconds")) return proactive.idleSeconds;
        if (path == QStringLiteral("proactive.quiet_hours.start")) return proactive.quietStart;
        if (path == QStringLiteral("proactive.quiet_hours.end")) return proactive.quietEnd;
        if (path == QStringLiteral("proactive.targets")) {
            QJsonArray targets;
            for (const auto &target : proactive.targets) {
                targets.append(QJsonObject{{QStringLiteral("channel"), target.first},
                                           {QStringLiteral("session_id"), target.second}});
            }
            return targets;
        }
        return {};
    }
    if (path == QStringLiteral("identity.self.qq")) {
        QJsonArray ids;
        for (const auto &id : config.getSelfQQAccounts()) ids.append(id);
        return ids;
    }
    if (path == QStringLiteral("identity.master.qq")) {
        QJsonArray ids;
        for (const auto &id : config.getMasterQQAccounts()) ids.append(id);
        return ids;
    }
    if (path == QStringLiteral("memory.blind_zones")) {
        QJsonArray zones;
        for (const auto &zone : config.blindZones()) {
            zones.append(QJsonObject{{QStringLiteral("channel"), zone.channel},
                                     {QStringLiteral("session_id"), zone.sessionId}});
        }
        return zones;
    }
    if (path == QStringLiteral("appearance.follow_external_mood"))
        return config.getFollowExternalMood();
    return {};
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
        if (field.path.join('.') == QStringLiteral("memory.retrieval.embedding_provider")
            && value.toString() == QStringLiteral("local_hash_test"))
            choice->addItem(QStringLiteral("本地测试（已配置）"), QStringLiteral("local_hash_test"));
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
    if (field.type == "ints") {
        QStringList initial;
        for (const auto &entry : value.toArray()) initial.append(QString::number(entry.toInt()));
        auto *text = new QLineEdit(initial.join(QStringLiteral(", ")), parent);
        text->setPlaceholderText(QStringLiteral("多个整数以逗号或空格分隔，例如 120, 300, 900"));
        return {field, text, label, [text]() -> QJsonValue {
            QString normalized = text->text();
            normalized.replace(QLatin1Char('，'), QLatin1Char(','));
            const auto parts = normalized.split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                                                Qt::SkipEmptyParts);
            QJsonArray result;
            if (parts.size() > 5)
                throw QStringLiteral("最多填写 5 个值");
            for (const auto &part : parts) {
                bool ok = false;
                const int seconds = part.toInt(&ok);
                if (!ok || seconds < 30 || seconds > 86400)
                    throw QStringLiteral("每个值应为 30 至 86400 之间的整数（秒）");
                result.append(seconds);
            }
            if (result.isEmpty()) throw QStringLiteral("至少填写一个值");
            return QJsonValue(result);
        }, {}};
    }

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

struct Group { QString title; QList<Field> fields; bool advanced = false; };
struct Page { QString title; QString detail; QString file; QList<Group> groups; };
Field field(const QString &path, const QString &label, QJsonValue fallback, const QString &type = {})
{
    return {path.split('.'), label, fallback, type};
}


QString openWakeCapStatus()
{
    auto &config = ConfigManager::instance();
    const auto wake = config.getOpenWakeConfig();
    const auto memory = config.getMemoryPolicyConfig();
    const int cap = wake.enabled && memory.longTermMemoryEnabled
        ? OpenWakeConfig::effectiveDailyCap(config.getProactiveConfig().dailyLimit,
                                           wake.maxPerDay, wake.sharedBudgetPercent) : 0;
    return QStringLiteral("已保存配置的有效日上限：%1 次。实际领取还受剩余额度和调度条件限制。").arg(cap);
}

QString integerRange(int minimum, int maximum)
{
    return QStringLiteral("num:%1:%2").arg(minimum).arg(maximum);
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
                field("api.vision_enabled", "识别对话图片", false, "bool"),
                field("api.vision_model", "视觉模型", "qwen3-vl-plus", "text"),
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
            }},
        }},
        {"记忆与检索", "管理对话上下文、检索与索引。", "setting.json", {
            {"对话记忆", {
                field("conversation.input_pause_ms", "连续输入等待（毫秒）", 1600, "num:0:6000"),
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
                field("memory.retrieval.rerank_limit", "检索结果上限", 10, "num:1:40"),
            }},
            {"主动回忆工具", {
                field("memory.agent_enabled", "启用补查工具", true, "bool"),
                field("memory.agent_mode", "回忆方式", "tool", "choice:tool=模型工具|legacy=独立规划"),
                field("memory.agent_protocol", "工具协议", "native", "choice:native=原生调用|json_envelope=JSON 信封"),
                field("memory.agent_timeout_ms", "规划与定向检索预算（毫秒）", 6000, "num:1000:15000"),
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
        {"隐私规则", "设置跨渠道记忆、内容过滤与策略版本。", "setting.json", {
            {"记忆权限", {
                field("memory.owner_cross_channel_private", "允许主人跨私聊渠道使用个人记忆", false, "bool"),
                field("memory.privacy_rules", "内容过滤规则", QJsonArray{}, "rules"),
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
        {"认知与自主行为", "设置主动联系、回顾、内部唤醒、每日上限与统一认知上下文。", "identity.json", {
            {"主动联系与回顾", {
                field("proactive.enabled", "允许主动联系", false, "bool"),
                field("proactive.private_writing", "允许私下回顾与创作", false, "bool"),
                field("proactive.concern_default_check_minutes", "关注再检查间隔（分钟）", 720, "num:0:10080"),
                field("proactive.concern_retention_days", "关注定时复查保留期（天）", 14, "num:1:365"),
                field("proactive.cooldown_seconds", "回顾间隔（秒）", 900, "num:60:604800"),
                field("proactive.idle_seconds", "空闲等待（秒）", 1800, "num:0:604800"),
                field("proactive.quiet_hours.start", "安静开始", "23:00", "time"),
                field("proactive.quiet_hours.end", "安静结束", "08:00", "time"),
                field("proactive.targets", "允许联系的会话", QJsonArray{}, "targets"),
            }},
            {"每日回顾额度与主动联系上限", {
                field("proactive.daily_limit", "每日回顾上限", 3, "num:0:999"),
                field("cognition.open_wake.max_per_day", "每日自主唤醒上限", OpenWakeConfig{}.maxPerDay, integerRange(0, OpenWakeConfig::maxDailyLimit)),
                field("proactive.contact_daily_limit", "每日主动联系上限（0 表示不限）", 3, "num:0:99"),
            }},
            {"内部唤醒与快照", {
                field("cognition.open_wake.enabled", "启用内部唤醒（OpenWake）", false, "bool"),
                field("cognition.open_wake.allow_contact", "允许内部唤醒联系主人", false, "bool"),
                field("cognition.open_wake.shared_budget_percent", "共享每日预算占比（%）", 30, "num:1:100"),
                field("cognition.snapshot_budget_bytes", "认知快照提示词预算（字节）", 4096, "num:2048:8192"),
                field("cognition.context_actual_enabled", "启用统一认知上下文（B5 实验功能）", false, "bool"),
            }},
            {"高级设置：调度与失败退避", {
                field("cognition.open_wake.initial_delay_seconds", "首次唤醒延迟（秒）", OpenWakeConfig{}.initialDelaySeconds,
                      integerRange(OpenWakeConfig::minDelaySeconds, OpenWakeConfig::maxDelaySeconds)),
                field("cognition.open_wake.default_interval_seconds", "默认唤醒间隔（秒）", OpenWakeConfig{}.defaultIntervalSeconds,
                      integerRange(OpenWakeConfig::minFollowupSeconds, OpenWakeConfig::maxDelaySeconds)),
                field("cognition.open_wake.min_interval_seconds", "最小唤醒间隔（秒）", OpenWakeConfig{}.minIntervalSeconds,
                      integerRange(OpenWakeConfig::minFollowupSeconds, OpenWakeConfig::maxDelaySeconds)),
                field("cognition.open_wake.max_interval_seconds", "最大唤醒间隔（秒）", OpenWakeConfig{}.maxIntervalSeconds,
                      integerRange(OpenWakeConfig::minFollowupSeconds, OpenWakeConfig::maxDelaySeconds)),
                field("cognition.open_wake.max_non_open_jobs_when_due", "到期后先让出的普通回顾次数", 3, "num:0:20"),
                field("cognition.review_backoff_seconds", "回顾失败退避（秒，逗号分隔）", QJsonArray{120, 300, 900}, "ints"),
                field("cognition.max_review_failures", "连续失败暂停阈值", 4, "num:1:10"),
            }, true},
        }},
        {"高级与诊断", "检索服务、Legacy 兼容参数与诊断日志；普通使用无需修改。", "setting.json", {
            {"检索服务（高级诊断）", {
                field("memory.retrieval.embedding_enabled", "启用向量召回", false, "bool"),
                field("memory.retrieval.embedding_provider", "向量服务", "", "choice:=未指定|dashscope=阿里云百炼"),
                field("memory.retrieval.embedding_endpoint", "向量端点", "", "text"),
                field("memory.retrieval.embedding_api_key_env", "向量密钥环境变量", "DASHSCOPE_API_KEY", "text"),
                field("memory.retrieval.embedding_model", "向量模型", "", "text"),
                field("memory.retrieval.embedding_dimension", "向量维度（0 为自动）", 0, "num:0:8192"),
                field("memory.retrieval.rerank_endpoint", "重排端点", "", "text"),
                field("memory.retrieval.rerank_model", "重排模型", "qwen3-rerank", "text"),
            }},
            {"兼容性：Legacy Planner", {
                field("memory.agent_plan_v2", "启用第二版检索规划", true, "bool"),
                field("memory.agent_trigger", "规划触发方式", "memory_question", "choice:memory_question=回忆类问题|always=每轮"),
            }},
            {"日志与诊断", {
                field("logging.directory", "日志文件夹", "", "log_directory"),
                field("logging.separate_runs", "每次启动单独创建日志", true, "bool"),
                field("memory.debug_full_logs", "记录完整对话与模型请求", false, "bool"),
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

    // 高级设置默认收起，避免常用参数与调度/退避参数混在一起。
    auto advancedCards = std::make_shared<QList<QGroupBox *>>();
    const bool hasAdvanced = std::any_of(page.groups.cbegin(), page.groups.cend(),
                                         [](const Group &group) { return group.advanced; });
    QCheckBox *advancedToggle = nullptr;
    if (hasAdvanced) {
        advancedToggle = new QCheckBox(QStringLiteral("显示高级设置"), contents);
        advancedToggle->setObjectName(QStringLiteral("showAdvancedSettings"));
        cards->addWidget(advancedToggle);
        QObject::connect(advancedToggle, &QCheckBox::toggled, contents,
                         [advancedCards](bool shown) {
                             for (QGroupBox *card : *advancedCards) card->setVisible(shown);
                         });
    }

    for (const auto &group : page.groups) {
        const auto agent = ConfigManager::instance().getMemoryPolicyConfig().agent;
        if (group.title == QStringLiteral("兼容性：Legacy Planner")
            && (!agent.enabled || agent.mode != QStringLiteral("legacy"))) continue;
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
            const auto key = field.path.join('.');
            QJsonValue configured = isPermissionTable(field)
                ? QJsonValue(permissionLists(*snapshot, field)) : readPath(*snapshot, field.path);
            if (!prompt && page.file == QStringLiteral("identity.json")) {
                const auto effectiveIdentity = effectiveIdentityValue(key);
                if (!effectiveIdentity.isUndefined()) configured = effectiveIdentity;
            }
            auto value = prompt ? QJsonValue(promptText) : configured;
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
            // actual 开关：同时显示配置值（勾选框）和当前实际运行状态（环境变量/配置）。
            if (key == "cognition.context_actual_enabled") {
                const QByteArray envValue = qgetenv("MAKO_COGNITIVE_CONTEXT_ACTUAL").trimmed();
                QString status;
                if (!envValue.isEmpty()) {
                    const QByteArray lowered = envValue.toLower();
                    const bool envOn = lowered == QByteArray("1") || lowered == QByteArray("true")
                        || lowered == QByteArray("on");
                    status = QStringLiteral("当前实际运行：环境变量覆盖为「%1」。"
                                            "环境变量显式设置时优先于配置（含 0）；保存配置不影响本次运行。")
                                 .arg(envOn ? QStringLiteral("开") : QStringLiteral("关"));
                } else {
                    status = QStringLiteral("当前实际运行：按 identity.json 配置为「%1」。"
                                            "保存后下一次独立认知运行生效；旧上下文路径仍可回退。")
                                 .arg(configured.toBool() ? QStringLiteral("开") : QStringLiteral("关"));
                }
                auto *runtimeStatus = new QLabel(status, card);
                runtimeStatus->setWordWrap(true);
                runtimeStatus->setObjectName(key + QStringLiteral(".status"));
                form->addRow(new QLabel(QStringLiteral("当前运行状态"), card), runtimeStatus);
            }
            if (key == "cognition.open_wake.max_per_day") {
                auto *status = new QLabel(card);
                status->setWordWrap(true);
                status->setObjectName(QStringLiteral("openWakeEffectiveDailyCap"));
                status->setText(openWakeCapStatus());
                form->addRow(new QLabel(QStringLiteral("有效上限"), card), status);
            }
            if (key == "memory.agent_mode") {
                const bool legacy = configured.toString() == QStringLiteral("legacy");
                auto *modeStatus = new QLabel(
                    legacy ? QStringLiteral("当前模式：独立规划。高级与诊断中的 Legacy 触发/规划字段参与运行。")
                           : QStringLiteral("当前模式：模型工具。高级与诊断中的 Legacy 触发/规划字段仅保留配置，不参与运行。"),
                    card);
                modeStatus->setWordWrap(true);
                modeStatus->setObjectName(key + QStringLiteral(".status"));
                form->addRow(new QLabel(QStringLiteral("当前模式有效性"), card), modeStatus);
            }
            indexes.append(view.bindings->size());
            view.bindings->append(std::move(binding));
        }
        cards->addWidget(card);
        if (group.advanced) {
            card->setVisible(false);
            advancedCards->append(card);
        }
        view.groups.append({card, indexes});
    }
    cards->addStretch();
    auto *footer = new QHBoxLayout;
    auto *feedback = new QLabel(panel);
    feedback->setObjectName(QStringLiteral("settingsFeedback"));
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
            if (!prompt && page.file == QStringLiteral("identity.json")) {
                const QString invalid = validateCognitionConfig(merged);
                if (!invalid.isEmpty()) {
                    feedback->setText(invalid);
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
            // 保存成功后回读“当前有效值”：identity 字段按 ConfigManager 夹紧/归一后的值显示。
            if (!prompt) {
                QByteArray refreshed;
                if (readFile(page.file, refreshed)) {
                    const auto freshObject = QJsonDocument::fromJson(refreshed).object();
                    for (auto &binding : *bindings) {
                        if (isPermissionTable(binding.field)) continue;
                        const auto key = binding.field.path.join('.');
                        QJsonValue effectiveValue = readPath(freshObject, binding.field.path);
                        if (page.file == QStringLiteral("identity.json")) {
                            const auto normalized = effectiveIdentityValue(key);
                            if (!normalized.isUndefined()) effectiveValue = normalized;
                        }
                        if (effectiveValue.isUndefined()) continue;
                        if (applyValueToBinding(binding, effectiveValue)) {
                            try { binding.initial = binding.value(); }
                            catch (const QString &) {}
                        }
                    }
                }
            }
            if (auto *status = panel->findChild<QLabel *>(QStringLiteral("openWakeEffectiveDailyCap")))
                status->setText(openWakeCapStatus());
            applied();
            QStringList changedKeys;
            for (const auto &[index, value] : edits) {
                Q_UNUSED(value);
                changedKeys.append(bindings->at(index).field.path.join('.'));
            }
            const bool nextRunOnly = page.file == QStringLiteral("identity.json")
                && std::any_of(changedKeys.cbegin(), changedKeys.cend(), [](const QString &key) {
                       return key.startsWith(QStringLiteral("cognition."))
                           || key.startsWith(QStringLiteral("proactive."));
                   });
            feedback->setText(restrictedAfterRemoval
                ? QStringLiteral("已保存。回复名单为空，已启用空名单限制。")
                : nextRunOnly
                    ? QStringLiteral("已保存；普通认知/自主参数在下一轮独立运行或调度边界生效，"
                                     "不会打断当前对话、活动或语音。")
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
            for (int groupIndex = 0; groupIndex < view.groups.size(); ++groupIndex) {
                auto [card, indexes] = view.groups.at(groupIndex);
                const bool advanced = definitions[i].groups.value(groupIndex).advanced;
                auto *advancedToggle = view.widget->findChild<QCheckBox *>(
                    QStringLiteral("showAdvancedSettings"));
                const bool groupMatches = all || card->title().contains(query, Qt::CaseInsensitive);
                bool any = false;
                for (const auto index : indexes) {
                    const auto &binding = view.bindings->at(index);
                    const bool show = groupMatches || binding.field.label.contains(query, Qt::CaseInsensitive)
                        || binding.field.path.join('.').contains(query, Qt::CaseInsensitive);
                    static_cast<QFormLayout *>(card->layout())->setRowVisible(binding.row, show);
                    any |= show;
                }
                if (advanced && any && !query.isEmpty() && !all && advancedToggle)
                    advancedToggle->setChecked(true);
                const bool advancedVisible = !advanced
                    || (advancedToggle && advancedToggle->isChecked());
                card->setVisible(any && advancedVisible);
                pageMatches |= any && advancedVisible;
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
