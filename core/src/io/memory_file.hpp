// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// An in-memory ovt::io::RandomAccessFile for unit tests and fuzz targets of the container readers
// (chapter 08 §8.10, WI-1.225). Header-only; the bytes are copied.

#ifndef OVT_IO_MEMORY_FILE_HPP
#define OVT_IO_MEMORY_FILE_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::io {

class MemoryFile final : public RandomAccessFile {
 public:
  explicit MemoryFile(std::span<const uint8_t> bytes, std::string display_name = "memory")
      : RandomAccessFile(FileInfo{bytes.size(), 0, std::move(display_name)}), data_(bytes.begin(), bytes.end()) {}

  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset >= data_.size()) {
      return 0;
    }
    const size_t n = std::min<size_t>(len, data_.size() - static_cast<size_t>(offset));
    std::memcpy(dst, std::span<const uint8_t>(data_).subspan(static_cast<size_t>(offset), n).data(), n);
    return n;
  }

  [[nodiscard]] Holder view(uint64_t offset, size_t len) const override {
    if (offset >= data_.size()) {
      return Holder{nullptr, {}};
    }
    const size_t n = std::min<size_t>(len, data_.size() - static_cast<size_t>(offset));
    return Holder{nullptr, std::span<const uint8_t>(data_).subspan(static_cast<size_t>(offset), n)};
  }

  void advise(uint64_t /*offset*/, uint64_t /*len*/, Access /*access*/) const noexcept override {}
  [[nodiscard]] bool is_mapped() const noexcept override { return true; }

 private:
  std::vector<uint8_t> data_;
};

}  // namespace ovt::io

#endif  // OVT_IO_MEMORY_FILE_HPP
