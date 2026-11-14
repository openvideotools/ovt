// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/diagnostics_model.h"

#include <QModelIndex>
#include <QObject>
#include <QStringList>

#include <algorithm>
#include <iterator>
#include <utility>

namespace ovt::app {

QString DiagnosticLocation::text() const {
  switch (kind) {
    case Kind::UnitBits:
      return QStringLiteral("unit #%1 bit %2").arg(unit_index).arg(bit_offset);
    case Kind::Box:
      return QStringLiteral("box %1").arg(box_path);
    case Kind::FrameBlock:
      return QStringLiteral("frame #%1 block %2").arg(decode_index).arg(block_id);
    case Kind::FrameProbe:
      return QStringLiteral("frame #%1 at (%2, %3)").arg(display_index).arg(probe.x()).arg(probe.y());
    case Kind::FileOffset:
      return QStringLiteral("file offset 0x%1").arg(file_offset, 0, 16);
    case Kind::None:
      break;
  }
  return {};
}

QString DiagnosticEntry::family() const {
  const qsizetype dash = code.indexOf(QLatin1Char('-'));
  return dash > 0 ? code.left(dash) : code;
}

QString DiagnosticsModel::severity_name(DiagSeverity severity) {
  switch (severity) {
    case DiagSeverity::Info:
      return QStringLiteral("Info");
    case DiagSeverity::Warning:
      return QStringLiteral("Warning");
    case DiagSeverity::Error:
      return QStringLiteral("Error");
    case DiagSeverity::Fatal:
      return QStringLiteral("Fatal");
  }
  return {};
}

QString DiagnosticsModel::plain_text(const DiagnosticEntry& entry) {
  QString text = QStringLiteral("%1 %2 %3").arg(severity_name(entry.severity).toUpper(), entry.code, entry.message);
  if (entry.location.kind != DiagnosticLocation::Kind::None) {
    text += QStringLiteral(" (%1)").arg(entry.location.text());
  }
  if (!entry.spec_ref.isEmpty()) {
    text += QStringLiteral(" [%1]").arg(entry.spec_ref);
  }
  return text;
}

DiagnosticsModel::DiagnosticsModel() : filter_(new DiagnosticsFilterModel(this)) {}

DiagnosticsModel::~DiagnosticsModel() = default;

void DiagnosticsModel::recount() {
  errors_ = static_cast<int>(
      std::ranges::count_if(entries_, [](const Row& r) { return r.entry.severity >= DiagSeverity::Error; }));
  warnings_ = static_cast<int>(
      std::ranges::count_if(entries_, [](const Row& r) { return r.entry.severity == DiagSeverity::Warning; }));
  Q_EMIT countsChanged();
}

void DiagnosticsModel::enforce_cap() {
  if (entries_.size() <= kCapacity) {
    return;
  }
  const auto extra = static_cast<int>(entries_.size() - kCapacity);
  beginRemoveRows({}, 0, extra - 1);
  entries_.erase(entries_.begin(), entries_.begin() + extra);
  endRemoveRows();
  dropped_ += extra;
}

void DiagnosticsModel::setSource(const QString& source, std::vector<DiagnosticEntry> entries) {
  beginResetModel();
  std::erase_if(entries_, [&](const Row& r) { return r.source == source; });
  for (auto& entry : entries) {
    entries_.push_back({source, std::move(entry)});
  }
  endResetModel();
  const std::size_t before = entries_.size();
  if (before > kCapacity) {
    beginResetModel();
    dropped_ += static_cast<int>(before - kCapacity);
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(before - kCapacity));
    endResetModel();
  }
  recount();
}

void DiagnosticsModel::removeSource(const QString& source) {
  setSource(source, {});
}

void DiagnosticsModel::addGui(int severity, const QString& code, const QString& message) {
  DiagnosticEntry entry;
  entry.severity = static_cast<DiagSeverity>(std::clamp(severity, 0, 3));
  entry.code = code;
  entry.message = message;
  const int row = static_cast<int>(entries_.size());
  beginInsertRows({}, row, row);
  entries_.push_back({QStringLiteral("gui"), std::move(entry)});
  endInsertRows();
  enforce_cap();
  recount();
}

void DiagnosticsModel::clear() {
  beginResetModel();
  entries_.clear();
  dropped_ = 0;
  endResetModel();
  recount();
}

