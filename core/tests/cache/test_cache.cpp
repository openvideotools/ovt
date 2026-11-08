// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.091 acceptance tests 1–3 and the behaviour of ovt::cache::Cache (chapter 06 §6.7.1–6.7.2): round trips,
// crash safety, the 1080p size budget, the cache-root rule, concurrent readers, planes, reports, plugin
// bookkeeping and the error mapping.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_templated.hpp>

#include "cache/atomic_file.hpp"
#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/cxx/model/plane.hpp"
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

void set_env(const char* name, const char* value) {
  if (value == nullptr) {
    ::unsetenv(name);  // NOLINT(concurrency-mt-unsafe): single-threaded test
  } else {
    ::setenv(name, value, 1);  // NOLINT(concurrency-mt-unsafe)
  }
}

}  // namespace

TEST_CASE("test_cache: hex encodes and parse_hex decodes a source hash", "[cache][fast]") {
  const SourceHash h = hash_of(0xA0);
  const std::string text = ovt::cache::hex(h);
  CHECK(text.size() == 64);
  CHECK(text.substr(0, 6) == "a0a1a2");
  CHECK(ovt::cache::parse_hex(text) == h);
  CHECK_FALSE(ovt::cache::parse_hex("zz").has_value());
  CHECK_FALSE(ovt::cache::parse_hex(std::string(63, 'a') + "g").has_value());
}

// Acceptance test 1. The directory is a fixed one so that the CTest test cache_meta_json_schema
// (core/tests/cache/validate_json_schema.py) can validate the meta.json it leaves behind with
// `python -m jsonschema`.
TEST_CASE("test_cache: AT1 index and three frames read back byte-identical after reopen", "[cache][fast]") {
  const fs::path root = ovt::cache::test::fixed_dir("at1");
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
TEST_CASE("test_cache: AT3 100 frames of 1080p grids take at most 100 MB on disk", "[cache][slow]") {
  const TempRoot tmp("at3");
  const SourceHash h = hash_of(0x30);
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(h);
  std::uint64_t uncompressed = 0;
  std::chrono::steady_clock::duration writing{};
  for (std::uint32_t i = 0; i < 100; ++i) {
    FrameSpec spec;
    spec.decode_index = static_cast<std::int32_t>(i);
    spec.cells_w = 480;
    spec.cells_h = 270;
    const std::vector<std::uint8_t> frame = make_frame(spec);
    uncompressed += frame.size();
    const auto t0 = std::chrono::steady_clock::now();
    entry->put_frame(i, frame);
    writing += std::chrono::steady_clock::now() - t0;
  }
  entry->flush();
  const std::uint64_t on_disk = walk_bytes(entry->dir());
  const double seconds = std::chrono::duration<double>(writing).count();
  WARN("AT3: " << uncompressed / 1'000'000 << " MB of FrameAnalysis -> " << on_disk / 1'000'000 << " MB on disk (ratio "
               << static_cast<double>(uncompressed) / static_cast<double>(on_disk) << "), write throughput "
               << static_cast<double>(uncompressed) / 1e6 / seconds << " MB/s uncompressed");
  CHECK(uncompressed >= 400'000'000);  // "≈ 5 MB each"
  CHECK(on_disk <= 100'000'000);
  CHECK(on_disk * 5 <= uncompressed);
  CHECK(entry->bytes() == on_disk);
}

TEST_CASE("test_cache: default_root follows set_default_root, OVT_CACHE_DIR and the platform rule", "[cache][fast]") {
  set_env("OVT_CACHE_DIR", nullptr);
  set_env("XDG_CACHE_HOME", nullptr);
  set_env("HOME", "/home/karl");
  ovt::cache::set_default_root(std::nullopt);
#if defined(__APPLE__)
  CHECK(ovt::cache::default_root() == fs::path("/home/karl/Library/Caches/org.openvideotools.ovt"));
#elif !defined(__ANDROID__)
  CHECK(ovt::cache::default_root() == fs::path("/home/karl/.cache/ovt"));  // acceptance test 4's rule
  set_env("XDG_CACHE_HOME", "/var/cache/me");
  CHECK(ovt::cache::default_root() == fs::path("/var/cache/me/ovt"));
  set_env("XDG_CACHE_HOME", "");  // empty counts as unset (§6.7.1)
  CHECK(ovt::cache::default_root() == fs::path("/home/karl/.cache/ovt"));
  set_env("HOME", nullptr);
  CHECK_THROWS_MATCHES(ovt::cache::default_root(), CacheError, StatusIs(OVT_E_IO));
#endif
  set_env("OVT_CACHE_DIR", "/tmp/ovt-env-root");
  CHECK(ovt::cache::default_root() == fs::path("/tmp/ovt-env-root"));
  ovt::cache::set_default_root(fs::path("/tmp/ovt-set-root"));
  CHECK(ovt::cache::default_root() == fs::path("/tmp/ovt-set-root"));
  ovt::cache::set_default_root(std::nullopt);
  set_env("OVT_CACHE_DIR", nullptr);
}

TEST_CASE("test_cache: Cache::open with no root uses default_root and creates it with mode 0700", "[cache][fast]") {
  const TempRoot tmp("default_root");
  ovt::cache::set_default_root(tmp.path() / "deeper" / "ovt");
  {
    auto cache = Cache::open(std::nullopt);
    CHECK(cache->root() == tmp.path() / "deeper" / "ovt");
    CHECK(cache->max_bytes() == ovt::cache::kDefaultMaxBytes);
  }
  ovt::cache::set_default_root(std::nullopt);
  CHECK(fs::is_directory(tmp.path() / "deeper" / "ovt"));
  CHECK((fs::status(tmp.path() / "deeper" / "ovt").permissions() & fs::perms::others_all) == fs::perms::none);
  CHECK((fs::status(tmp.path() / "deeper" / "ovt").permissions() & fs::perms::group_all) == fs::perms::none);
}

TEST_CASE("test_cache: concurrent readers see whole records while a writer adds and replaces frames",
          "[cache][fast][threads]") {
  const TempRoot tmp("concurrent");
  const SourceHash h = hash_of(0x40);
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(h);
  constexpr std::uint32_t kInitial = 8;
  constexpr std::uint32_t kTotal = 24;
  std::vector<std::vector<std::uint8_t>> frames;
  for (std::uint32_t i = 0; i < kTotal; ++i) {
    FrameSpec spec;
    spec.decode_index = static_cast<std::int32_t>(i);
    spec.cells_w = 48;
    spec.cells_h = 27;
    frames.push_back(make_frame(spec));
  }
  const std::vector<std::uint8_t> index = make_index(h, kTotal);
  entry->put_index(index);
  for (std::uint32_t i = 0; i < kInitial; ++i) {
    entry->put_frame(i, frames[i]);
  }
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<std::uint64_t> reads{0};
  auto reader = [&] {
    while (!stop.load()) {
      for (std::uint32_t i = 0; i < kTotal; ++i) {
        const auto got = entry->get_frame(i);
        if (got && *got != frames[i]) {
          failures.fetch_add(1);
        }
        if (!got && i < kInitial) {
          failures.fetch_add(1);
        }
      }
      if (entry->get_index() != index) {
        failures.fetch_add(1);
      }
      reads.fetch_add(1);
    }
  };
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back(reader);
  }
  for (std::uint32_t i = kInitial; i < kTotal; ++i) {
    entry->put_frame(i, frames[i]);
    entry->put_frame(i % kInitial, frames[i % kInitial]);  // replace an existing file with the same bytes
    entry->put_index(index);
  }
  stop.store(true);
  for (auto& t : readers) {
    t.join();
  }
  CHECK(failures.load() == 0);
  CHECK(reads.load() > 0);
  for (std::uint32_t i = 0; i < kTotal; ++i) {
    CHECK(entry->get_frame(i) == frames[i]);
  }
  CHECK(entry->meta().plugins.at(0).frames == kTotal);
}

