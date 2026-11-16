// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of RandomAccessFile::open_fd and the ovt/io.h C API (chapter 7 §7.1.2, §7.1.3, §7.1.5;
// chapter 12 §12.3.6; WI-1.101). Android's content:// path (acceptance test 3) runs in WI-0.051's
// instrumentation tests; here a descriptor of a temporary file stands in for ParcelFileDescriptor.detachFd().

#if defined(__APPLE__)
#include <sys/fcntl.h>  // macOS declares open(2), fcntl(2) and the O_/F_ constants here
#else
#include <fcntl.h>
#endif
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/io.h"
#include "ovt/status.h"

namespace {

using ovt::io::IoError;
using ovt::io::OpenMode;
using ovt::io::RandomAccessFile;

constexpr size_t kMiB = size_t{1} << 20U;
// 3 GB: above 2^31, the limit of a 32-bit off_t or jint (WI-0.051 acceptance test 2). Namespace scope, because
// GENERATE captures its arguments in a lambda.
constexpr uint64_t k3GB = uint64_t{3} * 1000 * 1000 * 1000;

// A file under the test binary directory (tests read no environment, chapter 14 §14.1.3), removed on
// destruction. The pid keeps parallel ctest runs apart.
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

int open_fd(const std::string& path, int flags = O_RDONLY) {
  const int fd = ::open(path.c_str(), flags | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  REQUIRE(fd >= 0);
  return fd;
}

// True while `fd` names an open descriptor; false once it is closed (fcntl fails with EBADF).
bool fd_is_open(int fd) {
  errno = 0;
  const int r = ::fcntl(fd, F_GETFD);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (r == -1) {
    REQUIRE(errno == EBADF);
    return false;
  }
  return true;
}

std::vector<uint8_t> read_all(const RandomAccessFile& f) {
  std::vector<uint8_t> buf(f.size());
  REQUIRE(f.read(0, buf.size(), buf.data()) == buf.size());
  return buf;
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

// The two ends of a pipe(2), closed on destruction unless released.
struct Pipe {
  std::array<int, 2> fds{-1, -1};
  Pipe() { REQUIRE(::pipe(fds.data()) == 0); }
  ~Pipe() {
    for (int fd : fds) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;
  Pipe(Pipe&&) = delete;
  Pipe& operator=(Pipe&&) = delete;
};

}  // namespace

// ---- RandomAccessFile::open_fd -------------------------------------------------------------------

// Acceptance test 1: an fd of a 1 MiB file with size = UINT64_MAX gets its size from fstat(2).
TEST_CASE("io_fd_size_from_fstat", "[io][fast]") {
  const TempPath path("io_fd_size");
  const auto bytes = random_bytes(kMiB, 101);
  write_file(path.str(), bytes);
  const OpenMode mode = GENERATE(OpenMode::Auto, OpenMode::Pread, OpenMode::Mmap);
  const uint64_t unknown = GENERATE(UINT64_MAX, uint64_t{0});  // ch.7 and ch.12 sentinels (#110)

  const int fd = open_fd(path.str());
  const auto f = RandomAccessFile::open_fd(fd, unknown, true, "clip.mp4", mode);
  CHECK(f->info().size == kMiB);
  CHECK(f->size() == kMiB);
  CHECK(f->info().display_name == "clip.mp4");
  CHECK(f->info().mtime_ns > 0);
  CHECK(f->is_mapped() == (mode != OpenMode::Pread));
  CHECK(read_all(*f) == bytes);
}

// Acceptance test 2 (§7.1.5 io_fd_ownership): take_ownership closes the fd on destruction.
TEST_CASE("io_fd_ownership", "[io][fast]") {
  const TempPath path("io_fd_own");
  write_file(path.str(), random_bytes(4096, 102));
  const OpenMode mode = GENERATE(OpenMode::Pread, OpenMode::Mmap);

  SECTION("take_ownership = true closes the fd") {
    const int fd = open_fd(path.str());
    auto f = RandomAccessFile::open_fd(fd, UINT64_MAX, true, "", mode);
    // The pread backend reads through fd; the mmap backend needs only the mapping and closes it at once.
    CHECK(fd_is_open(fd) == !f->is_mapped());
    f.reset();
    CHECK_FALSE(fd_is_open(fd));
  }
  SECTION("take_ownership = false leaves the fd open") {
    const int fd = open_fd(path.str());
    auto f = RandomAccessFile::open_fd(fd, UINT64_MAX, false, "", mode);
    f.reset();
    CHECK(fd_is_open(fd));
    std::array<uint8_t, 1> one{};
    CHECK(::pread(fd, one.data(), 1, 0) == 1);
    ::close(fd);
  }
}

TEST_CASE("io_fd_explicit_size_is_trusted", "[io][fast]") {
  const TempPath path("io_fd_explicit");
  const auto bytes = random_bytes(10000, 103);
  write_file(path.str(), bytes);
  const OpenMode mode = GENERATE(OpenMode::Pread, OpenMode::Mmap);

  const auto f = RandomAccessFile::open_fd(open_fd(path.str()), 6000, true, "", mode);
  CHECK(f->size() == 6000);
  std::vector<uint8_t> buf(10000);
  CHECK(f->read(5000, buf.size(), buf.data()) == 1000);
  CHECK(f->read(6000, buf.size(), buf.data()) == 0);
}

TEST_CASE("io_fd_size_beyond_eof_is_not_mapped", "[io][fast]") {
  // A mapping past the end of the file would raise SIGBUS (§7.1.3); such an fd is read with pread.
  const TempPath path("io_fd_beyond");
  write_file(path.str(), random_bytes(4096, 104));
  const auto f = RandomAccessFile::open_fd(open_fd(path.str()), 8192, true, "", OpenMode::Mmap);
  CHECK_FALSE(f->is_mapped());
  std::vector<uint8_t> buf(8192);
  CHECK(f->read(0, buf.size(), buf.data()) <= 4096);  // the bytes that exist; never a crash
}

TEST_CASE("io_fd_default_display_name", "[io][fast]") {
  const TempPath path("io_fd_name");
  write_file(path.str(), random_bytes(16, 105));
  const int fd = open_fd(path.str());
  const auto f = RandomAccessFile::open_fd(fd, UINT64_MAX, true, "");
  CHECK(f->info().display_name == "fd:" + std::to_string(fd));
}

TEST_CASE("io_fd_errors", "[io][fast]") {
  const TempPath path("io_fd_errors");
  write_file(path.str(), random_bytes(16, 106));

  SECTION("negative fd") {
    const IoError e = catch_io_error([] { (void)RandomAccessFile::open_fd(-1, 16, true, ""); });
    CHECK(e.code == OVT_E_INVALID_ARG);
  }
  SECTION("closed fd") {
    const int fd = open_fd(path.str());
    ::close(fd);
    const IoError e = catch_io_error([fd] { (void)RandomAccessFile::open_fd(fd, UINT64_MAX, false, ""); });
    CHECK(e.code == OVT_E_IO);
    CHECK(e.os_errno == EBADF);
  }
  SECTION("directory") {
    const int fd = open_fd(OVT_TEST_BINARY_DIR, O_RDONLY | O_DIRECTORY);
    const IoError e = catch_io_error([fd] { (void)RandomAccessFile::open_fd(fd, 4096, true, ""); });
    CHECK(e.code == OVT_E_INVALID_ARG);
    CHECK_FALSE(fd_is_open(fd));  // adopted, so closed on failure too (#110)
  }
  SECTION("empty file with unknown size") {
    const TempPath empty("io_fd_empty");
    write_file(empty.str(), {});
    const int fd = open_fd(empty.str());
    const IoError e = catch_io_error([fd] { (void)RandomAccessFile::open_fd(fd, UINT64_MAX, false, ""); });
    CHECK(e.code == OVT_E_INVALID_ARG);
    CHECK(fd_is_open(fd));  // borrowed, so left open
    ::close(fd);
  }
  SECTION("pipe with unknown size (getStatSize() == -1)") {
    Pipe p;
    const int fd = p.fds[0];
    p.fds[0] = -1;
    const IoError e = catch_io_error([fd] { (void)RandomAccessFile::open_fd(fd, UINT64_MAX, true, ""); });
    CHECK(e.code == OVT_E_INVALID_ARG);
    CHECK_FALSE(fd_is_open(fd));
  }
  SECTION("write-only fd") {
    const int fd = open_fd(path.str(), O_WRONLY);
    const IoError e = catch_io_error([fd] { (void)RandomAccessFile::open_fd(fd, 16, true, ""); });
    CHECK(e.code == OVT_E_INVALID_ARG);
    CHECK_FALSE(fd_is_open(fd));
  }
}

// ---- ovt/io.h ------------------------------------------------------------------------------------

TEST_CASE("capi_file_open_fd_owned", "[io][fast]") {
  const TempPath path("capi_fd_owned");
  const auto bytes = random_bytes(kMiB, 107);
  write_file(path.str(), bytes);
  const uint64_t unknown = GENERATE(UINT64_MAX, uint64_t{0});

  const int fd = open_fd(path.str());
  ovt_File* f = nullptr;
  REQUIRE(ovt_file_open_fd(fd, unknown, 1, "Movie.mp4", &f) == OVT_OK);
  REQUIRE(f != nullptr);
  CHECK(ovt_file_size(f) == kMiB);
  CHECK(std::string(ovt_file_display_name(f)) == "Movie.mp4");

  std::vector<uint8_t> buf(bytes.size() + 10);
  size_t n = 0;
  CHECK(ovt_file_read(f, 0, buf.data(), buf.size(), &n) == OVT_OK);
  CHECK(n == bytes.size());
  buf.resize(n);
  CHECK(buf == bytes);
  CHECK(ovt_file_read(f, kMiB + 5, buf.data(), 1, &n) == OVT_OK);
  CHECK(n == 0);

  ovt_file_close(f);
  CHECK_FALSE(fd_is_open(fd));
}

TEST_CASE("capi_file_open_fd_borrowed", "[io][fast]") {
  const TempPath path("capi_fd_borrowed");
  write_file(path.str(), random_bytes(4096, 108));
  const int fd = open_fd(path.str());

  ovt_File* f = nullptr;
  REQUIRE(ovt_file_open_fd(fd, 4096, 0, nullptr, &f) == OVT_OK);
  CHECK(std::string(ovt_file_display_name(f)) == "fd:" + std::to_string(fd));  // the caller's fd, not the dup
  ovt_file_close(f);
  CHECK(fd_is_open(fd));
  ::close(fd);
}

// WI-0.051 acceptance test 2 on the host: a 3 GB sparse file (above 2^31, the limit of a 32-bit off_t or jint) opened
// by descriptor, as MainActivity does with ParcelFileDescriptor.detachFd(), reports its 64-bit size, both when
// the caller passes getStatSize() and when it passes 0 and the library asks fstat(2). The same call chain runs on
// the emulator through ovt_jni_open_fd.
TEST_CASE("capi_file_open_fd_3gb_sparse", "[io][fast]") {
  const TempPath path("capi_fd_3gb");
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int fd = ::open(path.str().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, static_cast<off_t>(k3GB)) == 0);
    ::close(fd);
  }
  const uint64_t reported = GENERATE(k3GB, uint64_t{0});

  ovt_File* f = nullptr;
  REQUIRE(ovt_file_open_fd(open_fd(path.str()), reported, 1, "big.mp4", &f) == OVT_OK);
  REQUIRE(f != nullptr);
  CHECK(ovt_file_size(f) == k3GB);

  std::array<uint8_t, 16> tail{};
  tail.fill(0xAA);
  size_t n = 0;
  CHECK(ovt_file_read(f, k3GB - tail.size(), tail.data(), tail.size(), &n) == OVT_OK);
  CHECK(n == tail.size());
  CHECK(tail == std::array<uint8_t, 16>{});  // a hole reads as zeros
  ovt_file_close(f);
}

TEST_CASE("capi_file_open_fd_errors", "[io][fast]") {
  const TempPath path("capi_fd_errors");
  write_file(path.str(), random_bytes(16, 109));
  int poison = 0;
  auto* f = reinterpret_cast<ovt_File*>(&poison);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): must be reset

