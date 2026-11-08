// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// zstd compression and size-checked decompression of cache records (chapter 06 §6.7.1, WI-0.091 step 3).

#include "cache/zstd_io.hpp"

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/cxx/cache/error.hpp"
#include "ovt/status.h"

namespace ovt::cache {

namespace {

[[noreturn]] void throw_zstd(ovt_status code, std::string_view what, std::size_t rc) {
  std::string msg = "cache: zstd ";
  msg.append(what).append(": ").append(ZSTD_getErrorName(rc));
  throw CacheError(code, msg);
}

struct DCtxDeleter {
  void operator()(ZSTD_DCtx* ctx) const noexcept { ZSTD_freeDCtx(ctx); }
};

ZSTD_DCtx& thread_dctx() {
  thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> ctx(ZSTD_createDCtx());
  if (!ctx) {
    throw CacheError(OVT_E_NOMEM, "cache: ZSTD_createDCtx failed");
  }
  return *ctx;
}

}  // namespace

Compressor::Compressor() : ctx_(ZSTD_createCCtx()) {
  if (ctx_ == nullptr) {
    throw CacheError(OVT_E_NOMEM, "cache: ZSTD_createCCtx failed");
  }
}

Compressor::~Compressor() {
  ZSTD_freeCCtx(ctx_);
}

std::vector<std::uint8_t> Compressor::compress(std::span<const std::uint8_t> input, int level) {
  std::size_t rc = ZSTD_CCtx_reset(ctx_, ZSTD_reset_session_and_parameters);
  if (ZSTD_isError(rc) != 0U) {
    throw_zstd(OVT_E_INTERNAL, "reset", rc);
  }
  rc = ZSTD_CCtx_setParameter(ctx_, ZSTD_c_compressionLevel, level);
  if (ZSTD_isError(rc) != 0U) {
    throw_zstd(OVT_E_INTERNAL, "setParameter(level)", rc);
  }
  rc = ZSTD_CCtx_setParameter(ctx_, ZSTD_c_contentSizeFlag, 1);  // the reader relies on the header size
  if (ZSTD_isError(rc) != 0U) {
    throw_zstd(OVT_E_INTERNAL, "setParameter(contentSizeFlag)", rc);
  }
  rc = ZSTD_CCtx_setParameter(ctx_, ZSTD_c_checksumFlag, 1);
  if (ZSTD_isError(rc) != 0U) {
    throw_zstd(OVT_E_INTERNAL, "setParameter(checksumFlag)", rc);
  }
  std::vector<std::uint8_t> out(ZSTD_compressBound(input.size()));
  rc = ZSTD_compress2(ctx_, out.data(), out.size(), input.data(), input.size());
  if (ZSTD_isError(rc) != 0U) {
    throw_zstd(OVT_E_INTERNAL, "compress2", rc);
  }
  out.resize(rc);
  return out;
}

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> compressed, std::size_t max_size,
                                     std::optional<std::size_t> expected_size) {
  const unsigned long long content = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (content == ZSTD_CONTENTSIZE_ERROR) {
    throw CacheError(OVT_E_CORRUPT, "cache: not a zstd frame");
  }
  if (content == ZSTD_CONTENTSIZE_UNKNOWN) {
    throw CacheError(OVT_E_CORRUPT, "cache: zstd frame without a content size");
  }
  if (content > max_size) {
    throw CacheError(OVT_E_CORRUPT, "cache: zstd frame claims " + std::to_string(content) + " bytes, limit " +
                                        std::to_string(max_size));
  }
  if (expected_size && content != *expected_size) {
    throw CacheError(OVT_E_CORRUPT, "cache: zstd frame holds " + std::to_string(content) + " bytes, expected " +
                                        std::to_string(*expected_size));
  }
  // One frame per file: a trailing second frame (or garbage) is a malformed record.
  const std::size_t frame_size = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
  if (ZSTD_isError(frame_size) != 0U) {
    throw_zstd(OVT_E_CORRUPT, "frame", frame_size);
  }
  if (frame_size != compressed.size()) {
    throw CacheError(OVT_E_CORRUPT, "cache: trailing bytes after the zstd frame");
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(content));
  ZSTD_DCtx& ctx = thread_dctx();
  const std::size_t rc = ZSTD_decompressDCtx(&ctx, out.data(), out.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(rc) != 0U) {
    throw_zstd(OVT_E_CORRUPT, "decompress", rc);
  }
  if (rc != out.size()) {
    throw CacheError(OVT_E_CORRUPT, "cache: zstd frame decoded to " + std::to_string(rc) + " bytes, header says " +
                                        std::to_string(out.size()));
  }
  return out;
}

}  // namespace ovt::cache
