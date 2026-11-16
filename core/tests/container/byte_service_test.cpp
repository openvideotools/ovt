// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the byte service (chapter 08 §8.8, WI-1.234): the page cache, ovt_container_bytes,
// ovt_container_box_at, ovt_container_stats_json and the internal unit-bytes path behind ovt_stream_unit_bytes.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "container/byte_service.h"
#include "container/container.hpp"
#include "container/page_cache.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"

namespace {

using Bytes = std::vector<uint8_t>;
using ovt::container::PageCache;
using ovt::container::UnitCodec;

constexpr uint64_t kGiB = 1ULL << 30U;
constexpr uint64_t kMiB = 1ULL << 20U;

// The byte at file offset i of the procedural files below.
uint8_t pattern(uint64_t i) {
  return static_cast<uint8_t>((i * 131U) ^ (i >> 16U));
}

// A file of any size whose bytes are computed on read; counts the preads served.
class PatternFile final : public ovt::io::RandomAccessFile {
 public:
  explicit PatternFile(uint64_t size) : RandomAccessFile(ovt::io::FileInfo{size, 0, "pattern"}) {}
  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset >= size()) {
      return 0;
    }
    const auto n = static_cast<size_t>(std::min<uint64_t>(len, size() - offset));
    for (size_t i = 0; i < n; ++i) {
      dst[i] = pattern(offset + i);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    reads_.fetch_add(1);
    return n;
  }
  [[nodiscard]] Holder view(uint64_t /*offset*/, size_t /*len*/) const override { return Holder{nullptr, {}}; }
  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*a*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return false; }
  [[nodiscard]] uint64_t reads() const noexcept { return reads_.load(); }

 private:
  mutable std::atomic<uint64_t> reads_{0};
};

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OVT_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define OVT_TEST_SANITIZED 1
#endif
#endif

// Resident set size in bytes; 0 where /proc is not available or a sanitizer's shadow memory makes RSS meaningless
// (the RSS assertion is then skipped).
uint64_t rss_bytes() {
#ifdef OVT_TEST_SANITIZED
  return 0;
#endif
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) {
    return 0;
  }
  unsigned long long total = 0;
  unsigned long long resident = 0;
  const int got = std::fscanf(f, "%llu %llu", &total, &resident);  // NOLINT(cert-err34-c): two fixed integers
  std::fclose(f);
  return got == 2 ? resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) : 0;
}

// A container handle over `file` without a reader: enough for the byte service.
struct Handle {
  explicit Handle(std::unique_ptr<ovt::io::RandomAccessFile> f) : file{std::move(f)} {
    container = new ovt_Container{&file, ovt::container::Format::Ivf, nullptr, {}, {}};
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { ovt_container_close(container); }
  ovt_File file;
  ovt_Container* container = nullptr;
};

struct Buf {
  ovt_Buffer b{nullptr, 0};
  Buf() = default;
  Buf(const Buf&) = delete;
  Buf& operator=(const Buf&) = delete;
  // ovt_buffer_free() lives in the shared library; its release is this delete[] (api/buffer.cpp allocates with new[]).
  ~Buf() {
    delete[] b.data;  // NOLINT(cppcoreguidelines-owning-memory): rule 3a buffers are new[]
  }
  [[nodiscard]] Bytes bytes() const { return {b.data, b.data + b.size}; }
};

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}

// ftyp, moov{mvhd, trak{tkhd, mdia}}, mdat. Returns the file; *tkhd_offset is where the tkhd box starts.
Bytes make_mp4(uint64_t* tkhd_offset, uint64_t* moov_offset) {
  Bytes ftyp_payload;
  const std::string_view brand = "isom";
  ftyp_payload.insert(ftyp_payload.end(), brand.begin(), brand.end());
  put32(ftyp_payload, 0);
  ftyp_payload.insert(ftyp_payload.end(), brand.begin(), brand.end());
  const Bytes ftyp = box("ftyp", ftyp_payload);
  const Bytes mvhd = box("mvhd", Bytes(100, 0));
  const Bytes tkhd = box("tkhd", Bytes(84, 0));
  const Bytes mdia = box("mdia", box("mdhd", Bytes(24, 0)));
  const Bytes trak = box("trak", cat({tkhd, mdia}));
  const Bytes moov = box("moov", cat({mvhd, trak}));
  const Bytes mdat = box("mdat", Bytes(64, 0xAB));
  *moov_offset = ftyp.size();
  *tkhd_offset = ftyp.size() + 8 + mvhd.size() + 8;
  return cat({ftyp, moov, mdat});
}

}  // namespace

