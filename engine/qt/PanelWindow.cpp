#include "PanelWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSystemTrayIcon>

#include "BreezeTheme.h"

const ModeInfo kModes[cc::kModeCount] = {
    {"Dolby Digital 5.1 без апмикса", "Захват CABLE Output, 640 кбит/с", "DD"},
    {"Dolby Digital 5.1 с апмиксом", "Захват, стерео раскладывается в 5.1", "DD"},
    {"PCM Stereo 2.0 (обычный S/PDIF)", "Только FL/FR, без кодирования", "PCM"},
    {"DTS 5.1 Core - Toslink S/PDIF", "Захват CABLE Output, 768 кбит/с", "DTS"},
    {"PCM 5.1 -> DTS 5.1", "Loopback CABLE Input", "DTS"},
    {"PCM 5.1 -> Dolby Digital 5.1", "Loopback CABLE Input", "DD"},
    {"DTS 5.1 (бит-стрим) -> Dolby Digital 5.1", "Loopback, DTS декодируется", "DD"},
};
const char* const kChannelNames[6] = {"FL", "FR", "FC", "LFE", "SL", "SR"};

namespace {
constexpr int kMaxAutoRestarts = 10;

QIcon DotIcon(const QColor& color)
{
  QPixmap pixmap(32, 32);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(color);
  painter.setPen(QPen(QColor(30, 30, 30), 2));
  painter.drawEllipse(QRectF(3, 3, 26, 26));
  painter.end();
  return QIcon(pixmap);
}

QString Q(const std::wstring& text) { return QString::fromStdWString(text); }
} // namespace

PanelWindow::PanelWindow(bool startHidden) : startHidden_(startHidden) { clock_.start(); }

PanelWindow::~PanelWindow() = default;

// --------------------------------------------------------------------------- construction
void PanelWindow::init()
{
  backend_.EnsureUiIni();
  backend_.RefreshDevices();
  createWidgets();

  auto* central = new QWidget(this);
  setCentralWidget(central);
  buildLayout(central);
  setWindowTitle(QStringLiteral("AC3 / DTS Control Center  [v3 Qt ") + variantName() + QStringLiteral("]"));
  setWindowIcon(DotIcon(QColor(46, 184, 76)));

  connectSignals();
  loadSettingsIntoUi();
  setupTray();
  applyUiIni(true);

  gainTimer_.setSingleShot(true);
  connect(&gainTimer_, &QTimer::timeout, this, [this]() { backend_.WriteControl(false); });
  tick_.setInterval(500);
  connect(&tick_, &QTimer::timeout, this, [this]() { onTick(); });
  tick_.start();
}

