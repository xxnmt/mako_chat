#include "settingswidget.h"
#include "ui_settingswidget.h"
#include "configmanager.h"
#include "memorymanager.h"
#include "historyturn.h"

#include <QMessageBox>
#include <QInputDialog>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QHeaderView>
#include <QFileInfoList>
#include <QFileInfo>
#include <QDir>
#include <QScrollBar>
#include <QWheelEvent>
#include <QTreeWidgetItem>
#include <QAbstractItemView>
#include <QPoint>
#include <QSpinBox>

SettingsWidget::SettingsWidget(QWidget *parent)
    : QWidget(parent),
    ui(new Ui::SettingsWidget)
{



    ui->setupUi(this);
    setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
    setWindowTitle("mako chat 设置中心");
    resize(760, 520);
    setMinimumSize(520, 400);
    setupHistoryTable();

    // 记忆摘要 / 时间管理控件参数（TD-037 接线）
    ui->spinBox_MemorySummaryLength->setRange(1, 999);
    ui->spinBox_MemorySummaryLength->setSuffix(QStringLiteral(" 轮"));
    ui->timeEdit_nightStart->setDisplayFormat(QStringLiteral("HH:mm"));
    ui->timeEdit_nightEnd->setDisplayFormat(QStringLiteral("HH:mm"));
    const QList<QSpinBox*> lastingSpinBoxes = {
        ui->spinBox_blushingLasting,
        ui->spinBox_emotionLasting,
        ui->spinBox_distanceLasting,
    };
    for (QSpinBox *spin : lastingSpinBoxes) {
        spin->setRange(1, 3600);
        spin->setSuffix(QStringLiteral(" 秒"));
    }

    ui->btn_previousMemoryPage->setEnabled(false);
    ui->btn_nextMemoryPage->setEnabled(false);

    // 左右目录↔滚动 双向同步
    buildSync();

    // 滚轮透传：下拉框/微调框/时间编辑框不拦截界面滚动
    const QList<QObject*> wheelWidgets = {
        ui->comboBox_GPTModel,
        ui->comboBox_SoVitsModel,
        ui->spinBox_shortMemoryLength,
        ui->spinBox_MemorySummaryLength,
        ui->spinBox_blushingLasting,
        ui->spinBox_emotionLasting,
        ui->spinBox_distanceLasting,
        ui->timeEdit_nightStart,
        ui->timeEdit_nightEnd,
    };
    for (QObject *o : wheelWidgets)
        o->installEventFilter(this);

}

SettingsWidget::~SettingsWidget()
{
    delete ui;
}

void SettingsWidget::buildSync()
{
    // ① 一级区域：目录顶层项 → section1
    m_sectionMap[findTreeItem("对话系统")]     = ui->section1_chatSystem;
    m_sectionMap[findTreeItem("语音合成")]     = ui->section1_TTSSystem;
    m_sectionMap[findTreeItem("记忆系统")]     = ui->section1_memorySystem;
    m_sectionMap[findTreeItem("时间管理")]     = ui->section1_timeManagerSystem;
    m_sectionMap[findTreeItem("历史会话记录")] = ui->section1_historyTurn;
    // ② 二级分区：目录叶子项 → section2
    m_sectionMap[findTreeItem("大语言模型")]  = ui->section2_api;
    m_sectionMap[findTreeItem("GPT-Sovits")] = ui->section2_GPT_Sovits;
    m_sectionMap[findTreeItem("短期记忆")]    = ui->section2_shortMemory;
    m_sectionMap[findTreeItem("长期记忆")]    = ui->section2_longMemory;
    m_sectionMap[findTreeItem("昼夜切换")]    = ui->section2_nightChange;
    m_sectionMap[findTreeItem("自动退火")]    = ui->section2_annealingTime;

    // ③ 点击目录 → 滚动到分区
    connect(ui->treeIndex, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *cur, QTreeWidgetItem*){
        if (!cur) return;
        m_syncGuard = true;
        QWidget *sec = m_sectionMap.value(cur);
        if (sec) ui->scrollArea->ensureWidgetVisible(sec, 0, 20);
        m_syncGuard = false;
    });

    // ④ 滚动 → 高亮目录（scroll-spy）
    connect(ui->scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int){
        if (m_syncGuard) return;
        int viewTop = ui->scrollArea->verticalScrollBar()->value();
        QTreeWidgetItem *best = nullptr;
        int bestTop = -1;
        for (auto it = m_sectionMap.cbegin(); it != m_sectionMap.cend(); ++it) {
            int secTop = it.value()->mapTo(ui->scrollArea->viewport(), QPoint(0,0)).y();
            // 挑"顶部还没越过视口、且 y 最大（最贴近视口顶部）"的分区
            if (secTop <= viewTop && secTop > bestTop) {
                bestTop = secTop;
                best = it.key();
            }
        }
        if (best && best != ui->treeIndex->currentItem()) {
            m_syncGuard = true;
            ui->treeIndex->setCurrentItem(best);
            ui->treeIndex->scrollToItem(best, QAbstractItemView::PositionAtTop);
            m_syncGuard = false;
        }
    });
}

