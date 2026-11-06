// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Internal to the io module (chapter 7 §7.1.3): the two backends behind RandomAccessFile and the
// constants shared by them. Not installed, never included outside core/src/io/ and core/tests/io/.

#ifndef OVT_SRC_IO_IO_BACKENDS_HPP
#define OVT_SRC_IO_IO_BACKENDS_HPP

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::io::detail {

// size_t/off_t must be 64-bit everywhere (WI-1.100 notes; Linux builds add -D_FILE_OFFSET_BITS=64).
static_assert(sizeof(off_t) == 8, "off_t must be 64-bit: build with -D_FILE_OFFSET_BITS=64");
static_assert(sizeof(size_t) == 8, "OVT has no 32-bit targets");

// §7.1.4 rule 3: a single read() may not ask for more than 2^31 bytes.
inline constexpr size_t kMaxReadLen = size_t{1} << 31U;
// §7.1.3: files larger than 2^47 bytes cannot be mapped.
inline constexpr uint64_t kMaxMappedSize = uint64_t{1} << 47U;
// §7.1.3 pread cache: an LRU of 64 blocks of 1 MiB, each block buffer 64 KiB-aligned.
inline constexpr size_t kBlockSize = size_t{1} << 20U;
inline constexpr size_t kCacheBlocks = 64;
inline constexpr size_t kBlockAlign = size_t{64} << 10U;

// Maps the whole of `fd` (which stays owned by the caller). Returns nullptr and sets `err` to the errno
// value when mmap(2) fails; the caller then falls back to the pread backend (§7.1.3).
std::unique_ptr<RandomAccessFile> make_mmap_file(int fd, FileInfo info, int& err);

// Reads through pread(2). Closes `fd` in its destructor when `owns_fd` is true.
std::unique_ptr<RandomAccessFile> make_pread_file(int fd, bool owns_fd, FileInfo info);

}  // namespace ovt::io::detail

#endif  // OVT_SRC_IO_IO_BACKENDS_HPP
