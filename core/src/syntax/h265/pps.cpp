// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 pic_parameter_set_rbsp() parser and the ActivePps tile derivation (H.265 §7.3.2.3, §7.4.3.3, §6.5.1;
// chapter 7 §7.3.5 row 8, §7.3.8; WI-1.112).

#include "ovt/cxx/syntax/h265/pps.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string_view>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// SPEC-ASSUMPTION(#327): see sps.cpp; the same proposed code marks the extensions kept opaque in phase 1.
constexpr std::string_view kOpaqueCode = "SYN-H265-0004";

// Level 6.2 allows 20 tile columns and 22 rows (Table A.8); a PPS cannot be checked against its SPS here, so the
// level limit is a Diagnostic and only an absurd count (more than the 4096 CTB columns of a 65535-sample picture)
// throws.
constexpr int64_t kMaxTilesMinus1 = 4095;
// The tile tables are sized by PicSizeInCtbsY; cap it at the level 6.2 MaxLumaPs (35 651 584) in 16x16 CTBs so a
// hostile SPS cannot ask for hundreds of megabytes of scan tables (the level check itself is WI-4.109).
constexpr uint64_t kMaxCtbs = 35651584ULL / 256ULL;

void opaque_tail(SyntaxBuilder& b, Pps& pps, std::string_view node, std::string_view what) {
  b.diag(Severity::Warning, kOpaqueCode,
         fmt::format("{} not parsed in phase 1; remaining bits kept as opaque bytes", what), "H.265 7.3.2.3");
  b.opaque_rest(node);
  pps.opaque_tail = true;
}

void read_tiles(BitReader& br, SyntaxBuilder& b, Pps& pps) {
  pps.num_tile_columns_minus1 = b.read_ue("num_tile_columns_minus1");
  detail::require_range(br, "num_tile_columns_minus1", pps.num_tile_columns_minus1, 0, kMaxTilesMinus1);
  detail::check_range(b, "num_tile_columns_minus1", pps.num_tile_columns_minus1, 0, 19, "H.265 A.4.1");
  pps.num_tile_rows_minus1 = b.read_ue("num_tile_rows_minus1");
  detail::require_range(br, "num_tile_rows_minus1", pps.num_tile_rows_minus1, 0, kMaxTilesMinus1);
  detail::check_range(b, "num_tile_rows_minus1", pps.num_tile_rows_minus1, 0, 21, "H.265 A.4.1");
  if (pps.num_tile_columns_minus1 == 0 && pps.num_tile_rows_minus1 == 0) {
    b.diag(Severity::Error, "SYN-H265-0007",
           "num_tile_columns_minus1/num_tile_rows_minus1 out of range (both 0 with tiles_enabled_flag = 1)",
           "H.265 7.4.3.3.1");
  }
  pps.uniform_spacing_flag = b.read_flag("uniform_spacing_flag");
  if (!pps.uniform_spacing_flag) {
    pps.column_width_minus1.resize(pps.num_tile_columns_minus1);
    for (uint32_t i = 0; i < pps.num_tile_columns_minus1; ++i) {
      pps.column_width_minus1[i] = detail::read_ue_at(b, "column_width_minus1", static_cast<int>(i));
    }
    pps.row_height_minus1.resize(pps.num_tile_rows_minus1);
    for (uint32_t i = 0; i < pps.num_tile_rows_minus1; ++i) {
      pps.row_height_minus1[i] = detail::read_ue_at(b, "row_height_minus1", static_cast<int>(i));
    }
  }
  pps.loop_filter_across_tiles_enabled_flag = b.read_flag("loop_filter_across_tiles_enabled_flag");
}

