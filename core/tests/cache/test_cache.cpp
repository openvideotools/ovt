// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.091 acceptance tests 1 and 2 and the behaviour of ovt::cache::Cache (chapter 06 §6.7.1–6.7.2): round
// trips, crash safety and the error mapping. AT3 (1080p size budget), AT4 (cache-root rule), plugin bookkeeping,
// concurrent readers, planes and reports follow in the last PR of the WI-0.091 stack.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>

#include "cache/atomic_file.hpp"
#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/status.h"
#include "test_helpers.hpp"

namespace {

namespace fs = std::filesystem;
using ovt::cache::Cache;
using ovt::cache::CacheError;
using ovt::cache::Meta;
using ovt::cache::Options;
using ovt::cache::SourceHash;
using ovt::cache::test::FrameSpec;
using ovt::cache::test::hash_of;
using ovt::cache::test::make_frame;
using ovt::cache::test::make_index;
using ovt::cache::test::TempRoot;

struct StatusIs : Catch::Matchers::MatcherGenericBase {
  explicit StatusIs(ovt_status s) : status(s) {}
  [[nodiscard]] bool match(const CacheError& e) const { return e.code == status; }
  [[nodiscard]] std::string describe() const override { return std::string("has status ") + ovt_status_string(status); }
  ovt_status status;
};

std::uint64_t walk_bytes(const fs::path& dir) {
  std::uint64_t total = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.is_regular_file()) {
      total += e.file_size();
    }
  }
  return total;
}

std::vector<fs::path> tmp_files(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (e.path().extension() == ".tmp") {
      out.push_back(e.path());
    }
  }
  return out;
}

std::vector<std::uint8_t> read_all(const fs::path& p) {
  return ovt::cache::read_file(p, 1U << 30U);
}

}  // namespace

TEST_CASE("test_cache: AT1 index and three frames read back byte-identical after reopen", "[cache][fast]") {
  const fs::path root = fs::path(OVT_TEST_BINARY_DIR) / "at1";
  fs::remove_all(root);
  const SourceHash h = hash_of(0x10);
  const std::vector<std::uint8_t> index = make_index(h, 3);  // FrameInfo.pts set: minalign 8 like every real index
  std::vector<std::vector<std::uint8_t>> frames;
  for (int i = 0; i < 3; ++i) {
    FrameSpec spec;
    spec.decode_index = i;
    spec.cells_w = 32;
    spec.cells_h = 18;
    spec.with_pts = i != 2;  // frame 2 has minalign 4: both layouts must round-trip
    frames.push_back(make_frame(spec));
  }
  {
    auto cache = Cache::open(root);
    auto entry = cache->entry(h);
    entry->set_source_info("/videos/tears_of_steel_1080p.mp4", 734003200);
    entry->put_index(index);
    for (std::uint32_t i = 0; i < 3; ++i) {
      entry->put_frame(i, frames[i]);
    }
    CHECK(entry->meta().plugins.size() == 1);
  }
  // Layout of §6.7.1.
  const fs::path dir = root / ovt::cache::hex(h);
  CHECK(fs::is_regular_file(root / "cache.json"));
  CHECK(fs::is_regular_file(dir / "meta.json"));
  CHECK(fs::is_regular_file(dir / "index.fb"));
  CHECK(fs::is_regular_file(dir / "frames" / "00000000.fa.zst"));
  CHECK(fs::is_regular_file(dir / "frames" / "00000002.fa.zst"));
  CHECK(tmp_files(root).empty());

  auto cache = Cache::open(root);
  auto entry = cache->entry(h);
  REQUIRE(entry->get_index().has_value());
  CHECK(*entry->get_index() == index);
  for (std::uint32_t i = 0; i < 3; ++i) {
    REQUIRE(entry->get_frame(i).has_value());
    CHECK(*entry->get_frame(i) == frames[i]);
  }
  CHECK_FALSE(entry->get_frame(3).has_value());

  const Meta meta = entry->meta();
  CHECK(meta.source_hash == ovt::cache::hex(h));
  CHECK(meta.source_name == "tears_of_steel_1080p.mp4");  // basename only
  CHECK(meta.source_size == 734003200U);
  CHECK(meta.codec == "H265");
  CHECK(meta.frame_count == 3);
  CHECK(meta.index_complete == true);
  CHECK(meta.planes_cached == false);
  REQUIRE(meta.plugins.size() == 1);
  CHECK(meta.plugins[0].plugin_id == "ovt-plugin-mock");
  CHECK(meta.plugins[0].plugin_version == "1.0.0");
  CHECK(meta.plugins[0].frames == 3);
  CHECK(ovt::cache::is_hex64(meta.ovt_model.sha256));
  CHECK(ovt::cache::is_hex64(meta.ovt_container.sha256));
  CHECK(ovt::cache::is_hex64(meta.ovt_metrics.sha256));
  CHECK(ovt::cache::parse_meta(read_all(dir / "meta.json")) == meta);  // the file is what the object says
  CHECK(entry->bytes() == walk_bytes(dir));
  CHECK(cache->entries() == std::vector<SourceHash>{h});
}

