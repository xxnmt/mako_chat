#include "anchormanager.h"
#include "characterwidget.h"

#include <QDebug>

AnchorManager::AnchorManager(CharacterWidget *character, QObject *parent)
    :QObject(parent),m_character(character)
{
    if (m_character) {
        m_character->installEventFilter(this);
    }
}

AnchorManager::~AnchorManager()
{
    m_anchors.clear();
    qDebug()<<"[AnchorManager]:已清理位置管理";
}

void AnchorManager::registerWidget(QWidget *widget, AnchorConfig config)
{
    if(!widget){
        qDebug()<<"[anchormanager]绑定失败，缺失对象:"<<"widget";
        return;
    }
    if(!m_character){
        qDebug()<<"[anchormanager]绑定失败，缺失对象:"<<"m_character";
        return;
    }
    // widget->setParent(this);

    AnchorInfo info;
    info.widget=widget;
    info.config=config;
    info.hasCustomCalculator=false;
    m_anchors[widget]=info;


}

void AnchorManager::registerWidget(QWidget *widget, std::function<QPoint ()> customCalculator)
{
    if(!widget){
        qDebug()<<"[anchormanager]绑定失败，缺失对象:"<<"widget";
        return;
    }
    if(!m_character){
        qDebug()<<"[anchormanager]绑定失败，缺失对象:"<<"m_character";
        return;
    }
    AnchorInfo info;
    info.widget=widget;
    info.customCalculator=customCalculator;
    info.hasCustomCalculator=true;
    m_anchors[widget]=info;
}

void AnchorManager::unregisterWidget(QWidget *widget)
{
    auto it = m_anchors.find(widget);
    if (it != m_anchors.end()) {
        if (m_character) {
            widget->removeEventFilter(this);
        }
        m_anchors.erase(it);
    }
}

void AnchorManager::updateAllAnchors()
{
if (!m_character){
        qDebug()<<"[anchormanager]更行位置失败，缺失对象:"<<"m_character";
    return;
}

    QRect visibleRect=m_character->getVisibleRect();
    QPoint characterPos=m_character->pos();

    for (auto& pair : m_anchors) {
        QWidget* widget = pair.first;
        AnchorInfo& info = pair.second;
        if (!widget) continue;

        QPoint targetPos;

        if (info.hasCustomCalculator) {
            // 使用自定义计算函数
            targetPos = info.customCalculator();
        } else {
            // 使用预定义的锚点策略
            targetPos = calculatePosition(visibleRect, characterPos, widget->size(), info.config);
        }

        widget->move(targetPos);
    }

}

QPoint AnchorManager::calculatePosition(const QRect& visibleRect,
                                        const QPoint& characterPos,
                                        const QSize& widgetSize,
                                        const AnchorConfig& config)
{
    int x = 0, y = 0;

    switch (config.position) {
    case AnchorPosition::Bottom:
        x = visibleRect.center().x() - widgetSize.width() / 2;
        y = visibleRect.bottom() + 10;
        break;

    case AnchorPosition::WaistCenter:
        x = visibleRect.center().x() - widgetSize.width() / 2;
        y = visibleRect.top() + visibleRect.height() * 0.7 - widgetSize.height() / 2;
        break;

    case AnchorPosition::WaistLeft:
        x = visibleRect.left() - widgetSize.width() - 10;
        y = visibleRect.top() + visibleRect.height() * 0.7 - widgetSize.height() / 2;
        break;

    case AnchorPosition::WaistRight:
        x = visibleRect.right() + 10;
        y = visibleRect.top() + visibleRect.height() * 0.7 - widgetSize.height() / 2;
        break;

    case AnchorPosition::TopCenter:
        x = visibleRect.center().x() - widgetSize.width() / 2;
        y = visibleRect.top() - widgetSize.height() - 10;
        break;
    case AnchorPosition::HeadRight:
        x = visibleRect.right() + 10;
        y = visibleRect.top() + visibleRect.height() * 0.2 - widgetSize.height() / 2;
        break;

    default:
        x = visibleRect.center().x() - widgetSize.width() / 2;
        y = visibleRect.bottom() + 10;
        break;
    }

    // 加上额外偏移
    x += config.offset.x();
    y += config.offset.y();

    return characterPos + QPoint(x, y);
}

bool AnchorManager::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_character &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        updateAllAnchors();
    }
    return QObject::eventFilter(watched, event);
}