bool DiagnosticsModel::activate(int row) {
  if (sink_ == nullptr || row < 0 || row >= static_cast<int>(entries_.size())) {
    return false;
  }
  const DiagnosticLocation& loc = entries_[static_cast<std::size_t>(row)].entry.location;
  const QString origin = QString::fromLatin1(kOrigin);
  switch (loc.kind) {
    case DiagnosticLocation::Kind::UnitBits:
      sink_->showUnitBits(loc.unit_index, loc.bit_offset, loc.bit_length, origin);
      return true;
    case DiagnosticLocation::Kind::Box:
      sink_->showBox(loc.box_path, origin);
      return true;
    case DiagnosticLocation::Kind::FrameBlock:
      sink_->showFrameBlock(loc.decode_index, loc.display_index, loc.block_id, origin);
      return true;
    case DiagnosticLocation::Kind::FrameProbe:
      sink_->showFrameProbe(loc.decode_index, loc.display_index, loc.probe, origin);
      return true;
    case DiagnosticLocation::Kind::FileOffset:
      sink_->showFileOffset(loc.file_offset, origin);
      return true;
    case DiagnosticLocation::Kind::None:
      break;
  }
  return false;
}

int DiagnosticsModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int DiagnosticsModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(ColumnCount);
}

QVariant DiagnosticsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(entries_.size())) {
    return {};
  }
  const DiagnosticEntry& e = entries_[static_cast<std::size_t>(index.row())].entry;
  switch (role) {
    case Qt::DisplayRole:
      switch (index.column()) {
        case SeverityColumn:
          return severity_name(e.severity);
        case CodeColumn:
          return e.code;
        case MessageColumn:
          return e.message;
        case LocationColumn:
          return e.location.text();
        case SpecRefColumn:
          return e.spec_ref;
        default:
          return {};
      }
    case SeverityRole:
      return static_cast<int>(e.severity);
    case CodeRole:
      return e.code;
    case MessageRole:
      return e.message;
    case LocationRole:
      return e.location.text();
    case SpecRefRole:
      return e.spec_ref;
    case ModuleRole:
      return e.family();
    case NavigableRole:
      return e.location.kind != DiagnosticLocation::Kind::None;
    default:
      return {};
  }
}

QVariant DiagnosticsModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  static const QStringList titles{QStringLiteral("Severity"), QStringLiteral("Code"), QStringLiteral("Message"),
                                  QStringLiteral("Location"), QStringLiteral("Spec ref")};
  return section >= 0 && section < titles.size() ? QVariant(titles.at(section)) : QVariant();
}

QHash<int, QByteArray> DiagnosticsModel::roleNames() const {
  return {{Qt::DisplayRole, "display"}, {SeverityRole, "severity"},  {CodeRole, "code"},
          {MessageRole, "message"},     {LocationRole, "location"},  {SpecRefRole, "specRef"},
          {ModuleRole, "module"},       {NavigableRole, "navigable"}};
}

DiagnosticsFilterModel::DiagnosticsFilterModel(DiagnosticsModel* source)
    : QSortFilterProxyModel(source), source_(source) {
  setSourceModel(source);
}

void DiagnosticsFilterModel::setMinSeverity(int severity) {
  min_severity_ = severity;
  invalidateFilter();
  Q_EMIT filterChanged();
}

void DiagnosticsFilterModel::setModule(const QString& module) {
  module_ = module;
  invalidateFilter();
  Q_EMIT filterChanged();
}

void DiagnosticsFilterModel::setText(const QString& text) {
  text_ = text;
  invalidateFilter();
  Q_EMIT filterChanged();
}

bool DiagnosticsFilterModel::filterAcceptsRow(int row, const QModelIndex& /*parent*/) const {
  const DiagnosticEntry& e = source_->entry(row);
  if (static_cast<int>(e.severity) < min_severity_) {
    return false;
  }
  if (!module_.isEmpty() && e.family() != module_) {
    return false;
  }
  return text_.isEmpty() || DiagnosticsModel::plain_text(e).contains(text_, Qt::CaseInsensitive);
}

bool DiagnosticsFilterModel::activate(int row) {
  const QModelIndex source = mapToSource(index(row, 0));
  return source.isValid() && source_->activate(source.row());
}

QString DiagnosticsFilterModel::rowText(int row) const {
  const QModelIndex source = mapToSource(index(row, 0));
  return source.isValid() ? DiagnosticsModel::plain_text(source_->entry(source.row())) : QString();
}

QString DiagnosticsFilterModel::allText() const {
  QStringList lines;
  for (int row = 0; row < rowCount(); ++row) {
    lines << rowText(row);
  }
  return lines.join(QLatin1Char('\n'));
}

}  // namespace ovt::app
