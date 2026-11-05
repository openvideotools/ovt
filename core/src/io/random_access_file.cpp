// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// RandomAccessFile::open with the backend choice and mmap → pread fallback of chapter 7 §7.1.3, and the
// error rules of §7.1.4 (WI-1.100). open_fd is WI-1.101.

#include "ovt/cxx/io/random_access_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include "io_backends.hpp"

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

std::atomic<detail::LogHook> g_log_hook{nullptr};

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

  if (resolve_auto(mode) == OpenMode::Mmap) {
    if (info.size > detail::kMaxMappedSize) {
      throw IoError(OVT_E_UNSUPPORTED, 0, "io: file larger than 2^47 bytes cannot be mapped: '" + cpath + "'");
    }
    int err = 0;
    auto mapped = detail::make_mmap_file(fd, info, err);  // the mapping outlives the descriptor
    if (mapped) {
      return mapped;  // guard closes fd
    }
    detail::log(2, "io: mmap failed (" + errno_text(err) + "), using pread");
  }
  auto file = detail::make_pread_file(fd, /*owns_fd=*/true, std::move(info));
  guard.release();
  return file;
}

// §7.1.2 signature; the body is WI-1.101.
std::unique_ptr<RandomAccessFile> RandomAccessFile::open_fd(
    int /*fd*/, uint64_t /*size*/, bool /*take_ownership*/,
    std::string /*display_name*/,  // NOLINT(performance-unnecessary-value-param)
    OpenMode /*mode*/) {
  // Implemented by WI-1.101 (chapter 7 §7.8); declared here because the header is §7.1.2 verbatim.
  throw IoError(OVT_E_UNSUPPORTED, 0, "io: open_fd is not implemented yet (WI-1.101)");
}

namespace detail {

void set_log_hook(LogHook hook) noexcept {
  g_log_hook.store(hook, std::memory_order_release);
}

void log(int level, std::string_view message) {
  if (LogHook hook = g_log_hook.load(std::memory_order_acquire); hook != nullptr) {
    hook(level, message);
  }
}

}  // namespace detail

}  // namespace ovt::io
