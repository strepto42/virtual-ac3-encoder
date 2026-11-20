#include "StudioWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QKeySequence>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWindow>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

#include "AutoProfilesDialog.h"
#include "StudioTheme.h"
#include "dsp/ConfEdit.h"
#include "Widgets.h"

namespace {

const char* const kChannelTitles[vac3dsp::kMaxChannels] = {"L", "R", "C", "LFE", "Ls", "Rs"};

// A panel that can be squeezed below its natural size: it scrolls instead of forcing the window to stay big.
QWidget* Scrolled(QWidget* inner)
{
  auto* area = new QScrollArea;
  area->setWidgetResizable(true);
  area->setFrameShape(QFrame::NoFrame);
  area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  area->setWidget(inner);
  return area;
}

QFrame* MakeCard()
{
  auto* card = new QFrame;
  card->setObjectName(QStringLiteral("card"));
  return card;
}

QLabel* MakeLabel(const QString& text, const char* role = nullptr)
{
  auto* l = new QLabel(text);
  if (role)
    l->setProperty("role", role);
  return l;
}

QString DbText(float db)
{
  return QString::asprintf("%+.1f", static_cast<double>(db));
}

}  // namespace

QString StudioWindow::AppDir() { return QCoreApplication::applicationDirPath(); }
QString StudioWindow::ConfPath() { return AppDir() + QStringLiteral("/virtual-ac3-encoder.conf"); }
QString StudioWindow::UpmixPresetsDir() { return AppDir() + QStringLiteral("/upmix-presets"); }
QString StudioWindow::PresetsDir() { return AppDir() + QStringLiteral("/dsp-presets"); }

StudioWindow::StudioWindow()
    : file_(std::filesystem::path(QDir::toNativeSeparators(AppDir() + QStringLiteral("/dsp-settings.json")).toStdWString())),
      settingsPath_(QDir::toNativeSeparators(AppDir() + QStringLiteral("/dsp-settings.json")).toStdWString())
{
  setWindowTitle(QStringLiteral("VirtualAc3 DSP Studio"));
  // Everything sits in a scroll area: the window can be made smaller than its content (scroll bars appear
  // only then), so it never has to be taller than the screen.
  auto* content = new QWidget;
  buildUi(content);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(content);
  setCentralWidget(scroll);

  // initial state: whatever is in dsp-settings.json (defaults when there is no file yet)
  vac3dsp::DspParams loaded;
  std::string err;
  if (file_.Poll(loaded, &err) == vac3dsp::SettingsFile::Result::Loaded)
    model_.params = loaded;
  else if (!err.empty())
    setStatus(QStringLiteral("dsp-settings.json не удалось прочитать: ") + QString::fromStdString(err));

  history_.Reset(model_.params);
  specInShown_.assign(static_cast<size_t>(vac3dsp::kSpecBands), vac3dsp::kSpecFloorDb);
  specOutShown_.assign(static_cast<size_t>(vac3dsp::kSpecBands), vac3dsp::kSpecFloorDb);

  for (auto& row : specInCh_)
    for (float& v : row)
      v = vac3dsp::kSpecFloorDb;
  for (auto& row : specOutCh_)
    for (float& v : row)
      v = vac3dsp::kSpecFloorDb;
  loadPresetList();
  loadUpmixPresetList();
  refreshUi();

  QSettings ini(AppDir() + QStringLiteral("/dsp-studio.ini"), QSettings::IniFormat);
  const QByteArray geometry = ini.value(QStringLiteral("geometry")).toByteArray();
  if (!geometry.isEmpty())
    restoreGeometry(geometry);
  else
  {
    const QScreen* sc = QGuiApplication::primaryScreen();
    const QRect avail = sc ? sc->availableGeometry() : QRect(0, 0, 1280, 800);
    resize(std::min(1280, avail.width() - 40), std::min(780, avail.height() - 80));
  }
  setMinimumSize(640, 360);   // anything smaller than the content scrolls
  {
    const struct { QSplitter* split; const char* key; } splits[] = {
        {mainSplit_, "splitMain"}, {rightSplit_, "splitRight"}, {eqSplit_, "splitEq"}};
    for (const auto& sp : splits)
    {
      const QStringList parts = ini.value(QString::fromLatin1(sp.key)).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
      QList<int> sizes;
      for (const QString& part : parts)
        sizes << part.toInt();
      if (sizes.size() == sp.split->count())
        sp.split->setSizes(sizes);
    }
  }
  {
    const struct { QPushButton* btn; const char* key; } views[] = {{showCurveBtn_, "showCurve"},
                                                                    {showFadersBtn_, "showFaders"},
                                                                    {showSpectrumBtn_, "showSpectrum"},
                                                                    {showCompBtn_, "showCompressor"},
                                                                    {showLevelsBtn_, "showLevels"}};
    for (const auto& v : views)
    {
      const QSignalBlocker blocker(v.btn);
      v.btn->setChecked(ini.value(QString::fromLatin1(v.key), true).toBool());
    }
  }
  applyPanelVisibility();

  saveTimer_.setSingleShot(true);
  saveTimer_.setInterval(80);
  connect(&saveTimer_, &QTimer::timeout, this, [this]() { doSave(); });
  filePollTimer_.setInterval(500);
  connect(&filePollTimer_, &QTimer::timeout, this, [this]() { pollFile(); });
  filePollTimer_.start();
  meterTimer_.setInterval(33);
  connect(&meterTimer_, &QTimer::timeout, this, [this]() { pollMeters(); });
  sinceTick_.start();
  meterTimer_.start();
  // an editing gesture (a drag, a series of clicks) is one undo step once it has been quiet for a moment
  undoTimer_.setSingleShot(true);
  undoTimer_.setInterval(600);
  connect(&undoTimer_, &QTimer::timeout, this, [this]() { commitHistory(); });
  setupShortcuts();
  updateUndoButtons();
}

void StudioWindow::showStudio()
{
  showNormal();
  raise();
  activateWindow();
}

// A window restored from an old position (another monitor, another resolution, an older and taller layout)
// must never end up bigger than the visible desktop or outside it.
// Keeps a normal window inside the visible desktop (smaller than its content is fine, it scrolls). There is no
// fixed maximum size: Windows would grey out the "maximize" button for such a window. Instead the size is
// corrected after the fact: at start, when the window is resized or changes monitor.
void StudioWindow::fitToScreen()
{
  const QScreen* sc = screen() ? screen() : QGuiApplication::primaryScreen();
  if (!sc)
    return;
  const QRect avail = sc->availableGeometry();
  if (isMaximized() || isFullScreen())
  {
    // A maximized window is as big as the desktop by definition (its frame has invisible borders, so only the
    // client area is compared). Only a clearly oversized one is taken out of that state.
    if (geometry().height() <= avail.height() + 40 && geometry().width() <= avail.width() + 40)
      return;
    showNormal();
    QTimer::singleShot(0, this, [this]() { fitToScreen(); });
    return;
  }
  const QRect frame = frameGeometry();
  const int extraW = frame.width() - width();
  const int extraH = frame.height() - height();
  if (frame.width() > avail.width() + 16 || frame.height() > avail.height() + 16)
    resize(std::min(width(), avail.width() - extraW), std::min(height(), avail.height() - extraH));
  const QRect now = frameGeometry();
  const int x = std::min(std::max(now.x(), avail.left()), avail.right() - now.width() + 1);
  const int y = std::min(std::max(now.y(), avail.top()), avail.bottom() - now.height() + 1);
  if (x != now.x() || y != now.y())
    move(x, y);
}

void StudioWindow::resizeEvent(QResizeEvent* event)
{
  QMainWindow::resizeEvent(event);
  if (fitPending_ || isMaximized() || isFullScreen())
    return;
  fitPending_ = true;   // once the user stops dragging: a window that grew beyond the desktop is pulled back
  QTimer::singleShot(200, this, [this]() {
    fitPending_ = false;
    fitToScreen();
  });
}

void StudioWindow::resetWindowSize()
{
  showNormal();
  const QScreen* sc = screen() ? screen() : QGuiApplication::primaryScreen();
  const QRect avail = sc ? sc->availableGeometry() : QRect(0, 0, 1280, 800);
  resize(std::min(1280, avail.width() - 40), std::min(780, avail.height() - 80));
  move(avail.left() + std::max(0, (avail.width() - width()) / 2), avail.top() + 10);
  QTimer::singleShot(0, this, [this]() { fitToScreen(); });
  setStatus(QStringLiteral("Размер окна сброшен (Ctrl+0)"));
}

void StudioWindow::showEvent(QShowEvent* event)
{
  QMainWindow::showEvent(event);
  if (!fitted_)
  {
    fitted_ = true;
    QTimer::singleShot(0, this, [this]() {   // after the frame exists, so its size is known
      fitToScreen();
      if (windowHandle())
        connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen*) {
          QTimer::singleShot(0, this, [this]() { fitToScreen(); });
        });
    });
  }
}

void StudioWindow::closeEvent(QCloseEvent* event)
{
  if (abBypass_)
  {
    abBypass_ = false;   // A/B was on: put the real settings back in the file before leaving
    saveTimer_.stop();
    doSave();
  }
  else if (saveTimer_.isActive())
  {
    saveTimer_.stop();
    doSave();
  }
  QSettings ini(AppDir() + QStringLiteral("/dsp-studio.ini"), QSettings::IniFormat);
  ini.setValue(QStringLiteral("geometry"), saveGeometry());
  const struct { QSplitter* split; const char* key; } splits[] = {
      {mainSplit_, "splitMain"}, {rightSplit_, "splitRight"}, {eqSplit_, "splitEq"}};
  for (const auto& sp : splits)
  {
    const QList<int> sizes = sp.split->sizes();
    bool allOpen = !sizes.isEmpty();
    QStringList parts;
    for (int v : sizes)
    {
      allOpen = allOpen && v > 0;
      parts << QString::number(v);
    }
    if (allOpen)
      ini.setValue(QString::fromLatin1(sp.key), parts.join(QLatin1Char(',')));
  }
  event->accept();
}