  SECTION("negative fd") {
    CHECK(ovt_file_open_fd(-1, 16, 1, nullptr, &f) == OVT_E_INVALID_ARG);
    CHECK(f == nullptr);
  }
  SECTION("NULL out closes an adopted fd") {
    const int fd = open_fd(path.str());
    CHECK(ovt_file_open_fd(fd, 16, 1, nullptr, nullptr) == OVT_E_INVALID_ARG);
    CHECK_FALSE(fd_is_open(fd));
  }
  SECTION("closed fd") {
    const int fd = open_fd(path.str());
    ::close(fd);
    const int take = GENERATE(0, 1);
    CHECK(ovt_file_open_fd(fd, UINT64_MAX, take, nullptr, &f) == OVT_E_IO);
    CHECK(f == nullptr);
  }
  SECTION("pipe with unknown size closes an adopted fd") {
    Pipe p;
    const int fd = p.fds[0];
    p.fds[0] = -1;
    CHECK(ovt_file_open_fd(fd, UINT64_MAX, 1, nullptr, &f) == OVT_E_INVALID_ARG);
    CHECK(f == nullptr);
    CHECK_FALSE(fd_is_open(fd));
  }
  SECTION("pipe with unknown size leaves a borrowed fd open") {
    Pipe p;
    CHECK(ovt_file_open_fd(p.fds[0], UINT64_MAX, 0, nullptr, &f) == OVT_E_INVALID_ARG);
    CHECK(f == nullptr);
    CHECK(fd_is_open(p.fds[0]));
  }
}

