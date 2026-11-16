// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 values that the `gstparse` golden holds but OVT's syntax tree does not carry as coded elements (WI-1.572):
// inferred values (§7.4.3, §7.4.7.1, E.3.1), the derived st_ref_pic_set() arrays (§7.4.8), the scaling lists after
// the default rule (§7.4.5) and arrays laid out the way GStreamer's structs hold them (one fixed-size row per
// sub-layer, per set or per matrix). They are taken from OVT's parsed structs, so the comparison checks OVT's
// inference against GStreamer's.

#include "h265_derived.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ovt::test::syntax_golden {

namespace {

namespace h265 = ovt::syntax::h265;

// A flag as the integer value the goldens store.
constexpr int64_t flag(bool f) {
  return f ? 1 : 0;
}

// Row sizes of GStreamer's H.265 arrays (gsth265parser.h).
constexpr size_t kSubLayers = 6;  // sub_layer_* of profile_tier_level
constexpr size_t kCpbs = 32;      // sublayer_hrd_params entries
constexpr size_t kRpsPics = 16;   // DeltaPocS0 / S1 per set
constexpr size_t kCompatibilityFlags = 32;

void put(ParsedUnit& u, const std::string& key, std::vector<int64_t> values) {
  u.derived[key] = std::move(values);
}

template <class T, class F>
std::vector<int64_t> rows(size_t count, size_t width, const T& source, F value) {
  std::vector<int64_t> out;
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = 0; j < width; ++j) {
      out.push_back(value(source, i, j));
    }
  }
  return out;
}

void put_ptl(ParsedUnit& u, const h265::ProfileTierLevel& ptl, unsigned max_sub_layers_minus1) {
  // sub_layer_*[i] for i < max_sub_layers_minus1, zero where the sub-layer signals nothing.
  const size_t n = std::min<size_t>(max_sub_layers_minus1, kSubLayers);
  const auto per_layer = [&](const std::string& key, auto value) {
    std::vector<int64_t> v;
    for (size_t i = 0; i < n; ++i) {
      v.push_back(value(i));
    }
    put(u, key, std::move(v));
  };
  const auto profile = [&](size_t i) -> const h265::ProfileInfo* {
    return ptl.sub_layer_profile_present_flag.at(i) ? &ptl.sub_layer.at(i) : nullptr;
  };
  per_layer("sub_layer_profile_present_flag", [&](size_t i) { return ptl.sub_layer_profile_present_flag.at(i); });
  per_layer("sub_layer_level_present_flag", [&](size_t i) { return ptl.sub_layer_level_present_flag.at(i); });
  per_layer("sub_layer_level_idc", [&](size_t i) { return ptl.sub_layer_level_idc.at(i); });
  per_layer("sub_layer_profile_space", [&](size_t i) { return profile(i) != nullptr ? profile(i)->profile_space : 0; });
  per_layer("sub_layer_tier_flag", [&](size_t i) { return profile(i) != nullptr ? profile(i)->tier_flag : false; });
  per_layer("sub_layer_profile_idc", [&](size_t i) { return profile(i) != nullptr ? profile(i)->profile_idc : 0; });
  per_layer("sub_layer_progressive_source_flag",
            [&](size_t i) { return profile(i) != nullptr ? profile(i)->progressive_source_flag : false; });
  per_layer("sub_layer_interlaced_source_flag",
            [&](size_t i) { return profile(i) != nullptr ? profile(i)->interlaced_source_flag : false; });
  per_layer("sub_layer_non_packed_constraint_flag",
            [&](size_t i) { return profile(i) != nullptr ? profile(i)->non_packed_constraint_flag : false; });
  per_layer("sub_layer_frame_only_constraint_flag",
            [&](size_t i) { return profile(i) != nullptr ? profile(i)->frame_only_constraint_flag : false; });
  put(u, "sub_layer_profile_compatibility_flag",
      rows(n, kCompatibilityFlags, ptl, [&](const auto&, size_t i, size_t j) {
        return profile(i) != nullptr ? int64_t{profile(i)->profile_compatibility_flag.at(j)} : 0;
      }));
}

