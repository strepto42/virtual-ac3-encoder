// StudioTheme.h — the four looks of DSP Studio, one functional base:
//   Modern Dark / Modern Light  (rounded cards, teal / indigo accent)
//   Qt Dark / Qt Light          (KDE Breeze, the same look as the Control Center panels)
// Switching a look only changes palette and style sheet; processing and sound are never touched.
#pragma once

class QApplication;

namespace StudioTheme {

enum class Look
{
  ModernDark = 0,
  ModernLight = 1,
  QtDark = 2,
  QtLight = 3
};

constexpr int kLookCount = 4;

void Apply(QApplication& app, Look look);
const char* LookName(Look look);

}  // namespace StudioTheme
