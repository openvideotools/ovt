// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 global_motion_params() (chapter 7 §7.3.6 row 26; WI-1.118) with read_global_param(),
// decode_signed_subexp_with_ref(), decode_unsigned_subexp_with_ref(), decode_subexp() and inverse_recenter()
// (AV1 §5.9.24 – §5.9.29). The subexp_* syntax elements become nodes of their own, as FFmpeg's trace_headers prints
// them; the reconstructed gm_params[ref][idx] is a derived node next to them.

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecGlobalMotion = "AV1 5.9.24";
constexpr std::string_view kSpecGlobalParam = "AV1 5.9.25";

// decode_subexp(numSyms) (AV1 §5.9.28).
int64_t decode_subexp(SyntaxBuilder& b, int64_t numSyms) {
  int64_t i = 0;
  int64_t mk = 0;
  constexpr int64_t k = 3;
  while (true) {
    const int64_t b2 = i != 0 ? k + i - 1 : k;
    const int64_t a = int64_t{1} << b2;
    if (numSyms <= mk + 3 * a) {
      const auto subexp_final_bits = b.read_ns("subexp_final_bits", static_cast<uint32_t>(numSyms - mk));
      return subexp_final_bits + mk;
    }
    if (b.read_flag("subexp_more_bits")) {
      ++i;
      mk += a;
    } else {
      const auto subexp_bits = static_cast<int64_t>(b.read_u("subexp_bits", static_cast<unsigned>(b2)));
      return subexp_bits + mk;
    }
  }
}

// decode_unsigned_subexp_with_ref(mx, r) (AV1 §5.9.27).
int64_t decode_unsigned_subexp_with_ref(SyntaxBuilder& b, int64_t mx, int64_t r) {
  const int64_t v = decode_subexp(b, mx);
  if (r * 2 <= mx) {
    return inverse_recenter(static_cast<int32_t>(r), static_cast<int32_t>(v));
  }
  return mx - 1 - inverse_recenter(static_cast<int32_t>(mx - 1 - r), static_cast<int32_t>(v));
}

// decode_signed_subexp_with_ref(low, high, r) (AV1 §5.9.26).
int64_t decode_signed_subexp_with_ref(SyntaxBuilder& b, int64_t low, int64_t high, int64_t r) {
  const int64_t x = decode_unsigned_subexp_with_ref(b, high - low, r - low);
  return x + low;
}

// read_global_param(type, ref, idx) (AV1 §5.9.25).
void read_global_param(SyntaxBuilder& b, GlobalMotionType type, int ref, int idx, bool allow_high_precision_mv,
                       const GmParams& PrevGmParams, GmParams& gm_params) {
  int absBits = kGmAbsAlphaBits;
  int precBits = kGmAlphaPrecBits;
  if (idx < 2) {
    if (type == GlobalMotionType::Translation) {
      absBits = kGmAbsTransOnlyBits - (allow_high_precision_mv ? 0 : 1);
      precBits = kGmTransOnlyPrecBits - (allow_high_precision_mv ? 0 : 1);
    } else {
      absBits = kGmAbsTransBits;
      precBits = kGmTransPrecBits;
    }
  }
  const int precDiff = kWarpedModelPrecBits - precBits;
  const int64_t round = (idx % 3) == 2 ? (int64_t{1} << kWarpedModelPrecBits) : 0;
  const int64_t sub = (idx % 3) == 2 ? (int64_t{1} << precBits) : 0;
  const int64_t mx = int64_t{1} << absBits;
  const auto sref = static_cast<size_t>(ref);
  const auto sidx = static_cast<size_t>(idx);
  const int64_t r = (int64_t{PrevGmParams[sref][sidx]} >> precDiff) - sub;
  b.begin_array_element("gm_params", idx, kSpecGlobalParam);
  const int64_t decoded = decode_signed_subexp_with_ref(b, -mx, mx + 1, r);
  gm_params[sref][sidx] = static_cast<int32_t>(decoded * (int64_t{1} << precDiff) + round);
  b.derived("gm_params", fmt::format("gm_params[{}][{}] = {}", ref, idx, gm_params[sref][sidx]));
  b.end_array_element();
}

}  // namespace

