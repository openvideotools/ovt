// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The hex helper behind `ovt probe --bytes OFFSET:LEN` (chapter 12 §12.2.2, chapter 08 WI-1.235): a 16-bytes-per-row
// dump with an offset column and an ASCII column, over the byte service (ovt_container_bytes).

#ifndef OVT_CONTAINER_HEX_DUMP_H
#define OVT_CONTAINER_HEX_DUMP_H

#include <cstdint>
#include <span>
#include <string>

#include "ovt/status.h"

struct ovt_Container;

namespace ovt::container {

// Bytes per row of the dump.
inline constexpr size_t kHexDumpRowBytes = 16;

// Formats `bytes` as rows of the form
//   `00000010  66 74 79 70 69 73 6f 6d  00 00 02 00 69 73 6f 6d  |ftypisom....isom|\n`
// (offset in hex, minimum 8 digits; two groups of 8 bytes; `.` for bytes outside 0x20..0x7e). `first_offset` is the
// file offset of bytes[0]; the first row starts there (rows are not aligned to 16). A short last row is padded so
// the ASCII column lines up. Empty input gives an empty string. Every row, including the last, ends with '\n'.
// May throw std::bad_alloc.
[[nodiscard]] std::string format_hex_dump(uint64_t first_offset, std::span<const uint8_t> bytes);

// Reads up to `len` (at most 1 MiB) bytes at `offset` through ovt_container_bytes and formats them with
// format_hex_dump. The range is clipped at the end of the file, so the dump may be shorter than `len` or empty.
// OVT_E_INVALID_ARG for a null handle/out or len > 1 MiB; OVT_E_IO / OVT_E_NOMEM from the read. Never throws.
ovt_status hex_dump_bytes(ovt_Container* container, uint64_t offset, uint32_t len, std::string* out) noexcept;

}  // namespace ovt::container

#endif  // OVT_CONTAINER_HEX_DUMP_H
