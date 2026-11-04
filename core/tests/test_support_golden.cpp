// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of ovt::test::compare_or_update and normalize_json (chapter 14 §14.1.3, §14.2.2, WI-0.550).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support/golden.h"
#include "test_support/fb_normalize.h"

#ifndef OVT_TEST_SOURCE_DIR
#error "OVT_TEST_SOURCE_DIR must point at the repository root"
#endif

namespace {

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Restores the process-wide update mode chosen by main() when a test case ends.
class UpdateModeGuard {
 public:
  UpdateModeGuard() : saved_(ovt::test::update_golden()) {}
  ~UpdateModeGuard() { ovt::test::set_update_golden(saved_); }
  UpdateModeGuard(const UpdateModeGuard&) = delete;
  UpdateModeGuard& operator=(const UpdateModeGuard&) = delete;

 private:
  bool saved_;
};

std::span<const uint8_t> as_bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

}  // namespace

TEST_CASE("test_support/golden: compare, mismatch and update", "[test_support][fast]") {
  const UpdateModeGuard guard;
  const auto golden = std::filesystem::path(OVT_TEST_BINARY_DIR) / "scratch_golden" / "sample.txt";
  std::filesystem::remove_all(golden.parent_path());

  ovt::test::set_update_golden(false);
  CHECK(ovt::test::compare_bytes_or_update(golden, as_bytes("abc")).outcome == ovt::test::GoldenOutcome::kMissing);

  ovt::test::set_update_golden(true);
  CHECK(ovt::test::compare_bytes_or_update(golden, as_bytes("abc")).outcome == ovt::test::GoldenOutcome::kUpdated);
  CHECK(read_text(golden) == "abc");

  ovt::test::set_update_golden(false);
  CHECK(ovt::test::compare_bytes_or_update(golden, as_bytes("abc")).outcome == ovt::test::GoldenOutcome::kMatched);
  const auto diff = ovt::test::compare_bytes_or_update(golden, as_bytes("abd"));
  CHECK(diff.outcome == ovt::test::GoldenOutcome::kMismatch);
  CHECK(diff.first_difference == 2);
  const auto longer = ovt::test::compare_bytes_or_update(golden, as_bytes("abcd"));
  CHECK(longer.outcome == ovt::test::GoldenOutcome::kMismatch);
  CHECK(longer.first_difference == 3);

  std::filesystem::remove_all(golden.parent_path());
}

TEST_CASE("test_support/json: canonical form", "[test_support][fast]") {
  CHECK(ovt::test::normalize_json(R"({"b":1,"a":[2.5,true,null,"x"],"c":{}})") ==
        "{\n  \"a\": [\n    2.500000,\n    true,\n    null,\n    \"x\"\n  ],\n  \"b\": 1,\n  \"c\": {}\n}\n");
  CHECK(ovt::test::normalize_json("-0.0") == "0.000000\n");
  CHECK(ovt::test::normalize_json("1e-7") == "0.000000\n");
  CHECK(ovt::test::normalize_json("[18446744073709551615, -3]") == "[\n  18446744073709551615,\n  -3\n]\n");
  CHECK_THROWS_AS(ovt::test::normalize_json("{"), std::invalid_argument);
}

// The golden of this test is tests/golden/test_support/normalize_json.json. To see acceptance test 3 of
// WI-0.550: edit that file, run `OVT_UPDATE_GOLDEN=1 ctest -R normalize_json`, and the file is rewritten.
TEST_CASE("test_support/json: normalize_json matches its golden file", "[test_support][golden][fast]") {
  const std::string input =
      R"({"frames":[{"poc":0,"psnr_y":41.123456789,"type":"I"},{"poc":2,"psnr_y":39.5,"type":"P"}],)"
      R"("codec":"h265","bitrate_kbps":1500})";
  const auto golden = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/test_support/normalize_json.json";
  CHECK(ovt::test::compare_or_update(golden, ovt::test::normalize_json(input)));
}
