// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.091 steps 2–4: meta.json reader/writer against the rules of docs/spec/cache-meta-1.schema.json, the
// zstd record framing and the atomic file writer.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "cache/atomic_file.hpp"
#include "cache/zstd_io.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/status.h"
#include "test_helpers.hpp"

namespace {

using ovt::cache::CacheError;
using ovt::cache::Meta;

const std::string kHex64(64, 'a');

Meta sample() {
  Meta m;
  m.source_hash = kHex64;
  m.source_name = "tears_of_steel_1080p.mp4";
  m.source_size = 734003200;
  m.created = "2026-11-03T14:05:09Z";
  m.last_used = "2026-11-03T15:00:00Z";
  m.ovt_version = "0.3.0+g1a2b3c4";
  m.ovt_model = {1, kHex64};
  m.ovt_container = {1, kHex64};
  m.ovt_metrics = {1, kHex64};
  m.codec = "H265";
  m.frame_count = 17982;
  m.index_complete = true;
  m.plugins = {{"ovt-plugin-de265", "1.0.0+de265-1.0.15", 1, 240}};
  m.planes_cached = false;
  return m;
}

ovt_status parse_status(std::string_view text) {
  try {
    (void)ovt::cache::parse_meta(text);
    return OVT_OK;
  } catch (const CacheError& e) {
    return e.code;
  }
}

}  // namespace

TEST_CASE("test_cache_meta: serialize_meta and parse_meta round-trip, deterministically", "[cache][fast]") {
  const Meta m = sample();
  const std::string text = ovt::cache::serialize_meta(m);
  CHECK(text == ovt::cache::serialize_meta(m));
  CHECK(text.find("\"cache_format\": 1") != std::string::npos);
  CHECK(text.find("\"cache_format\"") < text.find("\"source_hash\""));  // schema order
  CHECK(text.find("\"schemas\"") < text.find("\"codec\""));
  CHECK(text.back() == '\n');
  CHECK(ovt::cache::parse_meta(text) == m);
  // The spec's example (§6.7.2) parses.
  const std::string example = R"({ "cache_format": 1, "source_hash": ")" + kHex64 +
                              R"(", "source_name": "tears_of_steel_1080p.mp4", "source_size": 734003200,
  "created": "2026-11-03T14:05:09Z", "last_used": "2026-11-03T15:00:00Z", "ovt_version": "0.3.0+g1a2b3c4",
  "schemas": { "ovt_model": {"major": 1, "sha256": ")" +
                              kHex64 + R"("}, "ovt_container": {"major": 1, "sha256": ")" + kHex64 +
                              R"("}, "ovt_metrics": {"major": 1, "sha256": ")" + kHex64 + R"("} },
  "codec": "H265", "frame_count": 17982, "index_complete": true,
  "plugins": [ { "plugin_id": "ovt-plugin-de265", "plugin_version": "1.0.0+de265-1.0.15", "plane_stage_mask": 1, "frames": 240 } ],
  "planes_cached": false })";
  CHECK(ovt::cache::parse_meta(example) == m);
}

TEST_CASE("test_cache_meta: parse_meta enforces the schema rules", "[cache][fast]") {
  const std::string good = ovt::cache::serialize_meta(sample());
  auto with = [&](std::string_view from, std::string_view to) {
    std::string s = good;
    const auto pos = s.find(from);
    REQUIRE(pos != std::string::npos);
    return s.replace(pos, from.size(), to);
  };
  CHECK(parse_status(good) == OVT_OK);
  CHECK(parse_status("") == OVT_E_CORRUPT);
  CHECK(parse_status("[]") == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"cache_format\": 1", "\"cache_format\": 2")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"cache_format\": 1,", "")) == OVT_E_CORRUPT);  // required
  CHECK(parse_status(with("\"source_hash\": \"" + kHex64, "\"source_hash\": \"" + std::string(64, 'A'))) ==
        OVT_E_CORRUPT);  // pattern is lowercase
  CHECK(parse_status(with("\"created\": \"2026-11-03T14:05:09Z\"", "\"created\": \"yesterday\"")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"created\": \"2026-11-03T14:05:09Z\"", "\"created\": \"2026-11-03T14:05:09.250+01:00\"")) ==
        OVT_OK);
  CHECK(parse_status(with("\"codec\": \"H265\"", "\"codec\": \"HEVC\"")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"frame_count\": 17982", "\"frame_count\": -1")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"frame_count\": 17982", "\"frame_count\": 1.5")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"major\": 1", "\"major\": 0")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"ovt_metrics\"", "\"ovt_other\"")) == OVT_E_CORRUPT);  // required schema missing
  CHECK(parse_status(with("\"frames\": 240", "\"frames\": \"240\"")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"index_complete\": true", "\"index_complete\": 1")) == OVT_E_CORRUPT);
  CHECK(parse_status(with("\"planes_cached\": false", "\"planes_cached\": false, \"future\": {\"x\": 1}")) == OVT_OK);
  CHECK(parse_status(with("\"plugins\": [", "\"plugins\": [ 7,")) == OVT_E_CORRUPT);
}

