// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/virtual_file/virtual_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <span>

namespace ovt::container::vfile {
namespace {

constexpr size_t kMaxOpenSegments = 16;  // open cache files kept for reads

nlohmann::json entry_json(const SegmentEntry& e) {
  return nlohmann::json{{"kind", "segment"},
                        {"segment_index", e.segment_index},
                        {"is_init", e.is_init},
                        {"url", e.url},
                        {"logical_offset", e.logical_offset},
                        {"length", e.length},
                        {"byte_range", {{"offset", e.byte_range.offset}, {"length", e.byte_range.length}}},
                        {"period_id", e.period_id},
                        {"discontinuity_sequence", e.discontinuity_sequence}};
}

}  // namespace

std::unique_ptr<VirtualFile> VirtualFile::create(std::vector<SegmentSpec> segments, std::shared_ptr<Fetcher> fetcher,
                                                 std::shared_ptr<SegmentCache> cache, std::string display_name) {
  SegmentMap map;
  map.reserve(segments.size());
  uint64_t offset = 0;
  uint32_t media_index = 0;
  for (SegmentSpec& s : segments) {
    SegmentEntry e;
    e.url = std::move(s.url);
    e.byte_range = s.byte_range;
    e.is_init = s.is_init;
    e.period_id = std::move(s.period_id);
    e.discontinuity_sequence = s.discontinuity_sequence;
    e.segment_index = e.is_init ? 0 : ++media_index;
    e.logical_offset = offset;
    if (e.byte_range.length > 0) {
      e.length = e.byte_range.length;
    } else if (const auto total = fetcher->size(e.url)) {
      if (*total < e.byte_range.offset) {
        throw io::IoError(OVT_E_IO, 0, "byte range starts past the end of " + e.url);
      }
      e.length = *total - e.byte_range.offset;
    } else {
      // Size unknown (no Content-Length): fetch the segment now; it goes into the cache for later reads.
      const auto bytes = fetcher->fetch(e.url, e.byte_range);
      cache->store(SegmentCache::key(e.url, e.byte_range), bytes);
      e.length = bytes.size();
    }
    if (e.length > UINT64_MAX - offset) {
      throw io::IoError(OVT_E_IO, 0, "the segments add up to more than 2^64 bytes");
    }
    offset += e.length;
    map.push_back(std::move(e));
  }
  io::FileInfo info{offset, 0, std::move(display_name)};
  return std::unique_ptr<VirtualFile>(
      new VirtualFile(std::move(info), std::move(map), std::move(fetcher), std::move(cache)));
}

const SegmentEntry* VirtualFile::segment_for_offset(uint64_t offset) const noexcept {
  if (offset >= size()) {
    return nullptr;
  }
  // The first entry that ends after `offset`; zero-length entries end where they start, so they never match.
  const auto it =
      std::ranges::upper_bound(map_, offset, {}, [](const SegmentEntry& e) { return e.logical_offset + e.length; });
  return it == map_.end() ? nullptr : &*it;
}

std::optional<std::string> VirtualFile::provenance_json(uint64_t offset) const {
  const SegmentEntry* e = segment_for_offset(offset);
  if (e == nullptr) {
    return std::nullopt;
  }
  nlohmann::json j = entry_json(*e);
  j["offset_in_segment"] = offset - e->logical_offset;
  return j.dump();
}

std::string VirtualFile::segment_map_json() const {
  auto arr = nlohmann::json::array();
  for (const SegmentEntry& e : map_) {
    arr.push_back(entry_json(e));
  }
  return arr.dump();
}

std::shared_ptr<io::RandomAccessFile> VirtualFile::open_segment(size_t index) const {
  const std::scoped_lock lock(mutex_);
  for (auto it = open_.begin(); it != open_.end(); ++it) {
    if (it->first == index) {
      if (it != open_.begin()) {
        std::rotate(open_.begin(), it, it + 1);
      }
      return open_.front().second;
    }
  }
  const SegmentEntry& e = map_[index];
  const std::string key = SegmentCache::key(e.url, e.byte_range);
  auto path = cache_->lookup(key);
  if (!path) {
    const auto bytes = fetcher_->fetch(e.url, e.byte_range);
    path = cache_->store(key, bytes);
  }
  std::shared_ptr<io::RandomAccessFile> file = io::RandomAccessFile::open(path->string(), io::OpenMode::Pread);
  if (file->size() != e.length) {
    throw io::IoError(OVT_E_IO, 0,
                      "segment " + e.url + " has " + std::to_string(file->size()) + " bytes, the layout expects " +
                          std::to_string(e.length));
  }
  open_.emplace_front(index, file);
  if (open_.size() > kMaxOpenSegments) {
    open_.pop_back();
  }
  return file;
}

size_t VirtualFile::read(uint64_t offset, size_t len, uint8_t* dst) const {
  if (offset >= size() || len == 0) {
    return 0;
  }
  len = static_cast<size_t>(std::min<uint64_t>(len, size() - offset));
  size_t done = 0;
  while (done < len) {
    const uint64_t pos = offset + done;
    const SegmentEntry* e = segment_for_offset(pos);
    if (e == nullptr) {
      break;
    }
    const auto index = static_cast<size_t>(e - map_.data());
    const uint64_t in_seg = pos - e->logical_offset;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len - done, e->length - in_seg));
    const auto seg = open_segment(index);
    const size_t got = seg->read(in_seg, n, std::span<uint8_t>(dst, len).subspan(done).data());
    done += got;
    if (got != n) {
      throw io::IoError(OVT_E_IO, 0, "short read of the cached segment " + e->url);
    }
  }
  return done;
}

io::RandomAccessFile::Holder VirtualFile::view(uint64_t offset, size_t len) const {
  auto buf = std::make_shared<std::vector<uint8_t>>();
  if (offset < size()) {
    buf->resize(static_cast<size_t>(std::min<uint64_t>(len, size() - offset)));
    buf->resize(read(offset, buf->size(), buf->data()));
  }
  const std::span<const uint8_t> bytes(buf->data(), buf->size());
  return Holder{std::shared_ptr<const void>(buf, buf.get()), bytes};
}

}  // namespace ovt::container::vfile
