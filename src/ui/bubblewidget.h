#ifndef BUBBLEWIDGET_H
#define BUBBLEWIDGET_H

#include <QWidget>
#include <QTimer>
#include <QString>

namespace Ui {
class BubbleWidget;
}

class BubbleWidget : public QWidget
{
    Q_OBJECT

public:
    explicit BubbleWidget(QWidget *parent = nullptr);
    ~BubbleWidget();

    void showMessage(const QString& text);

protected:
    void paintEvent(QPaintEvent* event);

signals:
    void bubbleShown();
    
private slots:
    void typeWriteEffect();

private:
    Ui::BubbleWidget *ui;



    QTimer* m_timer;
    QString m_text;
    int m_idex;
};

#endif // BUBBLEWIDGET_H