TEST_CASE("test_cache: planes are stored tight and read back with the PlaneRef geometry", "[cache][fast]") {
  const TempRoot tmp("planes");
  const SourceHash h = hash_of(0x50);
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(h);
  FrameSpec spec;
  spec.with_recon_y_ref = true;
  spec.plane_w = 64;
  spec.plane_h = 32;
  entry->put_frame(0, make_frame(spec));

  std::vector<std::uint8_t> padded(static_cast<std::size_t>(96) * 32);
  for (std::size_t i = 0; i < padded.size(); ++i) {
    padded[i] = static_cast<std::uint8_t>(i % 96 < 64 ? i : 0xEE);  // padding bytes must not reach the file
  }
  ovt::model::PlaneView view;
  view.data = padded.data();
  view.width = 64;
  view.height = 32;
  view.stride_bytes = 96;
  view.bit_depth = 8;
  view.sample_bytes = 1;
  view.format = ovt::model::SampleFormat::U8;
  entry->put_plane(0, ovt::fb::PlaneStage::Reconstruction, 0, view);
  CHECK(fs::is_regular_file(entry->dir() / "planes" / "00000000" / "recon-y.pl.zst"));
  CHECK(entry->meta().planes_cached == true);

  const auto got = entry->get_plane(0, ovt::fb::PlaneStage::Reconstruction, 0);
  REQUIRE(got.has_value());
  CHECK(got->bytes.size() == static_cast<std::size_t>(64U * 32U));
  CHECK(got->view.width == 64);
  CHECK(got->view.height == 32);
  CHECK(got->view.stride_bytes == 64);
  CHECK(got->view.data == got->bytes.data());
  bool rows_match = true;
  for (std::size_t row = 0; row < 32; ++row) {
    for (std::size_t x = 0; x < 64; ++x) {
      rows_match = rows_match && got->bytes[row * 64 + x] == padded[row * 96 + x];
    }
  }
  CHECK(rows_match);
  // No PlaneRef for the chroma plane: a miss, not an error.
  CHECK_FALSE(entry->get_plane(0, ovt::fb::PlaneStage::Reconstruction, 1).has_value());
  CHECK_FALSE(entry->get_plane(1, ovt::fb::PlaneStage::Reconstruction, 0).has_value());
  // I6: a plane file whose size disagrees with the PlaneRef is corrupt.
  view.width = 32;
  view.stride_bytes = 32;
  entry->put_plane(0, ovt::fb::PlaneStage::Reconstruction, 0, view);
  CHECK_THROWS_MATCHES(entry->get_plane(0, ovt::fb::PlaneStage::Reconstruction, 0), CacheError,
                       StatusIs(OVT_E_CORRUPT));
  CHECK_THROWS_MATCHES(entry->put_plane(0, ovt::fb::PlaneStage::Reconstruction, 7, view), CacheError,
                       StatusIs(OVT_E_INVALID_ARG));
}

