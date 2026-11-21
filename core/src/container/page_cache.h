// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The page cache of the byte service (chapter 08 §8.8, WI-1.234): 64 pages of 64 KiB (4 MiB) per ovt_Container,
// least-recently-used eviction, one mutex. Pages are never exposed by pointer: read() copies into the caller's
// memory, so an eviction can never invalidate anything a caller holds.

#ifndef OVT_CONTAINER_PAGE_CACHE_H
#define OVT_CONTAINER_PAGE_CACHE_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container {

class PageCache {
 public:
  static constexpr std::size_t kPageSize = std::size_t{64} * 1024;
  static constexpr std::size_t kCapacityPages = 64;

  struct Stats {
    uint64_t hits = 0;        // pages served from the cache
    uint64_t misses = 0;      // pages fetched from the file (one pread each)
    uint64_t bytes_read = 0;  // bytes fetched from the file, including the uncached bulk reads
  };

  // `file` must outlive the cache.
  explicit PageCache(const io::RandomAccessFile& file, std::size_t capacity_pages = kCapacityPages,
                     std::size_t page_size = kPageSize);

  PageCache(const PageCache&) = delete;
  PageCache& operator=(const PageCache&) = delete;
  PageCache(PageCache&&) = delete;
  PageCache& operator=(PageCache&&) = delete;
  ~PageCache() = default;

  // Copies min(len, size - offset) bytes at `offset` into `dst` and returns the count (0 at or after EOF).
  // A read longer than half the cache goes straight to the file: a bulk unit read must not evict the rows the
  // hex view is showing. Throws ovt::io::IoError on an I/O failure. Thread-safe.
  std::size_t read(uint64_t offset, std::size_t len, uint8_t* dst);

  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::size_t resident_pages() const;
  [[nodiscard]] uint64_t file_size() const noexcept { return file_.size(); }

 private:
  struct Page {
    std::vector<uint8_t> bytes;
    std::list<uint64_t>::iterator lru;  // position in lru_
  };

  // Caller holds mutex_. Returns the page, fetching and evicting as needed.
  const Page& page(uint64_t page_no);

  const io::RandomAccessFile& file_;
  std::size_t capacity_;
  std::size_t page_size_;
  mutable std::mutex mutex_;
  std::list<uint64_t> lru_;  // front = most recently used
  std::unordered_map<uint64_t, Page> pages_;
  Stats stats_;
};

}  // namespace ovt::container

#endif  // OVT_CONTAINER_PAGE_CACHE_H