// Acceptance test 2: a crash between the fsync of meta.json.tmp and the rename.
TEST_CASE("test_cache: AT2 a crash before rename keeps the previous meta.json and leaves no .tmp", "[cache][fast]") {
  const TempRoot tmp("at2");
  const SourceHash h = hash_of(0x20);
  FrameSpec spec;
  Meta before;
  {
    auto cache = Cache::open(tmp.path());
    auto entry = cache->entry(h);
    entry->put_frame(0, make_frame(spec));
    entry->flush();
    before = ovt::cache::parse_meta(read_all(entry->dir() / "meta.json"));
    REQUIRE(before.plugins.size() == 1);
    CHECK(before.plugins[0].frames == 1);

    spec.decode_index = 1;
    entry->put_frame(1, make_frame(spec));  // meta is now dirty: frames == 2 in memory
    ovt::cache::set_simulated_crash_before_rename(true);
    CHECK_THROWS_MATCHES(entry->flush(), CacheError, StatusIs(OVT_E_IO));
    // The process dies here: the destructors run with the crash still armed and cannot write either.
  }
  ovt::cache::set_simulated_crash_before_rename(false);
  const fs::path dir = tmp.path() / ovt::cache::hex(h);
  REQUIRE(fs::is_regular_file(dir / "meta.json.tmp"));
  CHECK(ovt::cache::parse_meta(read_all(dir / "meta.json")) == before);

  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(h);
  CHECK(tmp_files(tmp.path()).empty());
  const Meta after = ovt::cache::parse_meta(read_all(dir / "meta.json"));
  CHECK(after.plugins == before.plugins);
  CHECK(after.created == before.created);
  CHECK(entry->get_frame(1).has_value());  // the frame file itself was renamed before the crash
}

// Acceptance test 3: 100 dense 1080p grids (480 x 270 cells, about 4.4 MB each) compress at least 5x.
// The m1-air-8gb throughput budget (chapter 14 §14.9.3 bench_cache_write) is the perf harness's; the rate
// measured here is reported for the acceptance record.
TEST_CASE("test_cache: wrong buffers, read-only caches and corrupt files map to their status codes", "[cache][fast]") {
  const TempRoot tmp("errors");
  const SourceHash h = hash_of(0x80);
  const SourceHash other = hash_of(0x81);
  {
    auto cache = Cache::open(tmp.path());
    auto entry = cache->entry(h);
    CHECK_THROWS_MATCHES(entry->put_index(make_index(other, 1)), CacheError, StatusIs(OVT_E_INVALID_ARG));
    const std::vector<std::uint8_t> garbage(64, 0x55);
    CHECK_THROWS_MATCHES(entry->put_index(garbage), CacheError, StatusIs(OVT_E_INVALID_ARG));
    CHECK_THROWS_MATCHES(entry->put_frame(0, garbage), CacheError, StatusIs(OVT_E_INVALID_ARG));
    FrameSpec spec;
    spec.decode_index = 5;
    CHECK_THROWS_MATCHES(entry->put_frame(4, make_frame(spec)), CacheError, StatusIs(OVT_E_INVALID_ARG));
    entry->put_frame(5, make_frame(spec));
    entry->put_index(make_index(h, 6));
  }
  SECTION("read-only") {
    Options ro;
    ro.read_only = true;
    auto cache = Cache::open(tmp.path(), ro);
    auto entry = cache->entry(h);
    CHECK(entry->get_frame(5).has_value());
    CHECK_THROWS_MATCHES(entry->put_frame(5, make_frame(FrameSpec{.decode_index = 5})), CacheError,
                         StatusIs(OVT_E_UNSUPPORTED));
    CHECK_THROWS_MATCHES(cache->entry(other), CacheError, StatusIs(OVT_E_IO));
    CHECK_THROWS_MATCHES(Cache::open(tmp.path() / "absent", ro), CacheError, StatusIs(OVT_E_IO));
  }
  SECTION("truncated frame file") {
    const fs::path frame = tmp.path() / ovt::cache::hex(h) / "frames" / "00000005.fa.zst";
    const auto bytes = read_all(frame);
    ovt::cache::write_file_atomic(frame, std::span<const std::uint8_t>(bytes).first(bytes.size() / 2));
    auto cache = Cache::open(tmp.path());
    CHECK_THROWS_MATCHES(cache->entry(h)->get_frame(5), CacheError, StatusIs(OVT_E_CORRUPT));
  }
  SECTION("garbage index.fb") {
    const std::vector<std::uint8_t> garbage(100, 0x11);
    ovt::cache::write_file_atomic(tmp.path() / ovt::cache::hex(h) / "index.fb", garbage);
    auto cache = Cache::open(tmp.path());
    CHECK_THROWS_MATCHES(cache->entry(h)->get_index(), CacheError, StatusIs(OVT_E_CORRUPT));
  }
  SECTION("corrupt meta.json") {
    const std::string text = "{\"cache_format\": 2}";
    ovt::cache::write_file_atomic(
        tmp.path() / ovt::cache::hex(h) / "meta.json",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    auto cache = Cache::open(tmp.path());
    CHECK_THROWS_MATCHES(cache->entry(h), CacheError, StatusIs(OVT_E_CORRUPT));
  }
  SECTION("malformed cache.json is rebuilt from the directories") {
    // Not JSON, or JSON with members of the wrong type: neither may escape as an exception.
    const std::string text = GENERATE(as<std::string>{}, "not json", R"({"cache_format": "1", "entries": []})",
                                      R"({"cache_format": 1, "entries": [{"source_hash": 5, "last_used": 5}]})");
    ovt::cache::write_file_atomic(
        tmp.path() / "cache.json",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    Options opts;
    opts.max_bytes = 1234;
    auto cache = Cache::open(tmp.path(), opts);
    CHECK(cache->entries() == std::vector<SourceHash>{h});
    CHECK(cache->max_bytes() == 1234);
    cache.reset();
    CHECK(read_all(tmp.path() / "cache.json").size() > 20);
  }
}
