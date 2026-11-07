// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/golden_compare.h"

#include <fmt/format.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>  // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support/fb_normalize.h"
#include "test_support/golden.h"

namespace ovt::test {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxReportedDifferences = 20;

std::string six_decimals(double d) {
  std::string text = fmt::format("{:.6f}", d);
  return text == "-0.000000" ? "0.000000" : text;
}

// "$.frames[3].psnr_y" -> "frames[].psnr_y"
std::string pattern_of(const std::string& path) {
  std::string out;
  std::size_t i = 0;
  if (path.starts_with("$.")) {
    i = 2;
  } else if (path == "$") {
    i = 1;
  }
  while (i < path.size()) {
    if (path[i] == '[') {
      const std::size_t close = path.find(']', i);
      out += "[]";
      i = close == std::string::npos ? path.size() : close + 1;
    } else {
      out += path[i++];
    }
  }
  return out;
}

std::string brief(const json& value) {
  std::string text = value.dump();
  if (text.size() > 80) {
    text = text.substr(0, 77) + "...";
  }
  return text;
}

const char* type_name(const json& value) {
  if (value.is_object()) {
    return "object";
  }
  if (value.is_array()) {
    return "array";
  }
  if (value.is_string()) {
    return "string";
  }
  if (value.is_number()) {
    return "number";
  }
  if (value.is_boolean()) {
    return "boolean";
  }
  return "null";
}

bool numbers_equal(const json& golden, const json& actual, double tolerance) {
  if (golden.is_number_integer() && actual.is_number_integer()) {
    // nlohmann's operator== casts a negative signed value to unsigned (-1 == 2^64-1), so compare by hand.
    if (golden.is_number_unsigned() != actual.is_number_unsigned()) {
      const json& negative_candidate = golden.is_number_unsigned() ? actual : golden;
      const json& unsigned_value = golden.is_number_unsigned() ? golden : actual;
      const auto s = negative_candidate.get<int64_t>();
      return s >= 0 && static_cast<uint64_t>(s) == unsigned_value.get<uint64_t>();
    }
    return golden == actual;
  }
  const auto g = golden.get<double>();
  const auto a = actual.get<double>();
  if (tolerance > 0.0) {
    return std::fabs(g - a) <= tolerance;
  }
  return six_decimals(g) == six_decimals(a);
}

class Comparer {
 public:
  explicit Comparer(const CompareOptions& options) : options_(options) {}

  void run(const json& golden, const json& actual, const std::string& path) {
    if (golden.is_number() && actual.is_number()) {
      const auto it = options_.tolerances.find(pattern_of(path));
      const double tolerance = it != options_.tolerances.end() ? it->second : options_.default_tolerance;
      if (!numbers_equal(golden, actual, tolerance)) {
        add(path, "golden " + brief(golden) + ", OVT " + brief(actual) +
                      (tolerance > 0.0 ? fmt::format(" (tolerance {})", tolerance) : std::string()));
      }
      return;
    }
    if (golden.type() != actual.type() && !(golden.is_boolean() && actual.is_boolean())) {
      add(path, std::string("type mismatch: golden ") + type_name(golden) + ", OVT " + type_name(actual));
      return;
    }
    if (golden.is_object()) {
      for (const auto& [key, value] : golden.items()) {
        std::string child = path;
        child += '.';
        child += key;
        const auto it = actual.find(key);
        if (it == actual.end()) {
          add(child, "missing in OVT output (golden " + brief(value) + ")");
        } else {
          run(value, *it, child);
        }
      }
    } else if (golden.is_array()) {
      if (golden.size() != actual.size()) {
        add(path, fmt::format("array length: golden {}, OVT {}", golden.size(), actual.size()));
        return;
      }
      for (std::size_t i = 0; i < golden.size(); ++i) {
        std::string child = path;
        child += '[';
        child += std::to_string(i);
        child += ']';
        run(golden[i], actual[i], child);
      }
    } else if (golden != actual) {
      add(path, "golden " + brief(golden) + ", OVT " + brief(actual));
    }
  }

  std::vector<Difference> take() { return std::move(differences_); }

 private:
  void add(const std::string& path, std::string message) { differences_.push_back({path, std::move(message)}); }

