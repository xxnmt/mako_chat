#ifndef ANCHORMANAGER_H
#define ANCHORMANAGER_H

#include <QObject>
#include <QPoint>
#include <functional>
#include <unordered_map>

#include "anchorstrategy.h"

class CharacterWidget;
struct AnchorConfig;

class AnchorManager : public QObject {
    Q_OBJECT
public:
    explicit AnchorManager(CharacterWidget* character, QObject* parent = nullptr);
    ~AnchorManager();

    void registerWidget(QWidget* widget, AnchorConfig config);
    void registerWidget(QWidget* widget, std::function<QPoint()> customCalculator);
    void unregisterWidget(QWidget* widget);
    void updateAllAnchors();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    CharacterWidget* m_character;

    struct AnchorInfo {
        QWidget* widget;
        AnchorConfig config;
        std::function<QPoint()> customCalculator;
        bool hasCustomCalculator;
    };

    std::unordered_map<QWidget*, AnchorInfo> m_anchors;

    QPoint calculatePosition(const QRect& visibleRect,
                      const QPoint& characterPos,
                      const QSize& widgetSize,
                             const AnchorConfig& config);
};
#endif // ANCHORMANAGER_H