void read_deblocking(BitReader& br, SyntaxBuilder& b, Pps& pps) {
  pps.deblocking_filter_override_enabled_flag = b.read_flag("deblocking_filter_override_enabled_flag");
  pps.pps_deblocking_filter_disabled_flag = b.read_flag("pps_deblocking_filter_disabled_flag");
  if (!pps.pps_deblocking_filter_disabled_flag) {
    pps.pps_beta_offset_div2 = b.read_se("pps_beta_offset_div2");
    detail::require_range(br, "pps_beta_offset_div2", pps.pps_beta_offset_div2, -6, 6);
    pps.pps_tc_offset_div2 = b.read_se("pps_tc_offset_div2");
    detail::require_range(br, "pps_tc_offset_div2", pps.pps_tc_offset_div2, -6, 6);
  }
}

void read_range_extension(BitReader& br, SyntaxBuilder& b, const Pps& pps, PpsRangeExtension& r) {
  auto s = b.scoped("pps_range_extension", "H.265 7.3.2.3.2");
  if (pps.transform_skip_enabled_flag) {
    r.log2_max_transform_skip_block_size_minus2 = b.read_ue("log2_max_transform_skip_block_size_minus2");
    detail::require_range(br, "log2_max_transform_skip_block_size_minus2", r.log2_max_transform_skip_block_size_minus2,
                          0, 3);
  }
  r.cross_component_prediction_enabled_flag = b.read_flag("cross_component_prediction_enabled_flag");
  r.chroma_qp_offset_list_enabled_flag = b.read_flag("chroma_qp_offset_list_enabled_flag");
  if (r.chroma_qp_offset_list_enabled_flag) {
    r.diff_cu_chroma_qp_offset_depth = b.read_ue("diff_cu_chroma_qp_offset_depth");
    detail::require_range(br, "diff_cu_chroma_qp_offset_depth", r.diff_cu_chroma_qp_offset_depth, 0, 3);
    r.chroma_qp_offset_list_len_minus1 = b.read_ue("chroma_qp_offset_list_len_minus1");
    detail::require_range(br, "chroma_qp_offset_list_len_minus1", r.chroma_qp_offset_list_len_minus1, 0, 5);
    for (uint32_t i = 0; i <= r.chroma_qp_offset_list_len_minus1; ++i) {
      r.cb_qp_offset_list[i] = detail::read_se_at(b, "cb_qp_offset_list", static_cast<int>(i));
      detail::require_range(br, "cb_qp_offset_list", r.cb_qp_offset_list[i], -12, 12);
      r.cr_qp_offset_list[i] = detail::read_se_at(b, "cr_qp_offset_list", static_cast<int>(i));
      detail::require_range(br, "cr_qp_offset_list", r.cr_qp_offset_list[i], -12, 12);
    }
  }
  r.log2_sao_offset_scale_luma = b.read_ue("log2_sao_offset_scale_luma");
  r.log2_sao_offset_scale_chroma = b.read_ue("log2_sao_offset_scale_chroma");
}

void read_palette_initializers(BitReader& br, SyntaxBuilder& b, PpsSccExtension& e) {
  e.pps_num_palette_predictor_initializers = b.read_ue("pps_num_palette_predictor_initializers");
  detail::require_range(br, "pps_num_palette_predictor_initializers", e.pps_num_palette_predictor_initializers, 0, 128);
  if (e.pps_num_palette_predictor_initializers == 0) {
    return;
  }
  e.monochrome_palette_flag = b.read_flag("monochrome_palette_flag");
  e.luma_bit_depth_entry_minus8 = b.read_ue("luma_bit_depth_entry_minus8");
  detail::require_range(br, "luma_bit_depth_entry_minus8", e.luma_bit_depth_entry_minus8, 0, 8);
  if (!e.monochrome_palette_flag) {
    e.chroma_bit_depth_entry_minus8 = b.read_ue("chroma_bit_depth_entry_minus8");
    detail::require_range(br, "chroma_bit_depth_entry_minus8", e.chroma_bit_depth_entry_minus8, 0, 8);
  }
  const unsigned num_comps = e.monochrome_palette_flag ? 1U : 3U;
  for (unsigned comp = 0; comp < num_comps; ++comp) {
    const unsigned bits = (comp == 0 ? e.luma_bit_depth_entry_minus8 : e.chroma_bit_depth_entry_minus8) + 8U;
    b.begin_array_element("comp", static_cast<int>(comp));
    auto& list = e.pps_palette_predictor_initializer[comp];
    list.resize(e.pps_num_palette_predictor_initializers);
    for (uint32_t i = 0; i < e.pps_num_palette_predictor_initializers; ++i) {
      list[i] = static_cast<uint32_t>(
          detail::read_u_at(b, "pps_palette_predictor_initializer", static_cast<int>(i), bits, "u(v)"));
    }
    b.end_array_element();
  }
}

