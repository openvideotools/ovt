// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of ovt::io::RandomAccessFile (chapter 7 §7.1.5, WI-1.100). Both backends are checked against
// the bytes std::ifstream reads. Random offsets come from fixed seeds, so every run reads the same ranges.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"

namespace {

using ovt::io::IoError;
using ovt::io::OpenMode;
using ovt::io::RandomAccessFile;

constexpr size_t kMiB = size_t{1} << 20U;

// A file under the test binary directory (never TMPDIR: tests read no environment, chapter 14 §14.1.3),
// removed on destruction. The pid keeps parallel ctest runs apart.
class TempPath {
 public:
  explicit TempPath(const std::string& stem)
      : path_(std::filesystem::path(OVT_TEST_BINARY_DIR) / (stem + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(path_);
  }
  ~TempPath() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  TempPath(TempPath&&) = delete;
  TempPath& operator=(TempPath&&) = delete;

  [[nodiscard]] std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

std::vector<uint8_t> random_bytes(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seeds keep the tests deterministic
  std::vector<uint8_t> out(n);
  for (auto& b : out) {
    b = static_cast<uint8_t>(rng() >> 56U);
  }
  return out;
}

void write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

// The reference: the whole file as std::ifstream reads it.
std::vector<uint8_t> ifstream_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> slice(const std::vector<uint8_t>& ref, uint64_t offset, size_t len) {
  if (offset >= ref.size()) {
    return {};
  }
  const auto begin = static_cast<size_t>(offset);
  const size_t end = std::min(ref.size(), begin + len);
  return {ref.begin() + static_cast<std::ptrdiff_t>(begin), ref.begin() + static_cast<std::ptrdiff_t>(end)};
}

std::vector<uint8_t> read_range(const RandomAccessFile& f, uint64_t offset, size_t len) {
  std::vector<uint8_t> buf(len);
  const size_t n = f.read(offset, len, buf.data());
  buf.resize(n);
  return buf;
}

std::vector<uint8_t> view_range(const RandomAccessFile& f, uint64_t offset, size_t len) {
  const RandomAccessFile::Holder h = f.view(offset, len);
  return {h.bytes.begin(), h.bytes.end()};
}

// Mostly short reads (the parsers' pattern), some crossing 1 MiB cache blocks, a few longer than 1 MiB
// (the pread backend's cache bypass).
size_t random_len(std::mt19937_64& rng) {
  switch (rng() % 16U) {
    case 0:
      return rng() % (3 * kMiB);
    case 1:
    case 2:
      return rng() % (kMiB + 4096);
    default:
      return rng() % 4096U;
  }
}

uint64_t checksum(const std::vector<uint8_t>& bytes, uint64_t h) {
  for (uint8_t b : bytes) {  // FNV-1a
    h = (h ^ b) * 0x100000001b3ULL;
  }
  return h;
}

template <class Fn>
IoError catch_io_error(Fn&& fn) {
  try {
    fn();
  } catch (const IoError& e) {
    return e;
  }
  FAIL("no IoError thrown");
  return IoError(OVT_OK, 0, "");
}

}  // namespace

TEST_CASE("io_open_missing_path_throws", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Auto, OpenMode::Mmap, OpenMode::Pread);
  const TempPath missing("io_missing");
  const IoError e = catch_io_error([&] { (void)RandomAccessFile::open(missing.str(), mode); });
  CHECK(e.code == OVT_E_IO);
  CHECK(e.os_errno == ENOENT);

  const IoError empty = catch_io_error([&] { (void)RandomAccessFile::open("", mode); });
  CHECK(empty.code == OVT_E_INVALID_ARG);
}

TEST_CASE("io_open_empty_file_throws", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Auto, OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_empty");
  write_file(file.str(), {});
  const IoError e = catch_io_error([&] { (void)RandomAccessFile::open(file.str(), mode); });
  CHECK(e.code == OVT_E_INVALID_ARG);
  CHECK(e.os_errno == 0);
}

// WI-1.100 acceptance 3.
TEST_CASE("io_open_directory_throws", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Auto, OpenMode::Mmap, OpenMode::Pread);
  const TempPath dir("io_dir");
  std::filesystem::create_directory(dir.str());
  const IoError e = catch_io_error([&] { (void)RandomAccessFile::open(dir.str(), mode); });
  CHECK(e.code == OVT_E_INVALID_ARG);
}

