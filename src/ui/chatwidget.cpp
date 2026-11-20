#include "chatwidget.h"
#include "debuglog.h"
#include <QVBoxLayout>
#include <QDebug>

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget{parent}
{
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    setWindowTitle("与茉子聊天");

    resize(360, 60);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setPlaceholderText("想和茉子说什么...");
    QVBoxLayout *layout =new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(m_lineEdit);

    connect(m_lineEdit,&QLineEdit::returnPressed,this,[this](){
        QString text = m_lineEdit->text().trimmed();

        qDebug() << "[user] 输入长度:" << text.size();
        if (debugFullLogsEnabled()) {
            qDebug() << "[user][debug]:" << text;
        }

        if(!text.isEmpty()){
        emit textSubmitted(text);
        m_lineEdit->clear();
        hide();
        }
    });
}


void ChatWidget::popup()
{
    this->show();
    this->activateWindow();
}

void ChatWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_lineEdit->setFocus();
}

