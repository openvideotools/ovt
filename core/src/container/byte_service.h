// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The byte service of an ovt_Container (chapter 08 §8.8, WI-1.234): the lazily created PageCache, the lock that
// protects the tree buffer against ovt_container_expand, and the internal unit-bytes path that WI-1.130 calls
// from ovt_stream_unit_bytes (the public declaration is WI-1.130's, chapter 07).

#ifndef OVT_CONTAINER_BYTE_SERVICE_H
#define OVT_CONTAINER_BYTE_SERVICE_H

#include <cstdint>
#include <memory>
#include <mutex>

#include "container/page_cache.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/status.h"

namespace ovt::container {

// Thread-safety (the documented exception to chapter 12 §12.3.1 rule 4, chapter 08 §8.8): ovt_container_bytes,
// ovt_container_box_at, ovt_container_stats_json and ovt_container_size may run concurrently with any other call
// on the same ovt_Container; they take only the cache mutex or the tree lock.
// SPEC-ASSUMPTION(#683): documented here, not in core/include/ovt/container.h (public header, not touched).
// Owned by ovt_Container. Default-constructible so that handles built without a file (tests) still work.
class ByteService {
 public:
  // The cache for `file`, created on first use. Thread-safe.
  PageCache& cache(const io::RandomAccessFile& file);
  // Held by ovt_container_box_at while it walks ovt_Container::tree and by ovt_container_expand while it swaps it.
  [[nodiscard]] std::mutex& tree_mutex() noexcept { return tree_mutex_; }

 private:
  std::mutex create_mutex_;
  std::unique_ptr<PageCache> cache_;
  std::mutex tree_mutex_;
};

enum class UnitCodec : uint8_t { H264, H265, Av1, Other };

// Where a unit sits in the file: the first byte after the length prefix or start code, and its length (§8.7.1).
struct UnitLocation {
  uint64_t file_offset = 0;
  uint64_t length = 0;
};

// SPEC-ASSUMPTION(#683): this internal function is WI-1.234's share of ovt_stream_unit_bytes; WI-1.130 adds the public
// declaration and calls it. PagedSpan is not produced here (moved to WI-1.209 by #630).
// The body of ovt_stream_unit_bytes (backbone §0.6.1). `out_raw` receives the bytes as stored; an AV1 OBU stored
// without obu_size is re-framed with obu_has_size_field = 1 and a leb128 size. `out_rbsp` receives the
// emulation-prevention-free NAL for H.264/H.265 and an empty buffer for AV1 and other codecs. Either may be null.
// Both are caller-owned (ovt_buffer_free). OVT_E_CORRUPT when the unit lies outside the file or is empty AV1 or
// H.26x; OVT_E_IO / OVT_E_NOMEM from the reads and allocations. Never throws.
ovt_status read_unit_bytes(PageCache& cache, UnitCodec codec, UnitLocation where, ovt_Buffer* out_raw,
                           ovt_Buffer* out_rbsp) noexcept;

}  // namespace ovt::container

#endif  // OVT_CONTAINER_BYTE_SERVICE_H
