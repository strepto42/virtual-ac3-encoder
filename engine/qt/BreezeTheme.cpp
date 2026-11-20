#include "BreezeTheme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

namespace {
struct Colors
{
  const char* window;
  const char* base;
  const char* text;
  const char* dim;
  const char* button;
  const char* pressed;
  const char* border;
  const char* groove;
  const char* handle;
  const char* handleBorder;
  const char* accent;
  const char* accentHover;
  const char* accentText;
  const char* danger;
  const char* disabled;
  const char* hover;
  const char* tipBase;
  const char* tipText;
};

// Breeze (Plasma 5) colours. Text on the accent is dark in both themes: white on #3daee9 is too faint.
const Colors kLight = {"#eff0f1", "#fcfcfc", "#31363b", "#5d676f", "#f7f8f9", "#d9e8f2", "#bdc3c7",
                       "#c9cfd4", "#fcfcfc", "#8a9199", "#3daee9", "#5ab9ec", "#1b1e20", "#da4453",
                       "#a1a9b1", "#e3eef6", "#31363b", "#eff0f1"};
const Colors kDark = {"#31363b", "#232629", "#eff0f1", "#a1a9b1", "#3b4045", "#2b2f33", "#5e656b",
                      "#4d5359", "#cfd4d8", "#7f8c8d", "#3daee9", "#5ab9ec", "#1b1e20", "#da4453",
                      "#7a8189", "#3b4650", "#eff0f1", "#31363b"};

QString Fill(QString text, const Colors& c)
{
  const struct { const char* key; const char* value; } map[] = {
      {"@window@", c.window},   {"@base@", c.base},       {"@text@", c.text},
      {"@dim@", c.dim},         {"@button@", c.button},   {"@pressed@", c.pressed},
      {"@border@", c.border},   {"@groove@", c.groove},   {"@handleBorder@", c.handleBorder},
      {"@handle@", c.handle},   {"@accentHover@", c.accentHover}, {"@accentText@", c.accentText},
      {"@accent@", c.accent},   {"@danger@", c.danger},   {"@disabled@", c.disabled},
      {"@hover@", c.hover},     {"@tipBase@", c.tipBase}, {"@tipText@", c.tipText}};
  for (const auto& item : map) text.replace(QLatin1String(item.key), QLatin1String(item.value));
  return text;
}
} // namespace

QString BreezeTheme::StyleSheet(bool dark)
{
  static const char* const kTemplate = R"QSS(
QMainWindow, QDialog { background: @window@; }
QLabel { color: @text@; }
QLabel[role="dim"] { color: @dim@; }
QLabel[role="value"] { color: @dim@; }
QLabel[role="title"] { color: @text@; font-weight: 600; }
QFrame#mixer, QFrame#card { background: @base@; border: 1px solid @border@; border-radius: 4px; }
QFrame#side { background: @base@; border: 0; border-right: 1px solid @border@; }
QPushButton { background: @button@; color: @text@; border: 1px solid @border@; border-radius: 3px;
              padding: 4px 14px; min-height: 22px; }
QPushButton:hover { border-color: @accent@; }
QPushButton:pressed { background: @pressed@; }
QPushButton:disabled { color: @disabled@; }
QPushButton[primary="true"] { background: @accent@; color: @accentText@; border-color: @accent@; font-weight: 600; }
QPushButton[primary="true"]:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton[mute="true"] { padding: 1px 8px; min-height: 18px; }
QPushButton[mute="true"]:checked { background: @danger@; color: #ffffff; border-color: @danger@; }
QComboBox { background: @button@; color: @text@; border: 1px solid @border@; border-radius: 3px;
            padding: 3px 8px; min-height: 22px; }
QComboBox:hover { border-color: @accent@; }
QComboBox:disabled { color: @disabled@; }
QComboBox QAbstractItemView { background: @base@; color: @text@; border: 1px solid @border@; outline: 0;
                              selection-background-color: @accent@; selection-color: @accentText@; }
QCheckBox { color: @text@; spacing: 6px; }
QCheckBox:disabled { color: @disabled@; }
QSlider::groove:horizontal { height: 4px; background: @groove@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
QSlider::handle:horizontal { background: @handle@; border: 1px solid @handleBorder@; width: 14px; height: 14px;
                             margin: -6px 0; border-radius: 8px; }
QSlider::handle:horizontal:hover { border-color: @accent@; }
QSlider::groove:vertical { width: 4px; background: @groove@; border-radius: 2px; }
QSlider::handle:vertical { background: @handle@; border: 1px solid @handleBorder@; width: 14px; height: 14px;
                           margin: 0 -6px; border-radius: 8px; }
QSlider::handle:vertical:hover { border-color: @accent@; }
QSlider::handle:disabled { background: @groove@; border-color: @groove@; }
QListWidget { background: @base@; border: 0; outline: 0; color: @text@; }
QListWidget::item { padding: 8px 10px; border-radius: 3px; }
QListWidget::item:hover { background: @hover@; }
QListWidget::item:selected { background: @accent@; color: @accentText@; }
QToolTip { background: @tipBase@; color: @tipText@; border: 1px solid @border@; padding: 3px; }
QMenu { background: @base@; color: @text@; border: 1px solid @border@; }
QMenu::item { padding: 5px 24px; }
QMenu::item:selected { background: @accent@; color: @accentText@; }
QMenu::item:disabled { color: @disabled@; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 6px; }
)QSS";
  return Fill(QString::fromUtf8(kTemplate), dark ? kDark : kLight);
}

void BreezeTheme::Apply(QApplication& app, bool dark)
{
  const Colors& c = dark ? kDark : kLight;
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

  QPalette p;
  p.setColor(QPalette::Window, QColor(c.window));
  p.setColor(QPalette::WindowText, QColor(c.text));
  p.setColor(QPalette::Base, QColor(c.base));
  p.setColor(QPalette::AlternateBase, QColor(c.window));
  p.setColor(QPalette::Text, QColor(c.text));
  p.setColor(QPalette::Button, QColor(c.button));
  p.setColor(QPalette::ButtonText, QColor(c.text));
  p.setColor(QPalette::Highlight, QColor(c.accent));
  p.setColor(QPalette::HighlightedText, QColor(c.accentText));
  p.setColor(QPalette::ToolTipBase, QColor(c.tipBase));
  p.setColor(QPalette::ToolTipText, QColor(c.tipText));
  p.setColor(QPalette::PlaceholderText, QColor(c.dim));
  p.setColor(QPalette::Link, QColor(c.accent));
  p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(c.disabled));
  p.setColor(QPalette::Disabled, QPalette::Text, QColor(c.disabled));
  p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(c.disabled));
  app.setPalette(p);
  app.setStyleSheet(StyleSheet(dark));
}
