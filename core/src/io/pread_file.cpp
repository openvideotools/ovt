// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The pread backend of RandomAccessFile (chapter 7 §7.1.3, WI-1.100): the default on Android and the
// fallback wherever mmap fails. pread(2) is retried on EINTR. Reads of up to 1 MiB and every view() go
// through an LRU of 64 blocks of 1 MiB (64 KiB-aligned buffers) keyed by offset / 1 MiB and protected by a
// mutex; reads longer than 1 MiB bypass the cache.

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "io_backends.hpp"

namespace ovt::io::detail {

namespace {

// One cache block. Shared with the Holders that view() hands out, so eviction never invalidates a view.
class Block {
 public:
  Block() : data_(static_cast<uint8_t*>(::operator new[](kBlockSize, std::align_val_t{kBlockAlign}))) {}
  ~Block() { ::operator delete[](data_, std::align_val_t{kBlockAlign}); }
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;
  Block(Block&&) = delete;
  Block& operator=(Block&&) = delete;

  [[nodiscard]] std::span<uint8_t> storage() noexcept { return {data_, kBlockSize}; }
  [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return {data_, length}; }
  size_t length = 0;  // valid bytes: kBlockSize, less for the last block or a file truncated under us

 private:
  uint8_t* data_;
};

class PreadFile final : public RandomAccessFile {
 public:
  PreadFile(int fd, bool owns_fd, FileInfo info) : RandomAccessFile(std::move(info)), fd_(fd), owns_fd_(owns_fd) {}

  ~PreadFile() override {
    if (owns_fd_) {
      ::close(fd_);
    }
  }

  PreadFile(const PreadFile&) = delete;
  PreadFile& operator=(const PreadFile&) = delete;
  PreadFile(PreadFile&&) = delete;
  PreadFile& operator=(PreadFile&&) = delete;

  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (len > kMaxReadLen) {
      throw IoError(OVT_E_INVALID_ARG, 0, "io: read() length above 2^31 bytes");
    }
    const size_t n = clamp(offset, len);
    if (n == 0) {
      return 0;
    }
    if (n > kBlockSize) {
      return pread_full(offset, std::span<uint8_t>(dst, n));
    }
    return read_cached(offset, std::span<uint8_t>(dst, n));
  }

  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override {
    const size_t n = clamp(offset, len);
    if (n == 0) {
      return {};
    }
    const uint64_t index = offset / kBlockSize;
    const size_t in_block = offset % kBlockSize;
    if (in_block + n <= kBlockSize) {  // inside one block: share the cached block, no extra copy
      const std::shared_ptr<const Block> block = get_block(index);
      const std::span<const uint8_t> bytes = block->bytes();
      if (bytes.size() <= in_block) {
        return {};  // the file shrank after open
      }
      return Holder{block, bytes.subspan(in_block, std::min(n, bytes.size() - in_block))};
    }
    // Spans blocks: copy into a buffer of its own.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): default-initialised (not zeroed) bytes, then overwritten.
    const std::shared_ptr<uint8_t[]> buffer(new uint8_t[n]);  // NOLINT(modernize-avoid-c-arrays)
    const std::span<uint8_t> out(buffer.get(), n);
    const size_t got = n > kBlockSize ? pread_full(offset, out) : read_cached(offset, out);
    return Holder{std::shared_ptr<const void>(buffer, buffer.get()), out.first(got)};
  }

  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*access*/) const noexcept override {}

  [[nodiscard]] bool is_mapped() const noexcept override { return false; }

 private:
  using Lru = std::list<std::pair<uint64_t, std::shared_ptr<const Block>>>;

  [[nodiscard]] size_t clamp(uint64_t offset, size_t len) const noexcept {
    const uint64_t size = info().size;
    if (offset >= size) {
      return 0;
    }
    return static_cast<size_t>(std::min<uint64_t>(len, size - offset));
  }

  // Fills `out` up to end of file. EINTR is retried; any other failure throws IoError.
  size_t pread_full(uint64_t offset, std::span<uint8_t> out) const {
    size_t got = 0;
    while (got < out.size()) {
      const std::span<uint8_t> rest = out.subspan(got, std::min(out.size() - got, kMaxReadLen));
      const ssize_t r = ::pread(fd_, rest.data(), rest.size(), static_cast<off_t>(offset + got));
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        const int err = errno;
        throw IoError(OVT_E_IO, err,
                      "io: pread failed on '" + info().display_name + "': " + std::generic_category().message(err));
      }
      if (r == 0) {
        break;  // the file shrank after open: a short read, never an error (§7.1.4 rule 1)
      }
      got += static_cast<size_t>(r);
    }
    return got;
  }

  size_t read_cached(uint64_t offset, std::span<uint8_t> out) const {
    size_t done = 0;
    while (done < out.size()) {
      const uint64_t pos = offset + done;
      const size_t in_block = pos % kBlockSize;
      const std::shared_ptr<const Block> block = get_block(pos / kBlockSize);
      const std::span<const uint8_t> bytes = block->bytes();
      if (bytes.size() <= in_block) {
        break;  // the file shrank after open
      }
      const size_t take = std::min(out.size() - done, bytes.size() - in_block);
      std::memcpy(out.subspan(done, take).data(), bytes.subspan(in_block, take).data(), take);
      done += take;
      if (bytes.size() < kBlockSize) {
        break;  // last (short) block
      }
    }
    return done;
  }

  std::shared_ptr<const Block> get_block(uint64_t index) const {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (auto it = map_.find(index); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->second;
      }
    }
    // Miss: read outside the lock so that concurrent readers of other blocks are not serialised. Two
    // threads may read the same block; the second insert finds the first and keeps it.
    auto block = std::make_shared<Block>();
    const uint64_t start = index * kBlockSize;
    const auto want = static_cast<size_t>(std::min<uint64_t>(kBlockSize, info().size - start));
    block->length = pread_full(start, block->storage().first(want));

    const std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = map_.find(index); it != map_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second);
      return it->second->second;
    }
    lru_.emplace_front(index, std::move(block));
    map_.emplace(index, lru_.begin());
    if (lru_.size() > kCacheBlocks) {
      map_.erase(lru_.back().first);
      lru_.pop_back();
    }
    return lru_.front().second;
  }

  int fd_;
  bool owns_fd_;
  mutable std::mutex mutex_;
  mutable Lru lru_;
  mutable std::unordered_map<uint64_t, Lru::iterator> map_;
};

}  // namespace

std::unique_ptr<RandomAccessFile> make_pread_file(int fd, bool owns_fd, FileInfo info) {
  return std::make_unique<PreadFile>(fd, owns_fd, std::move(info));
}

}  // namespace ovt::io::detail
