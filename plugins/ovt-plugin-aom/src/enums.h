// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// libaom enum tables of the insp_mi_data mapping (chapter 07 §7.6.1, WI-2.156). The numbering is libaom's
// (av1/common/enums.h, v3.12.1), which equals the AV1 specification's for every table here except
// COMPOUND_TYPE (see kCompoundTypeToSpec). Header-only and free of libaom headers, so the unit tests use it too.
#ifndef OVT_PLUGIN_AOM_ENUMS_H
#define OVT_PLUGIN_AOM_ENUMS_H

#include <array>
#include <cstdint>
#include <string_view>

namespace ovt::aom {

constexpr int kNumBlockSizes = 22;  // BLOCK_SIZES_ALL
constexpr int kNumTxSizes = 19;     // TX_SIZES_ALL
constexpr int kNumModes = 25;       // MB_MODE_COUNT

/// Width and height in 4x4 units (MI) of BLOCK_SIZE 0..21 (BLOCK_4X4 .. BLOCK_64X16).
struct MiSize {
  uint8_t w;
  uint8_t h;
};
constexpr std::array<MiSize, kNumBlockSizes> kBlockMiSize = {{
    {1, 1},  {1, 2},   {2, 1},   {2, 2},   {2, 4},   {4, 2}, {4, 4}, {4, 8}, {8, 4}, {8, 8},  {8, 16},
    {16, 8}, {16, 16}, {16, 32}, {32, 16}, {32, 32}, {1, 4}, {4, 1}, {2, 8}, {8, 2}, {4, 16}, {16, 4},
}};

/// The BLOCK_SIZE of a w x h MI rectangle, or -1 when no block has that shape.
constexpr int block_size_of(int w, int h) {
  for (int b = 0; b < kNumBlockSizes; ++b) {
    if (kBlockMiSize.at(static_cast<size_t>(b)).w == w && kBlockMiSize.at(static_cast<size_t>(b)).h == h) {
      return b;
    }
  }
  return -1;
}

/// log2 of the width and height in luma samples of TX_SIZE 0..18 (TX_4X4 .. TX_64X16).
constexpr std::array<MiSize, kNumTxSizes> kTxLog2 = {{
    {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5},
    {5, 4}, {5, 6}, {6, 5}, {2, 4}, {4, 2}, {3, 5}, {5, 3}, {4, 6}, {6, 4},
}};

/// PREDICTION_MODE names (Av1YMode spellings; also the MotionVector.predictor of inter modes).
constexpr std::array<std::string_view, kNumModes> kModeNames = {
    "DC_PRED",     "V_PRED",        "H_PRED",        "D45_PRED",    "D135_PRED",     "D113_PRED",
    "D157_PRED",   "D203_PRED",     "D67_PRED",      "SMOOTH_PRED", "SMOOTH_V_PRED", "SMOOTH_H_PRED",
    "PAETH_PRED",  "NEARESTMV",     "NEARMV",        "GLOBALMV",    "NEWMV",         "NEAREST_NEARESTMV",
    "NEAR_NEARMV", "NEAREST_NEWMV", "NEW_NEARESTMV", "NEAR_NEWMV",  "NEW_NEARMV",    "GLOBAL_GLOBALMV",
    "NEW_NEWMV",
};

constexpr int kIntraModes = 13;    // DC_PRED .. PAETH_PRED
constexpr int kUvIntraModes = 14;  // .. UV_CFL_PRED; libaom writes UV_MODE_INVALID (15) for inter blocks
constexpr int kUvCflPred = 13;
constexpr int kGlobalMv = 15;
constexpr int kGlobalGlobalMv = 23;

/// YMode values whose motion vectors are fully predicted (no NEWMV part): Block.merge_or_direct (chapter 06 §6.3.1).
constexpr bool is_predicted_mv_mode(int mode) {
  return mode == 13 || mode == 14 || mode == kGlobalMv || mode == 17 || mode == 18 || mode == kGlobalGlobalMv;
}

/// RefSlot names of reference types 1..7 (LAST_FRAME .. ALTREF_FRAME); index 0 is the INTRA placeholder.
constexpr std::array<std::string_view, 8> kRefNames = {"INTRA",  "LAST",   "LAST2",   "LAST3",
                                                       "GOLDEN", "BWDREF", "ALTREF2", "ALTREF"};

/// libaom COMPOUND_TYPE (0 AVERAGE, 1 DISTWTD, 2 WEDGE, 3 DIFFWTD) -> Av1CompoundType (chapter 06: WEDGE 0,
/// DIFFWTD 1, AVERAGE 2, INTRA 3, DISTANCE 4).
constexpr std::array<uint8_t, 4> kCompoundTypeToSpec = {2, 4, 0, 1};
constexpr uint8_t kCompoundIntra = 3;

constexpr uint8_t kMotionObmc = 1;     // OBMC_CAUSAL
constexpr uint8_t kMotionWarped = 2;   // WARPED_CAUSAL
constexpr uint8_t kGmTranslation = 1;  // TRANSLATION; ROTZOOM and AFFINE are larger

// Av1Partition (chapter 06; PARTITION_* of AV1 §6.10.4).
enum class Partition : uint8_t { None = 0, Horz, Vert, Split, HorzA, HorzB, VertA, VertB, Horz4, Vert4 };

// ovt.fb.PredKind values.
constexpr uint8_t kPredNone = 0;
constexpr uint8_t kPredIntra = 1;
constexpr uint8_t kPredInter = 2;
constexpr uint8_t kPredIntraBc = 4;
constexpr uint8_t kPredPalette = 6;

// CellGrid.intra_mode_y codes beyond YMode (chapter 06 §6.3.1, AV1 row).
constexpr int16_t kIntraModeIntraBc = 32;
constexpr int16_t kIntraModePalette = 33;

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_ENUMS_H
