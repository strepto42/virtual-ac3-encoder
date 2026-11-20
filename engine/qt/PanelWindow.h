// PanelWindow.h — behaviour shared by both control-panel variants: engine start/stop/apply,
// per-channel gains, tray icon, autostart, supervision, themes, control-center.ini. A variant
// (MixerWindow = A, ModesWindow = B) only decides how the shared widgets are laid out.
#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include <array>

#include "Backend.h"

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QMenu;
class QPushButton;
class QSlider;
class QSystemTrayIcon;

struct ModeInfo
{
  const char* title;
  const char* subtitle;
  const char* badge;
};
extern const ModeInfo kModes[cc::kModeCount];
extern const char* const kChannelNames[6];

class PanelWindow : public QMainWindow
{
public:
  explicit PanelWindow(bool startHidden);
  ~PanelWindow() override;

  void startup();     // starts the engine (silently when launched with --minimized)
  void showPanel();   // bring the window to the front (also called when a second instance starts)

protected:
  // Called by the final subclass constructor, after the vtable is complete.
  void init();

  virtual void buildLayout(QWidget* central) = 0;
  virtual Qt::Orientation sliderOrientation() const = 0;
  virtual QString variantName() const = 0;
  virtual void modeChangedHook(int /*mode*/) {}

  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;

  // Shared widgets: created in init(), placed (or hidden) by the variant.
  QComboBox* modeCombo_ = nullptr;
  QComboBox* inputCombo_ = nullptr;
  QComboBox* outputCombo_ = nullptr;
  QComboBox* stereoCombo_ = nullptr;
  QComboBox* delayCombo_ = nullptr;
  QLabel* stereoLabel_ = nullptr;
  QLabel* delayLabel_ = nullptr;
  QLabel* statusLabel_ = nullptr;
  QLabel* stateDot_ = nullptr;
  QLabel* stateText_ = nullptr;
  std::array<QSlider*, 6> sliders_{};
  std::array<QLabel*, 6> values_{};
  std::array<QPushButton*, 6> mutes_{};
  QPushButton* applyBtn_ = nullptr;
  QPushButton* startStopBtn_ = nullptr;
  QPushButton* resetBtn_ = nullptr;
  QPushButton* themeBtn_ = nullptr;
  QPushButton* dspBtn_ = nullptr;   // launches DSP Studio
  QCheckBox* autostartCheck_ = nullptr;

  cc::Backend backend_;

private:
  void createWidgets();
  void connectSignals();
  void loadSettingsIntoUi();
  void populateInput(int mode, const std::wstring& preferredId, bool preserveCurrent);
  cc::Settings collectSettings() const;
  void updateModeControls();
  void updateValueLabel(int channel);
  void setStatus(const std::wstring& text);

  bool startEngine();
  void userStart();
  void userStop();
  void onApply();
  void onToggleAutostart(bool enable);
  void openDspStudio();
  void supervise();
  void onTick();
  void refreshState(bool force);

  void setupTray();
  void quitPanel();
  void applyTheme(bool dark);
  void applyUiIni(bool force);

  bool startHidden_;
  bool silent_ = false;           // no modal dialogs (autostart at logon)
  bool matrixUpmix_ = false;      // upmix_algo=matrix in the config: no FFT delay to choose
  QString delayTip_;
  bool quitting_ = false;
  bool userStopped_ = false;
  bool dark_ = true;
  bool inputUsesRender_ = false;
  int lastRunning_ = -1;
  int restartAttempts_ = 0;
  int iniTick_ = 0;
  qint64 nextRestartMs_ = 0;
  qint64 engineStartMs_ = 0;
  bool announcedGiveUp_ = false;
  std::array<int, 6> lastGain_{{0, 0, 0, 0, 0, 0}};

  QElapsedTimer clock_;
  QTimer tick_;
  QTimer gainTimer_;

  QSystemTrayIcon* tray_ = nullptr;
  QAction* actStartStop_ = nullptr;
  QAction* actAutostart_ = nullptr;
};
