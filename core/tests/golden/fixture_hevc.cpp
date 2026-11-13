// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// tiny16_hevc.fa: the chapter 06 §6.10 worked example, every field (WI-0.093 step 1).
//
// SPEC-ASSUMPTION(#164): where §6.10's prose and its JSON disagree, the JSON wins (edge_flags_v 10, qp_hist
// bins 158/160/161); QpC'Cb/Cr follow Table 8-10 (QpY 32 -> 31) instead of the JSON's 32; data_bit_offset is
// derived from the full slice header written here (32) instead of the abridged example's 72.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "fixtures_common.hpp"

namespace ovt::golden {
namespace {

// One 8x8 intra CU of the example: its parameters differ only in these numbers.
struct CuParams {
  int x;
  int y;
  std::uint32_t cu_id;   // CU id; PU = ids + 1, TU = ids + 2
  std::uint8_t mode;     // IntraPredModeY (= IntraPredModeC, chroma mode 4)
  std::uint8_t mpm_idx;  // position of `mode` in candModeList
  std::array<std::uint8_t, 3> cand;
  std::int8_t qp_pred;
  std::int8_t qp;
  std::int8_t qpc;  // Table 8-10 for ChromaArrayType 1
  std::int16_t delta;
  std::uint32_t cu_bits;
  std::uint32_t tu_bits;
  std::uint16_t nonzero;
  std::uint32_t abs_sum;
  std::uint8_t last_x;
  std::uint8_t last_y;
};

constexpr std::uint32_t kPuBits = 24;

// Block ids of the CTU's children follow the pre-order rule: CU, PU, TU per CU.
constexpr std::array<CuParams, 4> kCus{{
    {0, 0, 2, 1, 1, {0, 1, 26}, 32, 32, 31, 0, 320, 272, 5, 37, 2, 1},
    {8, 0, 5, 26, 2, {0, 1, 26}, 32, 32, 31, 0, 288, 240, 4, 29, 3, 0},
    {0, 8, 8, 0, 0, {0, 1, 26}, 32, 30, 29, -2, 256, 216, 7, 52, 1, 2},
    {8, 8, 11, 26, 1, {0, 26, 1}, 31, 33, 32, 2, 176, 144, 3, 18, 1, 1},
}};

std::vector<std::uint32_t> id_list(std::initializer_list<std::uint32_t> list) {
  return list;
}

void add_cu_blocks(fb::FrameAnalysisT& fa, const CuParams& cu) {
  const Box box{cu.x, cu.y, 8, 8};
  const auto qp = static_cast<std::int16_t>(cu.qp);

  fb::HevcCuT cu_ext;
  cu_ext.CtDepth = 1;
  cu_ext.log2CbSize = 3;
  cu_ext.CuPredMode = fb::HevcPredMode::MODE_INTRA;
  cu_ext.part_mode = fb::HevcPartMode::PART_2Nx2N;
  cu_ext.pcm_flag = 0;
  cu_ext.cu_qp_delta_abs = static_cast<std::uint8_t>(cu.delta < 0 ? -cu.delta : cu.delta);
  cu_ext.cu_qp_delta_sign_flag = cu.delta == 0 ? 255 : (cu.delta < 0 ? 1 : 0);
  cu_ext.CuQpDeltaVal = cu.delta;
  cu_ext.IsCuQpDeltaCoded = 1;
  cu_ext.qPY_PRED = cu.qp_pred;
  cu_ext.QpY = cu.qp;
  cu_ext.QpY_prime = cu.qp;
  cu_ext.QpC_cb_prime = cu.qpc;
  cu_ext.QpC_cr_prime = cu.qpc;
  cu_ext.quant_group_x = static_cast<std::uint16_t>(cu.x);
  cu_ext.quant_group_y = static_cast<std::uint16_t>(cu.y);
  cu_ext.intra_chroma_pred_mode = {4};
  cu_ext.IntraPredModeC = {cu.mode};
  cu_ext.bit_count_x8 = cu.cu_bits;

  fb::HevcPuT pu_ext;
  pu_ext.partIdx = 0;
  pu_ext.prev_intra_luma_pred_flag = 1;
  pu_ext.mpm_idx = cu.mpm_idx;
  pu_ext.IntraPredModeY = cu.mode;
  pu_ext.candModeList = {cu.cand.begin(), cu.cand.end()};

  fb::HevcTuT tu_ext;
  tu_ext.trafoDepth = 0;
  tu_ext.log2TrafoSize = 3;
  tu_ext.blkIdx = 0;
  tu_ext.split_transform_flag = 0;
  tu_ext.interSplitFlag = 0;
  tu_ext.cbf_luma = 1;
  tu_ext.cbf_cb = {0};
  tu_ext.cbf_cr = {0};
  tu_ext.transform_skip_flag = {0, 255, 255};
  tu_ext.last_sig_coeff_x = {cu.last_x, 255, 255};
  tu_ext.last_sig_coeff_y = {cu.last_y, 255, 255};
  tu_ext.num_nonzero = {cu.nonzero, 0, 0};
  tu_ext.coeff_abs_sum = {cu.abs_sum, 0, 0};
  tu_ext.sign_data_hiding_used = {1, 0, 0};
  tu_ext.dst_used = 0;
  tu_ext.scaling_list_used = 0;
  tu_ext.bit_count_x8 = cu.tu_bits;

  const auto common = [&](BlockSpec spec) {
    spec.parent = static_cast<std::int32_t>(spec.kind == fb::BlockKind::CU ? 1U : cu.cu_id);
    spec.box = box;
    spec.level = spec.kind == fb::BlockKind::CU ? 2 : 3;
    spec.pred = fb::PredKind::Intra;
    spec.qp = qp;
    return make_block(std::move(spec));
  };
  fa.blocks.push_back(common({.id = cu.cu_id,
                              .children = id_list({cu.cu_id + 1, cu.cu_id + 2}),
                              .kind = fb::BlockKind::CU,
                              .partition_type = 0,
                              .bits_x8 = cu.cu_bits,
                              .ext_type = "HevcCu",
                              .ext = nested<fb::HevcCu>(cu_ext)}));
  fa.blocks.push_back(common({.id = cu.cu_id + 1,
                              .kind = fb::BlockKind::PU,
                              .partition_type = 0,
                              .bits_x8 = kPuBits,
                              .ext_type = "HevcPu",
                              .ext = nested<fb::HevcPu>(pu_ext)}));
  fa.blocks.push_back(common({.id = cu.cu_id + 2,
                              .kind = fb::BlockKind::TU,
                              .partition_type = 0,
                              .bits_x8 = cu.tu_bits,
                              .ext_type = "HevcTu",
                              .ext = nested<fb::HevcTu>(tu_ext)}));
}

// Slice segment header with real, mutually consistent bit positions (bit 0 = forbidden_zero_bit of the NAL
// header, so the first element starts at bit 16; §6.2.7). I slice, SAO off, deblocking on.
std::unique_ptr<fb::SyntaxNodeT> slice_header() {
  constexpr std::string_view kRef = "H.265 7.3.6.1";
  auto header = std::make_unique<fb::SyntaxNodeT>();
  header->name = "slice_segment_header";
  header->span = std::make_unique<fb::BitSpan>(16U, 16U);
  using K = fb::SyntaxValueKind;
  const std::vector<NodeSpec> nodes{
      {"first_slice_segment_in_pic_flag", "u(1)", K::UInt, 1, "", 16, 1, std::string(kRef)},
      {"no_output_of_prior_pics_flag", "u(1)", K::UInt, 0, "", 17, 1, std::string(kRef)},
      {"slice_pic_parameter_set_id", "ue(v)", K::UInt, 0, "", 18, 1, std::string(kRef)},
      {"slice_type", "ue(v)", K::Enum, 2, "I", 19, 3, std::string(kRef)},
      {"slice_qp_delta", "se(v)", K::Int, 6, "", 22, 7, std::string(kRef)},
      {"slice_loop_filter_across_slices_enabled_flag", "u(1)", K::UInt, 1, "", 29, 1, std::string(kRef)},
      {"alignment_bit_equal_to_one", "f(1)", K::UInt, 1, "", 30, 1, std::string(kRef)},
      {"alignment_bit_equal_to_zero", "f(1)", K::UInt, 0, "", 31, 1, std::string(kRef)},
  };
  for (const auto& node : nodes) {
    header->children.push_back(make_node(node));
  }
  return header;
}

}  // namespace

Bytes build_tiny16_hevc() {
  fb::FrameAnalysisT fa;
  fa.schema_version = 1;
  fa.plugin_id = "ovt-plugin-de265";
  fa.plugin_version = "1.0.0+de265-1.0.15";

  // ---- frame ------------------------------------------------------------------------------------------
  fb::HevcRpsT rps;
  rps.PicOrderCntVal = 0;
  rps.NoRaslOutputFlag = 1;
  rps.pic_output_flag = 1;
  rps.dpb_fullness_after = 1;
  rps.sps_max_dec_pic_buffering_minus1 = 4;
  rps.sps_max_num_reorder_pics = 2;

  auto info = std::make_unique<fb::FrameInfoT>();
  info->id = frame_id(0, 0);
  info->type = fb::FrameType::I;
  info->shown = true;
  info->structure = fb::PictureStructure::Frame;
  info->is_rap = true;
  info->timebase_num = 1;
  info->timebase_den = 25;
  info->size_bytes = 143;
  info->units = {3};
  info->coded_size = rect({0, 0, 16, 16});
  info->crop = rect({0, 0, 16, 16});
  info->chroma_format = 1;
  info->bit_depth_luma = 8;
  info->bit_depth_chroma = 8;
  info->qp_base = 32;
  info->dpb_after = {0};
  info->ref_state = nested<fb::HevcRps>(rps);
  info->ref_state_type = "HevcRps";
  info->slice_count = 1;
  fa.frame = std::move(info);

  // ---- grid: 4x4 cells, one 8x8 CU per quadrant --------------------------------------------------------
  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = 4;
  grid->cells_h = 4;
  const GridPainter painter{4, 4};
  for (const auto& cu : kCus) {
    const Box box{cu.x, cu.y, 8, 8};
    painter.paint<std::uint32_t>(grid->block_id, box, cu.cu_id);
    painter.paint<std::uint16_t>(grid->partition_id, box, 0);
    painter.paint<std::uint8_t>(grid->pred, box, static_cast<std::uint8_t>(fb::PredKind::Intra));
    painter.paint<std::int16_t>(grid->qp, box, cu.qp);
    painter.paint<std::int16_t>(grid->intra_mode_y, box, cu.mode);
    painter.paint<std::int16_t>(grid->intra_mode_c, box, cu.mode);
    painter.paint<std::uint8_t>(grid->tx_log2_w, box, 3);
    painter.paint<std::uint8_t>(grid->tx_log2_h, box, 3);
    painter.paint<std::uint8_t>(grid->tx_type, box, 0);
    painter.paint<std::uint8_t>(grid->cbf, box, 1);
    painter.distribute_bits(grid->bits_x8, box, cu.cu_bits);
    painter.paint<std::uint8_t>(grid->filter_idx, box, 0);
  }
  grid->ref0.assign(16, -1);
  grid->ref1.assign(16, -1);
  grid->deblock_bs_v = {0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0};
  grid->deblock_bs_h = {0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0};
  fa.grid = std::move(grid);

  // ---- block tree: root, CTU, then {CU, PU, TU} per CU -------------------------------------------------
  fa.blocks.push_back(make_block({.id = 0,
                                  .children = id_list({1}),
                                  .kind = fb::BlockKind::Other,
                                  .box = {0, 0, 16, 16},
                                  .level = 0,
                                  .bits_x8 = 1048}));
  fb::HevcCtuT ctu;
  ctu.CtbAddrInRs = 0;
  ctu.CtbAddrInTs = 0;
  ctu.slice_segment_idx = 0;
  ctu.end_of_slice_segment_flag = 1;
  ctu.bit_count_x8 = 1048;
  fa.blocks.push_back(make_block({.id = 1,
                                  .parent = 0,
                                  .children = id_list({2, 5, 8, 11}),
                                  .kind = fb::BlockKind::CTU,
                                  .box = {0, 0, 16, 16},
                                  .level = 1,
                                  .qp = 32,
                                  .bits_x8 = 1048,
                                  .ext_type = "HevcCtu",
                                  .ext = nested<fb::HevcCtu>(ctu)}));
  for (const auto& cu : kCus) {
    add_cu_blocks(fa, cu);
  }

  // ---- partitions, filter params, planes, histogram -----------------------------------------------------
  auto slice = std::make_unique<fb::PartitionInfoT>();
  slice->kind = fb::PartitionKind::Slice;
  slice->index = 0;
  slice->unit_index = 3;
  slice->file_span = std::make_unique<fb::ByteSpan>(97U, 143U);
  slice->data_bit_offset = 32;
  slice->bit_count = 1072;
  slice->header = slice_header();
  slice->slice_type = 2;
  slice->rect = rect({0, 0, 16, 16});
  slice->first_block_addr = 0;
  slice->block_count = 1;
  slice->qp = 32;
  slice->qp_delta = 6;
  slice->deblocking_disable = 0;
  slice->loop_filter_across_slices = 1;
  slice->sao_luma = 0;
  slice->sao_chroma = 0;
  slice->max_num_merge_cand = 5;
  fa.partitions.push_back(std::move(slice));

  fb::HevcFilterParamsT filter;
  filter.pps_deblocking_filter_disabled_flag = 0;
  filter.slice_deblocking_filter_disabled_flag = {0};
  filter.slice_beta_offset_div2 = {0};
  filter.slice_tc_offset_div2 = {0};
  filter.pps_loop_filter_across_slices_enabled_flag = 1;
  filter.loop_filter_across_tiles_enabled_flag = 1;
  filter.pcm_loop_filter_disabled_flag = 0;
  filter.sample_adaptive_offset_enabled_flag = 0;
  filter.slice_sao_luma_flag = {0};
  filter.slice_sao_chroma_flag = {0};
  filter.edge_flags_v = {10};
  filter.edge_flags_h = {12};
  fa.filter_params = nested<fb::HevcFilterParams>(filter);
  fa.filter_params_type = "HevcFilterParams";

  fa.planes = recon_planes(0, 16, 16);
  fa.qp_hist = qp_histogram(fa.grid->qp, 128);
  return finish(fa);
}

}  // namespace ovt::golden

namespace ovt::golden {

// 16x16 4:2:0 8-bit gradient: Y rises to the right and downwards, U rises to the right, V falls downwards.
Bytes build_tiny16_yuv() {
  Bytes yuv;
  yuv.reserve(16U * 16U * 3U / 2U);
  for (unsigned y = 0; y < 16; ++y) {
    for (unsigned x = 0; x < 16; ++x) {
      yuv.push_back(static_cast<std::uint8_t>(16U + (x * 8U) + (y * 4U)));
    }
  }
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) {
      yuv.push_back(static_cast<std::uint8_t>(96U + (x * 8U)));
    }
  }
  for (unsigned y = 0; y < 8; ++y) {
    for (unsigned x = 0; x < 8; ++x) {
      yuv.push_back(static_cast<std::uint8_t>(160U - (y * 8U)));
    }
  }
  return yuv;
}

}  // namespace ovt::golden
