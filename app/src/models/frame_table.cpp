// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/frame_table.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ovt::app {

FrameTable::FrameTable(const fb::StreamIndex& index) {
  const auto* frames = index.frames();
  if (frames == nullptr) {
    return;
  }
  const std::size_t n = frames->size();
  size_bytes_.reserve(n);
  qp_.reserve(n);
  tid_.reserve(n);
  pts_.reserve(n);
  poc_.reserve(n);
  type_.reserve(n);
  rap_.reserve(n);
  shown_.reserve(n);
  for (const fb::FrameInfo* f : *frames) {
    size_bytes_.push_back(f->size_bytes());
    qp_.push_back(f->qp_base());
    tid_.push_back(f->temporal_id());
    pts_.push_back(f->pts());
    poc_.push_back(f->poc());
    type_.push_back(f->type());
    rap_.push_back(f->is_rap() ? 1 : 0);
    shown_.push_back(f->shown() ? 1 : 0);
    ref_begin_.push_back(refs_.size());
    if (const auto* refs = f->declared_refs(); refs != nullptr) {
      refs_.insert(refs_.end(), refs->begin(), refs->end());
    }
  }
  ref_begin_.push_back(refs_.size());
  if (n != 0) {
    timebase_num_ = (*frames)[0]->timebase_num();
    timebase_den_ = (*frames)[0]->timebase_den();
  }

  sorted_pts_ = pts_;
  std::sort(sorted_pts_.begin(), sorted_pts_.end());
  sorted_pts_.erase(std::unique(sorted_pts_.begin(), sorted_pts_.end()), sorted_pts_.end());

  // display_order lists decode indices per display slot; keep it only when it is a valid selection of frames.
  if (const auto* display = index.display_order(); display != nullptr) {
    std::vector<std::uint8_t> seen(n, 0);
    for (const int decode : *display) {
      if (decode >= 0 && static_cast<std::size_t>(decode) < n && seen[static_cast<std::size_t>(decode)] == 0) {
        seen[static_cast<std::size_t>(decode)] = 1;
        display_order_.push_back(decode);
      }
    }
  }
  if (display_order_.empty()) {
    // No display order stored: presentation order is pts order (ties keep decode order).
    display_order_.resize(n);
    std::iota(display_order_.begin(), display_order_.end(), 0);
    std::stable_sort(display_order_.begin(), display_order_.end(), [this](int a, int b) {
      return pts_[static_cast<std::size_t>(a)] < pts_[static_cast<std::size_t>(b)];
    });
  }
  display_of_.assign(n, -1);
  for (std::size_t slot = 0; slot < display_order_.size(); ++slot) {
    display_of_[static_cast<std::size_t>(display_order_[slot])] = static_cast<int>(slot);
  }
  rebuild_order();
}

std::span<const int> FrameTable::declared_refs(int decode_index) const {
  if (decode_index < 0 || static_cast<std::size_t>(decode_index) + 1 >= ref_begin_.size()) {
    return {};
  }
  const auto d = static_cast<std::size_t>(decode_index);
  return std::span<const int>(refs_).subspan(ref_begin_[d], ref_begin_[d + 1] - ref_begin_[d]);
}

int FrameTable::display_index_of(int decode_index) const {
  if (decode_index < 0 || static_cast<std::size_t>(decode_index) >= display_of_.size()) {
    return -1;
  }
  return display_of_[static_cast<std::size_t>(decode_index)];
}

void FrameTable::rebuild_order() {
  const std::size_t n = size();
  if (order_mode_ == FrameOrder::Display) {
    order_ = display_order_;
  } else {
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0);
  }
  position_of_.assign(n, -1);
  for (std::size_t p = 0; p < order_.size(); ++p) {
    position_of_[static_cast<std::size_t>(order_[p])] = static_cast<int>(p);
  }
  aggregators_.clear();
}

std::optional<std::size_t> FrameTable::set_order(FrameOrder order, std::optional<std::size_t> keep_position) {
  std::optional<int> kept;
  if (keep_position && *keep_position < order_.size()) {
    kept = order_[*keep_position];
  }
  if (order != order_mode_) {
    order_mode_ = order;
    rebuild_order();
  }
  return kept ? position_of(*kept) : std::nullopt;
}

