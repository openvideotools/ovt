// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The out-of-line helpers of metrics_golden_suite.hpp (WI-3.590).

#include "metrics_golden_suite.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "test_support/corpus.h"

namespace ovt::test::metrics_suite {

std::filesystem::path corpus_file(std::string_view entry, std::string_view name) {
  std::filesystem::path primary = ovt::test::corpus_or_skip(entry);
  if (name.empty() || primary.filename() == name) {
    return primary;
  }
  std::filesystem::path p = primary.parent_path() / std::string(name);
  if (!std::filesystem::exists(p)) {
    SKIP("corpus entry " << entry << " has no " << name << " (the fetcher does not decode it to Y4M yet)");
  }
  return p;
}

nlohmann::json read_golden_gz(const std::string& name) {
  const std::filesystem::path path = std::filesystem::path(OVT_GOLDEN_METRICS_DIR) / name;
  REQUIRE(std::filesystem::exists(path));
  const std::string cmd = "gzip -dc '" + path.string() + "'";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"),  // NOLINT(cert-env33-c): fixed command
                                             pclose);
  REQUIRE(pipe != nullptr);
  std::string text;
  std::array<char, 65536> chunk{};
  size_t n = 0;
  while ((n = std::fread(chunk.data(), 1, chunk.size(), pipe.get())) > 0) {
    text.append(chunk.data(), n);
  }
  REQUIRE(!text.empty());
  return nlohmann::json::parse(text);
}

double golden_value(const nlohmann::json& v) {
  if (v.is_string()) {
    const auto s = v.get<std::string>();
    REQUIRE((s == "inf" || s == "-inf"));
    return s == "inf" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
  }
  return v.get<double>();
}

double round2(double v) {
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), "%0.2f", v);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  return std::stod(text.data());
}

}  // namespace ovt::test::metrics_suite
