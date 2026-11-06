// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// RandomAccessFile::open with the backend choice and mmap → pread fallback of chapter 7 §7.1.3, and the
// error rules of §7.1.4 (WI-1.100), and open_fd (WI-1.101).

#include "ovt/cxx/io/random_access_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include "io_backends.hpp"
#include "ovt/cxx/log.hpp"

namespace ovt::io {

// The by-value msg is the §7.1.2 signature; std::runtime_error copies it either way.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
IoError::IoError(ovt_status c, int e, std::string msg) : std::runtime_error(msg), code(c), os_errno(e) {}

RandomAccessFile::~RandomAccessFile() = default;

const FileInfo& RandomAccessFile::info() const noexcept {
  return info_;
}

namespace {

std::string errno_text(int err) {
  // strerror is not guaranteed thread-safe; strerror_r has two incompatible signatures (GNU/XSI).
  // std::error_code's message() is thread-safe and portable.
  return std::generic_category().message(err);
}

[[noreturn]] void throw_os(ovt_status code, int err, std::string_view what, std::string_view path) {
  std::string msg = "io: ";
  msg.append(what).append(" '").append(path).append("': ").append(errno_text(err));
  throw IoError(code, err, std::move(msg));
}

int64_t mtime_ns_of(const struct stat& st) noexcept {
#if defined(__APPLE__)
  const struct timespec& ts = st.st_mtimespec;
#else
  const struct timespec& ts = st.st_mtim;
#endif
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + static_cast<int64_t>(ts.tv_nsec);
}

constexpr OpenMode resolve_auto(OpenMode mode) noexcept {
  if (mode != OpenMode::Auto) {
    return mode;
  }
#if defined(__ANDROID__)
  return OpenMode::Pread;
#else
  return OpenMode::Mmap;
#endif
}

class FdCloser {
 public:
  explicit FdCloser(int fd) noexcept : fd_(fd) {}
  ~FdCloser() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  FdCloser(const FdCloser&) = delete;
  FdCloser& operator=(const FdCloser&) = delete;
  FdCloser(FdCloser&&) = delete;
  FdCloser& operator=(FdCloser&&) = delete;
  int release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

// The backend for a checked descriptor: mmap when `try_mmap`, falling back to pread (§7.1.3). An owned fd is
// handed to the pread backend, or closed by `guard` once the mapping exists (the mapping outlives it).
std::unique_ptr<RandomAccessFile> make_backend(int fd, FdCloser& guard, bool owns_fd, FileInfo info, bool try_mmap) {
  if (try_mmap) {
    if (info.size > detail::kMaxMappedSize) {
      throw IoError(OVT_E_UNSUPPORTED, 0,
                    "io: file larger than 2^47 bytes cannot be mapped: '" + info.display_name + "'");
    }
    int err = 0;
    auto mapped = detail::make_mmap_file(fd, info, err);
    if (mapped) {
      return mapped;
    }
    OVT_LOG(ovt::log::Level::Info, "io", "mmap failed ({}), using pread", errno_text(err));
  }
  auto file = detail::make_pread_file(fd, owns_fd, std::move(info));
  if (owns_fd) {
    guard.release();
  }
  return file;
}

// What open_fd learns about a descriptor before choosing a backend.
struct FdFacts {
  FileInfo info;
  bool mappable = false;  // a regular file whose bytes cover info.size (a mapping past EOF raises SIGBUS)
};

// Checks `fd` and resolves an unknown size with fstat(2) (§7.1.2). Throws IoError; `guard` closes an owned fd.
FdFacts describe_fd(int fd, uint64_t size, std::string display_name, FdCloser& guard) {
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    const int err = errno;
    if (err == EBADF) {
      guard.release();  // nothing to close; closing the number again could hit a descriptor another thread reused
      throw_os(OVT_E_IO, err, "bad descriptor", display_name);  // §7.1.4 rule 2
    }
    // SPEC-ASSUMPTION(#110): both UINT64_MAX (chapter 7 §7.1.2) and 0 (chapter 12 §12.3.6) mean "size unknown,
    // use fstat(2)". An explicit 0 could never be opened anyway (§7.1.4 rule 4: empty files are rejected).
    if (size == 0 || size == UINT64_MAX) {
      throw_os(OVT_E_IO, err, "cannot stat", display_name);
    }
    return {FileInfo{size, 0, std::move(display_name)}, false};  // trust the caller's size; mtime unknown
  }
  const bool is_regular = S_ISREG(st.st_mode);
  if (S_ISDIR(st.st_mode)) {
    throw IoError(OVT_E_INVALID_ARG, 0, "io: not a regular file / empty: '" + display_name + "'");
  }
  if (size == 0 || size == UINT64_MAX) {
    // A pipe or socket reports no usable size; the shell copies such content to a file first (WI-1.101 notes).
    if (!is_regular || st.st_size <= 0) {
      throw IoError(OVT_E_INVALID_ARG, 0, "io: not a regular file / empty: '" + display_name + "'");
    }
    size = static_cast<uint64_t>(st.st_size);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): fcntl(2) is variadic
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0 && (flags & O_ACCMODE) == O_WRONLY) {
    throw IoError(OVT_E_INVALID_ARG, 0, "io: descriptor is not open for reading: '" + display_name + "'");
  }
  const bool mappable = is_regular && size <= static_cast<uint64_t>(st.st_size);
  return {FileInfo{size, mtime_ns_of(st), std::move(display_name)}, mappable};
}

}  // namespace