TEST_CASE("capi_file_open_path", "[io][fast]") {
  const TempPath path("capi_path");
  const auto bytes = random_bytes(3000, 110);
  write_file(path.str(), bytes);

  ovt_File* f = nullptr;
  REQUIRE(ovt_file_open(path.str().c_str(), &f) == OVT_OK);
  CHECK(ovt_file_size(f) == bytes.size());
  CHECK(std::string(ovt_file_display_name(f)) == path.str());
  std::vector<uint8_t> buf(100);
  size_t n = 0;
  CHECK(ovt_file_read(f, 2950, buf.data(), buf.size(), &n) == OVT_OK);
  CHECK(n == 50);
  CHECK(ovt_file_read(f, 0, nullptr, 1, &n) == OVT_E_INVALID_ARG);
  CHECK(ovt_file_read(f, 0, nullptr, 0, nullptr) == OVT_OK);
  CHECK(ovt_file_read(f, 0, buf.data(), (size_t{1} << 31U) + 1, &n) == OVT_E_INVALID_ARG);
  ovt_file_close(f);

  CHECK(ovt_file_open("", &f) == OVT_E_INVALID_ARG);
  CHECK(f == nullptr);
  CHECK(ovt_file_open(nullptr, &f) == OVT_E_INVALID_ARG);
  CHECK(ovt_file_open(path.str().c_str(), nullptr) == OVT_E_INVALID_ARG);
  const TempPath missing("capi_missing");
  CHECK(ovt_file_open(missing.str().c_str(), &f) == OVT_E_IO);
  CHECK(ovt_file_open(OVT_TEST_BINARY_DIR, &f) == OVT_E_INVALID_ARG);

  CHECK(ovt_file_size(nullptr) == 0);
  CHECK(ovt_file_display_name(nullptr) == nullptr);
  CHECK(ovt_file_read(nullptr, 0, buf.data(), 1, &n) == OVT_E_INVALID_ARG);
  ovt_file_close(nullptr);
}