void PanelWindow::createWidgets()
{
  modeCombo_ = new QComboBox(this);
  for (const ModeInfo& mode : kModes) modeCombo_->addItem(QString::fromUtf8(mode.title));
  inputCombo_ = new QComboBox(this);
  outputCombo_ = new QComboBox(this);

  stereoLabel_ = new QLabel(QStringLiteral("Стерео в 5.1:"), this);
  stereoCombo_ = new QComboBox(this);
  stereoCombo_->addItems({QStringLiteral("Авто"), QStringLiteral("Всегда"), QStringLiteral("Выкл")});
  stereoCombo_->setToolTip(QStringLiteral("Режимы PCM 5.1: как поступать со стерео-источником (браузер, плеер)."));

  delayLabel_ = new QLabel(QStringLiteral("Задержка апмикса:"), this);
  delayCombo_ = new QComboBox(this);
  delayCombo_->addItems({QStringLiteral("Качество (~87 мс)"), QStringLiteral("Баланс (~44 мс)"),
                         QStringLiteral("Мин. задержка (~22 мс)")});
  delayCombo_->setToolTip(QStringLiteral("Окно FFT апмиксера: меньше окно = меньше задержка, но грубее центр."));

  statusLabel_ = new QLabel(QStringLiteral("Готов."), this);
  statusLabel_->setWordWrap(true);
  statusLabel_->setProperty("role", "dim");
  stateDot_ = new QLabel(this);
  stateDot_->setFixedSize(10, 10);
  stateText_ = new QLabel(QStringLiteral("Остановлен"), this);
  stateText_->setProperty("role", "title");

  for (int i = 0; i < 6; ++i)
  {
    sliders_[i] = new QSlider(sliderOrientation(), this);
    sliders_[i]->setRange(-60, 0);
    sliders_[i]->setSingleStep(1);
    sliders_[i]->setPageStep(3);
    values_[i] = new QLabel(QStringLiteral("0 dB"), this);
    values_[i]->setProperty("role", "value");
    mutes_[i] = new QPushButton(QStringLiteral("M"), this);
    mutes_[i]->setCheckable(true);
    mutes_[i]->setProperty("mute", true);
    mutes_[i]->setToolTip(QStringLiteral("Заглушить канал"));
  }

  applyBtn_ = new QPushButton(QStringLiteral("Применить"), this);
  applyBtn_->setProperty("primary", true);
  startStopBtn_ = new QPushButton(QStringLiteral("Запустить"), this);
  resetBtn_ = new QPushButton(QStringLiteral("Сбросить уровни"), this);
  themeBtn_ = new QPushButton(QStringLiteral("Тема"), this);
  dspBtn_ = new QPushButton(QStringLiteral("DSP Studio…"), this);
  dspBtn_->setToolTip(QStringLiteral("Эквалайзер, компрессор и уровни"));
  autostartCheck_ = new QCheckBox(QStringLiteral("Запускать вместе с Windows (в трее)"), this);
}

void PanelWindow::connectSignals()
{
  connect(modeCombo_, &QComboBox::currentIndexChanged, this, [this](int mode) {
    if (mode < 0) return;
    populateInput(mode, std::wstring(), true);
    updateModeControls();
    modeChangedHook(mode);
  });

  for (int i = 0; i < 6; ++i)
  {
    connect(sliders_[i], &QSlider::valueChanged, this, [this, i](int value) {
      backend_.gains()[static_cast<size_t>(i)] = value;
      if (value > -60) lastGain_[static_cast<size_t>(i)] = value;
      updateValueLabel(i);
      {
        QSignalBlocker blocker(mutes_[i]);
        mutes_[i]->setChecked(value <= -60);
      }
      gainTimer_.start(40);  // coalesce fast slider moves into one write
    });
    connect(mutes_[i], &QPushButton::toggled, this, [this, i](bool on) {
      sliders_[i]->setValue(on ? -60 : lastGain_[static_cast<size_t>(i)]);
    });
  }

  connect(resetBtn_, &QPushButton::clicked, this, [this]() {
    for (QSlider* slider : sliders_) slider->setValue(0);
  });
  connect(applyBtn_, &QPushButton::clicked, this, [this]() { onApply(); });
  connect(startStopBtn_, &QPushButton::clicked, this, [this]() {
    if (backend_.EngineRunning()) userStop();
    else userStart();
  });
  connect(autostartCheck_, &QCheckBox::clicked, this, [this](bool on) { onToggleAutostart(on); });
  connect(dspBtn_, &QPushButton::clicked, this, [this]() { openDspStudio(); });
  connect(themeBtn_, &QPushButton::clicked, this, [this]() {
    const bool dark = !dark_;
    applyTheme(dark);
    backend_.UiIniSet(L"theme", dark ? L"dark" : L"light");
  });
}

