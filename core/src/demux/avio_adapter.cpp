// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The custom AVIOContext over ovt::io::RandomAccessFile (chapter 08 §8.7.2, WI-1.230). libavformat never
// opens a file itself: every byte it sees comes through read() below, so Android's pread-over-a-descriptor
// RandomAccessFile works unchanged (§8.7.2 rule 6).

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <utility>

#include "demux/demuxer.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace ovt::demux::detail {

std::unique_ptr<AvioAdapter> AvioAdapter::create(std::shared_ptr<ovt::io::RandomAccessFile> file) {
  std::unique_ptr<AvioAdapter> adapter(new AvioAdapter(std::move(file)));
  auto* buffer = static_cast<unsigned char*>(av_malloc(kBufferSize));
  if (buffer == nullptr) {
    return nullptr;
  }
  adapter->ctx_ = avio_alloc_context(buffer, kBufferSize, /*write_flag=*/0, adapter.get(), &AvioAdapter::read,
                                     /*write_packet=*/nullptr, &AvioAdapter::seek);
  if (adapter->ctx_ == nullptr) {
    av_free(buffer);
    return nullptr;
  }
  adapter->ctx_->seekable = AVIO_SEEKABLE_NORMAL;
  return adapter;
}

AvioAdapter::~AvioAdapter() {
  if (ctx_ != nullptr) {
    av_freep(static_cast<void*>(&ctx_->buffer));
    avio_context_free(&ctx_);
  }
}

// §8.7.2 rule 2. n == 0 → AVERROR_EOF; a throwing read → AVERROR(EIO), remembered for the status mapping.
int AvioAdapter::read(void* opaque, uint8_t* buf, int size) {
  auto* self = static_cast<AvioAdapter*>(opaque);
  const uint64_t file_size = self->file_->size();
  if (size <= 0 || self->pos_ >= file_size) {
    return AVERROR_EOF;
  }
  const auto n = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(size), file_size - self->pos_));
  try {
    const size_t got = self->file_->read(self->pos_, n, buf);
    if (got == 0) {
      return AVERROR_EOF;
    }
    self->pos_ += got;
    return static_cast<int>(got);
  } catch (const std::exception&) {
    self->io_failed_ = true;
    return AVERROR(EIO);
  }
}

// §8.7.2 rule 3: AVSEEK_SIZE → size; SEEK_SET/CUR/END clamped to [0, size].
int64_t AvioAdapter::seek(void* opaque, int64_t offset, int whence) {
  auto* self = static_cast<AvioAdapter*>(opaque);
  const auto size = static_cast<int64_t>(self->file_->size());
  if ((static_cast<unsigned>(whence) & static_cast<unsigned>(AVSEEK_SIZE)) != 0U) {
    return size;
  }
  int64_t base = 0;
  switch (static_cast<unsigned>(whence) & ~static_cast<unsigned>(AVSEEK_FORCE)) {
    case SEEK_SET:
      base = 0;
      break;
    case SEEK_CUR:
      base = static_cast<int64_t>(self->pos_);
      break;
    case SEEK_END:
      base = size;
      break;
    default:
      return AVERROR(EINVAL);
  }
  // Saturating add: offsets come from the input file and may be anything.
  int64_t target = 0;
  if (__builtin_add_overflow(base, offset, &target)) {
    target = offset < 0 ? 0 : size;
  }
  target = std::clamp<int64_t>(target, 0, size);
  self->pos_ = static_cast<uint64_t>(target);
  return target;
}

}  // namespace ovt::demux::detail