// ------------------------------------------------------------------------------------ layout
void StudioWindow::buildUi(QWidget* central)
{
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(10, 8, 10, 8);
  root->setSpacing(6);

  // top bar
  auto* top = new QHBoxLayout;
  top->setSpacing(6);
  dspToggle_ = new QPushButton(QStringLiteral("DSP выключен"));
  dspToggle_->setCheckable(true);
  dspToggle_->setMinimumWidth(100);
  abBtn_ = new QPushButton(QStringLiteral("A/B: сравнить"));
  abBtn_->setCheckable(true);
  abBtn_->setToolTip(QStringLiteral("Мгновенно отключает обработку, не меняя настройки (F4). Пока включено «Оригинал», в движок идёт звук без DSP.\n"
                                    "Для честного сравнения подберите громкость кнопкой «Авто-preamp»: более громкое звучит лучше."));
  matchBtn_ = new QPushButton(QStringLiteral("Выровнять громкость"));
  matchBtn_->setToolTip(QStringLiteral("Подбирает preamp так, чтобы звук с обработкой был не громче и не тише исходного. Включите музыку и подождите ~5 секунд.\n"
                                       "Так сравнение через A/B честнее: более громкое всегда кажется лучше."));
  undoBtn_ = new QPushButton(QStringLiteral("Отмена"));
  redoBtn_ = new QPushButton(QStringLiteral("Повтор"));
  undoBtn_->setToolTip(QStringLiteral("Отменить правку (Ctrl+Z)"));
  redoBtn_->setToolTip(QStringLiteral("Повторить отменённое (Ctrl+Y)"));
  engineDot_ = new QLabel(QStringLiteral("●"));
  engineText_ = new QLabel(QStringLiteral("Движок: не запущен"));
  engineText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // a long status is clipped, it never widens the window
  engineText_->setMinimumWidth(60);
  autoBtn_ = new QPushButton(QStringLiteral("Автопрофили…"));
  autoBtn_->setToolTip(QStringLiteral("Автоматически включать профиль DSP и пресет апмикса по тому, какое приложение играет"));
  presetCombo_ = new QComboBox;
  presetCombo_->setMinimumWidth(130);
  auto* savePreset = new QPushButton(QStringLiteral("Сохранить…"));
  savePreset->setToolTip(QStringLiteral("Сохранить текущие настройки как профиль"));
  lookCombo_ = new QComboBox;
  for (int i = 0; i < StudioTheme::kLookCount; ++i)
    lookCombo_->addItem(QString::fromLatin1(StudioTheme::LookName(static_cast<StudioTheme::Look>(i))));
  top->addWidget(dspToggle_);
  top->addWidget(abBtn_);
  top->addWidget(matchBtn_);
  top->addWidget(engineDot_);
  top->addWidget(engineText_, 1);
  top->addWidget(autoBtn_);
  top->addWidget(undoBtn_);
  top->addWidget(redoBtn_);
  top->addSpacing(12);
  top->addWidget(MakeLabel(QStringLiteral("Профиль:"), "dim"));
  top->addWidget(presetCombo_);
  top->addWidget(savePreset);
  top->addSpacing(12);
  top->addWidget(MakeLabel(QStringLiteral("Оформление:"), "dim"));
  top->addWidget(lookCombo_);
  root->addLayout(top);

  // which panels are shown (each can be hidden; the rest takes the freed space)
  showCurveBtn_ = new QPushButton(QStringLiteral("Кривая"));
  showFadersBtn_ = new QPushButton(QStringLiteral("Ползунки"));
  showSpectrumBtn_ = new QPushButton(QStringLiteral("Спектр"));
  showCompBtn_ = new QPushButton(QStringLiteral("Комп. / апмикс"));
  showLevelsBtn_ = new QPushButton(QStringLiteral("Уровни / спектр"));
  showCurveBtn_->setToolTip(QStringLiteral("Показать или скрыть кривую АЧХ (F2)"));
  showFadersBtn_->setToolTip(QStringLiteral("Показать или скрыть 30 ползунков (F3)"));
  showCompBtn_->setToolTip(QStringLiteral("Показать или скрыть вкладки «Компрессор» и «Апмикс» (F5)"));
  showLevelsBtn_->setToolTip(QStringLiteral("Показать или скрыть вкладки «Уровни» и «Спектр каналов» (F6)"));
  showSpectrumBtn_->setToolTip(QStringLiteral("Спектр до и после обработки на кривой (F7). Считается движком, только пока включено"));
  auto* viewRow = new QHBoxLayout;
  viewRow->setSpacing(5);
  viewRow->addWidget(MakeLabel(QStringLiteral("Показывать:"), "dim"));
  for (QPushButton* b : {showCurveBtn_, showFadersBtn_, showSpectrumBtn_, showCompBtn_, showLevelsBtn_})
  {
    b->setCheckable(true);
    b->setChecked(true);
    viewRow->addWidget(b);
  }
  viewRow->addSpacing(12);
  viewRow->addWidget(buildEqToolbar(), 1);
  root->addLayout(viewRow);

  // Left: equalizer (curve over faders). Right: compressor / upmix tabs over levels / spectrum tabs. Every
  // border between these panels is a splitter: drag it to give a panel more or less room.
  auto* tabs = new QTabWidget;
  tabs->setDocumentMode(true);
  tabs->addTab(Scrolled(buildCompressorCard()), QStringLiteral("Компрессор"));
  tabs->addTab(Scrolled(buildUpmixCard()), QStringLiteral("Апмикс"));
  compCard_ = tabs;
  levelsTabs_ = new QTabWidget;
  levelsTabs_->setDocumentMode(true);
  levelsTabs_->addTab(buildLevelsCard(), QStringLiteral("Уровни"));
  levelsTabs_->addTab(buildSpectrumGridCard(), QStringLiteral("Спектр каналов"));
  levelsCard_ = levelsTabs_;

  rightSplit_ = new QSplitter(Qt::Vertical);
  rightSplit_->setChildrenCollapsible(false);
  rightSplit_->setHandleWidth(6);
  rightSplit_->addWidget(compCard_);
  rightSplit_->addWidget(levelsCard_);
  rightSplit_->setStretchFactor(0, 3);
  rightSplit_->setStretchFactor(1, 2);
  rightSplit_->setMinimumWidth(250);
  rightCol_ = rightSplit_;

  mainSplit_ = new QSplitter(Qt::Horizontal);
  mainSplit_->setChildrenCollapsible(false);
  mainSplit_->setHandleWidth(6);
  mainSplit_->addWidget(buildEqCard());
  mainSplit_->addWidget(rightSplit_);
  mainSplit_->setStretchFactor(0, 1);
  mainSplit_->setStretchFactor(1, 0);
  mainSplit_->setSizes({900, 340});
  root->addWidget(mainSplit_, 1);

  statusLabel_ = MakeLabel(QString(), "dim");
  statusLabel_->setWordWrap(false);
  statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  statusLabel_->setFixedHeight(QFontMetrics(statusLabel_->font()).height() + 4);
  root->addWidget(statusLabel_);

  // signals
  connect(dspToggle_, &QPushButton::toggled, this, [this](bool on) {
    if (updating_)
      return;
    model_.params.enabled = on ? 1 : 0;
    dspToggle_->setText(on ? QStringLiteral("DSP включён") : QStringLiteral("DSP выключен"));
    scheduleSave();
  });
  const struct { QPushButton* btn; const char* key; } views[] = {{showCurveBtn_, "showCurve"},
                                                                  {showFadersBtn_, "showFaders"},
                                                                  {showSpectrumBtn_, "showSpectrum"},
                                                                  {showCompBtn_, "showCompressor"},
                                                                  {showLevelsBtn_, "showLevels"}};
  for (const auto& v : views)
  {
    const QString key = QString::fromLatin1(v.key);
    connect(v.btn, &QPushButton::toggled, this, [this, key](bool on) {
      QSettings ini(AppDir() + QStringLiteral("/dsp-studio.ini"), QSettings::IniFormat);
      ini.setValue(key, on);
      applyPanelVisibility();
    });
  }
  connect(abBtn_, &QPushButton::toggled, this, [this](bool on) { onAbToggled(on); });
  connect(matchBtn_, &QPushButton::clicked, this, [this]() { startLoudnessMatch(); });
  connect(autoBtn_, &QPushButton::clicked, this, [this]() { onAutoProfiles(); });
  connect(undoBtn_, &QPushButton::clicked, this, [this]() { undo(); });
  connect(redoBtn_, &QPushButton::clicked, this, [this]() { redo(); });
  connect(presetCombo_, &QComboBox::activated, this, [this](int i) { onPresetChosen(i); });
  connect(savePreset, &QPushButton::clicked, this, [this]() { onSavePreset(); });
  connect(lookCombo_, &QComboBox::activated, this, [this](int i) { onLookChosen(i); });
}

