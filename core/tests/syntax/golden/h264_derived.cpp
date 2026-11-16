// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 values that the `gstparse` golden holds but OVT's syntax tree does not carry as coded elements (WI-1.572):
// inferred values (§7.4.2.1.1, §7.4.2.2, §7.4.3, E.2.1) and the scaling matrices after the fall-back rule (§7.4.2.1.1
// Table 7-2). They are taken from OVT's parsed structs, so the comparison checks OVT's inference against GStreamer's.

#include "h264_derived.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace ovt::test::syntax_golden {

namespace {

namespace h264 = ovt::syntax::h264;

// Table 7-3 and 7-4, in zig-zag scan order (the order scalingList[] is coded in).
constexpr std::array<uint8_t, 16> kDefault4x4Intra = {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42};
constexpr std::array<uint8_t, 16> kDefault4x4Inter = {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34};
constexpr std::array<uint8_t, 64> kDefault8x8Intra = {6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
                                                      23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
                                                      27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
                                                      31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42};
constexpr std::array<uint8_t, 64> kDefault8x8Inter = {9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
                                                      21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
                                                      24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
                                                      27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35};

std::array<uint8_t, 64> default_list(size_t i) {
  std::array<uint8_t, 64> out{};
  if (i < 6) {
    const auto& d = i < 3 ? kDefault4x4Intra : kDefault4x4Inter;
    std::copy(d.begin(), d.end(), out.begin());
  } else {
    out = (i % 2 == 0) ? kDefault8x8Intra : kDefault8x8Inter;
  }
  return out;
}

std::array<uint8_t, 64> flat() {
  std::array<uint8_t, 64> out{};
  out.fill(16);
  return out;
}

// One level of Table 7-2: list i from its own scaling_list() when present, else from `fallback(i)`.
template <class Fallback>
ScalingMatrices apply(const std::array<h264::ScalingList, 12>& lists, size_t count, Fallback fallback) {
  ScalingMatrices m{};
  for (size_t i = 0; i < 12; ++i) {
    const h264::ScalingList& l = lists.at(i);
    if (i < count && l.present) {
      m.at(i) = l.UseDefaultScalingMatrixFlag ? default_list(i) : l.list;
    } else {
      m.at(i) = fallback(i, m);
    }
  }
  return m;
}

void put(ParsedUnit& u, const std::string& key, std::vector<int64_t> values) {
  u.derived[key] = std::move(values);
}

template <size_t N>
void put_matrices(ParsedUnit& u, const std::string& key, const ScalingMatrices& m, size_t first, size_t count,
                  std::integral_constant<size_t, N> /*size*/) {
  std::vector<int64_t> v;
  for (size_t i = first; i < first + count; ++i) {
    v.insert(v.end(), m.at(i).begin(), m.at(i).begin() + N);
  }
  put(u, key, std::move(v));
}

void put_scaling(ParsedUnit& u, const ScalingMatrices& m) {
  put_matrices(u, "scaling_lists_4x4", m, 0, 6, std::integral_constant<size_t, 16>{});
  put_matrices(u, "scaling_lists_8x8", m, 6, 6, std::integral_constant<size_t, 64>{});
}

}  // namespace

ScalingMatrices h264_sps_scaling(const h264::Sps& sps) {
  if (!sps.seq_scaling_matrix_present_flag) {
    ScalingMatrices m{};
    m.fill(flat());
    return m;
  }
  const size_t count = sps.chroma_format_idc != 3 ? 8 : 12;
  // Fall-back rule A: lists 0, 3, 6, 7 fall back to the default; the others to the list two (8x8) or one (4x4)
  // before them.
  return apply(sps.scaling_list, count, [](size_t i, const ScalingMatrices& m) {
    if (i == 0 || i == 3 || i == 6 || i == 7) {
      return default_list(i);
    }
    return m.at(i < 6 ? i - 1 : i - 2);
  });
}

ScalingMatrices h264_pps_scaling(const h264::Pps& pps, const h264::Sps& sps) {
  const ScalingMatrices seq = h264_sps_scaling(sps);
  if (!pps.pic_scaling_matrix_present_flag) {
    return seq;
  }
  // Fall-back rule B: lists 0, 3, 6, 7 fall back to the sequence-level list, the others as in rule A.
  return apply(pps.scaling_list, pps.num_scaling_lists, [&](size_t i, const ScalingMatrices& m) {
    if (i == 0 || i == 3 || i == 6 || i == 7) {
      return seq.at(i);
    }
    return m.at(i < 6 ? i - 1 : i - 2);
  });
}

void add_h264_sps_derived(const h264::Sps& sps, ParsedUnit& u) {
  put(u, "chroma_format_idc", {sps.chroma_format_idc});
  put_scaling(u, h264_sps_scaling(sps));
  if (sps.vui_parameters_present_flag) {
    const auto& vui = sps.vui;
    put(u, "video_format", {vui.video_format});
    put(u, "colour_primaries", {vui.colour_primaries});
    put(u, "transfer_characteristics", {vui.transfer_characteristics});
    put(u, "matrix_coefficients", {vui.matrix_coefficients});
  }
}

