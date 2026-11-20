#pragma once

#include <functional>

class QWidget;
QWidget *buildSettingsPages(QWidget *history, std::function<void()> applied, QWidget *parent);
