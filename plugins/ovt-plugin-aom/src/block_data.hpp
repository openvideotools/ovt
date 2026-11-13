// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// The per-frame block data the inspection callback copies out of libaom (chapter 07 §7.6.1, WI-2.156): the
// insp_mi_data grid and the frame-header values the mapping needs. Plain data, free of libaom and FlatBuffers
// headers, so decoder.hpp and the unit tests can use it.
#ifndef OVT_PLUGIN_AOM_BLOCK_DATA_HPP
#define OVT_PLUGIN_AOM_BLOCK_DATA_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ovt::aom {

/// The insp_mi_data fields of one MI (libaom av1/decoder/inspection.h, v3.12.1), same names and units.
struct MiInfo {
  std::array<int16_t, 2> mv_row{};  // 1/8 luma sample units
  std::array<int16_t, 2> mv_col{};
  std::array<int16_t, 2> ref_frame{0, -1};  // MV_REFERENCE_FRAME: -1 NONE, 0 INTRA_FRAME, 1..7 LAST..ALTREF
  int16_t mode = 0;
  int16_t uv_mode = 0;
  int16_t bsize = 0;
  int16_t skip = 0;
  int16_t segment_id = 0;
  std::array<int16_t, 2> filter{};  // InterpFilter, [0] y, [1] x
  int16_t tx_size = 0;
  int16_t cdef_level = 0;
  int16_t cdef_strength = 0;
  int16_t cfl_alpha_idx = 0;
  int16_t cfl_alpha_sign = 0;
  int16_t current_qindex = 0;
  int16_t compound_type = 0;
  int16_t motion_mode = 0;
  int16_t intrabc = 0;
  int16_t palette = 0;
  int16_t uv_palette = 0;
};

/// One libaom AccountingSymbol (av1/decoder/accounting.h, v3.12.1), same names and units: the accounting context
/// (MI column x, MI row y; -1 before the first block of a tile group), the dictionary id, the cost in 1/8 bits
/// (AOM_ACCT_BITRES = 3) and the number of reads libaom merged into the record.
struct AcctSymbol {
  int16_t x = -1;
  int16_t y = -1;
  uint32_t id = 0;
  uint32_t bits = 0;
  uint32_t samples = 0;
};

/// The frame's accounting as the inspection callback copied it (chapter 07 §7.6.1 accounting row, WI-2.157).
struct AccountingData {
  bool valid = false;               // false without CONFIG_ACCOUNTING or callback data
  std::vector<std::string> names;   // libaom's dictionary at callback time: names[id]
  std::vector<AcctSymbol> symbols;  // in libaom's recording order
  int32_t tile_groups = 1;          // tile groups of the frame; > 1 means the symbols cover the last one only
};

/// Per-frame block data: libaom's MI grid plus the frame-header values that insp_frame_data lacks, read from the
/// decoder state inside the inspection callback.
// SPEC-ASSUMPTION(#360): §7.6.1 takes the superblock size, ref_frame_idx[], tile layout and global-motion types
// from "the host's parsed headers", but the plugin ABI has no channel for them, so the plugin reads them from
// libaom's AV1_COMMON inside the callback (the same state ifd_inspect() reads).
struct BlockData {
  bool valid = false;   // false when the frame had no callback data
  int32_t mi_rows = 0;  // libaom's grid, padding included (mi_cols = 2 * ((FrameWidth + 7) >> 3))
  int32_t mi_cols = 0;
  std::vector<MiInfo> mi;   // mi_rows * mi_cols, row-major
  int32_t sb_mi = 16;       // superblock size in MIs: 16 (64x64) or 32 (128x128)
  bool intra_frame = true;  // KEY or INTRA_ONLY: no reference types
  bool monochrome = false;
  std::array<int8_t, 7> ref_frame_idx{-1, -1, -1, -1, -1, -1, -1};  // DPB slot of LAST..ALTREF
  std::array<uint8_t, 8> gm_type{};                                 // GmType per reference type (0 unused)
  std::array<std::array<int32_t, 6>, 8> gm_params{};
  std::vector<int32_t> tile_col_starts{0};  // MiColStarts[0..TileCols] (MI units, last = mi_cols)
  std::vector<int32_t> tile_row_starts{0};
  int32_t cdef_damping = 3;
  int32_t cdef_bits = 0;
  std::vector<int32_t> cdef_strengths;  // libaom cdef_strengths[]: pri * 4 + sec code, one per cdef_idx
  std::vector<int32_t> cdef_uv_strengths;
  int32_t superres_denom = 8;  // SuperresDenom (AV1 §5.9.8); 8 = no super-resolution
  int32_t frame_width = 0;     // FrameWidth: the coded width, before super-resolution
  int32_t upscaled_width = 0;  // UpscaledWidth: the output width (= frame_width without super-resolution)
  int32_t base_qindex = 0;
  bool delta_q_present = false;
  int32_t delta_q_res = 1;                            // libaom's value (1 << delta_q_res syntax element)
  std::array<std::array<int16_t, 2>, 8> y_dequant{};  // [segment][DC, AC]
  AccountingData accounting;                          // WI-2.157
};

}  // namespace ovt::aom

#endif  // OVT_PLUGIN_AOM_BLOCK_DATA_HPP
