// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Heatmap value extraction and normalisation (chapter 10 §10.5.5). WI-2.430.

#include "overlay/heatmap_source.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
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
    out.values.assign(custom, custom + cells);
    out.valid.assign(cells, 1);
    out.lo = 0;
    out.hi = 65535.0F;
    return true;
  }

  const auto* qp = usable(grid.qp(), cells, "qp", diag);
  const auto* bits = usable(grid.bits_x8(), cells, "bits_x8", diag);
  const auto* mv0x = usable(grid.mv0_x(), cells, "mv0_x", diag);
  const auto* mv0y = usable(grid.mv0_y(), cells, "mv0_y", diag);
  const auto* txw = usable(grid.tx_log2_w(), cells, "tx_log2_w", diag);
  const auto* txh = usable(grid.tx_log2_h(), cells, "tx_log2_h", diag);
  const auto* filt = usable(grid.filter_idx(), cells, "filter_idx", diag);
  const auto* ref0 = usable(grid.ref0(), cells, "ref0", diag);
  const auto* intra = usable(grid.intra_mode_y(), cells, "intra_mode_y", diag);

  // SPEC-ASSUMPTION(#317): the explicit range is in the source's natural unit (QP, bits, luma samples, ...). Only the
  // *auto* range of BitsX8 is log2 scaled (§10.2.5 config comment); an explicit range compares against bits_x8 / 8.
  const bool explicit_range = cfg.heatmap_min != 0.0F || cfg.heatmap_max != 0.0F;
  const bool log_bits = cfg.heatmap == HeatmapSource::BitsX8 && !explicit_range;

  auto raw_at = [&](size_t i) -> Raw {
    switch (cfg.heatmap) {
      case HeatmapSource::QP:
        return qp != nullptr && qp->Get(static_cast<flatbuffers::uoffset_t>(i)) >= 0
                   ? Raw{static_cast<float>(qp->Get(static_cast<flatbuffers::uoffset_t>(i))), true}
                   : Raw{};
      case HeatmapSource::BitsX8: {
        if (bits == nullptr) {
          return {};
        }
        const double b = static_cast<double>(bits->Get(static_cast<flatbuffers::uoffset_t>(i))) / 8.0;
        return {static_cast<float>(log_bits ? std::log2(1.0 + b) : b), true};
      }
      case HeatmapSource::MvMagnitude: {
        if (mv0x == nullptr || mv0y == nullptr) {
          return {};
        }
        if (ref0 != nullptr && ref0->Get(static_cast<flatbuffers::uoffset_t>(i)) < 0) {
          return {};  // no list-0 motion at this cell
        }
        const double x = mv0x->Get(static_cast<flatbuffers::uoffset_t>(i));
        const double y = mv0y->Get(static_cast<flatbuffers::uoffset_t>(i));
        return {static_cast<float>(std::sqrt((x * x) + (y * y)) / 16.0), true};
      }
      case HeatmapSource::TxSize: {
        if (txw == nullptr || txh == nullptr) {
          return {};
        }
        const uint8_t w = txw->Get(static_cast<flatbuffers::uoffset_t>(i));
        const uint8_t h = txh->Get(static_cast<flatbuffers::uoffset_t>(i));
        return w == 0xFF || h == 0xFF ? Raw{} : Raw{static_cast<float>(w + h), true};
      }
      case HeatmapSource::FilterIdx: {
        if (filt == nullptr) {
          return {};
        }
        const uint8_t f = filt->Get(static_cast<flatbuffers::uoffset_t>(i));
        return f == 0xFF ? Raw{} : Raw{static_cast<float>(f), true};
      }
      case HeatmapSource::RefIdx: {
        if (ref0 == nullptr) {
          return {};
        }
        const int8_t r = ref0->Get(static_cast<flatbuffers::uoffset_t>(i));
        return r < 0 ? Raw{} : Raw{static_cast<float>(r), true};
      }
      case HeatmapSource::IntraMode: {
        if (intra == nullptr) {
          return {};
        }
        const int16_t m = intra->Get(static_cast<flatbuffers::uoffset_t>(i));
        return m < 0 ? Raw{} : Raw{static_cast<float>(m), true};
      }
      default:
        return {};
    }
  };

  // Pass 1: raw values and their range over the applicable cells, one partial min/max per chunk.
  std::vector<float> raw(cells, 0.0F);
  out.valid.assign(cells, 0);
  constexpr unsigned kChunks = 4;
  std::vector<std::pair<float, float>> partial(
      kChunks, {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()});
  const size_t step = (cells + kChunks - 1) / kChunks;
  parallel_ranges(cells, kChunks, [&](size_t begin, size_t end) {
    auto& p = partial[std::min<size_t>(begin / step, kChunks - 1)];
    for (size_t i = begin; i < end; ++i) {
      const Raw r = raw_at(i);
      if (!r.valid) {
        continue;
      }
      raw[i] = r.value;
      out.valid[i] = 1;
      p.first = std::min(p.first, r.value);
      p.second = std::max(p.second, r.value);
    }
  });
  float data_lo = std::numeric_limits<float>::max();
  float data_hi = std::numeric_limits<float>::lowest();
  for (const auto& p : partial) {
    data_lo = std::min(data_lo, p.first);
    data_hi = std::max(data_hi, p.second);
  }
  const bool any = data_lo <= data_hi;
  if (!any && cfg.heatmap != HeatmapSource::QP && cfg.heatmap != HeatmapSource::TxSize) {
    // nothing applicable: the pass draws nothing
    data_lo = data_hi = 0.0F;
  }

  float lo = data_lo;
  float hi = data_hi;
  if (explicit_range) {
    lo = cfg.heatmap_min;
    hi = cfg.heatmap_max;
  } else if (cfg.heatmap == HeatmapSource::QP) {
    lo = 0.0F;
    hi = is_av1 ? 255.0F : 51.0F;
  } else if (cfg.heatmap == HeatmapSource::TxSize) {
    lo = 4.0F;
    hi = 12.0F;
  }
  out.lo = lo;
  out.hi = hi;

  // Pass 2: normalise. A degenerate range (hi <= lo) maps every applicable cell to 0.
  out.values.assign(cells, 0);
  const double span = static_cast<double>(hi) - static_cast<double>(lo);
  parallel_ranges(cells, kChunks, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      if (out.valid[i] == 0 || span <= 0.0) {
        continue;
      }
      const double v = std::clamp((static_cast<double>(raw[i]) - static_cast<double>(lo)) / span, 0.0, 1.0);
      out.values[i] = static_cast<uint16_t>(std::lround(v * 65535.0));
    }
  });
  return true;
}

}  // namespace ovt::render::detail
