// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Golden-file comparison (chapter 14 §14.2.2, WI-0.550). Goldens live under tests/golden/ (or a module's
// tests/golden/) and are compared as bytes after normalisation (§14.1.3). With OVT_UPDATE_GOLDEN=1 the
// golden file is rewritten instead and the comparison passes. WI-0.556 adds unified diffs for text
// goldens and the ovt_add_golden_test() CMake function on top of this.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace ovt::test {

enum class GoldenOutcome {
  kMatched,   ///< the golden file exists and equals the actual bytes
  kUpdated,   ///< update mode: the golden file was (re)written with the actual bytes
  kMissing,   ///< compare mode: the golden file does not exist
  kMismatch,  ///< compare mode: the golden file differs from the actual bytes
};

struct GoldenResult {
  GoldenOutcome outcome = GoldenOutcome::kMissing;
  std::size_t first_difference = 0;  ///< byte offset of the first difference (kMismatch)
  std::size_t golden_size = 0;
  std::size_t actual_size = 0;
  std::string message;  ///< human-readable summary for test output
};

/// True when golden files are rewritten instead of compared: OVT_UPDATE_GOLDEN=1, unless overridden by
/// set_update_golden().
bool update_golden();

/// Forces update mode on or off for the rest of the process (the golden runner's main and the tests of
/// this library use it).
void set_update_golden(bool enabled);

/// Compares @p actual with the file @p golden, or rewrites the file in update mode. Creates parent
/// directories when writing. Does not touch Catch2.
GoldenResult compare_bytes_or_update(const std::filesystem::path& golden, std::span<const uint8_t> actual);

/// compare_bytes_or_update() as a Catch2 check: records a failed check with the result message unless
/// the outcome is kMatched or kUpdated. Returns true on kMatched or kUpdated.
bool compare_or_update(const std::filesystem::path& golden, std::span<const uint8_t> actual);

/// Text overload: compares the UTF-8 bytes of @p actual.
bool compare_or_update(const std::filesystem::path& golden, const std::string& actual);

}  // namespace ovt::test
