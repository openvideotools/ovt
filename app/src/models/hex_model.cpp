// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/hex_model.h"

#include <QMetaObject>

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstring>

#include "goto_parser.h"
#include "ovt/status.h"

namespace ovt::app {

// ---- byte sources -----------------------------------------------------------------------------------------------

std::size_t MemoryByteSource::read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const {
  if (offset >= bytes_.size()) {
    return 0;
  }
  const std::size_t n = std::min<std::uint64_t>(len, bytes_.size() - offset);
  std::memcpy(dst, std::span(bytes_).subspan(offset, n).data(), n);
  return n;
}

ContainerByteSource::ContainerByteSource(ovt_Container* container) : container_(container) {}

std::uint64_t ContainerByteSource::size() const {
  return ovt_container_size(container_);
}

std::size_t ContainerByteSource::read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const {
  constexpr std::size_t kMaxCall = std::size_t{1} << 20U;  // the byte service's limit per call
  std::size_t done = 0;
  while (done < len && offset + done < size()) {
    const auto want = static_cast<std::uint32_t>(std::min(len - done, kMaxCall));
    ovt_Buffer buffer{};
    if (ovt_container_bytes(container_, offset + done, want, &buffer) != OVT_OK || buffer.data == nullptr) {
      break;
    }
    const std::size_t got = std::min<std::size_t>(buffer.size, want);
    std::memcpy(dst + done, buffer.data, got);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    ovt_buffer_free(&buffer);
    done += got;
    if (got == 0) {
      break;
    }
  }
  return done;
}

// ---- bands ------------------------------------------------------------------------------------

std::vector<HexBand> hex_bands_from_index(const fb::StreamIndex& index) {
  std::vector<HexBand> bands;
  const auto* units = index.units();
  if (units == nullptr) {
    return bands;
  }
  bands.reserve(units->size());
  for (flatbuffers::uoffset_t i = 0; i < units->size(); ++i) {
    const fb::Unit* unit = units->Get(i);
    if (unit == nullptr || unit->file_span() == nullptr) {
      continue;
    }
    HexBand band;
    band.kind = HexBand::Kind::Unit;
    band.index = static_cast<int>(i);
    band.offset = unit->file_span()->offset();
    band.length = unit->file_span()->length();
    const fb::ByteSpan* prefix = unit->prefix_span();
    if (prefix != nullptr && prefix->length() > 0 && prefix->offset() + prefix->length() == band.offset) {
      band.offset = prefix->offset();
      band.prefix = prefix->length();
    }
    bands.push_back(band);
  }
  std::stable_sort(bands.begin(), bands.end(), [](const HexBand& a, const HexBand& b) { return a.offset < b.offset; });
  return bands;
}

// ---- HexModel: inputs -------------------------------------------------------------------------------------------

HexModel::HexModel(QObject* parent) : QAbstractTableModel(parent) {}

HexModel::~HexModel() = default;

void HexModel::set_stream_index(StreamIndexModel* model) {
  if (stream_index_ == model) {
    return;
  }
  if (stream_index_ != nullptr) {
    disconnect(stream_index_, nullptr, this, nullptr);
  }
  stream_index_ = model;
  if (stream_index_ != nullptr) {
    connect(stream_index_, &StreamIndexModel::indexChanged, this,
            [this] { apply_index(stream_index_->selectionIndex(), stream_index_->handle()); });
    apply_index(stream_index_->selectionIndex(), stream_index_->handle());
  } else {
    apply_index(nullptr, nullptr);
  }
  Q_EMIT streamIndexChanged();
}

void HexModel::set_selection(SelectionModel* selection) {
  if (selection_ == selection) {
    return;
  }
  if (selection_ != nullptr) {
    disconnect(selection_, nullptr, this, nullptr);
  }
  selection_ = selection;
  if (selection_ != nullptr) {
    connect(selection_, &SelectionModel::changed, this, &HexModel::on_selection_changed);
  }
  recompute_highlight();
  Q_EMIT selectionChanged();
}

void HexModel::setSource(std::shared_ptr<const ByteSource> source) {
  beginResetModel();
  source_ = std::move(source);
  pages_.clear();
  page_order_.clear();
  pending_.clear();
  endResetModel();
  rebuild_view();
  Q_EMIT sourceChanged();
}

void HexModel::setHandle(std::shared_ptr<const StreamIndexHandle> handle) {
  std::shared_ptr<const SelectionIndex> index;
  if (handle) {
    index = std::make_shared<StreamIndexAdapter>(handle);
  }
  apply_index(std::move(index), std::move(handle));
}

void HexModel::apply_index(std::shared_ptr<const SelectionIndex> index,
                           std::shared_ptr<const StreamIndexHandle> handle) {
  index_ = std::move(index);
  handle_ = std::move(handle);
  field_cache_.clear();
  unit_bands_ = handle_ ? hex_bands_from_index(handle_->index()) : std::vector<HexBand>();
  rebuild_bands();
  rebuild_view();
  recompute_highlight();
}

void HexModel::setBoxBands(std::vector<HexBand> boxes) {
  std::stable_sort(boxes.begin(), boxes.end(), [](const HexBand& a, const HexBand& b) { return a.offset < b.offset; });
  box_bands_ = std::move(boxes);
  rebuild_bands();
  if (rowCount() > 0) {
    Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {BandRole});
  }
}

