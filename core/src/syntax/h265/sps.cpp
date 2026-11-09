// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 seq_parameter_set_rbsp() parser and the §7.4.3.2.1 derivations (chapter 7 §7.3.5 row 4, WI-1.112).

#include "ovt/cxx/syntax/h265/sps.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// SPEC-ASSUMPTION(#327): §7.3.5 row 4 asks for "opaque with Diagnostic" for a multilayer SPS, sps_3d_extension()
// and sps_extension_data_flag but names no code; chapter 7 §7.8 has none for "not parsed in phase 1". The code
// SYN-H265-0004 (Warning) is proposed in the spec question and used here until it is answered.
constexpr std::string_view kOpaqueCode = "SYN-H265-0004";

void opaque_tail(SyntaxBuilder& b, Sps& sps, std::string_view node, std::string_view what) {
  b.diag(Severity::Warning, kOpaqueCode,
         fmt::format("{} not parsed in phase 1; remaining bits kept as opaque bytes", what), "H.265 7.3.2.2");
  b.opaque_rest(node);
  sps.opaque_tail = true;
}

void read_sub_layer_ordering_info(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  const unsigned first = sps.sps_sub_layer_ordering_info_present_flag ? 0U : sps.sps_max_sub_layers_minus1;
  for (unsigned i = first; i <= sps.sps_max_sub_layers_minus1; ++i) {
    SubLayerOrderingInfo& o = sps.sub_layer_ordering_info[i];
    const int idx = static_cast<int>(i);
    o.max_dec_pic_buffering_minus1 = detail::read_ue_at(b, "sps_max_dec_pic_buffering_minus1", idx);
    detail::require_range(br, "sps_max_dec_pic_buffering_minus1", o.max_dec_pic_buffering_minus1, 0, 15);  // DPB size
    o.max_num_reorder_pics = detail::read_ue_at(b, "sps_max_num_reorder_pics", idx);
    detail::check_range(b, "sps_max_num_reorder_pics", o.max_num_reorder_pics, 0, o.max_dec_pic_buffering_minus1,
                        "H.265 7.4.3.2.1");
    o.max_latency_increase_plus1 = detail::read_ue_at(b, "sps_max_latency_increase_plus1", idx);
  }
  for (unsigned i = 0; i < first; ++i) {
    sps.sub_layer_ordering_info[i] = sps.sub_layer_ordering_info[sps.sps_max_sub_layers_minus1];
  }
}

// Arithmetic-safety bound only: with 16-bit dimensions every Ctb/MinCb count and PicSizeInSamplesY (uint64) stays in
// range. The level limit (A.4.1 MaxLumaPs) is the Warning SYN-CONF-0001 of WI-4.109, so oversize pictures keep parsing.
constexpr uint32_t kMaxLumaDimension = 65535;

void read_picture_format(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  sps.chroma_format_idc = static_cast<uint8_t>(b.read_ue("chroma_format_idc"));
  detail::require_range(br, "chroma_format_idc", sps.chroma_format_idc, 0, 3);
  static constexpr std::array<std::string_view, 4> kChromaNames = {"4:0:0", "4:2:0", "4:2:2", "4:4:4"};
  b.set_enum(kChromaNames[sps.chroma_format_idc]);
  if (sps.chroma_format_idc == 3) {
    sps.separate_colour_plane_flag = b.read_flag("separate_colour_plane_flag");
  }
  sps.pic_width_in_luma_samples = b.read_ue("pic_width_in_luma_samples");
  detail::require_range(br, "pic_width_in_luma_samples", sps.pic_width_in_luma_samples, 1, kMaxLumaDimension);
  sps.pic_height_in_luma_samples = b.read_ue("pic_height_in_luma_samples");
  detail::require_range(br, "pic_height_in_luma_samples", sps.pic_height_in_luma_samples, 1, kMaxLumaDimension);
  sps.conformance_window_flag = b.read_flag("conformance_window_flag");
  if (sps.conformance_window_flag) {
    sps.conf_win_left_offset = b.read_ue("conf_win_left_offset");
    sps.conf_win_right_offset = b.read_ue("conf_win_right_offset");
    sps.conf_win_top_offset = b.read_ue("conf_win_top_offset");
    sps.conf_win_bottom_offset = b.read_ue("conf_win_bottom_offset");
  }
  const uint32_t bdl = b.read_ue("bit_depth_luma_minus8");
  detail::require_range(br, "bit_depth_luma_minus8", bdl, 0, 8);
  sps.bit_depth_luma_minus8 = static_cast<uint8_t>(bdl);
  const uint32_t bdc = b.read_ue("bit_depth_chroma_minus8");
  detail::require_range(br, "bit_depth_chroma_minus8", bdc, 0, 8);
  sps.bit_depth_chroma_minus8 = static_cast<uint8_t>(bdc);
  const uint32_t poc = b.read_ue("log2_max_pic_order_cnt_lsb_minus4");
  detail::require_range(br, "log2_max_pic_order_cnt_lsb_minus4", poc, 0, 12);
  sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(poc);
}