TEST_CASE("byte service: 10 000 random 16-byte reads of a 4 GiB file stay in the cache budget",
          "[container][bytes][fast]") {
  auto file = std::make_unique<PatternFile>(4 * kGiB);
  const PatternFile* raw = file.get();
  Handle h(std::move(file));
  Buf warm;
  REQUIRE(ovt_container_bytes(h.container, 0, 16, &warm.b) == OVT_OK);  // creates the cache before the baseline
  const uint64_t before = rss_bytes();

  std::mt19937_64 rng(1234);  // NOLINT(cert-msc32-c,cert-msc51-cpp): a fixed seed makes the case reproducible
  for (int i = 0; i < 10000; ++i) {
    const uint64_t offset = rng() % (4 * kGiB - 16);
    Buf out;
    REQUIRE(ovt_container_bytes(h.container, offset, 16, &out.b) == OVT_OK);
    REQUIRE(out.b.size == 16);
    for (size_t k = 0; k < 16; ++k) {
      REQUIRE(out.b.data[k] == pattern(offset + k));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }
  const uint64_t after = rss_bytes();
  if (before != 0) {
    CHECK(after < before + (8 * kMiB));
  }
  CHECK(h.container->bytes.cache(*h.file.file).resident_pages() <= PageCache::kCapacityPages);
  CHECK(raw->reads() <= 10001);  // never more than one pread per page fetched
}

TEST_CASE("byte service: sequential rows hit the cache at least 90 percent of the time", "[container][bytes][fast]") {
  Handle h(std::make_unique<PatternFile>(64 * kMiB));
  for (uint64_t offset = 0; offset < 8 * kMiB; offset += 16) {  // a hex view scrolling through 8 MiB, 16 per row
    Buf out;
    REQUIRE(ovt_container_bytes(h.container, offset, 16, &out.b) == OVT_OK);
  }
  Buf json;
  REQUIRE(ovt_container_stats_json(h.container, &json.b) == OVT_OK);
  const auto stats = nlohmann::json::parse(json.bytes());
  const double hits = stats.at("hits").get<double>();
  const auto misses = stats.at("misses").get<uint64_t>();
  CHECK(hits / (hits + static_cast<double>(misses)) >= 0.90);
  CHECK(misses == 8 * kMiB / PageCache::kPageSize);
  CHECK(stats.at("bytes_read").get<uint64_t>() == 8 * kMiB);
}

TEST_CASE("byte service: reads are clipped at the end of the file", "[container][bytes][fast]") {
  const uint64_t size = (3 * PageCache::kPageSize) + 123;  // a short last page
  Handle h(std::make_unique<PatternFile>(size));
  Buf tail;
  REQUIRE(ovt_container_bytes(h.container, size - 5, 16, &tail.b) == OVT_OK);
  REQUIRE(tail.b.size == 5);
  for (size_t k = 0; k < 5; ++k) {
    CHECK(tail.b.data[k] == pattern(size - 5 + k));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  Buf at_end;
  CHECK(ovt_container_bytes(h.container, size, 16, &at_end.b) == OVT_OK);
  CHECK(at_end.b.size == 0);
  Buf past;
  CHECK(ovt_container_bytes(h.container, size + (1ULL << 40U), 16, &past.b) == OVT_OK);
  CHECK(past.b.size == 0);
  Buf none;
  CHECK(ovt_container_bytes(h.container, 0, 0, &none.b) == OVT_OK);
  CHECK(none.b.size == 0);
}

TEST_CASE("byte service: a read across pages equals the file, and 1 MiB is the limit", "[container][bytes][fast]") {
  Handle h(std::make_unique<PatternFile>(4 * kMiB));
  const uint64_t offset = PageCache::kPageSize - 7;  // straddles pages 0 and 1
  Buf across;
  REQUIRE(ovt_container_bytes(h.container, offset, 100, &across.b) == OVT_OK);
  REQUIRE(across.b.size == 100);
  for (size_t k = 0; k < 100; ++k) {
    CHECK(across.b.data[k] == pattern(offset + k));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  Buf big;
  REQUIRE(ovt_container_bytes(h.container, 12345, static_cast<uint32_t>(kMiB), &big.b) == OVT_OK);
  REQUIRE(big.b.size == kMiB);
  CHECK(big.b.data[kMiB - 1] == pattern(12345 + kMiB - 1));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  Buf too_big;
  CHECK(ovt_container_bytes(h.container, 0, static_cast<uint32_t>(kMiB) + 1, &too_big.b) == OVT_E_INVALID_ARG);
  CHECK(too_big.b.data == nullptr);
}

TEST_CASE("byte service: the cache evicts the least recently used page", "[container][bytes][fast]") {
  PatternFile file(160);
  PageCache cache(file, 2, 16);  // two pages of 16 bytes
  std::array<uint8_t, 4> dst{};
  cache.read(0, 4, dst.data());   // miss page 0
  cache.read(16, 4, dst.data());  // miss page 1
  cache.read(0, 4, dst.data());   // hit page 0: page 1 is now the oldest
  cache.read(32, 4, dst.data());  // miss page 2 evicts page 1
  CHECK(cache.resident_pages() == 2);
  cache.read(0, 4, dst.data());  // still resident
  CHECK(cache.stats().hits == 2);
  CHECK(cache.stats().misses == 3);
  cache.read(16, 4, dst.data());  // was evicted: a miss again
  CHECK(cache.stats().misses == 4);
}

TEST_CASE("byte service: a bulk read bypasses the cache", "[container][bytes][fast]") {
  PatternFile file(16 * kMiB);
  PageCache cache(file);
  Bytes big(3 * kMiB);
  REQUIRE(cache.read(1000, big.size(), big.data()) == big.size());
  CHECK(big.back() == pattern(1000 + big.size() - 1));
  CHECK(cache.resident_pages() == 0);
  CHECK(cache.stats().misses == 0);
  CHECK(cache.stats().bytes_read == big.size());
}

TEST_CASE("byte service: NULL arguments and a handle without a file are OVT_E_INVALID_ARG",
          "[container][bytes][fast]") {
  Buf out;
  uint64_t box_offset = 0;
  int depth = 0;
  CHECK(ovt_container_bytes(nullptr, 0, 16, &out.b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(nullptr, 0, &box_offset, &depth) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_stats_json(nullptr, &out.b) == OVT_E_INVALID_ARG);
  auto* bare = new ovt_Container{nullptr, ovt::container::Format::Ivf, nullptr, {}, {}};
  CHECK(ovt_container_bytes(bare, 0, 16, &out.b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_stats_json(bare, &out.b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(bare, 0, &box_offset, &depth) == OVT_E_INVALID_ARG);  // no tree
  ovt_container_close(bare);
}

TEST_CASE("byte service: box_at returns the innermost box and its depth", "[container][bytes][box_at][fast]") {
  uint64_t tkhd_offset = 0;
  uint64_t moov_offset = 0;
  const Bytes mp4 = make_mp4(&tkhd_offset, &moov_offset);
  ovt_File file{std::make_unique<ovt::io::MemoryFile>(mp4, "box_at.mp4")};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&file, &c) == OVT_OK);
  REQUIRE(c != nullptr);

  uint64_t box_offset = 0;
  int depth = -1;
  // AT3: an offset inside moov/trak[0]/tkhd.
  REQUIRE(ovt_container_box_at(c, tkhd_offset + 20, &box_offset, &depth) == OVT_OK);
  CHECK(box_offset == tkhd_offset);
  CHECK(depth == 2);
  // The first and last byte of a box belong to it; the byte after belongs to the next one.
  REQUIRE(ovt_container_box_at(c, tkhd_offset, &box_offset, &depth) == OVT_OK);
  CHECK(box_offset == tkhd_offset);
  REQUIRE(ovt_container_box_at(c, tkhd_offset + 8 + 84 - 1, &box_offset, &depth) == OVT_OK);
  CHECK(box_offset == tkhd_offset);
  REQUIRE(ovt_container_box_at(c, tkhd_offset + 8 + 84, &box_offset, &depth) == OVT_OK);
  CHECK(depth == 2);  // mdia, the next sibling
  CHECK(box_offset == tkhd_offset + 8 + 84);
  // A header byte of moov is moov itself.
  REQUIRE(ovt_container_box_at(c, moov_offset + 3, &box_offset, &depth) == OVT_OK);
  CHECK(box_offset == moov_offset);
  CHECK(depth == 0);
  // The first byte and a byte of the last top-level box.
  REQUIRE(ovt_container_box_at(c, 0, &box_offset, &depth) == OVT_OK);
  CHECK((box_offset == 0 && depth == 0));
  REQUIRE(ovt_container_box_at(c, mp4.size() - 1, &box_offset, &depth) == OVT_OK);
  CHECK(depth == 0);
  // Outside every box.
  CHECK(ovt_container_box_at(c, mp4.size(), &box_offset, &depth) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(c, UINT64_MAX, &box_offset, &depth) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(c, 0, nullptr, &depth) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_box_at(c, 0, &box_offset, nullptr) == OVT_E_INVALID_ARG);

  // ovt_container_bytes through the same handle returns the file's bytes.
  Buf out;
  REQUIRE(ovt_container_bytes(c, tkhd_offset + 4, 4, &out.b) == OVT_OK);
  CHECK(out.bytes() == Bytes{'t', 'k', 'h', 'd'});
  ovt_container_close(c);
}

TEST_CASE("byte service: the calls are thread-safe next to expand", "[container][bytes][threads][fast]") {
  uint64_t tkhd_offset = 0;
  uint64_t moov_offset = 0;
  const Bytes mp4 = make_mp4(&tkhd_offset, &moov_offset);
  ovt_File file{std::make_unique<ovt::io::MemoryFile>(mp4, "threads.mp4")};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&file, &c) == OVT_OK);

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<uint64_t>(t));  // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic per thread
      while (!stop.load()) {
        const uint64_t offset = rng() % mp4.size();
        Buf out;
        if (ovt_container_bytes(c, offset, 16, &out.b) != OVT_OK ||
            out.b.size != std::min<uint64_t>(16, mp4.size() - offset) || out.b.data[0] != mp4[offset]) {
          failures.fetch_add(1);
        }
        uint64_t box_offset = 0;
        int depth = 0;
        if (ovt_container_box_at(c, offset, &box_offset, &depth) != OVT_OK || box_offset > offset) {
          failures.fetch_add(1);
        }
        Buf json;
        if (ovt_container_stats_json(c, &json.b) != OVT_OK) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (int i = 0; i < 200; ++i) {
    (void)ovt_container_expand(c, moov_offset);  // swaps the tree buffer while the readers walk it
  }
  stop.store(true);
  for (auto& r : readers) {
    r.join();
  }
  CHECK(failures.load() == 0);
  ovt_container_close(c);
}

TEST_CASE("byte service: unit bytes of H.264 and H.265 units carry the RBSP without emulation prevention",
          "[container][bytes][unit][fast]") {
  // A NAL header (nal_unit_type 6) and a payload holding one emulation_prevention_three_byte.
  const Bytes nal = {0x06, 0x00, 0x00, 0x03, 0x01, 0x80};
  Bytes file_bytes(10, 0xEE);
  file_bytes.insert(file_bytes.end(), nal.begin(), nal.end());
  file_bytes.insert(file_bytes.end(), 4, 0xEE);
  ovt::io::MemoryFile file(file_bytes);
  PageCache cache(file);

  Buf raw;
  Buf rbsp;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::H264, {10, nal.size()}, &raw.b, &rbsp.b) == OVT_OK);
  CHECK(raw.bytes() == nal);
  CHECK(rbsp.bytes() == Bytes{0x06, 0x00, 0x00, 0x01, 0x80});

  Buf raw_only;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::H265, {10, nal.size()}, &raw_only.b, nullptr) == OVT_OK);
  CHECK(raw_only.bytes() == nal);

  Buf rbsp_only;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::H265, {10, nal.size()}, nullptr, &rbsp_only.b) == OVT_OK);
  CHECK(rbsp_only.b.size == 5);

  Buf other_raw;
  Buf other_rbsp;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::Other, {10, nal.size()}, &other_raw.b, &other_rbsp.b) ==
          OVT_OK);
  CHECK(other_raw.bytes() == nal);
  CHECK(other_rbsp.b.size == 0);  // empty for raw units
}

