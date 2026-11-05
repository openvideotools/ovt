// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::io::RandomAccessFile: the single bounds-checked file access class of libovt-core (chapter 7 §7.1,
// WI-1.100). C++ helper header (D22): semi-public, no ABI promise, never included from a C header.
//
// The public declarations below are chapter 7 §7.1.2 verbatim, plus [[nodiscard]] on the getters and view()
// (clang-tidy modernize-use-nodiscard) and deleted copy/move. The protected section (the FileInfo the
// non-virtual info() returns) is the only other addition; backends derive from this class in core/src/io/.

#ifndef OVT_CXX_IO_RANDOM_ACCESS_FILE_HPP
#define OVT_CXX_IO_RANDOM_ACCESS_FILE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ovt/status.h"

namespace ovt::io {

enum class OpenMode { Auto, Mmap, Pread };  // Auto = Mmap on desktop, Pread on Android (see 7.1.3)

struct FileInfo {
  uint64_t size;             // bytes
  int64_t mtime_ns;          // modification time, ns since epoch; 0 if unknown (fd without stat)
  std::string display_name;  // path, or "fd:<n>" / content URI display name supplied by the caller
};

class RandomAccessFile {
 public:
  // Desktop: open by path (UTF-8). Throws ovt::IoError on failure.
  static std::unique_ptr<RandomAccessFile> open(std::string_view path, OpenMode mode = OpenMode::Auto);
  // Android / any OS: adopt an already-open descriptor. `size` MUST be the total length
  // (Android: ParcelFileDescriptor.getStatSize(); if < 0, the caller passes UINT64_MAX and
  // open_fd() uses fstat(2); if that fails too, IoError{OVT_E_IO}).
  // The RandomAccessFile takes ownership of fd (closes it in the destructor) when take_ownership == true.
  static std::unique_ptr<RandomAccessFile> open_fd(int fd, uint64_t size, bool take_ownership, std::string display_name,
                                                   OpenMode mode = OpenMode::Auto);
  virtual ~RandomAccessFile();

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) = delete;
  RandomAccessFile& operator=(RandomAccessFile&&) = delete;

  [[nodiscard]] const FileInfo& info() const noexcept;
  [[nodiscard]] uint64_t size() const noexcept { return info().size; }

  // Copies min(len, size-offset) bytes into dst. Returns bytes copied (0 at/after EOF).
  // offset > size() → returns 0 (never throws for a short read). I/O failure → throws IoError.
  // Thread-safe: may be called concurrently from any number of threads.
  virtual size_t read(uint64_t offset, size_t len, uint8_t* dst) const = 0;

  // Zero-copy view when the backend is mmap; otherwise a copy into an internal, reference-counted
  // buffer. The returned span is valid as long as the returned Holder is alive.
  struct Holder {
    std::shared_ptr<const void> keepalive;
    std::span<const uint8_t> bytes;
  };
  [[nodiscard]] virtual Holder view(uint64_t offset, size_t len) const = 0;

  // Hint only. mmap backend: madvise(MADV_WILLNEED / MADV_SEQUENTIAL / MADV_RANDOM). pread backend: no-op.
  enum class Access { Normal, Sequential, Random, WillNeed, DontNeed };
  virtual void advise(uint64_t offset, uint64_t len, Access) const noexcept = 0;

  // True when `view()` is zero-copy (mmap backend).
  [[nodiscard]] virtual bool is_mapped() const noexcept = 0;

 protected:
  explicit RandomAccessFile(FileInfo info) : info_(std::move(info)) {}

 private:
  FileInfo info_;
};

// Error type for this module (caught at the C API boundary → OVT_E_IO).
struct IoError : std::runtime_error {
  ovt_status code;  // OVT_E_IO or OVT_E_INVALID_ARG
  int os_errno;     // errno value or 0
  IoError(ovt_status c, int e, std::string msg);
};

}  // namespace ovt::io

#endif  // OVT_CXX_IO_RANDOM_ACCESS_FILE_HPP