void read_block_sizes(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  // Hard bounds on every log2 size and depth: they become shift counts and loop depths here, in the PPS
  // activation and in the slice/CU parsers (WI-1.113), so an unbounded value must not leave this function.
  sps.log2_min_luma_coding_block_size_minus3 = b.read_ue("log2_min_luma_coding_block_size_minus3");
  detail::require_range(br, "log2_min_luma_coding_block_size_minus3", sps.log2_min_luma_coding_block_size_minus3, 0, 3);
  sps.log2_diff_max_min_luma_coding_block_size = b.read_ue("log2_diff_max_min_luma_coding_block_size");
  detail::require_range(br, "log2_diff_max_min_luma_coding_block_size", sps.log2_diff_max_min_luma_coding_block_size, 0,
                        3);
  sps.log2_min_luma_transform_block_size_minus2 = b.read_ue("log2_min_luma_transform_block_size_minus2");
  detail::require_range(br, "log2_min_luma_transform_block_size_minus2", sps.log2_min_luma_transform_block_size_minus2,
                        0, 3);
  sps.log2_diff_max_min_luma_transform_block_size = b.read_ue("log2_diff_max_min_luma_transform_block_size");
  detail::require_range(br, "log2_diff_max_min_luma_transform_block_size",
                        sps.log2_diff_max_min_luma_transform_block_size, 0, 3);
  sps.max_transform_hierarchy_depth_inter = b.read_ue("max_transform_hierarchy_depth_inter");
  detail::require_range(br, "max_transform_hierarchy_depth_inter", sps.max_transform_hierarchy_depth_inter, 0, 4);
  sps.max_transform_hierarchy_depth_intra = b.read_ue("max_transform_hierarchy_depth_intra");
  detail::require_range(br, "max_transform_hierarchy_depth_intra", sps.max_transform_hierarchy_depth_intra, 0, 4);
}

void read_scaling_and_tools(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  sps.scaling_list_enabled_flag = b.read_flag("scaling_list_enabled_flag");
  if (sps.scaling_list_enabled_flag) {
    sps.sps_scaling_list_data_present_flag = b.read_flag("sps_scaling_list_data_present_flag");
    sps.scaling_list =
        sps.sps_scaling_list_data_present_flag ? parse_scaling_list_data(br, b) : default_scaling_list_data();
  } else {
    sps.scaling_list = flat_scaling_list_data();
  }
  sps.amp_enabled_flag = b.read_flag("amp_enabled_flag");
  sps.sample_adaptive_offset_enabled_flag = b.read_flag("sample_adaptive_offset_enabled_flag");
  sps.pcm_enabled_flag = b.read_flag("pcm_enabled_flag");
  if (sps.pcm_enabled_flag) {
    sps.pcm_sample_bit_depth_luma_minus1 = static_cast<uint8_t>(b.read_u("pcm_sample_bit_depth_luma_minus1", 4));
    sps.pcm_sample_bit_depth_chroma_minus1 = static_cast<uint8_t>(b.read_u("pcm_sample_bit_depth_chroma_minus1", 4));
    sps.log2_min_pcm_luma_coding_block_size_minus3 = b.read_ue("log2_min_pcm_luma_coding_block_size_minus3");
    detail::require_range(br, "log2_min_pcm_luma_coding_block_size_minus3",
                          sps.log2_min_pcm_luma_coding_block_size_minus3, 0, 2);
    sps.log2_diff_max_min_pcm_luma_coding_block_size = b.read_ue("log2_diff_max_min_pcm_luma_coding_block_size");
    detail::require_range(br, "log2_diff_max_min_pcm_luma_coding_block_size",
                          sps.log2_diff_max_min_pcm_luma_coding_block_size, 0, 2);
    sps.pcm_loop_filter_disabled_flag = b.read_flag("pcm_loop_filter_disabled_flag");
  }
}