TEST_CASE("byte service: unit bytes of an AV1 OBU are re-framed when the container stored it without a size",
          "[container][bytes][unit][fast]") {
  // OBU_FRAME (type 6) without obu_size: header 0x30, 3 payload bytes. With the size field: 0x32 0x03 ...
  const Bytes without_size = {0x30, 0xA1, 0xA2, 0xA3};
  // The same with an extension byte: header 0x34 (extension flag), extension 0x08.
  const Bytes with_extension = {0x34, 0x08, 0xB1, 0xB2};
  // Already framed: header 0x0A (OBU_SEQUENCE_HEADER, has_size), size 2.
  const Bytes with_size = {0x0A, 0x02, 0xC1, 0xC2};
  const Bytes file_bytes = cat({without_size, with_extension, with_size});
  ovt::io::MemoryFile file(file_bytes);
  PageCache cache(file);

  Buf a;
  Buf rbsp;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::Av1, {0, 4}, &a.b, &rbsp.b) == OVT_OK);
  CHECK(a.bytes() == Bytes{0x32, 0x03, 0xA1, 0xA2, 0xA3});
  CHECK(rbsp.b.size == 0);  // empty for AV1
  Buf b;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::Av1, {4, 4}, &b.b, nullptr) == OVT_OK);
  CHECK(b.bytes() == Bytes{0x36, 0x08, 0x02, 0xB1, 0xB2});
  Buf c;
  REQUIRE(ovt::container::read_unit_bytes(cache, UnitCodec::Av1, {8, 4}, &c.b, nullptr) == OVT_OK);
  CHECK(c.bytes() == with_size);
}

