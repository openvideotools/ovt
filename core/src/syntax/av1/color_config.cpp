// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 color_config() and the ovt_ColorDescription candidate (chapter 7 §7.3.6 rows 2 and 6; WI-1.116).

#include <fmt/format.h>

#include <cstdint>
#include <cstring>

#include "ovt/cxx/syntax/av1/sequence_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecColorConfig = "AV1 5.5.2";

void set_subsampling(ColorConfig& c, bool x, bool y) noexcept {
  c.subsampling_x = x;
  c.subsampling_y = y;
}

uint8_t chroma_format_of(const ColorConfig& c) noexcept {
  if (c.mono_chrome) {
    return 0;
  }
  if (c.subsampling_x && c.subsampling_y) {
    return 1;
  }
  return c.subsampling_x ? 2 : 3;
}

std::string_view chroma_sample_position_name(ChromaSamplePosition p) noexcept {
  switch (p) {
    case ChromaSamplePosition::Unknown:
      return "CSP_UNKNOWN";
    case ChromaSamplePosition::Vertical:
      return "CSP_VERTICAL";
    case ChromaSamplePosition::Colocated:
      return "CSP_COLOCATED";
    case ChromaSamplePosition::Reserved:
    default:
      return "CSP_RESERVED";
  }
}

// color_description_present_flag and the three 8-bit codes, or their Unspecified defaults (AV1 §5.5.2).
void parse_color_description(SyntaxBuilder& b, ColorConfig& c) {
  c.color_description_present_flag = b.read_flag("color_description_present_flag");
  if (c.color_description_present_flag) {
    c.color_primaries = static_cast<uint8_t>(b.read_u("color_primaries", 8));
    c.transfer_characteristics = static_cast<uint8_t>(b.read_u("transfer_characteristics", 8));
    c.matrix_coefficients = static_cast<uint8_t>(b.read_u("matrix_coefficients", 8));
  } else {
    c.color_primaries = kCpUnspecified;
    c.transfer_characteristics = kTcUnspecified;
    c.matrix_coefficients = kMcUnspecified;
  }
}

// The non-monochrome branch of color_config() (AV1 §5.5.2): sRGB inference, color_range, the profile-dependent
// subsampling and chroma_sample_position.
void parse_chroma_layout(SyntaxBuilder& b, ColorConfig& c, uint8_t seq_profile) {
  if (c.color_primaries == kCpBt709 && c.transfer_characteristics == kTcSrgb && c.matrix_coefficients == kMcIdentity) {
    c.color_range = true;
    set_subsampling(c, false, false);
  } else {
    c.color_range = b.read_flag("color_range");
    if (seq_profile == 0) {
      set_subsampling(c, true, true);
    } else if (seq_profile == 1) {
      set_subsampling(c, false, false);
    } else if (c.BitDepth == 12) {
      c.subsampling_x = b.read_flag("subsampling_x");
      c.subsampling_y = c.subsampling_x ? b.read_flag("subsampling_y") : false;
    } else {
      set_subsampling(c, true, false);
    }
    if (c.subsampling_x && c.subsampling_y) {
      c.chroma_sample_position = static_cast<ChromaSamplePosition>(b.read_u("chroma_sample_position", 2));
      b.set_enum(chroma_sample_position_name(c.chroma_sample_position));
      if (c.chroma_sample_position == ChromaSamplePosition::Reserved) {
        // SPEC-ASSUMPTION(#321): reserved value, Warning SYN-AV1-0004, parsing continues (§7.3.1 rule 3).
        b.diag(Severity::Warning, "SYN-AV1-0004", "chroma_sample_position: reserved value 3", {}, true);
      }
    }
  }
}

}  // namespace

ColorConfig parse_color_config(BitReader& br, SyntaxBuilder& b, uint8_t seq_profile) {
  ColorConfig c;
  const auto start = br.mark();
  auto s = b.scoped("color_config", kSpecColorConfig);
  c.high_bitdepth = b.read_flag("high_bitdepth");
  if (seq_profile == 2 && c.high_bitdepth) {
    c.twelve_bit = b.read_flag("twelve_bit");
    c.BitDepth = c.twelve_bit ? 12 : 10;
  } else {
    c.BitDepth = c.high_bitdepth ? 10 : 8;
  }
  b.derived("BitDepth", c.BitDepth);
  if (seq_profile == 1) {
    c.mono_chrome = false;  // never read for the High profile (WI-1.116 pitfalls)
  } else {
    c.mono_chrome = b.read_flag("mono_chrome");
  }
  c.NumPlanes = c.mono_chrome ? 1 : 3;
  b.derived("NumPlanes", c.NumPlanes);
  parse_color_description(b, c);
  if (c.mono_chrome) {
    c.color_range = b.read_flag("color_range");
    set_subsampling(c, true, true);
    c.chroma_sample_position = ChromaSamplePosition::Unknown;
    c.separate_uv_delta_q = false;
  } else {
    parse_chroma_layout(b, c, seq_profile);
    c.separate_uv_delta_q = b.read_flag("separate_uv_delta_q");
  }
  c.chroma_format = chroma_format_of(c);
  c.chroma_siting = c.chroma_sample_position == ChromaSamplePosition::Colocated  ? 3
                    : c.chroma_sample_position == ChromaSamplePosition::Vertical ? 1
                                                                                 : 0;
  b.derived("subsampling",
            fmt::format("subsampling_x = {}, subsampling_y = {}", c.subsampling_x ? 1 : 0, c.subsampling_y ? 1 : 0));
  b.derived("chroma_format", fmt::format("chroma_format = 4:{}", c.chroma_format == 0   ? "0:0"
                                                                 : c.chroma_format == 1 ? "2:0"
                                                                 : c.chroma_format == 2 ? "2:2"
                                                                                        : "4:4"));
  c.span = br.span_since(start);
  return c;
}

ovt_ColorDescription color_description(const SequenceHeader& seq) noexcept {
  ovt_ColorDescription d;
  std::memset(&d, 0, sizeof d);
  d.struct_size = sizeof(ovt_ColorDescription);
  const ColorConfig& c = seq.color_config;
  d.primaries = c.color_primaries;
  d.transfer = c.transfer_characteristics;
  d.matrix = c.matrix_coefficients;
  d.full_range = c.color_range ? 1 : 0;
  d.chroma_siting = c.chroma_siting;
  d.chroma_format = c.chroma_format;
  d.bit_depth = c.BitDepth;
  return d;
}

}  // namespace ovt::syntax::av1
