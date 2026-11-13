// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 nal_unit_header() parser (H.265 §7.3.1.2; chapter 7 §7.3.5 row 1, WI-1.112).

#include "ovt/cxx/syntax/h265/nal_header.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace ovt::syntax::h265 {

namespace {

constexpr std::array<std::string_view, 64> kNalUnitTypeNames = {
    "TRAIL_N",     "TRAIL_R",        "TSA_N",          "TSA_R",       "STSA_N",         "STSA_R",         "RADL_N",
    "RADL_R",      "RASL_N",         "RASL_R",         "RSV_VCL_N10", "RSV_VCL_R11",    "RSV_VCL_N12",    "RSV_VCL_R13",
    "RSV_VCL_N14", "RSV_VCL_R15",    "BLA_W_LP",       "BLA_W_RADL",  "BLA_N_LP",       "IDR_W_RADL",     "IDR_N_LP",
    "CRA_NUT",     "RSV_IRAP_VCL22", "RSV_IRAP_VCL23", "RSV_VCL24",   "RSV_VCL25",      "RSV_VCL26",      "RSV_VCL27",
    "RSV_VCL28",   "RSV_VCL29",      "RSV_VCL30",      "RSV_VCL31",   "VPS_NUT",        "SPS_NUT",        "PPS_NUT",
    "AUD_NUT",     "EOS_NUT",        "EOB_NUT",        "FD_NUT",      "PREFIX_SEI_NUT", "SUFFIX_SEI_NUT", "RSV_NVCL41",
    "RSV_NVCL42",  "RSV_NVCL43",     "RSV_NVCL44",     "RSV_NVCL45",  "RSV_NVCL46",     "RSV_NVCL47",     "UNSPEC48",
    "UNSPEC49",    "UNSPEC50",       "UNSPEC51",       "UNSPEC52",    "UNSPEC53",       "UNSPEC54",       "UNSPEC55",
    "UNSPEC56",    "UNSPEC57",       "UNSPEC58",       "UNSPEC59",    "UNSPEC60",       "UNSPEC61",       "UNSPEC62",
    "UNSPEC63"};

}  // namespace

std::string_view nal_unit_type_name(uint8_t nal_unit_type) noexcept {
  return nal_unit_type < kNalUnitTypeNames.size() ? kNalUnitTypeNames[nal_unit_type] : std::string_view{"?"};
}

NalHeader parse_nal_header(BitReader& br, SyntaxBuilder& b) {
  NalHeader h;
  const uint64_t start = br.bit_pos();
  {
    auto s = b.scoped("nal_unit_header", "H.265 7.3.1.2");
    h.forbidden_zero_bit = static_cast<uint8_t>(b.read_f("forbidden_zero_bit", 1, 0));
    h.nal_unit_type = static_cast<uint8_t>(b.read_u("nal_unit_type", 6));
    b.set_enum(nal_unit_type_name(h.nal_unit_type));
    h.nuh_layer_id = static_cast<uint8_t>(b.read_u("nuh_layer_id", 6));
    h.nuh_temporal_id_plus1 = static_cast<uint8_t>(b.read_u("nuh_temporal_id_plus1", 3));
    if (h.nuh_temporal_id_plus1 == 0) {
      throw ParseError{"SYN-H265-0001", "nuh_temporal_id_plus1 == 0", br.bit_pos() - 3};
    }
    h.TemporalId = static_cast<uint8_t>(h.nuh_temporal_id_plus1 - 1);
    const uint8_t t = h.nal_unit_type;
    h.is_vcl = t < kVpsNut;
    h.is_irap = t >= kBlaWLp && t <= 23;
    h.is_idr = t == kIdrWRadl || t == kIdrNLp;
    h.is_bla = t >= kBlaWLp && t <= kBlaNLp;
    h.is_cra = t == kCraNut;
    h.is_rasl = t == kRaslN || t == kRaslR;
    h.is_radl = t == kRadlN || t == kRadlR;
    h.is_sub_layer_non_ref = t <= 14 && (t % 2) == 0;
    b.derived("TemporalId", h.TemporalId);
    b.derived("is_vcl", h.is_vcl);
    if (h.is_irap) {
      b.derived("is_irap", true);
    }
    if (h.is_sub_layer_non_ref) {
      b.derived("is_sub_layer_non_ref", true);
    }
  }
  h.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return h;
}

}  // namespace ovt::syntax::h265