// A FIFO without a writer must be rejected, not block in open(2).
TEST_CASE("io_open_fifo_throws", "[io][fast]") {
  const TempPath fifo("io_fifo");
  REQUIRE(::mkfifo(fifo.str().c_str(), 0600) == 0);
  const IoError e = catch_io_error([&] { (void)RandomAccessFile::open(fifo.str()); });
  CHECK(e.code == OVT_E_INVALID_ARG);
}

TEST_CASE("io_open_info", "[io][fast]") {
  const TempPath file("io_info");
  write_file(file.str(), random_bytes(12345, 1));

  const auto mapped = RandomAccessFile::open(file.str(), OpenMode::Mmap);
  CHECK(mapped->size() == 12345);
  CHECK(mapped->info().size == 12345);
  CHECK(mapped->info().display_name == file.str());
  CHECK(mapped->info().mtime_ns > 0);
  CHECK(mapped->is_mapped());

  const auto pread = RandomAccessFile::open(file.str(), OpenMode::Pread);
  CHECK(pread->size() == 12345);
  CHECK(pread->info().mtime_ns == mapped->info().mtime_ns);
  CHECK_FALSE(pread->is_mapped());

#if defined(__ANDROID__)
  CHECK_FALSE(RandomAccessFile::open(file.str())->is_mapped());
#else
  CHECK(RandomAccessFile::open(file.str())->is_mapped());  // Auto = Mmap on desktop (§7.1.3)
#endif
}

// WI-1.100 acceptance 2, §7.1.4 rule 1.
TEST_CASE("io_read_past_eof_returns_zero", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_eof");
  write_file(file.str(), random_bytes(5000, 2));
  const auto f = RandomAccessFile::open(file.str(), mode);

  std::vector<uint8_t> buf(16, 0xAA);
  uint64_t offset = GENERATE(uint64_t{5000}, uint64_t{5001}, UINT64_MAX);
  size_t n = 1;
  CHECK_NOTHROW(n = f->read(offset, buf.size(), buf.data()));
  CHECK(n == 0);
  CHECK(std::all_of(buf.begin(), buf.end(), [](uint8_t b) { return b == 0xAA; }));

  RandomAccessFile::Holder h;
  CHECK_NOTHROW(h = f->view(offset, 16));
  CHECK(h.bytes.empty());

  CHECK(f->read(0, 0, buf.data()) == 0);
}

TEST_CASE("io_read_tail_short", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_tail");
  const auto bytes = random_bytes(kMiB + 100, 3);  // the pread backend's last block is short
  write_file(file.str(), bytes);
  const auto f = RandomAccessFile::open(file.str(), mode);

  CHECK(read_range(*f, kMiB + 90, 64) == slice(bytes, kMiB + 90, 64));
  CHECK(read_range(*f, kMiB + 90, 64).size() == 10);
  CHECK(view_range(*f, kMiB - 10, 64) == slice(bytes, kMiB - 10, 64));
  CHECK(read_range(*f, 0, 2 * kMiB) == bytes);  // longer than 1 MiB: pread bypasses the cache
  CHECK(view_range(*f, 0, 2 * kMiB) == bytes);
}

// §7.1.4 rule 3.
TEST_CASE("io_read_len_above_2gib_throws", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_len");
  write_file(file.str(), random_bytes(64, 4));
  const auto f = RandomAccessFile::open(file.str(), mode);
  uint8_t sink = 0;
  const IoError e = catch_io_error([&] { (void)f->read(0, (size_t{1} << 31U) + 1, &sink); });
  CHECK(e.code == OVT_E_INVALID_ARG);
  CHECK(f->read(0, size_t{1} << 31U, std::vector<uint8_t>(64).data()) == 64);  // exactly 2^31 is allowed
}

