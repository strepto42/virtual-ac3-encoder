// StudioWindow.h — the DSP Studio main window: 30-band equalizer with response curve, compressor,
// level meters, profiles and four looks. It never touches the audio itself: it writes
// dsp-settings.json (the engine picks it up within 100 ms) and reads the level meters that the engine
// publishes through shared memory.
#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include <array>
#include <filesystem>
#include <vector>

#include "dsp/DspSettings.h"
#include "dsp/MeterMath.h"
#include "dsp/MeterShm.h"
#include "dsp/EqImport.h"
#include "dsp/Loudness.h"
#include "dsp/StudioModel.h"
#include "dsp/UndoHistory.h"

class BandBadge;
class BandSlider;
class CurveWidget;
class MeterWidget;
class ParamSlider;
class QCheckBox;
class QComboBox;
class QFrame;
class QSplitter;
class QTabWidget;
class SpectrumGrid;
class QLabel;
class QPushButton;

class StudioWindow : public QMainWindow
{
  Q_OBJECT
public:
  StudioWindow();
  void showStudio();   // bring the window to the front (also used when a second instance starts)

protected:
  void closeEvent(QCloseEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void buildUi(QWidget* central);
  QWidget* buildEqCard();
  QWidget* buildEqToolbar();   // EQ on/off, channel, preamp, reset, import/export: sits in the row with the show/hide buttons
  QWidget* buildSelectedBandRow();
  QWidget* buildCompressorCard();
  QWidget* buildUpmixCard();
  QWidget* buildSpectrumGridCard();
  void loadUpmixPresetList(const QString& select = QString());
  void onUpmixPresetChosen(int index);
  void onSaveUpmixPreset();
  static QString UpmixPresetsDir();
  QWidget* buildLevelsCard();

  void fitToScreen();               // keep the window inside the visible desktop
  void resetWindowSize();           // Ctrl+0: back to a normal size in the middle of the screen
  void applyPanelVisibility();      // show / hide the panels chosen in the "Показывать" row
  vac3dsp::DspParams EffectiveParams() const;   // what goes to the file: the settings, or "DSP off" during A/B
  void onAbToggled(bool on);
  void startLoudnessMatch();
  void finishLoudnessMatch(bool enoughSignal);
  void onImportEq();
  void onExportEq();
  void onAutoProfiles();
  void onAlgoChosen(int index);
  void updateUpmixStatus(bool alive, int engineMode);
  static QString ConfPath();
  void commitHistory();
  void undo();
  void redo();
  void updateUndoButtons();
  void setupShortcuts();
  void refreshUi();                 // model -> every control (no saving)
  void updateCurveAndLabels();
  void scheduleSave();
  void doSave();
  void pollFile();
  void pollMeters();
  void setStatus(const QString& text);

  void loadPresetList(const QString& select = QString());
  void onPresetChosen(int index);
  void onSavePreset();
  void onLookChosen(int index);
  void onChannelChosen(int index);
  void onBandMoved(int band, float db);
  void onBandFreqMoved(int band, float hz);
  void onBandWheel(int band, int steps);
  void selectBand(int band);
  void updateSelectedRow();

  static QString PresetsDir();
  static QString AppDir();

  vac3dsp::StudioModel model_;
  vac3dsp::SettingsFile file_;
  std::filesystem::path settingsPath_;
  vac3dsp::MeterShmClient shm_;
  bool updating_ = false;
  int sampleRate_ = 48000;

  QTimer saveTimer_;
  QTimer filePollTimer_;
  QTimer meterTimer_;

  // widgets
  QPushButton* dspToggle_ = nullptr;
  QPushButton* showCurveBtn_ = nullptr;
  QPushButton* showFadersBtn_ = nullptr;
  QPushButton* showCompBtn_ = nullptr;
  QPushButton* showLevelsBtn_ = nullptr;
  QPushButton* showSpectrumBtn_ = nullptr;
  QPushButton* abBtn_ = nullptr;
  QPushButton* matchBtn_ = nullptr;
  QPushButton* autoBtn_ = nullptr;
  QPushButton* undoBtn_ = nullptr;
  QPushButton* redoBtn_ = nullptr;
  QWidget* compCard_ = nullptr;
  QWidget* levelsCard_ = nullptr;
  QWidget* rightCol_ = nullptr;
  QWidget* eqPanel_ = nullptr;        // curve + faders (left side); hidden as a whole when both are off
  QSplitter* mainSplit_ = nullptr;    // equalizer | compressor + levels
  QSplitter* rightSplit_ = nullptr;   // compressor / upmix tabs over levels / spectrum tabs
  QSplitter* eqSplit_ = nullptr;      // curve over faders
  QFrame* curveCard_ = nullptr;
  QFrame* fadersCard_ = nullptr;
  QWidget* bandRow_ = nullptr;
  QLabel* engineDot_ = nullptr;
  QLabel* engineText_ = nullptr;
  QComboBox* presetCombo_ = nullptr;
  QComboBox* lookCombo_ = nullptr;
  QCheckBox* eqEnable_ = nullptr;
  QComboBox* channelCombo_ = nullptr;
  QLabel* channelHint_ = nullptr;
  ParamSlider* preamp_ = nullptr;
  CurveWidget* curve_ = nullptr;
  std::array<BandSlider*, vac3dsp::kBands> bandSliders_{};
  std::array<QLabel*, vac3dsp::kBands> bandValues_{};
  std::array<QLabel*, vac3dsp::kBands> bandFreqs_{};
  std::array<BandBadge*, vac3dsp::kBands> badges_{};
  QLabel* selTitle_ = nullptr;
  QComboBox* selType_ = nullptr;
  ParamSlider* selFreq_ = nullptr;
  ParamSlider* selWidth_ = nullptr;
  QLabel* selQ_ = nullptr;
  QCheckBox* compEnable_ = nullptr;
  QComboBox* orderCombo_ = nullptr;
  ParamSlider* threshold_ = nullptr;
  ParamSlider* ratio_ = nullptr;
  ParamSlider* attack_ = nullptr;
  ParamSlider* release_ = nullptr;
  ParamSlider* knee_ = nullptr;
  ParamSlider* makeup_ = nullptr;
  QCheckBox* lfeDetector_ = nullptr;
  QCheckBox* upmixEnable_ = nullptr;
  ParamSlider* centerWidth_ = nullptr;
  ParamSlider* centerFocus_ = nullptr;
  ParamSlider* centerLow_ = nullptr;
  ParamSlider* centerHigh_ = nullptr;
  ParamSlider* surLevel_ = nullptr;
  ParamSlider* surDelay_ = nullptr;
  QComboBox* algoCombo_ = nullptr;
  QCheckBox* lfeEnable_ = nullptr;
  ParamSlider* lfeLevel_ = nullptr;
  ParamSlider* lfeCut_ = nullptr;
  QComboBox* upmixPresetCombo_ = nullptr;
  QTabWidget* levelsTabs_ = nullptr;
  SpectrumGrid* grid_ = nullptr;
  float specInCh_[vac3dsp::kMaxChannels][vac3dsp::kSpecBands] = {};    // smoothed, per channel (spectrum grid)
  float specOutCh_[vac3dsp::kMaxChannels][vac3dsp::kSpecBands] = {};
  QLabel* upmixStatus_ = nullptr;
  std::array<MeterWidget*, vac3dsp::kMaxChannels> inMeters_{};
  std::array<MeterWidget*, vac3dsp::kMaxChannels> outMeters_{};
  MeterWidget* compMeter_ = nullptr;
  MeterWidget* limiterMeter_ = nullptr;
  QLabel* statusLabel_ = nullptr;

  // A/B compare, undo, spectrum
  bool abBypass_ = false;
  bool engineAlive_ = false;
  vac3dsp::LoudnessMatcher matcher_;      // "match loudness": averages input and output spectra for a few seconds
  bool matching_ = false;
  int matchFrames_ = 0;
  QElapsedTimer matchTimer_;
  QString lastActiveApp_;                 // application the engine reports as playing sound
  QString lastAutoProfile_;               // what the auto profiles applied last
  bool fitted_ = false;
  bool fitPending_ = false;
  vac3dsp::UndoHistory<vac3dsp::DspParams> history_{&vac3dsp::SameParams};
  QTimer undoTimer_;
  float specInRaw_[vac3dsp::kMaxChannels][vac3dsp::kSpecBands] = {};
  float specOutRaw_[vac3dsp::kMaxChannels][vac3dsp::kSpecBands] = {};
  std::vector<float> specInShown_, specOutShown_;
  bool specHave_ = false;
  int beatTick_ = 0;

  // meter state
  std::array<vac3dsp::MeterBallistics, vac3dsp::kMaxChannels> inBall_;
  std::array<vac3dsp::MeterBallistics, vac3dsp::kMaxChannels> outBall_;
  float compGr_ = 0.0f, limiterGr_ = 0.0f;
  uint32_t lastUpdates_ = 0;
  QElapsedTimer sinceFresh_;
  QElapsedTimer sinceTick_;
  int openTick_ = 0;
};
