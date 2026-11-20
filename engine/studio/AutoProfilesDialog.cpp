#include "AutoProfilesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

AutoProfilesDialog::AutoProfilesDialog(QWidget* parent, const QString& path, const QStringList& dspProfiles,
                                       const QStringList& upmixPresets, const QString& currentApp)
    : QDialog(parent), path_(path), dsp_(dspProfiles), upmix_(upmixPresets)
{
  setWindowTitle(QStringLiteral("Автопрофили"));
  resize(640, 460);

  vac3dsp::AutoProfileConfig cfg;
  {
    QFile f(path_);
    if (f.open(QIODevice::ReadOnly))
    {
      const QByteArray data = f.readAll();
      std::string err;
      vac3dsp::ParseAutoProfiles(std::string(data.constData(), static_cast<size_t>(data.size())), cfg, &err);
    }
  }

  auto* v = new QVBoxLayout(this);
  v->setSpacing(8);
  auto* hint = new QLabel(QStringLiteral(
      "Когда приложение из списка играет звук, движок сам включает выбранный профиль DSP и пресет апмикса. "
      "Правила проверяются сверху вниз, срабатывает первое, чьё приложение сейчас играет. Переключение идёт, "
      "только когда ситуация держится заданное время, и не мешает ручному выбору до следующей смены. "
      "Работает, когда движок захватывает звук устройства (loopback)."));
  hint->setWordWrap(true);
  v->addWidget(hint);

  auto* top = new QHBoxLayout;
  enabled_ = new QCheckBox(QStringLiteral("Включить автопрофили"));
  enabled_->setChecked(cfg.enabled);
  hold_ = new QSpinBox;
  hold_->setRange(1, 60);
  hold_->setSuffix(QStringLiteral(" с"));
  hold_->setValue(static_cast<int>(cfg.holdSeconds));
  top->addWidget(enabled_);
  top->addStretch(1);
  top->addWidget(new QLabel(QStringLiteral("Переключать через:")));
  top->addWidget(hold_);
  v->addLayout(top);

  table_ = new QTableWidget(0, 3);
  table_->setHorizontalHeaderLabels(QStringList() << QStringLiteral("Приложение (exe)") << QStringLiteral("Профиль DSP")
                                                  << QStringLiteral("Пресет апмикса"));
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  v->addWidget(table_, 1);
  for (const vac3dsp::AutoRule& r : cfg.rules)
    addRow(QString::fromUtf8(r.process.c_str()), QString::fromUtf8(r.dsp.c_str()), QString::fromUtf8(r.upmix.c_str()));

  auto* buttons = new QHBoxLayout;
  auto* add = new QPushButton(QStringLiteral("Добавить"));
  auto* addCurrent = new QPushButton(currentApp.isEmpty() ? QStringLiteral("Добавить играющее сейчас")
                                                          : QStringLiteral("Добавить: ") + currentApp);
  addCurrent->setEnabled(!currentApp.isEmpty());
  auto* remove = new QPushButton(QStringLiteral("Удалить"));
  auto* up = new QPushButton(QStringLiteral("Выше"));
  auto* down = new QPushButton(QStringLiteral("Ниже"));
  buttons->addWidget(add);
  buttons->addWidget(addCurrent);
  buttons->addWidget(remove);
  buttons->addStretch(1);
  buttons->addWidget(up);
  buttons->addWidget(down);
  v->addLayout(buttons);

  auto* def = new QHBoxLayout;
  defaultDsp_ = makeCombo(dsp_, QString::fromUtf8(cfg.defaultDsp.c_str()));
  defaultUpmix_ = makeCombo(upmix_, QString::fromUtf8(cfg.defaultUpmix.c_str()));
  def->addWidget(new QLabel(QStringLiteral("Если играет что-то другое:")));
  def->addWidget(defaultDsp_, 1);
  def->addWidget(defaultUpmix_, 1);
  v->addLayout(def);

  auto* ok = new QHBoxLayout;
  auto* cancel = new QPushButton(QStringLiteral("Отмена"));
  auto* saveBtn = new QPushButton(QStringLiteral("Сохранить"));
  ok->addStretch(1);
  ok->addWidget(cancel);
  ok->addWidget(saveBtn);
  v->addLayout(ok);

  connect(add, &QPushButton::clicked, this, [this]() { addRow(QString(), QString(), QString()); });
  connect(addCurrent, &QPushButton::clicked, this, [this, currentApp]() { addRow(currentApp, QString(), QString()); });
  connect(remove, &QPushButton::clicked, this, [this]() {
    const int r = table_->currentRow();
    if (r >= 0)
      table_->removeRow(r);
  });
  connect(up, &QPushButton::clicked, this, [this]() { moveRow(-1); });
  connect(down, &QPushButton::clicked, this, [this]() { moveRow(1); });
  connect(cancel, &QPushButton::clicked, this, [this]() { reject(); });
  connect(saveBtn, &QPushButton::clicked, this, [this]() { save(); });
}

