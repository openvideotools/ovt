// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// FrameTable (chapter 11 §11.3.1; WI-1.455a): the per-frame columns of a StreamIndex in the order the Timeline
// shows them (decode or display), the per-series level-of-detail tables, and the range statistics of the status line.
//
// The columns are copied out of the FlatBuffer once (structure of arrays), so the chart never touches the FlatBuffer
// while painting. The aggregator of a series is rebuilt only when the index grows or the order changes (§11.3.1
// pitfall: LOD tables are per series and rebuilt only when the index grows). No Qt types.

#ifndef OVT_APP_MODELS_FRAME_TABLE_H
#define OVT_APP_MODELS_FRAME_TABLE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "models/series_aggregator.h"
#include "ovt_model_generated.h"

namespace ovt::app {

// The series of the Timeline combo (§11.3.1 "Series combo").
enum class Series : std::uint8_t { Bits, Qp, TemporalId, Metric };

struct SeriesRef {
  Series kind = Series::Bits;
  std::string metric_id;  // kind == Metric only
  int plane = 0;          // kind == Metric only: the plane of a per-plane metric
  friend bool operator<(const SeriesRef& a, const SeriesRef& b) {
    return std::tie(a.kind, a.metric_id, a.plane) < std::tie(b.kind, b.metric_id, b.plane);
  }
  friend bool operator==(const SeriesRef&, const SeriesRef&) = default;
};

enum class FrameOrder : std::uint8_t { Decode, Display };

// What the status line shows for a range of frames.
struct RangeStats {
  std::size_t frames = 0;
  std::uint64_t bytes = 0;
  std::int64_t ticks = 0;        // Δpts in time base ticks
  double seconds = 0.0;          // ticks × timebase_num / timebase_den; 0 when the time base is unknown
  double bits_per_second = 0.0;  // bytes × 8 / seconds; 0 when seconds == 0
  double mean_qp = 0.0;          // mean of qp_base
  int first_decode_index = -1;   // the frames at the first and last position of the range
  int last_decode_index = -1;
};

class FrameTable {
 public:
  FrameTable() = default;
  // Copies the frame columns out of the index. The order starts as decode order.
  explicit FrameTable(const fb::StreamIndex& index);

  [[nodiscard]] std::size_t size() const noexcept { return size_bytes_.size(); }
  [[nodiscard]] FrameOrder order() const noexcept { return order_mode_; }
  // Switches the x axis between decode and display order. Returns the new position of the frame that sat at
  // `keep_position` before (the selection stays on the same frame), or nullopt when that was not a valid position.
  std::optional<std::size_t> set_order(FrameOrder order, std::optional<std::size_t> keep_position = std::nullopt);

  // Position <-> decode index in the current order.
  [[nodiscard]] int decode_index_at(std::size_t position) const;
  [[nodiscard]] std::optional<std::size_t> position_of(int decode_index) const;

  // Per-frame columns by DECODE index.
  [[nodiscard]] std::uint64_t size_bytes(int decode_index) const {
    return size_bytes_.at(static_cast<std::size_t>(decode_index));
  }
  [[nodiscard]] int qp_base(int decode_index) const { return qp_.at(static_cast<std::size_t>(decode_index)); }
  [[nodiscard]] int temporal_id(int decode_index) const { return tid_.at(static_cast<std::size_t>(decode_index)); }
  [[nodiscard]] std::int64_t pts(int decode_index) const { return pts_.at(static_cast<std::size_t>(decode_index)); }
  [[nodiscard]] fb::FrameType type(int decode_index) const { return type_.at(static_cast<std::size_t>(decode_index)); }
  [[nodiscard]] bool is_rap(int decode_index) const { return rap_.at(static_cast<std::size_t>(decode_index)) != 0; }
  [[nodiscard]] std::int64_t poc(int decode_index) const { return poc_.at(static_cast<std::size_t>(decode_index)); }
  // The display index of a frame (its slot in display order), or -1.
  [[nodiscard]] int display_index_of(int decode_index) const;
  [[nodiscard]] bool shown(int decode_index) const { return shown_.at(static_cast<std::size_t>(decode_index)) != 0; }

  // The positions (in the current order) of the random access points, ascending: the GOP markers.
  [[nodiscard]] std::vector<std::size_t> rap_positions() const;

  // Supplies a metric series (one value per frame by decode index, NaN = gap, e.g. from MetricsResult.frames).
  // Replaces an earlier series of the same id and plane.
  void set_metric(const std::string& metric_id, int plane, std::vector<double> by_decode_index);

  // The aggregator of a series for the current order; built on first use and cached until the order changes.
  // For a Metric series that was never supplied, a series of gaps. Bits are size_bytes × 8.
  [[nodiscard]] const SeriesAggregator& aggregator(const SeriesRef& series);

  // Statistics of the frames at positions [first, last] (inclusive, current order; swapped if reversed, clamped).
  // nullopt for an empty table.
  [[nodiscard]] std::optional<RangeStats> range_stats(std::size_t first, std::size_t last) const;

 private:
  void rebuild_order();

  std::vector<std::uint64_t> size_bytes_;
  std::vector<std::int16_t> qp_;
  std::vector<std::uint8_t> tid_;
  std::vector<std::int64_t> pts_;
  std::vector<std::int64_t> poc_;
  std::vector<int> display_of_;  // decode index -> display slot
  std::vector<fb::FrameType> type_;
  std::vector<std::uint8_t> rap_;
  std::vector<std::uint8_t> shown_;
  std::int64_t timebase_num_ = 0;
  std::int64_t timebase_den_ = 0;

  std::vector<int> display_order_;  // decode index per display slot, validated
  FrameOrder order_mode_ = FrameOrder::Decode;
  std::vector<int> order_;                // position -> decode index
  std::vector<int> position_of_;          // decode index -> position
  std::vector<std::int64_t> sorted_pts_;  // distinct pts, ascending

  std::map<SeriesRef, std::vector<double>> metrics_;  // by decode index
  std::map<SeriesRef, SeriesAggregator> aggregators_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_FRAME_TABLE_H
