// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.092 acceptance tests 1 and 2 and one test per invalidation rule of chapter 06 §6.7.3 (I1–I7): the
// check functions on their own, Cache::open_entry() on damaged directories, invalidating frame and plane
// reads, and the last_used throttle of §6.7.4.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "cache/atomic_file.hpp"
#include "cache/invalidate.hpp"
#include "cache/zstd_io.hpp"
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
using ovt::cache::InvalidationReason;
using ovt::cache::Meta;
using ovt::cache::Options;
using ovt::cache::SelectedPlugin;
using ovt::cache::SourceHash;
using ovt::cache::test::FrameSpec;
using ovt::cache::test::hash_of;
using ovt::cache::test::make_frame;
using ovt::cache::test::make_index;
using ovt::cache::test::TempRoot;

constexpr std::size_t kBig = 1U << 30U;

std::span<const std::uint8_t> as_bytes(const std::string& text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

fs::path entry_dir(const TempRoot& tmp, const SourceHash& h) {
  return tmp.path() / ovt::cache::hex(h);
}

// Rewrites <dir>/meta.json through parse_meta / serialize_meta.
void edit_meta(const fs::path& dir, const std::function<void(Meta&)>& edit) {
  Meta meta = ovt::cache::parse_meta(ovt::cache::read_file(dir / "meta.json", kBig));
  edit(meta);
  ovt::cache::write_file_atomic(dir / "meta.json", as_bytes(ovt::cache::serialize_meta(meta)));
}

// An entry with an H.265 index of four frames and frames 0..3 written by `plugin_id` `plugin_version`.
void populate(const TempRoot& tmp, const SourceHash& h, const std::string& plugin_version = "1.0") {
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(h);
  entry->put_index(make_index(h, 4, ovt::fb::Codec::H265));
  for (std::int32_t i = 0; i < 4; ++i) {
    FrameSpec spec;
    spec.decode_index = i;
    spec.plugin_id = "A";
    spec.plugin_version = plugin_version;
    spec.with_recon_y_ref = i == 0;
    entry->put_frame(static_cast<std::uint32_t>(i), make_frame(spec));
  }
}

std::vector<std::uint8_t> prefixed(std::vector<std::uint8_t> plain) {
  // The stored form of a record: what Entry writes before compression (size prefix + 4 pad bytes).
  const TempRoot tmp("invalidate_prefix");
  const SourceHash h = hash_of(0x01);
  auto cache = Cache::open(tmp.path());
  auto entry = cache->entry(h);
  entry->put_report("probe", plain);
  return ovt::cache::decompress(ovt::cache::read_file(entry->dir() / "reports" / "probe.fb.zst", kBig), kBig);
}

}  // namespace

