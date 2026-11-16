// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PagedSpan (chapter 08 §8.2.3 reading strategy and §8.8, WI-1.209): windowed reads of a box payload larger than the
// 1 MiB a decoder normally sees. It fetches 64 KiB pages on demand and hands out windows of at most 1 MiB, so no code
// path holds a copy of a larger payload. A decoder walks a large table window by window, building a BitReader over
// each window.
//
// SPEC-ASSUMPTION(#630): BitReader (a public C++ helper header, chapter 7 §7.2) reads a std::span and is not changed
// here, so "a BitReader over a PagedSpan" is realised as one BitReader per window; BoxContext::payload_pages carries
// the PagedSpan to the decoders that walk large tables.

#ifndef OVT_CONTAINER_ISOBMFF_PAGED_SPAN_H
#define OVT_CONTAINER_ISOBMFF_PAGED_SPAN_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::isobmff {

class PagedSpan {
 public:
  static constexpr uint64_t kPageSize = uint64_t{64} << 10U;
  static constexpr size_t kMaxWindow = size_t{1} << 20U;  // §8.2.3: never more than 1 MiB at a time

  // The `length` bytes at `begin` of `file`; the range is clamped to the file size.
  PagedSpan(const io::RandomAccessFile& file, uint64_t begin, uint64_t length);
  // A payload that is already in memory (a decoder run outside the walker); `memory` must outlive the PagedSpan.
  explicit PagedSpan(std::span<const uint8_t> memory) noexcept;

  [[nodiscard]] uint64_t size() const noexcept { return size_; }
  // The bytes [offset, offset + n), clamped to size() and kMaxWindow. Valid until the next window() call.
  [[nodiscard]] std::span<const uint8_t> window(uint64_t offset, size_t n);
  // Bytes read from the file so far (whole pages); 0 for a memory span.
  [[nodiscard]] uint64_t bytes_fetched() const noexcept { return fetched_; }

 private:
  const io::RandomAccessFile* file_ = nullptr;
  std::span<const uint8_t> memory_;
  uint64_t begin_ = 0;
  uint64_t size_ = 0;
  uint64_t fetched_ = 0;
  uint64_t buf_begin_ = 0;  // payload offset of buf_[0]
  std::vector<uint8_t> buf_;
};

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_PAGED_SPAN_H
