// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The out-of-line helpers of metrics_golden_suite.hpp (WI-3.590).

#include "metrics_golden_suite.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

#include "test_support/corpus.h"

namespace ovt::test::metrics_suite {

std::filesystem::path corpus_file(std::string_view entry, std::string_view name) {
  std::filesystem::path primary = ovt::test::corpus_or_skip(entry);
  if (name.empty() || primary.filename() == name) {
    return primary;
  }
  std::filesystem::path p = primary.parent_path() / std::string(name);
  if (!std::filesystem::exists(p)) {
    SKIP("corpus entry " << entry << " has no " << name);
  }
  return p;
}

DistY4m::DistY4m(const GoldenPair& pair) {
  const std::filesystem::path primary = ovt::test::corpus_or_skip(pair.dist_entry);
  if (pair.dist_file.empty() || primary.filename() == pair.dist_file) {
    path_ = primary;
    return;
  }
  const std::filesystem::path dir = primary.parent_path();
  if (std::filesystem::exists(dir / std::string(pair.dist_file)) || pair.dist_source.empty()) {
    path_ = corpus_file(pair.dist_entry, pair.dist_file);
    return;
  }
  const std::filesystem::path source = dir / std::string(pair.dist_source);
  REQUIRE(std::filesystem::exists(source));
  // Unique per process: ctest may run several of these cases at once.
  path_ = std::filesystem::temp_directory_path() /
          ("ovt_metrics_" + std::string(pair.id) + "_" + std::to_string(::getpid()) + "_dist.y4m");
  temporary_ = true;
  // The command of gen_metrics.py to_y4m() that made the goldens: first `frames` pictures, 8-bit 4:2:0.
  const std::string cmd = "ffmpeg -hide_banner -nostdin -v error -y -i '" + source.string() + "' -frames:v " +
                          std::to_string(pair.frames) + " -pix_fmt yuv420p -strict -1 -f yuv4mpegpipe '" +
                          path_.string() + "'";
  const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe): fixed command, test only
  if (rc != 0 || !std::filesystem::exists(path_)) {
    if (ovt::test::require_corpora()) {
      FAIL("cannot decode " << source << " with the ffmpeg CLI (exit status " << rc << ")");
    }
    SKIP("cannot decode " << source << " with the ffmpeg CLI (exit status " << rc << ")");
  }
}

DistY4m::~DistY4m() {
  if (temporary_) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
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