// --------------------------------------------------------------------------- settings <-> UI
void PanelWindow::loadSettingsIntoUi()
{
  const cc::Settings settings = backend_.LoadSettings();
  matrixUpmix_ = settings.matrixUpmix;
  if (delayTip_.isEmpty())
    delayTip_ = delayCombo_->toolTip();
  delayCombo_->setToolTip(matrixUpmix_ ? QStringLiteral("Включён матричный апмикс (upmix_algo=matrix): задержки апмикса нет, окно FFT не используется. Настройки матрицы: DSP Studio, вкладка «Апмикс».")
                                       : delayTip_);

  {
    QSignalBlocker blocker(outputCombo_);
    outputCombo_->clear();
    for (const cc::Device& device : backend_.renderDevices()) outputCombo_->addItem(Q(device.name));
    int index = cc::Backend::FindById(backend_.renderDevices(), settings.outputId);
    if (index < 0) index = backend_.PreferredOutputIndex();
    if (index >= 0) outputCombo_->setCurrentIndex(index);
  }
  {
    QSignalBlocker blocker(modeCombo_);
    modeCombo_->setCurrentIndex(settings.mode);
  }
  populateInput(settings.mode, settings.inputId, false);
  stereoCombo_->setCurrentIndex(settings.stereoUpmix);
  delayCombo_->setCurrentIndex(settings.delaySel);

  backend_.LoadGains();
  for (int i = 0; i < 6; ++i)
  {
    const int value = backend_.gains()[static_cast<size_t>(i)];
    lastGain_[static_cast<size_t>(i)] = value > -60 ? value : 0;
    QSignalBlocker blocker(sliders_[i]);
    sliders_[i]->setValue(value);
    updateValueLabel(i);
    QSignalBlocker muteBlocker(mutes_[i]);
    mutes_[i]->setChecked(value <= -60);
  }
  {
    QSignalBlocker blocker(autostartCheck_);
    autostartCheck_->setChecked(backend_.AutostartEnabled());
  }
  updateModeControls();
  modeChangedHook(settings.mode);
}

void PanelWindow::populateInput(int mode, const std::wstring& preferredId, bool preserveCurrent)
{
  const bool useRender = cc::Backend::IsLoopbackMode(mode);
  std::wstring selected = preferredId;
  if (preserveCurrent && inputUsesRender_ == useRender)
  {
    const auto& old = inputUsesRender_ ? backend_.renderDevices() : backend_.captureDevices();
    const int index = inputCombo_->currentIndex();
    if (index >= 0 && static_cast<size_t>(index) < old.size()) selected = old[static_cast<size_t>(index)].id;
  }
  const auto& devices = useRender ? backend_.renderDevices() : backend_.captureDevices();
  QSignalBlocker blocker(inputCombo_);
  inputCombo_->clear();
  for (const cc::Device& device : devices) inputCombo_->addItem(Q(device.name));
  int index = cc::Backend::FindById(devices, selected);
  if (index < 0) index = cc::Backend::FindBySubstring(devices, useRender ? L"CABLE Input" : L"CABLE Output");
  if (index < 0 && !devices.empty()) index = 0;
  if (index >= 0) inputCombo_->setCurrentIndex(index);
  inputUsesRender_ = useRender;
}

cc::Settings PanelWindow::collectSettings() const
{
  cc::Settings settings;
  settings.mode = modeCombo_->currentIndex();
  const auto& inputs = cc::Backend::IsLoopbackMode(settings.mode) ? backend_.renderDevices() : backend_.captureDevices();
  const int in = inputCombo_->currentIndex();
  if (in >= 0 && static_cast<size_t>(in) < inputs.size()) settings.inputId = inputs[static_cast<size_t>(in)].id;
  const auto& outputs = backend_.renderDevices();
  const int out = outputCombo_->currentIndex();
  if (out >= 0 && static_cast<size_t>(out) < outputs.size()) settings.outputId = outputs[static_cast<size_t>(out)].id;
  settings.stereoUpmix = stereoCombo_->currentIndex();
  settings.delaySel = delayCombo_->currentIndex();
  return settings;
}

