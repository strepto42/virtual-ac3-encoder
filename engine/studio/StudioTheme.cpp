#include "StudioTheme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>
#include <QStyleFactory>

#include "BreezeTheme.h"

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
  const char* accent;
  const char* accentText;
  const char* hover;
};

const Colors kModernDark = {"#14161c", "#1c1f27", "#e8eaf0", "#8b92a5", "#262a35", "#1f232d",
                            "#2f3441", "#2a2f3b", "#2dd4bf", "#06201c", "#232837"};
const Colors kModernLight = {"#f2f4f8", "#ffffff", "#1b2030", "#6a7285", "#eef1f6", "#e2e6ee",
                             "#d5dae4", "#d9deea", "#6366f1", "#ffffff", "#e8ecf5"};

QString Fill(QString text, const Colors& c)
{
  const struct { const char* key; const char* value; } map[] = {
      {"@window@", c.window}, {"@base@", c.base},     {"@text@", c.text},       {"@dim@", c.dim},
      {"@button@", c.button}, {"@pressed@", c.pressed}, {"@border@", c.border}, {"@groove@", c.groove},
      {"@accentText@", c.accentText}, {"@accent@", c.accent}, {"@hover@", c.hover}};
  for (const auto& item : map)
    text.replace(QLatin1String(item.key), QLatin1String(item.value));
  return text;
}

// Rules shared by every look. They use palette(...) so they follow whichever palette is active.
const char* const kCommonQss = R"QSS(
QPushButton:checked { background: palette(highlight); color: palette(highlighted-text);
                      border-color: palette(highlight); font-weight: 600; }
QLabel[role="small"] { font-size: 7pt; }
QLabel[role="section"] { font-weight: 600; }
QLineEdit { background: palette(base); color: palette(text); border: 1px solid palette(mid);
            border-radius: 4px; padding: 3px 6px; }
QTabWidget::pane { border: 0; }
QPushButton { padding: 2px 8px; min-height: 16px; }
QComboBox { padding: 1px 8px; min-height: 16px; }
QScrollArea { background: transparent; }
QSplitter::handle:horizontal { background: palette(mid); margin: 0px 2px; }
QSplitter::handle:vertical { background: palette(mid); margin: 2px 0px; }
QSplitter::handle:hover { background: palette(highlight); }
QTabBar::tab { padding: 3px 12px; margin-right: 2px; border: 1px solid palette(mid); border-bottom: none;
               border-top-left-radius: 6px; border-top-right-radius: 6px;
               background: palette(button); color: palette(button-text); }
QTabBar::tab:selected { background: palette(base); color: palette(text); font-weight: 600; }
)QSS";

const char* const kModernQss = R"QSS(
QMainWindow, QDialog, QMessageBox { background: @window@; }
QWidget { color: @text@; }
QLabel { color: @text@; }
QLabel[role="dim"] { color: @dim@; }
QLabel[role="value"] { color: @dim@; }
QFrame#card { background: @base@; border: 1px solid @border@; border-radius: 10px; }
QPushButton { background: @button@; color: @text@; border: 1px solid @border@; border-radius: 8px;
              padding: 2px 8px; min-height: 16px; }
QPushButton:hover { border-color: @accent@; }
QPushButton:pressed { background: @pressed@; }
QPushButton:disabled { color: @dim@; }
QComboBox { background: @button@; color: @text@; border: 1px solid @border@; border-radius: 8px;
            padding: 1px 8px; min-height: 16px; }
QComboBox:hover { border-color: @accent@; }
QComboBox QAbstractItemView { background: @base@; color: @text@; border: 1px solid @border@; outline: 0;
                              selection-background-color: @accent@; selection-color: @accentText@; }
QCheckBox { color: @text@; spacing: 8px; }
QSlider::groove:horizontal { height: 4px; background: @groove@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
QSlider::handle:horizontal { background: @accent@; border: 0; width: 14px; height: 14px; margin: -5px 0;
                             border-radius: 7px; }
QSlider::groove:vertical { width: 4px; background: @groove@; border-radius: 2px; }
QSlider::add-page:vertical { background: @groove@; border-radius: 2px; }
QSlider::handle:vertical { background: @accent@; border: 0; width: 14px; height: 14px; margin: 0 -5px;
                           border-radius: 7px; }
QSlider::handle:disabled { background: @groove@; }
QToolTip { background: @base@; color: @text@; border: 1px solid @border@; padding: 3px; }
QMenu { background: @base@; color: @text@; border: 1px solid @border@; }
QMenu::item { padding: 5px 24px; }
QMenu::item:selected { background: @accent@; color: @accentText@; }
)QSS";

void ApplyModern(QApplication& app, const Colors& c)
{
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
  p.setColor(QPalette::Mid, QColor(c.border));
  p.setColor(QPalette::ToolTipBase, QColor(c.base));
  p.setColor(QPalette::ToolTipText, QColor(c.text));
  p.setColor(QPalette::PlaceholderText, QColor(c.dim));
  p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(c.dim));
  p.setColor(QPalette::Disabled, QPalette::Text, QColor(c.dim));
  p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(c.dim));
  app.setPalette(p);
  app.setStyleSheet(Fill(QString::fromUtf8(kModernQss), c) + QString::fromUtf8(kCommonQss));
}

}  // namespace

namespace StudioTheme {

void Apply(QApplication& app, Look look)
{
  switch (look)
  {
    case Look::ModernDark:
      ApplyModern(app, kModernDark);
      break;
    case Look::ModernLight:
      ApplyModern(app, kModernLight);
      break;
    case Look::QtDark:
    case Look::QtLight:
      BreezeTheme::Apply(app, look == Look::QtDark);
      app.setStyleSheet(app.styleSheet() + QString::fromUtf8(kCommonQss));
      break;
  }
}

const char* LookName(Look look)
{
  switch (look)
  {
    case Look::ModernDark: return "Modern Dark";
    case Look::ModernLight: return "Modern Light";
    case Look::QtDark: return "Qt Dark";
    case Look::QtLight: return "Qt Light";
  }
  return "";
}

}  // namespace StudioTheme
