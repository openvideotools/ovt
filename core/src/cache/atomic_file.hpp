// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Atomic file replacement for the cache (chapter 06 §6.7.2, WI-0.091 step 2): write `<path>.tmp`, fsync,
// rename over `<path>`. rename(2) over an existing file is atomic on POSIX; the temporary lives next to the
// target, so the two are always on one file system (Android app cache dir included). Internal header.

#ifndef OVT_CORE_CACHE_ATOMIC_FILE_HPP
#define OVT_CORE_CACHE_ATOMIC_FILE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ovt::cache {

/// Suffix of the temporary file: `meta.json` is written as `meta.json.tmp` (§6.7.2).
inline constexpr const char* kTmpSuffix = ".tmp";

/// Replaces @p path with @p bytes atomically (mode 0600, parent directory fsync-ed after the rename). Throws
/// CacheError(OVT_E_IO). With the simulated crash armed (below) it stops after the fsync of the temporary,
/// leaving `<path>.tmp` behind and @p path untouched, and throws CacheError(OVT_E_IO).
void write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

/// Reads the whole regular file at @p path. Throws CacheError(OVT_E_IO) when it cannot be opened or read
/// and CacheError(OVT_E_CORRUPT) when it is larger than @p max_size.
[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path, std::size_t max_size);

/// Deletes every `*.tmp` file below @p dir (recursively): the leftovers of a crash between write and rename.
/// Returns how many were removed. Errors on single files are logged and skipped.
std::size_t remove_stale_tmp_files(const std::filesystem::path& dir);

/// Test hook (WI-0.091 step 6, acceptance test 2): while armed, write_file_atomic() behaves like a process
/// killed between the fsync and the rename. Process-wide; tests disarm it again.
void set_simulated_crash_before_rename(bool armed) noexcept;
[[nodiscard]] bool simulated_crash_before_rename() noexcept;

}  // namespace ovt::cache

#endif  // OVT_CORE_CACHE_ATOMIC_FILE_HPP
