// BreezeTheme.h — KDE Breeze look (light and dark) for Qt Widgets: Fusion style + Breeze palette
// + a style sheet for buttons, combo boxes, sliders and lists. No Breeze plugin is needed.
#pragma once

#include <QString>

class QApplication;

namespace BreezeTheme {
void Apply(QApplication& app, bool dark);
QString StyleSheet(bool dark);
} // namespace BreezeTheme
