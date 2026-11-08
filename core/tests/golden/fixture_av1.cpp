// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// tiny64_av1.fa: 64x64 inter frame, one 64x64 superblock with PARTITION_SPLIT into four 32x32 blocks
// (WI-0.093 step 3): DC_PRED intra (CfL chroma), NEWMV Mv (-5, 7) 1/8-pel -> neutral (-10, 14) as (row, col),
// GLOBALMV (is_global = 1), NEAREST_NEARESTMV compound RefFrame [LAST, ALTREF]; cdef_idx 2; one Av1LrUnit per
// plane.
//
// SPEC-ASSUMPTION(#164): the spec writes Mv pairs as (row, col) in Av1Block.Mv order, so "Mv (-5, 7)" is
// row -5, col 7 and the neutral pair is mv_y = -10, mv_x = 14.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "fixtures_common.hpp"

namespace ovt::golden {
namespace {

constexpr std::int16_t kNoMvd = -32768;

enum : std::int8_t { kLast = 1, kAltref = 7 };

// One 32x32 block of the superblock; ids follow the pre-order rule (block, then its TXB).
struct BlockParams {
  Box box;
  std::uint32_t id;
  bool inter;
  bool skip;
  fb::Av1YMode y_mode;
  std::array<std::int8_t, 2> ref_frame;
  std::array<std::int16_t, 4> mv;   // Mv[0].row, Mv[0].col, Mv[1].row, Mv[1].col in 1/8 luma samples
  std::array<std::int16_t, 4> mvd;  // same order; kNoMvd where the block has no NEWMV component
  std::uint32_t bits;
};

constexpr std::array<BlockParams, 4> kBlocks{{
    {{0, 0, 32, 32},
     2,
     false,
     false,
     fb::Av1YMode::DC_PRED,
     {0, -1},
     {0, 0, 0, 0},
     {kNoMvd, kNoMvd, kNoMvd, kNoMvd},
     1600},
    {{32, 0, 32, 32}, 4, true, false, fb::Av1YMode::NEWMV, {kLast, -1}, {-5, 7, 0, 0}, {-1, 1, kNoMvd, kNoMvd}, 1200},
    {{0, 32, 32, 32},
     6,
     true,
     true,
     fb::Av1YMode::GLOBALMV,
     {kLast, -1},
     {1, -2, 0, 0},
     {kNoMvd, kNoMvd, kNoMvd, kNoMvd},
     160},
    {{32, 32, 32, 32},
     8,
     true,
     false,
     fb::Av1YMode::NEAREST_NEARESTMV,
     {kLast, kAltref},
     {2, 2, -3, 1},
     {kNoMvd, kNoMvd, kNoMvd, kNoMvd},
     1000},
}};

// Block bits above the sum of the leaves: the SB's own syntax (partition symbols), the root's headers.
constexpr std::uint32_t kSbBits = 4000;
constexpr std::uint32_t kRootBits = 4096;

std::vector<std::uint32_t> id_list(std::initializer_list<std::uint32_t> list) {
  return list;
}

std::string predictor_name(const BlockParams& b) {
  switch (b.y_mode) {
    case fb::Av1YMode::NEWMV:
      return "NEWMV[0]";
    case fb::Av1YMode::GLOBALMV:
      return "GLOBALMV[0]+global";
    default:
      return "NEAREST_NEARESTMV[0]";
  }
}

Bytes block_ext(const BlockParams& b, bool is_superblock) {
  fb::Av1BlockT ext;
  ext.MiRow = static_cast<std::uint32_t>(b.box.y / 4);
  ext.MiCol = static_cast<std::uint32_t>(b.box.x / 4);
  ext.tile_idx = 1;
  ext.CurrentQIndex = 120;
  ext.segment_id = 0;
  ext.Lossless = 0;
  ext.cdef_idx = 2;
  if (is_superblock) {
    ext.bsize = fb::Av1BlockSize::BLOCK_64X64;
    ext.partition = fb::Av1Partition::PARTITION_SPLIT;
    ext.is_superblock = 1;
    ext.bit_count_x8 = kSbBits;
    return nested<fb::Av1Block>(ext);
  }
  ext.bsize = fb::Av1BlockSize::BLOCK_32X32;
  ext.partition = fb::Av1Partition::PARTITION_NONE;
  ext.skip_mode = 0;
  ext.skip = b.skip ? 1 : 0;
  ext.seg_id_predicted = 0;
  ext.is_inter = b.inter ? 1 : 0;
  ext.use_intrabc = 0;
  ext.YMode = b.y_mode;
  ext.RefFrame = {b.ref_frame.begin(), b.ref_frame.end()};
  ext.TxSize = fb::Av1TxSize::TX_32X32;
  ext.tx_depth = 0;
  ext.TxType = fb::Av1TxType::DCT_DCT;
  ext.bit_count_x8 = b.bits;
  if (!b.inter) {
    ext.UVMode = 13;  // UV_CFL_PRED
    ext.CflAlphaU = 3;
    ext.CflAlphaV = -2;
    ext.cfl_alpha_signs = 6;  // CFL_SIGN_POS for U, CFL_SIGN_NEG for V: (2 * 3 + 1) - 1
    ext.use_filter_intra = 0;
    return nested<fb::Av1Block>(ext);
  }
  ext.comp_mode = b.ref_frame[1] > 0 ? 1 : 0;
  if (b.ref_frame[1] > 0) {
    ext.comp_ref_type = fb::Av1CompRefType::BIDIR_COMP_REFERENCE;
    ext.compound_type = fb::Av1CompoundType::COMPOUND_AVERAGE;
    ext.comp_group_idx = 0;
    ext.compound_idx = 1;
  }
  ext.drl_mode_idx = 0;
  ext.NumMvFound = 2;
  ext.Mv = {b.mv.begin(), b.mv.end()};
  ext.Mvd = {b.mvd.begin(), b.mvd.end()};
  if (b.y_mode == fb::Av1YMode::NEWMV) {
    ext.mv_joint = {3};  // MV_JOINT_HNZVNZ
  }
  ext.interintra = 0;
  ext.motion_mode = fb::Av1MotionMode::SIMPLE;
  ext.interp_filter = {fb::Av1InterpFilter::EIGHTTAP, fb::Av1InterpFilter::EIGHTTAP};
  ext.is_global = b.y_mode == fb::Av1YMode::GLOBALMV ? 1 : 0;
  ext.RefOrderHint = {b.ref_frame[0] == kLast ? 2 : 0, b.ref_frame[1] == kAltref ? 0 : -1};
  return nested<fb::Av1Block>(ext);
}

Bytes txb_ext(const BlockParams& b) {
  fb::Av1TxbT ext;
  ext.plane = 0;
  ext.startX = static_cast<std::uint32_t>(b.box.x);
  ext.startY = static_cast<std::uint32_t>(b.box.y);
  ext.txSz = fb::Av1TxSize::TX_32X32;
  ext.TxType = fb::Av1TxType::DCT_DCT;
  ext.all_zero = b.skip ? 1 : 0;
  if (!b.skip) {
    ext.eob = 12;
    ext.dc_sign = 1;
    ext.dc_level = -96;
    ext.coeff_abs_sum = 180;
    ext.num_nonzero = 11;
    ext.bit_count_x8 = b.bits / 4;
  }
  return nested<fb::Av1Txb>(ext);
}

fb::Av1RefStateT ref_state() {
  fb::Av1RefStateT state;
  state.frame_type = fb::Av1FrameType::INTER_FRAME;
  state.show_frame = 1;
  state.showable_frame = 0;
  state.show_existing_frame = 0;
  state.error_resilient_mode = 0;
  state.disable_cdf_update = 0;
  state.primary_ref_frame = 0;
  state.refresh_frame_flags = 0x08;
  state.OrderHint = 3;
  state.ref_frame_idx = {0, 1, 2, 3, 4, 5, 6};
  state.OrderHints = {-1, 2, 1, 0, 0, 0, 0, 0};
  state.RefFrameSignBias = {0, 0, 0, 0, 0, 0, 0, 0};
  state.use_ref_frame_mvs = 1;
  state.allow_high_precision_mv = 1;
  state.force_integer_mv = 0;
  // Slots 0..2 hold the previous three frames, slots 3..7 still the key frame, except slot 3 which this frame
  // refreshes (refresh_frame_flags = 0x08).
  constexpr std::array<std::int32_t, 8> kFrame{2, 1, 0, 3, 0, 0, 0, 0};
  for (std::uint8_t slot = 0; slot < 8; ++slot) {
    auto s = std::make_unique<fb::Av1RefSlotT>();
    s->slot = slot;
    s->RefValid = 1;
    s->RefFrameType = kFrame[slot] == 0 ? fb::Av1FrameType::KEY_FRAME : fb::Av1FrameType::INTER_FRAME;
    s->RefOrderHint = kFrame[slot];
    s->frame = frame_id(kFrame[slot], kFrame[slot]);
    s->RefUpscaledWidth = 64;
    s->RefFrameHeight = 64;
    s->RefRenderWidth = 64;
    s->RefRenderHeight = 64;
    s->RefMiCols = 16;
    s->RefMiRows = 16;
    s->RefBitDepth = 8;
    s->RefSubsamplingX = 1;
    s->RefSubsamplingY = 1;
    state.slots_after.push_back(std::move(s));
  }
  return state;
}

fb::Av1FilterParamsT filter_params() {
  fb::Av1FilterParamsT f;
  f.loop_filter_level = {10, 10, 8, 8};
  f.loop_filter_delta_enabled = 1;
  f.loop_filter_ref_deltas = {1, 0, 0, 0, -1, 0, -1, -1};
  f.loop_filter_mode_deltas = {0, 0};
  f.enable_cdef = 1;
  f.cdef = std::make_unique<fb::Av1CdefParamsT>();
  f.cdef->cdef_damping_minus_3 = 2;
  f.cdef->cdef_bits = 2;
  f.cdef->CdefDamping = 5;
  f.cdef->cdef_y_pri_strength = {0, 2, 5, 9};
  f.cdef->cdef_y_sec_strength = {0, 1, 3, 2};  // index 2: raw 3, exported as 4 (5.9.19)
  f.cdef->cdef_uv_pri_strength = {0, 1, 3, 6};
  f.cdef->cdef_uv_sec_strength = {0, 1, 2, 3};
  f.cdef->cdef_idx = {2};
  f.enable_restoration = 1;
  f.FrameRestorationType = {fb::Av1RestorationType::RESTORE_SWITCHABLE, fb::Av1RestorationType::RESTORE_SGRPROJ,
                            fb::Av1RestorationType::RESTORE_NONE};
  f.LoopRestorationSize = {64, 64, 64};
  for (std::uint8_t plane = 0; plane < 3; ++plane) {
    auto unit = std::make_unique<fb::Av1LrUnitT>();
    unit->plane = plane;
    unit->rect = rect({0, 0, 64, 64});
    if (plane == 0) {
      unit->restoration_type = fb::Av1RestorationType::RESTORE_WIENER;
      unit->LrWiener = {3, -7, 15, 2, -4, 9};
    } else if (plane == 1) {
      unit->restoration_type = fb::Av1RestorationType::RESTORE_SGRPROJ;
      unit->LrSgrSet = 7;
      unit->LrSgrXqd = {-32, 31};
    } else {
      unit->restoration_type = fb::Av1RestorationType::RESTORE_NONE;
    }
    f.lr_units.push_back(std::move(unit));
  }
  f.FrameWidth = 64;
  f.UpscaledWidth = 64;
  f.base_q_idx = 120;
  f.delta_q_present = 0;
  f.delta_q_res = 0;
  f.segmentation_enabled = 0;
  f.TxMode = 2;
  f.reference_select = 1;
  f.skip_mode_present = 0;
  f.allow_warped_motion = 1;
  f.reduced_tx_set = 0;
  f.interpolation_filter = fb::Av1InterpFilter::SWITCHABLE;
  for (std::int8_t ref = 1; ref <= 7; ++ref) {
    auto gm = std::make_unique<fb::Av1GmParamsT>();
    gm->ref = static_cast<fb::Av1RefFrame>(ref);
    if (ref == kLast) {
      gm->GmType = fb::Av1GmType::ROTZOOM;  // constraint of 7.11.3.6: params[4] = -params[3], params[5] = params[2]
      gm->gm_params = {1024, -512, 65792, 128, -128, 65792};
    } else {
      gm->GmType = fb::Av1GmType::IDENTITY;
      gm->gm_params = {0, 0, 65536, 0, 0, 65536};
    }
    f.gm.push_back(std::move(gm));
  }
  return f;
}

std::unique_ptr<fb::SyntaxNodeT> tile_group_header() {
  // Bit 0 = obu_forbidden_bit; obu_header (8 bits) + obu_size (leb128, 2 bytes) precede the payload (§6.2.7).
  auto header = std::make_unique<fb::SyntaxNodeT>();
  header->name = "tile_group_obu";
  header->span = std::make_unique<fb::BitSpan>(24U, 1U);
  header->children.push_back(
      make_node({"tile_start_and_end_present_flag", "f(1)", fb::SyntaxValueKind::UInt, 0, "", 24, 1, "AV1 5.11.1"}));
  return header;
}

}  // namespace

Bytes build_tiny64_av1() {
  fb::FrameAnalysisT fa;
  fa.schema_version = 1;
  fa.plugin_id = "ovt-plugin-aom";
  fa.plugin_version = "1.0.0+aom-3.12.1";

  auto info = std::make_unique<fb::FrameInfoT>();
  info->id = frame_id(3, 3);
  info->type = fb::FrameType::Inter;
  info->shown = true;
  info->structure = fb::PictureStructure::Frame;
  info->poc = 3;
  info->pts = 3;
  info->dts = 3;
  info->timebase_num = 1;
  info->timebase_den = 30;
  info->size_bytes = 212;
  info->units = {6};
  info->coded_size = rect({0, 0, 64, 64});
  info->crop = rect({0, 0, 64, 64});
  info->chroma_format = 1;
  info->bit_depth_luma = 8;
  info->bit_depth_chroma = 8;
  info->qp_base = 120;
  info->declared_refs = {2, 1, 0};
  info->dpb_after = {0, 1, 2, 3};
  info->ref_state = nested<fb::Av1RefState>(ref_state());
  info->ref_state_type = "Av1RefState";
  info->slice_count = 1;
  fa.frame = std::move(info);

  // ---- grid: 16x16 cells (the MI grid) -------------------------------------------------------------------------
  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = 16;
  grid->cells_h = 16;
  const GridPainter painter{16, 16};
  grid->ref0.assign(256, -1);
  grid->ref1.assign(256, -1);
  grid->mv0_x.assign(256, 0);
  grid->mv0_y.assign(256, 0);
  grid->mv1_x.assign(256, 0);
  grid->mv1_y.assign(256, 0);
  for (const auto& b : kBlocks) {
    painter.paint<std::uint32_t>(grid->block_id, b.box, b.id);
    painter.paint<std::uint16_t>(grid->partition_id, b.box, 1);  // the Tile (partitions[1])
    painter.paint<std::uint8_t>(grid->pred, b.box,
                                static_cast<std::uint8_t>(b.inter ? fb::PredKind::Inter : fb::PredKind::Intra));
    painter.paint<std::int16_t>(grid->qp, b.box, 120);
    painter.paint<std::int16_t>(grid->intra_mode_y, b.box, b.inter ? -1 : 0);
    painter.paint<std::int16_t>(grid->intra_mode_c, b.box, b.inter ? -1 : 13);
    painter.paint<std::uint8_t>(grid->tx_log2_w, b.box, 5);
    painter.paint<std::uint8_t>(grid->tx_log2_h, b.box, 5);
    painter.paint<std::uint8_t>(grid->tx_type, b.box, 0);
    painter.paint<std::uint8_t>(grid->cbf, b.box, b.skip ? 0 : 1);
    painter.distribute_bits(grid->bits_x8, b.box, b.bits);
    painter.paint<std::uint8_t>(grid->filter_idx, b.box, 2);  // cdef_idx
    // AV1 stores the filter size of the edge (4, 6, 8, 16), not a strength (§6.3.1).
    painter.paint<std::uint8_t>(grid->deblock_bs_v, b.box, 0);
    painter.paint<std::uint8_t>(grid->deblock_bs_h, b.box, 0);
    if (b.inter) {
      painter.paint<std::int8_t>(grid->ref0, b.box, b.ref_frame[0]);
      painter.paint<std::int16_t>(grid->mv0_x, b.box, static_cast<std::int16_t>(b.mv[1] * 2));  // §6.2.3 x2
      painter.paint<std::int16_t>(grid->mv0_y, b.box, static_cast<std::int16_t>(b.mv[0] * 2));
      if (b.ref_frame[1] > 0) {
        painter.paint<std::int8_t>(grid->ref1, b.box, b.ref_frame[1]);
        painter.paint<std::int16_t>(grid->mv1_x, b.box, static_cast<std::int16_t>(b.mv[3] * 2));
        painter.paint<std::int16_t>(grid->mv1_y, b.box, static_cast<std::int16_t>(b.mv[2] * 2));
      }
    }
  }
  // Transform-block edges: the 32x32 transform grid and the superblock-internal 32-sample block edges.
  for (std::size_t y = 0; y < 16; ++y) {
    for (std::size_t x = 0; x < 16; ++x) {
      grid->deblock_bs_v[(y * 16) + x] = (x == 8) ? 16 : 0;
      grid->deblock_bs_h[(y * 16) + x] = (y == 8) ? 16 : 0;
    }
  }

  // ---- block tree: root, SB, then {Block, TXB} per 32x32 block ------------------------------------------------
  fa.blocks.push_back(make_block(
      {.id = 0, .children = id_list({1}), .kind = fb::BlockKind::Other, .box = {0, 0, 64, 64}, .bits_x8 = kRootBits}));
  fa.blocks.push_back(make_block({.id = 1,
                                  .parent = 0,
                                  .children = id_list({2, 4, 6, 8}),
                                  .kind = fb::BlockKind::SB,
                                  .box = {0, 0, 64, 64},
                                  .level = 1,
                                  .partition_type = static_cast<std::uint8_t>(fb::Av1Partition::PARTITION_SPLIT),
                                  .qp = 120,
                                  .bits_x8 = kSbBits,
                                  .ext_type = "Av1Block",
                                  .ext = block_ext(kBlocks[0], true)}));
  for (const auto& b : kBlocks) {
    const auto pred = b.inter ? fb::PredKind::Inter : fb::PredKind::Intra;
    const bool nearest = b.y_mode == fb::Av1YMode::GLOBALMV || b.y_mode == fb::Av1YMode::NEAREST_NEARESTMV;
    fa.blocks.push_back(make_block({.id = b.id,
                                    .parent = 1,
                                    .children = id_list({b.id + 1}),
                                    .kind = fb::BlockKind::Block,
                                    .box = b.box,
                                    .level = 2,
                                    .partition_type = static_cast<std::uint8_t>(fb::Av1Partition::PARTITION_NONE),
                                    .pred = pred,
                                    .skip = b.skip,
                                    .merge_or_direct = nearest,
                                    .qp = 120,
                                    .bits_x8 = b.bits,
                                    .ext_type = "Av1Block",
                                    .ext = block_ext(b, false)}));
    fa.blocks.push_back(make_block({.id = b.id + 1,
                                    .parent = static_cast<std::int32_t>(b.id),
                                    .kind = fb::BlockKind::TXB,
                                    .box = b.box,
                                    .level = 3,
                                    .partition_type = 0,
                                    .pred = pred,
                                    .skip = b.skip,
                                    .qp = 120,
                                    .bits_x8 = b.skip ? 0U : b.bits / 4,
                                    .ext_type = "Av1Txb",
                                    .ext = txb_ext(b)}));
  }

  // ---- motion vectors: sorted by (block, list) ---------------------------------------------------------------
  for (const auto& b : kBlocks) {
    if (!b.inter) {
      continue;
    }
    const std::uint16_t global_flag = b.y_mode == fb::Av1YMode::GLOBALMV ? 0x4U : 0U;
    const std::uint8_t lists = b.ref_frame[1] > 0 ? 2U : 1U;
    for (std::uint8_t list = 0; list < lists; ++list) {
      const std::size_t k = static_cast<std::size_t>(list) * 2;
      fa.mvs.push_back(make_mv({.block_id = b.id,
                                .list = list,
                                .ref = b.ref_frame[list],
                                .box = b.box,
                                .mv_x = static_cast<std::int16_t>(b.mv[k + 1] * 2),
                                .mv_y = static_cast<std::int16_t>(b.mv[k] * 2),
                                .mvd_x = b.mvd[k + 1] == kNoMvd ? kNoMvd : static_cast<std::int16_t>(b.mvd[k + 1] * 2),
                                .mvd_y = b.mvd[k] == kNoMvd ? kNoMvd : static_cast<std::int16_t>(b.mvd[k] * 2),
                                .predictor = predictor_name(b),
                                .flags = static_cast<std::uint16_t>(global_flag | (b.ref_frame[1] > 0 ? 0x1U : 0U))}));
    }
  }

  // ---- refs: INTRA placeholder, then LAST..ALTREF with their DPB slots (§6.3.1) -------------------------------
  constexpr std::array<const char*, 8> kNames{"INTRA",  "LAST",   "LAST2",   "LAST3",
                                              "GOLDEN", "BWDREF", "ALTREF2", "ALTREF"};
  constexpr std::array<std::int32_t, 8> kRefFrame{-1, 2, 1, 0, 0, 0, 0, 0};
  for (std::uint8_t t = 0; t < 8; ++t) {
    auto slot = std::make_unique<fb::RefSlotT>();
    slot->index = t;
    slot->frame = frame_id(kRefFrame[t], kRefFrame[t]);
    slot->name = kNames[t];
    slot->usage_cells = t == 0 ? 0U : usage_cells(*grid, static_cast<std::int8_t>(t));
    slot->slot = static_cast<std::int8_t>(t == 0 ? -1 : t - 1);
    fa.refs.push_back(std::move(slot));
  }

  // ---- partitions: tile group 0, its single tile 1 -----------------------------------------------------------------
  auto group = std::make_unique<fb::PartitionInfoT>();
  group->kind = fb::PartitionKind::TileGroup;
  group->index = 0;
  group->unit_index = 6;
  group->file_span = std::make_unique<fb::ByteSpan>(300U, 208U);
  group->bit_count = 1536;
  group->header = tile_group_header();
  group->rect = rect({0, 0, 64, 64});
  group->block_count = 256;
  group->num_entry_points = 1;
  fa.partitions.push_back(std::move(group));
  auto tile = std::make_unique<fb::PartitionInfoT>();
  tile->kind = fb::PartitionKind::Tile;
  tile->index = 1;
  tile->parent = 0;
  tile->unit_index = 6;
  tile->file_span = std::make_unique<fb::ByteSpan>(303U, 205U);
  tile->bit_count = 1520;
  tile->rect = rect({0, 0, 64, 64});
  tile->block_count = 256;
  tile->tile_row = 0;
  tile->tile_col = 0;
  fa.partitions.push_back(std::move(tile));

  fa.filter_params = nested<fb::Av1FilterParams>(filter_params());
  fa.filter_params_type = "Av1FilterParams";
  fa.planes = recon_planes(3, 64, 64);
  fa.qp_hist = qp_histogram(grid->qp, 0);
  fa.grid = std::move(grid);
  return finish(fa);
}

}  // namespace ovt::golden