TEST_CASE("byte service: unit bytes reject units outside the file and empty units", "[container][bytes][unit][fast]") {
  const Bytes file_bytes(32, 0x41);
  ovt::io::MemoryFile file(file_bytes);
  PageCache cache(file);
  Buf raw;
  CHECK(ovt::container::read_unit_bytes(cache, UnitCodec::H264, {30, 8}, &raw.b, nullptr) == OVT_E_CORRUPT);
  CHECK(ovt::container::read_unit_bytes(cache, UnitCodec::H264, {33, 0}, &raw.b, nullptr) == OVT_E_CORRUPT);
  CHECK(ovt::container::read_unit_bytes(cache, UnitCodec::H264, {UINT64_MAX, 8}, &raw.b, nullptr) == OVT_E_CORRUPT);
  CHECK(ovt::container::read_unit_bytes(cache, UnitCodec::Av1, {4, 0}, &raw.b, nullptr) == OVT_E_CORRUPT);
  CHECK(raw.b.data == nullptr);
  // An AV1 unit whose header announces an extension byte that is not there.
  const Bytes truncated = {0x34};
  ovt::io::MemoryFile short_file(truncated);
  PageCache short_cache(short_file);
  CHECK(ovt::container::read_unit_bytes(short_cache, UnitCodec::Av1, {0, 1}, &raw.b, nullptr) == OVT_E_CORRUPT);
}
