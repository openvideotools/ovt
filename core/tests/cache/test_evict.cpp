// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.092 acceptance test 3 and the size-cap policy of chapter 06 §6.7.4: eviction order, the open-entry
// exclusion, the "all entries open" warning MDL-CCH-0002, eviction after a write, and Cache::clear() /
// Cache::list() of `ovt cache` (chapter 12 §12.2.13). Large entries are sparse files: the cache accounts
// file sizes, so a 40 MB entry costs no disk space.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "cache/atomic_file.hpp"
#include "cache/cache_json.hpp"
#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/status.h"
#include "test_helpers.hpp"

namespace {

namespace fs = std::filesystem;
using ovt::cache::Cache;
using ovt::cache::CacheError;
using ovt::cache::ClearFilter;
using ovt::cache::Meta;
using ovt::cache::Options;
using ovt::cache::SourceHash;
using ovt::cache::test::FrameSpec;
using ovt::cache::test::hash_of;
using ovt::cache::test::make_frame;
using ovt::cache::test::make_index;
using ovt::cache::test::TempRoot;

constexpr std::size_t kBig = 1U << 30U;
constexpr std::uint64_t kMB = 1'000'000;

// A file of @p size bytes that occupies no disk space (sparse).
void sized_file(const fs::path& p, std::uint64_t size) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).close();
  fs::resize_file(p, size);
}

