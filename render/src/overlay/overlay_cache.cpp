// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.437: the overlay cache of chapter 10 §10.5.8 (see overlay_cache.h for the lifecycle).

#include "overlay/overlay_cache.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt_model_generated.h"

namespace ovt::render::detail {

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
constexpr uint64_t kMaxCells = 1ULL << 26;  // builder.cpp's bound: larger grids build no per-cell data

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

constexpr uint32_t kUniformLayers =
    bit(OverlayLayer::Selection) | bit(OverlayLayer::Probe) | bit(OverlayLayer::SelectionRect);

class Fnv {
 public:
  void bytes(const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
      h_ = (h_ ^ b[i]) * kFnvPrime;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }
  template <typename T>
  void value(T v) {
    bytes(&v, sizeof(v));
  }
  void str(const flatbuffers::String* s) {
    const std::string_view v = s != nullptr ? s->string_view() : std::string_view{};
    value(v.size());
    bytes(v.data(), v.size());
  }
  // Word-wise mix for long arrays (the host's per-cell values): one multiply per 8 bytes.
  void words(const uint16_t* p, size_t n) {
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
      uint64_t w = 0;
      std::memcpy(&w, p + i, sizeof(w));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      h_ = (h_ ^ w) * kFnvPrime;
    }
    for (; i < n; ++i) {
      value(p[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }
  [[nodiscard]] uint64_t get() const { return h_; }

 private:
  uint64_t h_ = kFnvOffset;
};

template <typename T>
size_t vec_bytes(const std::vector<T>& v) {
  return v.capacity() * sizeof(T);
}

template <typename T>
uint32_t len(const flatbuffers::Vector<T>* v) {
  return v != nullptr ? v->size() : 0U;
}

}  // namespace

OverlayConfig gpu_relevant_config(const OverlayConfig& cfg) {
  OverlayConfig g = cfg;
  const OverlayConfig defaults;
  g.layers &= ~kUniformLayers;
  g.selected_block = defaults.selected_block;
  g.hovered_block = defaults.hovered_block;  // WI-2.436 hover outline, a draw-time uniform like the selection
  g.selected_mv = defaults.selected_mv;
  g.selection_rect = defaults.selection_rect;
  g.probe_x = defaults.probe_x;
  g.probe_y = defaults.probe_y;
  g.heatmap_alpha = defaults.heatmap_alpha;
  g.lut = defaults.lut;
  g.label_zoom_threshold = defaults.label_zoom_threshold;
  g.mv_min_px = defaults.mv_min_px;
  g.line_px = defaults.line_px;
  if (g.heatmap == HeatmapSource::None) {  // the range only shapes heatmap data
    g.heatmap_min = 0;
    g.heatmap_max = 0;
  }
  return g;
}

uint64_t overlay_config_hash(const OverlayConfig& cfg) {
  const OverlayConfig g = gpu_relevant_config(cfg);
  Fnv h;
  h.value(g.layers);
  h.value(static_cast<uint8_t>(g.heatmap));
  h.value(g.heatmap_min);
  h.value(g.heatmap_max);
  h.value(static_cast<uint8_t>(g.label));
  h.value(g.mv_scale);
  return h.get();
}

bool OverlayCacheKey::same_as(const OverlayCacheKey& o) const {
  // Floats compare by value: a NaN mv_scale or range never matches, so such a set is rebuilt (and never wrong).
  return decode_index == o.decode_index && frame == o.frame && config == o.config && custom == o.custom &&
         gpu.layers == o.gpu.layers && gpu.heatmap == o.gpu.heatmap && gpu.heatmap_min == o.gpu.heatmap_min &&
         gpu.heatmap_max == o.gpu.heatmap_max && gpu.label == o.gpu.label && gpu.mv_scale == o.gpu.mv_scale;
}

bool overlay_cache_key(const ovt::fb::FrameAnalysis& fa, const OverlayConfig& cfg, const uint16_t* custom_cells,
                       OverlayCacheKey& out) {
  const ovt::fb::FrameInfo* info = fa.frame();
  if (info == nullptr || info->id() == nullptr) {
    return false;
  }
  out = OverlayCacheKey{};
  out.decode_index = info->id()->decode_index();
  // The key also fingerprints the frame (WI-2.437 goal), so that decode_index n of another stream (or a
  // re-analysis with another plugin) is not served from the cache.
  Fnv f;
  f.value(info->id()->display_index());
  f.value(info->poc());
  f.value(info->pts());
  f.value(info->dts());
  f.value(info->size_bytes());
  f.value(static_cast<uint8_t>(info->type()));
  if (info->coded_size() != nullptr) {
    f.value(info->coded_size()->w());
    f.value(info->coded_size()->h());
  }
  f.str(fa.plugin_id());
  f.str(fa.plugin_version());
  const ovt::fb::CellGrid* grid = fa.grid();
  f.value(grid != nullptr ? grid->cells_w() : 0U);
  f.value(grid != nullptr ? grid->cells_h() : 0U);
  f.value(len(fa.blocks()));
  f.value(len(fa.mvs()));
  f.value(len(fa.partitions()));
  out.frame = f.get();
  out.gpu = gpu_relevant_config(cfg);
  out.config = overlay_config_hash(cfg);
  const bool host_cells = cfg.heatmap == HeatmapSource::Custom || cfg.heatmap == HeatmapSource::MetricMap;
  if (custom_cells != nullptr && host_cells && grid != nullptr) {
    const uint64_t cells = static_cast<uint64_t>(grid->cells_w()) * grid->cells_h();
    if (cells <= kMaxCells) {
      Fnv c;
      c.words(custom_cells, static_cast<size_t>(cells));
      out.custom = c.get() | 1U;  // never 0, the "no host cells" value
    }
  }
  return true;
}

size_t overlay_set_bytes(const OverlaySet& set) {
  size_t n = sizeof(OverlaySet);
  n += vec_bytes(set.cell_attributes) + vec_bytes(set.heatmap) + vec_bytes(set.heat_valid);
  n += vec_bytes(set.partition_lines) + vec_bytes(set.grid_nodes);
  n += vec_bytes(set.cell_block_id);
  n += vec_bytes(set.deblock_edges) + vec_bytes(set.block_rects) + vec_bytes(set.mv_rects);
  n += vec_bytes(set.arrows.geom) + vec_bytes(set.arrows.attr);
  for (const std::string& d : set.diagnostics) {
    n += d.capacity();
  }
  n += vec_bytes(set.diagnostics);
  return n;
}

std::shared_ptr<const OverlaySet> OverlayCache::find(const OverlayCacheKey& key) {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->key.same_as(key)) {
      lru_.splice(lru_.begin(), lru_, it);
      ++hits_;
      return lru_.front().data;
    }
  }
  ++misses_;
  return nullptr;
}

