// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/unit_list_model.h"

#include <QStringList>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <functional>
#include <utility>

namespace ovt::app {

namespace {

constexpr int kObuSequenceHeader = 1;
constexpr int kObuTemporalDelimiter = 2;
constexpr int kObuFrameHeader = 3;
constexpr int kObuTileGroup = 4;
constexpr int kObuMetadata = 5;
constexpr int kObuFrame = 6;
constexpr int kObuRedundantFrameHeader = 7;
constexpr int kObuTileList = 8;
constexpr int kObuPadding = 15;

UnitClass classify_h264(int type) {
  switch (type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
      return UnitClass::Vcl;
    case 6:
      return UnitClass::Sei;
    case 7:
    case 8:
    case 13:  // SPS extension
    case 15:  // subset SPS
      return UnitClass::ParameterSet;
    case 9:
    case 12:  // filler data
      return UnitClass::Aud;
    default:
      return UnitClass::Other;
  }
}

UnitClass classify_h265(int type) {
  if (type >= 0 && type <= 31) {
    return UnitClass::Vcl;
  }
  if (type >= 32 && type <= 34) {
    return UnitClass::ParameterSet;  // VPS, SPS, PPS
  }
  if (type == 39 || type == 40) {
    return UnitClass::Sei;
  }
  if (type == 35 || type == 38) {
    return UnitClass::Aud;  // AUD, filler data
  }
  return UnitClass::Other;
}

UnitClass classify_av1(int type) {
  switch (type) {
    case kObuFrameHeader:
    case kObuTileGroup:
    case kObuFrame:
    case kObuRedundantFrameHeader:
    case kObuTileList:
      return UnitClass::Vcl;
    case kObuSequenceHeader:
      return UnitClass::ParameterSet;
    case kObuMetadata:
      return UnitClass::Sei;
    case kObuTemporalDelimiter:
    case kObuPadding:
      return UnitClass::Aud;
    default:
      return UnitClass::Other;
  }
}

const char* class_name(UnitClass c) {
  switch (c) {
    case UnitClass::Vcl:
      return "vcl";
    case UnitClass::ParameterSet:
      return "parameterSet";
    case UnitClass::Sei:
      return "sei";
    case UnitClass::Aud:
      return "aud";
    case UnitClass::Other:
      break;
  }
  return "other";
}

QString number(std::uint64_t value, bool hex) {
  if (hex) {
    return QStringLiteral("0x") + QString::number(value, 16).rightJustified(8, QLatin1Char('0')).toUpper();
  }
  return QString::number(value);
}

}  // namespace

UnitClass classify_unit(fb::Codec codec, int type, bool has_frame) {
  if (has_frame) {
    return UnitClass::Vcl;
  }
  switch (codec) {
    case fb::Codec::H264:
      return classify_h264(type);
    case fb::Codec::H265:
      return classify_h265(type);
    case fb::Codec::AV1:
      return classify_av1(type);
    default:
      return UnitClass::Other;
  }
}

UnitListModel::UnitListModel(QObject* parent) : QAbstractTableModel(parent) {}

UnitListModel::~UnitListModel() = default;

void UnitListModel::set_stream_index(StreamIndexModel* model) {
  if (model == stream_index_) {
    return;
  }
  if (stream_index_ != nullptr) {
    QObject::disconnect(stream_index_, nullptr, this, nullptr);
  }
  stream_index_ = model;
  if (stream_index_ != nullptr) {
    QObject::connect(stream_index_, &StreamIndexModel::indexChanged, this, &UnitListModel::on_index_changed);
    QObject::connect(stream_index_, &QObject::destroyed, this, [this] {
      stream_index_ = nullptr;
      setHandle(nullptr);
      Q_EMIT streamIndexChanged();
    });
    on_index_changed();
  } else {
    setHandle(nullptr);
  }
  Q_EMIT streamIndexChanged();
}

void UnitListModel::on_index_changed() {
  setHandle(stream_index_ != nullptr ? stream_index_->handle() : nullptr);
}

void UnitListModel::set_selection(SelectionModel* selection) {
  if (selection == selection_) {
    return;
  }
  if (selection_ != nullptr) {
    QObject::disconnect(selection_, nullptr, this, nullptr);
  }
  selection_ = selection;
  if (selection_ != nullptr) {
    QObject::connect(selection_, &QObject::destroyed, this, [this] {
      selection_ = nullptr;
      Q_EMIT selectionChanged();
    });
  }
  Q_EMIT selectionChanged();
}

void UnitListModel::setHandle(std::shared_ptr<const StreamIndexHandle> handle) {
  beginResetModel();
  handle_ = std::move(handle);
  classes_.clear();
  if (handle_) {
    const fb::StreamIndex& index = handle_->index();
    const auto* units = index.units();
    if (units != nullptr) {
      classes_.reserve(units->size());
      for (flatbuffers::uoffset_t i = 0; i < units->size(); ++i) {
        const fb::Unit& u = *units->Get(i);
        const bool has_frame = u.frame() != nullptr && u.frame()->decode_index() >= 0;
        classes_.push_back(static_cast<std::uint8_t>(classify_unit(index.codec(), u.type(), has_frame)));
      }
    }
  }
  rebuild();
  endResetModel();
  Q_EMIT streamIndexChanged();
}

int UnitListModel::unit_count() const noexcept {
  return static_cast<int>(classes_.size());
}

const fb::Unit* UnitListModel::unit_ptr(std::uint32_t unit_index) const {
  if (!handle_) {
    return nullptr;
  }
  const auto* units = handle_->index().units();
  if (units == nullptr || unit_index >= units->size()) {
    return nullptr;
  }
  return units->Get(unit_index);
}

UnitClass UnitListModel::class_of(std::uint32_t unit_index) const {
  return unit_index < classes_.size() ? static_cast<UnitClass>(classes_[unit_index]) : UnitClass::Other;
}

QString UnitListModel::type_label(const fb::Unit& unit) {
  if (unit.type_name() != nullptr && unit.type_name()->size() != 0) {
    return QString::fromUtf8(unit.type_name()->c_str(), static_cast<qsizetype>(unit.type_name()->size()));
  }
  return QStringLiteral("type %1").arg(unit.type());
}

bool UnitListModel::matches(std::uint32_t unit_index) const {
  const fb::Unit* u = unit_ptr(unit_index);
  if (u == nullptr) {
    return false;
  }
  const UnitClass cls = class_of(unit_index);
  switch (kind_) {
    case KindVcl:
      if (cls != UnitClass::Vcl) {
        return false;
      }
      break;
    case KindNonVcl:
      if (cls == UnitClass::Vcl) {
        return false;
      }
      break;
    case KindSei:
      if (cls != UnitClass::Sei) {
        return false;
      }
      break;
    case KindErrors:
      if (u->diagnostics() == nullptr || u->diagnostics()->size() == 0) {
        return false;
      }
      break;
    default:
      break;
  }
  return text_.isEmpty() || type_label(*u).contains(text_, Qt::CaseInsensitive);
}

void UnitListModel::rebuild() {
  rows_.clear();
  rows_.reserve(classes_.size());
  for (std::uint32_t i = 0; i < classes_.size(); ++i) {
    if (matches(i)) {
      rows_.push_back(i);
    }
  }
  sort_rows();
}

bool UnitListModel::less(std::uint32_t a, std::uint32_t b) const {
  const fb::Unit* ua = unit_ptr(a);
  const fb::Unit* ub = unit_ptr(b);
  if (ua == nullptr || ub == nullptr) {
    return false;
  }
  const auto span_of = [](const fb::Unit& u) { return u.file_span(); };
  switch (sort_column_) {
    case ColType: {
      const int c = type_label(*ua).compare(type_label(*ub));
      return c < 0;
    }
    case ColLayerTemporal:
      return std::pair(ua->layer_id(), ua->temporal_id()) < std::pair(ub->layer_id(), ub->temporal_id());
    case ColOffset: {
      const auto* sa = span_of(*ua);
      const auto* sb = span_of(*ub);
      return (sa != nullptr ? sa->offset() : 0U) < (sb != nullptr ? sb->offset() : 0U);
    }
    case ColSize: {
      const auto* sa = span_of(*ua);
      const auto* sb = span_of(*ub);
      return (sa != nullptr ? sa->length() : 0U) < (sb != nullptr ? sb->length() : 0U);
    }
    case ColFrame: {
      const int fa = ua->frame() != nullptr ? ua->frame()->decode_index() : -1;
      const int fb_decode = ub->frame() != nullptr ? ub->frame()->decode_index() : -1;
      return fa < fb_decode;
    }
    case ColSample:
      return ua->sample_index() < ub->sample_index();
    case ColFlags: {
      const auto da = ua->diagnostics() != nullptr ? ua->diagnostics()->size() : 0U;
      const auto db = ub->diagnostics() != nullptr ? ub->diagnostics()->size() : 0U;
      return da < db;
    }
    default:
      return a < b;
  }
}

void UnitListModel::sort_rows() {
  if (sort_column_ == ColNumber) {
    if (sort_descending_) {
      std::reverse(rows_.begin(), rows_.end());  // rows_ is in ascending index order here
    }
    return;
  }
  // Stable: equal rows keep the decode order, in either direction.
  if (sort_descending_) {
    std::stable_sort(rows_.begin(), rows_.end(), [this](std::uint32_t a, std::uint32_t b) { return less(b, a); });
  } else {
    std::stable_sort(rows_.begin(), rows_.end(), [this](std::uint32_t a, std::uint32_t b) { return less(a, b); });
  }
}

void UnitListModel::set_kind(int kind) {
  if (kind == kind_) {
    return;
  }
  beginResetModel();
  kind_ = kind;
  rebuild();
  endResetModel();
  Q_EMIT filterChanged();
}

void UnitListModel::set_text(const QString& text) {
  if (text == text_) {
    return;
  }
  beginResetModel();
  text_ = text;
  rebuild();
  endResetModel();
  Q_EMIT filterChanged();
}

void UnitListModel::set_hex_numbers(bool hex) {
  if (hex == hex_numbers_) {
    return;
  }
  hex_numbers_ = hex;
  if (!rows_.empty()) {
    Q_EMIT dataChanged(index(0, ColOffset), index(static_cast<int>(rows_.size()) - 1, ColSize));
  }
  Q_EMIT hexNumbersChanged();
}

int UnitListModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int UnitListModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

QString UnitListModel::cell(std::uint32_t unit_index, int column) const {
  const fb::Unit* u = unit_ptr(unit_index);
  if (u == nullptr) {
    return {};
  }
  switch (column) {
    case ColNumber:
      return QString::number(unit_index);
    case ColType:
      return type_label(*u);
    case ColLayerTemporal:
      return QStringLiteral("%1/%2").arg(u->layer_id()).arg(u->temporal_id());
    case ColOffset:
      return u->file_span() != nullptr ? number(u->file_span()->offset(), hex_numbers_) : QString();
    case ColSize:
      return u->file_span() != nullptr ? number(u->file_span()->length(), hex_numbers_) : QString();
    case ColFrame:
      if (u->frame() == nullptr || u->frame()->decode_index() < 0) {
        return {};
      }
      return QStringLiteral("%1/%2").arg(u->frame()->decode_index()).arg(u->frame()->display_index());
    case ColSample:
      return u->sample_index() >= 0 ? QString::number(u->sample_index()) : QString();
    case ColFlags: {
      QStringList flags;
      if (u->frame() != nullptr && u->frame()->decode_index() >= 0 && handle_) {
        const auto* frames = handle_->index().frames();
        const auto decode = static_cast<flatbuffers::uoffset_t>(u->frame()->decode_index());
        if (frames != nullptr && decode < frames->size() && frames->Get(decode)->is_rap()) {
          flags.push_back(QStringLiteral("RAP"));
        }
      }
      const auto diagnostics = u->diagnostics() != nullptr ? u->diagnostics()->size() : 0U;
      if (diagnostics != 0) {
        flags.push_back(QString::fromUtf8("\xE2\x9A\xA0") + QString::number(diagnostics));
      }
      return flags.join(QLatin1Char(' '));
    }
    default:
      return {};
  }
}

QVariant UnitListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size() ||
      index.column() < 0 || index.column() >= ColumnCount) {
    return {};
  }
  const std::uint32_t unit = rows_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case Qt::DisplayRole:
      return cell(unit, index.column());
    case ClassRole:
      return QString::fromLatin1(class_name(class_of(unit)));
    case UnitIndexRole:
      return static_cast<int>(unit);
    case IsVclRole:
      return class_of(unit) == UnitClass::Vcl;
    case DiagnosticCountRole: {
      const fb::Unit* u = unit_ptr(unit);
      return u != nullptr && u->diagnostics() != nullptr ? static_cast<int>(u->diagnostics()->size()) : 0;
    }
    case Qt::TextAlignmentRole:
      return index.column() == ColType || index.column() == ColFlags ? int(Qt::AlignLeft | Qt::AlignVCenter)
                                                                     : int(Qt::AlignRight | Qt::AlignVCenter);
    default:
      return {};
  }
}