QComboBox* AutoProfilesDialog::makeCombo(const QStringList& names, const QString& selected) const
{
  auto* combo = new QComboBox;
  combo->addItem(QStringLiteral("(не менять)"), QString());
  for (const QString& n : names)
    combo->addItem(n, n);
  int index = 0;
  if (!selected.isEmpty())
  {
    index = combo->findData(selected);
    if (index < 0)   // a profile that no longer exists: keep it visible so that it is not lost silently
    {
      combo->addItem(selected + QStringLiteral(" (нет файла)"), selected);
      index = combo->count() - 1;
    }
  }
  combo->setCurrentIndex(index);
  return combo;
}

void AutoProfilesDialog::addRow(const QString& process, const QString& dsp, const QString& upmix)
{
  const int r = table_->rowCount();
  table_->insertRow(r);
  table_->setItem(r, 0, new QTableWidgetItem(process));
  table_->setCellWidget(r, 1, makeCombo(dsp_, dsp));
  table_->setCellWidget(r, 2, makeCombo(upmix_, upmix));
  table_->setCurrentCell(r, 0);
}

std::vector<vac3dsp::AutoRule> AutoProfilesDialog::rows() const
{
  std::vector<vac3dsp::AutoRule> out;
  for (int r = 0; r < table_->rowCount(); ++r)
  {
    vac3dsp::AutoRule rule;
    const QTableWidgetItem* item = table_->item(r, 0);
    rule.process = (item ? item->text().trimmed() : QString()).toUtf8().toStdString();
    const auto* d = qobject_cast<const QComboBox*>(table_->cellWidget(r, 1));
    const auto* u = qobject_cast<const QComboBox*>(table_->cellWidget(r, 2));
    rule.dsp = (d ? d->currentData().toString() : QString()).toUtf8().toStdString();
    rule.upmix = (u ? u->currentData().toString() : QString()).toUtf8().toStdString();
    out.push_back(rule);
  }
  return out;
}

void AutoProfilesDialog::setRows(const std::vector<vac3dsp::AutoRule>& rules)
{
  table_->setRowCount(0);
  for (const vac3dsp::AutoRule& r : rules)
    addRow(QString::fromUtf8(r.process.c_str()), QString::fromUtf8(r.dsp.c_str()), QString::fromUtf8(r.upmix.c_str()));
}

void AutoProfilesDialog::moveRow(int delta)
{
  const int r = table_->currentRow();
  const int target = r + delta;
  if (r < 0 || target < 0 || target >= table_->rowCount())
    return;
  std::vector<vac3dsp::AutoRule> list = rows();
  std::swap(list[static_cast<size_t>(r)], list[static_cast<size_t>(target)]);
  setRows(list);
  table_->setCurrentCell(target, 0);
}

void AutoProfilesDialog::save()
{
  vac3dsp::AutoProfileConfig cfg;
  cfg.enabled = enabled_->isChecked();
  cfg.holdSeconds = hold_->value();
  cfg.defaultDsp = defaultDsp_->currentData().toString().toUtf8().toStdString();
  cfg.defaultUpmix = defaultUpmix_->currentData().toString().toUtf8().toStdString();
  for (const vac3dsp::AutoRule& r : rows())
    if (!r.process.empty())
      cfg.rules.push_back(r);
  const std::string text = vac3dsp::FormatAutoProfiles(cfg);
  QSaveFile out(path_);
  if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), static_cast<qint64>(text.size())) < 0 || !out.commit())
  {
    QMessageBox::warning(this, windowTitle(), QStringLiteral("Не удалось сохранить: ") + out.errorString());
    return;
  }
  accept();
}
