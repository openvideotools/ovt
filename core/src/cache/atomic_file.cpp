// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// write_file_atomic, read_file and the stale-temporary sweep (chapter 06 §6.7.2, WI-0.091 step 2).

#include "cache/atomic_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "ovt/cxx/cache/error.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/status.h"

namespace ovt::cache {

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_simulate_crash{false};

[[noreturn]] void throw_io(std::string_view what, const fs::path& path, int err) {
  std::string msg = "cache: ";
  msg.append(what).append(" '").append(path.string()).append("': ").append(std::generic_category().message(err));
  throw CacheError(OVT_E_IO, msg);
}

class Fd {
 public:
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() { close(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&&) = delete;
  Fd& operator=(Fd&&) = delete;
  [[nodiscard]] int get() const noexcept { return fd_; }
  void close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_;
};

void write_all(int fd, std::span<const std::uint8_t> bytes, const fs::path& path) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_io("write", path, errno);
    }
    done += static_cast<std::size_t>(n);
  }
}

void fsync_dir(const fs::path& dir) {
  // A failed directory fsync is not fatal: the rename is already durable on most file systems; log it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): open(2) is variadic
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  if (::fsync(fd) != 0) {
    OVT_LOG(log::Level::Debug, "cache", "fsync of directory '{}' failed: {}", dir.string(),
            std::generic_category().message(errno));
  }
  ::close(fd);
}

}  // namespace

void set_simulated_crash_before_rename(bool armed) noexcept {
  g_simulate_crash.store(armed, std::memory_order_relaxed);
}

bool simulated_crash_before_rename() noexcept {
  return g_simulate_crash.load(std::memory_order_relaxed);
}

void write_file_atomic(const fs::path& path, std::span<const std::uint8_t> bytes) {
  fs::path tmp = path;
  tmp += kTmpSuffix;
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): open(2) is variadic
    Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (fd.get() < 0) {
      throw_io("create", tmp, errno);
    }
    write_all(fd.get(), bytes, tmp);
    if (::fsync(fd.get()) != 0) {
      const int err = errno;
      fd.close();
      ::unlink(tmp.c_str());
      throw_io("fsync", tmp, err);
    }
  }
  if (simulated_crash_before_rename()) {
    throw CacheError(OVT_E_IO, "cache: simulated crash before rename of '" + tmp.string() + "'");
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const int err = errno;
    ::unlink(tmp.c_str());
    throw_io("rename", path, err);
  }
  fsync_dir(path.parent_path());
}

std::vector<std::uint8_t> read_file(const fs::path& path, std::size_t max_size) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): open(2) is variadic
  const Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    throw_io("open", path, errno);
  }
  struct stat st = {};
  if (::fstat(fd.get(), &st) != 0) {
    throw_io("fstat", path, errno);
  }
  if (!S_ISREG(st.st_mode)) {
    throw CacheError(OVT_E_IO, "cache: not a regular file '" + path.string() + "'");
  }
  if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_size) {
    throw CacheError(OVT_E_CORRUPT,
                     "cache: file '" + path.string() + "' exceeds " + std::to_string(max_size) + " bytes");
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(st.st_size));
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t n = ::read(fd.get(), std::span(out).subspan(done).data(), out.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_io("read", path, errno);
    }
    if (n == 0) {
      out.resize(done);  // truncated under us; the caller's verifier decides
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  return out;
}

namespace {

/// Removes @p path if it is a stale temporary; true when a file was removed.
bool remove_if_stale_tmp(const fs::directory_entry& e) {
  std::error_code ec;
  if (!e.is_regular_file(ec) || e.path().extension() != kTmpSuffix) {
    return false;
  }
  if (fs::remove(e.path(), ec)) {
    OVT_LOG(log::Level::Info, "cache", "removed stale temporary '{}'", e.path().string());
    return true;
  }
  OVT_LOG(log::Level::Warn, "cache", "cannot remove stale temporary '{}': {}", e.path().string(), ec.message());
  return false;
}

}  // namespace

std::size_t remove_stale_tmp_files(const fs::path& dir) {
  std::error_code ec;
  const fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    return 0;
  }
  std::size_t removed = 0;
  for (const fs::directory_entry& e : it) {
    if (remove_if_stale_tmp(e)) {
      ++removed;
    }
  }
  return removed;
}

}  // namespace ovt::cache
