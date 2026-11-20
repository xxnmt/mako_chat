#include "bubblewidget.h"
#include "ui_bubblewidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QEvent>
BubbleWidget::BubbleWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::BubbleWidget)
{
    ui->setupUi(this);

    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    hide();

    m_timer=new QTimer;
    connect(m_timer,&QTimer::timeout,this,&BubbleWidget::typeWriteEffect);

}

BubbleWidget::~BubbleWidget()
{
    delete ui;
}

void BubbleWidget::showMessage(const QString& text)
{
    m_text=text;
    m_idex=0;
    ui->label_text->clear();

    setFixedSize(250,120);    
    emit bubbleShown();
    this->show();
    m_timer->start(50);


}



void BubbleWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);//抗锯齿
    //paint the label_text
    QPainterPath path;
    path.addRoundedRect(rect(),15,15);
    painter.fillPath(path,QColor(0,0,0,180));
}



void BubbleWidget::typeWriteEffect()
{
    if(m_idex<m_text.length()){
        m_idex++;
        ui->label_text->setText(m_text.left(m_idex));
    }
    else{
        m_timer->stop();
    }
}