void OverlayCache::insert(const OverlayCacheKey& key, std::shared_ptr<const OverlaySet> data) {
  if (data == nullptr) {
    return;
  }
  const size_t bytes = overlay_set_bytes(*data);
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = lru_.begin(); it != lru_.end(); ++it) {
    if (it->key.same_as(key)) {
      bytes_ -= it->bytes;
      lru_.erase(it);
      break;
    }
  }
  if (bytes > budget_ || max_entries_ == 0) {
    return;  // the handle on screen keeps it alive; caching it would evict everything else for nothing
  }
  lru_.push_front(Entry{key, std::move(data), bytes});
  bytes_ += bytes;
  evict_locked();
}

void OverlayCache::evict_locked() {
  while (!lru_.empty() && (lru_.size() > max_entries_ || bytes_ > budget_)) {
    bytes_ -= lru_.back().bytes;
    lru_.pop_back();  // a handle still showing these arrays keeps them alive
  }
}

void OverlayCache::clear() {
  std::list<Entry> dropped;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    dropped.swap(lru_);
    bytes_ = 0;
  }
  // `dropped` frees the arrays outside the lock.
}

size_t OverlayCache::size() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return lru_.size();
}

size_t OverlayCache::bytes() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return bytes_;
}

uint64_t OverlayCache::hits() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return hits_;
}

uint64_t OverlayCache::misses() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return misses_;
}

std::unique_ptr<OverlaySet> build_cached(OverlayCache* cache, const ovt::fb::FrameAnalysis* fa,
                                         const OverlayConfig& cfg, const uint16_t* custom_cells) {
  OverlayCacheKey key;
  if (cache == nullptr || fa == nullptr || !overlay_cache_key(*fa, cfg, custom_cells, key)) {
    return build_overlay_set(fa, cfg, custom_cells);
  }
  if (std::shared_ptr<const OverlaySet> hit = cache->find(key)) {
    auto handle = std::make_unique<OverlaySet>();
    handle->config = cfg;
    handle->shared = std::move(hit);
    return handle;
  }
  std::unique_ptr<OverlaySet> set = build_overlay_set(fa, cfg, custom_cells);
  set->cache_tagged = true;
  set->cache_key = key;
  return set;
}

std::unique_ptr<OverlaySet> adopt_into_cache(OverlayCache& cache, std::unique_ptr<OverlaySet> set) {
  if (set == nullptr || set->shared != nullptr || !set->cache_tagged) {
    return set;
  }
  const OverlayConfig cfg = set->config;
  const OverlayCacheKey key = set->cache_key;
  set->cache_tagged = false;
  std::shared_ptr<const OverlaySet> data(std::move(set));
  cache.insert(key, data);
  auto handle = std::make_unique<OverlaySet>();
  handle->config = cfg;
  handle->shared = std::move(data);
  return handle;
}

}  // namespace ovt::render::detail

namespace ovt::render {

// WI-2.437: both overloads go through the owner's overlay cache (overlay_cache.h). A hit returns a handle that
// shares the cached arrays, so a layer toggled back, a selection or probe move, or one of the last 8 frames costs no
// rebuild.
std::unique_ptr<OverlaySet> OverlayBuilder::build(const ovt::fb::FrameAnalysis* fa, const OverlayConfig& cfg,
                                                  const uint16_t* custom_cells) {
  return detail::build_cached(detail::overlay_cache(*owner_), fa, cfg, custom_cells);
}

std::unique_ptr<OverlaySet> OverlayBuilder::build(const uint8_t* buf, size_t size, const OverlayConfig& cfg,
                                                  const uint16_t* custom_cells) {
  if (buf == nullptr || !detail::verify_frame_analysis(buf, size)) {
    return nullptr;
  }
  return build(ovt::fb::GetFrameAnalysis(buf), cfg, custom_cells);
}

}  // namespace ovt::render