// sps/vps_max_dec_pic_buffering_minus1[i] etc. for i = 0..max_sub_layers_minus1; without
// sub_layer_ordering_info_present_flag only i = max_sub_layers_minus1 is coded and the others take its values
// (§7.4.3.1, §7.4.3.2.1).
void put_ordering(ParsedUnit& u, const std::array<h265::SubLayerOrderingInfo, 7>& info, bool present,
                  unsigned max_sub_layers_minus1) {
  std::vector<int64_t> dpb;
  std::vector<int64_t> reorder;
  std::vector<int64_t> latency;
  for (unsigned i = 0; i <= max_sub_layers_minus1 && i < info.size(); ++i) {
    const auto& o = info.at(present ? i : max_sub_layers_minus1);
    dpb.push_back(o.max_dec_pic_buffering_minus1);
    reorder.push_back(o.max_num_reorder_pics);
    latency.push_back(o.max_latency_increase_plus1);
  }
  put(u, "max_dec_pic_buffering_minus1", std::move(dpb));
  put(u, "max_num_reorder_pics", std::move(reorder));
  put(u, "max_latency_increase_plus1", std::move(latency));
}

void put_hrd(ParsedUnit& u, const h265::HrdParameters& h) {
  const size_t n = size_t{h.max_num_sub_layers_minus1} + 1;
  const auto per_layer = [&](const std::string& key, auto value) {
    std::vector<int64_t> v;
    for (size_t i = 0; i < n && i < h.sub_layer.size(); ++i) {
      v.push_back(value(h.sub_layer.at(i)));
    }
    put(u, key, std::move(v));
  };
  per_layer("fixed_pic_rate_general_flag", [](const auto& s) { return s.fixed_pic_rate_general_flag; });
  per_layer("fixed_pic_rate_within_cvs_flag", [](const auto& s) { return s.fixed_pic_rate_within_cvs_flag; });
  per_layer("elemental_duration_in_tc_minus1", [](const auto& s) { return s.elemental_duration_in_tc_minus1; });
  per_layer("low_delay_hrd_flag", [](const auto& s) { return s.low_delay_hrd_flag; });
  per_layer("cpb_cnt_minus1", [](const auto& s) { return s.cpb_cnt_minus1; });
  put(u, "initial_cpb_removal_delay_length_minus1", {h.initial_cpb_removal_delay_length_minus1});
  put(u, "au_cpb_removal_delay_length_minus1", {h.au_cpb_removal_delay_length_minus1});
  put(u, "dpb_output_delay_length_minus1", {h.dpb_output_delay_length_minus1});
  // GStreamer keeps one sublayer_hrd_params() row per sub-layer: the VCL one when present, else the NAL one.
  const auto cpb = [&](const std::string& key, auto value) {
    put(u, key, rows(n, kCpbs, h, [&](const h265::HrdParameters& hrd, size_t i, size_t j) -> int64_t {
          const auto& s = hrd.sub_layer.at(i);
          const auto& p = hrd.vcl_hrd_parameters_present_flag ? s.vcl : s.nal;
          return j < p.cpb.size() ? int64_t{value(p.cpb.at(j))} : 0;
        }));
  };
  cpb("bit_rate_value_minus1", [](const h265::CpbParameters& c) { return c.bit_rate_value_minus1; });
  cpb("cpb_size_value_minus1", [](const h265::CpbParameters& c) { return c.cpb_size_value_minus1; });
  cpb("cpb_size_du_value_minus1", [](const h265::CpbParameters& c) { return c.cpb_size_du_value_minus1; });
  cpb("bit_rate_du_value_minus1", [](const h265::CpbParameters& c) { return c.bit_rate_du_value_minus1; });
  cpb("cbr_flag", [](const h265::CpbParameters& c) { return c.cbr_flag; });
}