TEST_CASE("test_invalidate: each check function detects its rule", "[cache][fast]") {
  const SourceHash h = hash_of(0x20);
  Meta meta;
  meta.source_hash = ovt::cache::hex(h);
  meta.created = "2026-11-08T00:00:00Z";
  meta.ovt_version = "0.0.0";
  meta.ovt_model.sha256 = meta.ovt_container.sha256 = meta.ovt_metrics.sha256 = std::string(64, 'a');
  meta.codec = "H265";

  SECTION("I1") {
    CHECK(ovt::cache::check_source_hash(meta, ovt::cache::hex(h)) == InvalidationReason::None);
    CHECK(ovt::cache::check_source_hash(meta, ovt::cache::hex(hash_of(0x21))) == InvalidationReason::ForeignSourceHash);
  }
  SECTION("I2") {
    const std::string good = ovt::cache::serialize_meta(meta);
    Meta parsed;
    CHECK(ovt::cache::check_cache_format(as_bytes(good), &parsed) == InvalidationReason::None);
    CHECK(parsed == meta);
    const std::string bad =
        GENERATE(as<std::string>{}, "", "not json", "[]", R"({"cache_format": 2})", R"({"cache_format": 1})");
    CHECK(ovt::cache::check_cache_format(as_bytes(bad)) == InvalidationReason::CacheFormat);
  }
  SECTION("I3") {
    CHECK(ovt::cache::check_schema_majors(meta) == InvalidationReason::None);
    Meta m = meta;
    m.ovt_model.major = 2;
    CHECK(ovt::cache::check_schema_majors(m) == InvalidationReason::SchemaMajor);
    m = meta;
    m.ovt_metrics.major = 2;
    CHECK(ovt::cache::check_schema_majors(m) == InvalidationReason::SchemaMajor);
  }
  SECTION("I4") {
    const auto index = prefixed(make_index(h, 2, ovt::fb::Codec::H265));
    CHECK(ovt::cache::check_index(index, meta, std::nullopt) == InvalidationReason::None);
    CHECK(ovt::cache::check_index(index, meta, ovt::fb::Codec::H265) == InvalidationReason::None);
    CHECK(ovt::cache::check_index(index, meta, ovt::fb::Codec::AV1) == InvalidationReason::Index);
    Meta m = meta;
    m.codec = "H264";
    CHECK(ovt::cache::check_index(index, m, std::nullopt) == InvalidationReason::Index);
    const std::vector<std::uint8_t> garbage(64, 0x33);
    CHECK(ovt::cache::check_index(garbage, meta, std::nullopt) == InvalidationReason::Index);
  }
  SECTION("I5") {
    FrameSpec spec;
    spec.decode_index = 3;
    spec.plugin_id = "A";
    spec.plugin_version = "1.0";
    const auto frame = prefixed(make_frame(spec));
    CHECK(ovt::cache::check_frame(frame, 3, std::nullopt) == InvalidationReason::None);
    CHECK(ovt::cache::check_frame(frame, 3, SelectedPlugin{"A", "1.0"}) == InvalidationReason::None);
    CHECK(ovt::cache::check_frame(frame, 3, SelectedPlugin{"A", "1.1"}) == InvalidationReason::Frame);
    CHECK(ovt::cache::check_frame(frame, 3, SelectedPlugin{"B", "1.0"}) == InvalidationReason::Frame);
    CHECK(ovt::cache::check_frame(frame, 4, std::nullopt) == InvalidationReason::Frame);
    CHECK(ovt::cache::check_frame(std::span(frame).first(frame.size() / 2), 3, std::nullopt) ==
          InvalidationReason::Frame);
  }
  SECTION("I6") {
    FrameSpec spec;
    spec.with_recon_y_ref = true;  // recon/Y 64 x 32, 8-bit
    const auto frame = make_frame(spec);
    const auto* fa = flatbuffers::GetRoot<ovt::fb::FrameAnalysis>(frame.data());
    CHECK(ovt::cache::check_plane(*fa, ovt::fb::PlaneStage::Reconstruction, 0, std::size_t{64} * 32) ==
          InvalidationReason::None);
    CHECK(ovt::cache::check_plane(*fa, ovt::fb::PlaneStage::Reconstruction, 0, std::size_t{64} * 31) ==
          InvalidationReason::Plane);
    CHECK(ovt::cache::check_plane(*fa, ovt::fb::PlaneStage::Reconstruction, 1, std::size_t{64} * 32) ==
          InvalidationReason::Plane);
  }
  SECTION("I7") {
    CHECK(ovt::cache::check_ovt_version(meta) == InvalidationReason::OvtVersion);
  }
  CHECK(ovt::cache::to_string(InvalidationReason::SchemaMajor) == "I3 schema major");
}

// Acceptance test 1.
TEST_CASE("test_invalidate: AT1 an entry written with ovt_model major 2 is deleted and recreated (I3)",
          "[cache][fast]") {
  const TempRoot tmp("invalidate_at1");
  const SourceHash h = hash_of(0x30);
  populate(tmp, h);
  const fs::path dir = entry_dir(tmp, h);
  edit_meta(dir, [](Meta& m) { m.ovt_model.major = 2; });
  REQUIRE(fs::exists(dir / "frames" / "00000001.fa.zst"));

  auto cache = Cache::open(tmp.path());
  InvalidationReason reason = InvalidationReason::None;
  auto entry = cache->open_entry(h, ovt::fb::Codec::H265, SelectedPlugin{"A", "1.0"}, &reason);
  CHECK(reason == InvalidationReason::SchemaMajor);
  CHECK(fs::is_directory(dir));  // recreated
  CHECK_FALSE(fs::exists(dir / "frames"));
  CHECK_FALSE(fs::exists(dir / "index.fb"));
  CHECK_FALSE(entry->get_frame(1).has_value());
  CHECK(entry->meta().ovt_model.major == 1);
  const Meta on_disk = ovt::cache::parse_meta(ovt::cache::read_file(dir / "meta.json", kBig));
  CHECK(on_disk.ovt_model.major == 1);
  CHECK(on_disk.plugins.empty());
  CHECK(entry->bytes() == fs::file_size(dir / "meta.json"));
}