void read_reference_sets(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  sps.num_short_term_ref_pic_sets = b.read_ue("num_short_term_ref_pic_sets");
  detail::require_range(br, "num_short_term_ref_pic_sets", sps.num_short_term_ref_pic_sets, 0, 64);
  sps.st_ref_pic_set.reserve(sps.num_short_term_ref_pic_sets);
  const uint32_t max_dpb = sps.sub_layer_ordering_info[sps.sps_max_sub_layers_minus1].max_dec_pic_buffering_minus1;
  for (uint32_t i = 0; i < sps.num_short_term_ref_pic_sets; ++i) {
    sps.st_ref_pic_set.push_back(parse_st_ref_pic_set(br, b, i, sps.num_short_term_ref_pic_sets,
                                                      std::span<const StRefPicSet>(sps.st_ref_pic_set), max_dpb));
  }
  sps.long_term_ref_pics_present_flag = b.read_flag("long_term_ref_pics_present_flag");
  if (sps.long_term_ref_pics_present_flag) {
    sps.num_long_term_ref_pics_sps = b.read_ue("num_long_term_ref_pics_sps");
    detail::require_range(br, "num_long_term_ref_pics_sps", sps.num_long_term_ref_pics_sps, 0, 32);
    const unsigned v = sps.log2_max_pic_order_cnt_lsb_minus4 + 4U;
    for (uint32_t i = 0; i < sps.num_long_term_ref_pics_sps; ++i) {
      sps.lt_ref_pic_poc_lsb_sps[i] =
          static_cast<uint32_t>(detail::read_u_at(b, "lt_ref_pic_poc_lsb_sps", static_cast<int>(i), v, "u(v)"));
      sps.used_by_curr_pic_lt_sps_flag[i] =
          detail::read_flag_at(b, "used_by_curr_pic_lt_sps_flag", static_cast<int>(i));
    }
  }
  sps.sps_temporal_mvp_enabled_flag = b.read_flag("sps_temporal_mvp_enabled_flag");
  sps.strong_intra_smoothing_enabled_flag = b.read_flag("strong_intra_smoothing_enabled_flag");
}

void read_range_extension(SyntaxBuilder& b, SpsRangeExtension& r) {
  auto s = b.scoped("sps_range_extension", "H.265 7.3.2.2.2");
  r.transform_skip_rotation_enabled_flag = b.read_flag("transform_skip_rotation_enabled_flag");
  r.transform_skip_context_enabled_flag = b.read_flag("transform_skip_context_enabled_flag");
  r.implicit_rdpcm_enabled_flag = b.read_flag("implicit_rdpcm_enabled_flag");
  r.explicit_rdpcm_enabled_flag = b.read_flag("explicit_rdpcm_enabled_flag");
  r.extended_precision_processing_flag = b.read_flag("extended_precision_processing_flag");
  r.intra_smoothing_disabled_flag = b.read_flag("intra_smoothing_disabled_flag");
  r.high_precision_offsets_enabled_flag = b.read_flag("high_precision_offsets_enabled_flag");
  r.persistent_rice_adaptation_enabled_flag = b.read_flag("persistent_rice_adaptation_enabled_flag");
  r.cabac_bypass_alignment_enabled_flag = b.read_flag("cabac_bypass_alignment_enabled_flag");
}

