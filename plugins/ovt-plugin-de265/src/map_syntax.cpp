// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// libde265 patch 0002 syntax -> HevcCu / HevcPu / HevcTu fields, MotionVector.mvd / predictor and the CellGrid fields
// cbf, tx_type and bits_x8 (chapter 07 §7.6.2, WI-2.165).

#include "map_syntax.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "map_grid.hpp"

namespace ovt::de265 {

namespace {

namespace fb = ovt::fb;

constexpr uint8_t kUnset = 255;
constexpr int16_t kMvdUnavailable = -32768;
constexpr uint8_t kPart2Nx2N = 0;
constexpr uint8_t kPartNxN = 3;
constexpr uint8_t kChroma444 = 3;
constexpr uint8_t kChroma422 = 2;
constexpr uint8_t kTxDct = 0;
constexpr uint8_t kTxDst = 1;
constexpr uint8_t kTxSkip = 2;
constexpr uint8_t kTxBypass = 3;

// A cbf value of 4:2:2 chroma holds one bit per chroma block; elsewhere it is 0 or 1.
std::vector<uint8_t> chroma_cbf(uint8_t v, uint8_t chroma_format) {
  if (chroma_format == kChroma422) {
    return {static_cast<uint8_t>(v & 1U), static_cast<uint8_t>((v >> 1U) & 1U)};
  }
  return {static_cast<uint8_t>(v != 0)};
}

// The QP syntax of a coded TU, when @p out has none yet. A CU codes cu_qp_delta and cu_chroma_qp_offset at most once
// (H.265 7.3.8.10), so the scan order of its cells does not matter.
void take_qp_syntax(CuSyntax& out, const TuSyntax& tu) {
  if (out.cu_qp_delta_abs == kUnset && tu.cu_qp_delta_abs != kUnset) {
    out.cu_qp_delta_abs = tu.cu_qp_delta_abs;
    out.cu_qp_delta_sign_flag = tu.cu_qp_delta_sign_flag;
    const int sign = tu.cu_qp_delta_sign_flag == 1 ? -1 : 1;
    out.CuQpDeltaVal = static_cast<int16_t>(sign * int{tu.cu_qp_delta_abs});
  }
  if (out.cu_chroma_qp_offset_flag == kUnset && tu.cu_chroma_qp_offset_flag != kUnset) {
    out.cu_chroma_qp_offset_flag = tu.cu_chroma_qp_offset_flag;
    out.cu_chroma_qp_offset_idx = tu.cu_chroma_qp_offset_idx;
  }
}

// intra_chroma_pred_mode of an intra CU: one entry, or one per PB for 4:4:4 PART_NxN; empty when unknown.
std::vector<uint8_t> chroma_modes(const SyntaxData& s, const CellIndex& at, const CuShape& cu) {
  std::vector<uint8_t> out;
  if (cu.chroma_format == 0) {
    return out;
  }
  if (cu.chroma_format == kChroma444 && cu.part_mode == kPartNxN) {
    const int32_t h = (1 << cu.log2) / 2;
    for (const auto& [dx, dy] : {std::pair{0, 0}, std::pair{h, 0}, std::pair{0, h}, std::pair{h, h}}) {
      out.push_back(s.pu[at(cu.x + dx, cu.y + dy)].intra_chroma_pred_mode);
    }
  } else {
    out.push_back(s.pu[at(cu.x, cu.y)].intra_chroma_pred_mode);
  }
  if (std::find(out.begin(), out.end(), kUnset) != out.end()) {
    out.clear();
  }
  return out;
}

}  // namespace

CuSyntax cu_syntax(const SyntaxData& s, const CellIndex& at, const CuShape& cu) {
  CuSyntax out;
  const int32_t n = 1 << cu.log2;
  bool all_merged = true;
  for (int32_t y = cu.y; y < cu.y + n; y += 4) {
    for (int32_t x = cu.x; x < cu.x + n; x += 4) {
      const std::size_t c = at(x, y);
      if (s.tu[c].coded != 0) {
        out.any_coded = true;
        take_qp_syntax(out, s.tu[c]);
      }
      all_merged = all_merged && (s.pu[c].kind != static_cast<uint8_t>(PuKind::Inter) || s.pu[c].merge_flag == 1);
    }
  }
  if (cu.intra) {
    out.intra_chroma_pred_mode = chroma_modes(s, at, cu);
    return out;  // rqt_root_cbf is not signalled for intra CUs
  }
  out.all_merged = all_merged;
  // H.265 7.3.8.5: rqt_root_cbf is signalled for inter CUs except skip and PART_2Nx2N with merge_flag = 1. When
  // signalled, transform_tree() is read exactly when it is 1.
  const bool merged_2nx2n = cu.part_mode == kPart2Nx2N && all_merged;
  if (!cu.skip && !merged_2nx2n) {
    out.rqt_root_cbf = static_cast<uint8_t>(out.any_coded);
  }
  return out;
}

void add_cu_syntax(fb::HevcCuBuilder& b, const CuSyntax& cs, flatbuffers::Offset<flatbuffers::Vector<uint8_t>> chroma,
                   uint32_t bits) {
  if (cs.rqt_root_cbf != kUnset) {
    b.add_rqt_root_cbf(cs.rqt_root_cbf);
  }
  if (cs.cu_qp_delta_abs != kUnset) {
    b.add_cu_qp_delta_abs(cs.cu_qp_delta_abs);
    b.add_cu_qp_delta_sign_flag(cs.cu_qp_delta_sign_flag);
    b.add_CuQpDeltaVal(cs.CuQpDeltaVal);
  }
  if (cs.cu_chroma_qp_offset_flag != kUnset) {
    b.add_cu_chroma_qp_offset_flag(cs.cu_chroma_qp_offset_flag);
    b.add_cu_chroma_qp_offset_idx(cs.cu_chroma_qp_offset_idx);
  }
  if (!chroma.IsNull()) {
    b.add_intra_chroma_pred_mode(chroma);
  }
  b.add_bit_count_x8(bits);
}

bool amvp_list(const PuSyntax& pu, int list) {
  if (pu.kind != static_cast<uint8_t>(PuKind::Inter) || pu.merge_flag != 0 || pu.inter_pred_idc > 2) {
    return false;
  }
  return pu.inter_pred_idc == 2 || pu.inter_pred_idc == list;
}

std::string predictor(const PuSyntax& pu, int list) {
  if (pu.kind != static_cast<uint8_t>(PuKind::Inter)) {
    return "?";
  }
  if (pu.merge_flag == 1 && pu.merge_idx != kUnset) {
    return "merge[" + std::to_string(pu.merge_idx) + "]";
  }
  const uint8_t f = pu.mvp_flag.at(static_cast<std::size_t>(list));
  if (amvp_list(pu, list) && f != kUnset) {
    return "amvp[" + std::to_string(f) + "]";
  }
  return "?";
}

std::array<int16_t, 2> mvd16(const PuSyntax& pu, int list) {
  if (!amvp_list(pu, list)) {
    return {kMvdUnavailable, kMvdUnavailable};
  }
  const auto l = static_cast<std::size_t>(list);
  return {mv16(pu.mvd_x.at(l)), mv16(pu.mvd_y.at(l))};
}

void add_pu_syntax(fb::HevcPuBuilder& b, const PuSyntax& pu) {
  if (pu.kind == static_cast<uint8_t>(PuKind::Intra)) {
    if (pu.prev_intra_luma_pred_flag != kUnset) {
      b.add_prev_intra_luma_pred_flag(pu.prev_intra_luma_pred_flag);
    }
    if (pu.mpm_idx != kUnset) {
      b.add_mpm_idx(pu.mpm_idx);
    }
    if (pu.rem_intra_luma_pred_mode != kUnset) {
      b.add_rem_intra_luma_pred_mode(pu.rem_intra_luma_pred_mode);
    }
    return;
  }
  if (pu.kind != static_cast<uint8_t>(PuKind::Inter)) {
    return;
  }
  if (pu.merge_flag != kUnset) {
    b.add_merge_flag(pu.merge_flag);
  }
  if (pu.merge_flag == 1) {
    if (pu.merge_idx != kUnset) {
      b.add_merge_idx(pu.merge_idx);
    }
    return;
  }
  if (amvp_list(pu, 0)) {
    if (pu.ref_idx[0] >= 0) {
      b.add_ref_idx_l0(pu.ref_idx[0]);
    }
    b.add_mvd_l0_x(pu.mvd_x[0]);
    b.add_mvd_l0_y(pu.mvd_y[0]);
    if (pu.mvp_flag[0] != kUnset) {
      b.add_mvp_l0_flag(pu.mvp_flag[0]);
    }
  }
  if (amvp_list(pu, 1)) {
    if (pu.ref_idx[1] >= 0) {
      b.add_ref_idx_l1(pu.ref_idx[1]);
    }
    b.add_mvd_l1_x(pu.mvd_x[1]);  // 0 when mvd_l1_zero_flag forced MvdL1 = 0 (7.3.8.9)
    b.add_mvd_l1_y(pu.mvd_y[1]);
    if (pu.mvp_flag[1] != kUnset) {
      b.add_mvp_l1_flag(pu.mvp_flag[1]);
    }
  }
}

TuVectors tu_vectors(flatbuffers::FlatBufferBuilder& fbb, const TuSyntax& tu, uint8_t chroma_format, bool chroma_here) {
  const std::size_t comps = chroma_format == 0 ? 1 : 3;
  auto per_comp8 = [&](const std::array<uint8_t, 3>& a) {
    std::vector<uint8_t> v(a.begin(), a.end());
    v.resize(3, kUnset);
    std::fill(v.begin() + static_cast<std::ptrdiff_t>(comps), v.end(), kUnset);
    return fbb.CreateVector(v);
  };
  TuVectors v;
  if (chroma_format != 0 && chroma_here) {
    v.cbf_cb = fbb.CreateVector(chroma_cbf(tu.cbf[1], chroma_format));
    v.cbf_cr = fbb.CreateVector(chroma_cbf(tu.cbf[2], chroma_format));
  }
  v.transform_skip_flag = per_comp8(tu.transform_skip_flag);
  v.explicit_rdpcm_flag = per_comp8(tu.explicit_rdpcm_flag);
  v.explicit_rdpcm_dir_flag = per_comp8(tu.explicit_rdpcm_dir_flag);
  v.last_sig_coeff_x = per_comp8(tu.last_sig_coeff_x);
  v.last_sig_coeff_y = per_comp8(tu.last_sig_coeff_y);
  std::vector<uint16_t> nnz(tu.nnz.begin(), tu.nnz.end());
  std::fill(nnz.begin() + static_cast<std::ptrdiff_t>(comps), nnz.end(), uint16_t{0});
  v.num_nonzero = fbb.CreateVector(nnz);
  return v;
}

uint8_t grid_tx_type(const TuSyntax& tu, bool bypass, bool intra) {
  if (bypass) {
    return kTxBypass;
  }
  if (tu.transform_skip_flag[0] == 1) {
    return kTxSkip;
  }
  return intra && tu.log2TrafoSize == 2 ? kTxDst : kTxDct;
}

bool dst_used(const TuSyntax& tu, bool bypass, bool intra) {
  return grid_tx_type(tu, bypass, intra) == kTxDst && tu.cbf[0] != 0;
}

void add_tu_syntax(fb::HevcTuBuilder& b, const TuSyntax& tu, const TuVectors& v) {
  b.add_cbf_luma(static_cast<uint8_t>(tu.cbf[0] != 0));
  if (!v.cbf_cb.IsNull()) {
    b.add_cbf_cb(v.cbf_cb);
    b.add_cbf_cr(v.cbf_cr);
  }
  b.add_transform_skip_flag(v.transform_skip_flag);
  b.add_explicit_rdpcm_flag(v.explicit_rdpcm_flag);
  b.add_explicit_rdpcm_dir_flag(v.explicit_rdpcm_dir_flag);
  b.add_last_sig_coeff_x(v.last_sig_coeff_x);
  b.add_last_sig_coeff_y(v.last_sig_coeff_y);
  b.add_num_nonzero(v.num_nonzero);
}

uint8_t grid_cbf(const TuSyntax& tu) {
  return static_cast<uint8_t>((tu.cbf[0] != 0 ? 1U : 0U) | (tu.cbf[1] != 0 ? 2U : 0U) | (tu.cbf[2] != 0 ? 4U : 0U));
}

void spread_bits(std::vector<uint32_t>& grid_bits, const CellIndex& at, uint32_t cells_h, int32_t x, int32_t y,
                 int32_t n, uint32_t bits) {
  const auto in_grid = [&](int32_t cx, int32_t cy) {
    return cx >= 0 && cy >= 0 && static_cast<uint32_t>(cx >> 2) < at.cells_w &&
           static_cast<uint32_t>(cy >> 2) < cells_h;
  };
  uint32_t cells = 0;
  for (int32_t yy = y; yy < y + n; yy += 4) {
    for (int32_t xx = x; xx < x + n; xx += 4) {
      cells += in_grid(xx, yy) ? 1U : 0U;
    }
  }
  if (cells == 0) {
    return;
  }
  const uint32_t each = bits / cells;
  uint32_t extra = bits % cells;
  for (int32_t yy = y; yy < y + n; yy += 4) {
    for (int32_t xx = x; xx < x + n; xx += 4) {
      if (!in_grid(xx, yy)) {
        continue;
      }
      grid_bits[at(xx, yy)] = each + (extra > 0 ? 1U : 0U);
      extra -= extra > 0 ? 1U : 0U;
    }
  }
}

}  // namespace ovt::de265