QTreeWidgetItem *SettingsWidget::findTreeItem(const QString &text) const
{
    auto search = [&](auto &&self, QTreeWidgetItem *p) -> QTreeWidgetItem* {
        for (int i = 0; i < p->childCount(); ++i) {
            QTreeWidgetItem *c = p->child(i);
            if (c->text(0) == text) return c;
            if (auto *r = self(self, c)) return r;
        }
        return nullptr;
    };
    for (int i = 0; i < ui->treeIndex->topLevelItemCount(); ++i) {
        QTreeWidgetItem *t = ui->treeIndex->topLevelItem(i);
        if (t->text(0) == text) return t;
        if (auto *r = search(search, t)) return r;
    }
    return nullptr;
}

bool SettingsWidget::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::Wheel) {
        // 把滚轮转给右侧滚动条，自身不再滚动
        ui->scrollArea->verticalScrollBar()->event(ev);
        return true;   // 消费掉，阻止下拉框翻项/微调框自增
    }
    return QWidget::eventFilter(obj, ev);
}

void SettingsWidget::setMemoryManager(MemoryManager *manager)
{
    m_memoryManager = manager;
    if (m_memoryManager) {
        refreshHistoryTurnList();
    }
}

void SettingsWidget::setMemoryLength(int length)
{
    ui->spinBox_shortMemoryLength->setValue(length);
}

void SettingsWidget::refreshHistoryTurnList()
{
    if(!m_memoryManager){
        qDebug()<<"[SettingsWidget]:数据库获取失败";
        return;
    }
    m_totalRecords=m_memoryManager->getTotalHistoryCount();
    m_totalPages=(m_totalRecords+m_pageSize-1)/m_pageSize;

    if (m_totalPages < 1){
        m_totalPages = 1;
    }
    if(m_currentPage<m_totalPages){
        m_currentPage=m_totalPages-1;
    }
    if(m_currentPage<0){
        m_currentPage=0;
    }
    updatePageInfo();
    loadHistoryPage(m_currentPage);
}

void SettingsWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    loadSettings();
    refreshHistoryTurnList();

}

void SettingsWidget::on_btn_saveApiKey_clicked()
{
    ConfigManager &configSettings=ConfigManager::instance();
    configSettings.setApiKey(ui->lineEdit_apiKey->text());
    if(configSettings.saveSetting()){
        emit settingsSaved();
        qDebug()<<"[SettingWidget]:apiKey保存成功";
    }
    else{
        qDebug()<<"[SettingsWidget]:配置文件保存失败";
    }
}