void read_scc_extension(BitReader& br, SyntaxBuilder& b, const Sps& sps, SpsSccExtension& e) {
  auto s = b.scoped("sps_scc_extension", "H.265 7.3.2.2.3");
  e.sps_curr_pic_ref_enabled_flag = b.read_flag("sps_curr_pic_ref_enabled_flag");
  e.palette_mode_enabled_flag = b.read_flag("palette_mode_enabled_flag");
  if (e.palette_mode_enabled_flag) {
    e.palette_max_size = b.read_ue("palette_max_size");
    detail::require_range(br, "palette_max_size", e.palette_max_size, 0, 64);  // sizes the palette tables
    e.delta_palette_max_predictor_size = b.read_ue("delta_palette_max_predictor_size");
    detail::require_range(br, "delta_palette_max_predictor_size", e.delta_palette_max_predictor_size, 0, 128);
    e.PaletteMaxPredictorSize = e.palette_max_size + e.delta_palette_max_predictor_size;
    b.derived("PaletteMaxPredictorSize", e.PaletteMaxPredictorSize);
    e.sps_palette_predictor_initializers_present_flag = b.read_flag("sps_palette_predictor_initializers_present_flag");
    if (e.sps_palette_predictor_initializers_present_flag) {
      e.sps_num_palette_predictor_initializers_minus1 = b.read_ue("sps_num_palette_predictor_initializers_minus1");
      detail::require_range(br, "sps_num_palette_predictor_initializers_minus1",
                            e.sps_num_palette_predictor_initializers_minus1, 0,
                            e.PaletteMaxPredictorSize == 0 ? 0 : int64_t{e.PaletteMaxPredictorSize} - 1);
      const unsigned num_comps = sps.chroma_format_idc == 0 ? 1U : 3U;
      for (unsigned comp = 0; comp < num_comps; ++comp) {
        const unsigned bits = comp == 0 ? sps.BitDepthY : sps.BitDepthC;
        b.begin_array_element("comp", static_cast<int>(comp));
        auto& list = e.sps_palette_predictor_initializer[comp];
        list.resize(static_cast<size_t>(e.sps_num_palette_predictor_initializers_minus1) + 1);
        for (uint32_t i = 0; i <= e.sps_num_palette_predictor_initializers_minus1; ++i) {
          list[i] = static_cast<uint32_t>(
              detail::read_u_at(b, "sps_palette_predictor_initializer", static_cast<int>(i), bits, "u(v)"));
        }
        b.end_array_element();
      }
    }
  }
  e.motion_vector_resolution_control_idc = static_cast<uint8_t>(b.read_u("motion_vector_resolution_control_idc", 2));
  e.intra_boundary_filtering_disabled_flag = b.read_flag("intra_boundary_filtering_disabled_flag");
}

// Returns false when the rest of the RBSP was consumed as opaque bytes.
bool read_extensions(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  sps.sps_range_extension_flag = b.read_flag("sps_range_extension_flag");
  sps.sps_multilayer_extension_flag = b.read_flag("sps_multilayer_extension_flag");
  sps.sps_3d_extension_flag = b.read_flag("sps_3d_extension_flag");
  sps.sps_scc_extension_flag = b.read_flag("sps_scc_extension_flag");
  sps.sps_extension_4bits = static_cast<uint8_t>(b.read_u("sps_extension_4bits", 4));
  if (sps.sps_range_extension_flag) {
    read_range_extension(b, sps.range_extension);
  }
  if (sps.sps_multilayer_extension_flag) {
    auto s = b.scoped("sps_multilayer_extension", "H.265 F.7.3.2.2.4");
    sps.inter_view_mv_vert_constraint_flag = b.read_flag("inter_view_mv_vert_constraint_flag");
  }
  if (sps.sps_3d_extension_flag) {
    opaque_tail(b, sps, "sps_3d_extension", "sps_3d_extension() (I.7.3.2.2.5)");
    return false;
  }
  if (sps.sps_scc_extension_flag) {
    read_scc_extension(br, b, sps, sps.scc_extension);
  }
  if (sps.sps_extension_4bits != 0) {
    opaque_tail(b, sps, "sps_extension_data_flag", "sps_extension_4bits data");
    return false;
  }
  return true;
}

// §7.4.3.2.1 derived variables that only depend on the picture format; needed before the SCC extension (bit
// depths size the palette initializers) and emitted after the syntax elements.
void derive_format(Sps& sps) {
  sps.ChromaArrayType = sps.separate_colour_plane_flag ? 0 : sps.chroma_format_idc;
  sps.SubWidthC = (sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2) ? 2 : 1;
  sps.SubHeightC = sps.chroma_format_idc == 1 ? 2 : 1;
  sps.BitDepthY = 8U + sps.bit_depth_luma_minus8;
  sps.BitDepthC = 8U + sps.bit_depth_chroma_minus8;
  sps.QpBdOffsetY = 6U * sps.bit_depth_luma_minus8;
  sps.QpBdOffsetC = 6U * sps.bit_depth_chroma_minus8;
  sps.MaxPicOrderCntLsb = 1U << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4U);
}

