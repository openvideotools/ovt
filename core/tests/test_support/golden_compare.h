// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tolerant comparison of canonical JSON against goldens (chapter 14 §14.4.0 step 3, WI-0.556).
//
// Semantics: the golden is the ORACLE and a lower bound of what OVT must report. Every field of the golden
// must exist in OVT's output and be equal; fields only OVT reports are ignored. Arrays must have the same
// length and are compared element by element. Numbers are compared as numbers, never as text: 1, 1.0 and 1e0 are
// equal, and two numbers of which at least one is a double are equal when they print the same with "%.6f" (the
// precision of the Python normalisers) or, with a tolerance entry, when they differ by at most the tolerance.
// SPEC-ASSUMPTION(#111): acceptance test 1 of WI-0.556 is read this way; see the issue.

#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::test {

struct CompareOptions {
  /// Absolute tolerance per path pattern. A pattern is a path without the leading "$." in which array indices
  /// are written "[]": "frames[].psnr_y" covers $.frames[0].psnr_y, $.frames[1].psnr_y, ... No entry means 0.
  std::map<std::string, double> tolerances;
  /// Used for numbers without a tolerances entry. 0 = equal at "%.6f" precision.
  double default_tolerance = 0.0;
};

struct Difference {
  std::string path;     ///< "$.frames[3].psnr_y"
  std::string message;  ///< what differs, golden value first
};

/// Differences of @p actual against @p golden, in document order (empty = pass). Missing fields are reported
/// with the path of the field.
std::vector<Difference> compare_canonical(const nlohmann::json& golden, const nlohmann::json& actual,
                                          const CompareOptions& options = {});

/// Reads a tolerance map: a JSON object {"<pattern>": <tolerance>, ...}; the key "default" sets default_tolerance.
/// Throws std::invalid_argument on a malformed file.
CompareOptions load_compare_options(const std::filesystem::path& file);

/// Unified diff (3 lines of context) of two texts, "" when they are equal.
std::string unified_diff(std::string_view expected, std::string_view actual, std::string_view expected_label = "golden",
                         std::string_view actual_label = "actual");

struct JsonGoldenResult {
  bool ok = false;
  bool updated = false;  ///< update mode: the golden file was rewritten
  std::string message;   ///< empty on a plain match; otherwise differences and a unified diff
};

/// Compares the canonical JSON @p actual with the golden file @p golden. The golden file stores the
/// normalize_json() text of the canonical JSON. In update mode (OVT_UPDATE_GOLDEN=1) the file is rewritten with that
/// text and the result is ok. Does not touch Catch2.
JsonGoldenResult compare_json_golden(const std::filesystem::path& golden, const nlohmann::json& actual,
                                     const CompareOptions& options = {});

/// compare_json_golden() as a Catch2 check; records a failed check with the message. Returns ok.
bool compare_json_or_update(const std::filesystem::path& golden, const nlohmann::json& actual,
                            const CompareOptions& options = {});

}  // namespace ovt::test