void add_h264_pps_derived(const h264::Pps& pps, const h264::Sps* sps, ParsedUnit& u) {
  put(u, "second_chroma_qp_index_offset", {pps.second_chroma_qp_index_offset});
  if (sps != nullptr) {
    put_scaling(u, h264_pps_scaling(pps, *sps));
  }
}

void add_h264_slice_derived(const h264::SliceHeader& sh, const h264::Sps* sps, ParsedUnit& u) {
  // GStreamer infers the active reference counts only for the lists the slice type uses (zero otherwise).
  const bool uses_l0 = sh.slice_type_mod5 == h264::kSliceP || sh.slice_type_mod5 == h264::kSliceSP ||
                       sh.slice_type_mod5 == h264::kSliceB;
  if (uses_l0) {
    put(u, "num_ref_idx_l0_active_minus1", {sh.num_ref_idx_l0_active_minus1});
  }
  if (sh.slice_type_mod5 == h264::kSliceB) {
    put(u, "num_ref_idx_l1_active_minus1", {sh.num_ref_idx_l1_active_minus1});
  }
  put(u, "delta_pic_order_cnt", {sh.delta_pic_order_cnt[0], sh.delta_pic_order_cnt[1]});

  const auto& rplm = sh.ref_pic_list_modification;
  // GStreamer stores the terminating modification_of_pic_nums_idc 3 as an entry (value 0) and counts it.
  const auto ops = [&](bool flag, const std::vector<h264::RefPicListModificationOp>& list, const std::string& l) {
    std::vector<int64_t> idc;
    std::vector<int64_t> value;
    for (const auto& op : list) {
      idc.push_back(op.modification_of_pic_nums_idc);
      value.push_back(op.modification_of_pic_nums_idc == 2 ? op.long_term_pic_num : op.abs_diff_pic_num_minus1);
    }
    if (flag) {
      idc.push_back(3);
      value.push_back(0);
    }
    put(u, "n_ref_pic_list_modification_" + l, {static_cast<int64_t>(idc.size())});
    put(u, "ref_pic_list_modification_" + l + ".modification_of_pic_nums_idc", std::move(idc));
    put(u, "ref_pic_list_modification_" + l + ".value", std::move(value));
  };
  ops(rplm.ref_pic_list_modification_flag_l0, rplm.ops_l0, "l0");
  ops(rplm.ref_pic_list_modification_flag_l1, rplm.ops_l1, "l1");

  if (sh.has_pred_weight_table) {
    const auto& pwt = sh.pred_weight_table;
    // With ChromaArrayType 0 no chroma weights are coded and GStreamer leaves them at 0, while OVT's struct holds the
    // §7.4.3.2 inferred 2^chroma_log2_weight_denom: the chroma keys are left out, so the golden must hold zeros.
    const bool chroma = sps == nullptr || sps->ChromaArrayType != 0;
    const auto list = [&](const h264::PredWeightTable::List& l, uint32_t active_minus1, const std::string& n) {
      std::vector<int64_t> lw;
      std::vector<int64_t> lo;
      std::vector<int64_t> cw;
      std::vector<int64_t> co;
      for (uint32_t i = 0; i <= active_minus1 && i < h264::kMaxRefIdx; ++i) {
        lw.push_back(l.luma_weight.at(i));
        lo.push_back(l.luma_offset.at(i));
        for (size_t j = 0; j < 2; ++j) {
          cw.push_back(l.chroma_weight.at(i).at(j));
          co.push_back(l.chroma_offset.at(i).at(j));
        }
      }
      put(u, "luma_weight_" + n, std::move(lw));
      put(u, "luma_offset_" + n, std::move(lo));
      if (chroma) {
        put(u, "chroma_weight_" + n, std::move(cw));
        put(u, "chroma_offset_" + n, std::move(co));
      }
    };
    list(pwt.l0, sh.num_ref_idx_l0_active_minus1, "l0");
    if (sh.slice_type_mod5 == h264::kSliceB) {
      list(pwt.l1, sh.num_ref_idx_l1_active_minus1, "l1");
    }
  }

  if (sh.has_dec_ref_pic_marking) {
    const auto& m = sh.dec_ref_pic_marking;
    std::vector<int64_t> op;
    std::vector<int64_t> diff;
    std::vector<int64_t> ltpn;
    std::vector<int64_t> ltfi;
    std::vector<int64_t> maxlt;
    for (const auto& o : m.mmco) {
      op.push_back(o.memory_management_control_operation);
      diff.push_back(o.difference_of_pic_nums_minus1);
      ltpn.push_back(o.long_term_pic_num);
      ltfi.push_back(o.long_term_frame_idx);
      maxlt.push_back(o.max_long_term_frame_idx_plus1);
    }
    put(u, "n_ref_pic_marking", {static_cast<int64_t>(m.mmco.size())});
    put(u, "memory_management_control_operation", std::move(op));
    put(u, "difference_of_pic_nums_minus1", std::move(diff));
    put(u, "long_term_pic_num", std::move(ltpn));
    put(u, "long_term_frame_idx", std::move(ltfi));
    put(u, "max_long_term_frame_idx_plus1", std::move(maxlt));
  }
}

}  // namespace ovt::test::syntax_golden
