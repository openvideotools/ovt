// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the virtual file, the segment cache and the fetchers (chapter 08 §8.6.7, WI-4.210a). The packages are
// synthesised (no corpus needed); the HTTP tests serve them with `python3 -I -m http.server` on 127.0.0.1 and skip
// themselves when that cannot start.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "container/virtual_file/fetcher.hpp"
#include "container/virtual_file/segment_cache.hpp"
#include "container/virtual_file/virtual_file.hpp"
#include "virtual_file_fixture.hpp"

namespace {

using namespace ovt::test::vfile;  // NOLINT(google-build-using-namespace): the fixtures of this directory

}  // namespace

TEST_CASE("sha256_hex matches the FIPS 180-4 vectors", "[container][virtual_file]") {
  CHECK(vf::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(vf::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(vf::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");  // 56 bytes: padding spills a block
  CHECK(vf::sha256_hex(std::string(1000, 'a')) == vf::sha256_hex(std::string(1000, 'a')));
  CHECK(vf::sha256_hex(std::string(1'000'000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SegmentCache keys on the URL and evicts the least recently used file", "[container][virtual_file]") {
  CHECK(vf::SegmentCache::key("http://x/a", {}) == vf::sha256_hex("http://x/a"));
  CHECK(vf::SegmentCache::key("http://x/a", {10, 20}) != vf::SegmentCache::key("http://x/a", {10, 21}));

  const TempDir t("cache");
  vf::SegmentCache cache(t.path / "segments", 250);
  const Bytes hundred(100, 7);
  const auto pa = cache.store("a", hundred);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  cache.store("b", hundred);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(cache.lookup("a"));  // refreshes a: b is now the oldest
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  cache.store("c", hundred);  // 300 bytes > 250: evicts b
  CHECK(cache.lookup("a"));
  CHECK_FALSE(cache.lookup("b"));
  CHECK(cache.lookup("c"));
  CHECK(cache.total_bytes() == 200);
  CHECK(fs::exists(pa));
  CHECK_FALSE(cache.lookup("never stored"));
}

TEST_CASE("VirtualFile lays segments out and reads across their boundaries", "[container][virtual_file]") {
  const TempDir t("layout");
  const Bytes all = write_dash_package(t.path);
  std::vector<vf::SegmentSpec> specs{{(t.path / "init.mp4").string(), {}, true, "p0", 0}};
  for (uint32_t n = 1; n <= kSegments; ++n) {
    specs.push_back({(t.path / ("seg" + std::to_string(n) + ".m4s")).string(), {}, false, "p0", 0});
  }
  const auto file =
      vf::VirtualFile::create(specs, std::shared_ptr<vf::Fetcher>(vf::make_local_fetcher()), fresh_cache(t), "test");
  REQUIRE(file->size() == all.size());
  CHECK(read_all(*file) == all);
  CHECK_FALSE(file->is_mapped());

  const auto& map = file->segment_map();
  REQUIRE(map.size() == 5);
  CHECK(map[0].is_init);
  CHECK(map[0].segment_index == 0);
  CHECK(map[3].segment_index == 3);
  CHECK(map[3].logical_offset == map[2].logical_offset + map[2].length);

  // Reads that straddle every boundary, byte by byte around it, agree with the concatenation.
  for (const auto& e : map) {
    for (const int64_t d : {-5, -1, 0, 1, 5}) {
      const int64_t start = static_cast<int64_t>(e.logical_offset) + d - 7;
      if (start < 0) {
        continue;
      }
      Bytes got(20);
      const size_t n = file->read(static_cast<uint64_t>(start), got.size(), got.data());
      const size_t want = std::min<size_t>(20, all.size() - static_cast<size_t>(start));
      REQUIRE(n == want);
      CHECK(std::equal(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(n), all.begin() + start));
    }
  }
  CHECK(file->read(all.size(), 10, nullptr) == 0);
  CHECK(file->read(all.size() + 100, 10, nullptr) == 0);

  const auto holder = file->view(map[2].logical_offset + 3, 50);
  REQUIRE(holder.bytes.size() == 50);
  CHECK(std::equal(holder.bytes.begin(), holder.bytes.end(),
                   all.begin() + static_cast<std::ptrdiff_t>(map[2].logical_offset + 3)));

  // Provenance: an offset inside segment 3 reports segment_index 3 and the offset within it.
  const uint64_t inside = map[3].logical_offset + 17;
  const auto j = nlohmann::json::parse(*file->provenance_json(inside));
  CHECK(j["kind"] == "segment");
  CHECK(j["segment_index"] == 3);
  CHECK(j["offset_in_segment"] == 17);
  CHECK(j["length"] == map[3].length);
  CHECK(j["url"] == map[3].url);
  CHECK(j["period_id"] == "p0");
  CHECK_FALSE(file->provenance_json(all.size()));
  CHECK(nlohmann::json::parse(file->segment_map_json()).size() == 5);
}

TEST_CASE("VirtualFile honours byte ranges and fetches segments of unknown size", "[container][virtual_file]") {
  const TempDir t("ranges");
  Bytes whole(1000);
  for (size_t i = 0; i < whole.size(); ++i) {
    whole[i] = static_cast<uint8_t>(i * 7);
  }
  write_file(t.path / "all.mp4", whole);
  const std::string url = (t.path / "all.mp4").string();
  const auto cache = fresh_cache(t);
  // Two ranges of one resource must not share a cache file; the third segment is "the rest".
  const auto file = vf::VirtualFile::create(
      {{url, {0, 100}, true, "", 0}, {url, {100, 300}, false, "", 0}, {url, {400, 0}, false, "", 0}},
      std::shared_ptr<vf::Fetcher>(vf::make_local_fetcher()), cache, "ranges");
  REQUIRE(file->size() == 100 + 300 + 600);
  CHECK(read_all(*file) == whole);
  CHECK(file->segment_map()[1].byte_range.offset == 100);
  CHECK(file->segment_map()[2].length == 600);
}

TEST_CASE("VirtualFile reports a missing segment as an I/O error", "[container][virtual_file]") {
  const TempDir t("missing");
  CHECK_THROWS_AS(vf::VirtualFile::create({{(t.path / "nope.m4s").string(), {}, false, "", 0}},
                                          std::shared_ptr<vf::Fetcher>(vf::make_local_fetcher()), fresh_cache(t), "x"),
                  ovt::io::IoError);
}

TEST_CASE("OVT_OFFLINE=1 refuses http fetches", "[container][virtual_file]") {
#ifndef _WIN32
  setenv("OVT_OFFLINE", "1", 1);
  auto fetcher = vf::make_default_fetcher();
  CHECK_THROWS_AS(fetcher->fetch("http://127.0.0.1:9/seg1.m4s", {}), ovt::io::IoError);
  unsetenv("OVT_OFFLINE");
#endif
  CHECK(vf::is_http_url("HTTPS://example.org/a.mpd"));
  CHECK_FALSE(vf::is_http_url("/tmp/a.mpd"));
  CHECK(vf::local_path_of("file:///tmp/a.mpd") == "/tmp/a.mpd");
}

TEST_CASE("the HTTP fetcher serves whole files, byte ranges and sizes", "[container][virtual_file][http]") {
  unsetenv("OVT_OFFLINE");
  const TempDir t("http");
  const Bytes all = write_dash_package(t.path);
  const HttpServer server(t.path);
  if (!server.up()) {
    SKIP("python3 http.server did not start");
  }
  const auto fetcher = vf::make_default_fetcher();
  CHECK(*fetcher->size(server.base() + "seg1.m4s") == fs::file_size(t.path / "seg1.m4s"));
  CHECK(fetcher->fetch(server.base() + "seg1.m4s", {}).size() == fs::file_size(t.path / "seg1.m4s"));
  // The server may answer 206 or, like python's, 200 with the whole body: both give exactly the range.
  const Bytes ranged = fetcher->fetch(server.base() + "seg1.m4s", {8, 20});
  const Bytes local = vf::make_local_fetcher()->fetch((t.path / "seg1.m4s").string(), {8, 20});
  CHECK(ranged == local);
  CHECK(fetcher->fetch(server.base() + "seg1.m4s", {8, 0}).size() == fs::file_size(t.path / "seg1.m4s") - 8);
  CHECK_THROWS_AS(fetcher->fetch(server.base() + "absent.m4s", {}), ovt::io::IoError);
  CHECK_THROWS_AS(fetcher->size(server.base() + "absent.m4s"), ovt::io::IoError);

  // A virtual file over http reads the same bytes as the files on disk.
  std::vector<vf::SegmentSpec> specs{{server.base() + "init.mp4", {}, true, "p0", 0}};
  for (uint32_t n = 1; n <= kSegments; ++n) {
    specs.push_back({server.base() + "seg" + std::to_string(n) + ".m4s", {}, false, "p0", 0});
  }
  const TempDir cache_dir("httpcache");
  const auto file = vf::VirtualFile::create(specs, std::shared_ptr<vf::Fetcher>(vf::make_default_fetcher()),
                                            fresh_cache(cache_dir), "http");
  CHECK(read_all(*file) == all);
  CHECK(fs::exists(cache_dir.path / "cache" / "segments" / vf::sha256_hex(server.base() + "seg2.m4s")));
}