TEST_CASE("test_invalidate: open_entry deletes and recreates a directory failing I1, I2 or I4", "[cache][fast]") {
  const TempRoot tmp("invalidate_dirs");
  const SourceHash h = hash_of(0x40);
  populate(tmp, h);
  const fs::path dir = entry_dir(tmp, h);
  ovt::fb::Codec codec = ovt::fb::Codec::H265;
  InvalidationReason expected = InvalidationReason::None;

  SECTION("I2: meta.json is not JSON") {
    ovt::cache::write_file_atomic(dir / "meta.json", as_bytes("{ truncated"));
    expected = InvalidationReason::CacheFormat;
  }
  SECTION("I2: meta.json is missing") {
    fs::remove(dir / "meta.json");
    expected = InvalidationReason::CacheFormat;
  }
  SECTION("I2: cache_format 2") {
    std::string text =
        ovt::cache::serialize_meta(ovt::cache::parse_meta(ovt::cache::read_file(dir / "meta.json", kBig)));
    text.replace(text.find("\"cache_format\": 1"), 17, "\"cache_format\": 2");
    ovt::cache::write_file_atomic(dir / "meta.json", as_bytes(text));
    expected = InvalidationReason::CacheFormat;
  }
  SECTION("I1: meta.source_hash names another source") {
    edit_meta(dir, [](Meta& m) { m.source_hash = ovt::cache::hex(hash_of(0x41)); });
    expected = InvalidationReason::ForeignSourceHash;
  }
  SECTION("I4: index.fb does not verify") {
    ovt::cache::write_file_atomic(dir / "index.fb", std::vector<std::uint8_t>(128, 0x5A));
    expected = InvalidationReason::Index;
  }
  SECTION("I4: index codec differs from meta.codec") {
    edit_meta(dir, [](Meta& m) { m.codec = "AV1"; });
    expected = InvalidationReason::Index;
  }
  SECTION("I4: index codec differs from the stream's") {
    codec = ovt::fb::Codec::H264;
    expected = InvalidationReason::Index;
  }
  auto cache = Cache::open(tmp.path());
  InvalidationReason reason = InvalidationReason::None;
  auto entry = cache->open_entry(h, codec, std::nullopt, &reason);
  CHECK(reason == expected);
  CHECK_FALSE(fs::exists(dir / "frames"));
  CHECK_FALSE(entry->get_index().has_value());
  CHECK(ovt::cache::parse_meta(ovt::cache::read_file(dir / "meta.json", kBig)).source_hash == ovt::cache::hex(h));
}

TEST_CASE("test_invalidate: a valid entry, or one only of another ovt version (I7), is kept", "[cache][fast]") {
  const TempRoot tmp("invalidate_keep");
  const SourceHash h = hash_of(0x50);
  populate(tmp, h);
  const fs::path dir = entry_dir(tmp, h);
  const bool other_version = GENERATE(false, true);
  if (other_version) {
    edit_meta(dir, [](Meta& m) { m.ovt_version = "0.0.1+gdeadbee"; });
  }
  auto cache = Cache::open(tmp.path());
  InvalidationReason reason = InvalidationReason::Frame;
  auto entry = cache->open_entry(h, ovt::fb::Codec::H265, SelectedPlugin{"A", "1.0"}, &reason);
  CHECK(reason == InvalidationReason::None);
  CHECK(entry->invalidating());
  CHECK(entry->selected_plugin() == SelectedPlugin{"A", "1.0"});
  CHECK(entry->get_index().has_value());
  CHECK(entry->get_frame(2).has_value());
}

