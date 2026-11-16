// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 film_grain_params() (chapter 7 §7.3.6 row 27; WI-1.118) with reset_grain_params() and load_grain_params()
// (AV1 §5.9.30, §6.8.20, §7.21).

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecFilmGrain = "AV1 5.9.30";
constexpr std::string_view kSpecFilmGrainSemantics = "AV1 6.8.20";

// reset_grain_params(): every film_grain_params() syntax element 0.
FilmGrainParams reset_grain_params() noexcept {
  FilmGrainParams fg;
  fg.update_grain = false;
  return fg;
}

// SPEC-ASSUMPTION(#533): §7.3.6 row 27 asks for a Diagnostic on these AV1 §6.8.20 conformance requirements but
// §7.9 registers no code; SYN-AV1-0015 (frame-header value outside a conformance requirement, #513) is used.
void conformance(SyntaxBuilder& b, std::string_view message) {
  b.diag(Severity::Error, kDiagFrameHeaderRange, message, kSpecFilmGrainSemantics);
}

// point_<name>_value[i] f(8), point_<name>_scaling[i] f(8) for i < count; values must increase (AV1 §6.8.20).
void read_points(SyntaxBuilder& b, std::string_view plane, uint8_t count, std::array<uint8_t, 16>& value,
                 std::array<uint8_t, 16>& scaling) {
  const std::string value_name = fmt::format("point_{}_value", plane);
  const std::string scaling_name = fmt::format("point_{}_scaling", plane);
  for (int i = 0; i < count; ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element(value_name, i, kSpecFilmGrain);
    value[si] = static_cast<uint8_t>(b.read_u(value_name, 8));
    if (i > 0 && value[si] <= value[si - 1]) {
      conformance(b,
                  fmt::format("{}[{}] = {} does not increase (previous {})", value_name, i, value[si], value[si - 1]));
    }
    scaling[si] = static_cast<uint8_t>(b.read_u(scaling_name, 8));
    b.end_array_element();
  }
}

template <size_t N>
void read_ar_coeffs(SyntaxBuilder& b, std::string_view name, int count, std::array<uint8_t, N>& out) {
  for (int i = 0; i < count; ++i) {
    b.begin_array_element(name, i, kSpecFilmGrain);
    out[static_cast<size_t>(i)] = static_cast<uint8_t>(b.read_u(name, 8));
    b.end_array_element();
  }
}

// The num_cb_points … point_cr_scaling part of film_grain_params() (AV1 §5.9.30).
void read_chroma_points(SyntaxBuilder& b, const ColorConfig& cc, FilmGrainParams& fg) {
  const bool ss420 = cc.subsampling_x && cc.subsampling_y;
  if (cc.mono_chrome || fg.chroma_scaling_from_luma || (ss420 && fg.num_y_points == 0)) {
    fg.num_cb_points = 0;
    fg.num_cr_points = 0;
  } else {
    fg.num_cb_points = static_cast<uint8_t>(b.read_u("num_cb_points", 4));
    if (fg.num_cb_points > kMaxNumCbCrPoints) {
      conformance(b, fmt::format("num_cb_points = {} exceeds {}", fg.num_cb_points, kMaxNumCbCrPoints));
    }
    read_points(b, "cb", fg.num_cb_points, fg.point_cb_value, fg.point_cb_scaling);
    fg.num_cr_points = static_cast<uint8_t>(b.read_u("num_cr_points", 4));
    if (fg.num_cr_points > kMaxNumCbCrPoints) {
      conformance(b, fmt::format("num_cr_points = {} exceeds {}", fg.num_cr_points, kMaxNumCbCrPoints));
    }
    read_points(b, "cr", fg.num_cr_points, fg.point_cr_value, fg.point_cr_scaling);
    if (ss420 && ((fg.num_cb_points == 0) != (fg.num_cr_points == 0))) {
      conformance(b, fmt::format("4:2:0 with num_cb_points = {} and num_cr_points = {} (both or neither must be 0)",
                                 fg.num_cb_points, fg.num_cr_points));
    }
  }
}