// EQ-wide controls (always visible, whichever panels are shown). They live in the row of the show/hide buttons.
QWidget* StudioWindow::buildEqToolbar()
{
  auto* bar = new QWidget;
  auto* head = new QHBoxLayout(bar);
  head->setContentsMargins(0, 0, 0, 0);
  head->setSpacing(6);
  eqEnable_ = new QCheckBox(QStringLiteral("Эквалайзер"));
  eqEnable_->setProperty("role", "section");
  eqEnable_->setToolTip(QStringLiteral("Эквалайзер на 30 полос"));
  channelCombo_ = new QComboBox;
  channelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  channelCombo_->setMinimumContentsLength(9);
  channelCombo_->setToolTip(QStringLiteral("Какой канал правится: все вместе или один"));
  channelCombo_->addItem(QStringLiteral("Все (связаны)"));
  const char* const full[vac3dsp::kMaxChannels] = {"L — передний левый", "R — передний правый", "C — центр",
                                                   "LFE — сабвуфер", "Ls — тыловой левый", "Rs — тыловой правый"};
  for (const char* name : full)
    channelCombo_->addItem(QString::fromUtf8(name));
  preamp_ = new ParamSlider(QStringLiteral("Preamp"), -24.0, 12.0, QStringLiteral("дБ"), 1, false);
  preamp_->setMaximumWidth(300);
  auto* autoBtn = new QPushButton(QStringLiteral("Авто"));
  autoBtn->setToolTip(QStringLiteral("Авто-preamp: подобрать preamp так, чтобы усиление полос не поднимало общий уровень"));
  auto* resetBtn = new QPushButton(QStringLiteral("Сброс EQ"));
  resetBtn->setToolTip(QStringLiteral("Все полосы в 0 дБ, исходные частоты, ширина и форма"));
  auto* fileBtn = new QPushButton(QStringLiteral("Файл EQ"));
  fileBtn->setToolTip(QStringLiteral("Импорт и экспорт пресетов в формате Equalizer APO / AutoEq / REW (текстовые файлы с фильтрами Filter 1: ON PK Fc … Gain … Q …)"));
  {
    auto* menu = new QMenu(fileBtn);
    QAction* imp = menu->addAction(QStringLiteral("Импорт EQ из файла…"));
    QAction* expAct = menu->addAction(QStringLiteral("Экспорт EQ в файл…"));
    connect(imp, &QAction::triggered, this, [this]() { onImportEq(); });
    connect(expAct, &QAction::triggered, this, [this]() { onExportEq(); });
    fileBtn->setMenu(menu);
  }
  head->addWidget(eqEnable_);
  head->addWidget(MakeLabel(QStringLiteral("Каналы:"), "dim"));
  head->addWidget(channelCombo_);
  head->addWidget(preamp_, 1);
  head->addWidget(autoBtn);
  head->addWidget(resetBtn);
  head->addWidget(fileBtn);

  connect(eqEnable_, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_)
      return;
    model_.params.eq.enabled = on ? 1 : 0;
    scheduleSave();
  });
  connect(channelCombo_, &QComboBox::activated, this, [this](int i) { onChannelChosen(i); });
  connect(preamp_, &ParamSlider::valueChanged, this, [this](double value) {
    if (updating_)
      return;
    model_.params.eq.preampDb = static_cast<float>(value);
    scheduleSave();
  });
  connect(autoBtn, &QPushButton::clicked, this, [this]() {
    model_.params.eq.preampDb = model_.AutoPreampDb(sampleRate_);
    refreshUi();
    scheduleSave();
  });
  connect(resetBtn, &QPushButton::clicked, this, [this]() {
    model_.ResetEq();
    refreshUi();
    scheduleSave();
  });
  return bar;
}

QWidget* StudioWindow::buildEqCard()
{
  auto* panel = new QWidget;
  auto* v = new QVBoxLayout(panel);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(5);

  channelHint_ = MakeLabel(QString(), "dim");
  channelHint_->setWordWrap(true);   // a long hint must never widen the window
  v->addWidget(channelHint_);

  // panel 1: response curve (above the faders; the border between them can be dragged)
  eqSplit_ = new QSplitter(Qt::Vertical);
  eqSplit_->setChildrenCollapsible(false);
  eqSplit_->setHandleWidth(6);
  curveCard_ = MakeCard();
  auto* curveLayout = new QVBoxLayout(curveCard_);
  curveLayout->setContentsMargins(10, 6, 10, 6);
  curve_ = new CurveWidget;
  curveLayout->addWidget(curve_, 1);
  eqSplit_->addWidget(curveCard_);

  // shape of the selected band: needed with either panel, so it sits between them
  bandRow_ = MakeCard();
  auto* bandLayout = new QVBoxLayout(bandRow_);
  bandLayout->setContentsMargins(10, 5, 10, 5);
  bandLayout->addWidget(buildSelectedBandRow());
  v->addWidget(bandRow_);

  // panel 2: the 30 faders
  fadersCard_ = MakeCard();
  auto* fadersLayout = new QVBoxLayout(fadersCard_);
  fadersLayout->setContentsMargins(10, 6, 10, 6);
  auto* faders = new QHBoxLayout;
  faders->setSpacing(2);
  for (int b = 0; b < vac3dsp::kBands; ++b)
  {
    auto* col = new QVBoxLayout;
    col->setSpacing(3);
    bandValues_[static_cast<size_t>(b)] = MakeLabel(QStringLiteral("0"), "small");
    bandValues_[static_cast<size_t>(b)]->setAlignment(Qt::AlignHCenter);
    auto* slider = new BandSlider;
    bandSliders_[static_cast<size_t>(b)] = slider;
    bandFreqs_[static_cast<size_t>(b)] = MakeLabel(QString::fromStdString(vac3dsp::BandLabel(b)), "small");
    bandFreqs_[static_cast<size_t>(b)]->setAlignment(Qt::AlignHCenter);
    auto* badge = new BandBadge;
    badges_[static_cast<size_t>(b)] = badge;
    col->addWidget(bandValues_[static_cast<size_t>(b)], 0, Qt::AlignHCenter);
    col->addWidget(slider, 1, Qt::AlignHCenter);
    col->addWidget(bandFreqs_[static_cast<size_t>(b)], 0, Qt::AlignHCenter);
    col->addWidget(badge, 0, Qt::AlignHCenter);
    faders->addLayout(col, 1);
    connect(slider, &QSlider::valueChanged, this, [this, b](int value) {
      if (updating_)
        return;
      onBandMoved(b, static_cast<float>(value) / 10.0f);
    });
    connect(slider, &QSlider::sliderPressed, this, [this, b]() { selectBand(b); });
    connect(badge, &BandBadge::clicked, this, [this, b]() { selectBand(b); });
  }
  fadersLayout->addLayout(faders, 1);
  eqSplit_->addWidget(fadersCard_);
  eqSplit_->setStretchFactor(0, 2);
  eqSplit_->setStretchFactor(1, 3);
  v->addWidget(eqSplit_, 1);

  connect(curve_, &CurveWidget::bandSelected, this, [this](int band) { selectBand(band); });
  connect(curve_, &CurveWidget::bandEdited, this, [this](int band, float db) { onBandMoved(band, db); });
  connect(curve_, &CurveWidget::bandFreqEdited, this, [this](int band, float hz) { onBandFreqMoved(band, hz); });
  connect(curve_, &CurveWidget::bandWheel, this, [this](int band, int steps) { onBandWheel(band, steps); });
  connect(curve_, &CurveWidget::bandReset, this, [this](int band) { onBandMoved(band, 0.0f); });
  eqPanel_ = panel;
  return panel;
}

void StudioWindow::applyPanelVisibility()
{
  const bool curve = showCurveBtn_->isChecked();
  const bool faders = showFadersBtn_->isChecked();
  const bool comp = showCompBtn_->isChecked();
  const bool levels = showLevelsBtn_->isChecked();
  const bool leftWasHidden = eqPanel_->isHidden();   // (isVisible() is false for everything before the window is shown)
  curveCard_->setVisible(curve);
  fadersCard_->setVisible(faders);
  eqPanel_->setVisible(curve || faders);   // neither panel: the compressor / upmix and level tabs get the whole window
  if ((curve || faders) && leftWasHidden && mainSplit_->width() > 0)   // coming back: give the left side its room again
    mainSplit_->setSizes({std::max(300, mainSplit_->width() - 340), 340});
  bandRow_->setVisible(curve || faders);   // the band editor is useful with either panel
  eqSplit_->setVisible(curve || faders);
  compCard_->setVisible(comp);
  levelsCard_->setVisible(levels);
  rightCol_->setVisible(comp || levels);   // both hidden: the equalizer gets the whole width
  showSpectrumBtn_->setEnabled(curve);     // the spectrum is drawn on the curve
}

