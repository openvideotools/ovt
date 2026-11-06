// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/golden_compare.h"

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>

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
  std::size_t i = path.rfind("$.", 0) == 0 ? 2 : (path == "$" ? 1 : 0);
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
        const std::string child = path + "." + key;
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
        run(golden[i], actual[i], path + "[" + std::to_string(i) + "]");
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

// Edit script from common prefix/suffix trimming plus an LCS table on the middle (goldens are line-oriented JSON
// of a few thousand lines at most; a huge middle degrades to one replace hunk).
std::vector<Edit> edit_script(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  std::size_t prefix = 0;
  while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) {
    ++prefix;
  }
  std::size_t suffix = 0;
  while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
         a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
    ++suffix;
  }
  const std::size_t n = a.size() - prefix - suffix;
  const std::size_t m = b.size() - prefix - suffix;
  std::vector<Edit> script;
  for (std::size_t i = 0; i < prefix; ++i) {
    script.push_back({' ', i, i});
  }
  constexpr std::size_t kMaxCells = 16U * 1024U * 1024U;
  if (n * m > kMaxCells) {
    for (std::size_t i = 0; i < n; ++i) {
      script.push_back({'-', prefix + i, 0});
    }
    for (std::size_t j = 0; j < m; ++j) {
      script.push_back({'+', 0, prefix + j});
    }
  } else {
    std::vector<uint32_t> lcs((n + 1) * (m + 1), 0);
    const auto cell = [&](std::size_t i, std::size_t j) -> uint32_t& { return lcs[i * (m + 1) + j]; };
    for (std::size_t i = n; i-- > 0;) {
      for (std::size_t j = m; j-- > 0;) {
        cell(i, j) = a[prefix + i] == b[prefix + j] ? cell(i + 1, j + 1) + 1 : std::max(cell(i + 1, j), cell(i, j + 1));
      }
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
      if (i < n && j < m && a[prefix + i] == b[prefix + j]) {
        script.push_back({' ', prefix + i, prefix + j});
        ++i;
        ++j;
      } else if (i < n && (j == m || cell(i + 1, j) >= cell(i, j + 1))) {
        script.push_back({'-', prefix + i, 0});  // deletions before insertions, as diff(1) prints them
        ++i;
      } else {
        script.push_back({'+', 0, prefix + j});
        ++j;
      }
    }
  }
  for (std::size_t k = 0; k < suffix; ++k) {
    script.push_back({' ', a.size() - suffix + k, b.size() - suffix + k});
  }
  return script;
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

}  // namespace

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
  constexpr std::size_t kContext = 3;

  std::string out = fmt::format("--- {}\n+++ {}\n", expected_label, actual_label);
  std::size_t k = 0;
  while (k < script.size()) {
    if (script[k].op == ' ') {
      ++k;
      continue;
    }
    // Hunk: from `kContext` lines before the first change to `kContext` lines after the last change, merging
    // changes that are closer than 2*kContext lines.
    std::size_t first = k >= kContext ? k - kContext : 0;
    while (first < k && script[first].op != ' ') {
      ++first;  // cannot happen (k is the first change after context) but keeps the bounds honest
    }
    first = k >= kContext ? k - kContext : 0;
    std::size_t last = k;  // last change index
    std::size_t scan = k;
    while (scan < script.size()) {
      if (script[scan].op != ' ') {
        last = scan;
      } else if (scan - last > 2 * kContext) {
        break;
      }
      ++scan;
    }
    const std::size_t end = std::min(script.size(), last + kContext + 1);
    std::size_t a_start = 0;
    std::size_t b_start = 0;
    std::size_t a_count = 0;
    std::size_t b_count = 0;
    bool seen_a = false;
    bool seen_b = false;
    std::string body;
    for (std::size_t i = first; i < end; ++i) {
      const Edit& e = script[i];
      if (e.op != '+') {
        if (!seen_a) {
          a_start = e.a + 1;
          seen_a = true;
        }
        ++a_count;
      }
      if (e.op != '-') {
        if (!seen_b) {
          b_start = e.b + 1;
          seen_b = true;
        }
        ++b_count;
      }
      body += e.op;
      body += e.op == '+' ? b[e.b] : a[e.a];
      body += '\n';
    }
    if (!seen_a) {  // pure insertion hunk with no context: the start is the line before the insertion
      a_start = 0;
    }
    if (!seen_b) {
      b_start = 0;
    }
    out += fmt::format("@@ -{},{} +{},{} @@\n", a_start, a_count, b_start, b_count);
    out += body;
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