void PanelWindow::updateModeControls()
{
  const int mode = modeCombo_->currentIndex();
  if (mode < 0) return;
  const bool pcm = mode == 2;
  for (int i = 0; i < 6; ++i)
  {
    const bool enabled = !pcm || i < 2;  // PCM stereo has only FL / FR
    sliders_[i]->setEnabled(enabled);
    mutes_[i]->setEnabled(enabled);
    values_[i]->setEnabled(enabled);
  }
  const bool loopbackStereo = cc::Backend::IsLoopbackMode(mode);
  stereoCombo_->setEnabled(loopbackStereo);
  stereoLabel_->setEnabled(loopbackStereo);
  const bool upmix = mode != 0 && mode != 2;
  delayCombo_->setEnabled(upmix && !matrixUpmix_);
  delayLabel_->setEnabled(upmix && !matrixUpmix_);
  setStatus(std::wstring(QString::fromUtf8(kModes[mode].title).toStdWString()) + L": " +
            QString::fromUtf8(kModes[mode].subtitle).toStdWString() + L". Нажми «Применить».");
}

void PanelWindow::updateValueLabel(int channel)
{
  const int value = sliders_[static_cast<size_t>(channel)]->value();
  values_[static_cast<size_t>(channel)]->setText(value <= -60 ? QStringLiteral("MUTE") : QStringLiteral("%1 dB").arg(value));
}

void PanelWindow::setStatus(const std::wstring& text) { statusLabel_->setText(Q(text)); }

// --------------------------------------------------------------------------- engine control
bool PanelWindow::startEngine()
{
  std::wstring message;
  const cc::Settings settings = collectSettings();
  cc::StartResult result = backend_.StartEngine(settings, &message);
  if (result == cc::StartResult::ForeignRunning)
  {
    if (silent_)
    {
      setStatus(message + L" Закрой его вручную или запусти панель без --minimized.");
      return false;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("AC3 Control Center"),
        QStringLiteral("Уже работает engine.exe из другой папки (старая сборка) и занимает цифровой выход.\n"
                       "Закрыть его и запустить кодировщик из этой папки?"));
    if (answer != QMessageBox::Yes)
    {
      setStatus(L"Запуск отменён: работает engine.exe из другой папки.");
      return false;
    }
    backend_.TerminateProcesses(backend_.ForeignEngines());
    result = backend_.StartEngine(settings, &message);
  }
  setStatus(message);
  if (result == cc::StartResult::Failed || result == cc::StartResult::ForeignRunning)
  {
    if (!silent_ && result == cc::StartResult::Failed)
      QMessageBox::warning(this, QStringLiteral("AC3 Control Center"), Q(message));
    return false;
  }
  engineStartMs_ = clock_.elapsed();
  return true;
}

void PanelWindow::userStart()
{
  userStopped_ = false;
  restartAttempts_ = 0;
  nextRestartMs_ = 0;
  startEngine();
  refreshState(true);
}

void PanelWindow::userStop()
{
  userStopped_ = true;
  std::wstring message;
  backend_.StopEngine(&message);
  setStatus(message);
  refreshState(true);
}

void PanelWindow::onApply()
{
  const cc::Settings settings = collectSettings();
  std::wstring error;
  if (!backend_.SaveSettings(settings, &error))
  {
    QMessageBox::warning(this, QStringLiteral("AC3 Control Center"), Q(error));
    return;
  }
  if (!backend_.EngineRunning())
  {
    setStatus(L"Настройки сохранены. Нажми «Запустить», чтобы применить.");
    return;
  }
  const auto answer = QMessageBox::question(
      this, QStringLiteral("AC3 Control Center"),
      QStringLiteral("Для смены режима кодировщик нужно перезапустить (звук прервётся на секунду). Продолжить?"));
  if (answer != QMessageBox::Yes)
  {
    setStatus(L"Настройки сохранены; перезапуск не выполнен.");
    return;
  }
  std::wstring message;
  backend_.StopEngine(&message);
  if (startEngine()) setStatus(L"Настройки применены, кодировщик перезапущен.");
  refreshState(true);
}

