// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.556: tolerant comparison of canonical JSON against goldens (chapter 14 §14.4.0 step 3).
//
// Acceptance test 1: a field only the oracle has -> reported with its path; a field only OVT has -> ignored.
// Acceptance test 2: numerically equal numbers formatted differently compare equal.
// WI-0.041 tests 1-2 (moved here): a mismatch prints a unified diff; the scratch directories below are unique per test
// case, so `ctest -j8` cannot collide.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>  // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>

#include "test_support/fb_normalize.h"
#include "test_support/golden.h"
#include "test_support/golden_compare.h"

namespace {

using nlohmann::json;
using ovt::test::compare_canonical;
using ovt::test::CompareOptions;

class UpdateModeGuard {
 public:
  explicit UpdateModeGuard(bool enabled) : saved_(ovt::test::update_golden()) { ovt::test::set_update_golden(enabled); }
  ~UpdateModeGuard() { ovt::test::set_update_golden(saved_); }
  UpdateModeGuard(const UpdateModeGuard&) = delete;
  UpdateModeGuard& operator=(const UpdateModeGuard&) = delete;

 private:
  bool saved_;
};

// A directory below the test binary directory named after the test case: no two test cases share a file.
std::filesystem::path scratch(const std::string& name) {
  auto dir = std::filesystem::path(OVT_TEST_BINARY_DIR) / "scratch_golden_compare" / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

TEST_CASE("golden_compare: a field only OVT reports is ignored, a field only the oracle has fails by path",
          "[test_support][golden][fast]") {
  const json golden = json::parse(R"({"fields":{"timescale":1000},"path":"moov/mvhd"})");
  const json with_extra = json::parse(R"({"fields":{"timescale":1000,"version":0},"path":"moov/mvhd","hdr":8})");
  CHECK(compare_canonical(golden, with_extra).empty());

  const json missing = json::parse(R"({"fields":{"version":0},"path":"moov/mvhd"})");
  const auto differences = compare_canonical(golden, missing);
  REQUIRE(differences.size() == 1);
  CHECK(differences[0].path == "$.fields.timescale");
  CHECK(differences[0].message.find("missing in OVT output") != std::string::npos);
}

TEST_CASE("golden_compare: arrays are compared element by element with the index in the path",
          "[test_support][golden][fast]") {
  const json golden = json::parse(R"([{"i":0,"size":10},{"i":1,"size":20}])");
  const auto wrong_value = compare_canonical(golden, json::parse(R"([{"i":0,"size":10},{"i":1,"size":21}])"));
  REQUIRE(wrong_value.size() == 1);
  CHECK(wrong_value[0].path == "$[1].size");

  const auto shorter = compare_canonical(golden, json::parse(R"([{"i":0,"size":10}])"));
  REQUIRE(shorter.size() == 1);
  CHECK(shorter[0].path == "$");
  CHECK(shorter[0].message.find("array length") != std::string::npos);

  const auto wrong_type = compare_canonical(json::parse(R"({"a":"1"})"), json::parse(R"({"a":1})"));
  REQUIRE(wrong_type.size() == 1);
  CHECK(wrong_type[0].message.find("type mismatch") != std::string::npos);
}

TEST_CASE("golden_compare: numbers are compared as numbers, not as text", "[test_support][golden][fast]") {
  // Same value, different spellings, tolerance 0.
  CHECK(compare_canonical(json::parse("1"), json::parse("1.0")).empty());
  CHECK(compare_canonical(json::parse("1.50"), json::parse("1.5e0")).empty());
  CHECK(compare_canonical(json::parse("0.1"), json::parse("0.10000000000000001")).empty());
  CHECK(compare_canonical(json::parse("-0.0"), json::parse("0")).empty());
  CHECK(compare_canonical(json::parse("18446744073709551615"), json::parse("18446744073709551615")).empty());
  // Doubles equal at the %.6f precision of the Python normalisers, different beyond it.
  CHECK(compare_canonical(json::parse("41.1234564"), json::parse("41.1234559")).empty());
  CHECK_FALSE(compare_canonical(json::parse("41.123456"), json::parse("41.123457")).empty());
  // Integers are exact.
  CHECK_FALSE(compare_canonical(json::parse("7"), json::parse("8")).empty());
  CHECK_FALSE(compare_canonical(json::parse("-1"), json::parse("18446744073709551615")).empty());
}

TEST_CASE("golden_compare: per-field tolerance map", "[test_support][golden][fast]") {
  const json golden =
      json::parse(R"({"frames":[{"i":0,"psnr_y":40.0},{"i":1,"psnr_y":39.0}],"pooled":{"psnr_y":{"mean":39.5}}})");
  const json actual =
      json::parse(R"({"frames":[{"i":0,"psnr_y":40.004},{"i":1,"psnr_y":39.0}],"pooled":{"psnr_y":{"mean":39.5}}})");
  CHECK_FALSE(compare_canonical(golden, actual).empty());

  CompareOptions options;
  options.tolerances["frames[].psnr_y"] = 0.005;
  CHECK(compare_canonical(golden, actual, options).empty());

  options.tolerances["frames[].psnr_y"] = 0.001;
  const auto differences = compare_canonical(golden, actual, options);
  REQUIRE(differences.size() == 1);
  CHECK(differences[0].path == "$.frames[0].psnr_y");
  CHECK(differences[0].message.find("tolerance 0.001") != std::string::npos);

  // The tolerance of one pattern does not leak into another.
  const json other = json::parse(R"({"pooled":{"psnr_y":{"mean":39.504}}})");
  options.tolerances["frames[].psnr_y"] = 1.0;
  CHECK_FALSE(compare_canonical(json::parse(R"({"pooled":{"psnr_y":{"mean":39.5}}})"), other, options).empty());
  options.default_tolerance = 0.01;
  CHECK(compare_canonical(json::parse(R"({"pooled":{"psnr_y":{"mean":39.5}}})"), other, options).empty());
}

TEST_CASE("golden_compare: tolerance file", "[test_support][golden][fast]") {
  const auto dir = scratch("tolerance_file");
  write_text(dir / "tol.json", R"({"default":0.5,"frames[].vmaf":0.01})");
  const CompareOptions options = ovt::test::load_compare_options(dir / "tol.json");
  CHECK(options.default_tolerance == 0.5);
  CHECK(options.tolerances.at("frames[].vmaf") == 0.01);

  write_text(dir / "bad.json", R"({"x":"high"})");
  CHECK_THROWS_AS(ovt::test::load_compare_options(dir / "bad.json"), std::invalid_argument);
  CHECK_THROWS_AS(ovt::test::load_compare_options(dir / "missing.json"), std::invalid_argument);
}

TEST_CASE("golden_compare: unified diff", "[test_support][golden][fast]") {
  CHECK(ovt::test::unified_diff("a\nb\n", "a\nb\n").empty());

  const std::string expected = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n";
  const std::string actual = "1\n2\n3\n4\nFIVE\n6\n7\n8\n9\n10\n";
  CHECK(ovt::test::unified_diff(expected, actual, "golden.json", "actual.json") ==
        "--- golden.json\n+++ actual.json\n@@ -2,7 +2,7 @@\n 2\n 3\n 4\n-5\n+FIVE\n 6\n 7\n 8\n");

  // Insertion at the end and deletion at the start, two hunks.
  const auto diff = ovt::test::unified_diff("x\n1\n2\n3\n4\n5\n6\n7\n8\n9\n", "1\n2\n3\n4\n5\n6\n7\n8\n9\ny\n");
  CHECK(diff.find("-x\n") != std::string::npos);
  CHECK(diff.find("+y\n") != std::string::npos);
  CHECK(diff.find("@@") != std::string::npos);
}

TEST_CASE("golden_compare: compare_json_golden reports mismatch with paths and a unified diff",
          "[test_support][golden][fast]") {
  const UpdateModeGuard compare_mode(false);
  const auto dir = scratch("json_golden_mismatch");
  const auto golden = dir / "x.pybox.json";

  CHECK_FALSE(ovt::test::compare_json_golden(golden, json::array()).ok);  // missing golden
  CHECK(ovt::test::compare_json_golden(golden, json::array()).message.find("golden file missing") != std::string::npos);

  write_text(golden, ovt::test::normalize_json(R"([{"path":"ftyp","offset":0,"size":28,"fields":{}}])"));
  CHECK(
      ovt::test::compare_json_golden(golden, json::parse(R"([{"path":"ftyp","offset":0,"size":28,"fields":{"v":1}}])"))
          .ok);

  const auto result =
      ovt::test::compare_json_golden(golden, json::parse(R"([{"path":"ftyp","offset":0,"size":32,"fields":{}}])"));
  CHECK_FALSE(result.ok);
  CHECK(result.message.find("$[0].size: golden 28, OVT 32") != std::string::npos);
  CHECK(result.message.find("@@") != std::string::npos);
  CHECK(result.message.find("-    \"size\": 28") != std::string::npos);
  CHECK(result.message.find("+    \"size\": 32") != std::string::npos);
  CHECK(result.message.find("OVT_UPDATE_GOLDEN=1") != std::string::npos);
}

TEST_CASE("golden_compare: update mode writes the normalised canonical JSON", "[test_support][golden][fast]") {
  const auto dir = scratch("json_golden_update");
  const auto golden = dir / "sub" / "y.json";
  {
    const UpdateModeGuard update_mode(true);
    const auto result = ovt::test::compare_json_golden(golden, json::parse(R"({"b":1.5,"a":[2]})"));
    CHECK(result.ok);
    CHECK(result.updated);
  }
  CHECK(read_text(golden) == "{\n  \"a\": [\n    2\n  ],\n  \"b\": 1.500000\n}\n");
  const UpdateModeGuard compare_mode(false);
  CHECK(ovt::test::compare_json_golden(golden, json::parse(R"({"a":[2],"b":1.5,"c":true})")).ok);
}
