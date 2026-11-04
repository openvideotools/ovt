// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/golden.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

#include "test_support/env.h"

namespace ovt::test {
namespace {

std::optional<bool>& update_override() {
  static std::optional<bool> value;
  return value;
}

std::optional<std::vector<uint8_t>> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::vector<uint8_t> bytes;
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) {
    return std::nullopt;
  }
  bytes.resize(static_cast<std::size_t>(size));
  in.seekg(0, std::ios::beg);
  if (!bytes.empty()) {
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!in) {
    return std::nullopt;
  }
  return bytes;
}

bool write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  if (!bytes.empty()) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  return static_cast<bool>(out);
}

}  // namespace

bool update_golden() {
  if (update_override()) {
    return *update_override();
  }
  return env_flag("OVT_UPDATE_GOLDEN");
}

void set_update_golden(bool enabled) { update_override() = enabled; }

GoldenResult compare_bytes_or_update(const std::filesystem::path& golden, std::span<const uint8_t> actual) {
  GoldenResult result;
  result.actual_size = actual.size();
  if (update_golden()) {
    if (!write_file(golden, actual)) {
      result.outcome = GoldenOutcome::kMismatch;
      result.message = "cannot write golden file " + golden.string();
      return result;
    }
    result.outcome = GoldenOutcome::kUpdated;
    result.golden_size = actual.size();
    result.message = "golden file updated: " + golden.string();
    return result;
  }

  const auto expected = read_file(golden);
  if (!expected) {
    result.outcome = GoldenOutcome::kMissing;
    result.message = "golden file missing: " + golden.string() + " (run with OVT_UPDATE_GOLDEN=1 to create it)";
    return result;
  }
  result.golden_size = expected->size();
  const auto mismatch = std::mismatch(expected->begin(), expected->end(), actual.begin(), actual.end());
  if (mismatch.first == expected->end() && mismatch.second == actual.end()) {
    result.outcome = GoldenOutcome::kMatched;
    return result;
  }
  result.outcome = GoldenOutcome::kMismatch;
  result.first_difference = static_cast<std::size_t>(std::distance(expected->begin(), mismatch.first));
  result.message = "golden mismatch: " + golden.string() + ": first difference at byte " +
                   std::to_string(result.first_difference) + " (golden " + std::to_string(result.golden_size) +
                   " bytes, actual " + std::to_string(result.actual_size) +
                   " bytes); rerun with OVT_UPDATE_GOLDEN=1 if the change is intended";
  return result;
}

bool compare_or_update(const std::filesystem::path& golden, std::span<const uint8_t> actual) {
  const GoldenResult result = compare_bytes_or_update(golden, actual);
  const bool ok = result.outcome == GoldenOutcome::kMatched || result.outcome == GoldenOutcome::kUpdated;
  if (result.outcome == GoldenOutcome::kUpdated) {
    WARN(result.message);
  }
  if (!ok) {
    FAIL_CHECK(result.message);
  }
  return ok;
}

bool compare_or_update(const std::filesystem::path& golden, const std::string& actual) {
  const auto* data = reinterpret_cast<const uint8_t*>(actual.data());
  return compare_or_update(golden, std::span<const uint8_t>(data, actual.size()));
}

}  // namespace ovt::test