// "Selected band": shape, frequency, width (and its Q) of the band that was last clicked. These three are
// shared by all channels; only the gain follows the channel selector.
QWidget* StudioWindow::buildSelectedBandRow()
{
  auto* row = new QWidget;
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(10);

  selTitle_ = MakeLabel(QString(), "section");
  selTitle_->setMinimumWidth(78);
  selType_ = new QComboBox;
  selType_->addItem(QStringLiteral("Колокол"));
  selType_->addItem(QStringLiteral("Нижняя полка"));
  selType_->addItem(QStringLiteral("Верхняя полка"));
  selType_->addItem(QStringLiteral("Режектор (вырез)"));
  selType_->addItem(QStringLiteral("Срез низов (HP)"));
  selType_->addItem(QStringLiteral("Срез верхов (LP)"));
  selType_->setToolTip(QStringLiteral("Колокол и полки используют усиление полосы; режектор и срезы работают сами, без усиления"));
  selFreq_ = new ParamSlider(QStringLiteral("Частота"), 20.0, 20000.0, QStringLiteral("Гц"), 0, true);
  selWidth_ = new ParamSlider(QStringLiteral("Ширина"), 0.05, 4.0, QStringLiteral("окт"), 2, true);
  selQ_ = MakeLabel(QString(), "dim");
  selQ_->setMinimumWidth(52);
  auto* reset = new QPushButton(QStringLiteral("Сбросить полосу"));

  h->addWidget(selTitle_);
  h->addWidget(selType_);
  h->addWidget(selFreq_, 1);
  h->addWidget(selWidth_, 1);
  h->addWidget(selQ_);
  h->addWidget(reset);

  connect(selType_, &QComboBox::activated, this, [this](int i) {
    model_.SetType(model_.selectedBand, i);
    updateCurveAndLabels();
    scheduleSave();
  });
  connect(selFreq_, &ParamSlider::valueChanged, this, [this](double hz) {
    if (updating_)
      return;
    model_.SetFreq(model_.selectedBand, static_cast<float>(hz));
    updateCurveAndLabels();
    scheduleSave();
  });
  connect(selWidth_, &ParamSlider::valueChanged, this, [this](double w) {
    if (updating_)
      return;
    model_.SetWidth(model_.selectedBand, static_cast<float>(w));
    updateCurveAndLabels();
    scheduleSave();
  });
  connect(reset, &QPushButton::clicked, this, [this]() {
    model_.ResetBand(model_.selectedBand);
    refreshUi();
    scheduleSave();
  });
  return row;
}

QWidget* StudioWindow::buildCompressorCard()
{
  QFrame* card = MakeCard();
  auto* v = new QVBoxLayout(card);
  v->setContentsMargins(10, 6, 10, 6);
  v->setSpacing(6);

  compEnable_ = new QCheckBox(QStringLiteral("Компрессор"));
  compEnable_->setProperty("role", "section");
  v->addWidget(compEnable_);

  threshold_ = new ParamSlider(QStringLiteral("Порог"), -60.0, 0.0, QStringLiteral("дБ"), 1, false);
  ratio_ = new ParamSlider(QStringLiteral("Ratio"), 1.0, 20.0, QStringLiteral(":1"), 1, false);
  attack_ = new ParamSlider(QStringLiteral("Атака"), 0.1, 200.0, QStringLiteral("мс"), 1, true);
  release_ = new ParamSlider(QStringLiteral("Спад"), 5.0, 2000.0, QStringLiteral("мс"), 0, true);
  knee_ = new ParamSlider(QStringLiteral("Колено"), 0.0, 24.0, QStringLiteral("дБ"), 1, false);
  makeup_ = new ParamSlider(QStringLiteral("Makeup"), 0.0, 24.0, QStringLiteral("дБ"), 1, false);
  for (ParamSlider* s : {threshold_, ratio_, attack_, release_, knee_, makeup_})
    v->addWidget(s);

  lfeDetector_ = new QCheckBox(QStringLiteral("LFE влияет на детектор"));
  lfeDetector_->setToolTip(QStringLiteral("Обычно выключено: громкий бас не должен «качать» диалоги"));
  v->addWidget(lfeDetector_);

  v->addSpacing(6);
  v->addWidget(MakeLabel(QStringLiteral("Порядок обработки"), "dim"));
  orderCombo_ = new QComboBox;
  orderCombo_->addItem(QStringLiteral("Эквалайзер → компрессор"));
  orderCombo_->addItem(QStringLiteral("Компрессор → эквалайзер"));
  v->addWidget(orderCombo_);
  v->addStretch(1);

  auto bind = [this](ParamSlider* s, float vac3dsp::CompParams::*field) {
    connect(s, &ParamSlider::valueChanged, this, [this, field](double value) {
      if (updating_)
        return;
      model_.params.comp.*field = static_cast<float>(value);
      scheduleSave();
    });
  };
  bind(threshold_, &vac3dsp::CompParams::thresholdDb);
  bind(ratio_, &vac3dsp::CompParams::ratio);
  bind(attack_, &vac3dsp::CompParams::attackMs);
  bind(release_, &vac3dsp::CompParams::releaseMs);
  bind(knee_, &vac3dsp::CompParams::kneeDb);
  bind(makeup_, &vac3dsp::CompParams::makeupDb);
  connect(compEnable_, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_)
      return;
    model_.params.comp.enabled = on ? 1 : 0;
    scheduleSave();
  });
  connect(lfeDetector_, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_)
      return;
    model_.params.comp.lfeInDetector = on ? 1 : 0;
    scheduleSave();
  });
  connect(orderCombo_, &QComboBox::activated, this, [this](int i) {
    model_.params.order = i == 1 ? 1 : 0;
    scheduleSave();
  });
  return card;
}

// Stereo -> 5.1 matrix upmix (engine option upmix_algo=matrix): what goes to the centre channel and how the
// surrounds are fed. The values are written to dsp-settings.json like everything else and act at once, but
// only while the engine runs with upmix_algo=matrix; the algorithm itself is chosen in the config and needs
// an engine restart.
QWidget* StudioWindow::buildUpmixCard()
{
  QFrame* card = MakeCard();
  auto* v = new QVBoxLayout(card);
  v->setContentsMargins(10, 6, 10, 6);
  v->setSpacing(6);

  upmixEnable_ = new QCheckBox(QStringLiteral("Матричный апмикс включён"));
  upmixEnable_->setProperty("role", "section");
  upmixEnable_->setToolTip(QStringLiteral("Выключено: стерео остаётся в передних каналах (центр и тылы молчат)"));
  v->addWidget(upmixEnable_);

  upmixStatus_ = MakeLabel(QString(), "dim");
  upmixStatus_->setWordWrap(true);
  v->addWidget(upmixStatus_);

  auto* presetRow = new QHBoxLayout;
  presetRow->setSpacing(8);
  upmixPresetCombo_ = new QComboBox;
  upmixPresetCombo_->setToolTip(QStringLiteral("Готовые настройки апмикса (кино, музыка, речь). Меняются только настройки этой вкладки, не эквалайзер"));
  auto* savePresetBtn = new QPushButton(QStringLiteral("Сохранить…"));
  presetRow->addWidget(upmixPresetCombo_, 1);
  presetRow->addWidget(savePresetBtn);
  v->addLayout(presetRow);

  centerWidth_ = new ParamSlider(QStringLiteral("Ширина центра"), 0.0, 100.0, QStringLiteral("%"), 0, false);
  centerWidth_->setToolTip(QStringLiteral("Насколько далеко от середины источник ещё считается центром (голос, чуть сдвинутый вбок)"));
  centerFocus_ = new ParamSlider(QStringLiteral("Фокус центра"), 0.0, 100.0, QStringLiteral("%"), 0, false);
  centerFocus_->setToolTip(QStringLiteral("Насколько строго левый и правый должны совпадать. Мало: в центр попадает и «размазанный» звук; много: только чистый моно-голос"));
  centerLow_ = new ParamSlider(QStringLiteral("Центр от"), 60.0, 400.0, QStringLiteral("Гц"), 0, true);
  centerHigh_ = new ParamSlider(QStringLiteral("Центр до"), 2000.0, 12000.0, QStringLiteral("Гц"), 0, true);
  centerLow_->setToolTip(QStringLiteral("Центр берётся только из этого диапазона; бас и высокие остаются в L и R"));
  centerHigh_->setToolTip(centerLow_->toolTip());
  surLevel_ = new ParamSlider(QStringLiteral("Тылы, уровень"), -24.0, 6.0, QStringLiteral("дБ"), 1, false);
  surDelay_ = new ParamSlider(QStringLiteral("Тылы, задержка"), 0.0, 40.0, QStringLiteral("мс"), 1, false);
  surDelay_->setToolTip(QStringLiteral("Задержка тыловых каналов относительно передних: чем больше, тем шире ощущение пространства"));
  for (ParamSlider* s : {centerWidth_, centerFocus_, centerLow_, centerHigh_, surLevel_, surDelay_})
    v->addWidget(s);

  lfeEnable_ = new QCheckBox(QStringLiteral("LFE из низких частот"));
  lfeEnable_->setToolTip(QStringLiteral("Канал LFE получает низ (L+R)/2 до частоты среза. Выключено: LFE молчит, бас идёт через разделение каналов ресивера"));
  lfeLevel_ = new ParamSlider(QStringLiteral("LFE, уровень"), -24.0, 6.0, QStringLiteral("дБ"), 1, false);
  lfeCut_ = new ParamSlider(QStringLiteral("LFE, срез"), 40.0, 200.0, QStringLiteral("Гц"), 0, true);
  v->addWidget(lfeEnable_);
  v->addWidget(lfeLevel_);
  v->addWidget(lfeCut_);

  v->addSpacing(6);
  v->addWidget(MakeLabel(QStringLiteral("Алгоритм апмикса в конфиге"), "dim"));
  algoCombo_ = new QComboBox;
  algoCombo_->addItem(QStringLiteral("FFT surround (как раньше, задержка ~87 мс)"));
  algoCombo_->addItem(QStringLiteral("Матрица (без задержки, настройки выше)"));
  {
    QFile f(ConfPath());
    std::string text;
    if (f.open(QIODevice::ReadOnly))
    {
      const QByteArray data = f.readAll();
      text.assign(data.constData(), static_cast<size_t>(data.size()));
    }
    algoCombo_->setCurrentIndex(vac3dsp::ConfGet(text, "upmix_algo", "surround") == "matrix" ? 1 : 0);
  }
  v->addWidget(algoCombo_);
  v->addStretch(1);

  connect(upmixEnable_, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_)
      return;
    model_.params.upmix.enabled = on ? 1 : 0;
    scheduleSave();
  });
  auto bindPercent = [this](ParamSlider* s, float vac3dsp::UpmixParams::*field) {
    connect(s, &ParamSlider::valueChanged, this, [this, field](double value) {
      if (updating_)
        return;
      model_.params.upmix.*field = static_cast<float>(value / 100.0);
      scheduleSave();
    });
  };
  auto bind = [this](ParamSlider* s, float vac3dsp::UpmixParams::*field) {
    connect(s, &ParamSlider::valueChanged, this, [this, field](double value) {
      if (updating_)
        return;
      model_.params.upmix.*field = static_cast<float>(value);
      scheduleSave();
    });
  };
  bindPercent(centerWidth_, &vac3dsp::UpmixParams::centerWidth);
  bindPercent(centerFocus_, &vac3dsp::UpmixParams::centerFocus);
  bind(centerLow_, &vac3dsp::UpmixParams::centerLowHz);
  bind(centerHigh_, &vac3dsp::UpmixParams::centerHighHz);
  bind(surLevel_, &vac3dsp::UpmixParams::surroundLevelDb);
  bind(surDelay_, &vac3dsp::UpmixParams::surroundDelayMs);
  bind(lfeLevel_, &vac3dsp::UpmixParams::lfeLevelDb);
  bind(lfeCut_, &vac3dsp::UpmixParams::lfeCutHz);
  connect(lfeEnable_, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_)
      return;
    model_.params.upmix.lfeEnabled = on ? 1 : 0;
    scheduleSave();
  });
  connect(upmixPresetCombo_, &QComboBox::activated, this, [this](int i) { onUpmixPresetChosen(i); });
  connect(savePresetBtn, &QPushButton::clicked, this, [this]() { onSaveUpmixPreset(); });
  connect(algoCombo_, &QComboBox::activated, this, [this](int i) { onAlgoChosen(i); });
  return card;
}