// Acceptance test 2.
TEST_CASE("test_invalidate: AT2 a frame of plugin A 1.0 read with A 1.1 is deleted and misses (I5)", "[cache][fast]") {
  const TempRoot tmp("invalidate_at2");
  const SourceHash h = hash_of(0x60);
  populate(tmp, h, "1.0");
  const fs::path frames = entry_dir(tmp, h) / "frames";
  auto cache = Cache::open(tmp.path());
  auto entry = cache->open_entry(h, ovt::fb::Codec::H265, SelectedPlugin{"A", "1.1"});
  const std::uint64_t before = entry->bytes();
  const std::uint64_t frame_size = fs::file_size(frames / "00000002.fa.zst");

  CHECK_FALSE(entry->get_frame(2).has_value());  // "miss"
  CHECK_FALSE(fs::exists(frames / "00000002.fa.zst"));
  for (const char* other : {"00000000.fa.zst", "00000001.fa.zst", "00000003.fa.zst"}) {
    CHECK(fs::exists(frames / other));  // untouched
  }
  CHECK(entry->bytes() == before - frame_size);
  REQUIRE(entry->meta().plugins.size() == 1);
  CHECK(entry->meta().plugins[0].frames == 3);
  // The frame is re-analysed by the selected plugin and then hits.
  FrameSpec spec;
  spec.decode_index = 2;
  spec.plugin_id = "A";
  spec.plugin_version = "1.1";
  entry->put_frame(2, make_frame(spec));
  CHECK(entry->get_frame(2) == make_frame(spec));
}

TEST_CASE("test_invalidate: an undecodable frame file is deleted on an invalidating read (I5)", "[cache][fast]") {
  const TempRoot tmp("invalidate_frame_corrupt");
  const SourceHash h = hash_of(0x61);
  populate(tmp, h);
  const fs::path frame = entry_dir(tmp, h) / "frames" / "00000001.fa.zst";
  const auto bytes = ovt::cache::read_file(frame, kBig);
  ovt::cache::write_file_atomic(frame, std::span<const std::uint8_t>(bytes).first(bytes.size() / 2));
  auto cache = Cache::open(tmp.path());
  SECTION("raw entry() keeps reporting OVT_E_CORRUPT and deletes nothing") {
    auto entry = cache->entry(h);
    CHECK_FALSE(entry->invalidating());
    CHECK_THROWS_AS(entry->get_frame(1), CacheError);
    CHECK(fs::exists(frame));
  }
  SECTION("open_entry() deletes the file and misses") {
    auto entry = cache->open_entry(h, ovt::fb::Codec::H265, std::nullopt);
    CHECK_FALSE(entry->get_frame(1).has_value());
    CHECK_FALSE(fs::exists(frame));
    CHECK(entry->get_frame(0).has_value());
  }
}

TEST_CASE("test_invalidate: a plane without PlaneRef or of the wrong size is deleted (I6)", "[cache][fast]") {
  const TempRoot tmp("invalidate_planes");
  const SourceHash h = hash_of(0x70);
  populate(tmp, h);  // frame 0 carries PlaneRef recon/Y 64 x 32; frame 1 has none
  std::vector<std::uint8_t> samples(std::size_t{64} * 32, 0x80);
  ovt::model::PlaneView view;
  view.data = samples.data();
  view.width = 64;
  view.height = 32;
  view.stride_bytes = 64;
  view.bit_depth = 8;
  view.sample_bytes = 1;
  view.format = ovt::model::SampleFormat::U8;
  const fs::path planes = entry_dir(tmp, h) / "planes";
  {
    auto cache = Cache::open(tmp.path());
    auto entry = cache->entry(h);
    entry->put_plane(1, ovt::fb::PlaneStage::Reconstruction, 0, view);  // no PlaneRef in frame 1
    view.height = 16;                                                   // 64 x 16: wrong size for frame 0
    entry->put_plane(0, ovt::fb::PlaneStage::Reconstruction, 0, view);
  }
  auto cache = Cache::open(tmp.path());
  auto entry = cache->open_entry(h, ovt::fb::Codec::H265, SelectedPlugin{"A", "1.0"});
  CHECK_FALSE(entry->get_plane(1, ovt::fb::PlaneStage::Reconstruction, 0).has_value());
  CHECK_FALSE(fs::exists(planes / "00000001" / "recon-y.pl.zst"));
  CHECK_FALSE(entry->get_plane(0, ovt::fb::PlaneStage::Reconstruction, 0).has_value());
  CHECK_FALSE(fs::exists(planes / "00000000" / "recon-y.pl.zst"));
  CHECK(entry->get_frame(0).has_value());  // the frame stays
  // A plane whose frame file is gone is garbage too.
  REQUIRE(fs::remove(entry_dir(tmp, h) / "frames" / "00000003.fa.zst"));
  entry->put_plane(3, ovt::fb::PlaneStage::Reconstruction, 0, view);
  CHECK_FALSE(entry->get_plane(3, ovt::fb::PlaneStage::Reconstruction, 0).has_value());
  CHECK_FALSE(fs::exists(planes / "00000003" / "recon-y.pl.zst"));
  // Regenerated with the right geometry, the plane reads back.
  view.height = 32;
  entry->put_plane(0, ovt::fb::PlaneStage::Reconstruction, 0, view);
  const auto got = entry->get_plane(0, ovt::fb::PlaneStage::Reconstruction, 0);
  REQUIRE(got.has_value());
  CHECK(got->bytes == samples);
}

