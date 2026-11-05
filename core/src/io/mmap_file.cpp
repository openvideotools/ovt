// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The mmap backend of RandomAccessFile (chapter 7 §7.1.3, WI-1.100): one PROT_READ/MAP_PRIVATE mapping of
// the whole file. read() is a memcpy from the mapping, view() is zero-copy. Never MAP_POPULATE (slow on
// network drives). A SIGBUS from truncation while mapped is not handled (§7.1.3).

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "io_backends.hpp"

namespace ovt::io::detail {

namespace {

// Owns the mapping. Shared with every Holder returned by view(), so a view outlives its file object.
struct Mapping {
  Mapping(void* a, size_t l) noexcept : addr(a), length(l) {}
  ~Mapping() { ::munmap(addr, length); }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&&) = delete;
  Mapping& operator=(Mapping&&) = delete;

  [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return {static_cast<const uint8_t*>(addr), length}; }

  void* addr;
  size_t length;
};

class MmapFile final : public RandomAccessFile {
 public:
  MmapFile(std::shared_ptr<const Mapping> mapping, FileInfo info)
      : RandomAccessFile(std::move(info)), mapping_(std::move(mapping)) {}

  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (len > kMaxReadLen) {
      throw IoError(OVT_E_INVALID_ARG, 0, "io: read() length above 2^31 bytes");
    }
    const size_t n = clamp(offset, len);
    if (n != 0) {
      std::memcpy(dst, mapping_->bytes().subspan(offset, n).data(), n);
    }
    return n;
  }

  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override {
    const size_t n = clamp(offset, len);
    if (n == 0) {
      return {};
    }
    return Holder{mapping_, mapping_->bytes().subspan(offset, n)};
  }

  void advise(uint64_t offset, uint64_t len, Access access) const noexcept override {
    const uint64_t size = info().size;
    if (offset >= size || len == 0) {
      return;
    }
    const uint64_t end = offset + std::min(len, size - offset);
    // posix_madvise needs a page-aligned start; widen the range down to the page boundary.
    const auto page = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    const uint64_t start = offset - (offset % page);
    int advice = POSIX_MADV_NORMAL;
    switch (access) {
      case Access::Normal:
        advice = POSIX_MADV_NORMAL;
        break;
      case Access::Sequential:
        advice = POSIX_MADV_SEQUENTIAL;
        break;
      case Access::Random:
        advice = POSIX_MADV_RANDOM;
        break;
      case Access::WillNeed:
        advice = POSIX_MADV_WILLNEED;
        break;
      case Access::DontNeed:
        advice = POSIX_MADV_DONTNEED;
        break;
    }
    // A hint: failure is ignored. The mapping is never written, so DONTNEED cannot lose data.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): posix_madvise takes a non-const pointer.
    void* addr = const_cast<uint8_t*>(mapping_->bytes().subspan(start).data());
    const size_t length = end - start;  // same width on every 64-bit target (static_assert in io_backends.hpp)
    (void)::posix_madvise(addr, length, advice);
  }

  [[nodiscard]] bool is_mapped() const noexcept override { return true; }

 private:
  [[nodiscard]] size_t clamp(uint64_t offset, size_t len) const noexcept {
    const uint64_t size = info().size;
    if (offset >= size) {
      return 0;
    }
    return static_cast<size_t>(std::min<uint64_t>(len, size - offset));
  }

  std::shared_ptr<const Mapping> mapping_;
};

}  // namespace

std::unique_ptr<RandomAccessFile> make_mmap_file(int fd, FileInfo info, int& err) {
  const auto length = static_cast<size_t>(info.size);
  void* addr = ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED) {  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    err = errno;
    return nullptr;
  }
  auto mapping = std::make_shared<const Mapping>(addr, length);
  return std::make_unique<MmapFile>(std::move(mapping), std::move(info));
}

}  // namespace ovt::io::detail
