// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PagedSpan (chapter 08 §8.2.3 / §8.8, WI-1.209). See the header.

#include "container/isobmff/paged_span.h"

#include <algorithm>

namespace ovt::container::isobmff {

PagedSpan::PagedSpan(const io::RandomAccessFile& file, uint64_t begin, uint64_t length)
    : file_(&file),
      begin_(std::min(begin, file.size())),
      size_(std::min(length, file.size() - std::min(begin, file.size()))) {}

PagedSpan::PagedSpan(std::span<const uint8_t> memory) noexcept : memory_(memory), size_(memory.size()) {}

std::span<const uint8_t> PagedSpan::window(uint64_t offset, size_t n) {
  if (offset >= size_) {
    return {};
  }
  const auto len = std::min<uint64_t>({n, kMaxWindow, size_ - offset});
  if (file_ == nullptr) {
    return memory_.subspan(offset, len);
  }
  const bool cached = offset >= buf_begin_ && offset + len <= buf_begin_ + buf_.size();
  if (!cached) {
    // Whole 64 KiB pages (relative to the payload start) covering the window; the last one is cut at the payload end.
    const uint64_t first = offset / kPageSize * kPageSize;
    const uint64_t last = std::min((offset + len + kPageSize - 1) / kPageSize * kPageSize, size_);
    buf_.resize(last - first);
    buf_.resize(file_->read(begin_ + first, buf_.size(), buf_.data()));
    buf_begin_ = first;
    fetched_ += buf_.size();
    if (offset >= buf_begin_ + buf_.size()) {  // the file shrank under us
      return {};
    }
  }
  const uint64_t avail = buf_begin_ + buf_.size() - offset;
  return std::span<const uint8_t>(buf_).subspan(offset - buf_begin_, std::min(len, avail));
}

}  // namespace ovt::container::isobmff
