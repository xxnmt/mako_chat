#include "chatwidget.h"
#include "debuglog.h"
#include <QHBoxLayout>
#include <QToolButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QDebug>

ChatWidget::ChatWidget(QWidget *parent)
    : QWidget{parent}
{
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    setWindowTitle("与茉子聊天");

    resize(360, 60);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setPlaceholderText("想和茉子说什么...");
    auto *layout=new QHBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(m_lineEdit);

    auto *attach=new QToolButton(this);
    attach->setText(tr("附图"));
    attach->setToolTip(tr("选择要与本条消息一起发送的图片"));
    layout->addWidget(attach);
    connect(attach,&QToolButton::clicked,this,[this,attach] {
        if (m_imageFile.isEmpty()) {
            m_imageFile=QFileDialog::getOpenFileName(this,tr("选择图片"),{},tr("图片 (*.png *.jpg *.jpeg *.webp)"));
        } else {
            m_imageFile.clear();
        }
        attach->setText(m_imageFile.isEmpty() ? tr("附图") : tr("移除图片"));
        attach->setToolTip(m_imageFile.isEmpty() ? tr("选择图片") : QFileInfo(m_imageFile).fileName());
        m_lineEdit->setFocus();
    });
    connect(m_lineEdit,&QLineEdit::returnPressed,this,[this,attach](){
        QString text = m_lineEdit->text().trimmed();

        qDebug() << "[user] 输入长度:" << text.size();
        if (debugFullLogsEnabled()) {
            qDebug() << "[user][debug]:" << text;
        }

        if(!text.isEmpty() || !m_imageFile.isEmpty()){
            emit textSubmitted(m_imageFile.isEmpty() ? text : QStringLiteral("[图片]")+text,m_imageFile);
            m_imageFile.clear();
            attach->setText(tr("附图"));
            attach->setToolTip(tr("选择图片"));
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