// The st_ref_pic_set() arrays of GStreamer's struct for `sets` (one entry per set, DeltaPocS0/S1 and
// UsedByCurrPicS0/S1 one row of kRpsPics per set).
void put_st_rps(ParsedUnit& u, const std::vector<const h265::StRefPicSet*>& sets,
                const std::vector<h265::StRefPicSet>& sps_sets) {
  std::vector<int64_t> pred;
  std::vector<int64_t> delta_idx;
  std::vector<int64_t> sign;
  std::vector<int64_t> abs_delta;
  std::vector<int64_t> neg;
  std::vector<int64_t> pos;
  std::vector<int64_t> num;
  std::vector<int64_t> num_ref;
  std::vector<int64_t> s0;
  std::vector<int64_t> s1;
  std::vector<int64_t> u0;
  std::vector<int64_t> u1;
  for (const h265::StRefPicSet* s : sets) {
    pred.push_back(flag(s->inter_ref_pic_set_prediction_flag));
    delta_idx.push_back(s->delta_idx_minus1);
    sign.push_back(flag(s->delta_rps_sign));
    abs_delta.push_back(s->abs_delta_rps_minus1);
    neg.push_back(s->NumNegativePics);
    pos.push_back(s->NumPositivePics);
    num.push_back(s->NumDeltaPocs);
    num_ref.push_back(0);
    for (size_t j = 0; j < kRpsPics; ++j) {
      s0.push_back(s->DeltaPocS0.at(j));
      s1.push_back(s->DeltaPocS1.at(j));
      u0.push_back(flag(s->UsedByCurrPicS0.at(j)));
      u1.push_back(flag(s->UsedByCurrPicS1.at(j)));
    }
  }
  // NumDeltaPocs[RefRpsIdx] of the predicting set (§7.4.8); RefRpsIdx names an SPS set.
  for (size_t i = 0; i < sets.size(); ++i) {
    if (sets[i]->inter_ref_pic_set_prediction_flag && sets[i]->RefRpsIdx < sps_sets.size()) {
      num_ref[i] = sps_sets[sets[i]->RefRpsIdx].NumDeltaPocs;
    }
  }
  put(u, "inter_ref_pic_set_prediction_flag", std::move(pred));
  put(u, "delta_idx_minus1", std::move(delta_idx));
  put(u, "delta_rps_sign", std::move(sign));
  put(u, "abs_delta_rps_minus1", std::move(abs_delta));
  put(u, "NumNegativePics", std::move(neg));
  put(u, "NumPositivePics", std::move(pos));
  put(u, "NumDeltaPocs", std::move(num));
  put(u, "NumDeltaPocsOfRefRpsIdx", std::move(num_ref));
  put(u, "DeltaPocS0", std::move(s0));
  put(u, "DeltaPocS1", std::move(s1));
  put(u, "UsedByCurrPicS0", std::move(u0));
  put(u, "UsedByCurrPicS1", std::move(u1));
}

// pred_weight_table() as GStreamer's struct holds it: every list entry (kMaxRefIdx of them), zero where the flag
// leaves the value uncoded; the coded deltas, not the derived weights.
void put_pwt(ParsedUnit& u, const h265::PredWeightTable& t, bool b_slice) {
  const auto list = [&](const h265::PredWeightTable::List& l, const std::string& n) {
    std::vector<int64_t> lf;
    std::vector<int64_t> cf;
    std::vector<int64_t> lw;
    std::vector<int64_t> lo;
    std::vector<int64_t> cw;
    std::vector<int64_t> co;
    for (size_t i = 0; i < h265::kMaxRefIdx; ++i) {
      lf.push_back(flag(l.luma_weight_flag.at(i)));
      cf.push_back(flag(l.chroma_weight_flag.at(i)));
      lw.push_back(l.delta_luma_weight.at(i));
      lo.push_back(l.luma_offset.at(i));
      for (size_t j = 0; j < 2; ++j) {
        cw.push_back(l.delta_chroma_weight.at(i).at(j));
        co.push_back(l.delta_chroma_offset.at(i).at(j));
      }
    }
    put(u, "luma_weight_" + n + "_flag", std::move(lf));
    put(u, "chroma_weight_" + n + "_flag", std::move(cf));
    put(u, "delta_luma_weight_" + n, std::move(lw));
    put(u, "luma_offset_" + n, std::move(lo));
    put(u, "delta_chroma_weight_" + n, std::move(cw));
    put(u, "delta_chroma_offset_" + n, std::move(co));
  };
  list(t.l0, "l0");
  if (b_slice) {
    list(t.l1, "l1");
  }
}