void HexModel::setFieldProvider(HexFieldProvider provider) {
  field_provider_ = std::move(provider);
  field_cache_.clear();
}

void HexModel::setBoxSpan(ByteSpan span) {
  box_span_ = span;
  if (scope_ == ScopeBox) {
    rebuild_view();
  }
}

void HexModel::rebuild_bands() {
  // Box scope and container-only files show boxes; stream files show their units.
  bands_ = (scope_ == ScopeBox || unit_bands_.empty()) ? box_bands_ : unit_bands_;
}

std::uint64_t HexModel::file_size() const {
  return source_ ? source_->size() : 0;
}

void HexModel::set_scope(int scope) {
  const auto next = static_cast<Scope>(std::clamp(scope, 0, 2));
  if (next == scope_) {
    return;
  }
  scope_ = next;
  rebuild_bands();
  rebuild_view();
  Q_EMIT scopeChanged();
}

void HexModel::set_show_ribbon(bool show) {
  if (show != show_ribbon_) {
    show_ribbon_ = show;
    Q_EMIT ribbonChanged();
  }
}

void HexModel::rebuild_view() {
  std::uint64_t begin = 0;
  std::uint64_t end = file_size();
  if (scope_ == ScopeUnit && selection_ != nullptr && selection_->unit_index() >= 0) {
    for (const HexBand& band : unit_bands_) {
      if (band.index == selection_->unit_index()) {
        begin = band.offset;
        end = std::min(end, band.end());
        break;
      }
    }
  } else if (scope_ == ScopeBox && box_span_.length > 0) {
    begin = box_span_.offset;
    end = std::min(end, box_span_.offset + box_span_.length);
  }
  if (begin == view_begin_ && end == view_end_) {
    return;
  }
  beginResetModel();
  view_begin_ = begin;
  view_end_ = end;
  endResetModel();
}

// ---- HexModel: the table ----------------------------------------------------------------------------------------

int HexModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() || view_end_ <= view_begin_) {
    return 0;
  }
  const std::uint64_t first = view_begin_ / kBytesPerRow;
  const std::uint64_t last = (view_end_ + kBytesPerRow - 1) / kBytesPerRow;
  return static_cast<int>(std::min<std::uint64_t>(last - first, 0x7FFFFFFFU));
}

int HexModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

qlonglong HexModel::offsetOfRow(int row) const {
  const std::uint64_t first_row = view_begin_ / kBytesPerRow;
  const std::uint64_t offset = (first_row + static_cast<std::uint64_t>(row)) * kBytesPerRow;
  return static_cast<qlonglong>(offset);
}

