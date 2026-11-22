// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/hex_dump.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "container/container.hpp"
#include "ovt/container.h"

namespace ovt::container {

namespace {

constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
constexpr size_t kMinOffsetDigits = 8;
constexpr size_t kGroupBytes = 8;

void append_hex(std::string& out, uint64_t value, size_t digits) {
  for (size_t i = digits; i > 0; --i) {
    out.push_back(kDigits[(value >> ((i - 1) * 4)) & 0xFU]);
  }
}

size_t hex_digits(uint64_t value) {
  size_t n = 1;
  while (value >= 16) {
    value >>= 4;
    ++n;
  }
  return n;
}

// Releases a buffer made by api::make_buffer: the delete[] of ovt_buffer_free() (core/src/status.cpp, which lives in
// the shared library and so is not linked into the module objects).
void release(ovt_Buffer& buf) noexcept {
  delete[] buf.data;  // NOLINT(cppcoreguidelines-owning-memory): the allocator of rule 3a is new[] (api/buffer.cpp)
  buf = ovt_Buffer{nullptr, 0};
}

}  // namespace

std::string format_hex_dump(uint64_t first_offset, std::span<const uint8_t> bytes) {
  std::string out;
  if (bytes.empty()) {
    return out;
  }
  // The widest offset is that of the last row; a fixed width keeps the columns aligned.
  const uint64_t last_row = first_offset + ((bytes.size() - 1) / kHexDumpRowBytes) * kHexDumpRowBytes;
  const size_t width = std::max(kMinOffsetDigits, hex_digits(last_row));
  out.reserve(((bytes.size() + kHexDumpRowBytes - 1) / kHexDumpRowBytes) *
              (width + 2 + (kHexDumpRowBytes * 3) + 3 + kHexDumpRowBytes + 3));
  for (size_t row = 0; row < bytes.size(); row += kHexDumpRowBytes) {
    const size_t n = std::min(kHexDumpRowBytes, bytes.size() - row);
    append_hex(out, first_offset + row, width);
    out.push_back(' ');
    for (size_t i = 0; i < kHexDumpRowBytes; ++i) {
      if (i % kGroupBytes == 0) {
        out.push_back(' ');  // two spaces before the first group, one extra between the groups
      }
      if (i < n) {
        append_hex(out, bytes[row + i], 2);
      } else {
        out.append("  ");
      }
      out.push_back(' ');
    }
    out.append(" |");
    for (size_t i = 0; i < n; ++i) {
      const uint8_t b = bytes[row + i];
      out.push_back(b >= 0x20 && b <= 0x7E ? static_cast<char>(b) : '.');
    }
    out.append("|\n");
  }
  return out;
}

ovt_status hex_dump_bytes(ovt_Container* container, uint64_t offset, uint32_t len, std::string* out) noexcept {
  if (out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  out->clear();
  ovt_Buffer buf{nullptr, 0};
  const ovt_status st = ovt_container_bytes(container, offset, len, &buf);
  if (st != OVT_OK) {
    return st;
  }
  try {
    *out = format_hex_dump(offset, std::span<const uint8_t>(buf.data, buf.size));
  } catch (...) {
    release(buf);
    return status_of_current_exception();
  }
  release(buf);
  return OVT_OK;
}

}  // namespace ovt::container