// The scaling lists in coded (up-right diagonal) order after the §7.4.5 default rule: 4x4, 8x8 and 16x16 for the six
// matrixIds, 32x32 for matrixId 0 and 3, and the DC values. Nothing when scaling lists are off (GStreamer zeros).
void put_scaling(ParsedUnit& u, const h265::ScalingListData& s) {
  const auto lists = [&](const std::string& key, size_t size_id, size_t width, const std::vector<size_t>& ids) {
    std::vector<int64_t> v;
    for (const size_t m : ids) {
      for (size_t k = 0; k < width; ++k) {
        v.push_back(s.ScalingList.at(size_id).at(m).at(k));
      }
    }
    put(u, key, std::move(v));
  };
  lists("scaling_lists_4x4", 0, 16, {0, 1, 2, 3, 4, 5});
  lists("scaling_lists_8x8", 1, 64, {0, 1, 2, 3, 4, 5});
  lists("scaling_lists_16x16", 2, 64, {0, 1, 2, 3, 4, 5});
  lists("scaling_lists_32x32", 3, 64, {0, 3});
  std::vector<int64_t> dc16;
  for (size_t m = 0; m < 6; ++m) {
    dc16.push_back(s.scaling_list_dc_coef_minus8.at(0).at(m));
  }
  put(u, "scaling_list_dc_coef_minus8_16x16", std::move(dc16));
  put(u, "scaling_list_dc_coef_minus8_32x32",
      {s.scaling_list_dc_coef_minus8.at(1).at(0), s.scaling_list_dc_coef_minus8.at(1).at(3)});
}

}  // namespace

void add_h265_vps_derived(const h265::Vps& vps, ParsedUnit& u) {
  put_ptl(u, vps.profile_tier_level, vps.vps_max_sub_layers_minus1);
  put_ordering(u, vps.sub_layer_ordering_info, vps.vps_sub_layer_ordering_info_present_flag,
               vps.vps_max_sub_layers_minus1);
  // cprms_present_flag[0] is inferred 1 (§7.4.3.1).
  std::vector<int64_t> cprms{1};
  for (size_t i = 1; i < vps.hrd.size(); ++i) {
    cprms.push_back(flag(vps.hrd.at(i).cprms_present_flag));
  }
  put(u, "cprms_present_flag", std::move(cprms));
  if (!vps.hrd.empty()) {
    put(u, "hrd_layer_set_idx", {vps.hrd.front().hrd_layer_set_idx});
    put_hrd(u, vps.hrd.front().hrd_parameters);
  }
}

void add_h265_sps_derived(const h265::Sps& sps, ParsedUnit& u) {
  put_ptl(u, sps.profile_tier_level, sps.sps_max_sub_layers_minus1);
  put_ordering(u, sps.sub_layer_ordering_info, sps.sps_sub_layer_ordering_info_present_flag,
               sps.sps_max_sub_layers_minus1);
  std::vector<const h265::StRefPicSet*> sets;
  sets.reserve(sps.st_ref_pic_set.size());
  for (const auto& s : sps.st_ref_pic_set) {
    sets.push_back(&s);
  }
  put_st_rps(u, sets, sps.st_ref_pic_set);
  if (sps.scaling_list_enabled_flag) {
    put_scaling(u, sps.scaling_list);
  }
  if (sps.vui) {
    const auto& v = *sps.vui;
    put(u, "video_format", {v.video_format});
    put(u, "colour_primaries", {v.colour_primaries});
    put(u, "transfer_characteristics", {v.transfer_characteristics});
    put(u, "matrix_coefficients", {v.matrix_coeffs});
    put(u, "motion_vectors_over_pic_boundaries_flag", {flag(v.motion_vectors_over_pic_boundaries_flag)});
    put(u, "max_bytes_per_pic_denom", {v.max_bytes_per_pic_denom});
    put(u, "max_bits_per_min_cu_denom", {v.max_bits_per_min_cu_denom});
    put(u, "log2_max_mv_length_horizontal", {v.log2_max_mv_length_horizontal});
    put(u, "log2_max_mv_length_vertical", {v.log2_max_mv_length_vertical});
    if (v.hrd_parameters) {
      put_hrd(u, *v.hrd_parameters);
    }
  }
}

