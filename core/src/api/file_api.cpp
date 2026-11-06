// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/io.h entry points (chapter 12 §12.3.6, chapter 7 §7.1.3, WI-1.101): thin noexcept wrappers around
// ovt::io::RandomAccessFile. Exceptions never cross the ABI (§12.3.1 rule 8).
// SPEC-ASSUMPTION(#110): both opens use OpenMode::Pread; ovt_file_open_fd dup()s a borrowed fd; an
// IoError maps to its own code (INVALID_ARG, IO or UNSUPPORTED).

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/io.h"

struct ovt_File {
  std::unique_ptr<ovt::io::RandomAccessFile> file;
};

namespace {

// Runs `fn` and turns any exception into an ovt_status.
template <class Fn>
ovt_status guarded(Fn&& fn) noexcept {
  try {
    return std::forward<Fn>(fn)();
  } catch (const ovt::io::IoError& e) {
    try {
      OVT_LOG(ovt::log::Level::Debug, "io", "{}", e.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch): logging must not change the status
    }
    return e.code;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

void close_fd(int fd) noexcept {
  ::close(fd);
}

}  // namespace

extern "C" {

OVT_API ovt_status ovt_file_open(const char* path, ovt_File** out) {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  if (path == nullptr || *path == '\0') {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    auto file = ovt::io::RandomAccessFile::open(path, ovt::io::OpenMode::Pread);
    auto handle = std::make_unique<ovt_File>(ovt_File{std::move(file)});
    *out = handle.release();
    return OVT_OK;
  });
}

OVT_API ovt_status ovt_file_open_fd(int fd, uint64_t size, int take_ownership, const char* display_name,
                                    ovt_File** out) {
  if (out == nullptr || fd < 0) {
    if (take_ownership != 0 && fd >= 0) {
      close_fd(fd);  // ownership passes at the call, even when the call fails
    }
    if (out != nullptr) {
      *out = nullptr;
    }
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;

  // Name the caller's descriptor, not the duplicate made below.
  std::string name;
  try {
    name = display_name != nullptr ? std::string(display_name) : std::string();
    if (name.empty()) {
      name = "fd:" + std::to_string(fd);
    }
  } catch (...) {  // std::bad_alloc
    if (take_ownership != 0) {
      close_fd(fd);
    }
    return OVT_E_NOMEM;
  }

  int owned = fd;
  if (take_ownership == 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg): fcntl(2) is variadic
    owned = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0) {
      return OVT_E_IO;  // EBADF (a closed descriptor) or EMFILE
    }
  }
  return guarded([&] {
    // open_fd closes `owned` if it throws; the RandomAccessFile closes it if allocating the handle throws.
    auto file = ovt::io::RandomAccessFile::open_fd(owned, size, true, std::move(name), ovt::io::OpenMode::Pread);
    auto handle = std::make_unique<ovt_File>(ovt_File{std::move(file)});
    *out = handle.release();
    return OVT_OK;
  });
}

OVT_API uint64_t ovt_file_size(const ovt_File* file) {
  return file != nullptr ? file->file->size() : 0;
}

OVT_API ovt_status ovt_file_read(ovt_File* file, uint64_t offset, uint8_t* dst, size_t len, size_t* out_read) {
  if (out_read != nullptr) {
    *out_read = 0;
  }
  if (file == nullptr || (dst == nullptr && len > 0)) {
    return OVT_E_INVALID_ARG;
  }
  return guarded([&] {
    const size_t n = file->file->read(offset, len, dst);
    if (out_read != nullptr) {
      *out_read = n;
    }
    return OVT_OK;
  });
}

OVT_API const char* ovt_file_display_name(const ovt_File* file) {
  return file != nullptr ? file->file->info().display_name.c_str() : nullptr;
}

OVT_API void ovt_file_close(ovt_File* file) {
  delete file;  // NOLINT(cppcoreguidelines-owning-memory): the handle came from unique_ptr::release()
}

}  // extern "C"
