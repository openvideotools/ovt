// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// format_hex_dump (container/hex_dump.h): pure formatting with no dependency on the container internals, so the CLI
// (`ovt probe --bytes`, WI-1.503) compiles this file into its own binary.

#include "container/hex_dump.h"

#include <algorithm>
#include <array>
#include <cstddef>

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

}  // namespace ovt::container