  const CompareOptions& options_;
  std::vector<Difference> differences_;
};

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

struct Edit {
  char op;        // ' ', '-', '+'
  std::size_t a;  // line index in expected (for ' ' and '-')
  std::size_t b;  // line index in actual (for ' ' and '+')
};

// Number of leading lines both sides share, then of trailing lines they share after that.
struct Trim {
  std::size_t prefix = 0;
  std::size_t suffix = 0;
};

Trim common_ends(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  Trim trim;
  while (trim.prefix < a.size() && trim.prefix < b.size() && a[trim.prefix] == b[trim.prefix]) {
    ++trim.prefix;
  }
  const std::size_t room = std::min(a.size(), b.size()) - trim.prefix;
  while (trim.suffix < room && a[a.size() - 1 - trim.suffix] == b[b.size() - 1 - trim.suffix]) {
    ++trim.suffix;
  }
  return trim;
}

// Edits for a[from_a, to_a) against b[from_b, to_b) by a longest-common-subsequence table. Deletions come before
// insertions, as diff(1) prints them.
void lcs_edits(const std::vector<std::string>& a, const std::vector<std::string>& b, std::size_t offset, std::size_t n,
               std::size_t m, std::vector<Edit>& script) {
  const std::size_t width = m + 1;
  std::vector<uint32_t> lcs((n + 1) * width, 0);
  const auto cell = [&](std::size_t i, std::size_t j) -> uint32_t& { return lcs[(i * width) + j]; };
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      const bool same = a[offset + i] == b[offset + j];
      cell(i, j) = same ? cell(i + 1, j + 1) + 1 : std::max(cell(i + 1, j), cell(i, j + 1));
    }
  }
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && a[offset + i] == b[offset + j]) {
      script.push_back({' ', offset + i, offset + j});
      ++i;
      ++j;
    } else if (i < n && (j == m || cell(i + 1, j) >= cell(i, j + 1))) {
      script.push_back({'-', offset + i, 0});
      ++i;
    } else {
      script.push_back({'+', 0, offset + j});
      ++j;
    }
  }
}

// Edit script from common prefix/suffix trimming plus an LCS table on the middle (goldens are line-oriented JSON
// of a few thousand lines at most; a huge middle degrades to one replace hunk).
std::vector<Edit> edit_script(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  const Trim trim = common_ends(a, b);
  const std::size_t n = a.size() - trim.prefix - trim.suffix;
  const std::size_t m = b.size() - trim.prefix - trim.suffix;
  std::vector<Edit> script;
  script.reserve(a.size() + b.size());
  for (std::size_t i = 0; i < trim.prefix; ++i) {
    script.push_back({' ', i, i});
  }
  constexpr std::size_t kMaxCells = static_cast<std::size_t>(16) * 1024 * 1024;
  if (n * m > kMaxCells) {
    for (std::size_t i = 0; i < n; ++i) {
      script.push_back({'-', trim.prefix + i, 0});
    }
    for (std::size_t j = 0; j < m; ++j) {
      script.push_back({'+', 0, trim.prefix + j});
    }
  } else {
    lcs_edits(a, b, trim.prefix, n, m, script);
  }
  for (std::size_t k = 0; k < trim.suffix; ++k) {
    script.push_back({' ', a.size() - trim.suffix + k, b.size() - trim.suffix + k});
  }
  return script;
}

constexpr std::size_t kDiffContext = 3;

// End (exclusive) of the hunk that starts at the change `first_change`: it extends past the last change that is
// followed by fewer than 2 * context unchanged lines.
std::size_t hunk_end(const std::vector<Edit>& script, std::size_t first_change) {
  std::size_t last = first_change;
  for (std::size_t scan = first_change; scan < script.size(); ++scan) {
    if (script[scan].op != ' ') {
      last = scan;
    } else if (scan - last > 2 * kDiffContext) {
      break;
    }
  }
  return std::min(script.size(), last + kDiffContext + 1);
}

// One "@@ -a,n +b,m @@" hunk with its lines for script[first, end).
std::string format_hunk(const std::vector<Edit>& script, const std::vector<std::string>& a,
                        const std::vector<std::string>& b, std::size_t first, std::size_t end) {
  std::optional<std::size_t> a_start;
  std::optional<std::size_t> b_start;
  std::size_t a_count = 0;
  std::size_t b_count = 0;
  std::string body;
  for (std::size_t i = first; i < end; ++i) {
    const Edit& e = script[i];
    if (e.op != '+') {
      a_start = a_start.value_or(e.a + 1);
      ++a_count;
    }
    if (e.op != '-') {
      b_start = b_start.value_or(e.b + 1);
      ++b_count;
    }
    body += e.op;
    body += e.op == '+' ? b[e.b] : a[e.a];
    body += '\n';
  }
  // A side without any line (pure insertion or deletion with no context) starts at 0.
  std::string out = fmt::format("@@ -{},{} +{},{} @@\n", a_start.value_or(0), a_count, b_start.value_or(0), b_count);
  out += body;
  return out;
}

bool read_text(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return true;
}