// The grain_scaling_minus_8 … cr_offset part of film_grain_params() (AV1 §5.9.30).
void read_ar_and_scaling(SyntaxBuilder& b, FilmGrainParams& fg) {
  fg.grain_scaling_minus_8 = static_cast<uint8_t>(b.read_u("grain_scaling_minus_8", 2));
  fg.ar_coeff_lag = static_cast<uint8_t>(b.read_u("ar_coeff_lag", 2));
  const int numPosLuma = 2 * fg.ar_coeff_lag * (fg.ar_coeff_lag + 1);
  int numPosChroma = numPosLuma;
  if (fg.num_y_points != 0) {
    numPosChroma = numPosLuma + 1;
    read_ar_coeffs(b, "ar_coeffs_y_plus_128", numPosLuma, fg.ar_coeffs_y_plus_128);
  }
  if (fg.chroma_scaling_from_luma || fg.num_cb_points != 0) {
    read_ar_coeffs(b, "ar_coeffs_cb_plus_128", numPosChroma, fg.ar_coeffs_cb_plus_128);
  }
  if (fg.chroma_scaling_from_luma || fg.num_cr_points != 0) {
    read_ar_coeffs(b, "ar_coeffs_cr_plus_128", numPosChroma, fg.ar_coeffs_cr_plus_128);
  }
  fg.ar_coeff_shift_minus_6 = static_cast<uint8_t>(b.read_u("ar_coeff_shift_minus_6", 2));
  fg.grain_scale_shift = static_cast<uint8_t>(b.read_u("grain_scale_shift", 2));
  if (fg.num_cb_points != 0) {
    fg.cb_mult = static_cast<uint8_t>(b.read_u("cb_mult", 8));
    fg.cb_luma_mult = static_cast<uint8_t>(b.read_u("cb_luma_mult", 8));
    fg.cb_offset = static_cast<uint16_t>(b.read_u("cb_offset", 9));
  }
  if (fg.num_cr_points != 0) {
    fg.cr_mult = static_cast<uint8_t>(b.read_u("cr_mult", 8));
    fg.cr_luma_mult = static_cast<uint8_t>(b.read_u("cr_luma_mult", 8));
    fg.cr_offset = static_cast<uint16_t>(b.read_u("cr_offset", 9));
  }
}

}  // namespace

FilmGrainParams load_grain_params(const DecoderState& state, uint8_t idx) noexcept {
  return state.slot(std::min<int>(idx, kNumRefFrames - 1)).SavedFilmGrainParams;
}

FilmGrainParams parse_film_grain_params(BitReader& /*br*/, SyntaxBuilder& b, const SequenceHeader& seq,
                                        const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                        FrameType frame_type, bool show_frame, bool showable_frame) {
  // AV1 §5.9.30
  auto s = b.scoped("film_grain_params", kSpecFilmGrain);
  const ColorConfig& cc = seq.color_config;
  if (!seq.film_grain_params_present || (!show_frame && !showable_frame)) {
    b.derived("film_grain_params", seq.film_grain_params_present
                                       ? "reset_grain_params() (frame neither shown nor showable)"
                                       : "reset_grain_params() (film_grain_params_present = 0)");
    return reset_grain_params();
  }
  FilmGrainParams fg;
  fg.apply_grain = b.read_flag("apply_grain");
  if (!fg.apply_grain) {
    b.derived("film_grain_params", "reset_grain_params() (apply_grain = 0)");
    return reset_grain_params();
  }
  fg.grain_seed = static_cast<uint16_t>(b.read_u("grain_seed", 16));
  if (frame_type == FrameType::Inter) {
    fg.update_grain = b.read_flag("update_grain");
  } else {
    fg.update_grain = true;
  }
  if (!fg.update_grain) {
    fg.film_grain_params_ref_idx = static_cast<uint8_t>(b.read_u("film_grain_params_ref_idx", 3));
    if (std::find(ref_frame_idx.begin(), ref_frame_idx.end(), static_cast<int>(fg.film_grain_params_ref_idx)) ==
        ref_frame_idx.end()) {
      conformance(
          b, fmt::format("film_grain_params_ref_idx = {} is not one of ref_frame_idx[]", fg.film_grain_params_ref_idx));
    }
    const uint16_t tempGrainSeed = fg.grain_seed;
    const uint8_t ref_idx = fg.film_grain_params_ref_idx;
    fg = load_grain_params(state, ref_idx);
    fg.grain_seed = tempGrainSeed;
    // Deviation from the letter of load_grain_params(): the two elements that say where the parameters came from
    // keep the values read (see frame_header.hpp).
    fg.update_grain = false;
    fg.film_grain_params_ref_idx = ref_idx;
    b.derived("film_grain_params", fmt::format("load_grain_params({}): num_y_points = {}, grain_seed = {}", ref_idx,
                                               fg.num_y_points, fg.grain_seed));
    return fg;
  }
  fg.num_y_points = static_cast<uint8_t>(b.read_u("num_y_points", 4));
  if (fg.num_y_points > kMaxNumYPoints) {
    conformance(b, fmt::format("num_y_points = {} exceeds {}", fg.num_y_points, kMaxNumYPoints));
  }
  read_points(b, "y", fg.num_y_points, fg.point_y_value, fg.point_y_scaling);
  if (cc.mono_chrome) {
    fg.chroma_scaling_from_luma = false;
  } else {
    fg.chroma_scaling_from_luma = b.read_flag("chroma_scaling_from_luma");
  }
  read_chroma_points(b, cc, fg);
  read_ar_and_scaling(b, fg);
  fg.overlap_flag = b.read_flag("overlap_flag");
  fg.clip_to_restricted_range = b.read_flag("clip_to_restricted_range");
  return fg;
}

}  // namespace ovt::syntax::av1