QVariant UnitListModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  switch (section) {
    case ColNumber:
      return QStringLiteral("#");
    case ColType:
      return QStringLiteral("Type");
    case ColLayerTemporal:
      return QStringLiteral("L/T");
    case ColOffset:
      return QStringLiteral("Offset");
    case ColSize:
      return QStringLiteral("Size");
    case ColFrame:
      return QStringLiteral("Frame");
    case ColSample:
      return QStringLiteral("Sample");
    case ColFlags:
      return QStringLiteral("Flags");
    default:
      return {};
  }
}

QHash<int, QByteArray> UnitListModel::roleNames() const {
  return {{Qt::DisplayRole, "display"},
          {ClassRole, "unitClass"},
          {UnitIndexRole, "unitIndex"},
          {DiagnosticCountRole, "diagnosticCount"},
          {IsVclRole, "isVcl"}};
}

void UnitListModel::sort(int column, Qt::SortOrder order) {
  if (column < 0 || column >= ColumnCount) {
    return;
  }
  beginResetModel();
  sort_column_ = column;
  sort_descending_ = order == Qt::DescendingOrder;
  rebuild();
  endResetModel();
  Q_EMIT sortChanged();
}

void UnitListModel::toggleSort(int column) {
  if (column == sort_column_) {
    sort(column, sort_descending_ ? Qt::AscendingOrder : Qt::DescendingOrder);
  } else {
    sort(column, Qt::AscendingOrder);
  }
}