void SettingsWidget::on_btn_saveShortMemoryLength_clicked()
{
    ConfigManager &configSettings=ConfigManager::instance();
    configSettings.setShortMemoryLength(ui->spinBox_shortMemoryLength->value());
    if(configSettings.saveSetting()){
        emit settingsSaved();
        qDebug()<<"[SettingWidget]:茉子短期记忆轮数成功设置为"<<ui->spinBox_shortMemoryLength->value()<<"轮";
    }
    else{
        qDebug()<<"[SettingsWidget]:茉子短期记忆轮数设置失败";
    }

}


void SettingsWidget::on_btn_saveMemorySummaryLength_clicked()
{
    ConfigManager &configSettings = ConfigManager::instance();
    configSettings.setSummaryMemoryLength(ui->spinBox_MemorySummaryLength->value());
    if (configSettings.saveSetting()) {
        emit settingsSaved();
        qDebug() << "[SettingsWidget]:长期记忆自动摘轮数设置为"
                 << ui->spinBox_MemorySummaryLength->value() << "轮";
    } else {
        qDebug() << "[SettingsWidget]:长期记忆自动摘轮数设置失败";
    }
}

void SettingsWidget::on_btn_saveNightTime_clicked()
{
    ConfigManager &configSettings = ConfigManager::instance();
    configSettings.setNightStartTime(ui->timeEdit_nightStart->time());
    configSettings.setNightEndTime(ui->timeEdit_nightEnd->time());
    if (configSettings.saveSetting()) {
        emit settingsSaved();
        qDebug() << "[SettingsWidget]:夜晚时段设置为"
                 << ui->timeEdit_nightStart->time().toString("HH:mm")
                 << "-" << ui->timeEdit_nightEnd->time().toString("HH:mm");
    } else {
        qDebug() << "[SettingsWidget]:夜晚时段设置失败";
    }
}

void SettingsWidget::on_btn_saveAnnealingTime_clicked()
{
    ConfigManager &configSettings = ConfigManager::instance();
    configSettings.setBlushLasting(ui->spinBox_blushingLasting->value());
    configSettings.setEmotionLasting(ui->spinBox_emotionLasting->value());
    configSettings.setDistanceLasting(ui->spinBox_distanceLasting->value());
    if (configSettings.saveSetting()) {
        emit settingsSaved();
        qDebug() << "[SettingsWidget]:退火时长设置为 脸红"
                 << ui->spinBox_blushingLasting->value() << "秒 表情"
                 << ui->spinBox_emotionLasting->value() << "秒 距离"
                 << ui->spinBox_distanceLasting->value() << "秒";
    } else {
        qDebug() << "[SettingsWidget]:退火时长设置失败";
    }
}

void SettingsWidget::on_btn_deleteSelectedMemory_clicked()
{
    if(!m_memoryManager){
        qDebug()<<"[SettingsWidget]:数据库获取失败";
        return;
    }
    QList<int> ids=getSelectedMemoryIDs();
    if(ids.isEmpty()){
        qDebug()<<"[SettingsWidget]:你他妈故意找茬是不是？我一个做删除的，能删除你的空对象？";
        return;
    }
    QMessageBox::StandardButton confirm = QMessageBox::question(
        this,
        "确认删除",
        QString("确定要删除选中的 %1 条记录吗？此操作不可撤销！").arg(ids.size()),
        QMessageBox::Yes | QMessageBox::No
        );
    if (confirm != QMessageBox::Yes) return;

    // API Key 验证
    bool ok;
    QString inputKey = QInputDialog::getText(
        this,
        "身份验证",
        "请输入您的 API Key 以确认删除操作：",
        QLineEdit::Password,
        "",
        &ok
        );
    if (!ok || inputKey.isEmpty()) {
        QMessageBox::warning(this, "验证取消", "已取消删除操作");
        qDebug()<<"[SettingsWidget]:取消删除操作";
        return;
    }

    if (!verifyApiKey(inputKey)) {
        QMessageBox::warning(this, "验证失败", "API Key 不正确，拒绝删除！");
        qDebug()<<"[SettingsWidget]:删除验证失败";
        return;
    }
    int successCount = 0;
    for (int id : ids) {
        if (m_memoryManager->deleteTurnByID(id)) {
            successCount++;
        }
    }
    QMessageBox::information(
        this,
        "删除完成",
        QString("成功删除 %1 条记录").arg(successCount)
        );
    qDebug()<<"[SettingsWidget]:共删除"<<successCount<<"条记录";
    refreshHistoryTurnList();

}


