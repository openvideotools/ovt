// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// zstd framing of cache records (chapter 06 §6.7.1, WI-0.091 step 3): one zstd frame per file whose header
// carries the decompressed size, so a reader checks the size before it allocates. Internal header.

#ifndef OVT_CORE_CACHE_ZSTD_IO_HPP
#define OVT_CORE_CACHE_ZSTD_IO_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct ZSTD_CCtx_s;

namespace ovt::cache {

/// Compression levels of §6.7.1.
inline constexpr int kZstdLevelFrames = 3;  ///< frames/*.fa.zst, container.fb.zst, reports
inline constexpr int kZstdLevelPlanes = 1;  ///< planes/**.pl.zst, maps/**.pl.zst

/// Owns one ZSTD_CCtx and reuses it across calls (WI-0.091 step 3). Not thread-safe; the Entry that owns it
/// serialises its writes.
class Compressor {
 public:
  Compressor();
  ~Compressor();
  Compressor(const Compressor&) = delete;
  Compressor& operator=(const Compressor&) = delete;
  Compressor(Compressor&&) = delete;
  Compressor& operator=(Compressor&&) = delete;

  /// One zstd frame with the content size in its header. Throws CacheError(OVT_E_INTERNAL) on a zstd error
  /// and std::bad_alloc when the output buffer cannot be allocated.
  [[nodiscard]] std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input, int level);

 private:
  ZSTD_CCtx_s* ctx_;
};

/// Decompresses one zstd frame. Throws CacheError(OVT_E_CORRUPT) when @p compressed is not a single complete
/// frame, when its header does not state the content size, when that size exceeds @p max_size or differs from
/// @p expected_size (if given), or when the decoded length differs from the header. Uses a thread-local
/// ZSTD_DCtx; safe to call concurrently.
[[nodiscard]] std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> compressed, std::size_t max_size,
                                                   std::optional<std::size_t> expected_size = std::nullopt);

}  // namespace ovt::cache

#endif  // OVT_CORE_CACHE_ZSTD_IO_HPP