// Writes upmix_algo into virtual-ac3-encoder.conf (the engine reads it at start).
void StudioWindow::onAlgoChosen(int index)
{
  const std::string value = index == 1 ? "matrix" : "surround";
  QFile in(ConfPath());
  std::string text;
  if (in.open(QIODevice::ReadOnly))
  {
    const QByteArray data = in.readAll();
    text.assign(data.constData(), static_cast<size_t>(data.size()));
    in.close();
  }
  const std::string changed = vac3dsp::ConfSet(text, "upmix_algo", value);
  QSaveFile out(ConfPath());
  if (!out.open(QIODevice::WriteOnly) || out.write(changed.data(), static_cast<qint64>(changed.size())) < 0 || !out.commit())
  {
    setStatus(QStringLiteral("Не удалось записать virtual-ac3-encoder.conf: ") + out.errorString());
    return;
  }
  setStatus(QStringLiteral("В конфиг записано upmix_algo=") + QString::fromStdString(value) +
            QStringLiteral(". Чтобы изменение заработало, перезапустите движок (закройте engine.exe и запустите заново или примените настройки в панели)."));
}

// What the engine really runs (it tells us through the level block): the settings of this tab only act in "matrix".
void StudioWindow::updateUpmixStatus(bool alive, int engineMode)
{
  QString text;
  if (!alive)
    text = QStringLiteral("Движок не запущен. Настройки сохраняются и подхватятся при запуске, если в конфиге upmix_algo=matrix.");
  else if (engineMode == 2)
    text = QStringLiteral("Движок работает в режиме «матрица»: изменения слышны сразу.");
  else if (engineMode == 1)
    text = QStringLiteral("Движок сейчас использует FFT surround, эти настройки не действуют. Выберите «Матрица» внизу и перезапустите движок.");
  else
    text = QStringLiteral("В движке апмикс выключен (upmix=off в конфиге).");
  if (upmixStatus_->text() != text)
    upmixStatus_->setText(text);
}

QWidget* StudioWindow::buildLevelsCard()
{
  QFrame* card = MakeCard();
  card->setToolTip(QStringLiteral("Слева вход DSP, справа выход DSP, пик держится около 1,5 с.\n"
                                  "Комп. и Лимит. показывают снижение усиления в дБ."));
  auto* v = new QVBoxLayout(card);
  v->setContentsMargins(10, 5, 10, 5);
  v->setSpacing(4);
  auto* title = new QHBoxLayout;
  title->addWidget(MakeLabel(QStringLiteral("Уровни"), "section"));
  title->addWidget(MakeLabel(QStringLiteral("вход | выход"), "dim"));
  title->addStretch(1);
  v->addLayout(title);

  auto* row = new QHBoxLayout;
  row->setSpacing(8);
  for (int c = 0; c < vac3dsp::kMaxChannels; ++c)
  {
    auto* col = new QVBoxLayout;
    col->setSpacing(3);
    auto* bars = new QHBoxLayout;
    bars->setSpacing(2);
    inMeters_[static_cast<size_t>(c)] = new MeterWidget(MeterWidget::Mode::Level);
    outMeters_[static_cast<size_t>(c)] = new MeterWidget(MeterWidget::Mode::Level);
    bars->addWidget(inMeters_[static_cast<size_t>(c)]);
    bars->addWidget(outMeters_[static_cast<size_t>(c)]);
    col->addLayout(bars, 1);
    auto* name = MakeLabel(QString::fromLatin1(kChannelTitles[c]), "small");
    name->setAlignment(Qt::AlignHCenter);
    col->addWidget(name);
    row->addLayout(col, 1);
  }
  row->addSpacing(6);
  auto addReduction = [&](MeterWidget*& slot, const QString& caption) {
    auto* col = new QVBoxLayout;
    col->setSpacing(3);
    slot = new MeterWidget(MeterWidget::Mode::Reduction);
    col->addWidget(slot, 1, Qt::AlignHCenter);
    auto* name = MakeLabel(caption, "small");
    name->setAlignment(Qt::AlignHCenter);
    col->addWidget(name);
    row->addLayout(col);
  };
  addReduction(compMeter_, QStringLiteral("Комп."));
  addReduction(limiterMeter_, QStringLiteral("Лимит."));
  v->addLayout(row, 1);
  card->setMinimumHeight(130);
  return card;
}

// ------------------------------------------------------------------------------------ model <-> UI
void StudioWindow::refreshUi()
{
  updating_ = true;
  const vac3dsp::DspParams& p = model_.params;

  dspToggle_->setChecked(p.enabled != 0);
  dspToggle_->setText(p.enabled ? QStringLiteral("DSP включён") : QStringLiteral("DSP выключен"));
  eqEnable_->setChecked(p.eq.enabled != 0);
  preamp_->setValue(static_cast<double>(p.eq.preampDb));
  for (int b = 0; b < vac3dsp::kBands; ++b)
    bandSliders_[static_cast<size_t>(b)]->setValue(static_cast<int>(std::lround(model_.Band(b) * 10.0f)));

  compEnable_->setChecked(p.comp.enabled != 0);
  threshold_->setValue(static_cast<double>(p.comp.thresholdDb));
  ratio_->setValue(static_cast<double>(p.comp.ratio));
  attack_->setValue(static_cast<double>(p.comp.attackMs));
  release_->setValue(static_cast<double>(p.comp.releaseMs));
  knee_->setValue(static_cast<double>(p.comp.kneeDb));
  makeup_->setValue(static_cast<double>(p.comp.makeupDb));
  lfeDetector_->setChecked(p.comp.lfeInDetector != 0);
  upmixEnable_->setChecked(p.upmix.enabled != 0);
  centerWidth_->setValue(static_cast<double>(p.upmix.centerWidth) * 100.0);
  centerFocus_->setValue(static_cast<double>(p.upmix.centerFocus) * 100.0);
  centerLow_->setValue(static_cast<double>(p.upmix.centerLowHz));
  centerHigh_->setValue(static_cast<double>(p.upmix.centerHighHz));
  surLevel_->setValue(static_cast<double>(p.upmix.surroundLevelDb));
  surDelay_->setValue(static_cast<double>(p.upmix.surroundDelayMs));
  lfeEnable_->setChecked(p.upmix.lfeEnabled != 0);
  lfeLevel_->setValue(static_cast<double>(p.upmix.lfeLevelDb));
  lfeCut_->setValue(static_cast<double>(p.upmix.lfeCutHz));
  orderCombo_->setCurrentIndex(p.order == 1 ? 1 : 0);
  channelCombo_->setCurrentIndex(model_.channel + 1);

  QSettings ini(AppDir() + QStringLiteral("/dsp-studio.ini"), QSettings::IniFormat);
  lookCombo_->setCurrentIndex(std::min(StudioTheme::kLookCount - 1, std::max(0, ini.value(QStringLiteral("look"), 0).toInt())));
  updating_ = false;
  updateCurveAndLabels();
}

