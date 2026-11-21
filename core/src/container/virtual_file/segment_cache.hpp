// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The on-disk segment cache of the DASH/HLS virtual file (chapter 08 §8.6.7, WI-4.210): one file per fetched
// segment under `<cache root>/segments/<sha256(url)>`, evicted least-recently-used once the total passes the cap.
// SPEC-ASSUMPTION(#629): a byte-range segment is stored under sha256(url + "\n" + offset + "-" + length), so two
// ranges of one resource never share a file; a whole-resource segment uses sha256(url) exactly as the spec says.

#ifndef OVT_CONTAINER_VIRTUAL_FILE_SEGMENT_CACHE_HPP
#define OVT_CONTAINER_VIRTUAL_FILE_SEGMENT_CACHE_HPP

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "container/virtual_file/mpd.hpp"

namespace ovt::container::vfile {

inline constexpr uint64_t kDefaultCacheCapBytes = 2ULL << 30;  // 2 GiB

// Lower-case hex SHA-256 (FIPS 180-4) of `data`.
[[nodiscard]] std::string sha256_hex(std::string_view data);

class SegmentCache {
 public:
  // `dir` is the directory that holds the segment files (`.../segments`); it is created on first store.
  explicit SegmentCache(std::filesystem::path dir, uint64_t cap_bytes = kDefaultCacheCapBytes);

  [[nodiscard]] static std::string key(std::string_view url, const mpd::ByteRange& range);

  // The path of the cached segment, or nullopt on a miss. A hit refreshes the file's recency.
  [[nodiscard]] std::optional<std::filesystem::path> lookup(const std::string& key);
  // Writes the bytes (temp file + rename, so a reader never sees a partial file), then evicts down to the cap.
  // Throws io::IoError on a write failure. The file just written is never evicted by its own store.
  std::filesystem::path store(const std::string& key, std::span<const uint8_t> bytes);

  [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }
  [[nodiscard]] uint64_t cap_bytes() const noexcept { return cap_; }
  // Total bytes of the segment files now in the directory.
  [[nodiscard]] uint64_t total_bytes() const;

 private:
  void evict_locked(const std::filesystem::path& keep);

  std::filesystem::path dir_;
  uint64_t cap_;
  std::mutex mutex_;
};

}  // namespace ovt::container::vfile

#endif  // OVT_CONTAINER_VIRTUAL_FILE_SEGMENT_CACHE_HPP
