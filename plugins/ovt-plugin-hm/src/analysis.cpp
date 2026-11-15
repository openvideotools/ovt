// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Analysis -> FlatBuffers (chapter 06 §6.3, WI-4.105 part b).

#include "analysis.hpp"

namespace ovt::hm {

void Grid::resize(uint32_t w, uint32_t h) {
  w4 = w;
  h4 = h;
  const std::size_t n = std::size_t{w} * h;
  block_id.assign(n, 0);
  pred.assign(n, 0);
  qp.assign(n, 0);
  intra_y.assign(n, -1);
  intra_c.assign(n, -1);
  mv0_x.assign(n, 0);
  mv0_y.assign(n, 0);
  mv1_x.assign(n, 0);
  mv1_y.assign(n, 0);
  ref0.assign(n, -1);
  ref1.assign(n, -1);
  tx_log2.assign(n, 0xFF);
  tx_type.assign(n, 0xFF);
  cbf.assign(n, 0);
  filter_idx.assign(n, 0);
  partition_id.assign(n, 0);
}

namespace {

flatbuffers::Offset<fb::CellGrid> build_grid(flatbuffers::FlatBufferBuilder& fbb, const Grid& g) {
  const auto block_id = fbb.CreateVector(g.block_id);
  const auto pred = fbb.CreateVector(g.pred);
  const auto qp = fbb.CreateVector(g.qp);
  const auto intra_y = fbb.CreateVector(g.intra_y);
  const auto intra_c = fbb.CreateVector(g.intra_c);
  const auto mv0_x = fbb.CreateVector(g.mv0_x);
  const auto mv0_y = fbb.CreateVector(g.mv0_y);
  const auto mv1_x = fbb.CreateVector(g.mv1_x);
  const auto mv1_y = fbb.CreateVector(g.mv1_y);
  const auto ref0 = fbb.CreateVector(g.ref0);
  const auto ref1 = fbb.CreateVector(g.ref1);
  const auto tx_w = fbb.CreateVector(g.tx_log2);
  const auto tx_h = fbb.CreateVector(g.tx_log2);  // HEVC transforms are square
  const auto tx_type = fbb.CreateVector(g.tx_type);
  const auto cbf = fbb.CreateVector(g.cbf);
  const auto filter_idx = fbb.CreateVector(g.filter_idx);
  const auto partition_id = fbb.CreateVector(g.partition_id);
  fb::CellGridBuilder b(fbb);
  b.add_cells_w(g.w4);
  b.add_cells_h(g.h4);
  b.add_block_id(block_id);
  b.add_partition_id(partition_id);
  b.add_pred(pred);
  b.add_qp(qp);
  b.add_intra_mode_y(intra_y);
  b.add_intra_mode_c(intra_c);
  b.add_mv0_x(mv0_x);
  b.add_mv0_y(mv0_y);
  b.add_mv1_x(mv1_x);
  b.add_mv1_y(mv1_y);
  b.add_ref0(ref0);
  b.add_ref1(ref1);
  b.add_tx_log2_w(tx_w);
  b.add_tx_log2_h(tx_h);
  b.add_tx_type(tx_type);
  b.add_cbf(cbf);
  b.add_filter_idx(filter_idx);
  return b.Finish();
}

// A nested buffer keeps its own alignment inside the [ubyte] vector (chapter 06 §6.3.3).
flatbuffers::Offset<flatbuffers::Vector<uint8_t>> nested(flatbuffers::FlatBufferBuilder& fbb,
                                                         const std::vector<uint8_t>& ext) {
  if (ext.empty()) {
    return 0;
  }
  fbb.ForceVectorAlignment(ext.size(), 1, 8);
  return fbb.CreateVector(ext);
}

flatbuffers::Offset<fb::PartitionInfo> build_partition(flatbuffers::FlatBufferBuilder& fbb, const PartRec& r,
                                                       uint16_t index) {
  fb::PartitionInfoBuilder p(fbb);
  p.add_kind(r.kind);
  p.add_index(index);
  p.add_parent(r.parent);
  p.add_slice_type(r.slice_type);
  p.add_rect(&r.rect);
  p.add_first_block_addr(r.first_block_addr);
  p.add_block_count(r.block_count);
  p.add_qp(r.qp);
  p.add_qp_delta(r.qp_delta);
  p.add_deblocking_disable(r.deblocking_disable);
  p.add_deblock_alpha_or_beta_offset_div2(r.beta_offset_div2);
  p.add_deblock_beta_or_tc_offset_div2(r.tc_offset_div2);
  p.add_loop_filter_across_slices(r.loop_filter_across_slices);
  p.add_sao_luma(r.sao_luma);
  p.add_sao_chroma(r.sao_chroma);
  p.add_tile_row(r.tile_row);
  p.add_tile_col(r.tile_col);
  p.add_num_entry_points(r.num_entry_points);
  p.add_cabac_init(r.cabac_init);
  p.add_max_num_merge_cand(r.max_num_merge_cand);
  return p.Finish();
}

}  // namespace

MappedAnalysis build_analysis(flatbuffers::FlatBufferBuilder& fbb, const Analysis& a, int32_t coded_w,
                              int32_t coded_h) {
  MappedAnalysis out;
  if (!a.valid) {
    out.grid =
        fb::CreateCellGrid(fbb, static_cast<uint32_t>((coded_w + 3) / 4), static_cast<uint32_t>((coded_h + 3) / 4));
    return out;
  }
  std::vector<flatbuffers::Offset<fb::Block>> blocks;
  blocks.reserve(a.blocks.size());
  for (std::size_t id = 0; id < a.blocks.size(); ++id) {
    const BlockRec& b = a.blocks[id];
    const auto children = fbb.CreateVector(b.children);
    const auto ext_type = fbb.CreateString(b.ext_type);
    const auto ext = nested(fbb, b.ext);
    blocks.push_back(fb::CreateBlock(fbb, static_cast<uint32_t>(id), b.parent, children, b.kind, &b.rect, b.level,
                                     b.partition_type, b.pred, b.skip, b.merge, b.qp, 0, ext_type, ext));
  }
  out.blocks = fbb.CreateVector(blocks);
  std::vector<flatbuffers::Offset<fb::MotionVector>> mvs;
  mvs.reserve(a.mvs.size());
  for (const MvRec& m : a.mvs) {
    mvs.push_back(fb::CreateMotionVectorDirect(fbb, m.block_id, m.list, m.ref, &m.rect, m.mv_x, m.mv_y, m.mvd_x,
                                               m.mvd_y, m.predictor.c_str(), m.flags));
  }
  out.mvs = fbb.CreateVector(mvs);
  std::vector<flatbuffers::Offset<fb::RefSlot>> refs;
  refs.reserve(a.refs.size());
  for (std::size_t i = 0; i < a.refs.size(); ++i) {
    const RefRec& r = a.refs[i];
    const fb::FrameId frame(r.decode_index, -1);  // the host assigns display_index
    refs.push_back(
        fb::CreateRefSlotDirect(fbb, static_cast<uint8_t>(i), &frame, r.name.c_str(), r.long_term, r.usage_cells));
  }
  out.refs = fbb.CreateVector(refs);
  std::vector<flatbuffers::Offset<fb::PartitionInfo>> parts;
  parts.reserve(a.partitions.size());
  for (std::size_t i = 0; i < a.partitions.size(); ++i) {
    parts.push_back(build_partition(fbb, a.partitions[i], static_cast<uint16_t>(i)));
  }
  out.partitions = fbb.CreateVector(parts);
  if (!a.filter_params.empty()) {
    out.filter_params = nested(fbb, a.filter_params);
    out.filter_params_type = fbb.CreateString("HevcFilterParams");
  }
  if (!a.ref_state.empty()) {
    out.ref_state = nested(fbb, a.ref_state);
    out.ref_state_type = fbb.CreateString("HevcRps");
  }
  out.grid = build_grid(fbb, a.grid);
  return out;
}

}  // namespace ovt::hm
