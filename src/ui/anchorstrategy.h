#ifndef ANCHORSTRATEGY_H
#define ANCHORSTRATEGY_H
#include <QPoint>

enum class AnchorPosition {
    Bottom,// 底部
    WaistCenter,// 腰部居中
    WaistLeft,// 腰部左侧
    WaistRight,// 腰部右侧
    TopCenter,// 顶部居中
    HeadRight,//头部右侧
    Custom,// 自定义
};

//带偏移量
struct AnchorConfig {
    AnchorPosition position;
    QPoint offset;  //偏移量
};
#endif // ANCHORSTRATEGY_H