void PanelWindow::supervise()
{
  if (userStopped_) return;
  if (backend_.EngineRunning())
  {
    if (clock_.elapsed() - engineStartMs_ > 30000) restartAttempts_ = 0;
    nextRestartMs_ = 0;
    announcedGiveUp_ = false;
    return;
  }
  if (restartAttempts_ >= kMaxAutoRestarts)
  {
    if (!announcedGiveUp_)
    {
      announcedGiveUp_ = true;
      setStatus(L"Автоперезапуск остановлен после " + std::to_wstring(kMaxAutoRestarts) +
                L" попыток. Проверь engine.log и нажми «Запустить».");
    }
    return;
  }
  const qint64 now = clock_.elapsed();
  if (nextRestartMs_ == 0)
  {
    nextRestartMs_ = now + 5000;
    return;
  }
  if (now < nextRestartMs_) return;
  nextRestartMs_ = 0;
  ++restartAttempts_;
  setStatus(L"Кодировщик не работает: автоперезапуск (попытка " + std::to_wstring(restartAttempts_) + L" из " +
            std::to_wstring(kMaxAutoRestarts) + L")…");
  const bool previousSilent = silent_;
  silent_ = true;
  startEngine();
  silent_ = previousSilent;
}

void PanelWindow::onTick()
{
  unsigned long code = 0;
  if (backend_.PollExit(&code))
    setStatus(L"Движок остановлен (код " + std::to_wstring(code) + L"). Подробности: engine.log");
  supervise();
  refreshState(false);
  if (++iniTick_ % 4 == 0) applyUiIni(false);
}

void PanelWindow::refreshState(bool force)
{
  const int running = backend_.EngineRunning() ? 1 : 0;
  if (!force && running == lastRunning_) return;
  lastRunning_ = running;
  stateDot_->setStyleSheet(QStringLiteral("background:%1; border-radius:5px;")
                               .arg(running ? QStringLiteral("#27ae60") : QStringLiteral("#da4453")));
  stateText_->setText(running ? QStringLiteral("Работает") : QStringLiteral("Остановлен"));
  startStopBtn_->setText(running ? QStringLiteral("Остановить") : QStringLiteral("Запустить"));
  if (tray_)
  {
    tray_->setIcon(DotIcon(running ? QColor(46, 184, 76) : QColor(205, 60, 60)));
    tray_->setToolTip(running ? QStringLiteral("AC3 / DTS Control Center: кодировщик работает")
                              : QStringLiteral("AC3 / DTS Control Center: кодировщик остановлен"));
    actStartStop_->setText(running ? QStringLiteral("Остановить кодировщик") : QStringLiteral("Запустить кодировщик"));
  }
}

// --------------------------------------------------------------------------- DSP Studio

// DSP Studio is a separate exe next to the panel. If it is already running, its single-instance check
// just brings the existing window to the front.
void PanelWindow::openDspStudio()
{
  const QString exe = QCoreApplication::applicationDirPath() + QStringLiteral("/VirtualAc3-DSP-Studio.exe");
  if (!QFileInfo::exists(exe))
  {
    if (!silent_)
      QMessageBox::information(this, windowTitle(),
                               QStringLiteral("VirtualAc3-DSP-Studio.exe не найден рядом с панелью.\nСобери проект через build.ps1."));
    return;
  }
  QProcess::startDetached(exe, QStringList());
}