void derive_sizes(BitReader& br, SyntaxBuilder& b, Sps& sps) {
  sps.MinCbLog2SizeY = sps.log2_min_luma_coding_block_size_minus3 + 3;                   // in [3, 6] (hard bound above)
  sps.CtbLog2SizeY = sps.MinCbLog2SizeY + sps.log2_diff_max_min_luma_coding_block_size;  // in [3, 9]
  detail::require_range(br, "CtbLog2SizeY", sps.CtbLog2SizeY, 4, 6);  // 7.4.3.2.1; sizes the Ctb tables
  sps.MinCbSizeY = 1U << sps.MinCbLog2SizeY;
  sps.CtbSizeY = 1U << sps.CtbLog2SizeY;
  sps.PicWidthInMinCbsY = sps.pic_width_in_luma_samples / sps.MinCbSizeY;
  sps.PicWidthInCtbsY = (sps.pic_width_in_luma_samples + sps.CtbSizeY - 1) / sps.CtbSizeY;
  sps.PicHeightInMinCbsY = sps.pic_height_in_luma_samples / sps.MinCbSizeY;
  sps.PicHeightInCtbsY = (sps.pic_height_in_luma_samples + sps.CtbSizeY - 1) / sps.CtbSizeY;
  sps.PicSizeInMinCbsY = sps.PicWidthInMinCbsY * sps.PicHeightInMinCbsY;
  sps.PicSizeInCtbsY = sps.PicWidthInCtbsY * sps.PicHeightInCtbsY;
  sps.PicSizeInSamplesY = uint64_t{sps.pic_width_in_luma_samples} * sps.pic_height_in_luma_samples;
  sps.PicWidthInSamplesC = sps.pic_width_in_luma_samples / sps.SubWidthC;
  sps.PicHeightInSamplesC = sps.pic_height_in_luma_samples / sps.SubHeightC;
  if (sps.pic_width_in_luma_samples % sps.MinCbSizeY != 0 || sps.pic_height_in_luma_samples % sps.MinCbSizeY != 0) {
    b.diag(Severity::Error, "SYN-H265-0007",
           fmt::format("pic_width/height_in_luma_samples out of range ({}x{} not a multiple of MinCbSizeY = {})",
                       sps.pic_width_in_luma_samples, sps.pic_height_in_luma_samples, sps.MinCbSizeY),
           "H.265 7.4.3.2.1");
  }
  sps.MinTbLog2SizeY = sps.log2_min_luma_transform_block_size_minus2 + 2;
  sps.MaxTbLog2SizeY = sps.MinTbLog2SizeY + sps.log2_diff_max_min_luma_transform_block_size;
  detail::check_range(b, "MaxTbLog2SizeY", sps.MaxTbLog2SizeY, 2, std::min<int64_t>(5, sps.CtbLog2SizeY),
                      "H.265 7.4.3.2.1");
  if (sps.pcm_enabled_flag) {
    sps.PcmBitDepthY = sps.pcm_sample_bit_depth_luma_minus1 + 1U;
    sps.PcmBitDepthC = sps.pcm_sample_bit_depth_chroma_minus1 + 1U;
    sps.Log2MinIpcmCbSizeY = sps.log2_min_pcm_luma_coding_block_size_minus3 + 3;
    sps.Log2MaxIpcmCbSizeY = sps.Log2MinIpcmCbSizeY + sps.log2_diff_max_min_pcm_luma_coding_block_size;
  }
}

void derive_precision(Sps& sps) {
  const bool ext = sps.range_extension.extended_precision_processing_flag;
  const unsigned log2_y = ext ? std::max(15U, sps.BitDepthY + 6U) : 15U;
  const unsigned log2_c = ext ? std::max(15U, sps.BitDepthC + 6U) : 15U;
  sps.CoeffMinY = -(1 << log2_y);
  sps.CoeffMinC = -(1 << log2_c);
  sps.CoeffMaxY = (1 << log2_y) - 1;
  sps.CoeffMaxC = (1 << log2_c) - 1;
  const bool hp = sps.range_extension.high_precision_offsets_enabled_flag;
  sps.WpOffsetBdShiftY = hp ? 0U : sps.BitDepthY - 8U;
  sps.WpOffsetBdShiftC = hp ? 0U : sps.BitDepthC - 8U;
  sps.WpOffsetHalfRangeY = 1U << (hp ? sps.BitDepthY - 1U : 7U);
  sps.WpOffsetHalfRangeC = 1U << (hp ? sps.BitDepthC - 1U : 7U);
}