int HexModel::rowOfOffset(qlonglong offset) const {
  const auto off = static_cast<std::uint64_t>(offset);
  if (offset < 0 || off < view_begin_ || off >= view_end_) {
    return -1;
  }
  return static_cast<int>(off / kBytesPerRow - view_begin_ / kBytesPerRow);
}

QString HexModel::offsetText(qlonglong offset) const {
  const int width = file_size() > (std::uint64_t{1} << 32U) ? 16 : 8;
  return QStringLiteral("%1").arg(static_cast<qulonglong>(offset), width, 16, QLatin1Char('0')).toUpper();
}

const HexBand* HexModel::band_at(std::uint64_t offset, std::size_t* ordinal) const {
  auto it = std::upper_bound(bands_.begin(), bands_.end(), offset,
                             [](std::uint64_t value, const HexBand& band) { return value < band.offset; });
  if (it == bands_.begin()) {
    return nullptr;
  }
  --it;
  if (offset >= it->end()) {
    return nullptr;
  }
  if (ordinal != nullptr) {
    *ordinal = static_cast<std::size_t>(it - bands_.begin());
  }
  return &*it;
}

int HexModel::bandTintAt(qlonglong offset) const {
  std::size_t ordinal = 0;
  const HexBand* band = band_at(static_cast<std::uint64_t>(offset), &ordinal);
  if (band == nullptr) {
    return TintNone;
  }
  const bool in_prefix = static_cast<std::uint64_t>(offset) < band->offset + band->prefix;
  if (band->kind == HexBand::Kind::Unit) {
    return in_prefix ? TintUnitPrefix : (ordinal % 2 == 0 ? TintUnitA : TintUnitB);
  }
  return in_prefix ? TintBoxHeader : (ordinal % 2 == 0 ? TintBoxA : TintBoxB);
}

bool HexModel::is_epb(std::uint64_t offset) const {
  if (!index_) {
    return false;
  }
  const int unit_index = index_->unit_at_byte(offset);
  if (unit_index < 0) {
    return false;
  }
  const std::optional<SelectionUnit> unit = index_->unit(unit_index);
  if (!unit || offset < unit->file_span.offset || offset >= unit->file_span.offset + unit->file_span.length) {
    return false;
  }
  return std::binary_search(unit->epb_positions.begin(), unit->epb_positions.end(),
                            static_cast<std::uint32_t>(offset - unit->file_span.offset));
}

std::uint8_t HexModel::highlight_mask(std::uint64_t offset) const {
  const Highlight& h = highlight_;
  if (!h.active || offset < h.first || offset > h.last) {
    return 0;
  }
  if (h.is_field && is_epb(offset)) {
    return 0;  // the EPBs inside a field are skipped visually
  }
  std::uint8_t mask = 0xFF;
  if (offset == h.first) {
    mask &= h.first_mask;
  }
  if (offset == h.last) {
    mask &= h.last_mask;
  }
  return mask;
}

const HexModel::Page* HexModel::page_for(std::uint64_t offset) const {
  const std::uint64_t page = offset / kPageBytes;
  const auto it = pages_.constFind(page);
  if (it != pages_.constEnd()) {
    return &*it;
  }
  if (source_ && !pending_.contains(page)) {
    pending_.insert(page);
    if (!load_queued_) {
      load_queued_ = true;
      // By name: the functor overload trips clang-analyzer's NewDeleteLeaks inside Qt's header.
      QMetaObject::invokeMethod(const_cast<HexModel*>(this), "loadPendingPages", Qt::QueuedConnection);
    }
  }
  return nullptr;
}