void StudioWindow::updateCurveAndLabels()
{
  std::vector<double> response(static_cast<size_t>(vac3dsp::kCurvePoints));
  model_.Curve(sampleRate_, response.data());
  curve_->setCurve(response);

  std::vector<CurveBand> bands(static_cast<size_t>(vac3dsp::kBands));
  for (int b = 0; b < vac3dsp::kBands; ++b)
  {
    const size_t i = static_cast<size_t>(b);
    const vac3dsp::BandShape& sh = model_.Shape(b);
    const bool gain = vac3dsp::BandTypeUsesGain(sh.type);
    bands[i].db = model_.Band(b);
    bands[i].freq = sh.freqHz;
    bands[i].type = sh.type;
    bandValues_[i]->setText(gain ? DbText(model_.Band(b)) : QStringLiteral("—"));
    bandFreqs_[i]->setText(QString::fromStdString(vac3dsp::FreqLabel(static_cast<double>(sh.freqHz))));
    bandSliders_[i]->setEnabled(gain);
    badges_[i]->setShape(QString::fromLatin1(vac3dsp::BandTypeShort(sh.type)), vac3dsp::WidthFraction(sh.widthOct),
                         b == model_.selectedBand);
  }
  curve_->setBands(bands, model_.selectedBand);
  updateSelectedRow();

  if (model_.channel == vac3dsp::kAllChannels && model_.ChannelsDiffer())
    channelHint_->setText(QStringLiteral("Каналы настроены по-разному: показана кривая L. Любое изменение сейчас затронет полосу во всех каналах. Частота, ширина и форма полос общие для всех каналов."));
  else if (model_.channel != vac3dsp::kAllChannels)
    channelHint_->setText(QStringLiteral("Правится только канал ") + QString::fromLatin1(kChannelTitles[model_.channel]) +
                          QStringLiteral(". Частота, ширина и форма полос общие для всех каналов. В режиме PCM stereo работают только L и R."));
  else
    channelHint_->setText(QStringLiteral("Подсказка: Shift+перетаскивание точки двигает частоту полосы, колёсико над кривой меняет ширину."));
}

void StudioWindow::selectBand(int band)
{
  if (band < 0 || band >= vac3dsp::kBands || band == model_.selectedBand)
    return;
  model_.selectedBand = band;
  updateCurveAndLabels();
}

// Shows the selected band's shape in the row above the faders (no signals, nothing is saved).
void StudioWindow::updateSelectedRow()
{
  const int b = std::min(vac3dsp::kBands - 1, std::max(0, model_.selectedBand));
  const vac3dsp::BandShape& sh = model_.Shape(b);
  const bool wasUpdating = updating_;
  updating_ = true;
  selTitle_->setText(QStringLiteral("Полоса %1").arg(b + 1));
  selType_->setCurrentIndex(vac3dsp::NormalizeBandType(sh.type));
  selFreq_->setValue(static_cast<double>(sh.freqHz));
  selWidth_->setValue(static_cast<double>(sh.widthOct));
  selQ_->setText(QStringLiteral("Q ") + QString::number(model_.Q(b, sampleRate_), 'f', 2));
  updating_ = wasUpdating;
}

void StudioWindow::onBandFreqMoved(int band, float hz)
{
  if (band < 0 || band >= vac3dsp::kBands)
    return;
  model_.selectedBand = band;
  model_.SetFreq(band, hz);
  updateCurveAndLabels();
  scheduleSave();
}

void StudioWindow::onBandWheel(int band, int steps)
{
  if (band < 0 || band >= vac3dsp::kBands)
    return;
  model_.selectedBand = band;
  model_.SetWidth(band, model_.Shape(band).widthOct * std::pow(1.12f, static_cast<float>(steps)));
  updateCurveAndLabels();
  scheduleSave();
}

void StudioWindow::onBandMoved(int band, float db)
{
  if (band < 0 || band >= vac3dsp::kBands)
    return;
  model_.selectedBand = band;
  model_.SetBand(band, db);
  updating_ = true;
  bandSliders_[static_cast<size_t>(band)]->setValue(static_cast<int>(std::lround(model_.Band(band) * 10.0f)));
  updating_ = false;
  updateCurveAndLabels();
  scheduleSave();
}

void StudioWindow::onChannelChosen(int index)
{
  model_.channel = index <= 0 ? vac3dsp::kAllChannels : std::min(index - 1, vac3dsp::kMaxChannels - 1);
  updating_ = true;
  for (int b = 0; b < vac3dsp::kBands; ++b)
    bandSliders_[static_cast<size_t>(b)]->setValue(static_cast<int>(std::lround(model_.Band(b) * 10.0f)));
  updating_ = false;
  updateCurveAndLabels();
}

// ------------------------------------------------------------------------------------ files
void StudioWindow::setStatus(const QString& text)
{
  QString oneLine = text;
  oneLine.replace(QLatin1Char('\n'), QLatin1Char(' '));
  statusLabel_->setToolTip(text);
  const int room = std::max(160, statusLabel_->width());
  statusLabel_->setText(QFontMetrics(statusLabel_->font()).elidedText(oneLine, Qt::ElideRight, room));
}

void StudioWindow::scheduleSave()
{
  saveTimer_.start();
  undoTimer_.start();   // restarts while the user keeps editing: a whole gesture becomes one undo step
}

void StudioWindow::doSave()
{
  std::string err;
  if (vac3dsp::SaveSettings(settingsPath_, EffectiveParams(), &err))
    setStatus(abBypass_ ? QStringLiteral("A/B: в движок идёт звук без обработки, настройки сохранены и не тронуты")
                        : QStringLiteral("Сохранено в dsp-settings.json, движок подхватит изменения сам"));
  else
    setStatus(QStringLiteral("Не удалось сохранить dsp-settings.json: ") + QString::fromStdString(err));
}

void StudioWindow::pollFile()
{
  if (saveTimer_.isActive())
    return;  // our own edit is on its way to the file: do not read the old one over it
  vac3dsp::DspParams p;
  std::string err;
  const auto result = file_.Poll(p, &err);
  if (result == vac3dsp::SettingsFile::Result::Loaded)
  {
    // Compare through the file formatter: values the UI holds with more digits than the file keeps must
    // not look like an outside change right after our own save.
    if (vac3dsp::FormatSettings(p) != vac3dsp::FormatSettings(EffectiveParams()))
    {
      if (abBypass_)   // an outside change ends the A/B comparison
      {
        abBypass_ = false;
        const QSignalBlocker blocker(abBtn_);
        abBtn_->setChecked(false);
        abBtn_->setText(QStringLiteral("A/B: сравнить"));
      }
      model_.params = p;   // someone else (a text editor, a copied preset) changed the file
      history_.Rebase(model_.params);   // not an edit of ours: no undo step
      undoTimer_.stop();
      refreshUi();
      setStatus(QStringLiteral("dsp-settings.json изменён снаружи, настройки перечитаны"));
    }
  }
  else if (result == vac3dsp::SettingsFile::Result::Failed)
    setStatus(QStringLiteral("dsp-settings.json содержит ошибку (") + QString::fromStdString(err) +
              QStringLiteral("); пока действуют прежние настройки"));
}

void StudioWindow::loadPresetList(const QString& select)
{
  const bool was = presetCombo_->blockSignals(true);
  presetCombo_->clear();
  presetCombo_->addItem(QStringLiteral("— выбрать профиль —"));
  const QStringList files = QDir(PresetsDir()).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name);
  int selectIndex = 0;
  for (const QString& f : files)
  {
    const QString base = QFileInfo(f).completeBaseName();
    presetCombo_->addItem(base, PresetsDir() + QLatin1Char('/') + f);
    if (!select.isEmpty() && base == select)
      selectIndex = presetCombo_->count() - 1;
  }
  presetCombo_->setCurrentIndex(selectIndex);
  presetCombo_->blockSignals(was);
}

void StudioWindow::onPresetChosen(int index)
{
  if (index <= 0)
    return;
  const QString path = presetCombo_->itemData(index).toString();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось открыть файл профиля:\n") + path);
    return;
  }
  const QByteArray data = f.readAll();
  vac3dsp::DspParams p;
  std::string err;
  const std::string profileText(data.constData(), static_cast<size_t>(data.size()));
  if (!vac3dsp::ParseSettings(profileText, p, &err))
  {
    QMessageBox::warning(this, windowTitle(),
                         QStringLiteral("Профиль повреждён: ") + QString::fromStdString(err));
    return;
  }
  const vac3dsp::UpmixParams keepUpmix = model_.params.upmix;
  model_.params = p;
  if (!vac3dsp::HasUpmixSection(profileText))
    model_.params.upmix = keepUpmix;   // an older profile has no upmix settings: keep the user's own
  refreshUi();
  scheduleSave();
  setStatus(QStringLiteral("Профиль «") + presetCombo_->itemText(index) + QStringLiteral("» применён"));
}

void StudioWindow::onSavePreset()
{
  bool ok = false;
  QString name = QInputDialog::getText(this, QStringLiteral("Сохранить профиль"), QStringLiteral("Название профиля:"),
                                       QLineEdit::Normal, QString(), &ok)
                     .trimmed();
  if (!ok || name.isEmpty())
    return;
  for (const QChar ch : QStringLiteral("\\/:*?\"<>|"))
    name.replace(ch, QLatin1Char('_'));
  QDir().mkpath(PresetsDir());
  const QString path = PresetsDir() + QLatin1Char('/') + name + QStringLiteral(".json");
  if (QFileInfo::exists(path) &&
      QMessageBox::question(this, windowTitle(), QStringLiteral("Профиль «") + name + QStringLiteral("» уже есть. Заменить?")) !=
          QMessageBox::Yes)
    return;
  std::string err;
  if (!vac3dsp::SaveSettings(std::filesystem::path(QDir::toNativeSeparators(path).toStdWString()), model_.params, &err))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось сохранить профиль: ") + QString::fromStdString(err));
    return;
  }
  loadPresetList(name);
  setStatus(QStringLiteral("Профиль «") + name + QStringLiteral("» сохранён в папке dsp-presets"));
}

void StudioWindow::onLookChosen(int index)
{
  index = std::min(StudioTheme::kLookCount - 1, std::max(0, index));
  StudioTheme::Apply(*qApp, static_cast<StudioTheme::Look>(index));
  QSettings ini(AppDir() + QStringLiteral("/dsp-studio.ini"), QSettings::IniFormat);
  ini.setValue(QStringLiteral("look"), index);
}

