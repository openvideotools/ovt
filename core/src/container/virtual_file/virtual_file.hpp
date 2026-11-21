// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// VirtualFile (chapter 08 §8.6.7, WI-4.210): the init segment and the media segments of one DASH Representation or
// HLS variant as one RandomAccessFile, with the SegmentMap that says which segment every logical byte came from.
// The ISOBMFF and MPEG-TS readers run on it unchanged. Segments are fetched on demand and kept in a SegmentCache.
// SPEC-ASSUMPTION(#629): segment_index is 0 for an init segment and 1, 2, ... for the media segments in order, so
// "the third media segment" is segment_index 3 whether or not an init segment exists.

#ifndef OVT_CONTAINER_VIRTUAL_FILE_VIRTUAL_FILE_HPP
#define OVT_CONTAINER_VIRTUAL_FILE_VIRTUAL_FILE_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "container/virtual_file/fetcher.hpp"
#include "container/virtual_file/mpd.hpp"
#include "container/virtual_file/segment_cache.hpp"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::vfile {

// One segment as the manifest lists it.
struct SegmentSpec {
  std::string url;
  mpd::ByteRange byte_range;  // length 0 = whole resource
  bool is_init = false;
  std::string period_id;
  uint32_t discontinuity_sequence = 0;
};

// One SegmentMap entry: where the segment sits in the logical byte space and where it came from.
struct SegmentEntry {
  uint64_t logical_offset = 0;
  uint64_t length = 0;
  std::string url;
  uint32_t segment_index = 0;
  mpd::ByteRange byte_range;
  std::string period_id;
  uint32_t discontinuity_sequence = 0;
  bool is_init = false;
};
using SegmentMap = std::vector<SegmentEntry>;

class VirtualFile final : public io::RandomAccessFile {
 public:
  // Computes the layout: a segment's size is its byte range, else the fetcher's size(), else the segment is fetched
  // in full (in manifest order). Throws io::IoError when a segment cannot be sized or fetched.
  [[nodiscard]] static std::unique_ptr<VirtualFile> create(std::vector<SegmentSpec> segments,
                                                           std::shared_ptr<Fetcher> fetcher,
                                                           std::shared_ptr<SegmentCache> cache,
                                                           std::string display_name);

  [[nodiscard]] const SegmentMap& segment_map() const noexcept { return map_; }
  // The entry whose span holds `offset`; nullptr at or past the end.
  [[nodiscard]] const SegmentEntry* segment_for_offset(uint64_t offset) const noexcept;

  // {"kind":"segment", "segment_index", "is_init", "url", "logical_offset", "length", "offset_in_segment",
  // "byte_range":{"offset", "length"}, "period_id", "discontinuity_sequence"}; nullopt when `offset` is outside the
  // file.
  [[nodiscard]] std::optional<std::string> provenance_json(uint64_t offset) const;
  // The whole SegmentMap as a JSON array of the same objects (without offset_in_segment).
  [[nodiscard]] std::string segment_map_json() const;

  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override;
  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override;
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*access*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }

 private:
  VirtualFile(io::FileInfo info, SegmentMap map, std::shared_ptr<Fetcher> fetcher, std::shared_ptr<SegmentCache> cache)
      : RandomAccessFile(std::move(info)),
        map_(std::move(map)),
        fetcher_(std::move(fetcher)),
        cache_(std::move(cache)) {}

  [[nodiscard]] std::shared_ptr<io::RandomAccessFile> open_segment(size_t index) const;

  SegmentMap map_;
  std::shared_ptr<Fetcher> fetcher_;
  std::shared_ptr<SegmentCache> cache_;
  mutable std::mutex mutex_;  // guards open_ and serialises fetches
  mutable std::deque<std::pair<size_t, std::shared_ptr<io::RandomAccessFile>>> open_;  // most recent first
};

}  // namespace ovt::container::vfile

#endif  // OVT_CONTAINER_VIRTUAL_FILE_VIRTUAL_FILE_HPP