void HexModel::loadPendingPages() {
  load_queued_ = false;
  const QSet<std::uint64_t> todo = pending_;
  pending_.clear();
  for (const std::uint64_t page : todo) {
    Page loaded;
    const std::uint64_t base = page * kPageBytes;
    const std::uint64_t want = std::min<std::uint64_t>(kPageBytes, file_size() > base ? file_size() - base : 0);
    loaded.bytes.resize(static_cast<qsizetype>(want));
    const std::size_t got = source_ ? source_->read(base, reinterpret_cast<std::uint8_t*>(loaded.bytes.data()),
                                                    static_cast<std::size_t>(want))
                                    : 0;
    loaded.bytes.resize(static_cast<qsizetype>(got));
    pages_.insert(page, loaded);
    page_order_.append(page);
    while (page_order_.size() > static_cast<qsizetype>(kMaxCachedPages)) {
      pages_.remove(page_order_.takeFirst());
    }
    const std::uint64_t first_row = view_begin_ / kBytesPerRow;
    const std::uint64_t row0 = base / kBytesPerRow;
    if (row0 + kPageRows > first_row && rowCount() > 0) {
      const int top = row0 > first_row ? static_cast<int>(row0 - first_row) : 0;
      const int bottom = std::min(rowCount() - 1, static_cast<int>(row0 + kPageRows - 1 - first_row));
      if (top <= bottom) {
        Q_EMIT dataChanged(index(top, 0), index(bottom, ColumnCount - 1));
      }
    }
  }
}

std::optional<std::uint8_t> HexModel::byte_at(std::uint64_t offset) const {
  if (offset < view_begin_ || offset >= view_end_) {
    return std::nullopt;
  }
  const Page* page = page_for(offset);
  const std::uint64_t in_page = offset % kPageBytes;
  if (page == nullptr || in_page >= static_cast<std::uint64_t>(page->bytes.size())) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(page->bytes.at(static_cast<qsizetype>(in_page)));
}

QString HexModel::ascii_of_row(std::uint64_t row_offset) const {
  QString text;
  for (std::uint64_t i = 0; i < kBytesPerRow; ++i) {
    const std::optional<std::uint8_t> b = byte_at(row_offset + i);
    text += (b && *b >= 0x20 && *b < 0x7F) ? QChar(*b) : (b ? QLatin1Char('.') : QLatin1Char(' '));
  }
  return text;
}

QVariant HexModel::byte_cell(std::uint64_t off, int role) const {
  const bool inside = off >= view_begin_ && off < view_end_;
  const std::optional<std::uint8_t> value = byte_at(off);
  switch (role) {
    case Qt::DisplayRole:
      return value ? QVariant(QStringLiteral("%1").arg(*value, 2, 16, QLatin1Char('0')).toUpper())
                   : QVariant(QString());
    case ByteRole:
      return value ? static_cast<int>(*value) : -1;
    case LoadedRole:
      return !inside || value.has_value();
    case BandRole:
      return inside ? bandTintAt(static_cast<qlonglong>(off)) : static_cast<int>(TintNone);
    case EpbRole:
      return inside && is_epb(off);
    case HighlightRole:
      return inside ? static_cast<int>(highlight_mask(off)) : 0;
    case SelectedRole: {
      if (!inside || selection_ == nullptr || selection_->byte_length() <= 0) {
        return false;
      }
      const auto from = static_cast<std::uint64_t>(selection_->byte_offset());
      return off >= from && off < from + static_cast<std::uint64_t>(selection_->byte_length());
    }
    default:
      return {};
  }
}

QVariant HexModel::data(const QModelIndex& idx, int role) const {
  if (!idx.isValid() || idx.row() >= rowCount() || idx.column() >= ColumnCount) {
    return {};
  }
  const auto row_offset = static_cast<std::uint64_t>(offsetOfRow(idx.row()));
  if (idx.column() == ColOffset) {
    return role == Qt::DisplayRole ? QVariant(offsetText(static_cast<qlonglong>(row_offset))) : QVariant();
  }
  if (idx.column() == ColAscii) {
    if (role == Qt::DisplayRole) {
      return ascii_of_row(row_offset);
    }
    return role == LoadedRole ? QVariant(page_for(row_offset) != nullptr) : QVariant();
  }
  return byte_cell(row_offset + static_cast<std::uint64_t>(idx.column() - ColByte0), role);
}

