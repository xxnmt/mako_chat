#ifndef CHATWIDGET_H
#define CHATWIDGET_H

#include <QWidget>
#include <QLineEdit>
#include <QShowEvent>

class ChatWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ChatWidget(QWidget *parent = nullptr);

public slots:
    void popup();

signals:
    void textSubmitted(const QString &text,const QString &imageFile);
protected:
    void showEvent(QShowEvent *event) override;

private:

    QString m_imageFile;
    QLineEdit *m_lineEdit;
};

#endif // CHATWIDGET_H