void SettingsWidget::on_btn_claenAllMemory_clicked()
{
    if(!m_memoryManager){
        qDebug()<<"[SettingsWidget]:数据库获取失败";
        return;
    }
    QMessageBox::StandardButton confirm = QMessageBox::question(
        this,
        "⚠️ 危险操作",
        "确定要清空所有历史记录吗？此操作不可撤销！",
        QMessageBox::Yes | QMessageBox::No
        );
    if (confirm != QMessageBox::Yes) return;

    bool ok;
    QString inputKey = QInputDialog::getText(
        this,
        "身份验证",
        "请输入您的 API Key 以确认删除操作：",
        QLineEdit::Password,
        "",
        &ok
        );
    if (!ok || inputKey.isEmpty()) {
        QMessageBox::warning(this, "验证取消", "已取消删除操作");
        qDebug()<<"[SettingsWidget]:取消删除操作";
        return;
    }

    if (!verifyApiKey(inputKey)) {
        QMessageBox::warning(this, "验证失败", "API Key 不正确，拒绝删除！");
        qDebug()<<"[SettingsWidget]:删除验证失败";
        return;
    }
    if (m_memoryManager->clearAllHistory()) {
        QMessageBox::information(this, "清空完成", "所有历史记录已清空");
        qDebug()<<"[SettingsWidget]:所有历史记录已清空";
        refreshHistoryTurnList();
    }
    else {
        QMessageBox::critical(this, "清空失败", "清空历史记录时发生错误");
        qDebug()<<"[SettingsWidget]:历史记录清空失败";
    }
}


void SettingsWidget::on_btn_previousMemoryPage_clicked()
{
    if(m_currentPage>0){
        m_currentPage--;
        loadHistoryPage(m_currentPage);
        updatePageInfo();
        qDebug()<<"[SettingsManager]切换上一页:"<<m_currentPage;
    }
}


void SettingsWidget::on_btn_nextMemoryPage_clicked()
{
    if(m_currentPage<m_totalPages-1){
        m_currentPage++;
        loadHistoryPage(m_currentPage);
        updatePageInfo();
        qDebug()<<"[SettingsManager]切换下一页:"<<m_currentPage;
    }
}

void SettingsWidget::loadSettings()
{
    ConfigManager &cfg = ConfigManager::instance();
    ConfigManager &configSettings= ConfigManager::instance();
    ui->lineEdit_apiKey->setText(configSettings.getApiKey());
    ui->spinBox_shortMemoryLength->setValue(configSettings.getShortMemoryLength());
    ui->spinBox_MemorySummaryLength->setValue(cfg.getSummaryMemoryLength());
    ui->timeEdit_nightStart->setTime(cfg.getNightStartTime());
    ui->timeEdit_nightEnd->setTime(cfg.getNightEndTime());
    ui->spinBox_blushingLasting->setValue(cfg.getBlushLasting());
    ui->spinBox_emotionLasting->setValue(cfg.getEmotionLasting());
    ui->spinBox_distanceLasting->setValue(cfg.getDistanceLasting());
    ui->lineEdit_GPTSovitsFilePath->setText(cfg.getGPTSovitsRootPath());
    ui->lineEdit_GPTModelFilePath->setText(cfg.getGPTModelDir());
    ui->lineEdit_sovitsModelFilePath->setText(cfg.getSoVITSModelDir());
    QString gptPath = cfg.getGPTWeightsPath();
    QString sovitsPath = cfg.getSoVITSWeightsPath();

    // 自动扫描模型文件并回填当前选中项
    on_btn_loadGPTModel_clicked();
    on_btn_loadSovitsModel_clicked();
    if (!gptPath.isEmpty()) {
        QString gptFileName = QFileInfo(gptPath).fileName();
        int idx = ui->comboBox_GPTModel->findText(gptFileName);
        if (idx >= 0) {
            ui->comboBox_GPTModel->setCurrentIndex(idx);
        }
    }
    if (!sovitsPath.isEmpty()) {
        QString sovitsFileName = QFileInfo(sovitsPath).fileName();
        int idx = ui->comboBox_SoVitsModel->findText(sovitsFileName);
        if (idx >= 0) {
            ui->comboBox_SoVitsModel->setCurrentIndex(idx);
        }
    }
}