// ------------------------------------------------------------------------------------ meters
void StudioWindow::pollMeters()
{
  const float dt = std::min(0.25f, static_cast<float>(sinceTick_.restart()) / 1000.0f);

  if (!shm_.IsOpen() && (++openTick_ % 15) == 0)
    shm_.Open();   // the engine creates the block when it starts; try about twice a second

  vac3dsp::DspMeterBlock b;
  bool fresh = false;
  if (shm_.IsOpen() && shm_.Read(b) && b.updates != lastUpdates_)
  {
    lastUpdates_ = b.updates;
    sinceFresh_.restart();
    fresh = true;
    if (b.sampleRate == 32000u || b.sampleRate == 44100u || b.sampleRate == 48000u || b.sampleRate == 96000u)
      if (static_cast<int>(b.sampleRate) != sampleRate_)
      {
        sampleRate_ = static_cast<int>(b.sampleRate);
        updateCurveAndLabels();
      }
  }
  const bool alive = sinceFresh_.isValid() && sinceFresh_.elapsed() < 1500;
  engineAlive_ = alive;
  engineDot_->setStyleSheet(alive ? QStringLiteral("color: #2ebe64;") : QStringLiteral("color: #da4453;"));
  engineText_->setText(alive ? (b.pcmMode ? QStringLiteral("Движок работает (PCM stereo)") : QStringLiteral("Движок работает"))
                             : QStringLiteral("Движок: не запущен"));
  updateUpmixStatus(alive, alive ? b.upmixMode : -1);
  if (alive)
  {
    if (fresh)
    {
      lastActiveApp_ = QString::fromUtf8(b.activeApp);
      lastAutoProfile_ = QString::fromUtf8(b.autoProfile);
    }
    QString text = b.pcmMode ? QStringLiteral("Движок работает (PCM stereo)") : QStringLiteral("Движок работает");
    if (!lastActiveApp_.isEmpty())
      text += QStringLiteral(" · играет: ") + lastActiveApp_;
    if (!lastAutoProfile_.isEmpty())
      text += QStringLiteral(" · авто: ") + lastAutoProfile_;
    engineText_->setText(text);   // (the same text every tick, so nothing flickers)
  }

  for (int c = 0; c < vac3dsp::kMaxChannels; ++c)
  {
    const size_t i = static_cast<size_t>(c);
    inBall_[i].Update(fresh ? vac3dsp::PeakToDb(b.inPeak[c]) : vac3dsp::kMeterFloorDb, dt);
    outBall_[i].Update(fresh ? vac3dsp::PeakToDb(b.outPeak[c]) : vac3dsp::kMeterFloorDb, dt);
    inMeters_[i]->setValues(inBall_[i].level(), inBall_[i].peak());
    outMeters_[i]->setValues(outBall_[i].level(), outBall_[i].peak());
  }
  auto fall = [dt](float& state, float measured) { state = std::max(measured, state - 30.0f * dt); };
  fall(compGr_, fresh ? b.compReductionDb : 0.0f);
  fall(limiterGr_, fresh ? b.limiterReductionDb : 0.0f);
  compMeter_->setValues(compGr_, compGr_);
  limiterMeter_->setValues(limiterGr_, limiterGr_);

  // Spectrum: keep what the engine last published and show the chosen channel (all channels summed when
  // they are linked), falling gently instead of flickering at the engine's 10 Hz.
  if (fresh && b.spectrumValid)
  {
    std::copy(&b.specIn[0][0], &b.specIn[0][0] + vac3dsp::kMaxChannels * vac3dsp::kSpecBands, &specInRaw_[0][0]);
    std::copy(&b.specOut[0][0], &b.specOut[0][0] + vac3dsp::kMaxChannels * vac3dsp::kSpecBands, &specOutRaw_[0][0]);
    specHave_ = true;
  }
  if (!alive)
    specHave_ = false;
  const bool spectrumShown = showSpectrumBtn_->isChecked() && showCurveBtn_->isChecked();
  const bool gridShown = levelsCard_->isVisible() && levelsTabs_->currentIndex() == 1;
  if (matching_ && fresh && b.spectrumValid)
  {
    matcher_.AddFrame(b.specIn, b.specOut);
    if (++matchFrames_ >= 50)   // ~5 s of published frames
      finishLoudnessMatch(true);
  }
  if (matching_ && matchTimer_.elapsed() > 15000)
    finishLoudnessMatch(false);
  if ((spectrumShown || gridShown || matching_) && shm_.IsOpen() && (++beatTick_ % 8) == 0)
    shm_.Heartbeat();   // tells the engine that a spectrum is wanted (it computes none while nobody asks)
  curve_->setSpectrumVisible(spectrumShown);
  if (spectrumShown)
  {
    float inNow[vac3dsp::kSpecBands], outNow[vac3dsp::kSpecBands];
    if (specHave_)
    {
      vac3dsp::CombineSpectrum(specInRaw_, model_.channel, inNow);
      vac3dsp::CombineSpectrum(specOutRaw_, model_.channel, outNow);
    }
    else
      for (int i = 0; i < vac3dsp::kSpecBands; ++i)
        inNow[i] = outNow[i] = vac3dsp::kSpecFloorDb;
    for (size_t i = 0; i < static_cast<size_t>(vac3dsp::kSpecBands); ++i)
    {
      specInShown_[i] = std::max(inNow[i], specInShown_[i] - 18.0f * dt);
      specOutShown_[i] = std::max(outNow[i], specOutShown_[i] - 18.0f * dt);
    }
    curve_->setSpectrum(specInShown_, specOutShown_);
  }
  if (gridShown)
  {
    for (int c = 0; c < vac3dsp::kMaxChannels; ++c)
      for (int i = 0; i < vac3dsp::kSpecBands; ++i)
      {
        const float inNow = specHave_ ? specInRaw_[c][i] : vac3dsp::kSpecFloorDb;
        const float outNow = specHave_ ? specOutRaw_[c][i] : vac3dsp::kSpecFloorDb;
        specInCh_[c][i] = std::max(inNow, specInCh_[c][i] - 18.0f * dt);
        specOutCh_[c][i] = std::max(outNow, specOutCh_[c][i] - 18.0f * dt);
      }
    grid_->setSpectra(specInCh_, specOutCh_);
  }
}

// ------------------------------------------------------------------------------------ upmix presets, channel spectra
QWidget* StudioWindow::buildSpectrumGridCard()
{
  QFrame* card = MakeCard();
  auto* v = new QVBoxLayout(card);
  v->setContentsMargins(10, 8, 10, 8);
  v->setSpacing(4);
  v->addWidget(MakeLabel(QStringLiteral("Спектр каждого канала: серый вход, оранжевый выход (−90…−10 дБFS). Виден результат апмикса в центре и тылах."), "dim"));
  static_cast<QLabel*>(v->itemAt(0)->widget())->setWordWrap(true);
  grid_ = new SpectrumGrid;
  v->addWidget(grid_, 1);
  return card;
}

namespace {
QString UpmixPresetTitle(const QString& base)
{
  const struct { const char* key; const char* title; } known[] = {
      {"Standard", "Стандарт"}, {"Movie", "Кино"}, {"Music", "Музыка"}, {"Speech", "Речь"}};
  for (const auto& k : known)
    if (base.endsWith(QString::fromLatin1(k.key)))
      return QString::fromUtf8(k.title);
  return base;
}
}  // namespace

void StudioWindow::loadUpmixPresetList(const QString& select)
{
  const bool was = upmixPresetCombo_->blockSignals(true);
  upmixPresetCombo_->clear();
  upmixPresetCombo_->addItem(QStringLiteral("— пресет апмикса —"));
  const QStringList files = QDir(UpmixPresetsDir()).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name);
  int selectIndex = 0;
  for (const QString& f : files)
  {
    const QString base = QFileInfo(f).completeBaseName();
    upmixPresetCombo_->addItem(UpmixPresetTitle(base), UpmixPresetsDir() + QLatin1Char('/') + f);
    if (!select.isEmpty() && base == select)
      selectIndex = upmixPresetCombo_->count() - 1;
  }
  upmixPresetCombo_->setCurrentIndex(selectIndex);
  upmixPresetCombo_->blockSignals(was);
}

// A preset replaces the upmix settings only (not the equalizer, not the compressor); the on/off switch stays as it is.
void StudioWindow::onUpmixPresetChosen(int index)
{
  if (index <= 0)
    return;
  const QString path = upmixPresetCombo_->itemData(index).toString();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось открыть пресет:\n") + path);
    return;
  }
  const QByteArray data = f.readAll();
  vac3dsp::UpmixParams u;
  std::string err;
  if (!vac3dsp::ParseUpmixPreset(std::string(data.constData(), static_cast<size_t>(data.size())), u, &err))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Пресет повреждён: ") + QString::fromStdString(err));
    return;
  }
  const int enabled = model_.params.upmix.enabled;
  model_.params.upmix = u;
  model_.params.upmix.enabled = enabled;
  refreshUi();
  scheduleSave();
  setStatus(QStringLiteral("Пресет апмикса «") + upmixPresetCombo_->itemText(index) + QStringLiteral("» применён"));
}

