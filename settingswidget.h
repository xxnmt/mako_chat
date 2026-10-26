#ifndef SETTINGWIDGET_H
#define SETTINGWIDGET_H

#include <QWidget>
#include <QVector>
#include <QList>
#include <QEvent>
#include <QString>
#include <QMap>

#include <QStandardItemModel>


class MemoryManager;
class QTreeWidgetItem;

namespace Ui {
class SettingsWidget;
}

class SettingsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsWidget(QWidget *parent = nullptr);
    ~SettingsWidget();

    void setMemoryManager(MemoryManager *manager);
    void setMemoryLength(int length);

    void refreshHistoryTurnList();

protected:
    void showEvent(QShowEvent *event)override;
    bool eventFilter(QObject *obj, QEvent *ev) override;
signals:
    void settingsSaved();
    void ttsModelSwitchRequested(const QString &gptPath, const QString &sovitsPath);

private slots:
    //tab config
    void on_btn_saveApiKey_clicked();

    //tab memory
    void on_btn_saveShortMemoryLength_clicked();
    void on_btn_saveMemorySummaryLength_clicked();
    void on_btn_saveNightTime_clicked();
    void on_btn_saveAnnealingTime_clicked();
    void on_btn_deleteSelectedMemory_clicked();
    void on_btn_claenAllMemory_clicked();
    void on_btn_previousMemoryPage_clicked();
    void on_btn_nextMemoryPage_clicked();

    //tab TTs
    void on_btn_saveGPTSoVitsPath_clicked();
    void on_btn_saveGPTModelPath_clicked();
    void on_btn_saveSoVitsModelPath_clicked();
    void on_btn_saveGPTPath_clicked();
    void on_btn_loadGPTModel_clicked();
    void on_btn_saveSoVitsPath_clicked();
    void on_btn_loadSovitsModel_clicked();

private:
    Ui::SettingsWidget *ui;



    MemoryManager *m_memoryManager=nullptr;
    QStandardItemModel *m_historyModel=nullptr;

    int m_currentPage = 0;
    const int m_pageSize = 50;
    int m_totalRecords = 0;
    int m_totalPages = 1;

    bool m_syncGuard = false;                       // 防止 滚动↔点击 互踩
    QMap<QTreeWidgetItem*, QWidget*> m_sectionMap;  // 目录项 → 分区
    void buildSync();                               // 建映射 + 连两个方向
    QTreeWidgetItem *findTreeItem(const QString &text) const; // 按文本找目录项

    void loadSettings();
    void setupHistoryTable();
    void loadHistoryPage(int page);
    void updatePageInfo();
    bool verifyApiKey(const QString &inputKey);
    QList<int> getSelectedMemoryIDs();
};

#endif // SETTINGWIDGET_H
