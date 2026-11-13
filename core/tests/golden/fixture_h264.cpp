// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// tiny32_h264.fa: 32x32 P frame, four macroblocks (WI-0.093 step 2):
//   MB0 I_16x16_2_0_0 (DC)   MB1 P_L0_16x16, mv (3, -1) quarter-pel -> neutral (12, -4)
//   MB2 P_8x8, sub_mb_type P_L0_8x8 / P_L0_8x4 / P_L0_4x8 / P_L0_4x4   MB3 P_Skip
// CABAC (estimates in bit_position_*, left at 0 = unknown), two RefSlots L0[0] / L0[1].

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "fixtures_common.hpp"

namespace ovt::golden {
namespace {

constexpr std::uint32_t kCells = 8;  // 32 / 4
constexpr std::int16_t kNoMvd = -32768;

// One prediction block of the frame: units are quarter luma samples as H.264 signals them (§6.2.3 x4 for the
// neutral grid). mvp is taken as (0, 0), so mvd == mv.
struct Part {
  Box box;
  std::uint32_t block;
  std::int8_t ref;
  std::int16_t qx;
  std::int16_t qy;
  const char* predictor;
};

constexpr std::array<Part, 11> kParts{{
    {{16, 0, 16, 16}, 2, 0, 3, -1, "mvp"},
    {{0, 16, 8, 8}, 4, 0, 4, 0, "mvp"},
    {{8, 16, 8, 4}, 5, 1, -2, 1, "mvp"},
    {{8, 20, 8, 4}, 5, 1, -2, 2, "mvp"},
    {{0, 24, 4, 8}, 6, 0, 1, 3, "mvp"},
    {{4, 24, 4, 8}, 6, 0, 1, 4, "mvp"},
    {{8, 24, 4, 4}, 7, 1, 0, 1, "mvp"},
    {{12, 24, 4, 4}, 7, 1, 0, 2, "mvp"},
    {{8, 28, 4, 4}, 7, 1, 0, 3, "mvp"},
    {{12, 28, 4, 4}, 7, 1, 0, 4, "mvp"},
    {{16, 16, 16, 16}, 8, 0, 0, 0, "skip"},
}};

// luma4x4BlkIdx -> (x4, y4) inside a macroblock (§6.4.3 z-order).
constexpr std::array<std::array<int, 2>, 16> kZOrder{{{0, 0},
                                                      {1, 0},
                                                      {0, 1},
                                                      {1, 1},
                                                      {2, 0},
                                                      {3, 0},
                                                      {2, 1},
                                                      {3, 1},
                                                      {0, 2},
                                                      {1, 2},
                                                      {0, 3},
                                                      {1, 3},
                                                      {2, 2},
                                                      {3, 2},
                                                      {2, 3},
                                                      {3, 3}}};

std::vector<std::uint32_t> id_list(std::initializer_list<std::uint32_t> list) {
  return list;
}

// Per-cell facts the deblocking strengths are derived from.
struct CellFacts {
  std::vector<std::uint8_t> intra = std::vector<std::uint8_t>(64, 0);
  std::vector<std::uint8_t> coded = std::vector<std::uint8_t>(64, 0);
  std::vector<std::int8_t> ref = std::vector<std::int8_t>(64, -1);
  std::vector<std::int16_t> qx = std::vector<std::int16_t>(64, 0);
  std::vector<std::int16_t> qy = std::vector<std::int16_t>(64, 0);
};

std::uint8_t boundary_strength(const CellFacts& f, std::size_t p, std::size_t q, bool mb_edge) {
  if (f.intra[p] != 0 || f.intra[q] != 0) {
    return mb_edge ? 4 : 3;  // §8.7.2.1
  }
  if (f.coded[p] != 0 || f.coded[q] != 0) {
    return 2;
  }
  const bool far = std::abs(f.qx[p] - f.qx[q]) >= 4 || std::abs(f.qy[p] - f.qy[q]) >= 4;
  return (f.ref[p] != f.ref[q] || far) ? 1 : 0;
}

// Left (vertical == true) or top edge strength of every cell. An edge is filtered on a macroblock boundary or
// on the transform grid of the cell's own macroblock; the picture edge is never filtered.
std::vector<std::uint8_t> deblock(const CellFacts& f, const fb::CellGridT& grid, bool vertical) {
  std::vector<std::uint8_t> bs(64, 0);
  for (std::uint32_t y = 0; y < kCells; ++y) {
    for (std::uint32_t x = 0; x < kCells; ++x) {
      const std::uint32_t pos = vertical ? x : y;
      if (pos == 0) {
        continue;
      }
      const std::size_t q = (y * kCells) + x;
      const std::size_t p = vertical ? q - 1 : q - kCells;
      const bool mb_edge = pos % 4 == 0;
      const std::uint8_t log2_tx = vertical ? grid.tx_log2_w[q] : grid.tx_log2_h[q];
      const bool on_tx_grid = log2_tx != 255 && (pos * 4) % (1U << log2_tx) == 0;
      if (mb_edge || on_tx_grid) {
        bs[q] = boundary_strength(f, p, q, mb_edge);
      }
    }
  }
  return bs;
}

struct MbData {
  Box box;
  std::uint32_t id;
  std::int8_t qp;
  std::int8_t qpc;
  std::uint32_t bits;
  std::uint8_t mb_type_raw;
  fb::AvcMbType type;
  std::uint8_t cbp;
};

constexpr std::array<MbData, 4> kMbs{{
    {{0, 0, 16, 16}, 1, 28, 28, 800, 8, fb::AvcMbType::I_16x16_2_0_0, 0},
    {{16, 0, 16, 16}, 2, 28, 28, 640, 0, fb::AvcMbType::P_L0_16x16, 31},
    {{0, 16, 16, 16}, 3, 30, 29, 928, 3, fb::AvcMbType::P_8x8, 11},
    {{16, 16, 16, 16}, 8, 30, 29, 8, 255, fb::AvcMbType::P_Skip, 255},
}};

// The 16 per-4x4 entries (luma4x4BlkIdx order) of a macroblock, taken from a per-cell vector.
template <class T>
std::vector<T> by_blk(const std::vector<T>& cells, Box mb) {
  std::vector<T> out;
  out.reserve(kZOrder.size());
  for (const auto& xy : kZOrder) {
    const std::size_t x = std::size_t{static_cast<unsigned>(mb.x / 4)} + static_cast<unsigned>(xy[0]);
    const std::size_t y = std::size_t{static_cast<unsigned>(mb.y / 4)} + static_cast<unsigned>(xy[1]);
    out.push_back(cells[(y * kCells) + x]);
  }
  return out;
}

Bytes mb_ext(const MbData& mb, const std::vector<std::uint8_t>& bs_v, const std::vector<std::uint8_t>& bs_h,
             const CellFacts& facts) {
  fb::AvcMbT ext;
  ext.mb_addr = static_cast<std::uint32_t>(((mb.box.y / 16) * 2) + (mb.box.x / 16));
  ext.mb_type_raw = mb.mb_type_raw;
  ext.mb_type = mb.type;
  const bool skipped = mb.type == fb::AvcMbType::P_Skip;
  const bool intra = mb.type == fb::AvcMbType::I_16x16_2_0_0;
  ext.mb_skip_flag = skipped ? 1 : 0;
  ext.coded_block_pattern = mb.cbp;
  ext.mb_qp_delta = static_cast<std::int8_t>(mb.qp - 28);
  ext.QPY = mb.qp;
  ext.QPC_cb = mb.qpc;
  ext.QPC_cr = mb.qpc;
  ext.bs_vertical = by_blk(bs_v, mb.box);
  ext.bs_horizontal = by_blk(bs_h, mb.box);
  ext.filterLeftMbEdgeFlag = mb.box.x > 0 ? 1 : 0;
  ext.filterTopMbEdgeFlag = mb.box.y > 0 ? 1 : 0;
  if (intra) {
    ext.Intra16x16PredMode = fb::AvcIntra16x16PredMode::I16_DC;
    ext.intra_chroma_pred_mode = fb::AvcIntraChromaPredMode::Chroma_DC;
    ext.i16_dc_nonzero = 1;
    return nested<fb::AvcMb>(ext);
  }
  if (mb.type == fb::AvcMbType::P_8x8) {
    ext.sub_mb_type = {fb::AvcSubMbType::P_L0_8x8, fb::AvcSubMbType::P_L0_8x4, fb::AvcSubMbType::P_L0_4x8,
                       fb::AvcSubMbType::P_L0_4x4};
    ext.ref_idx_l0 = {0, 1, 0, 1};
    ext.transform_size_8x8_flag = 0;
  } else {
    ext.ref_idx_l0 = {0, 0, 0, 0};
    ext.transform_size_8x8_flag = skipped ? 255 : 1;
  }
  ext.ref_idx_l1 = {-1, -1, -1, -1};
  ext.mvL0_x = by_blk(facts.qx, mb.box);
  ext.mvL0_y = by_blk(facts.qy, mb.box);
  ext.mvd_l0_x = skipped ? std::vector<std::int16_t>(16, kNoMvd) : ext.mvL0_x;
  ext.mvd_l0_y = skipped ? std::vector<std::int16_t>(16, kNoMvd) : ext.mvL0_y;
  for (auto* v : {&ext.mvd_l1_x, &ext.mvd_l1_y, &ext.mvL1_x, &ext.mvL1_y}) {
    v->assign(16, kNoMvd);
  }
  ext.pred_flag.assign(16, 1);
  ext.direct_8x8 = {0, 0, 0, 0};
  return nested<fb::AvcMb>(ext);
}

std::unique_ptr<fb::SyntaxNodeT> slice_header() {
  // Bit 0 = forbidden_zero_bit of the 1-byte NAL header (§6.2.7). P slice, CABAC, frame_num 2, QP 28.
  constexpr std::string_view kRef = "H.264 7.3.3";
  using K = fb::SyntaxValueKind;
  auto header = std::make_unique<fb::SyntaxNodeT>();
  header->name = "slice_header";
  header->span = std::make_unique<fb::BitSpan>(8U, 32U);
  const std::vector<NodeSpec> nodes{
      {"first_mb_in_slice", "ue(v)", K::UInt, 0, "", 8, 1, std::string(kRef)},
      {"slice_type", "ue(v)", K::Enum, 5, "P", 9, 5, std::string(kRef)},
      {"pic_parameter_set_id", "ue(v)", K::UInt, 0, "", 14, 1, std::string(kRef)},
      {"frame_num", "u(4)", K::UInt, 2, "", 15, 4, std::string(kRef)},
      {"num_ref_idx_active_override_flag", "u(1)", K::UInt, 1, "", 19, 1, std::string(kRef)},
      {"num_ref_idx_l0_active_minus1", "ue(v)", K::UInt, 1, "", 20, 3, std::string(kRef)},
      {"ref_pic_list_modification_flag_l0", "u(1)", K::UInt, 0, "", 23, 1, std::string(kRef)},
      {"adaptive_ref_pic_marking_mode_flag", "u(1)", K::UInt, 0, "", 24, 1, std::string(kRef)},
      {"cabac_init_idc", "ue(v)", K::UInt, 0, "", 25, 1, std::string(kRef)},
      {"slice_qp_delta", "se(v)", K::Int, 2, "", 26, 5, std::string(kRef)},
      {"disable_deblocking_filter_idc", "ue(v)", K::UInt, 0, "", 31, 1, std::string(kRef)},
      {"slice_alpha_c0_offset_div2", "se(v)", K::Int, 0, "", 32, 1, std::string(kRef)},
      {"slice_beta_offset_div2", "se(v)", K::Int, 0, "", 33, 1, std::string(kRef)},
  };
  for (const auto& node : nodes) {
    header->children.push_back(make_node(node));
  }
  return header;
}

fb::AvcRefMarkingT ref_marking() {
  fb::AvcRefMarkingT marking;
  marking.frame_num = 2;
  marking.adaptive_ref_pic_marking_mode_flag = 0;
  marking.RefPicList0 = {1, 0};
  marking.max_num_ref_frames = 3;
  for (std::int32_t k = 0; k < 3; ++k) {
    auto pic = std::make_unique<fb::AvcRefPicT>();
    pic->frame = std::make_unique<fb::FrameId>(k, k);
    pic->FrameNum = k;
    pic->FrameNumWrap = k;
    pic->PicNum = k;
    pic->is_reference = true;
    pic->structure = fb::PictureStructure::Frame;
    pic->TopFieldOrderCnt = 2 * k;
    pic->BottomFieldOrderCnt = 2 * k;
    marking.dpb_after.push_back(std::move(pic));
  }
  return marking;
}

}  // namespace

Bytes build_tiny32_h264() {
  fb::FrameAnalysisT fa;
  fa.schema_version = 1;
  fa.plugin_id = "ovt-plugin-ffmpeg-h264";
  fa.plugin_version = "1.0.0+ffmpeg-7.1";

  auto info = std::make_unique<fb::FrameInfoT>();
  info->id = frame_id(2, 2);
  info->type = fb::FrameType::P;
  info->shown = true;
  info->structure = fb::PictureStructure::Frame;
  info->poc = 4;
  info->pts = 2;
  info->dts = 2;
  info->timebase_num = 1;
  info->timebase_den = 25;
  info->size_bytes = 104;
  info->units = {4};
  info->coded_size = rect({0, 0, 32, 32});
  info->crop = rect({0, 0, 32, 32});
  info->chroma_format = 1;
  info->bit_depth_luma = 8;
  info->bit_depth_chroma = 8;
  info->qp_base = 28;
  info->declared_refs = {1, 0};
  info->dpb_after = {0, 1, 2};
  info->ref_state = nested<fb::AvcRefMarking>(ref_marking());
  info->ref_state_type = "AvcRefMarking";
  info->frame_num = 2;
  info->slice_count = 1;
  fa.frame = std::move(info);

  // ---- grid (8x8 cells) ----------------------------------------------------------------------------------
  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = kCells;
  grid->cells_h = kCells;
  const GridPainter painter{kCells, kCells};
  CellFacts facts;

  // leaf blocks: MB0, MB1, the four sub-macroblocks of MB2, MB3
  struct Leaf {
    Box box;
    std::uint32_t id;
    fb::PredKind pred;
    std::int16_t qp;
    std::uint32_t bits;
    std::uint8_t tx;  // log2 transform size, 255 = none
    std::uint8_t tx_type;
    std::uint8_t cbf;
  };
  const std::array<Leaf, 7> leaves{{
      {{0, 0, 16, 16}, 1, fb::PredKind::Intra, 28, 800, 2, 2, 0},
      {{16, 0, 16, 16}, 2, fb::PredKind::Inter, 28, 640, 3, 1, 7},
      {{0, 16, 8, 8}, 4, fb::PredKind::Inter, 30, 240, 2, 0, 1},
      {{8, 16, 8, 8}, 5, fb::PredKind::Inter, 30, 256, 2, 0, 1},
      {{0, 24, 8, 8}, 6, fb::PredKind::Inter, 30, 112, 2, 0, 0},
      {{8, 24, 8, 8}, 7, fb::PredKind::Inter, 30, 288, 2, 0, 1},
      {{16, 16, 16, 16}, 8, fb::PredKind::Skip, 30, 8, 255, 255, 0},
  }};
  for (const auto& leaf : leaves) {
    const bool intra = leaf.pred == fb::PredKind::Intra;
    painter.paint<std::uint32_t>(grid->block_id, leaf.box, leaf.id);
    painter.paint<std::uint16_t>(grid->partition_id, leaf.box, 0);
    painter.paint<std::uint8_t>(grid->pred, leaf.box, static_cast<std::uint8_t>(leaf.pred));
    painter.paint<std::int16_t>(grid->qp, leaf.box, leaf.qp);
    painter.paint<std::int16_t>(grid->intra_mode_y, leaf.box, intra ? 18 : -1);  // 16 + Intra16x16PredMode DC
    painter.paint<std::int16_t>(grid->intra_mode_c, leaf.box, intra ? 0 : -1);
    painter.paint<std::uint8_t>(grid->tx_log2_w, leaf.box, leaf.tx);
    painter.paint<std::uint8_t>(grid->tx_log2_h, leaf.box, leaf.tx);
    painter.paint<std::uint8_t>(grid->tx_type, leaf.box, leaf.tx_type);
    painter.paint<std::uint8_t>(grid->cbf, leaf.box, leaf.cbf);
    painter.distribute_bits(grid->bits_x8, leaf.box, leaf.bits);
    painter.paint<std::uint8_t>(grid->filter_idx, leaf.box, 0);
    painter.paint<std::uint8_t>(facts.intra, leaf.box, intra ? 1 : 0);
    painter.paint<std::uint8_t>(facts.coded, leaf.box, leaf.cbf & 1U);
  }
  // Cells that no prediction block covers (the intra macroblock) keep ref0 = -1 and a zero vector.
  grid->ref0.assign(painter.count(), -1);
  grid->mv0_x.assign(painter.count(), 0);
  grid->mv0_y.assign(painter.count(), 0);
  for (const auto& part : kParts) {
    const auto ref = part.ref;
    painter.paint<std::int8_t>(grid->ref0, part.box, ref);
    painter.paint<std::int8_t>(facts.ref, part.box, ref);
    painter.paint<std::int16_t>(facts.qx, part.box, part.qx);
    painter.paint<std::int16_t>(facts.qy, part.box, part.qy);
    painter.paint<std::int16_t>(grid->mv0_x, part.box, static_cast<std::int16_t>(part.qx * 4));  // §6.2.3 x4
    painter.paint<std::int16_t>(grid->mv0_y, part.box, static_cast<std::int16_t>(part.qy * 4));
  }
  grid->ref1.assign(painter.count(), -1);
  grid->mv1_x.assign(painter.count(), 0);
  grid->mv1_y.assign(painter.count(), 0);
  grid->deblock_bs_v = deblock(facts, *grid, true);
  grid->deblock_bs_h = deblock(facts, *grid, false);

  // ---- block tree: root, MB0, MB1, MB2 {4,5,6,7}, MB3 -----------------------------------------------------
  fa.blocks.push_back(make_block({.id = 0,
                                  .children = id_list({1, 2, 3, 8}),
                                  .kind = fb::BlockKind::Other,
                                  .box = {0, 0, 32, 32},
                                  .bits_x8 = 2400}));
  for (const auto& mb : kMbs) {
    const bool is8x8 = mb.type == fb::AvcMbType::P_8x8;
    const bool skipped = mb.type == fb::AvcMbType::P_Skip;
    fa.blocks.push_back(make_block({.id = mb.id,
                                    .parent = 0,
                                    .children = is8x8 ? id_list({4, 5, 6, 7}) : std::vector<std::uint32_t>{},
                                    .kind = fb::BlockKind::MB,
                                    .box = mb.box,
                                    .level = 1,
                                    .partition_type = static_cast<std::uint8_t>(mb.type),
                                    .pred = mb.type == fb::AvcMbType::I_16x16_2_0_0
                                                ? fb::PredKind::Intra
                                                : (skipped ? fb::PredKind::Skip : fb::PredKind::Inter),
                                    .skip = skipped,
                                    .merge_or_direct = skipped,
                                    .qp = mb.qp,
                                    .bits_x8 = mb.bits,
                                    .ext_type = "AvcMb",
                                    .ext = mb_ext(mb, grid->deblock_bs_v, grid->deblock_bs_h, facts)}));
    if (is8x8) {
      constexpr std::array<fb::AvcSubMbType, 4> kSubTypes{fb::AvcSubMbType::P_L0_8x8, fb::AvcSubMbType::P_L0_8x4,
                                                          fb::AvcSubMbType::P_L0_4x8, fb::AvcSubMbType::P_L0_4x4};
      constexpr std::array<std::array<std::uint8_t, 3>, 4> kShape{{{1, 8, 8}, {2, 8, 4}, {2, 4, 8}, {4, 4, 4}}};
      constexpr std::array<std::int8_t, 4> kRef{0, 1, 0, 1};
      for (std::uint8_t k = 0; k < 4; ++k) {
        const Leaf& leaf = leaves[2U + k];
        fb::AvcSubMbT sub;
        sub.mbPartIdx = k;
        sub.sub_mb_type = kSubTypes[k];
        sub.ref_idx_l0 = kRef[k];
        sub.NumSubMbPart = kShape[k][0];
        sub.SubMbPartWidth = kShape[k][1];
        sub.SubMbPartHeight = kShape[k][2];
        fa.blocks.push_back(make_block({.id = leaf.id,
                                        .parent = 3,
                                        .kind = fb::BlockKind::SubMB,
                                        .box = leaf.box,
                                        .level = 2,
                                        .partition_type = static_cast<std::uint8_t>(kSubTypes[k]),
                                        .pred = fb::PredKind::Inter,
                                        .qp = 30,
                                        .bits_x8 = leaf.bits,
                                        .ext_type = "AvcSubMb",
                                        .ext = nested<fb::AvcSubMb>(sub)}));
      }
    }
  }

  // ---- motion vectors (sorted by block, list, y, x; §6.3.7), refs ------------------------------------------
  for (const auto& part : kParts) {
    const bool skip = std::string_view(part.predictor) == "skip";
    fa.mvs.push_back(make_mv({.block_id = part.block,
                              .list = 0,
                              .ref = part.ref,
                              .box = part.box,
                              .mv_x = static_cast<std::int16_t>(part.qx * 4),
                              .mv_y = static_cast<std::int16_t>(part.qy * 4),
                              .mvd_x = skip ? kNoMvd : static_cast<std::int16_t>(part.qx * 4),
                              .mvd_y = skip ? kNoMvd : static_cast<std::int16_t>(part.qy * 4),
                              .predictor = part.predictor,
                              .flags = 0}));
  }
  for (std::uint8_t k = 0; k < 2; ++k) {
    auto slot = std::make_unique<fb::RefSlotT>();
    slot->index = k;
    slot->frame = frame_id(1 - k, 1 - k);
    slot->name = k == 0 ? "L0[0]" : "L0[1]";
    slot->usage_cells = usage_cells(*grid, static_cast<std::int8_t>(k));
    fa.refs.push_back(std::move(slot));
  }

  // ---- partitions, filter params, planes, histogram ---------------------------------------------------------
  auto slice = std::make_unique<fb::PartitionInfoT>();
  slice->kind = fb::PartitionKind::Slice;
  slice->unit_index = 4;
  slice->file_span = std::make_unique<fb::ByteSpan>(124U, 100U);
  slice->data_bit_offset = 40;  // header ends at bit 34; cabac_alignment_one_bit pads to the byte boundary
  slice->bit_count = 760;
  slice->header = slice_header();
  slice->slice_type = 5;
  slice->rect = rect({0, 0, 32, 32});
  slice->first_block_addr = 0;
  slice->block_count = 4;
  slice->qp = 28;
  slice->qp_delta = 2;
  slice->deblocking_disable = 0;
  slice->cabac_init = 0;
  slice->num_ref_idx_active = {2, 0};
  fa.partitions.push_back(std::move(slice));

  fb::AvcFilterParamsT filter;
  filter.deblocking_filter_control_present_flag = 1;
  filter.disable_deblocking_filter_idc = {0};
  filter.slice_alpha_c0_offset_div2 = {0};
  filter.slice_beta_offset_div2 = {0};
  filter.FilterOffsetA = {0};
  filter.FilterOffsetB = {0};
  fa.filter_params = nested<fb::AvcFilterParams>(filter);
  fa.filter_params_type = "AvcFilterParams";

  fa.planes = recon_planes(2, 32, 32);
  fa.qp_hist = qp_histogram(grid->qp, 128);
  fa.grid = std::move(grid);
  return finish(fa);
}

}  // namespace ovt::golden
