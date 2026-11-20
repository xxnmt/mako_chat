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

    void refreshHistoryTurnList();

protected:
    void showEvent(QShowEvent *event)override;
signals:
    void settingsSaved();

private slots:
    //tab config

    //tab memory
    void on_btn_deleteSelectedMemory_clicked();
    void on_btn_claenAllMemory_clicked();
    void on_btn_previousMemoryPage_clicked();
    void on_btn_nextMemoryPage_clicked();

    //tab TTs

private:
    Ui::SettingsWidget *ui;
    QWidget *m_settingsPages = nullptr;



    MemoryManager *m_memoryManager=nullptr;
    QStandardItemModel *m_historyModel=nullptr;

    int m_currentPage = 0;
    const int m_pageSize = 50;
    int m_totalRecords = 0;
    int m_totalPages = 1;


    void setupHistoryTable();
    void loadHistoryPage(int page);
    void updatePageInfo();
    bool verifyApiKey(const QString &inputKey);
    QList<int> getSelectedMemoryIDs();
};

#endif // SETTINGWIDGET_H