void read_scc_extension(BitReader& br, SyntaxBuilder& b, PpsSccExtension& e) {
  auto s = b.scoped("pps_scc_extension", "H.265 7.3.2.3.3");
  e.pps_curr_pic_ref_enabled_flag = b.read_flag("pps_curr_pic_ref_enabled_flag");
  e.residual_adaptive_colour_transform_enabled_flag = b.read_flag("residual_adaptive_colour_transform_enabled_flag");
  if (e.residual_adaptive_colour_transform_enabled_flag) {
    e.pps_slice_act_qp_offsets_present_flag = b.read_flag("pps_slice_act_qp_offsets_present_flag");
    e.pps_act_y_qp_offset_plus5 = b.read_se("pps_act_y_qp_offset_plus5");
    detail::require_range(br, "pps_act_y_qp_offset_plus5", e.pps_act_y_qp_offset_plus5, -7, 17);
    e.pps_act_cb_qp_offset_plus5 = b.read_se("pps_act_cb_qp_offset_plus5");
    detail::require_range(br, "pps_act_cb_qp_offset_plus5", e.pps_act_cb_qp_offset_plus5, -7, 17);
    e.pps_act_cr_qp_offset_plus3 = b.read_se("pps_act_cr_qp_offset_plus3");
    detail::require_range(br, "pps_act_cr_qp_offset_plus3", e.pps_act_cr_qp_offset_plus3, -9, 15);
  }
  e.pps_palette_predictor_initializers_present_flag = b.read_flag("pps_palette_predictor_initializers_present_flag");
  if (e.pps_palette_predictor_initializers_present_flag) {
    read_palette_initializers(br, b, e);
  }
}

// Returns false when the rest of the RBSP was consumed as opaque bytes.
bool read_extensions(BitReader& br, SyntaxBuilder& b, Pps& pps) {
  pps.pps_range_extension_flag = b.read_flag("pps_range_extension_flag");
  pps.pps_multilayer_extension_flag = b.read_flag("pps_multilayer_extension_flag");
  pps.pps_3d_extension_flag = b.read_flag("pps_3d_extension_flag");
  pps.pps_scc_extension_flag = b.read_flag("pps_scc_extension_flag");
  pps.pps_extension_4bits = static_cast<uint8_t>(b.read_u("pps_extension_4bits", 4));
  if (pps.pps_range_extension_flag) {
    read_range_extension(br, b, pps, pps.range_extension);
  }
  if (pps.pps_multilayer_extension_flag) {
    opaque_tail(b, pps, "pps_multilayer_extension", "pps_multilayer_extension() (F.7.3.2.3.4)");
    return false;
  }
  if (pps.pps_3d_extension_flag) {
    opaque_tail(b, pps, "pps_3d_extension", "pps_3d_extension() (I.7.3.2.3.7)");
    return false;
  }
  if (pps.pps_scc_extension_flag) {
    read_scc_extension(br, b, pps.scc_extension);
  }
  if (pps.pps_extension_4bits != 0) {
    opaque_tail(b, pps, "pps_extension_data_flag", "pps_extension_4bits data");
    return false;
  }
  return true;
}

