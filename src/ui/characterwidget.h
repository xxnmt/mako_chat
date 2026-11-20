#ifndef CHARACTERWIDGET_H
#define CHARACTERWIDGET_H

#include <QWidget>
#include <QMouseEvent>
#include <QLabel>
#include <QContextMenuEvent>


class CharacterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CharacterWidget(QWidget *parent = nullptr);
    ~CharacterWidget() override;

    void updatePath(const QString &imagePath);
    //get bubble绘制起始坐标
    QPoint getBubbleAnchorPos() const;
    QPoint getChatAnchorPos() const;
    QRect getVisibleRect()const;

signals:
    void chatRequested();
    void settingsRequested();

protected:
    //mouseevent Rewrite
    void mousePressEvent(QMouseEvent* event);
    void mouseMoveEvent(QMouseEvent* event);
    void contextMenuEvent(QContextMenuEvent *event);

private slots:


private:
    QPoint dragPosition; //鼠标点击时相对坐标
    QLabel *imageLabel;  //立绘图片的标签

    //
    // BubbleWidget *speakBubble;
    //存储立绘中真正有像素的有效区域
    QRect m_visibleRect;
    //扫描像素的辅助函数
    QRect calculateVisibleRect(const QPixmap &pixmap);


    // LLMService *m_llmService;

};
#endif // CHARACTERWIDGET_H
