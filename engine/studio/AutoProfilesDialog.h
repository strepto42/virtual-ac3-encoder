// AutoProfilesDialog.h — editor of auto-profiles.json: which DSP profile / upmix preset the engine switches to
// when an application plays sound. The first matching rule (top to bottom) wins.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <vector>

#include "dsp/AutoProfiles.h"

class QCheckBox;
class QComboBox;
class QSpinBox;
class QTableWidget;

class AutoProfilesDialog : public QDialog
{
public:
  AutoProfilesDialog(QWidget* parent, const QString& path, const QStringList& dspProfiles, const QStringList& upmixPresets,
                     const QString& currentApp);

private:
  QComboBox* makeCombo(const QStringList& names, const QString& selected) const;
  void addRow(const QString& process, const QString& dsp, const QString& upmix);
  std::vector<vac3dsp::AutoRule> rows() const;
  void setRows(const std::vector<vac3dsp::AutoRule>& rules);
  void moveRow(int delta);
  void save();

  QString path_;
  QStringList dsp_, upmix_;
  QCheckBox* enabled_ = nullptr;
  QSpinBox* hold_ = nullptr;
  QComboBox* defaultDsp_ = nullptr;
  QComboBox* defaultUpmix_ = nullptr;
  QTableWidget* table_ = nullptr;
};