void read_core(BitReader& br, SyntaxBuilder& b, Pps& pps) {
  pps.dependent_slice_segments_enabled_flag = b.read_flag("dependent_slice_segments_enabled_flag");
  pps.output_flag_present_flag = b.read_flag("output_flag_present_flag");
  pps.num_extra_slice_header_bits = static_cast<uint8_t>(b.read_u("num_extra_slice_header_bits", 3));
  detail::check_range(b, "num_extra_slice_header_bits", pps.num_extra_slice_header_bits, 0, 2, "H.265 7.4.3.3.1");
  pps.sign_data_hiding_enabled_flag = b.read_flag("sign_data_hiding_enabled_flag");
  pps.cabac_init_present_flag = b.read_flag("cabac_init_present_flag");
  pps.num_ref_idx_l0_default_active_minus1 = b.read_ue("num_ref_idx_l0_default_active_minus1");
  // Hard bounds (ParseError) on everything a later stage adds, shifts or sizes a list with: QP values and offsets
  // enter int32 arithmetic, the log2 fields become shift counts, num_ref_idx sizes the reference lists.
  detail::require_range(br, "num_ref_idx_l0_default_active_minus1", pps.num_ref_idx_l0_default_active_minus1, 0, 14);
  pps.num_ref_idx_l1_default_active_minus1 = b.read_ue("num_ref_idx_l1_default_active_minus1");
  detail::require_range(br, "num_ref_idx_l1_default_active_minus1", pps.num_ref_idx_l1_default_active_minus1, 0, 14);
  pps.init_qp_minus26 = b.read_se("init_qp_minus26");
  detail::require_range(br, "init_qp_minus26", pps.init_qp_minus26, -(26 + 48), 25);  // QpBdOffsetY <= 48
  pps.SliceQpY_default = 26 + pps.init_qp_minus26;
  b.derived("SliceQpY(default)", pps.SliceQpY_default);
  pps.constrained_intra_pred_flag = b.read_flag("constrained_intra_pred_flag");
  pps.transform_skip_enabled_flag = b.read_flag("transform_skip_enabled_flag");
  pps.cu_qp_delta_enabled_flag = b.read_flag("cu_qp_delta_enabled_flag");
  if (pps.cu_qp_delta_enabled_flag) {
    pps.diff_cu_qp_delta_depth = b.read_ue("diff_cu_qp_delta_depth");
    detail::require_range(br, "diff_cu_qp_delta_depth", pps.diff_cu_qp_delta_depth, 0, 3);
  }
  pps.pps_cb_qp_offset = b.read_se("pps_cb_qp_offset");
  detail::require_range(br, "pps_cb_qp_offset", pps.pps_cb_qp_offset, -12, 12);
  pps.pps_cr_qp_offset = b.read_se("pps_cr_qp_offset");
  detail::require_range(br, "pps_cr_qp_offset", pps.pps_cr_qp_offset, -12, 12);
  pps.pps_slice_chroma_qp_offsets_present_flag = b.read_flag("pps_slice_chroma_qp_offsets_present_flag");
  pps.weighted_pred_flag = b.read_flag("weighted_pred_flag");
  pps.weighted_bipred_flag = b.read_flag("weighted_bipred_flag");
  pps.transquant_bypass_enabled_flag = b.read_flag("transquant_bypass_enabled_flag");
  pps.tiles_enabled_flag = b.read_flag("tiles_enabled_flag");
  pps.entropy_coding_sync_enabled_flag = b.read_flag("entropy_coding_sync_enabled_flag");
  if (pps.tiles_enabled_flag) {
    read_tiles(br, b, pps);
  }
  pps.pps_loop_filter_across_slices_enabled_flag = b.read_flag("pps_loop_filter_across_slices_enabled_flag");
  pps.deblocking_filter_control_present_flag = b.read_flag("deblocking_filter_control_present_flag");
  if (pps.deblocking_filter_control_present_flag) {
    read_deblocking(br, b, pps);
  }
  pps.pps_scaling_list_data_present_flag = b.read_flag("pps_scaling_list_data_present_flag");
  if (pps.pps_scaling_list_data_present_flag) {
    pps.scaling_list = parse_scaling_list_data(br, b);
  }
  pps.lists_modification_present_flag = b.read_flag("lists_modification_present_flag");
  pps.log2_parallel_merge_level_minus2 = b.read_ue("log2_parallel_merge_level_minus2");
  detail::require_range(br, "log2_parallel_merge_level_minus2", pps.log2_parallel_merge_level_minus2, 0, 4);
  pps.slice_segment_header_extension_present_flag = b.read_flag("slice_segment_header_extension_present_flag");
}

