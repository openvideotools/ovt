// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Heatmap value extraction and normalisation (chapter 10 §10.5.5). WI-2.430.

#include "overlay/heatmap_source.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace ovt::render::detail {
namespace {

constexpr size_t kMinParallel = 4096;  // below this many cells the job overhead outweighs the work

// A CellGrid vector is usable only when it has exactly one entry per cell (the schema allows empty = not provided).
template <typename V>
const V* usable(const V* v, size_t cells, const char* name, std::vector<std::string>& diag) {
  if (v == nullptr || v->size() == 0) {
    return nullptr;
  }
  if (v->size() != cells) {
    diag.push_back(std::string("CellGrid.") + name + " has " + std::to_string(v->size()) + " entries, expected " +
                   std::to_string(cells) + "; ignored");
    return nullptr;
  }
  return v;
}

struct Raw {
  float value = 0;
  bool valid = false;
};

}  // namespace

// SPEC-ASSUMPTION(#318): WI-2.430 step 5 asks for ovt::jobs::Pool, but that C++ helper is not exported from
// libovt_core (hidden symbols, "no ABI promise", jobs.hpp) and the C API has no submit entry point. The chunks run on
// short-lived std::thread workers instead; the result does not depend on the thread count.
void parallel_ranges(size_t count, unsigned chunks, const std::function<void(size_t, size_t)>& fn) {
  if (count == 0) {
    return;
  }
  if (chunks <= 1 || count < kMinParallel) {
    fn(0, count);
    return;
  }
  const size_t step = (count + chunks - 1) / chunks;
  std::vector<std::thread> workers;
  std::atomic<bool> failed{false};
  size_t begin = step;  // the calling thread takes chunk 0
  for (; begin < count; begin += step) {
    const size_t end = std::min(count, begin + step);
    try {
      workers.emplace_back([&fn, &failed, begin, end] {
        try {
          fn(begin, end);
        } catch (...) {
          failed = true;
        }
      });
    } catch (const std::system_error&) {  // cannot start a thread: run this chunk here
      fn(begin, end);
    }
  }
  fn(0, std::min(count, step));
  for (auto& w : workers) {
    w.join();
  }
  if (failed) {  // a worker threw (bad_alloc): redo the whole range inline; the passes are idempotent
    fn(0, count);
  }
}

namespace {

// The CellGrid columns a heatmap source can read; null = not provided (or unusable).
struct Columns {
  const flatbuffers::Vector<int16_t>* qp = nullptr;
  const flatbuffers::Vector<uint32_t>* bits = nullptr;
  const flatbuffers::Vector<int16_t>* mv0x = nullptr;
  const flatbuffers::Vector<int16_t>* mv0y = nullptr;
  const flatbuffers::Vector<uint8_t>* txw = nullptr;
  const flatbuffers::Vector<uint8_t>* txh = nullptr;
  const flatbuffers::Vector<uint8_t>* filt = nullptr;
  const flatbuffers::Vector<int8_t>* ref0 = nullptr;
  const flatbuffers::Vector<int16_t>* intra = nullptr;
};

Columns load_columns(const ovt::fb::CellGrid& g, size_t cells, std::vector<std::string>& diag) {
  Columns c;
  c.qp = usable(g.qp(), cells, "qp", diag);
  c.bits = usable(g.bits_x8(), cells, "bits_x8", diag);
  c.mv0x = usable(g.mv0_x(), cells, "mv0_x", diag);
  c.mv0y = usable(g.mv0_y(), cells, "mv0_y", diag);
  c.txw = usable(g.tx_log2_w(), cells, "tx_log2_w", diag);
  c.txh = usable(g.tx_log2_h(), cells, "tx_log2_h", diag);
  c.filt = usable(g.filter_idx(), cells, "filter_idx", diag);
  c.ref0 = usable(g.ref0(), cells, "ref0", diag);
  c.intra = usable(g.intra_mode_y(), cells, "intra_mode_y", diag);
  return c;
}

using Index = flatbuffers::uoffset_t;

Raw raw_mv_magnitude(const Columns& c, Index i) {
  if (c.mv0x == nullptr || c.mv0y == nullptr) {
    return {};
  }
  if (c.ref0 != nullptr && c.ref0->Get(i) < 0) {
    return {};  // no list-0 motion at this cell
  }
  const double x = c.mv0x->Get(i);
  const double y = c.mv0y->Get(i);
  return {static_cast<float>(std::sqrt((x * x) + (y * y)) / 16.0), true};
}

Raw raw_tx_size(const Columns& c, Index i) {
  if (c.txw == nullptr || c.txh == nullptr) {
    return {};
  }
  const uint8_t w = c.txw->Get(i);
  const uint8_t h = c.txh->Get(i);
  return (w == 0xFF || h == 0xFF) ? Raw{} : Raw{static_cast<float>(w + h), true};
}

// One cell's source value; `log_bits` selects the log2 form of BitsX8 (auto range only).
Raw raw_at(const Columns& c, HeatmapSource src, bool log_bits, Index i) {
  switch (src) {
    case HeatmapSource::QP:
      return (c.qp != nullptr && c.qp->Get(i) >= 0) ? Raw{static_cast<float>(c.qp->Get(i)), true} : Raw{};
    case HeatmapSource::BitsX8: {
      if (c.bits == nullptr) {
        return {};
      }
      const double b = static_cast<double>(c.bits->Get(i)) / 8.0;
      return {static_cast<float>(log_bits ? std::log2(1.0 + b) : b), true};
    }
    case HeatmapSource::MvMagnitude:
      return raw_mv_magnitude(c, i);
    case HeatmapSource::TxSize:
      return raw_tx_size(c, i);
    case HeatmapSource::FilterIdx:
      return (c.filt != nullptr && c.filt->Get(i) != 0xFF) ? Raw{static_cast<float>(c.filt->Get(i)), true} : Raw{};
    case HeatmapSource::RefIdx:
      return (c.ref0 != nullptr && c.ref0->Get(i) >= 0) ? Raw{static_cast<float>(c.ref0->Get(i)), true} : Raw{};
    case HeatmapSource::IntraMode:
      return (c.intra != nullptr && c.intra->Get(i) >= 0) ? Raw{static_cast<float>(c.intra->Get(i)), true} : Raw{};
    default:
      return {};
  }
}

constexpr unsigned kChunks = 4;

// Pass 1: raw values and the min/max over the applicable cells (one partial per chunk, combined in a fixed order).
std::pair<float, float> gather(const Columns& c, const OverlayConfig& cfg, bool log_bits, std::vector<float>& raw,
                               std::vector<uint8_t>& valid) {
  const size_t cells = raw.size();
  const size_t step = (cells + kChunks - 1) / kChunks;
  std::vector<std::pair<float, float>> partial(
      kChunks, {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()});
  parallel_ranges(cells, kChunks, [&](size_t begin, size_t end) {
    auto& p = partial[std::min<size_t>(begin / step, kChunks - 1)];
    for (size_t i = begin; i < end; ++i) {
      const Raw r = raw_at(c, cfg.heatmap, log_bits, static_cast<Index>(i));
      if (!r.valid) {
        continue;
      }
      raw[i] = r.value;
      valid[i] = 1;
      p.first = std::min(p.first, r.value);
      p.second = std::max(p.second, r.value);
    }
  });
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  for (const auto& p : partial) {
    lo = std::min(lo, p.first);
    hi = std::max(hi, p.second);
  }
  return lo <= hi ? std::make_pair(lo, hi) : std::make_pair(0.0F, 0.0F);
}

// The range to normalise against: explicit, else the source default (QP, TxSize), else the data range.
std::pair<float, float> resolve_range(const OverlayConfig& cfg, bool explicit_range, bool is_av1,
                                      std::pair<float, float> data) {
  if (explicit_range) {
    return {cfg.heatmap_min, cfg.heatmap_max};
  }
  if (cfg.heatmap == HeatmapSource::QP) {
    return {0.0F, is_av1 ? 255.0F : 51.0F};
  }
  if (cfg.heatmap == HeatmapSource::TxSize) {
    return {4.0F, 12.0F};
  }
  return data;
}

}  // namespace

