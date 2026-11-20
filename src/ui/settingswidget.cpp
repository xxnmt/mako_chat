#include "settingswidget.h"
#include "settingspages.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
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
#include <QAbstractItemView>
#include <QPoint>
#include <QPushButton>

SettingsWidget::SettingsWidget(QWidget *parent)
    : QWidget(parent),
    ui(new Ui::SettingsWidget)
{



    ui->setupUi(this);
    ui->horizontalLayout->removeWidget(ui->section1_historyTurn);
    ui->horizontalLayout->setContentsMargins(0, 0, 0, 0);
    setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint);
    setWindowTitle(QStringLiteral("茉子设置"));
    resize(900, 640);
    setMinimumSize(620, 460);
    setupHistoryTable();
}

SettingsWidget::~SettingsWidget()
{
    delete ui;
}

void SettingsWidget::setMemoryManager(MemoryManager *manager)
{
    m_memoryManager = manager;
    if (m_memoryManager) {
        refreshHistoryTurnList();
    }
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
    ui->section1_historyTurn->setParent(this);
    delete m_settingsPages;
    m_settingsPages = buildSettingsPages(
        ui->section1_historyTurn, [this] { emit settingsSaved(); }, this);
    ui->horizontalLayout->addWidget(m_settingsPages);
    refreshHistoryTurnList();

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
        if (turn.state == "quiet") replyText = QStringLiteral("主动安静 · ") + turn.detail;
        else if (turn.state == "interrupted") replyText = QStringLiteral("处理中断 · ") + turn.detail;
        else if (turn.state == "received") replyText = QStringLiteral("已收到输入，未记录处理结果");
        else if (turn.state == "legacy") {
            const auto legacy = QJsonDocument::fromJson(turn.rawReply.toUtf8()).object();
            if (legacy["sentences"].isArray() && legacy["sentences"].toArray().isEmpty())
                replyText = QStringLiteral("历史输入，未记录表达原因");
        }
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