std::optional<std::string> array_rows_have(Family family, const json& document,
                                           std::initializer_list<const char*> keys) {
  if (!document.is_array()) {
    return std::string(family_name(family)) + " must be a JSON array";
  }
  for (std::size_t i = 0; i < document.size(); ++i) {
    for (const char* key : keys) {
      if (!document[i].is_object() || !document[i].contains(key)) {
        return fmt::format("{}[{}] has no \"{}\"", family_name(family), i, key);
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> check_family_shape(Family family, const json& document) {
  switch (family) {
    case Family::kSyntaxElements:
      return array_rows_have(family, document, {"unit", "name", "bit_offset", "bit_length", "value"});
    case Family::kBoxTree:
      return array_rows_have(family, document, {"path", "offset", "size", "fields"});
    case Family::kSampleTable:
      return array_rows_have(family, document, {"i", "dts", "cts", "size", "offset", "sync"});
    case Family::kHeaderStruct:
      return document.is_object() ? std::nullopt : std::optional<std::string>("header-struct must be a JSON object");
    case Family::kBlockGrid: {
      const bool ok = document.is_object() && document.contains("w4") && document.contains("h4") &&
                      document.contains("fields") && document["fields"].is_object();
      return ok ? std::nullopt : std::optional<std::string>("block-grid must be an object with w4, h4 and fields");
    }
    case Family::kScores: {
      const bool ok = document.is_object() && document.contains("frames") && document["frames"].is_array() &&
                      document.contains("pooled") && document["pooled"].is_object();
      return ok ? std::nullopt
                : std::optional<std::string>("scores must be an object with frames (array) and pooled (object)");
    }
  }
  return std::nullopt;
}

std::vector<Difference> compare_canonical(const json& golden, const json& actual, const CompareOptions& options) {
  Comparer comparer(options);
  comparer.run(golden, actual, "$");
  return comparer.take();
}

CompareOptions load_compare_options(const std::filesystem::path& file) {
  std::string text;
  if (!read_text(file, text)) {
    throw std::invalid_argument("cannot read tolerance file " + file.string());
  }
  const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) {
    throw std::invalid_argument("tolerance file " + file.string() + " is not a JSON object");
  }
  CompareOptions options;
  for (const auto& [key, value] : parsed.items()) {
    if (!value.is_number() || value.get<double>() < 0.0) {
      throw std::invalid_argument("tolerance file " + file.string() + ": '" + key + "' is not a number >= 0");
    }
    if (key == "default") {
      options.default_tolerance = value.get<double>();
    } else {
      options.tolerances[key] = value.get<double>();
    }
  }
  return options;
}

std::string unified_diff(std::string_view expected, std::string_view actual, std::string_view expected_label,
                         std::string_view actual_label) {
  if (expected == actual) {
    return {};
  }
  const auto a = split_lines(expected);
  const auto b = split_lines(actual);
  const auto script = edit_script(a, b);

  std::string out = fmt::format("--- {}\n+++ {}\n", expected_label, actual_label);
  std::size_t k = 0;
  while (k < script.size()) {
    if (script[k].op == ' ') {
      ++k;
      continue;
    }
    const std::size_t first = k >= kDiffContext ? k - kDiffContext : 0;
    const std::size_t end = hunk_end(script, k);
    out += format_hunk(script, a, b, first, end);
    k = end;
  }
  return out;
}

JsonGoldenResult compare_json_golden(const std::filesystem::path& golden, const json& actual,
                                     const CompareOptions& options) {
  JsonGoldenResult result;
  const std::string actual_text = normalize_json(actual.dump());
  if (update_golden()) {
    const GoldenResult written =
        compare_bytes_or_update(golden, {reinterpret_cast<const uint8_t*>(actual_text.data()), actual_text.size()});
    result.ok = written.outcome == GoldenOutcome::kUpdated;
    result.updated = result.ok;
    result.message = written.message;
    return result;
  }
  std::string golden_text;
  if (!read_text(golden, golden_text)) {
    result.message = "golden file missing: " + golden.string() + " (run with OVT_UPDATE_GOLDEN=1 to create it)";
    return result;
  }
  const json golden_json = json::parse(golden_text, nullptr, /*allow_exceptions=*/false);
  if (golden_json.is_discarded()) {
    result.message = "golden file is not valid JSON: " + golden.string();
    return result;
  }
  const auto differences = compare_canonical(golden_json, actual, options);
  if (differences.empty()) {
    result.ok = true;
    return result;
  }
  std::string message = fmt::format("golden mismatch: {}: {} difference(s)\n", golden.string(), differences.size());
  for (std::size_t i = 0; i < differences.size() && i < kMaxReportedDifferences; ++i) {
    message += fmt::format("  {}: {}\n", differences[i].path, differences[i].message);
  }
  if (differences.size() > kMaxReportedDifferences) {
    message += fmt::format("  ... and {} more\n", differences.size() - kMaxReportedDifferences);
  }
  message += unified_diff(golden_text, actual_text, golden.string(), "OVT output (normalised)");
  message += "rerun with OVT_UPDATE_GOLDEN=1 if the change is intended\n";
  result.message = std::move(message);
  return result;
}

bool compare_json_or_update(const std::filesystem::path& golden, const json& actual, const CompareOptions& options) {
  const JsonGoldenResult result = compare_json_golden(golden, actual, options);
  if (result.updated) {
    WARN(result.message);
  }
  if (!result.ok) {
    FAIL_CHECK(result.message);
  }
  return result.ok;
}

}  // namespace ovt::test