bool build_heatmap(const ovt::fb::CellGrid& grid, bool is_av1, const OverlayConfig& cfg, const uint16_t* custom,
                   HeatmapResult& out, std::vector<std::string>& diag) {
  out = HeatmapResult{};
  const size_t cells = static_cast<size_t>(grid.cells_w()) * grid.cells_h();
  if (cells == 0 || cfg.heatmap == HeatmapSource::None) {
    return false;
  }

  // Custom / MetricMap: the host already produced 0..65535 per cell; nothing to normalise (§10.2.5).
  if (cfg.heatmap == HeatmapSource::Custom || cfg.heatmap == HeatmapSource::MetricMap) {
    if (custom == nullptr) {
      diag.emplace_back("HeatmapSource::Custom/MetricMap needs custom_cells; heatmap left empty");
      return false;
    }
    const std::span<const uint16_t> in(custom, cells);
    out.values.assign(in.begin(), in.end());
    out.valid.assign(cells, 1);
    out.lo = 0;
    out.hi = 65535.0F;
    return true;
  }

  const Columns cols = load_columns(grid, cells, diag);
  // SPEC-ASSUMPTION(#317): the explicit range is in the source's natural unit (QP, bits, luma samples, ...). Only the
  // *auto* range of BitsX8 is log2 scaled (§10.2.5 config comment); an explicit range compares against bits_x8 / 8.
  const bool explicit_range = cfg.heatmap_min != 0.0F || cfg.heatmap_max != 0.0F;
  const bool log_bits = cfg.heatmap == HeatmapSource::BitsX8 && !explicit_range;

  std::vector<float> raw(cells, 0.0F);
  out.valid.assign(cells, 0);
  const auto data_range = gather(cols, cfg, log_bits, raw, out.valid);
  const auto range = resolve_range(cfg, explicit_range, is_av1, data_range);
  out.lo = range.first;
  out.hi = range.second;

  // Pass 2: normalise. A degenerate range (hi <= lo) maps every applicable cell to 0.
  out.values.assign(cells, 0);
  const double lo = out.lo;
  const double span = static_cast<double>(out.hi) - lo;
  parallel_ranges(cells, kChunks, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      if (out.valid[i] != 0 && span > 0.0) {
        const double v = std::clamp((static_cast<double>(raw[i]) - lo) / span, 0.0, 1.0);
        out.values[i] = static_cast<uint16_t>(std::lround(v * 65535.0));
      }
    }
  });
  return true;
}

}  // namespace ovt::render::detail
