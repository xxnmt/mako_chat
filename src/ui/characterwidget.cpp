#include "characterwidget.h"

#include <QDebug>
#include <QVBoxLayout>
#include <QPixmap>
#include <QCoreApplication>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <QMenu>
#include <QAction>
#include <QContextMenuEvent>

CharacterWidget::CharacterWidget(QWidget *parent)
    : QWidget(parent)
{
    //without side  always on top
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    //without background
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground); // 透明窗口切换时减少系统重绘闪帧

    imageLabel = new QLabel(this);

    //设置布局，让图片填满整个窗口
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); // 取消边距
    layout->addWidget(imageLabel);

}

CharacterWidget::~CharacterWidget() {}

void CharacterWidget::updatePath(const QString &imagePath)
{
    QPixmap pixmap(imagePath);
    if (pixmap.isNull()) return;

    // 首次加载：直接设置（构造期调用，窗口未显示，无需锚定）
    if (m_visibleRect.isEmpty()) {
        imageLabel->setPixmap(pixmap);
        resize(pixmap.size());
        m_visibleRect = calculateVisibleRect(pixmap);
        qDebug()<<"[character]扫描位置为："<<m_visibleRect;
        return;
    }

    // 后续切换：锚定“脚底中心”。两张立绘 bottom 都是 767（脚底对齐），
    // 保持脚底不动、角色原地生长，避免 center 锚定让 closer 脚底下坠造成向下漂。
    QPoint anchor = this->pos()
                  + QPoint(m_visibleRect.center().x(), m_visibleRect.bottom());

    imageLabel->setPixmap(pixmap);
    m_visibleRect = calculateVisibleRect(pixmap);
    qDebug()<<"[character]扫描位置为："<<m_visibleRect;

    QPoint newFeet = this->pos()
                   + QPoint(m_visibleRect.center().x(), m_visibleRect.bottom());
    QPoint newPos = this->pos() + (anchor - newFeet);
    // 先同步渲染新图，再移动窗口：避免 setGeometry 移动瞬间系统仍以旧图重绘造成“向右闪一下”
    repaint();
    setGeometry(newPos.x(), newPos.y(), pixmap.width(), pixmap.height());
}

QPoint CharacterWidget::getBubbleAnchorPos() const
{
    return this->pos() + m_visibleRect.topRight() + QPoint(5, 10);
}

QPoint CharacterWidget::getChatAnchorPos() const
{
    return this->pos() + QPoint(0,m_visibleRect.bottom()+10);
}

QRect CharacterWidget::getVisibleRect() const
{
    return this->m_visibleRect;
}


void CharacterWidget::mousePressEvent(QMouseEvent *event)
{
    if(event->button()==Qt::LeftButton){
        dragPosition=event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
    }
}

void CharacterWidget::mouseMoveEvent(QMouseEvent *event)
{
    if(event->buttons() & Qt::LeftButton){
        move(event->globalPosition().toPoint() - dragPosition);
        event->accept();
    }
}

void CharacterWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *chatAction = menu.addAction("和茉子聊天");
    QAction *settingsAction = menu.addAction("设置");
    menu.addSeparator();
    QAction *quitAction= menu.addAction("退出");

    connect(chatAction, &QAction::triggered, this, &CharacterWidget::chatRequested);
    connect(settingsAction, &QAction::triggered, this, &CharacterWidget::settingsRequested);

    connect(quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);

    menu.exec(event->globalPos());
}


QRect CharacterWidget::calculateVisibleRect(const QPixmap &pixmap)
{
    QImage image = pixmap.toImage();
    int width = image.width();
    int height = image.height();
    //扫描像素点的透明度（Alpha 通道）
    int left = width, right = 0, top = height, bottom = 0;
    // bool hasAlpha = false;
    for(int y=0;y<height;y++){
        for(int x=0;x<width;x++){
            if (qAlpha(image.pixel(x, y)) > 10) {
                if (x < left)   left = x;
                if (x > right)  right = x;
                if (y < top)    top = y;
                if (y > bottom) bottom = y;
                // hasAlpha = true;
            }
        }
    }
    return QRect(left, top, right - left + 1, bottom - top + 1);
}
