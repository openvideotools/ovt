// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// libde265 patch 0002 (PU/TU syntax and bit positions) -> HevcCu / HevcPu / HevcTu fields, MotionVector.mvd and
// predictor, CellGrid.cbf / tx_type / bits_x8 and Block.bits_x8 (chapter 07 §7.6.2 mapping table, the patch-0002
// rows; WI-2.165). decoder.cpp copies the syntax into BlockData (map_blocks.hpp) through the patch-0002 getters;
// map_blocks.cpp calls these helpers while it builds the block tree. Free of libde265 headers so that the unit tests
// can build the syntax by hand.
#ifndef OVT_PLUGIN_DE265_MAP_SYNTAX_HPP
#define OVT_PLUGIN_DE265_MAP_SYNTAX_HPP

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::de265 {

/// de265_internals_pu_syntax.kind (DE265_INTERNALS_PU_*).
enum class PuKind : uint8_t { None = 0, Inter = 1, Intra = 2 };

/// de265_internals_pu_syntax of one 4x4 unit: the prediction block's syntax as signalled. 255 (-1 for ref_idx, 0 for
/// mvd) where an element does not apply or was not signalled.
struct PuSyntax {
  uint8_t kind = 0;  // PuKind
  uint8_t merge_flag = 255;
  uint8_t merge_idx = 255;
  uint8_t inter_pred_idc = 255;  // H.265 Table 7-11: 0 PRED_L0, 1 PRED_L1, 2 PRED_BI
  std::array<int8_t, 2> ref_idx{-1, -1};
  std::array<uint8_t, 2> mvp_flag{255, 255};
  std::array<int16_t, 2> mvd_x{};  // MvdLX, quarter samples
  std::array<int16_t, 2> mvd_y{};
  uint8_t mvd_l1_zero = 255;
  uint8_t prev_intra_luma_pred_flag = 255;
  uint8_t mpm_idx = 255;
  uint8_t rem_intra_luma_pred_mode = 255;
  uint8_t intra_chroma_pred_mode = 255;
};

/// de265_internals_tu_syntax of one 4x4 unit: the syntax of the transform unit covering it. Per-component arrays are
/// Y, Cb, Cr; 255 where an element was not signalled in this TU.
struct TuSyntax {
  uint8_t coded = 0;
  uint8_t trafoDepth = 0;
  uint8_t log2TrafoSize = 0;
  std::array<uint8_t, 3> cbf{};  // 4:2:2 chroma: bit 0 first, bit 1 second chroma block
  std::array<uint8_t, 3> transform_skip_flag{255, 255, 255};
  std::array<uint8_t, 3> explicit_rdpcm_flag{255, 255, 255};
  std::array<uint8_t, 3> explicit_rdpcm_dir_flag{255, 255, 255};
  std::array<uint8_t, 3> last_sig_coeff_x{255, 255, 255};
  std::array<uint8_t, 3> last_sig_coeff_y{255, 255, 255};
  std::array<uint16_t, 3> nnz{};
  uint8_t cu_qp_delta_abs = 255;
  uint8_t cu_qp_delta_sign_flag = 255;
  uint8_t cu_chroma_qp_offset_flag = 255;
  uint8_t cu_chroma_qp_offset_idx = 255;
};

/// The patch-0002 data of one picture. pu and tu have one entry per CellGrid cell, cu_bits one per MinCb unit (the
/// CU's value at every unit it covers), ctb_bits one per CTB. Empty (and has_syntax false) without patch 0002 data.
struct SyntaxData {
  bool has_syntax = false;
  std::vector<PuSyntax> pu;
  std::vector<TuSyntax> tu;
  std::vector<uint32_t> cu_bits;   // 1/8 bits: coding_unit()
  std::vector<uint32_t> ctb_bits;  // 1/8 bits: coding_tree_unit() and the slice data around it
};

/// HevcCu fields that come from the syntax of the CU's prediction and transform units.
struct CuSyntax {
  uint8_t rqt_root_cbf = 255;  // 255 when not signalled (intra, skip, 2Nx2N merge)
  uint8_t cu_qp_delta_abs = 255;
  uint8_t cu_qp_delta_sign_flag = 255;
  int16_t CuQpDeltaVal = 0;
  uint8_t cu_chroma_qp_offset_flag = 255;
  uint8_t cu_chroma_qp_offset_idx = 255;
  std::vector<uint8_t> intra_chroma_pred_mode;  // 0..4 as signalled: 1 entry, 4 for 4:4:4 PART_NxN; empty if n/a
  bool any_coded = false;                       // a transform_unit() was read in the CU
  bool all_merged = false;                      // inter CU whose prediction units all have merge_flag = 1
};

/// What cu_syntax() needs to know about a CU besides its position.
struct CuShape {
  int32_t x = 0;
  int32_t y = 0;
  int32_t log2 = 0;
  bool intra = false;
  bool skip = false;
  uint8_t part_mode = 0;
  uint8_t chroma_format = 1;
};

/// Cells of a picture: the index of the 4x4 at luma (x, y).
struct CellIndex {
  uint32_t cells_w = 0;
  [[nodiscard]] std::size_t operator()(int32_t x, int32_t y) const {
    return (static_cast<std::size_t>(y >> 2) * cells_w) + static_cast<std::size_t>(x >> 2);
  }
};

/// The CuSyntax of @p cu from @p s (which must have has_syntax set and one entry per cell of @p at).
CuSyntax cu_syntax(const SyntaxData& s, const CellIndex& at, const CuShape& cu);

/// Adds the fields of @p cs and bit_count_x8 = @p bits to a HevcCu; @p chroma is the intra_chroma_pred_mode vector made
/// from cs (null when it has none).
void add_cu_syntax(ovt::fb::HevcCuBuilder& b, const CuSyntax& cs,
                   flatbuffers::Offset<flatbuffers::Vector<uint8_t>> chroma, uint32_t bits);

/// MotionVector.predictor (chapter 07 §7.6.2): "merge[merge_idx]" or "amvp[mvp_lX_flag]"; "?" when unknown.
std::string predictor(const PuSyntax& pu, int list);

/// MotionVector.mvd_x/mvd_y (1/16 luma samples, clamped to ±32767) of list @p list; -32768 for merge or unknown.
std::array<int16_t, 2> mvd16(const PuSyntax& pu, int list);

/// True when list @p list is used by the signalled inter_pred_idc of an AMVP-coded @p pu.
bool amvp_list(const PuSyntax& pu, int list);

/// Adds the signalled HevcPu fields of @p pu (merge_flag, merge_idx, ref_idx, mvd, mvp flags; for intra blocks the
/// MPM syntax). inter_pred_idc is left to the caller, which also knows the derived value. Fields @p pu does not have
/// stay at their defaults.
void add_pu_syntax(ovt::fb::HevcPuBuilder& b, const PuSyntax& pu);

/// The HevcTu vectors of one TU, made before its HevcTuBuilder starts.
struct TuVectors {
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> cbf_cb;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> cbf_cr;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> transform_skip_flag;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> explicit_rdpcm_flag;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> explicit_rdpcm_dir_flag;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> last_sig_coeff_x;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> last_sig_coeff_y;
  flatbuffers::Offset<flatbuffers::Vector<uint16_t>> num_nonzero;
};

/// Creates the TuVectors of @p tu in @p fbb. cbf_cb / cbf_cr are left out in 4:0:0 (@p chroma_format 0) and where
/// the TU codes no chroma (@p chroma_here false: blkIdx 0..2 of a split into 4x4 luma TBs outside 4:4:4, whose chroma
/// is coded with blkIdx 3); 4:2:2 (@p chroma_format 2) has two entries each.
TuVectors tu_vectors(flatbuffers::FlatBufferBuilder& fbb, const TuSyntax& tu, uint8_t chroma_format, bool chroma_here);

/// CellGrid.tx_type of a luma TB (chapter 06 §6.3.1): 3 transquant bypass, 2 transform skip, 1 DST-VII (4x4 intra
/// luma), else 0 (DCT-II).
uint8_t grid_tx_type(const TuSyntax& tu, bool bypass, bool intra);

/// HevcTu.dst_used: 1 when the 4x4 intra luma DST-VII was applied (a coded luma TB, not bypassed or skipped).
bool dst_used(const TuSyntax& tu, bool bypass, bool intra);

/// Adds @p v and cbf_luma of @p tu to a leaf HevcTu.
void add_tu_syntax(ovt::fb::HevcTuBuilder& b, const TuSyntax& tu, const TuVectors& v);

/// CellGrid.cbf of a cell of @p tu: bit 0 cbf_luma, bit 1 cbf_cb, bit 2 cbf_cr (4:2:2: either chroma block).
uint8_t grid_cbf(const TuSyntax& tu);

/// Writes @p bits over the cells of the luma rect (x, y, n, n) of @p grid_bits (CellGrid.bits_x8): floor(bits / cells)
/// each, plus one for the first `bits % cells` cells in raster order (chapter 06 §6.3.4, rule V-020).
/// SPEC-ASSUMPTION(#605): V-020 wins over the §7.6.2 row's "remainder to the first cell". Cells outside
/// the grid are skipped and do not count.
void spread_bits(std::vector<uint32_t>& grid_bits, const CellIndex& at, uint32_t cells_h, int32_t x, int32_t y,
                 int32_t n, uint32_t bits);

}  // namespace ovt::de265

#endif  // OVT_PLUGIN_DE265_MAP_SYNTAX_HPP
