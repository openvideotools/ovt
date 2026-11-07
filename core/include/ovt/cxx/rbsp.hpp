// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Emulation-prevention removal for H.264/H.265 NAL units and the raw <-> RBSP offset mapping used by the
// hex view (chapter 7 §7.2.5). C++ helper header (D22): semi-public, no ABI promise, never included from
// a C header.
//
// The declarations below are chapter 7 §7.2.5 verbatim, plus [[nodiscard]] (clang-tidy
// modernize-use-nodiscard). No signature changes. Implemented in core/src/bitreader/rbsp.cpp (WI-1.104).

#ifndef OVT_CXX_RBSP_HPP
#define OVT_CXX_RBSP_HPP

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ovt {

struct Rbsp {
  std::vector<uint8_t> rbsp;            // NAL header bytes + payload with EPBs removed
  std::vector<uint32_t> epb_positions;  // RAW byte offsets (within the NAL unit, header byte 0 = 0) of each
                                        // removed 0x03, ascending
};

// H.264 §7.3.1 / H.265 §7.3.1.1 nal_unit(): removes emulation_prevention_three_byte. `nal_bytes` excludes
// the start code or length prefix. Every 0x03 preceded by two 0x00 bytes (counted after removal) is an EPB,
// whatever follows it, including a 00 00 03 at the very end of the unit (cabac_zero_word padding).
// Throws std::length_error when nal_bytes.size() > UINT32_MAX, since epb_positions are 32-bit.
[[nodiscard]] Rbsp rbsp_from_nal(std::span<const uint8_t> nal_bytes);

// Mapping helpers used by the hex view (both O(log n) by binary search over epb_positions):
[[nodiscard]] uint64_t rbsp_bit_to_raw_bit(uint64_t rbsp_bit, std::span<const uint32_t> epb_positions);
// raw_byte_is_epb → true when the raw byte is a removed 0x03 (the hex view paints it grey; it has no RBSP
// position). For an EPB the returned RBSP byte is the position the *next* RBSP byte has.
[[nodiscard]] std::pair<uint64_t, bool> raw_byte_to_rbsp_byte(uint64_t raw_byte,
                                                              std::span<const uint32_t> epb_positions);

}  // namespace ovt

#endif  // OVT_CXX_RBSP_HPP