std::string_view gm_type_name(GlobalMotionType t) noexcept {
  switch (t) {
    case GlobalMotionType::Identity:
      return "IDENTITY";
    case GlobalMotionType::Translation:
      return "TRANSLATION";
    case GlobalMotionType::RotZoom:
      return "ROTZOOM";
    case GlobalMotionType::Affine:
      return "AFFINE";
  }
  return "";
}

int32_t inverse_recenter(int32_t r, int32_t v) noexcept {
  // AV1 §5.9.29, in 64 bits so that no operand range can overflow.
  const int64_t r64 = r;
  const int64_t v64 = v;
  if (v64 > 2 * r64) {
    return v;
  }
  if ((v64 & 1) != 0) {
    return static_cast<int32_t>(r64 - ((v64 + 1) >> 1));
  }
  return static_cast<int32_t>(r64 + (v64 >> 1));
}

GlobalMotionParams parse_global_motion_params(BitReader& br, SyntaxBuilder& b, bool FrameIsIntra,
                                              bool allow_high_precision_mv, const GmParams& PrevGmParams) {
  // AV1 §5.9.24
  GlobalMotionParams g;
  const auto start = br.mark();
  auto s = b.scoped("global_motion_params", kSpecGlobalMotion);
  g.gm_params = identity_gm_params();
  g.GmType.fill(GlobalMotionType::Identity);
  if (FrameIsIntra) {
    b.derived("global_motion_params", "GmType[LAST_FRAME..ALTREF_FRAME] = IDENTITY (FrameIsIntra)");
    g.span = br.span_since(start);
    return g;
  }
  for (int ref = kLastFrame; ref <= kAltrefFrame; ++ref) {
    const auto sref = static_cast<size_t>(ref);
    b.begin_array_element("gm", ref, kSpecGlobalMotion);
    GlobalMotionType type = GlobalMotionType::Identity;
    g.is_global[sref] = b.read_flag("is_global");
    b.set_enum(ref_frame_name(ref));
    if (g.is_global[sref]) {
      if (b.read_flag("is_rot_zoom")) {
        type = GlobalMotionType::RotZoom;
      } else {
        type = b.read_flag("is_translation") ? GlobalMotionType::Translation : GlobalMotionType::Affine;
      }
    }
    g.GmType[sref] = type;
    if (type >= GlobalMotionType::RotZoom) {
      read_global_param(b, type, ref, 2, allow_high_precision_mv, PrevGmParams, g.gm_params);
      read_global_param(b, type, ref, 3, allow_high_precision_mv, PrevGmParams, g.gm_params);
      if (type == GlobalMotionType::Affine) {
        read_global_param(b, type, ref, 4, allow_high_precision_mv, PrevGmParams, g.gm_params);
        read_global_param(b, type, ref, 5, allow_high_precision_mv, PrevGmParams, g.gm_params);
      } else {
        g.gm_params[sref][4] = -g.gm_params[sref][3];
        g.gm_params[sref][5] = g.gm_params[sref][2];
      }
    }
    if (type >= GlobalMotionType::Translation) {
      read_global_param(b, type, ref, 0, allow_high_precision_mv, PrevGmParams, g.gm_params);
      read_global_param(b, type, ref, 1, allow_high_precision_mv, PrevGmParams, g.gm_params);
    }
    if (type != GlobalMotionType::Identity) {
      const auto& p = g.gm_params[sref];
      b.derived("GmType", fmt::format("GmType[{}] = {}, gm_params = {{{}, {}, {}, {}, {}, {}}}", ref_frame_name(ref),
                                      gm_type_name(type), p[0], p[1], p[2], p[3], p[4], p[5]));
    }
    b.end_array_element();
  }
  g.span = br.span_since(start);
  return g;
}

}  // namespace ovt::syntax::av1