void StudioWindow::onSaveUpmixPreset()
{
  bool ok = false;
  QString name = QInputDialog::getText(this, QStringLiteral("Сохранить пресет апмикса"), QStringLiteral("Название:"),
                                       QLineEdit::Normal, QString(), &ok)
                     .trimmed();
  if (!ok || name.isEmpty())
    return;
  for (const QChar ch : QStringLiteral("\\/:*?\"<>|"))
    name.replace(ch, QLatin1Char('_'));
  QDir().mkpath(UpmixPresetsDir());
  const QString path = UpmixPresetsDir() + QLatin1Char('/') + name + QStringLiteral(".json");
  if (QFileInfo::exists(path) &&
      QMessageBox::question(this, windowTitle(), QStringLiteral("Пресет «") + name + QStringLiteral("» уже есть. Заменить?")) !=
          QMessageBox::Yes)
    return;
  const std::string text = vac3dsp::FormatUpmixPreset(model_.params.upmix);
  QSaveFile out(path);
  if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), static_cast<qint64>(text.size())) < 0 || !out.commit())
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось сохранить пресет: ") + out.errorString());
    return;
  }
  loadUpmixPresetList(name);
  setStatus(QStringLiteral("Пресет апмикса «") + name + QStringLiteral("» сохранён в папке upmix-presets"));
}

// ------------------------------------------------------------------------------------ loudness match, EQ files, auto profiles
// Averages the loudness (K-weighted) of the DSP input and output for ~5 s of real material and moves the preamp
// by the difference, so that the processed sound is exactly as loud as the original.
void StudioWindow::startLoudnessMatch()
{
  if (abBypass_)
  {
    setStatus(QStringLiteral("Сначала выключите A/B («Оригинал»): громкость сравнивается с включённой обработкой."));
    return;
  }
  if (!engineAlive_)
  {
    setStatus(QStringLiteral("Движок не запущен: измерять нечего."));
    return;
  }
  matcher_.Reset();
  matchFrames_ = 0;
  matching_ = true;
  matchTimer_.start();
  matchBtn_->setEnabled(false);
  setStatus(QStringLiteral("Измеряю громкость: пусть играет обычная музыка или фильм, около 5 секунд…"));
}

void StudioWindow::finishLoudnessMatch(bool enoughSignal)
{
  matching_ = false;
  matchBtn_->setEnabled(true);
  if (!enoughSignal || matcher_.Frames() < 20)
  {
    setStatus(QStringLiteral("Не хватило сигнала (тихо или ничего не играет), preamp не менялся."));
    return;
  }
  const double delta = matcher_.DeltaDb();
  if (std::fabs(delta) < 0.05)
  {
    setStatus(QStringLiteral("Громкость с обработкой и без неё уже совпадает, preamp не менялся."));
    return;
  }
  const float current = model_.params.eq.preampDb;
  const float target = std::min(12.0f, std::max(vac3dsp::kEqPreampMinDb, current + static_cast<float>(delta)));
  model_.params.eq.preampDb = std::round(target * 10.0f) / 10.0f;
  preamp_->setValue(static_cast<double>(model_.params.eq.preampDb));   // only the preamp changed: nothing else is touched
  scheduleSave();
  setStatus(QStringLiteral("Громкость выровнена: preamp %1 → %2 дБ (с обработкой было %3 на %4 дБ)")
                .arg(static_cast<double>(current), 0, 'f', 1)
                .arg(static_cast<double>(model_.params.eq.preampDb), 0, 'f', 1)
                .arg(delta < 0.0 ? QStringLiteral("громче") : QStringLiteral("тише"))
                .arg(std::fabs(delta), 0, 'f', 1));
}

void StudioWindow::onImportEq()
{
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт EQ"), AppDir(),
                                                    QStringLiteral("Пресеты EQ (*.txt *.cfg);;Все файлы (*)"));
  if (path.isEmpty())
    return;
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось открыть файл:\n") + path);
    return;
  }
  const QByteArray data = f.readAll();
  vac3dsp::EqParams eq = model_.params.eq;
  vac3dsp::EqImportResult result;
  if (!vac3dsp::ParseApoText(std::string(data.constData(), static_cast<size_t>(data.size())), sampleRate_, eq, &result))
  {
    QMessageBox::warning(this, windowTitle(),
                         QStringLiteral("В файле не нашлось фильтров в формате Equalizer APO / AutoEq / REW (строки вида «Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1.0»)."));
    return;
  }
  model_.params.eq = eq;
  model_.params.eq.enabled = 1;
  refreshUi();
  scheduleSave();
  QString text = QStringLiteral("Импортировано фильтров: %1").arg(result.imported);
  if (result.skipped > 0)
    text += QStringLiteral(", пропущено: %1").arg(result.skipped);
  if (result.preamp)
    text += QStringLiteral(". Preamp: %1 дБ").arg(static_cast<double>(eq.preampDb), 0, 'f', 1);
  text += QStringLiteral(".");
  for (size_t i = 0; i < result.notes.size() && i < 8; ++i)
    text += QStringLiteral("\n• ") + QString::fromUtf8(result.notes[i].c_str());
  if (!model_.params.enabled)
    text += QStringLiteral("\n\nDSP сейчас выключен: нажмите «DSP выключен», чтобы услышать результат.");
  setStatus(QStringLiteral("EQ импортирован из ") + QFileInfo(path).fileName());
  QMessageBox::information(this, windowTitle(), text);
}

void StudioWindow::onExportEq()
{
  const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Экспорт EQ"), AppDir() + QStringLiteral("/EQ.txt"),
                                                    QStringLiteral("Пресеты EQ (*.txt);;Все файлы (*)"));
  if (path.isEmpty())
    return;
  const std::string text = vac3dsp::FormatApoText(model_.params.eq, model_.channel, sampleRate_);
  QSaveFile out(path);
  if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), static_cast<qint64>(text.size())) < 0 || !out.commit())
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось сохранить файл: ") + out.errorString());
    return;
  }
  setStatus(QStringLiteral("EQ сохранён: ") + QFileInfo(path).fileName() +
            (model_.channel == vac3dsp::kAllChannels ? QString() : QStringLiteral(" (кривая выбранного канала)")));
}

void StudioWindow::onAutoProfiles()
{
  QStringList dsp, upmix;
  for (const QString& f : QDir(PresetsDir()).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name))
    dsp << QFileInfo(f).completeBaseName();
  for (const QString& f : QDir(UpmixPresetsDir()).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name))
    upmix << QFileInfo(f).completeBaseName();
  AutoProfilesDialog dialog(this, AppDir() + QStringLiteral("/auto-profiles.json"), dsp, upmix, lastActiveApp_);
  if (dialog.exec() == QDialog::Accepted)
    setStatus(QStringLiteral("Автопрофили сохранены, движок подхватит их сам (в течение пары секунд)."));
}

// ------------------------------------------------------------------------------------ A/B, undo, hotkeys
vac3dsp::DspParams StudioWindow::EffectiveParams() const
{
  vac3dsp::DspParams p = model_.params;
  if (abBypass_)
    p.enabled = 0;   // the settings stay as edited; only what the engine sees is switched off
  return p;
}

void StudioWindow::onAbToggled(bool on)
{
  abBypass_ = on;
  abBtn_->setText(on ? QStringLiteral("◀ Оригинал (без DSP)") : QStringLiteral("A/B: сравнить"));
  saveTimer_.start();   // goes to the file like any change, but it is not an edit: no undo step
  setStatus(on ? QStringLiteral("A/B: идёт сравнение, в движок уходит звук без обработки; настройки не тронуты")
               : QStringLiteral("A/B: обработка снова включена"));
}

void StudioWindow::commitHistory()
{
  undoTimer_.stop();
  if (history_.Commit(model_.params))
    updateUndoButtons();
}

void StudioWindow::undo()
{
  commitHistory();   // a gesture that is still open becomes a step first
  if (!history_.Undo(model_.params))
    return;
  refreshUi();
  saveTimer_.start();
  updateUndoButtons();
  setStatus(QStringLiteral("Отменено"));
}

void StudioWindow::redo()
{
  commitHistory();
  if (!history_.Redo(model_.params))
    return;
  refreshUi();
  saveTimer_.start();
  updateUndoButtons();
  setStatus(QStringLiteral("Повторено"));
}

void StudioWindow::updateUndoButtons()
{
  undoBtn_->setEnabled(history_.CanUndo());
  redoBtn_->setEnabled(history_.CanRedo());
}

void StudioWindow::setupShortcuts()
{
  auto bind = [this](const QKeySequence& key, std::function<void()> action, bool repeat) {
    auto* shortcut = new QShortcut(key, this);
    shortcut->setAutoRepeat(repeat);
    connect(shortcut, &QShortcut::activated, this, std::move(action));
  };
  bind(QKeySequence(Qt::Key_F2), [this]() { showCurveBtn_->toggle(); }, false);
  bind(QKeySequence(Qt::Key_F3), [this]() { showFadersBtn_->toggle(); }, false);
  bind(QKeySequence(Qt::Key_F4), [this]() { abBtn_->toggle(); }, false);
  bind(QKeySequence(Qt::Key_F5), [this]() { showCompBtn_->toggle(); }, false);
  bind(QKeySequence(Qt::Key_F6), [this]() { showLevelsBtn_->toggle(); }, false);
  bind(QKeySequence(Qt::Key_F7), [this]() { if (showSpectrumBtn_->isEnabled()) showSpectrumBtn_->toggle(); }, false);
  bind(QKeySequence(Qt::CTRL | Qt::Key_0), [this]() { resetWindowSize(); }, false);
  bind(QKeySequence(Qt::CTRL | Qt::Key_Z), [this]() { undo(); }, true);
  bind(QKeySequence(Qt::CTRL | Qt::Key_Y), [this]() { redo(); }, true);
  bind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this]() { redo(); }, true);
}