void derive_crop(SyntaxBuilder& b, Sps& sps) {
  const uint64_t crop_w = uint64_t{sps.SubWidthC} * (uint64_t{sps.conf_win_left_offset} + sps.conf_win_right_offset);
  const uint64_t crop_h = uint64_t{sps.SubHeightC} * (uint64_t{sps.conf_win_top_offset} + sps.conf_win_bottom_offset);
  if (crop_w >= sps.pic_width_in_luma_samples || crop_h >= sps.pic_height_in_luma_samples) {
    b.diag(Severity::Error, "SYN-H265-0007",
           fmt::format("conf_win offsets out of range (crop {}x{} not inside {}x{})", crop_w, crop_h,
                       sps.pic_width_in_luma_samples, sps.pic_height_in_luma_samples),
           "H.265 7.4.3.2.1");
    sps.conformance_crop = {0, 0, sps.pic_width_in_luma_samples, sps.pic_height_in_luma_samples};
    return;
  }
  sps.conformance_crop.x = sps.SubWidthC * sps.conf_win_left_offset;
  sps.conformance_crop.y = sps.SubHeightC * sps.conf_win_top_offset;
  sps.conformance_crop.w = sps.pic_width_in_luma_samples - static_cast<uint32_t>(crop_w);
  sps.conformance_crop.h = sps.pic_height_in_luma_samples - static_cast<uint32_t>(crop_h);
}

void emit_derived(SyntaxBuilder& b, const Sps& sps) {
  b.derived("ChromaArrayType", sps.ChromaArrayType);
  b.derived("SubWidthC", sps.SubWidthC);
  b.derived("SubHeightC", sps.SubHeightC);
  b.derived("MinCbLog2SizeY", sps.MinCbLog2SizeY);
  b.derived("CtbLog2SizeY", sps.CtbLog2SizeY);
  b.derived("MinCbSizeY", sps.MinCbSizeY);
  b.derived("CtbSizeY", sps.CtbSizeY);
  b.derived("PicWidthInMinCbsY", sps.PicWidthInMinCbsY);
  b.derived("PicWidthInCtbsY", sps.PicWidthInCtbsY);
  b.derived("PicHeightInMinCbsY", sps.PicHeightInMinCbsY);
  b.derived("PicHeightInCtbsY", sps.PicHeightInCtbsY);
  b.derived("PicSizeInMinCbsY", sps.PicSizeInMinCbsY);
  b.derived("PicSizeInCtbsY", sps.PicSizeInCtbsY);
  b.derived("PicSizeInSamplesY", sps.PicSizeInSamplesY);
  b.derived("PicWidthInSamplesC", sps.PicWidthInSamplesC);
  b.derived("PicHeightInSamplesC", sps.PicHeightInSamplesC);
  b.derived("MinTbLog2SizeY", sps.MinTbLog2SizeY);
  b.derived("MaxTbLog2SizeY", sps.MaxTbLog2SizeY);
  b.derived("BitDepthY", sps.BitDepthY);
  b.derived("BitDepthC", sps.BitDepthC);
  b.derived("QpBdOffsetY", sps.QpBdOffsetY);
  b.derived("QpBdOffsetC", sps.QpBdOffsetC);
  b.derived("MaxPicOrderCntLsb", sps.MaxPicOrderCntLsb);
  if (sps.pcm_enabled_flag) {
    b.derived("PcmBitDepthY", sps.PcmBitDepthY);
    b.derived("PcmBitDepthC", sps.PcmBitDepthC);
    b.derived("Log2MinIpcmCbSizeY", sps.Log2MinIpcmCbSizeY);
    b.derived("Log2MaxIpcmCbSizeY", sps.Log2MaxIpcmCbSizeY);
  }
  b.derived("CoeffMinY", sps.CoeffMinY);
  b.derived("CoeffMaxY", sps.CoeffMaxY);
  b.derived("CoeffMinC", sps.CoeffMinC);
  b.derived("CoeffMaxC", sps.CoeffMaxC);
  b.derived("WpOffsetBdShiftY", sps.WpOffsetBdShiftY);
  b.derived("WpOffsetBdShiftC", sps.WpOffsetBdShiftC);
  b.derived("WpOffsetHalfRangeY", sps.WpOffsetHalfRangeY);
  b.derived("WpOffsetHalfRangeC", sps.WpOffsetHalfRangeC);
  b.derived("conformance_crop",
            std::string_view{fmt::format("conformance_crop = {}x{}+{}+{}", sps.conformance_crop.w,
                                         sps.conformance_crop.h, sps.conformance_crop.x, sps.conformance_crop.y)});
  b.derived("sps_max_dec_pic_buffering_minus1[HighestTid]", sps.MaxDpbSizeMinus1);
}

}  // namespace

