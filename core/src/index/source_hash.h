// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// StreamIndex.source_hash (chapter 6 §6.2.8, chapter 7 §7.4.2 S10; WI-1.121): SHA-256 over the first 1 MiB of the
// file, its size and its modification time. Module-internal, not installed.

#ifndef OVT_INDEX_SOURCE_HASH_H
#define OVT_INDEX_SOURCE_HASH_H

#include <array>
#include <cstdint>
#include <span>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::index {

/// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  void update(std::span<const uint8_t> data);
  /// Pads and returns the digest. The object must not be used afterwards.
  [[nodiscard]] std::array<uint8_t, 32> finish();

 private:
  void compress(std::span<const uint8_t, 64> block);

  std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<uint8_t, 64> block_{};
  size_t fill_ = 0;
  uint64_t bytes_ = 0;
};

/// SHA-256( file[0 : min(size, 1 MiB)] ∥ LE64(size) ∥ LE64(mtime_ns) ). Throws io::IoError when a read fails.
[[nodiscard]] std::array<uint8_t, 32> source_hash(const io::RandomAccessFile& file);

}  // namespace ovt::index

#endif  // OVT_INDEX_SOURCE_HASH_H