int UnitListModel::unitAt(int row) const {
  if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
    return -1;
  }
  return static_cast<int>(rows_[static_cast<std::size_t>(row)]);
}

int UnitListModel::rowOfUnit(int unit_index) const {
  if (unit_index < 0) {
    return -1;
  }
  const auto wanted = static_cast<std::uint32_t>(unit_index);
  if (sort_column_ == ColNumber) {
    // rows_ is sorted by unit index, ascending or descending.
    const auto it = sort_descending_ ? std::lower_bound(rows_.begin(), rows_.end(), wanted, std::greater<>())
                                     : std::lower_bound(rows_.begin(), rows_.end(), wanted);
    return it != rows_.end() && *it == wanted ? static_cast<int>(it - rows_.begin()) : -1;
  }
  const auto it = std::find(rows_.begin(), rows_.end(), wanted);
  return it == rows_.end() ? -1 : static_cast<int>(it - rows_.begin());
}

void UnitListModel::select(int row, const QString& origin) {
  const int unit = unitAt(row);
  if (unit >= 0 && selection_ != nullptr) {
    selection_->setUnit(unit, origin);
  }
}

void UnitListModel::activate(int row, const QString& origin) {
  const int unit = unitAt(row);
  if (unit < 0 || selection_ == nullptr) {
    return;
  }
  const fb::Unit* u = unit_ptr(static_cast<std::uint32_t>(unit));
  if (u != nullptr && u->frame() != nullptr && u->frame()->decode_index() >= 0) {
    selection_->setFrame(FrameId{u->frame()->decode_index(), u->frame()->display_index()}, origin);
  } else {
    selection_->setUnit(unit, origin);
  }
}

QString UnitListModel::rowText(int row) const {
  const int unit = unitAt(row);
  if (unit < 0) {
    return {};
  }
  QStringList cells;
  for (int c = 0; c < ColumnCount; ++c) {
    cells.push_back(cell(static_cast<std::uint32_t>(unit), c));
  }
  return cells.join(QLatin1Char('\t'));
}

}  // namespace ovt::app