// §6.5.1 (6-3, 6-4): tile column boundaries in CTBs; the explicit widths are clamped so that every tile has at
// least one CTB and the last one absorbs the remainder.
std::vector<uint32_t> boundaries(uint32_t num_minus1, bool uniform, const std::vector<uint32_t>& sizes_minus1,
                                 uint32_t pic_in_ctbs, std::string_view what, ActivePps& out) {
  std::vector<uint32_t> bd(static_cast<size_t>(num_minus1) + 2, 0);
  if (uniform) {
    for (uint32_t i = 0; i <= num_minus1; ++i) {
      const uint64_t width =
          (uint64_t{i} + 1) * pic_in_ctbs / (num_minus1 + 1) - uint64_t{i} * pic_in_ctbs / (num_minus1 + 1);
      bd[i + 1] = bd[i] + static_cast<uint32_t>(width);
    }
    return bd;
  }
  for (uint32_t i = 0; i < num_minus1; ++i) {
    const uint64_t width = uint64_t{sizes_minus1[i]} + 1;
    const uint64_t left = uint64_t{pic_in_ctbs} - bd[i];
    const uint64_t remaining_tiles = uint64_t{num_minus1} - i;  // tiles after this one, each needs >= 1 CTB
    if (width + remaining_tiles > left) {
      out.issues.push_back(
          {"SYN-H265-0007", fmt::format("{}[{}] out of range ({} = {}, tiles exceed the picture of {} CTBs)", what, i,
                                        what, sizes_minus1[i], pic_in_ctbs)});
      bd[i + 1] = bd[i] + static_cast<uint32_t>(left > remaining_tiles ? left - remaining_tiles : 1);
      continue;
    }
    bd[i + 1] = bd[i] + static_cast<uint32_t>(width);
  }
  bd[static_cast<size_t>(num_minus1) + 1] = std::max(pic_in_ctbs, bd[num_minus1] + 1);
  return bd;
}

}  // namespace

Pps parse_pps(BitReader& br, SyntaxBuilder& b) {
  Pps pps;
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("pic_parameter_set_rbsp", "H.265 7.3.2.3.1");
  const uint32_t pps_id = b.read_ue("pps_pic_parameter_set_id");
  if (pps_id > 63) {
    throw ParseError{"SYN-H265-0010", "pps_pic_parameter_set_id out of range [0,63]", br.bit_pos()};
  }
  pps.pps_pic_parameter_set_id = static_cast<uint8_t>(pps_id);
  const uint32_t sps_id = b.read_ue("pps_seq_parameter_set_id");
  detail::require_range(br, "pps_seq_parameter_set_id", sps_id, 0, 15);
  pps.pps_seq_parameter_set_id = static_cast<uint8_t>(sps_id);
  read_core(br, b, pps);
  pps.pps_extension_present_flag = b.read_flag("pps_extension_present_flag");
  bool complete = true;
  if (pps.pps_extension_present_flag) {
    complete = read_extensions(br, b, pps);
  }
  pps.Log2ParMrgLevel = pps.log2_parallel_merge_level_minus2 + 2;
  pps.Log2MaxTransformSkipSize = pps.range_extension.log2_max_transform_skip_block_size_minus2 + 2;
  b.derived("Log2ParMrgLevel", pps.Log2ParMrgLevel);
  b.derived("Log2MaxTransformSkipSize", pps.Log2MaxTransformSkipSize);
  if (complete) {
    detail::read_rbsp_trailing_bits(br, b);
  }
  pps.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return pps;
}