// --------------------------------------------------------------------------- autostart, tray
void PanelWindow::onToggleAutostart(bool enable)
{
  if (!backend_.SetAutostart(enable))
  {
    QMessageBox::warning(this, QStringLiteral("AC3 Control Center"),
                         QStringLiteral("Не удалось изменить автозапуск (папка Startup)."));
  }
  else if (enable && !silent_)
  {
    const std::vector<std::wstring> others = backend_.OtherPanelShortcuts();
    if (!others.empty())
    {
      QString text = QStringLiteral("В автозагрузке найден другой ярлык панели управления:\n\n");
      for (const std::wstring& path : others)
      {
        const size_t slash = path.find_last_of(L"\\/");
        text += QStringLiteral("  • ") + Q(slash == std::wstring::npos ? path : path.substr(slash + 1)) + QStringLiteral("\n");
      }
      text += QStringLiteral("\nУдалить его, чтобы при входе в Windows не запускались две панели и два кодировщика?");
      if (QMessageBox::question(this, QStringLiteral("AC3 Control Center"), text) == QMessageBox::Yes)
        for (const std::wstring& path : others) cc::Backend::RemoveFile(path);
    }
  }
  const bool actual = backend_.AutostartEnabled();
  QSignalBlocker blocker(autostartCheck_);
  autostartCheck_->setChecked(actual);
  if (actAutostart_) actAutostart_->setChecked(actual);
}

void PanelWindow::setupTray()
{
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
  tray_ = new QSystemTrayIcon(this);
  auto* menu = new QMenu(this);

  QAction* show = menu->addAction(QStringLiteral("Показать панель"));
  connect(show, &QAction::triggered, this, [this]() { showPanel(); });
  menu->addSeparator();
  actStartStop_ = menu->addAction(QStringLiteral("Запустить кодировщик"));
  connect(actStartStop_, &QAction::triggered, this, [this]() {
    if (backend_.EngineRunning()) userStop();
    else userStart();
  });
  actAutostart_ = menu->addAction(QStringLiteral("Запускать вместе с Windows"));
  actAutostart_->setCheckable(true);
  actAutostart_->setChecked(backend_.AutostartEnabled());
  connect(actAutostart_, &QAction::triggered, this, [this](bool on) { onToggleAutostart(on); });
  menu->addSeparator();
  QAction* exit = menu->addAction(QStringLiteral("Выход (кодировщик продолжит работать)"));
  connect(exit, &QAction::triggered, this, [this]() { quitPanel(); });

  tray_->setContextMenu(menu);
  connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showPanel();
  });
  tray_->setIcon(DotIcon(QColor(205, 60, 60)));
  tray_->show();
}

void PanelWindow::quitPanel()
{
  quitting_ = true;
  backend_.DetachEngine();  // the engine is a separate process and keeps running
  if (tray_) tray_->hide();
  QApplication::quit();
}

void PanelWindow::showPanel()
{
  show();
  setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
  raise();
  activateWindow();
}

void PanelWindow::closeEvent(QCloseEvent* event)
{
  if (quitting_ || !tray_)
  {
    quitting_ = true;
    backend_.DetachEngine();
    event->accept();
    QApplication::quit();
    return;
  }
  event->ignore();
  hide();  // the window only hides in the tray; panel and engine keep running
}

void PanelWindow::changeEvent(QEvent* event)
{
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange && isMinimized() && tray_)
    QTimer::singleShot(0, this, [this]() { hide(); });  // minimise to tray
}

// --------------------------------------------------------------------------- theme, ini, startup
void PanelWindow::applyTheme(bool dark)
{
  dark_ = dark;
  BreezeTheme::Apply(*qApp, dark);
  themeBtn_->setText(dark ? QStringLiteral("Тема: тёмная") : QStringLiteral("Тема: светлая"));
  lastRunning_ = -1;  // re-apply the state dot colour
  refreshState(true);
}

void PanelWindow::applyUiIni(bool force)
{
  const bool changed = backend_.UiIniChanged();
  if (!force && !changed) return;
  const bool showDelay = backend_.UiIniInt(L"show_upmix_latency_menu", 1) != 0;
  delayLabel_->setVisible(showDelay);
  delayCombo_->setVisible(showDelay);
  const bool dark = backend_.UiIniString(L"theme", L"dark") != L"light";
  if (force || dark != dark_) applyTheme(dark);
}

void PanelWindow::startup()
{
  silent_ = startHidden_;
  userStopped_ = false;
  startEngine();
  silent_ = false;
  refreshState(true);
}
