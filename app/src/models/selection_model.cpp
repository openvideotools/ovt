// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SelectionModel (chapter 11 §11.4.1; WI-1.452). See selection_model.h.

#include "selection_model.h"

#include <algorithm>
#include <utility>

namespace ovt::app {

namespace {

[[nodiscard]] bool is_vcl(FrameId frame) {
  return frame.decode_index >= 0;
}

[[nodiscard]] bool is_none(FrameId frame) {
  return frame.decode_index < 0 && frame.display_index < 0;
}

}  // namespace

SelectionModel::SelectionModel(QObject* parent) : QObject(parent) {}

SelectionModel::~SelectionModel() = default;

void SelectionModel::setIndex(std::shared_ptr<const SelectionIndex> index) {
  index_ = std::move(index);
}

void SelectionModel::setChainLength(ChainLength chain_length) {
  chain_length_ = std::move(chain_length);
}

void SelectionModel::set_lock_pixel_selection(bool lock) {
  if (lock_pixel_ != lock) {
    lock_pixel_ = lock;
    Q_EMIT lockChanged();
  }
}

bool SelectionModel::isEcho(const QString& panel_id) const {
  return !panel_id.isEmpty() && s_.origin == panel_id;
}

// ---- the §11.3.5 mapping ------------------------------------------------------------------------------------------

std::uint64_t SelectionModel::raw_offset_of_bit(const SelectionUnit& unit, std::uint64_t bit_offset) {
  std::uint64_t raw = bit_offset / 8;
  for (const std::uint32_t position : unit.epb_positions) {  // ascending
    if (position <= raw) {
      ++raw;
    } else {
      break;
    }
  }
  return unit.file_span.offset + raw;
}

ByteSpan SelectionModel::field_bytes(const SelectionUnit& unit, BitSpan bits) {
  const std::uint64_t first = raw_offset_of_bit(unit, bits.bit_offset);
  if (bits.bit_length == 0) {
    return {first, 0};
  }
  const std::uint64_t last = raw_offset_of_bit(unit, bits.bit_offset + bits.bit_length - 1);
  return {first, last - first + 1};
}

// ---- derivations ---------------------------------------------------------------------------------------------------

void SelectionModel::set_frame_keep_rules(State& s, FrameId frame) const {
  if (frame != s.frame) {
    // A different frame drops everything that belongs to the old picture; the probe stays when the pixel selection
    // is locked (§11.4.1).
    s.block_id = kNoBlock;
    s.level = -1;
    s.mv_index = -1;
    if (!lock_pixel_) {
      s.probe = QPoint(-1, -1);
    }
  }
  s.frame = frame;
}

void SelectionModel::derive_from_unit(State& s, int unit_index, bool with_bytes) const {
  s.field_path.clear();
  s.bit_range = {};
  s.unit_index = unit_index;
  if (!index_ || unit_index < 0) {
    return;
  }
  const std::optional<SelectionUnit> unit = index_->unit(unit_index);
  if (!unit) {
    return;
  }
  if (is_vcl(unit->frame)) {
    set_frame_keep_rules(s, index_->resolve_frame(unit->frame).value_or(unit->frame));
  }
  s.sample_index = unit->sample_index;
  if (with_bytes) {
    s.byte_range = unit->file_span;
  }
}

unsigned SelectionModel::diff(const State& a, const State& b) {
  unsigned mask = ChangeNone;
  const auto flag = [&mask](bool differs, Change bit) {
    if (differs) {
      mask |= bit;
    }
  };
  flag(a.frame != b.frame, ChangeFrame);
  flag(a.block_id != b.block_id || a.level != b.level, ChangeBlock);
  flag(a.mv_index != b.mv_index, ChangeMv);
  flag(a.unit_index != b.unit_index, ChangeUnit);
  flag(a.sample_index != b.sample_index, ChangeSample);
  flag(a.byte_range != b.byte_range, ChangeBytes);
  flag(a.field_path != b.field_path || a.bit_range != b.bit_range, ChangeField);
  flag(a.probe != b.probe, ChangeProbe);
  flag(a.selection_rect != b.selection_rect, ChangeRect);
  flag(a.display_order != b.display_order, ChangeOrder);
  return mask;
}

void SelectionModel::commit(State next, const QString& origin) {
  unsigned mask = diff(s_, next);
  const bool source_changed = s_.file_id != next.file_id || s_.track_index != next.track_index;
  if (mask == ChangeNone && !source_changed) {
    return;
  }
  next.origin = origin;
  s_ = std::move(next);
  // A source change with no other difference still tells the panels to reload; Frame is the closest bit.
  if (mask == ChangeNone) {
    mask = ChangeFrame;
  }
  Q_EMIT changed(static_cast<int>(mask));
}

// ---- setters ------------------------------------------------------------------------------------------------------

void SelectionModel::setSource(const QString& file_id, int track_index, const QString& origin) {
  State next;
  next.file_id = file_id;
  next.track_index = track_index;
  next.display_order = s_.display_order;
  commit(std::move(next), origin);
}

void SelectionModel::setFrame(FrameId frame, const QString& origin) {
  State next = s_;
  if (!is_none(frame) && index_) {
    const std::optional<FrameId> resolved = index_->resolve_frame(frame);
    if (!resolved) {
      return;  // not a frame of the open stream
    }
    frame = *resolved;
  }
  set_frame_keep_rules(next, frame);
  const int unit = (index_ && !is_none(frame)) ? index_->first_vcl_unit(frame) : -1;
  next.sample_index = -1;
  next.byte_range = {};
  derive_from_unit(next, unit, true);
  commit(std::move(next), origin);
}

void SelectionModel::setBlock(std::uint32_t block_id, int level, const QString& origin) {
  if (block_id == kNoBlock) {
    clearBlock(origin);
    return;
  }
  State next = s_;
  const int chain = std::max(1, chain_length_ ? chain_length_(block_id) : 1);
  if (level >= 0) {
    next.level = chain_length_ ? std::min(level, chain - 1) : level;
  } else if (block_id == s_.block_id && s_.level >= 0) {
    next.level = (s_.level + 1) % chain;
  } else {
    next.level = 0;
  }
  next.block_id = block_id;
  next.mv_index = -1;  // setting a block clears the motion vector
  commit(std::move(next), origin);
}

void SelectionModel::setMv(int mv_index, const QString& origin) {
  State next = s_;
  next.mv_index = mv_index;
  commit(std::move(next), origin);
}

void SelectionModel::setUnit(int unit_index, const QString& origin) {
  if (index_ && unit_index >= 0 && !index_->unit(unit_index)) {
    return;
  }
  State next = s_;
  next.sample_index = -1;
  next.byte_range = {};
  derive_from_unit(next, unit_index, true);
  commit(std::move(next), origin);
}

void SelectionModel::setSample(int sample_index, const QString& origin) {
  State next = s_;
  next.sample_index = sample_index;
  next.field_path.clear();
  next.bit_range = {};
  next.byte_range = {};
  next.unit_index = -1;
  if (index_ && sample_index >= 0) {
    const std::optional<SelectionSample> sample = index_->sample(sample_index);
    if (!sample) {
      return;
    }
    next.unit_index = sample->first_unit;
    if (is_vcl(sample->frame)) {
      set_frame_keep_rules(next, index_->resolve_frame(sample->frame).value_or(sample->frame));
    }
    next.byte_range = sample->span;
  }
  commit(std::move(next), origin);
}

void SelectionModel::setBytes(ByteSpan bytes, const QString& origin) {
  State next = s_;
  const int unit = (index_ && bytes.length > 0) ? index_->unit_at_byte(bytes.offset) : -1;
  derive_from_unit(next, unit, false);
  next.byte_range = bytes;
  commit(std::move(next), origin);
}

void SelectionModel::setField(int unit_index, const QString& path, BitSpan bits, const QString& origin) {
  State next = s_;
  derive_from_unit(next, unit_index, false);
  if (index_ && unit_index >= 0) {
    const std::optional<SelectionUnit> unit = index_->unit(unit_index);
    if (!unit) {
      return;
    }
    next.byte_range = bits.bit_length > 0 ? field_bytes(*unit, bits) : ByteSpan{};
  } else {
    next.byte_range = {};
  }
  next.field_path = path;
  next.bit_range = bits;
  commit(std::move(next), origin);
}

void SelectionModel::setProbe(QPoint probe, const QString& origin) {
  State next = s_;
  next.probe = probe;
  commit(std::move(next), origin);
}

void SelectionModel::setRect(QRect rect, const QString& origin) {
  State next = s_;
  next.selection_rect = rect;
  commit(std::move(next), origin);
}

void SelectionModel::setDisplayOrder(bool display_order, const QString& origin) {
  State next = s_;
  next.display_order = display_order;
  commit(std::move(next), origin);
}

void SelectionModel::clearBlock(const QString& origin) {
  State next = s_;
  next.block_id = kNoBlock;
  next.level = -1;
  next.mv_index = -1;
  commit(std::move(next), origin);
}

void SelectionModel::clear(const QString& origin) {
  State next;
  next.file_id = s_.file_id;
  next.track_index = s_.track_index;
  next.display_order = s_.display_order;
  commit(std::move(next), origin);
}

// ---- QML entry points ---------------------------------------------------------------------------------------------

void SelectionModel::selectFrame(int decode_index, int display_index, const QString& origin) {
  setFrame(FrameId{decode_index, display_index}, origin);
}

void SelectionModel::selectBlock(quint32 block_id, int level, const QString& origin) {
  setBlock(block_id, level, origin);
}

void SelectionModel::selectMv(int mv_index, const QString& origin) {
  setMv(mv_index, origin);
}

void SelectionModel::selectUnit(int unit_index, const QString& origin) {
  setUnit(unit_index, origin);
}

void SelectionModel::selectSample(int sample_index, const QString& origin) {
  setSample(sample_index, origin);
}

void SelectionModel::selectBytes(qlonglong offset, qlonglong length, const QString& origin) {
  setBytes(ByteSpan{static_cast<std::uint64_t>(std::max<qlonglong>(offset, 0)),
                    static_cast<std::uint64_t>(std::max<qlonglong>(length, 0))},
           origin);
}

void SelectionModel::selectField(int unit_index, const QString& path, qlonglong bit_offset, int bit_length,
                                 const QString& origin) {
  setField(unit_index, path,
           BitSpan{static_cast<std::uint64_t>(std::max<qlonglong>(bit_offset, 0)),
                   static_cast<std::uint32_t>(std::max(bit_length, 0))},
           origin);
}

void SelectionModel::selectProbe(int x, int y, const QString& origin) {
  setProbe(QPoint(x, y), origin);
}

void SelectionModel::selectRect(const QRect& rect, const QString& origin) {
  setRect(rect, origin);
}

void SelectionModel::selectDisplayOrder(bool display_order, const QString& origin) {
  setDisplayOrder(display_order, origin);
}

void SelectionModel::clearBlockSelection(const QString& origin) {
  clearBlock(origin);
}

void SelectionModel::clearSelection(const QString& origin) {
  clear(origin);
}

}  // namespace ovt::app