TEST_CASE("test_cache: a frame rewritten by another plugin moves the count in meta.plugins", "[cache][fast]") {
  const TempRoot tmp("plugins");
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(hash_of(0x60));
  FrameSpec a;
  entry->put_frame(0, make_frame(a));
  a.decode_index = 1;
  entry->put_frame(1, make_frame(a));
  FrameSpec b;
  b.plugin_id = "ovt-plugin-hm";
  b.plugin_version = "18.0";
  const std::vector<std::uint8_t> hm = make_frame(b);
  entry->put_frame(0, hm);
  const Meta meta = entry->meta();
  REQUIRE(meta.plugins.size() == 2);
  CHECK(meta.plugins[0].plugin_id == "ovt-plugin-mock");
  CHECK(meta.plugins[0].frames == 1);
  CHECK(meta.plugins[1].plugin_id == "ovt-plugin-hm");
  CHECK(meta.plugins[1].frames == 1);
  CHECK(entry->get_frame(0) == hm);
  b.decode_index = 1;
  entry->put_frame(1, make_frame(b));
  REQUIRE(entry->meta().plugins.size() == 1);  // the mock plugin has no frames left
  CHECK(entry->meta().plugins[0].frames == 2);
}

TEST_CASE("test_cache: reports round-trip under reports/<kind>.fb.zst", "[cache][fast]") {
  const TempRoot tmp("reports");
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(hash_of(0x70));
  const std::vector<std::uint8_t> report = make_index(hash_of(0x70), 1);  // any FlatBuffer is opaque here
  entry->put_report("metrics", report);
  CHECK(fs::is_regular_file(entry->dir() / "reports" / "metrics.fb.zst"));
  CHECK(entry->get_report("metrics") == report);
  CHECK_FALSE(entry->get_report("hdr").has_value());
  CHECK_THROWS_MATCHES(entry->put_report("../etc", report), CacheError, StatusIs(OVT_E_INVALID_ARG));
  CHECK_THROWS_MATCHES(entry->put_report("", report), CacheError, StatusIs(OVT_E_INVALID_ARG));
}

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

TEST_CASE("test_cache: parse_options_json reads max_bytes and read_only", "[cache][fast]") {
  CHECK(ovt::cache::parse_options_json("") == Options{});
  CHECK(ovt::cache::parse_options_json("null") == Options{});
  CHECK(ovt::cache::parse_options_json("{}") == Options{});
  const Options parsed = ovt::cache::parse_options_json(R"({"max_bytes": 2147483648, "read_only": true, "x": 1})");
  CHECK(parsed.max_bytes == 2147483648U);
  CHECK(parsed.read_only);
  CHECK_THROWS_MATCHES(ovt::cache::parse_options_json("[1]"), CacheError, StatusIs(OVT_E_INVALID_ARG));
  CHECK_THROWS_MATCHES(ovt::cache::parse_options_json(R"({"max_bytes": -1})"), CacheError, StatusIs(OVT_E_INVALID_ARG));
  CHECK_THROWS_MATCHES(ovt::cache::parse_options_json(R"({"max_bytes": "big"})"), CacheError,
                       StatusIs(OVT_E_INVALID_ARG));
  CHECK_THROWS_MATCHES(ovt::cache::parse_options_json(R"({"read_only": 1})"), CacheError, StatusIs(OVT_E_INVALID_ARG));
  CHECK_THROWS_MATCHES(ovt::cache::parse_options_json("{"), CacheError, StatusIs(OVT_E_INVALID_ARG));
}
