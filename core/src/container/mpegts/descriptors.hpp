// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The MPEG-TS descriptor registry (chapter 08 §8.6.7, WI-4.205): a hand table of the descriptors the analyser
// decodes (ISO/IEC 13818-1 §2.6 and ETSI EN 300 468 §6) and a bounds-checked decoder that turns one descriptor body
// into named fields. A tag outside the table, or a body shorter than its syntax, decodes to one hex field.

#ifndef OVT_CONTAINER_MPEGTS_DESCRIPTORS_HPP
#define OVT_CONTAINER_MPEGTS_DESCRIPTORS_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::container::mpegts {

struct DescriptorInfo {
  uint8_t tag;
  std::string_view name;      // snake_case syntax element name of the specification
  std::string_view spec_ref;  // "ISO/IEC 13818-1 §2.6.x" or "ETSI EN 300 468 §6.2.x"
};

// nullptr for a tag that is not in the table.
[[nodiscard]] const DescriptorInfo* find_descriptor(uint8_t tag) noexcept;

// One decoded syntax element. `bit_offset` counts from the tag byte of the descriptor (the 16-bit header comes
// first); a string field has `is_text`. Fields without a position (derived values) have `bit_length == 0`.
struct DescField {
  std::string name;
  uint64_t value = 0;
  std::string text;
  bool is_text = false;
  uint32_t bit_offset = 0;
  uint32_t bit_length = 0;
};

// Decodes the body (the bytes after `descriptor_length`) of descriptor `tag`. Never reads outside `body`.
[[nodiscard]] std::vector<DescField> decode_descriptor(uint8_t tag, std::span<const uint8_t> body);

// Lower-case hex of `bytes`.
[[nodiscard]] std::string to_hex(std::span<const uint8_t> bytes);

// DVB text (EN 300 468 Annex A) reduced to printable UTF-8: character-table selector bytes and control codes are
// dropped and bytes >= 0xA0 are read as ISO 8859-1.
[[nodiscard]] std::string dvb_text(std::span<const uint8_t> bytes);

}  // namespace ovt::container::mpegts

#endif  // OVT_CONTAINER_MPEGTS_DESCRIPTORS_HPP