void SettingsWidget::setupHistoryTable()
{
    m_historyModel = new QStandardItemModel(this);
    m_historyModel->setColumnCount(4);

    QStringList headers;
    headers << "ID" << "时间" << "用户输入" << "AI回复";
    m_historyModel->setHorizontalHeaderLabels(headers);

    ui->tableView_historyTurn->setModel(m_historyModel);

    //列宽设置
    ui->tableView_historyTurn->setColumnWidth(0, 50);   // ID
    ui->tableView_historyTurn->setColumnWidth(1, 150);  // 时间
    ui->tableView_historyTurn->setColumnWidth(2, 200);  // 用户输入


    //整行选择，可多选
    ui->tableView_historyTurn->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableView_historyTurn->setSelectionMode(QAbstractItemView::ExtendedSelection);

    ui->tableView_historyTurn->horizontalHeader()->setStretchLastSection(true);
    ui->tableView_historyTurn->setAlternatingRowColors(true);
}

void SettingsWidget::loadHistoryPage(int page)
{
    if(!m_memoryManager){
        qDebug()<<"[SettingsWidget]:数据库获取失败";
        return;
    }
    int offset=page*m_pageSize;
    QList<HistoryTurn> turns =m_memoryManager->getHistoryTurn(offset,m_pageSize);
    m_historyModel->removeRows(0,m_historyModel->rowCount());

    for(const HistoryTurn &turn :turns){
        QList<QStandardItem*>row;
        //ID
        QStandardItem *idItem = new QStandardItem(QString::number(turn.id));
        idItem->setData(turn.id, Qt::UserRole);  // 存储 ID
        idItem->setTextAlignment(Qt::AlignCenter);
        //Time
        QStandardItem *timeItem = new QStandardItem(
            turn.timestamp.toString("yyyy-MM-dd hh:mm:ss")
            );
        timeItem->setTextAlignment(Qt::AlignCenter);
        //user
        QString userText = turn.userInput;
        if (userText.length() > 40) {
            userText = userText.left(40) + "...";
        }
        QStandardItem *userItem = new QStandardItem(userText);
        userItem->setToolTip(turn.userInput);  // 悬停显示完整内容
        //茉子
        QString replyText = turn.rawReply;
        if (replyText.length() > 600) {
            replyText = replyText.left(600) + "...";
        }
        QStandardItem *replyItem = new QStandardItem(replyText);
        replyItem->setToolTip(turn.rawReply);

        row << idItem << timeItem << userItem << replyItem;
        m_historyModel->appendRow(row);
    }

    ui->btn_previousMemoryPage->setEnabled(m_currentPage > 0);
    ui->btn_nextMemoryPage->setEnabled(m_currentPage < m_totalPages - 1);
}

void SettingsWidget::updatePageInfo()
{
    ui->label_MemoryPage->setText(
        QString("%1 / %2 页").arg(m_currentPage + 1).arg(m_totalPages)
        );
}


bool SettingsWidget::verifyApiKey(const QString &inputKey)
{
    QString currentKey=ConfigManager::instance().getApiKey();
    qDebug()<<"api"<<ConfigManager::instance().getApiKey();
    qDebug()<<"userinput"<<inputKey;
    if(currentKey==inputKey){
        qDebug()<<"[SettingsWidget]:api删除验证成功";
        return true;
    }
    else{
        qDebug()<<"[SettingsWidget]:api删除验证失败";
        return false;
    }
}