ActivePps activate_pps(const Pps& pps, const Sps& sps) {
  ActivePps a;
  a.pps = &pps;
  a.sps = &sps;
  const uint32_t cols_minus1 = pps.tiles_enabled_flag ? pps.num_tile_columns_minus1 : 0;
  const uint32_t rows_minus1 = pps.tiles_enabled_flag ? pps.num_tile_rows_minus1 : 0;
  const bool uniform = !pps.tiles_enabled_flag || pps.uniform_spacing_flag;
  const uint32_t w = std::max<uint32_t>(sps.PicWidthInCtbsY, 1);
  const uint32_t h = std::max<uint32_t>(sps.PicHeightInCtbsY, 1);
  if (uint64_t{w} * h > kMaxCtbs) {
    a.issues.push_back({"SYN-H265-0007", fmt::format("PicSizeInCtbsY out of range ({}x{} CTBs exceeds MaxLumaPs of "
                                                     "level 6.2); tile tables not derived",
                                                     w, h)});
    return a;
  }
  if (cols_minus1 >= w || rows_minus1 >= h) {
    a.issues.push_back(
        {"SYN-H265-0007", fmt::format("num_tile_columns_minus1/num_tile_rows_minus1 out of range ({}/{} for a picture "
                                      "of {}x{} CTBs)",
                                      cols_minus1, rows_minus1, w, h)});
    return a;
  }
  a.colBd = boundaries(cols_minus1, uniform, pps.column_width_minus1, w, "column_width_minus1", a);
  a.rowBd = boundaries(rows_minus1, uniform, pps.row_height_minus1, h, "row_height_minus1", a);
  for (uint32_t i = 0; i <= cols_minus1; ++i) {
    a.ColumnWidthInLumaSamples.push_back((a.colBd[i + 1] - a.colBd[i]) << sps.CtbLog2SizeY);
  }
  for (uint32_t j = 0; j <= rows_minus1; ++j) {
    a.RowHeightInLumaSamples.push_back((a.rowBd[j + 1] - a.rowBd[j]) << sps.CtbLog2SizeY);
  }
  // 6-5 .. 6-9: raster <-> tile scan conversion and TileId.
  const uint32_t pic_size = w * h;
  a.CtbAddrRsToTs.resize(pic_size);
  a.CtbAddrTsToRs.resize(pic_size);
  a.TileId.resize(pic_size);
  for (uint32_t rs = 0; rs < pic_size; ++rs) {
    const uint32_t tb_x = rs % w;
    const uint32_t tb_y = rs / w;
    uint32_t tile_x = 0;
    for (uint32_t i = 0; i <= cols_minus1; ++i) {
      if (tb_x >= a.colBd[i]) {
        tile_x = i;
      }
    }
    uint32_t tile_y = 0;
    for (uint32_t j = 0; j <= rows_minus1; ++j) {
      if (tb_y >= a.rowBd[j]) {
        tile_y = j;
      }
    }
    uint32_t ts = 0;
    for (uint32_t i = 0; i < tile_x; ++i) {
      ts += (a.rowBd[tile_y + 1] - a.rowBd[tile_y]) * (a.colBd[i + 1] - a.colBd[i]);
    }
    for (uint32_t j = 0; j < tile_y; ++j) {
      ts += w * (a.rowBd[j + 1] - a.rowBd[j]);
    }
    ts += (tb_y - a.rowBd[tile_y]) * (a.colBd[tile_x + 1] - a.colBd[tile_x]) + tb_x - a.colBd[tile_x];
    a.CtbAddrRsToTs[rs] = ts;
    a.CtbAddrTsToRs[ts] = rs;
    a.TileId[ts] = tile_y * (cols_minus1 + 1) + tile_x;
  }
  a.Log2MinCuQpDeltaSize = sps.CtbLog2SizeY - std::min(pps.diff_cu_qp_delta_depth, sps.CtbLog2SizeY);
  a.Log2MinCuChromaQpOffsetSize =
      sps.CtbLog2SizeY - std::min(pps.range_extension.diff_cu_chroma_qp_offset_depth, sps.CtbLog2SizeY);
  a.Log2ParMrgLevel = pps.Log2ParMrgLevel;
  a.Log2MaxTransformSkipSize = pps.Log2MaxTransformSkipSize;
  a.SliceQpY_default = pps.SliceQpY_default;
  return a;
}

}  // namespace ovt::syntax::h265