TEST_CASE("test_cache_meta: timestamps", "[cache][fast]") {
  CHECK(ovt::cache::format_iso8601_utc(std::chrono::system_clock::time_point{}) == "1970-01-01T00:00:00Z");
  CHECK(ovt::cache::is_iso8601_date_time("2026-11-03T14:05:09Z"));
  CHECK(ovt::cache::is_iso8601_date_time("2026-11-03t14:05:09.5z"));
  CHECK(ovt::cache::is_iso8601_date_time("2026-11-03T14:05:09-08:00"));
  CHECK_FALSE(ovt::cache::is_iso8601_date_time("2026-11-03T14:05:09"));
  CHECK_FALSE(ovt::cache::is_iso8601_date_time("2026-11-03 14:05:09Z"));
  CHECK_FALSE(ovt::cache::is_iso8601_date_time("2026-11-03T14:05:09.Z"));
  CHECK_FALSE(ovt::cache::is_iso8601_date_time("2026-11-03T14:05:09+0800"));
  CHECK(ovt::cache::is_hex64(kHex64));
  CHECK_FALSE(ovt::cache::is_hex64(std::string(64, 'A')));
  CHECK_FALSE(ovt::cache::is_hex64(std::string(63, 'a')));
}

TEST_CASE("test_cache_meta: zstd records carry their size and reject malformed frames", "[cache][fast]") {
  ovt::cache::Compressor compressor;
  std::vector<std::uint8_t> input(100'000);
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::uint8_t>((i / 64) % 251);
  }
  const auto frames = compressor.compress(input, ovt::cache::kZstdLevelFrames);
  const auto planes = compressor.compress(input, ovt::cache::kZstdLevelPlanes);
  CHECK(frames.size() < input.size() / 5);
  CHECK(ovt::cache::decompress(frames, 1U << 20U) == input);
  CHECK(ovt::cache::decompress(planes, 1U << 20U, input.size()) == input);
  auto status_of = [](auto&& fn) {
    try {
      fn();
      return OVT_OK;
    } catch (const CacheError& e) {
      return e.code;
    }
  };
  CHECK(status_of([&] { (void)ovt::cache::decompress(frames, input.size() - 1); }) == OVT_E_CORRUPT);  // too large
  CHECK(status_of([&] { (void)ovt::cache::decompress(frames, 1U << 20U, input.size() + 1); }) == OVT_E_CORRUPT);
  std::vector<std::uint8_t> trailing = frames;
  trailing.push_back(0);
  CHECK(status_of([&] { (void)ovt::cache::decompress(trailing, 1U << 20U); }) == OVT_E_CORRUPT);
  std::vector<std::uint8_t> truncated(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(frames.size() / 2));
  CHECK(status_of([&] { (void)ovt::cache::decompress(truncated, 1U << 20U); }) == OVT_E_CORRUPT);
  std::vector<std::uint8_t> flipped = frames;
  flipped[flipped.size() / 2] ^= 0xFFU;
  CHECK(status_of([&] { (void)ovt::cache::decompress(flipped, 1U << 20U); }) == OVT_E_CORRUPT);
  const std::vector<std::uint8_t> garbage = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(status_of([&] { (void)ovt::cache::decompress(garbage, 1U << 20U); }) == OVT_E_CORRUPT);
  CHECK(status_of([&] { (void)ovt::cache::decompress({}, 1U << 20U); }) == OVT_E_CORRUPT);
  // A skippable frame (magic 0x184D2A50, 4-byte size, payload) decodes to nothing: not a record.
  const std::vector<std::uint8_t> skippable = {0x50, 0x2A, 0x4D, 0x18, 0x02, 0x00, 0x00, 0x00, 0xAB, 0xCD};
  CHECK(status_of([&] { (void)ovt::cache::decompress(skippable, 1U << 20U); }) == OVT_E_CORRUPT);
}

TEST_CASE("test_cache_meta: write_file_atomic replaces the target and leaves no temporary", "[cache][fast]") {
  const ovt::cache::test::TempRoot tmp("atomic");
  std::filesystem::create_directories(tmp.path());
  const auto path = tmp.path() / "meta.json";
  const std::vector<std::uint8_t> one = {'o', 'n', 'e'};
  const std::vector<std::uint8_t> two = {'t', 'w', 'o', '!'};
  ovt::cache::write_file_atomic(path, one);
  CHECK(ovt::cache::read_file(path, 100) == one);
  ovt::cache::write_file_atomic(path, two);
  CHECK(ovt::cache::read_file(path, 100) == two);
  CHECK_FALSE(std::filesystem::exists(tmp.path() / "meta.json.tmp"));
  CHECK_THROWS_AS(ovt::cache::read_file(path, 3), CacheError);  // over max_size
  CHECK_THROWS_AS(ovt::cache::read_file(tmp.path() / "absent", 100), CacheError);
  CHECK_THROWS_AS(ovt::cache::write_file_atomic(tmp.path() / "no-dir" / "x", one), CacheError);
  ovt::cache::set_simulated_crash_before_rename(true);
  CHECK_THROWS_AS(ovt::cache::write_file_atomic(path, one), CacheError);
  ovt::cache::set_simulated_crash_before_rename(false);
  CHECK(std::filesystem::exists(tmp.path() / "meta.json.tmp"));
  CHECK(ovt::cache::read_file(path, 100) == two);  // the target is untouched
  CHECK(ovt::cache::remove_stale_tmp_files(tmp.path()) == 1);
  CHECK_FALSE(std::filesystem::exists(tmp.path() / "meta.json.tmp"));
}
