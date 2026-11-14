// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/stream_index_model.h"

#include <algorithm>
#include <utility>

namespace ovt::app {

StreamIndexModel::StreamIndexModel(QObject* parent) : QObject(parent) {}

StreamIndexModel::~StreamIndexModel() = default;

void StreamIndexModel::setIndex(std::shared_ptr<const StreamIndexHandle> handle) {
  handle_ = std::move(handle);
  table_ = handle_ ? FrameTable(handle_->index()) : FrameTable();
  const bool had_range = range_.has_value();
  range_.reset();
  Q_EMIT indexChanged();
  if (had_range) {
    Q_EMIT rangeChanged(-1, -1);
  }
}

std::shared_ptr<const SelectionIndex> StreamIndexModel::selectionIndex() const {
  if (!handle_) {
    return nullptr;
  }
  return std::make_shared<StreamIndexAdapter>(handle_);
}

void StreamIndexModel::set_display_order(bool display) {
  const FrameOrder wanted = display ? FrameOrder::Display : FrameOrder::Decode;
  if (wanted == table_.order()) {
    return;
  }
  // The range follows its frames (the selection stays on the same frames): the positions of its first and last frame
  // in the new order, normalised so first <= last. A range in decode order thus becomes the span between the same two
  // frames in display order, which is what the user dragged between.
  const int first_frame = range_ ? table_.decode_index_at(range_->first) : -1;
  const int last_frame = range_ ? table_.decode_index_at(range_->second) : -1;
  table_.set_order(wanted);
  Q_EMIT orderChanged();
  if (!range_) {
    return;
  }
  const auto a = table_.position_of(first_frame);
  const auto b = table_.position_of(last_frame);
  if (!a || !b) {
    clearRange();
    return;
  }
  range_ = std::make_pair(std::min(*a, *b), std::max(*a, *b));
  Q_EMIT rangeChanged(static_cast<int>(range_->first), static_cast<int>(range_->second));
}

std::vector<SeriesColumn> StreamIndexModel::columns(const SeriesRef& series, std::size_t first, std::size_t last,
                                                    std::size_t width) {
  return table_.aggregator(series).columns(first, last, width);
}

int StreamIndexModel::decodeIndexAt(int position) const {
  return position < 0 ? -1 : table_.decode_index_at(static_cast<std::size_t>(position));
}

int StreamIndexModel::displayIndexOf(int decode_index) const {
  return table_.display_index_of(decode_index);
}

int StreamIndexModel::positionOf(int decode_index) const {
  const auto position = table_.position_of(decode_index);
  return position ? static_cast<int>(*position) : -1;
}

void StreamIndexModel::setRange(int first, int last) {
  if (table_.size() == 0) {
    return;
  }
  const auto clamp = [this](int v) {
    return std::min<std::size_t>(static_cast<std::size_t>(std::max(v, 0)), table_.size() - 1);
  };
  std::size_t a = clamp(first);
  std::size_t b = clamp(last);
  if (a > b) {
    std::swap(a, b);
  }
  if (range_ && range_->first == a && range_->second == b) {
    return;
  }
  range_ = std::make_pair(a, b);
  Q_EMIT rangeChanged(static_cast<int>(a), static_cast<int>(b));
}

void StreamIndexModel::clearRange() {
  if (range_) {
    range_.reset();
    Q_EMIT rangeChanged(-1, -1);
  }
}

std::optional<RangeStats> StreamIndexModel::range_stats() const {
  return range_ ? table_.range_stats(range_->first, range_->second) : std::nullopt;
}

QVariantMap StreamIndexModel::range_stats_map() const {
  QVariantMap out;
  if (const auto stats = range_stats()) {
    out.insert(QStringLiteral("frames"), static_cast<qulonglong>(stats->frames));
    out.insert(QStringLiteral("bytes"), static_cast<qulonglong>(stats->bytes));
    out.insert(QStringLiteral("seconds"), stats->seconds);
    out.insert(QStringLiteral("bitsPerSecond"), stats->bits_per_second);
    out.insert(QStringLiteral("meanQp"), stats->mean_qp);
    out.insert(QStringLiteral("firstDecodeIndex"), stats->first_decode_index);
    out.insert(QStringLiteral("lastDecodeIndex"), stats->last_decode_index);
  }
  return out;
}

}  // namespace ovt::app