QList<int> SettingsWidget::getSelectedMemoryIDs()
{
    QList<int>ids;
    QModelIndexList selectedRows =ui->tableView_historyTurn->selectionModel()->selectedRows();
    for(const QModelIndex &index:selectedRows){
        int row =index.row();
        QModelIndex idIndex=m_historyModel->index(row,0);
        int id=m_historyModel->data(idIndex,Qt::UserRole).toInt();
        if(id>0){
            ids.append(id);
        }
    }
    qDebug()<<"[SettingsWidget]已选择将要删除的记录id:"<<ids;
    return ids;
}


void SettingsWidget::on_btn_saveGPTSoVitsPath_clicked()
{
    QString rootPath = ui->lineEdit_GPTSovitsFilePath->text().trimmed();
    if (rootPath.isEmpty()) {
        // QMessageBox::warning(this, "路径为空", "请先输入 GPT‑SoVITS 的安装目录");
        qDebug()<<"[SettingsWidget]:路径为空,请先输入 GPT‑SoVITS 的安装目录";
        return;
    }
    ConfigManager::instance().setGPTSovitsRootPath(rootPath);
    ConfigManager::instance().saveSetting();
    // QMessageBox::information(this, "保存成功", "GPT‑SoVITS 目录已保存");
    qDebug()<<"[SettingsWidget]:保存成功,GPT‑SoVITS目录已保存"<<rootPath;

}

void SettingsWidget::on_btn_saveGPTModelPath_clicked()
{
    QString selected = ui->comboBox_GPTModel->currentText();
    if (selected.isEmpty()) {
        // QMessageBox::warning(this, "未选择", "请先选择一个 GPT 模型");
        qDebug()<<"[Settings]未选择,请先选择一个GPT模型:";
        return;
    }
    QString dirPath = ConfigManager::instance().getGPTModelDir();
    if (dirPath.isEmpty()) {
        // QMessageBox::warning(this, "目录未设置", "请先设置 GPT 模型文件夹路径");
        qDebug()<<"[Settings]目录未设置,请先设置GPT模型文件夹路径:";
        return;
    }
    QString fullPath = QDir(dirPath).filePath(selected);
    ConfigManager::instance().setGPTWeightsPath(fullPath);
    qDebug()<<"[Settings]GPT模型已设为:"<<fullPath;
    // QMessageBox::information(this, "保存成功", "GPT 模型路径已保存");
    emit ttsModelSwitchRequested(
        ConfigManager::instance().getGPTWeightsPath(),
        ConfigManager::instance().getSoVITSWeightsPath());
}


void SettingsWidget::on_btn_saveSoVitsModelPath_clicked()
{
    QString selected = ui->comboBox_SoVitsModel->currentText();
    if (selected.isEmpty()) {
        // QMessageBox::warning(this, "未选择", "请先选择一个 SoVits 模型");
        qDebug()<<"[Settings]未选择,请先选择一个SoVits模型:";
        return;
    }
    QString dirPath = ConfigManager::instance().getSoVITSModelDir();
    if (dirPath.isEmpty()) {
        // QMessageBox::warning(this, "目录未设置", "请先设置 SoVits 模型文件夹路径");
        qDebug()<<"[Settings]目录未设置,请先设置SoVits模型文件夹路径:";
        return;
    }
    QString fullPath = QDir(dirPath).filePath(selected);
    ConfigManager::instance().setSoVITSWeightsPath(fullPath);
    qDebug()<<"[Settings]SoVits模型已设为:"<<fullPath;
    // QMessageBox::information(this, "保存成功", "SoVits 模型路径已保存");
    emit ttsModelSwitchRequested(
        ConfigManager::instance().getGPTWeightsPath(),
        ConfigManager::instance().getSoVITSWeightsPath());
}