std::unique_ptr<RandomAccessFile> RandomAccessFile::open(std::string_view path, OpenMode mode) {
  if (path.empty() || path.find('\0') != std::string_view::npos) {
    throw IoError(OVT_E_INVALID_ARG, 0, "io: empty path or path with an embedded NUL");
  }
  const std::string cpath(path);

  // O_NONBLOCK: opening a FIFO for reading must not block waiting for a writer; such a file is then
  // rejected by the S_ISREG check below. Regular-file reads ignore the flag.
  int fd = -1;
  do {
    fd = ::open(cpath.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    const int err = errno;
    // A socket cannot be opened with open(2) (ENXIO); §7.1.4 rule 4 classes it as an invalid argument.
    throw_os(err == ENXIO ? OVT_E_INVALID_ARG : OVT_E_IO, err, "cannot open", path);
  }
  FdCloser guard(fd);

  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    throw_os(OVT_E_IO, errno, "cannot stat", path);
  }
  if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
    throw IoError(OVT_E_INVALID_ARG, 0, "io: not a regular file / empty: '" + cpath + "'");
  }

  FileInfo info{static_cast<uint64_t>(st.st_size), mtime_ns_of(st), cpath};

  return make_backend(fd, guard, /*owns_fd=*/true, std::move(info), resolve_auto(mode) == OpenMode::Mmap);
}

std::unique_ptr<RandomAccessFile> RandomAccessFile::open_fd(int fd, uint64_t size, bool take_ownership,
                                                            std::string display_name, OpenMode mode) {
  if (fd < 0) {
    throw IoError(OVT_E_INVALID_ARG, 0, "io: open_fd with a negative descriptor");
  }
  // SPEC-ASSUMPTION(#110): an adopted descriptor is closed on every failure path too, so a caller that
  // handed over ParcelFileDescriptor.detachFd() never has to clean up after an error.
  FdCloser guard(take_ownership ? fd : -1);
  if (display_name.empty()) {
    display_name = "fd:" + std::to_string(fd);
  }
  FdFacts facts = describe_fd(fd, size, std::move(display_name), guard);
  // §7.1.3: mmap only for a regular file; anything else, e.g. a DocumentsProvider pipe, is read with pread(2).
  const bool try_mmap = resolve_auto(mode) == OpenMode::Mmap && facts.mappable;
  return make_backend(fd, guard, take_ownership, std::move(facts.info), try_mmap);
}

}  // namespace ovt::io
