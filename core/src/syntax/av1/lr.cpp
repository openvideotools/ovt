// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 lr_params() (chapter 7 §7.3.6 row 22; WI-1.118) and the loop restoration unit grid of AV1 §5.11.57.

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecLr = "AV1 5.9.20";

// Round2(x, n) (AV1 §4.7) for the chroma plane sizes.
constexpr uint32_t round2(uint32_t x, unsigned n) noexcept {
  return n == 0 ? x : (x + (1U << (n - 1U))) >> n;
}

// lr_type per plane → FrameRestorationType, UsesLr, usesChromaLr (AV1 §5.9.20).
void read_lr_types(SyntaxBuilder& b, const ColorConfig& cc, LrParams& lr) {
  const int numPlanes = std::min<int>(cc.NumPlanes, 3);
  for (int i = 0; i < numPlanes; ++i) {
    const auto si = static_cast<size_t>(i);
    b.begin_array_element("lr_type", i, kSpecLr);
    lr.lr_type[si] = static_cast<uint8_t>(b.read_u("lr_type", 2));
    lr.FrameRestorationType[si] = kRemapLrType[lr.lr_type[si]];
    b.set_enum(restoration_type_name(lr.FrameRestorationType[si]));
    if (lr.FrameRestorationType[si] != RestorationType::None) {
      lr.UsesLr = true;
      lr.usesChromaLr = lr.usesChromaLr || i > 0;
    }
    b.end_array_element();
  }
}

// lr_unit_shift (and lr_unit_extra_shift with 64x64 superblocks) of AV1 §5.9.20.
void read_lr_unit_shift(SyntaxBuilder& b, bool use_128x128_superblock, LrParams& lr) {
  lr.lr_unit_shift = static_cast<uint8_t>(b.read_u("lr_unit_shift", 1));
  if (use_128x128_superblock) {
    ++lr.lr_unit_shift;
  } else if (lr.lr_unit_shift != 0) {
    lr.lr_unit_extra_shift = b.read_flag("lr_unit_extra_shift");
    lr.lr_unit_shift = static_cast<uint8_t>(lr.lr_unit_shift + (lr.lr_unit_extra_shift ? 1U : 0U));
  }
}

// AV1 §5.11.57: unitRows / unitCols per plane with restoration, on the upscaled plane size.
void compute_unit_grid(const ColorConfig& cc, uint32_t UpscaledWidth, uint32_t FrameHeight, LrParams& lr) {
  const int numPlanes = std::min<int>(cc.NumPlanes, 3);
  for (int plane = 0; plane < numPlanes; ++plane) {
    const auto sp = static_cast<size_t>(plane);
    if (lr.FrameRestorationType[sp] == RestorationType::None) {
      continue;
    }
    const unsigned ssx = plane > 0 && cc.subsampling_x ? 1U : 0U;
    const unsigned ssy = plane > 0 && cc.subsampling_y ? 1U : 0U;
    const uint32_t unitSize = lr.LoopRestorationSize[sp];
    lr.LrUnitRows[sp] = count_units_in_frame(unitSize, round2(FrameHeight, ssy));
    lr.LrUnitCols[sp] = count_units_in_frame(unitSize, round2(UpscaledWidth, ssx));
  }
}

}  // namespace

std::string_view restoration_type_name(RestorationType t) noexcept {
  switch (t) {
    case RestorationType::None:
      return "RESTORE_NONE";
    case RestorationType::Wiener:
      return "RESTORE_WIENER";
    case RestorationType::Sgrproj:
      return "RESTORE_SGRPROJ";
    case RestorationType::Switchable:
      return "RESTORE_SWITCHABLE";
  }
  return "";
}

uint32_t count_units_in_frame(uint32_t unitSize, uint32_t frameSize) noexcept {
  // AV1 §5.11.57
  if (unitSize == 0) {
    return 1;
  }
  return std::max((frameSize + (unitSize >> 1U)) / unitSize, 1U);
}

LrParams parse_lr_params(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, bool AllLossless,
                         bool allow_intrabc, uint32_t UpscaledWidth, uint32_t FrameHeight) {
  // AV1 §5.9.20
  LrParams lr;
  const auto start = br.mark();
  auto s = b.scoped("lr_params", kSpecLr);
  const ColorConfig& cc = seq.color_config;
  if (AllLossless || allow_intrabc || !seq.enable_restoration) {
    lr.FrameRestorationType = {RestorationType::None, RestorationType::None, RestorationType::None};
    lr.UsesLr = false;
    b.derived("lr_params", AllLossless     ? "FrameRestorationType[0..2] = RESTORE_NONE (AllLossless)"
                           : allow_intrabc ? "FrameRestorationType[0..2] = RESTORE_NONE (allow_intrabc)"
                                           : "FrameRestorationType[0..2] = RESTORE_NONE (enable_restoration = 0)");
    lr.span = br.span_since(start);
    return lr;
  }
  lr.coded = true;
  lr.UsesLr = false;
  lr.usesChromaLr = false;
  read_lr_types(b, cc, lr);
  if (lr.UsesLr) {
    read_lr_unit_shift(b, seq.use_128x128_superblock, lr);
    lr.LoopRestorationSize[0] = static_cast<uint16_t>(kRestorationTileSizeMax >> (2U - lr.lr_unit_shift));
    if (cc.subsampling_x && cc.subsampling_y && lr.usesChromaLr) {
      lr.lr_uv_shift = static_cast<uint8_t>(b.read_u("lr_uv_shift", 1));
    } else {
      lr.lr_uv_shift = 0;
    }
    lr.LoopRestorationSize[1] = static_cast<uint16_t>(lr.LoopRestorationSize[0] >> lr.lr_uv_shift);
    lr.LoopRestorationSize[2] = static_cast<uint16_t>(lr.LoopRestorationSize[0] >> lr.lr_uv_shift);
    b.derived("LoopRestorationSize",
              fmt::format("LoopRestorationSize = {{{}, {}, {}}} (lr_unit_shift = {})", lr.LoopRestorationSize[0],
                          lr.LoopRestorationSize[1], lr.LoopRestorationSize[2], lr.lr_unit_shift));
    compute_unit_grid(cc, UpscaledWidth, FrameHeight, lr);
    b.derived("lr_units", fmt::format("unitRows x unitCols = {}x{}, {}x{}, {}x{}", lr.LrUnitRows[0], lr.LrUnitCols[0],
                                      lr.LrUnitRows[1], lr.LrUnitCols[1], lr.LrUnitRows[2], lr.LrUnitCols[2]));
  }
  lr.span = br.span_since(start);
  return lr;
}

}  // namespace ovt::syntax::av1