Sps parse_sps(BitReader& br, SyntaxBuilder& b, uint8_t nuh_layer_id) {
  Sps sps;
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("seq_parameter_set_rbsp", "H.265 7.3.2.2.1");
  sps.sps_video_parameter_set_id = static_cast<uint8_t>(b.read_u("sps_video_parameter_set_id", 4));
  if (nuh_layer_id > 0) {
    // F.7.3.2.2.1: sps_ext_or_max_sub_layers_minus1 and the multilayer SPS layout; phase 1 keeps it opaque.
    opaque_tail(b, sps, "seq_parameter_set_rbsp_multilayer", fmt::format("SPS with nuh_layer_id {}", nuh_layer_id));
    sps.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
    return sps;
  }
  sps.sps_max_sub_layers_minus1 = static_cast<uint8_t>(b.read_u("sps_max_sub_layers_minus1", 3));
  detail::require_range(br, "sps_max_sub_layers_minus1", sps.sps_max_sub_layers_minus1, 0, 6);
  sps.sps_temporal_id_nesting_flag = b.read_flag("sps_temporal_id_nesting_flag");
  if (sps.sps_max_sub_layers_minus1 == 0 && !sps.sps_temporal_id_nesting_flag) {
    b.diag(Severity::Error, "SYN-H265-0007",
           "sps_temporal_id_nesting_flag out of range (sps_temporal_id_nesting_flag = 0, allowed 1 when "
           "sps_max_sub_layers_minus1 == 0)",
           "H.265 7.4.3.2.1", true);
  }
  sps.profile_tier_level = parse_profile_tier_level(br, b, true, sps.sps_max_sub_layers_minus1);
  const uint32_t sps_id = b.read_ue("sps_seq_parameter_set_id");
  detail::require_range(br, "sps_seq_parameter_set_id", sps_id, 0, 15);
  sps.sps_seq_parameter_set_id = static_cast<uint8_t>(sps_id);
  read_picture_format(br, b, sps);
  derive_format(sps);
  sps.sps_sub_layer_ordering_info_present_flag = b.read_flag("sps_sub_layer_ordering_info_present_flag");
  read_sub_layer_ordering_info(br, b, sps);
  sps.MaxDpbSizeMinus1 = sps.sub_layer_ordering_info[sps.sps_max_sub_layers_minus1].max_dec_pic_buffering_minus1;
  read_block_sizes(br, b, sps);
  read_scaling_and_tools(br, b, sps);
  read_reference_sets(br, b, sps);
  sps.vui_parameters_present_flag = b.read_flag("vui_parameters_present_flag");
  if (sps.vui_parameters_present_flag) {
    sps.vui = parse_vui_parameters(br, b, sps.sps_max_sub_layers_minus1, sps.ChromaArrayType);
    sps.vui->color.chroma_format = sps.chroma_format_idc;
    sps.vui->color.bit_depth = static_cast<uint8_t>(sps.BitDepthY);
  }
  sps.sps_extension_present_flag = b.read_flag("sps_extension_present_flag");
  bool complete = true;
  if (sps.sps_extension_present_flag) {
    complete = read_extensions(br, b, sps);
  }
  derive_sizes(br, b, sps);
  derive_precision(sps);
  derive_crop(b, sps);
  emit_derived(b, sps);
  if (complete) {
    detail::read_rbsp_trailing_bits(br, b);
  }
  sps.span = BitSpan{start, static_cast<uint32_t>(br.bit_pos() - start)};
  return sps;
}

}  // namespace ovt::syntax::h265