void set_last_used(const fs::path& dir, const std::string& when) {
  Meta meta = ovt::cache::parse_meta(ovt::cache::read_file(dir / "meta.json", kBig));
  meta.last_used = when;
  const std::string text = ovt::cache::serialize_meta(meta);
  ovt::cache::write_file_atomic(dir / "meta.json", {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

// Closed entries of exactly @p bytes each under @p root: planes/ holds @p planes bytes, frames/ the rest after
// meta.json. Entry i has last_used 2026-01-0<i+1>; cache.json is rebuilt from the directories (it is removed,
// the one case where §6.7.4 allows the walk). The cap is left at 1 TB.
std::vector<SourceHash> make_entries(const fs::path& root, int count, std::uint64_t bytes, std::uint64_t planes) {
  std::vector<SourceHash> hashes;
  Options big;
  big.max_bytes = 1'000'000 * kMB;
  {
    auto cache = Cache::open(root, big);
    for (int i = 0; i < count; ++i) {
      hashes.push_back(hash_of(static_cast<std::uint8_t>(0xA0 + 0x10 * i)));
      (void)cache->entry(hashes.back());
    }
  }
  for (int i = 0; i < count; ++i) {
    const fs::path dir = root / ovt::cache::hex(hashes[static_cast<std::size_t>(i)]);
    set_last_used(dir, "2026-01-0" + std::to_string(i + 1) + "T00:00:00Z");
    sized_file(dir / "planes" / "00000000" / "recon-y.pl.zst", planes);
    sized_file(dir / "frames" / "00000000.fa.zst", bytes - planes - fs::file_size(dir / "meta.json"));
  }
  fs::remove(root / "cache.json");
  return hashes;
}

ovt::cache::CacheJson read_cache_json(const fs::path& root) {
  auto doc = ovt::cache::parse_cache_json(ovt::cache::read_file(root / "cache.json", kBig));
  REQUIRE(doc.has_value());
  return *doc;
}

// Collects the diagnostic codes logged while alive.
class CodeLog {
 public:
  CodeLog() {
    ovt::log::set_sink([this](ovt::log::Level, std::string_view, std::string_view code, std::string_view) {
      if (!code.empty()) {
        codes_.emplace_back(code);
      }
    });
  }
  ~CodeLog() { ovt::log::set_sink({}); }
  CodeLog(const CodeLog&) = delete;
  CodeLog& operator=(const CodeLog&) = delete;
  CodeLog(CodeLog&&) = delete;
  CodeLog& operator=(CodeLog&&) = delete;
  [[nodiscard]] long count(std::string_view code) const { return std::ranges::count(codes_, code); }

 private:
  std::vector<std::string> codes_;
};

ClearFilter older_than(std::uint32_t days) {
  ClearFilter f;
  f.older_than_days = days;
  return f;
}

ClearFilter of_source(const SourceHash& source) {
  ClearFilter f;
  f.source = source;
  return f;
}

ClearFilter of_kind(const std::string& kind) {
  ClearFilter f;
  f.kind = kind;
  return f;
}

}  // namespace

// Acceptance test 3. The root is a fixed directory so that the CTest test cache_json_schema (acceptance test 4,
// validate_json_schema.py) can validate the cache.json it leaves behind.
TEST_CASE("test_evict: AT3 cap 100 MB and five 40 MB entries evict the oldest to at most 90 MB", "[cache][fast]") {
  const fs::path root = ovt::cache::test::fixed_dir("at3");
  fs::remove_all(root);
  const std::vector<SourceHash> h = make_entries(root, 5, 40 * kMB, 36 * kMB);
  auto cache = Cache::open(root, Options{.max_bytes = 1'000'000 * kMB});
  REQUIRE(cache->total_bytes() == 200 * kMB);
  cache->set_max_bytes(100 * kMB);

  const ovt::cache::EvictionResult result = cache->enforce_cap();
  CHECK(result.bytes_before == 200 * kMB);
  // Oldest first, planes/ before the whole directory: entries 0 and 1 go, entry 2 loses planes/ and the total
  // (4 + 40 + 40 MB) is then at most 0.9 x cap.
  CHECK(result.removed == std::vector<SourceHash>{h[0], h[1]});
  CHECK(result.trimmed == std::vector<SourceHash>{h[2]});
  CHECK(result.bytes_after == 84 * kMB);
  CHECK(result.bytes_after <= 90 * kMB);
  CHECK_FALSE(result.all_open);
  CHECK(cache->total_bytes() == 84 * kMB);
  CHECK_FALSE(fs::exists(root / ovt::cache::hex(h[0])));
  CHECK_FALSE(fs::exists(root / ovt::cache::hex(h[1])));
  CHECK_FALSE(fs::exists(root / ovt::cache::hex(h[2]) / "planes"));
  CHECK(fs::exists(root / ovt::cache::hex(h[2]) / "frames" / "00000000.fa.zst"));
  CHECK(fs::exists(root / ovt::cache::hex(h[4]) / "planes" / "00000000" / "recon-y.pl.zst"));

  // cache.json reflects it.
  const ovt::cache::CacheJson doc = read_cache_json(root);
  CHECK(doc.size_cap_bytes == 100 * kMB);
  REQUIRE(doc.records.size() == 3);
  CHECK(doc.records.at(ovt::cache::hex(h[2])).bytes == 4 * kMB);
  CHECK(doc.records.at(ovt::cache::hex(h[3])).bytes == 40 * kMB);
  CHECK(doc.records.at(ovt::cache::hex(h[4])).bytes == 40 * kMB);
  CHECK(doc.records.at(ovt::cache::hex(h[3])).last_used == "2026-01-04T00:00:00Z");
  // A second run has nothing to do.
  CHECK(cache->enforce_cap().bytes_after == 84 * kMB);
}

TEST_CASE("test_evict: open entries are never evicted; eviction also runs at open", "[cache][fast]") {
  const TempRoot tmp("evict_open");
  const std::vector<SourceHash> h = make_entries(tmp.path(), 3, 40 * kMB, 36 * kMB);
  SECTION("the oldest entry is open: the next oldest is trimmed") {
    auto cache = Cache::open(tmp.path(), Options{.max_bytes = 1'000'000 * kMB});
    auto held = cache->entry(h[0]);
    cache->set_max_bytes(100 * kMB);
    const auto result = cache->enforce_cap();
    CHECK(result.removed.empty());
    CHECK(result.trimmed == std::vector<SourceHash>{h[1]});
    CHECK(result.bytes_after == 84 * kMB);
    CHECK(fs::exists(tmp.path() / ovt::cache::hex(h[0]) / "planes"));
  }
  SECTION("Cache::open with a smaller cap evicts at startup") {
    auto cache = Cache::open(tmp.path(), Options{.max_bytes = 100 * kMB});
    CHECK(cache->total_bytes() == 84 * kMB);
    CHECK_FALSE(fs::exists(tmp.path() / ovt::cache::hex(h[0]) / "planes"));
  }
}

TEST_CASE("test_evict: with every entry open the cap is exceeded and MDL-CCH-0002 is logged once", "[cache][fast]") {
  const TempRoot tmp("evict_all_open");
  const std::vector<SourceHash> h = make_entries(tmp.path(), 3, 40 * kMB, 36 * kMB);
  const CodeLog log;
  auto cache = Cache::open(tmp.path(), Options{.max_bytes = 1'000'000 * kMB});
  std::vector<std::shared_ptr<ovt::cache::Entry>> held;
  held.reserve(h.size());
  for (const SourceHash& s : h) {
    held.push_back(cache->entry(s));
  }
  cache->set_max_bytes(100 * kMB);
  const auto first = cache->enforce_cap();
  CHECK(first.all_open);
  CHECK(first.removed.empty());
  CHECK(first.trimmed.empty());
  CHECK(first.bytes_after == 120 * kMB);
  CHECK(cache->enforce_cap().all_open);
  CHECK(log.count("MDL-CCH-0002") == 1);
  // Once an entry closes, the next run frees space.
  held.erase(held.begin());
  const auto after_close = cache->enforce_cap();
  CHECK_FALSE(after_close.all_open);
  CHECK(after_close.trimmed == std::vector<SourceHash>{h[0]});
}

TEST_CASE("test_evict: a write that pushes the total above the cap evicts closed entries", "[cache][fast]") {
  const TempRoot tmp("evict_on_write");
  const std::vector<SourceHash> h = make_entries(tmp.path(), 1, 2 * kMB, 2 * kMB - 100'000);
  auto cache = Cache::open(tmp.path(), Options{.max_bytes = 3 * kMB});
  REQUIRE(fs::exists(tmp.path() / ovt::cache::hex(h[0]) / "planes"));
  auto entry = cache->entry(hash_of(0x01));
  std::vector<std::uint8_t> noise(1'500'000);
  std::mt19937 rng(7);  // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic test data
  for (auto& b : noise) {
    b = static_cast<std::uint8_t>(rng());
  }
  entry->put_report("verify", noise);  // incompressible: about 1.5 MB on disk
  CHECK_FALSE(fs::exists(tmp.path() / ovt::cache::hex(h[0]) / "planes"));
  CHECK(cache->total_bytes() <= 3 * kMB);
}

TEST_CASE("test_evict: clear filters by source, age and kind and skips open entries", "[cache][fast]") {
  const TempRoot tmp("evict_clear");
  const SourceHash a = hash_of(0x10);  // index + frames, used 2026-01-01
  const SourceHash b = hash_of(0x20);  // frames only, used 2026-02-25
  const SourceHash c = hash_of(0x30);  // held open
  {
    auto cache = Cache::open(tmp.path());
    auto ea = cache->entry(a);
    ea->put_index(make_index(a, 1));
    ea->put_frame(0, make_frame(FrameSpec{}));
    cache->entry(b)->put_frame(0, make_frame(FrameSpec{}));
    (void)cache->entry(c);
  }
  set_last_used(tmp.path() / ovt::cache::hex(a), "2026-01-01T00:00:00Z");
  set_last_used(tmp.path() / ovt::cache::hex(b), "2026-02-25T00:00:00Z");
  fs::remove(tmp.path() / "cache.json");
  ovt::cache::set_now_for_testing(std::chrono::sys_days{std::chrono::year{2026} / 3 / 1});
  auto cache = Cache::open(tmp.path());
  auto held = cache->entry(c);

  SECTION("everything") {
    const auto result = cache->clear({});
    CHECK(result.removed == std::vector<SourceHash>{a, b});
    CHECK(result.skipped_open == std::vector<SourceHash>{c});
    CHECK(result.bytes_freed > 0);
    CHECK(cache->entries() == std::vector<SourceHash>{c});
    CHECK(read_cache_json(tmp.path()).records.size() == 1);
  }
  SECTION("older than 30 days") {
    const auto result = cache->clear(older_than(30));
    CHECK(result.removed == std::vector<SourceHash>{a});
    CHECK(fs::exists(tmp.path() / ovt::cache::hex(b)));
  }
  SECTION("one source") {
    const auto result = cache->clear(of_source(b));
    CHECK(result.removed == std::vector<SourceHash>{b});
    CHECK(fs::exists(tmp.path() / ovt::cache::hex(a)));
  }
  SECTION("kind analysis") {
    const auto result = cache->clear(of_kind("analysis"));
    CHECK(result.trimmed == std::vector<SourceHash>{a});  // keeps index.fb
    CHECK(result.removed == std::vector<SourceHash>{b});  // nothing but meta.json was left
    const fs::path dir = tmp.path() / ovt::cache::hex(a);
    CHECK(fs::exists(dir / "index.fb"));
    CHECK_FALSE(fs::exists(dir / "frames"));
    CHECK(ovt::cache::parse_meta(ovt::cache::read_file(dir / "meta.json", kBig)).plugins.empty());
    CHECK(read_cache_json(tmp.path()).records.at(ovt::cache::hex(a)).bytes ==
          fs::file_size(dir / "index.fb") + fs::file_size(dir / "meta.json"));
  }
  SECTION("errors") {
    CHECK_THROWS_AS(cache->clear(of_kind("planes")), CacheError);
    Options ro;
    ro.read_only = true;
    CHECK_THROWS_AS(Cache::open(tmp.path(), ro)->clear({}), CacheError);
  }
  ovt::cache::set_now_for_testing(std::nullopt);
}

TEST_CASE("test_evict: list re-walks the entries and reports one row per kind", "[cache][fast]") {
  const TempRoot tmp("evict_list");
  const SourceHash h = hash_of(0x44);
  {
    auto cache = Cache::open(tmp.path());
    auto entry = cache->entry(h);
    entry->set_source_info("clip.mkv", 1234);
    entry->put_index(make_index(h, 1));
    entry->put_frame(0, make_frame(FrameSpec{}));
    const std::vector<std::uint8_t> report(64, 0x42);
    entry->put_report("metrics", report);
    entry->put_report("hdr", report);
  }
  const fs::path dir = tmp.path() / ovt::cache::hex(h);
  sized_file(dir / "maps" / "vmaf" / "00000000-y.pl.zst", 5000);  // written behind the cache's back
  auto cache = Cache::open(tmp.path());
  const std::uint64_t before = cache->total_bytes();
  const auto rows = cache->list();
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].kind == "analysis");
  CHECK(rows[1].kind == "index");
  CHECK(rows[2].kind == "metrics");
  CHECK(rows[0].size_bytes ==
        fs::file_size(dir / "frames" / "00000000.fa.zst") + fs::file_size(dir / "reports" / "hdr.fb.zst"));
  CHECK(rows[1].size_bytes == fs::file_size(dir / "index.fb"));
  CHECK(rows[2].size_bytes == 5000 + fs::file_size(dir / "reports" / "metrics.fb.zst"));
  CHECK(rows[0].key == h);
  CHECK(rows[0].source_name == "clip.mkv");
  CHECK_FALSE(rows[0].created.empty());
  CHECK(cache->total_bytes() == before + 5000);  // the walk corrected cache.json
  CHECK(read_cache_json(tmp.path()).records.at(ovt::cache::hex(h)).bytes == before + 5000);
}

TEST_CASE("test_evict: kind_of maps entry files to index, analysis and metrics", "[cache][fast]") {
  using ovt::cache::kind_of;
  CHECK(kind_of("index.fb") == "index");
  CHECK(kind_of("container.fb.zst") == "index");
  CHECK(kind_of("frames/00000001.fa.zst") == "analysis");
  CHECK(kind_of("planes/00000001/recon-y.pl.zst") == "analysis");
  CHECK(kind_of("reports/verify.fb.zst") == "analysis");
  CHECK(kind_of("reports/metrics.fb.zst") == "metrics");
  CHECK(kind_of("maps/vmaf/00000001-y.pl.zst") == "metrics");
  CHECK(kind_of("metrics/0123456789abcdef.fm.zst") == "metrics");
  CHECK_FALSE(kind_of("meta.json").has_value());
  CHECK_FALSE(kind_of("frames/00000001.fa.zst.tmp").has_value());
}

TEST_CASE("test_evict: set_max_bytes is recorded in cache.json and survives a reopen", "[cache][fast]") {
  const TempRoot tmp("evict_cap");
  {
    auto cache = Cache::open(tmp.path());
    cache->set_max_bytes(123 * kMB);
  }
  CHECK(read_cache_json(tmp.path()).size_cap_bytes == 123 * kMB);
  auto cache = Cache::open(tmp.path());
  CHECK(cache->max_bytes() == 123 * kMB);
  cache->set_max_bytes(0);
  CHECK(cache->max_bytes() == ovt::cache::kDefaultMaxBytes);
}