QVariant HexModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
    return {};
  }
  if (section == ColOffset) {
    return QStringLiteral("Offset");
  }
  if (section == ColAscii) {
    return QStringLiteral("ASCII");
  }
  return QStringLiteral("%1").arg(section - ColByte0, 2, 16, QLatin1Char('0')).toUpper();
}

QHash<int, QByteArray> HexModel::roleNames() const {
  auto roles = QAbstractTableModel::roleNames();
  roles.insert(ByteRole, "byteValue");
  roles.insert(BandRole, "band");
  roles.insert(EpbRole, "epb");
  roles.insert(HighlightRole, "highlight");
  roles.insert(LoadedRole, "loaded");
  roles.insert(SelectedRole, "selected");
  return roles;
}

// ---- HexModel: selection, highlight, ribbon ---------------------------------------------------------------------

void HexModel::on_selection_changed(int mask) {
  const auto bits = static_cast<unsigned>(mask);
  if ((bits & (SelectionModel::ChangeBytes | SelectionModel::ChangeField | SelectionModel::ChangeUnit)) == 0) {
    return;
  }
  const bool unit_scope_change = scope_ == ScopeUnit && (bits & SelectionModel::ChangeUnit) != 0;
  recompute_highlight();
  if (unit_scope_change) {
    rebuild_view();
  }
  if (highlight_.active && selection_ != nullptr && !selection_->isEcho(QStringLiteral("hex"))) {
    reveal(highlight_.first);
  }
}

void HexModel::recompute_highlight() {
  Highlight next;
  if (selection_ != nullptr) {
    const BitSpan bits = selection_->bit_range();
    const int unit_index = selection_->unit_index();
    std::optional<SelectionUnit> unit;
    if (bits.bit_length > 0 && unit_index >= 0 && index_) {
      unit = index_->unit(unit_index);
    }
    if (unit) {
      const std::uint64_t last_bit = bits.bit_offset + bits.bit_length - 1;
      next.active = true;
      next.is_field = true;
      next.first = SelectionModel::raw_offset_of_bit(*unit, bits.bit_offset);
      next.last = SelectionModel::raw_offset_of_bit(*unit, last_bit);
      next.first_mask = static_cast<std::uint8_t>(0xFFU >> (bits.bit_offset % 8U));
      next.last_mask = static_cast<std::uint8_t>((0xFFU << (7U - last_bit % 8U)) & 0xFFU);
      next.name = selection_->field_path();
    } else if (selection_->byte_length() > 0) {
      next.active = true;
      next.first = static_cast<std::uint64_t>(selection_->byte_offset());
      next.last = next.first + static_cast<std::uint64_t>(selection_->byte_length()) - 1;
    }
  }
  highlight_ = std::move(next);
  if (rowCount() > 0) {
    Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {HighlightRole, SelectedRole});
  }
  Q_EMIT highlightChanged();
}

QVariantList HexModel::ribbon() const {
  QVariantList out;
  if (!highlight_.active || !source_) {
    return out;
  }
  const std::uint64_t start = highlight_.first > 0 ? highlight_.first - 1 : 0;
  std::array<std::uint8_t, kRibbonBytes> buffer{};
  const std::size_t got = source_->read(start, buffer.data(), buffer.size());
  for (std::size_t i = 0; i < got; ++i) {
    const std::uint64_t off = start + i;
    QVariantMap cell;
    cell.insert(QStringLiteral("offset"), static_cast<qulonglong>(off));
    cell.insert(QStringLiteral("value"), static_cast<int>(buffer.at(i)));
    cell.insert(QStringLiteral("bits"), QStringLiteral("%1").arg(buffer.at(i), 8, 2, QLatin1Char('0')));
    cell.insert(QStringLiteral("mask"), static_cast<int>(highlight_mask(off)));
    cell.insert(QStringLiteral("epb"), is_epb(off));
    out.append(cell);
  }
  return out;
}