void add_h265_pps_derived(const h265::Pps& pps, const h265::Sps* sps, ParsedUnit& u) {
  put(u, "uniform_spacing_flag", {flag(pps.uniform_spacing_flag)});
  put(u, "loop_filter_across_tiles_enabled_flag", {flag(pps.loop_filter_across_tiles_enabled_flag)});
  if (sps != nullptr && pps.tiles_enabled_flag) {
    // With uniform spacing GStreamer stores the column widths and row heights of §6.5.1 (6-3, 6-4).
    const h265::ActivePps a = h265::activate_pps(pps, *sps);
    std::vector<int64_t> cols;
    for (size_t i = 0; i < pps.num_tile_columns_minus1 && i + 1 < a.colBd.size(); ++i) {
      cols.push_back(int64_t{a.colBd.at(i + 1)} - a.colBd.at(i) - 1);
    }
    std::vector<int64_t> rows_;
    for (size_t i = 0; i < pps.num_tile_rows_minus1 && i + 1 < a.rowBd.size(); ++i) {
      rows_.push_back(int64_t{a.rowBd.at(i + 1)} - a.rowBd.at(i) - 1);
    }
    put(u, "column_width_minus1", std::move(cols));
    put(u, "row_height_minus1", std::move(rows_));
  }
  // Only the PPS's own lists: GStreamer does not copy the SPS lists into a PPS without scaling_list_data().
  if (pps.pps_scaling_list_data_present_flag && pps.scaling_list) {
    put_scaling(u, *pps.scaling_list);
  }
}

void add_h265_slice_derived(const h265::SliceSegmentHeader& sh, const h265::ActivePps* active, ParsedUnit& u) {
  if (sh.dependent_slice_segment_flag) {
    // GStreamer does not copy the independent segment's header into a dependent one: it keeps the defaults of a
    // fresh header and the PPS inference of slice_loop_filter_across_slices_enabled_flag.
    put(u, "pic_output_flag", {1});
    put(u, "collocated_from_l0_flag", {1});
    if (active != nullptr) {
      put(u, "loop_filter_across_slices_enabled_flag", {flag(active->pps->pps_loop_filter_across_slices_enabled_flag)});
    }
    return;
  }
  put(u, "pic_output_flag", {flag(sh.pic_output_flag)});
  put(u, "collocated_from_l0_flag", {flag(sh.collocated_from_l0_flag)});
  put(u, "loop_filter_across_slices_enabled_flag", {flag(sh.slice_loop_filter_across_slices_enabled_flag)});
  put(u, "deblocking_filter_disabled_flag", {flag(sh.slice_deblocking_filter_disabled_flag)});
  put(u, "slice_beta_offset_div2", {sh.slice_beta_offset_div2});
  put(u, "slice_tc_offset_div2", {sh.slice_tc_offset_div2});
  // P and B slices: without num_ref_idx_active_override_flag GStreamer takes both PPS defaults, whatever the slice
  // type; with it, l0 is coded and l1 only for B slices (a P slice keeps 0). The spec leaves l1 unused in P slices.
  if (sh.slice_type != h265::kSliceI) {
    put(u, "num_ref_idx_l0_active_minus1", {sh.num_ref_idx_l0_active_minus1});
    if (sh.slice_type == h265::kSliceB) {
      put(u, "num_ref_idx_l1_active_minus1", {sh.num_ref_idx_l1_active_minus1});
    } else if (!sh.num_ref_idx_active_override_flag && active != nullptr) {
      put(u, "num_ref_idx_l1_active_minus1", {active->pps->num_ref_idx_l1_default_active_minus1});
    }
  }
  if (active != nullptr && !sh.short_term_ref_pic_set_sps_flag) {
    put_st_rps(u, {&sh.st_ref_pic_set}, active->sps->st_ref_pic_set);
  }
  if (sh.pred_weight_table) {
    put_pwt(u, *sh.pred_weight_table, sh.slice_type == h265::kSliceB);
  }
}

}  // namespace ovt::test::syntax_golden
