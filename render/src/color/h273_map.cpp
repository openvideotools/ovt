// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.402: chapter 10 §10.3.3 tables as switch statements.

#include "color/h273_map.h"

#include <libplacebo/colorspace.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ovt/color.h"

namespace ovt::render::detail {
namespace {

constexpr const char* kRnd0020 = "RND-0020";
constexpr const char* kRnd0021 = "RND-0021";

// Codes 0 (reserved; what a zero-initialised description holds) and 2 (unspecified) fall back silently; every other
// code outside the tables raises RND-0020.
// SPEC-ASSUMPTION(#237): "unknown" means "outside the §10.3.3 tables", not the unspecified codes 0 and 2.
bool is_unspecified(uint8_t code) {
  return code == 0 || code == 2;
}

const char* primaries_name(pl_color_primaries p) {
  switch (p) {
    case PL_COLOR_PRIM_BT_709:
      return "BT.709";
    case PL_COLOR_PRIM_BT_601_525:
      return "BT.601-525";
    case PL_COLOR_PRIM_BT_601_625:
      return "BT.601-625";
    default:
      return "libplacebo's guess";
  }
}

const char* system_name(pl_color_system s) {
  switch (s) {
    case PL_COLOR_SYSTEM_BT_709:
      return "BT.709";
    case PL_COLOR_SYSTEM_BT_601:
      return "BT.601";
    default:
      return "libplacebo's guess";
  }
}

const char* field_name(H273Field f) {
  switch (f) {
    case H273Field::Primaries:
      return "primaries";
    case H273Field::Transfer:
      return "transfer";
    case H273Field::Matrix:
      return "matrix";
  }
  return "?";
}

// 0.00002 chromaticity units → CIE xy.
constexpr float kChromaUnit = 2e-5F;
// 0.0001 cd/m² units → cd/m².
constexpr float kLumaUnit = 1e-4F;

pl_cie_xy chroma(const uint16_t (&xy)[2]) {  // NOLINT(modernize-avoid-c-arrays): ovt_ColorDescription is C
  return pl_cie_xy{.x = static_cast<float>(xy[0]) * kChromaUnit, .y = static_cast<float>(xy[1]) * kChromaUnit};
}

void fnv1a(uint64_t& h, uint64_t value, int bytes) {
  constexpr uint64_t kPrime = 0x100000001b3ULL;
  for (int i = 0; i < bytes; ++i) {
    h ^= (value >> (8 * i)) & 0xFFU;
    h *= kPrime;
  }
}

}  // namespace

pl_color_primaries map_primaries(uint8_t code, int width, int height, H273Notices* notices) {
  switch (code) {
    case 1:
      return PL_COLOR_PRIM_BT_709;
    case 4:
      return PL_COLOR_PRIM_BT_470M;
    case 5:
      return PL_COLOR_PRIM_BT_601_625;
    case 6:
    case 7:
      return PL_COLOR_PRIM_BT_601_525;
    case 8:
      return PL_COLOR_PRIM_FILM_C;
    case 9:
      return PL_COLOR_PRIM_BT_2020;
    case 10:
      return PL_COLOR_PRIM_CIE_1931;
    case 11:
      return PL_COLOR_PRIM_DCI_P3;
    case 12:
      return PL_COLOR_PRIM_DISPLAY_P3;
    case 22:
      return PL_COLOR_PRIM_EBU_3213;
    default:
      break;
  }
  const pl_color_primaries guess = pl_color_primaries_guess(width, height);
  if (notices != nullptr && !is_unspecified(code)) {
    notices->add({.code = kRnd0020, .field = H273Field::Primaries, .value = code, .assumed = primaries_name(guess)});
  }
  return guess;
}

pl_color_transfer map_transfer(uint8_t code, H273Notices* notices) {
  switch (code) {
    case 1:
    case 6:
    case 14:
    case 15:
    case 7:  // SMPTE 240M is displayed via BT.1886 as well (§10.3.3)
      return PL_COLOR_TRC_BT_1886;
    case 4:
      return PL_COLOR_TRC_GAMMA22;
    case 5:
      return PL_COLOR_TRC_GAMMA28;
    case 8:
      return PL_COLOR_TRC_LINEAR;
    case 11:  // IEC 61966-2-4 (xvYCC)
    case 12:  // BT.1361
      if (notices != nullptr) {
        notices->add({.code = kRnd0020, .field = H273Field::Transfer, .value = code, .assumed = "BT.1886"});
      }
      return PL_COLOR_TRC_BT_1886;
    case 13:
      return PL_COLOR_TRC_SRGB;
    case 16:
      return PL_COLOR_TRC_PQ;
    case 17:
      return PL_COLOR_TRC_ST428;
    case 18:
      return PL_COLOR_TRC_HLG;
    default:
      break;
  }
  if (notices != nullptr && !is_unspecified(code)) {
    notices->add({.code = kRnd0020, .field = H273Field::Transfer, .value = code, .assumed = "BT.1886"});
  }
  return PL_COLOR_TRC_BT_1886;  // PL_COLOR_TRC_UNKNOWN → BT.1886
}

pl_color_system map_matrix(uint8_t matrix, uint8_t transfer, int width, int height, H273Notices* notices) {
  switch (matrix) {
    case 0:
      return PL_COLOR_SYSTEM_RGB;
    case 1:
      return PL_COLOR_SYSTEM_BT_709;
    case 5:
    case 6:
      return PL_COLOR_SYSTEM_BT_601;
    case 7:
      return PL_COLOR_SYSTEM_SMPTE_240M;
    case 8:
      return PL_COLOR_SYSTEM_YCGCO;
    case 9:
      return PL_COLOR_SYSTEM_BT_2020_NC;
    case 10:
      return PL_COLOR_SYSTEM_BT_2020_C;
    case 14:
      if (transfer == 18) {
        return PL_COLOR_SYSTEM_BT_2100_HLG;
      }
      // SPEC-ASSUMPTION(#237): ICtCp with a transfer other than PQ (16) or HLG (18) renders as the PQ variant.
      if (transfer != 16 && notices != nullptr) {
        notices->add({.code = kRnd0020, .field = H273Field::Matrix, .value = matrix, .assumed = "BT.2100 PQ ICtCp"});
      }
      return PL_COLOR_SYSTEM_BT_2100_PQ;
    case 12:
    case 13:
      if (notices != nullptr) {
        notices->add({.code = kRnd0021, .field = H273Field::Matrix, .value = matrix, .assumed = "BT.709"});
      }
      return PL_COLOR_SYSTEM_BT_709;
    default:
      break;
  }
  const pl_color_system guess = pl_color_system_guess_ycbcr(width, height);
  if (notices != nullptr && matrix != 2) {
    notices->add({.code = kRnd0020, .field = H273Field::Matrix, .value = matrix, .assumed = system_name(guess)});
  }
  return guess;
}

H273Mapping map_color_description(const ovt_ColorDescription& color, int width, int height) {
  H273Mapping m;
  m.repr.sys = map_matrix(color.matrix, color.transfer, width, height, &m.notices);
  m.repr.levels = color.full_range != 0 ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
#if PL_API_VER >= 344
  m.repr.alpha = PL_ALPHA_NONE;
#else
  m.repr.alpha = PL_ALPHA_UNKNOWN;  // libplacebo < 7.344 (distro builds): "unknown or no alpha channel"
#endif
  m.space.primaries = map_primaries(color.primaries, width, height, &m.notices);
  m.space.transfer = map_transfer(color.transfer, &m.notices);

  pl_hdr_metadata& hdr = m.space.hdr;
  // 0 = absent: libplacebo infers the luminance range from the transfer (PQ 10000, HLG 1000, SDR 203).
  hdr.min_luma = static_cast<float>(color.mdcv_min_luminance) * kLumaUnit;
  hdr.max_luma = static_cast<float>(color.mdcv_max_luminance) * kLumaUnit;
  hdr.max_cll = static_cast<float>(color.max_cll);  // already cd/m²
  hdr.max_fall = static_cast<float>(color.max_fall);
  // mdcv_primaries is always R, G, B (chapter 7 §7.3.5 row 15): no reordering here.
  hdr.prim.red = chroma(color.mdcv_primaries[0]);
  hdr.prim.green = chroma(color.mdcv_primaries[1]);
  hdr.prim.blue = chroma(color.mdcv_primaries[2]);
  hdr.prim.white = chroma(color.mdcv_white_point);
  return m;
}

uint64_t color_description_hash(const ovt_ColorDescription& c) {
  uint64_t h = 0xcbf29ce484222325ULL;
  fnv1a(h, c.struct_size, 4);
  for (const uint8_t v :
       {c.primaries, c.transfer, c.matrix, c.full_range, c.chroma_siting, c.chroma_format, c.bit_depth}) {
    fnv1a(h, v, 1);
  }
  for (const auto& xy : c.mdcv_primaries) {
    fnv1a(h, xy[0], 2);
    fnv1a(h, xy[1], 2);
  }
  fnv1a(h, c.mdcv_white_point[0], 2);
  fnv1a(h, c.mdcv_white_point[1], 2);
  fnv1a(h, c.mdcv_max_luminance, 4);
  fnv1a(h, c.mdcv_min_luminance, 4);
  fnv1a(h, c.max_cll, 2);
  fnv1a(h, c.max_fall, 2);
  for (const uint8_t v : {c.has_hdr10plus, c.has_dolby_vision, c.dv_profile, c.dv_level}) {
    fnv1a(h, v, 1);
  }
  return h;
}

std::string format_notice(const H273Notice& n) {
  const std::string value = std::to_string(n.value);
  if (std::string(n.code) == kRnd0021) {
    return std::string(kRnd0021) + " matrix coefficients " + value +
           " (chromaticity-derived) are unsupported; rendered approximately as BT.709";
  }
  return std::string(n.code) + " unknown H.273 code " + value + " for " + field_name(n.field) + ", assuming " +
         n.assumed;
}

std::vector<std::string> H273NoticeLog::first_seen(const ovt_ColorDescription& color, const H273Mapping& mapping) {
  std::vector<std::string> lines;
  if (mapping.notices.count == 0 || !seen_.insert(color_description_hash(color)).second) {
    return lines;
  }
  for (size_t i = 0; i < mapping.notices.count; ++i) {
    lines.push_back(format_notice(mapping.notices.items.at(i)));
  }
  return lines;
}

}  // namespace ovt::render::detail
