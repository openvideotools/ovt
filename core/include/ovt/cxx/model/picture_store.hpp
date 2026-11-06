// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The in-memory plane and FrameAnalysis store with per-category frame budgets, a hard memory cap and
// pinning (chapter 06 §6.6.3, backbone §0.7.3, WI-0.087).
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_PICTURE_STORE_HPP
#define OVT_CXX_MODEL_PICTURE_STORE_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/model/plane.hpp"

namespace ovt::model {

// SPEC-ASSUMPTION(#157): eviction counting, pin granularity, budget vs pins and the trim order below.
/// Holds planes (keyed by blob_id, §6.6.2) and FrameAnalysis buffers of open streams.
///
/// Entries are grouped into frames keyed by (source hash, decode_index) and kept in three LRU categories:
/// FrameAnalysis buffers, planes of every stage (incl. "tb-<id>" coefficient blobs) and metric maps. A frame
/// counts as one entry of its category whatever its number of stages and planes. Any access (put or get)
/// makes a frame the most recently used of its category.
///
/// After every put the store evicts least recently used, unpinned frames (never the frame just written)
/// while a category holds more frames than its budget, and then while resident bytes exceed 0.6 × the hard
/// cap. A put whose frame alone would exceed the cap, or that cannot fit under the cap without evicting a
/// pinned frame, throws NoMemError and changes nothing.
///
/// Thread-safe: one mutex guards the tables; blobs are immutable once published and plane copies run outside
/// the lock. An evicted blob stays valid for every holder of a shared_ptr to it.
class PictureStore {
 public:
  struct Limits {
    std::uint32_t analysis_frames = 64;  ///< FrameAnalysis LRU (16 on Android; §0.7.3).
    std::uint32_t plane_frames = 4;      ///< Plane LRU, all stages share it (2 on Android).
    std::uint32_t map_frames = 16;       ///< Metric-map LRU.
    // SPEC-ASSUMPTION(#157): the interface gives no default; 2 GiB is the desktop cap.
    std::uint64_t hard_cap_bytes = 2ULL << 30U;  ///< 2 GiB desktop, 1 GiB Android (§6.6.3).
  };

  struct Stats {
    std::uint64_t resident_bytes = 0;  ///< plane_bytes + map_bytes + analysis_bytes.
    std::uint64_t plane_bytes = 0;
    std::uint64_t map_bytes = 0;
    std::uint64_t analysis_bytes = 0;
    std::uint32_t plane_frames = 0;
    std::uint32_t plane_blobs = 0;
    std::uint32_t map_frames = 0;
    std::uint32_t map_blobs = 0;
    std::uint32_t analysis_frames = 0;
    std::uint32_t pinned_frames = 0;  ///< Frames (of any category) with at least one pinned blob_id.
    std::uint64_t evictions = 0;      ///< Frames evicted since construction, all categories.
    std::uint64_t hits = 0;           ///< get() and get_analysis() calls that found the entry.
    std::uint64_t misses = 0;         ///< get() and get_analysis() calls that did not.
  };

  /// Called once per evicted plane or map blob, outside the store's lock, from the thread whose put caused
  /// the eviction. The view is valid for the duration of the call. Should not throw: an exception escapes
  /// from that put after the store is updated, and the rest of its evicted blobs go unreported.
  using EvictCallback = std::function<void(std::string_view blob_id, PlaneView view)>;

  /// Throws std::invalid_argument when a frame budget or the cap is 0.
  explicit PictureStore(Limits limits);
  ~PictureStore();
  PictureStore(const PictureStore&) = delete;
  PictureStore& operator=(const PictureStore&) = delete;
  PictureStore(PictureStore&&) = delete;
  PictureStore& operator=(PictureStore&&) = delete;

  /// Stores a plane under @p blob_id, replacing a blob with the same id. @p src is copied into 64-byte
  /// aligned rows unless @p adopt is given, in which case the store takes that memory without copying
  /// (PlaneBlob::adopt). Throws std::invalid_argument for a blob_id outside the §6.6.2 grammar or a
  /// malformed view, and NoMemError as described above.
  std::shared_ptr<const PlaneBlob> put(
      std::string blob_id, PlaneView src,
      std::unique_ptr<std::uint8_t[]> adopt = {});  // NOLINT(*-avoid-c-arrays): WI-0.087

  /// The blob, or nullptr if absent (the caller regenerates it). Never evicts.
  std::shared_ptr<const PlaneBlob> get(std::string_view blob_id);

  /// Pins the frame @p blob_id belongs to: it is never evicted while any of its pins is held. Pins are
  /// counted per blob_id and may be taken before the blob is put. Throws std::invalid_argument for a
  /// blob_id outside the grammar.
  void pin(std::string_view blob_id);
  /// Releases one pin() of @p blob_id; a no-op when it holds none. Evicts nothing by itself.
  void unpin(std::string_view blob_id);

  /// Stores (or replaces) the FrameAnalysis buffer of frame (@p hash, @p decode_index). Throws
  /// std::invalid_argument for a negative decode_index and NoMemError as described above.
  void put_analysis(int decode_index, std::span<const std::uint8_t, 32> hash, std::vector<std::uint8_t> fa);
  /// The buffer, or nullptr if absent. Never evicts.
  std::shared_ptr<const std::vector<std::uint8_t>> get_analysis(int decode_index,
                                                                std::span<const std::uint8_t, 32> hash);

  [[nodiscard]] Stats stats() const;

  /// Sets (or, with an empty function, clears) the eviction hook. The cache layer writes planes to disk
  /// here when "cache planes" is on.
  void set_evict_callback(EvictCallback callback);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_PICTURE_STORE_HPP