int FrameTable::decode_index_at(std::size_t position) const {
  return position < order_.size() ? order_[position] : -1;
}

std::optional<std::size_t> FrameTable::position_of(int decode_index) const {
  if (decode_index < 0 || static_cast<std::size_t>(decode_index) >= position_of_.size() ||
      position_of_[static_cast<std::size_t>(decode_index)] < 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(position_of_[static_cast<std::size_t>(decode_index)]);
}

std::vector<std::size_t> FrameTable::rap_positions() const {
  std::vector<std::size_t> out;
  for (std::size_t p = 0; p < order_.size(); ++p) {
    if (rap_[static_cast<std::size_t>(order_[p])] != 0) {
      out.push_back(p);
    }
  }
  return out;
}

void FrameTable::set_metric(const std::string& metric_id, int plane, std::vector<double> by_decode_index) {
  const SeriesRef ref{Series::Metric, metric_id, plane};
  by_decode_index.resize(size(), std::numeric_limits<double>::quiet_NaN());
  metrics_[ref] = std::move(by_decode_index);
  aggregators_.erase(ref);
}

const SeriesAggregator& FrameTable::aggregator(const SeriesRef& series) {
  SeriesRef key = series;
  if (key.kind != Series::Metric) {
    key.metric_id.clear();
    key.plane = 0;
  }
  if (const auto it = aggregators_.find(key); it != aggregators_.end()) {
    return it->second;
  }
  std::vector<double> values(size(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t p = 0; p < order_.size(); ++p) {
    const auto d = static_cast<std::size_t>(order_[p]);
    switch (key.kind) {
      case Series::Bits:
        values[p] = static_cast<double>(size_bytes_[d]) * 8.0;
        break;
      case Series::Qp:
        values[p] = static_cast<double>(qp_[d]);
        break;
      case Series::TemporalId:
        values[p] = static_cast<double>(tid_[d]);
        break;
      case Series::Metric:
        if (const auto m = metrics_.find(key); m != metrics_.end()) {
          values[p] = m->second[d];
        }
        break;
    }
  }
  return aggregators_.emplace(key, SeriesAggregator(std::move(values))).first->second;
}

std::optional<RangeStats> FrameTable::range_stats(std::size_t first, std::size_t last) const {
  if (order_.empty()) {
    return std::nullopt;
  }
  if (first > last) {
    std::swap(first, last);
  }
  last = std::min(last, order_.size() - 1);
  first = std::min(first, last);

  RangeStats r;
  r.frames = last - first + 1;
  r.first_decode_index = order_[first];
  r.last_decode_index = order_[last];
  std::int64_t min_pts = std::numeric_limits<std::int64_t>::max();
  std::int64_t max_pts = std::numeric_limits<std::int64_t>::min();
  double qp_sum = 0.0;
  for (std::size_t p = first; p <= last; ++p) {
    const auto d = static_cast<std::size_t>(order_[p]);
    r.bytes += size_bytes_[d];
    qp_sum += qp_[d];
    min_pts = std::min(min_pts, pts_[d]);
    max_pts = std::max(max_pts, pts_[d]);
  }
  r.mean_qp = qp_sum / static_cast<double>(r.frames);

  // SPEC-ASSUMPTION(#240): Δpts spans from the earliest pts of the range to the end of its latest frame: the pts
  // distance from that frame to the next frame in presentation order (the previous distance for the stream's last).
  const auto upper = std::upper_bound(sorted_pts_.begin(), sorted_pts_.end(), max_pts);
  std::int64_t last_duration = 0;
  if (upper != sorted_pts_.end()) {
    last_duration = *upper - max_pts;
  } else if (sorted_pts_.size() > 1) {
    last_duration = max_pts - sorted_pts_[sorted_pts_.size() - 2];
  }
  r.ticks = (max_pts - min_pts) + last_duration;
  if (timebase_den_ > 0 && timebase_num_ > 0) {
    r.seconds = static_cast<double>(r.ticks) * static_cast<double>(timebase_num_) / static_cast<double>(timebase_den_);
  }
  if (r.seconds > 0.0) {
    r.bits_per_second = static_cast<double>(r.bytes) * 8.0 / r.seconds;
  }
  return r;
}

}  // namespace ovt::app