TEST_CASE("test_invalidate: open entries are not re-checked; read-only caches refuse a failing entry",
          "[cache][fast]") {
  const TempRoot tmp("invalidate_open");
  const SourceHash h = hash_of(0x80);
  populate(tmp, h);
  const fs::path dir = entry_dir(tmp, h);
  SECTION("open") {
    auto cache = Cache::open(tmp.path());
    auto held = cache->entry(h);
    edit_meta(dir, [](Meta& m) { m.ovt_model.major = 2; });
    InvalidationReason reason = InvalidationReason::Frame;
    auto again = cache->open_entry(h, ovt::fb::Codec::H265, std::nullopt, &reason);
    CHECK(reason == InvalidationReason::None);
    CHECK(again == held);
    CHECK(fs::exists(dir / "frames" / "00000001.fa.zst"));
  }
  SECTION("read-only") {
    edit_meta(dir, [](Meta& m) { m.ovt_model.major = 2; });
    Options ro;
    ro.read_only = true;
    auto cache = Cache::open(tmp.path(), ro);
    CHECK_THROWS_AS(cache->open_entry(h, ovt::fb::Codec::H265, std::nullopt), CacheError);
    CHECK(fs::exists(dir / "frames" / "00000001.fa.zst"));
  }
}

TEST_CASE("test_invalidate: last_used is written at most once per minute while open", "[cache][fast]") {
  const TempRoot tmp("invalidate_last_used");
  const SourceHash h = hash_of(0x90);
  populate(tmp, h);
  const auto t0 = std::chrono::sys_days{std::chrono::year{2030} / 1 / 1} + std::chrono::hours(12);
  ovt::cache::set_now_for_testing(t0);
  {
    auto cache = Cache::open(tmp.path());
    auto entry = cache->open_entry(h, ovt::fb::Codec::H265, std::nullopt);
    CHECK(entry->meta().last_used == "2030-01-01T12:00:00Z");
    ovt::cache::set_now_for_testing(t0 + std::chrono::seconds(59));
    (void)entry->get_frame(0);
    CHECK(entry->meta().last_used == "2030-01-01T12:00:00Z");
    ovt::cache::set_now_for_testing(t0 + std::chrono::seconds(60));
    (void)entry->get_frame(0);
    CHECK(entry->meta().last_used == "2030-01-01T12:01:00Z");
    ovt::cache::set_now_for_testing(t0 + std::chrono::seconds(90));
    (void)entry->get_index();
    CHECK(entry->meta().last_used == "2030-01-01T12:01:00Z");
  }
  ovt::cache::set_now_for_testing(std::nullopt);
  const Meta on_disk = ovt::cache::parse_meta(ovt::cache::read_file(entry_dir(tmp, h) / "meta.json", kBig));
  CHECK(on_disk.last_used == "2030-01-01T12:01:00Z");
}
