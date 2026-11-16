// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PageCache (chapter 08 §8.8, WI-1.234).

#include "container/page_cache.h"

#include <algorithm>
#include <cstring>

namespace ovt::container {

PageCache::PageCache(const io::RandomAccessFile& file, std::size_t capacity_pages, std::size_t page_size)
    : file_(file),
      capacity_(std::max<std::size_t>(capacity_pages, 1)),
      page_size_(std::max<std::size_t>(page_size, 1)) {}

const PageCache::Page& PageCache::page(uint64_t page_no) {
  if (const auto it = pages_.find(page_no); it != pages_.end()) {
    ++stats_.hits;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second;
  }
  ++stats_.misses;
  const uint64_t begin = page_no * page_size_;
  const auto len = static_cast<std::size_t>(std::min<uint64_t>(page_size_, file_.size() - begin));
  std::vector<uint8_t> bytes(len);
  bytes.resize(file_.read(begin, len, bytes.data()));  // a short read (file shrank) keeps what arrived
  stats_.bytes_read += bytes.size();
  if (pages_.size() >= capacity_) {
    pages_.erase(lru_.back());
    lru_.pop_back();
  }
  lru_.push_front(page_no);
  return pages_.emplace(page_no, Page{std::move(bytes), lru_.begin()}).first->second;
}

std::size_t PageCache::read(uint64_t offset, std::size_t len, uint8_t* dst) {
  const uint64_t size = file_.size();
  if (offset >= size || len == 0) {
    return 0;
  }
  const auto total = static_cast<std::size_t>(std::min<uint64_t>(len, size - offset));
  const std::lock_guard lock(mutex_);
  if (total > capacity_ * page_size_ / 2) {
    const std::size_t n = file_.read(offset, total, dst);
    stats_.bytes_read += n;
    return n;
  }
  std::size_t done = 0;
  while (done < total) {
    const uint64_t pos = offset + done;
    const Page& p = page(pos / page_size_);
    const std::size_t in_page = pos % page_size_;
    if (in_page >= p.bytes.size()) {
      break;  // short page: the file shrank under us
    }
    const std::size_t n = std::min(total - done, p.bytes.size() - in_page);
    std::memcpy(dst + done, p.bytes.data() + in_page, n);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    done += n;
  }
  return done;
}

PageCache::Stats PageCache::stats() const {
  const std::lock_guard lock(mutex_);
  return stats_;
}

std::size_t PageCache::resident_pages() const {
  const std::lock_guard lock(mutex_);
  return pages_.size();
}

}  // namespace ovt::container