TEST_CASE("io_view_equals_read", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_view");
  write_file(file.str(), random_bytes(3 * kMiB + 12345, 5));
  const auto ref = ifstream_bytes(file.str());
  auto f = RandomAccessFile::open(file.str(), mode);

  std::mt19937_64 rng(6);  // NOLINT(cert-msc32-c,cert-msc51-cpp)
  for (int i = 0; i < 500; ++i) {
    const uint64_t offset = rng() % (ref.size() + 100);
    const size_t len = random_len(rng);
    const auto expected = slice(ref, offset, len);
    REQUIRE(view_range(*f, offset, len) == expected);
    REQUIRE(read_range(*f, offset, len) == expected);
  }

  // A view stays valid after the file object (and, for pread, its cache) is gone.
  const RandomAccessFile::Holder inside = f->view(100, 1000);       // inside one block
  const RandomAccessFile::Holder across = f->view(kMiB - 50, 100);  // across two blocks
  f.reset();
  CHECK(std::equal(inside.bytes.begin(), inside.bytes.end(), ref.begin() + 100));
  CHECK(std::equal(across.bytes.begin(), across.bytes.end(), ref.begin() + static_cast<std::ptrdiff_t>(kMiB - 50)));
}

// WI-1.100 acceptance 1.
TEST_CASE("io_pread_backend_matches_mmap", "[io][fast]") {
  const TempPath file("io_backends");
  write_file(file.str(), random_bytes(10 * kMiB, 7));
  const auto ref = ifstream_bytes(file.str());
  REQUIRE(ref.size() == 10 * kMiB);

  const auto mapped = RandomAccessFile::open(file.str(), OpenMode::Mmap);
  const auto pread = RandomAccessFile::open(file.str(), OpenMode::Pread);
  REQUIRE(mapped->is_mapped());
  REQUIRE_FALSE(pread->is_mapped());

  std::mt19937_64 rng(8);  // NOLINT(cert-msc32-c,cert-msc51-cpp)
  int mismatches = 0;
  for (int i = 0; i < 10'000; ++i) {
    const uint64_t offset = rng() % ref.size();
    const size_t len = random_len(rng);
    const auto expected = slice(ref, offset, len);
    if (read_range(*mapped, offset, len) != expected || read_range(*pread, offset, len) != expected) {
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
}

// WI-1.100 acceptance 4, §7.1.4 rule 5.
TEST_CASE("io_concurrent_reads", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_threads");
  write_file(file.str(), random_bytes(4 * kMiB + 777, 9));
  const auto ref = ifstream_bytes(file.str());
  const auto f = RandomAccessFile::open(file.str(), mode);

  constexpr int kThreads = 8;
  constexpr int kReads = 1000;
  std::vector<uint64_t> expected(kThreads);
  std::vector<uint64_t> actual(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    std::mt19937_64 rng(100 + static_cast<uint64_t>(t));  // NOLINT(cert-msc32-c,cert-msc51-cpp)
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < kReads; ++i) {
      const uint64_t offset = rng() % ref.size();
      h = checksum(slice(ref, offset, rng() % 70'000U), h);
    }
    expected[static_cast<size_t>(t)] = h;
  }

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937_64 rng(100 + static_cast<uint64_t>(t));  // NOLINT(cert-msc32-c,cert-msc51-cpp)
      uint64_t h = 0xcbf29ce484222325ULL;
      for (int i = 0; i < kReads; ++i) {
        const uint64_t offset = rng() % ref.size();
        const size_t len = rng() % 70'000U;
        // Alternate read() and view() so both paths share the pread cache under contention.
        h = checksum(i % 2 == 0 ? read_range(*f, offset, len) : view_range(*f, offset, len), h);
      }
      actual[static_cast<size_t>(t)] = h;
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  CHECK(actual == expected);
}

TEST_CASE("io_advise_is_a_hint", "[io][fast]") {
  const OpenMode mode = GENERATE(OpenMode::Mmap, OpenMode::Pread);
  const TempPath file("io_advise");
  const auto bytes = random_bytes(kMiB + 3, 10);
  write_file(file.str(), bytes);
  const auto f = RandomAccessFile::open(file.str(), mode);
  using Access = RandomAccessFile::Access;
  for (Access a : {Access::Normal, Access::Sequential, Access::Random, Access::WillNeed, Access::DontNeed}) {
    f->advise(4097, 100'000, a);
    f->advise(0, UINT64_MAX, a);
    f->advise(UINT64_MAX, 1, a);
  }
  CHECK(read_range(*f, 0, bytes.size()) == bytes);
}
