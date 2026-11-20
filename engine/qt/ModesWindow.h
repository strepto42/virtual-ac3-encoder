// ModesWindow.h — variant B: mode list on the left, devices / sliders / options on the right.
#pragma once

#include "PanelWindow.h"

class QListWidget;

class ModesWindow : public PanelWindow
{
public:
  explicit ModesWindow(bool startHidden);

protected:
  void buildLayout(QWidget* central) override;
  Qt::Orientation sliderOrientation() const override { return Qt::Horizontal; }
  QString variantName() const override { return QStringLiteral("B"); }
  void modeChangedHook(int mode) override;

private:
  QListWidget* modeList_ = nullptr;
};
