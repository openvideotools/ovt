// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The overlay cache of chapter 10 §10.5.8 (WI-2.437): an LRU of 8 built OverlaySets per VideoSurface, keyed by
// (decode_index, hash of the OverlayConfig fields that change the built data), with a byte budget of 256 MB on desktop
// and 64 MB on Android. A cache hit hands out a handle OverlaySet that shares the cached arrays (OverlaySet::shared)
// and carries the requested configuration, so toggling a layer back, moving the selection or the probe, or stepping
// back to one of the last 8 frames costs no rebuild. Internal to libovt-render.
//
// Lifecycle: OverlayBuilder::build() looks the key up; a miss builds a new, owned set tagged with its key. When the
// host passes a tagged set to set_overlays(), the surface moves its arrays into shared ownership, inserts them into
// its cache and keeps a handle on screen. An entry that is evicted stays alive for as long as a handle uses it. The
// cache holds CPU arrays only: the GPU copies belong to the surface's OverlayLayerGpu, which re-uploads only when the
// shown arrays change, so surface teardown never races a cache entry. SPEC-ASSUMPTION(#706): CPU arrays, not GPU
// textures, are what the cache keeps; the upload (≤ 1.0 ms, §10.5.8) is not the cost a toggle has to avoid.

#ifndef OVT_RENDER_SRC_OVERLAY_OVERLAY_CACHE_H
#define OVT_RENDER_SRC_OVERLAY_OVERLAY_CACHE_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>

#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"
#include "ovt_model_generated.h"

namespace ovt::render::detail {

inline constexpr size_t kOverlayCacheEntries = 8;
#if defined(__ANDROID__)
inline constexpr size_t kOverlayCacheBudgetBytes = size_t{64} << 20U;
#else
inline constexpr size_t kOverlayCacheBudgetBytes = size_t{256} << 20U;
#endif

// `cfg` with every field that is a draw-time uniform reset to its default: Selection, Probe and SelectionRect layer
// bits, selected_block, hovered_block, selected_mv, selection_rect, probe_x/y (WI-2.437 note), and heatmap_alpha, lut,
// label_zoom_threshold, mv_min_px and line_px, which the passes read when drawing. What is left (the other layer
// bits, heatmap source and range, label content, mv_scale, which the arrow instances bake in) decides the built data.
// A pass that bakes another field into OverlaySet must keep it here.
OVT_RENDER_API OverlayConfig gpu_relevant_config(const OverlayConfig& cfg);

// 64-bit FNV-1a over gpu_relevant_config(cfg). Equal for configurations that differ only in uniforms.
OVT_RENDER_API uint64_t overlay_config_hash(const OverlayConfig& cfg);

// OverlayCacheKey is defined in overlay_set_impl.h (an OverlaySet carries one).

// The key of building `fa` with `cfg` and `custom_cells`. False when `fa` has no FrameInfo.id: such a frame is
// not cached.
OVT_RENDER_API bool overlay_cache_key(const ovt::fb::FrameAnalysis& fa, const OverlayConfig& cfg,
                                      const uint16_t* custom_cells, OverlayCacheKey& out);

// Bytes the CPU arrays of `set` hold (what the budget counts).
OVT_RENDER_API size_t overlay_set_bytes(const OverlaySet& set);

class OVT_RENDER_API OverlayCache {
 public:
  explicit OverlayCache(size_t max_entries = kOverlayCacheEntries, size_t budget_bytes = kOverlayCacheBudgetBytes)
      : max_entries_(max_entries), budget_(budget_bytes) {}

  // The cached arrays of `key`, now the most recently used entry; nullptr on a miss. Any thread.
  std::shared_ptr<const OverlaySet> find(const OverlayCacheKey& key);
  // Inserts (or replaces) `key` as the most recently used entry, then evicts least recently used entries while there
  // are more than max_entries or more than budget_bytes. A set larger than the whole budget is not kept. Any thread.
  void insert(const OverlayCacheKey& key, std::shared_ptr<const OverlaySet> data);
  void clear();

  [[nodiscard]] size_t size() const;
  [[nodiscard]] size_t bytes() const;
  [[nodiscard]] uint64_t hits() const;
  [[nodiscard]] uint64_t misses() const;

 private:
  struct Entry {
    OverlayCacheKey key;
    std::shared_ptr<const OverlaySet> data;
    size_t bytes = 0;
  };
  void evict_locked();

  mutable std::mutex mutex_;
  std::list<Entry> lru_;  // front = most recently used
  size_t max_entries_;
  size_t budget_;
  size_t bytes_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

// OverlayBuilder::build through `cache` (nullptr = no cache): a hit returns a handle sharing the cached arrays with
// config = `cfg`; a miss builds with build_overlay_set() and tags the set with its key for set_overlays().
OVT_RENDER_API std::unique_ptr<OverlaySet> build_cached(OverlayCache* cache, const ovt::fb::FrameAnalysis* fa,
                                                        const OverlayConfig& cfg, const uint16_t* custom_cells);

// set_overlays() side: turns a tagged, owned set into shared arrays in `cache` and returns the handle to show. A
// handle or an untagged set is returned unchanged.
OVT_RENDER_API std::unique_ptr<OverlaySet> adopt_into_cache(OverlayCache& cache, std::unique_ptr<OverlaySet> set);

// The cache of a surface made by VideoSurface::create() (video_surface_impl.cpp); nullptr for another VideoSurface.
OVT_RENDER_API OverlayCache* overlay_cache(VideoSurface& surface);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_OVERLAY_CACHE_H