void SettingsWidget::on_btn_saveGPTPath_clicked()
{
    QString dirPath = ui->lineEdit_GPTModelFilePath->text().trimmed();
    if (dirPath.isEmpty()) {
        qDebug()<<"[SettingsWidget]GPT模型文件夹路径为空";
        return;
    }
    ConfigManager::instance().setGPTModelDir(dirPath);
    ConfigManager::instance().saveSetting();
    qDebug()<<"[SettingsWidget]GPT模型文件夹路径已保存:"<<dirPath;
}


void SettingsWidget::on_btn_loadGPTModel_clicked()
{
    QString dirPath = ConfigManager::instance().getGPTModelDir();
    if (dirPath.isEmpty()) {
        // QMessageBox::warning(this, "路径未设置", "请先在上方输入 GPT 模型文件夹路径并保存");
        qDebug()<<"[SettingsWidget]:路径未设置, 请先在上方输入GPT模型文件夹路径并保存";
        return;
    }
    QDir dir(dirPath);
    if (!dir.exists()) {
        // QMessageBox::warning(this, "目录不存在", "找不到模型目录：" + dirPath);
        qDebug()<<"[SettingsWidget]目录不存在, 找不到模型目录："<<dirPath;
        return;
    }
    QStringList filters;
    filters << "*.ckpt" << "*.pth";
    QStringList modelFiles = dir.entryList(filters, QDir::Files, QDir::Name);
    ui->comboBox_GPTModel->clear();
    if (modelFiles.isEmpty()) {
        // QMessageBox::information(this, "扫描结果", "未找到模型文件");
        qDebug()<<"[SettingsWidget]扫描结果:未找到模型文件";
        return;
    }
    ui->comboBox_GPTModel->addItems(modelFiles);
    // QMessageBox::information(this, "扫描完成", QString("找到 %1 个模型文件").arg(modelFiles.size()));
    qDebug()<<"[SettingsWidget]扫描完成:找到"<<modelFiles.size()<<"模型文件";
}


void SettingsWidget::on_btn_saveSoVitsPath_clicked()
{
    QString dirPath = ui->lineEdit_sovitsModelFilePath->text().trimmed();
    if (dirPath.isEmpty()) {
        qDebug()<<"[SettingsWidget]SoVITS模型文件夹路径为空";
        return;
    }
    ConfigManager::instance().setSoVITSModelDir(dirPath);
    ConfigManager::instance().saveSetting();
    qDebug()<<"[SettingsWidget]SoVITS模型文件夹路径已保存:"<<dirPath;
}


void SettingsWidget::on_btn_loadSovitsModel_clicked()
{
    QString dirPath = ConfigManager::instance().getSoVITSModelDir();
    if (dirPath.isEmpty()) {
        // QMessageBox::warning(this, "路径未设置", "请先在上方输入 SoVITS 模型文件夹路径并保存");
        qDebug()<<"[SettingsWidget]:路径未设置, 请先在上方输入SoVITS模型文件夹路径并保存";

        return;
    }
    QDir dir(dirPath);
    if (!dir.exists()) {
        // QMessageBox::warning(this, "目录不存在", "找不到模型目录：" + dirPath);
        qDebug()<<"[SettingsWidget]目录不存在, 找不到模型目录："<<dirPath;
        return;
    }
    QStringList filters;
    filters << "*.pth" << "*.ckpt";
    QStringList modelFiles = dir.entryList(filters, QDir::Files, QDir::Name);
    ui->comboBox_SoVitsModel->clear();
    if (modelFiles.isEmpty()) {
        // QMessageBox::information(this, "扫描结果", "未找到模型文件");
        qDebug()<<"[SettingsWidget]扫描结果:未找到模型文件";
        return;
    }
    ui->comboBox_SoVitsModel->addItems(modelFiles);
    // QMessageBox::information(this, "扫描完成", QString("找到 %1 个模型文件").arg(modelFiles.size()));
    qDebug()<<"[SettingsWidget]扫描完成:找到"<<modelFiles.size()<<"模型文件";
}