void HexModel::reveal(std::uint64_t offset) {
  const int row = rowOfOffset(static_cast<qlonglong>(offset));
  if (row >= 0) {
    Q_EMIT revealRequested(row);
  }
}

// ---- HexModel: Go to, click, reverse lookup ---------------------------------------------------------------------

HexGoto HexModel::parseGoto(const QString& text) const {
  HexGoto out;
  const QString t = text.trimmed().toLower();
  static const QRegularExpression hex(QStringLiteral("^0x([0-9a-f]{1,15})$"));
  static const QRegularExpression decimal(QStringLiteral("^[0-9]{1,18}$"));
  static const QRegularExpression unit(QStringLiteral("^unit:([0-9]{1,9})$"));
  static const QRegularExpression frame(QStringLiteral("^frame:([0-9]{1,9})$"));
  if (const auto mh = hex.match(t); mh.hasMatch()) {
    out.offset = mh.captured(1).toULongLong(nullptr, 16);
  } else if (decimal.match(t).hasMatch()) {
    out.offset = t.toULongLong();
  } else if (const auto mu = unit.match(t); mu.hasMatch()) {
    out.unit = mu.captured(1).toInt();
  } else if (const auto mf = frame.match(t); mf.hasMatch()) {
    out.unit = index_ ? index_->first_vcl_unit(FrameId{mf.captured(1).toInt(), -1}) : -1;
    if (out.unit < 0) {
      out.error = QStringLiteral("no frame %1").arg(mf.captured(1));
      return out;
    }
  } else if (t.startsWith(QLatin1Char('@'))) {
    QString error;
    const auto target = ovt::app::parseGoto(t, &error);
    if (!target) {
      out.error = error;
      return out;
    }
    out.offset = static_cast<std::uint64_t>(target->integer);
  } else {
    out.error = QStringLiteral("go to 0x1A3F40, a decimal offset, unit:N or frame:N");
    return out;
  }
  if (out.offset) {
    if (*out.offset >= file_size()) {
      out.error = QStringLiteral("offset beyond the end of the file (%1 bytes)").arg(file_size());
      out.offset.reset();
    }
    return out;
  }
  for (const HexBand& band : unit_bands_) {
    if (band.index == out.unit) {
      out.offset = band.offset;
      return out;
    }
  }
  out.error = QStringLiteral("no unit %1").arg(out.unit);
  return out;
}

QString HexModel::goTo(const QString& text, const QString& origin) {
  const HexGoto target = parseGoto(text);
  if (!target.offset) {
    return target.error;
  }
  if (selection_ != nullptr) {
    if (target.unit >= 0) {
      selection_->selectUnit(target.unit, origin);
    } else {
      selection_->selectBytes(static_cast<qlonglong>(*target.offset), 1, origin);
    }
  }
  reveal(*target.offset);
  return {};
}

