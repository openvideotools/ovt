// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 nal_unit_header() (H.265 §7.3.1.2): typed struct and parser (chapter 7 §7.3.5 row 1, §7.3.7; WI-1.112).
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_SYNTAX_H265_NAL_HEADER_HPP
#define OVT_CXX_SYNTAX_H265_NAL_HEADER_HPP

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h265 {

/// nal_unit_type values of H.265 Table 7-1 that the parsers test for.
enum NalUnitType : uint8_t {
  kTrailN = 0,
  kTrailR = 1,
  kTsaN = 2,
  kTsaR = 3,
  kStsaN = 4,
  kStsaR = 5,
  kRadlN = 6,
  kRadlR = 7,
  kRaslN = 8,
  kRaslR = 9,
  kBlaWLp = 16,
  kBlaWRadl = 17,
  kBlaNLp = 18,
  kIdrWRadl = 19,
  kIdrNLp = 20,
  kCraNut = 21,
  kVpsNut = 32,
  kSpsNut = 33,
  kPpsNut = 34,
  kAudNut = 35,
  kEosNut = 36,
  kEobNut = 37,
  kFdNut = 38,
  kPrefixSeiNut = 39,
  kSuffixSeiNut = 40
};

/// Table 7-1 name of a nal_unit_type ("TRAIL_R", "RSV_VCL_N10", "UNSPEC62", …).
[[nodiscard]] std::string_view nal_unit_type_name(uint8_t nal_unit_type) noexcept;

/// Typed nal_unit_header() (§7.3.7). Syntax elements carry the spec names; derived values follow §7.3.5 row 1.
struct NalHeader {
  // syntax elements
  uint8_t forbidden_zero_bit = 0;
  uint8_t nal_unit_type = 0;
  uint8_t nuh_layer_id = 0;
  uint8_t nuh_temporal_id_plus1 = 1;
  // derived
  uint8_t TemporalId = 0;             // nuh_temporal_id_plus1 - 1
  bool is_vcl = false;                // nal_unit_type < 32
  bool is_irap = false;               // 16..23
  bool is_idr = false;                // 19, 20
  bool is_bla = false;                // 16..18
  bool is_cra = false;                // 21
  bool is_rasl = false;               // 8, 9
  bool is_radl = false;               // 6, 7
  bool is_sub_layer_non_ref = false;  // nal_unit_type <= 14 && even
  BitSpan span{0, 0};
  const SyntaxNodeBuffer* tree = nullptr;  // set by the owning Unit
};

/// Parses the two header bytes at the reader's position (bit 0 of the unit) into the node `nal_unit_header`.
/// nuh_temporal_id_plus1 == 0 throws ParseError SYN-H265-0001; forbidden_zero_bit == 1 is Diagnostic Error
/// SYN-GEN-0001 and parsing continues.
NalHeader parse_nal_header(BitReader& br, SyntaxBuilder& b);

}  // namespace ovt::syntax::h265

#endif  // OVT_CXX_SYNTAX_H265_NAL_HEADER_HPP