QVariantMap HexModel::lookup(qlonglong offset) const {
  QVariantMap out;
  out.insert(QStringLiteral("unit"), -1);
  out.insert(QStringLiteral("box"), -1);
  out.insert(QStringLiteral("fieldPath"), QString());
  out.insert(QStringLiteral("bitOffset"), 0);
  out.insert(QStringLiteral("bitLength"), 0);
  const auto off = static_cast<std::uint64_t>(offset);
  if (const auto it = std::upper_bound(box_bands_.begin(), box_bands_.end(), off,
                                       [](std::uint64_t v, const HexBand& b) { return v < b.offset; });
      it != box_bands_.begin() && off < std::prev(it)->end()) {
    out.insert(QStringLiteral("box"), std::prev(it)->index);
  }
  if (!index_ || offset < 0) {
    return out;
  }
  const int unit_index = index_->unit_at_byte(off);
  out.insert(QStringLiteral("unit"), unit_index);
  const std::optional<SelectionUnit> unit = unit_index >= 0 ? index_->unit(unit_index) : std::nullopt;
  if (!unit || !field_provider_ || off < unit->file_span.offset ||
      off >= unit->file_span.offset + unit->file_span.length) {
    return out;
  }
  // File byte → RBSP byte (the inverse of the §11.3.5 mapping); an EPB belongs to no field.
  const auto rel = static_cast<std::uint32_t>(off - unit->file_span.offset);
  if (std::binary_search(unit->epb_positions.begin(), unit->epb_positions.end(), rel)) {
    return out;
  }
  const auto removed = static_cast<std::uint64_t>(
      std::lower_bound(unit->epb_positions.begin(), unit->epb_positions.end(), rel) - unit->epb_positions.begin());
  const std::uint64_t bit = (rel - removed) * 8U;
  auto cached = field_cache_.find(unit_index);
  if (cached == field_cache_.end()) {
    std::vector<HexField> fields = field_provider_(unit_index);
    std::stable_sort(fields.begin(), fields.end(),
                     [](const HexField& a, const HexField& b) { return a.bits.bit_offset < b.bits.bit_offset; });
    cached = field_cache_.insert(unit_index, fields);
  }
  const std::vector<HexField>& fields = *cached;
  // Deepest = shortest span among those covering the bit. Spans are sorted by start; a nested span starts after its
  // parent, so walk back from the last span starting at or before the bit.
  const HexField* best = nullptr;
  auto first_after = std::upper_bound(fields.begin(), fields.end(), bit,
                                      [](std::uint64_t v, const HexField& f) { return v < f.bits.bit_offset; });
  std::size_t scanned = 0;
  for (auto it = first_after; it != fields.begin() && scanned < 4096; ++scanned) {
    --it;
    if (it->bits.bit_offset + it->bits.bit_length > bit && it->bits.bit_length > 0 &&
        (best == nullptr || it->bits.bit_length < best->bits.bit_length)) {
      best = &*it;
    }
  }
  if (best != nullptr) {
    out.insert(QStringLiteral("fieldPath"), best->path);
    out.insert(QStringLiteral("bitOffset"), static_cast<qulonglong>(best->bits.bit_offset));
    out.insert(QStringLiteral("bitLength"), static_cast<int>(best->bits.bit_length));
  }
  return out;
}

void HexModel::selectByteRange(qlonglong first, qlonglong last, const QString& origin) {
  if (selection_ == nullptr || first < 0) {
    return;
  }
  if (last < first) {
    std::swap(first, last);
  }
  if (first == last) {
    // SPEC-ASSUMPTION(#704): a click inside a field selects the field (its bytes include the clicked one).
    const QVariantMap hit = lookup(first);
    const QString path = hit.value(QStringLiteral("fieldPath")).toString();
    if (!path.isEmpty()) {
      selection_->selectField(hit.value(QStringLiteral("unit")).toInt(), path,
                              hit.value(QStringLiteral("bitOffset")).toLongLong(),
                              hit.value(QStringLiteral("bitLength")).toInt(), origin);
      return;
    }
  }
  selection_->selectBytes(first, last - first + 1, origin);
}

QString HexModel::copyHex(qlonglong offset, qlonglong length) const {
  QStringList out;
  if (!source_ || offset < 0 || length <= 0) {
    return {};
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<qlonglong>(length, 1 << 20)));
  bytes.resize(source_->read(static_cast<std::uint64_t>(offset), bytes.data(), bytes.size()));
  for (const std::uint8_t b : bytes) {
    out.append(QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper());
  }
  return out.join(QLatin1Char(' '));
}

QString HexModel::copyCArray(qlonglong offset, qlonglong length) const {
  QStringList parts = copyHex(offset, length).split(QLatin1Char(' '), Qt::SkipEmptyParts);
  for (QString& p : parts) {
    p.prepend(QStringLiteral("0x"));
  }
  return QStringLiteral("{ %1 }").arg(parts.join(QStringLiteral(", ")));
}

}  // namespace ovt::app
