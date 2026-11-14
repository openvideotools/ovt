// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// CellGrid vectors from libde265's per-4x4 metadata (chapter 07 §7.6.2 mapping table; WI-2.164 steps 2, 3 and 5).

#include "map_grid.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace ovt::de265 {

namespace fb = ovt::fb;

GridCells::GridCells(std::size_t n)
    : block_id(n),
      partition_id(n),
      pred(n, kPredNone),
      qp(n),
      intra_mode_y(n, -1),
      intra_mode_c(n, -1),
      mv0_x(n),
      mv0_y(n),
      mv1_x(n),
      mv1_y(n),
      ref0(n, -1),
      ref1(n, -1),
      tx_log2(n, kNotApplicable),
      tx_type(n, kNotApplicable),
      cbf(n),
      deblock_bs_v(n),
      deblock_bs_h(n),
      filter_idx(n) {}

namespace {

constexpr std::size_t kMaxRefs = 16;
constexpr std::size_t kMaxSlots = 127;  // refs[] indices are int8 in the grid

// Active entries of list @p list of @p sh (H.265 7.4.7.1: L1 only in B slices).
int32_t list_size(const SliceHeader& sh, int list) {
  if (sh.slice_type == 2 || (list == 1 && sh.slice_type != 0)) {
    return 0;
  }
  return std::clamp<int32_t>(sh.num_ref_idx_active.at(static_cast<std::size_t>(list)), 0,
                             static_cast<int32_t>(kMaxRefs));
}

bool same_lists(const SliceHeader& a, const SliceHeader& b) {
  for (int l = 0; l < 2; ++l) {
    const int32_t n = list_size(a, l);
    if (n != list_size(b, l)) {
      return false;
    }
    const auto ul = static_cast<std::size_t>(l);
    for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
      if (a.RefPicList_POC.at(ul).at(i) != b.RefPicList_POC.at(ul).at(i) ||
          a.RefPicList_LongTerm.at(ul).at(i) != b.RefPicList_LongTerm.at(ul).at(i)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

RefTable::RefTable(const std::vector<SliceHeader>& slices) {
  base_.reserve(slices.size());
  size_.reserve(slices.size());
  for (std::size_t s = 0; s < slices.size(); ++s) {
    const SliceHeader& sh = slices[s];
    const std::array<int32_t, 2> n = {list_size(sh, 0), list_size(sh, 1)};
    std::array<int32_t, 2> base = shared_base(slices, s);
    if (base[0] < 0 && base[1] < 0 && (n[0] > 0 || n[1] > 0) &&
        slots_.size() + static_cast<std::size_t>(n[0] + n[1]) <= kMaxSlots) {
      base = add_slots(sh, s, n);
    }
    base_.push_back(base);
    size_.push_back(base[0] >= 0 || base[1] >= 0 ? n : std::array<int32_t, 2>{0, 0});
  }
}

std::array<int32_t, 2> RefTable::shared_base(const std::vector<SliceHeader>& slices, std::size_t s) const {
  for (std::size_t t = 0; t < s; ++t) {
    if ((size_[t][0] > 0 || size_[t][1] > 0) && same_lists(slices[t], slices[s])) {
      return base_[t];
    }
  }
  return {-1, -1};
}

std::array<int32_t, 2> RefTable::add_slots(const SliceHeader& sh, std::size_t s, const std::array<int32_t, 2>& n) {
  std::array<int32_t, 2> base = {-1, -1};
  const std::string prefix = slots_.empty() ? std::string() : "S" + std::to_string(s) + ":";
  for (int l = 0; l < 2; ++l) {
    const auto ul = static_cast<std::size_t>(l);
    base.at(ul) = n.at(ul) > 0 ? static_cast<int32_t>(slots_.size()) : -1;
    for (int32_t i = 0; i < n.at(ul); ++i) {
      slots_.push_back(Slot{prefix + (l == 0 ? "L0[" : "L1[") + std::to_string(i) + "]",
                            sh.RefPicList_LongTerm.at(ul).at(static_cast<std::size_t>(i)) != 0, 0});
    }
  }
  return base;
}

int8_t RefTable::slot(std::size_t slice, int list, int ref_idx) const {
  if (slice >= base_.size() || list < 0 || list > 1) {
    return -1;
  }
  const auto ul = static_cast<std::size_t>(list);
  const int32_t base = base_[slice].at(ul);
  if (base < 0 || ref_idx < 0 || ref_idx >= size_[slice].at(ul)) {
    return -1;
  }
  return static_cast<int8_t>(base + ref_idx);
}

namespace {

// The cell fields that do not depend on the coding unit: partition, deblocking and SAO.
void fill_filters(GridCells& g, const BlockData& d, std::size_t i, uint32_t y, std::size_t ctb) {
  const std::size_t slice = d.ctb_slice[ctb];
  g.partition_id[i] = d.ctb_slice[ctb];
  // H.265 8.7.2: vertical edges lie on the 8x8 grid (the accessor gives 0 elsewhere); the horizontal pass visits
  // every 8th luma row only, so on the other rows the stock byte still holds a vertical bS (issue #216).
  g.deblock_bs_v[i] = d.bs_v[i];
  const uint8_t deblk = d.deblk[i];
  g.deblock_bs_h[i] = (y % 2 == 0 && (deblk & (kDeblockFlagHoriz | kDeblockPbEdgeHoriz)) != 0)
                          ? static_cast<uint8_t>(deblk & kDeblockBsMask)
                          : uint8_t{0};
  const bool sao_luma = slice < d.slices.size() && d.slices[slice].slice_sao_luma_flag != 0;
  g.filter_idx[i] = sao_luma ? d.sao[ctb].SaoTypeIdx[0] : uint8_t{0};
}

void fill_intra(GridCells& g, const BlockData& d, const PictureFormat& fmt, const CbInfo& cb, std::size_t i) {
  if (cb.pcm_flag != 0) {
    g.pred[i] = kPredPcm;
    g.intra_mode_y[i] = kIntraModePcm;
    return;
  }
  g.pred[i] = kPredIntra;
  g.intra_mode_y[i] = d.intra_y[i] == kNotApplicable ? int16_t{-1} : int16_t{d.intra_y[i]};
  if (fmt.chroma_format != 0) {
    g.intra_mode_c[i] = d.intra_c[i] == kNotApplicable ? int16_t{-1} : int16_t{d.intra_c[i]};
  }
}

void fill_motion(GridCells& g, const PbInfo& pb, std::size_t slice, std::size_t i, RefTable& refs) {
  for (int l = 0; l < 2; ++l) {
    const auto ul = static_cast<std::size_t>(l);
    if (pb.predFlag.at(ul) == 0) {
      continue;
    }
    const int8_t r = refs.slot(slice, l, pb.refIdx.at(ul));
    if (r < 0) {
      continue;  // a reference index outside the slice's list (corrupt input): no vector
    }
    refs.count(r);
    (l == 0 ? g.ref0 : g.ref1)[i] = r;
    (l == 0 ? g.mv0_x : g.mv1_x)[i] = mv16(pb.mv_x.at(ul));
    (l == 0 ? g.mv0_y : g.mv1_y)[i] = mv16(pb.mv_y.at(ul));
  }
}

}  // namespace

void fill_cells(GridCells& g, const BlockData& d, const PictureFormat& fmt, RefTable& refs) {
  const auto cb_shift = static_cast<uint32_t>(d.min_cb_log2 - 2);
  const auto ctb_shift = static_cast<uint32_t>(d.ctb_log2 - 2);
  for (uint32_t y = 0; y < d.cells_h; ++y) {
    for (uint32_t x = 0; x < d.cells_w; ++x) {
      const std::size_t i = std::size_t{y} * d.cells_w + x;
      const std::size_t ctb = std::size_t{y >> ctb_shift} * static_cast<std::size_t>(d.ctbs_w) + (x >> ctb_shift);
      fill_filters(g, d, i, y, ctb);
      const CbInfo& cb = d.cb[std::size_t{y >> cb_shift} * static_cast<std::size_t>(d.cbs_w) + (x >> cb_shift)];
      if (cb.log2CbSize == 0) {
        continue;  // not decoded (a missing or broken slice): pred None
      }
      g.qp[i] = cb.QP_Y;
      const auto mode = static_cast<De265PredMode>(cb.PredMode);
      if (mode == De265PredMode::Intra) {
        fill_intra(g, d, fmt, cb, i);
        continue;
      }
      g.pred[i] = mode == De265PredMode::Skip ? kPredSkip : kPredInter;
      fill_motion(g, d.pb[i], d.ctb_slice[ctb], i, refs);
    }
  }
}

flatbuffers::Offset<fb::CellGrid> build_grid(flatbuffers::FlatBufferBuilder& fbb, uint32_t cells_w, uint32_t cells_h,
                                             const GridCells& g) {
  const auto block_id = fbb.CreateVector(g.block_id);
  const auto partition_id = fbb.CreateVector(g.partition_id);
  const auto pred = fbb.CreateVector(g.pred);
  const auto qp = fbb.CreateVector(g.qp);
  const auto mode_y = fbb.CreateVector(g.intra_mode_y);
  const auto mode_c = fbb.CreateVector(g.intra_mode_c);
  const auto mv0_x = fbb.CreateVector(g.mv0_x);
  const auto mv0_y = fbb.CreateVector(g.mv0_y);
  const auto mv1_x = fbb.CreateVector(g.mv1_x);
  const auto mv1_y = fbb.CreateVector(g.mv1_y);
  const auto ref0 = fbb.CreateVector(g.ref0);
  const auto ref1 = fbb.CreateVector(g.ref1);
  const auto tx_w = fbb.CreateVector(g.tx_log2);
  const auto tx_h = fbb.CreateVector(g.tx_log2);
  const auto tx_type = fbb.CreateVector(g.tx_type);
  const auto cbf = fbb.CreateVector(g.cbf);
  const auto bs_v = fbb.CreateVector(g.deblock_bs_v);
  const auto bs_h = fbb.CreateVector(g.deblock_bs_h);
  const auto filter_idx = fbb.CreateVector(g.filter_idx);
  fb::CellGridBuilder b(fbb);
  b.add_cells_w(cells_w);
  b.add_cells_h(cells_h);
  b.add_block_id(block_id);
  b.add_partition_id(partition_id);
  b.add_pred(pred);
  b.add_qp(qp);
  b.add_intra_mode_y(mode_y);
  b.add_intra_mode_c(mode_c);
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
  b.add_deblock_bs_v(bs_v);
  b.add_deblock_bs_h(bs_h);
  b.add_filter_idx(filter_idx);
  return b.Finish();
}

flatbuffers::Offset<flatbuffers::Vector<uint32_t>> build_qp_hist(flatbuffers::FlatBufferBuilder& fbb,
                                                                 const GridCells& g) {
  std::vector<uint32_t> hist(256);
  for (const int16_t q : g.qp) {
    ++hist[static_cast<std::size_t>(std::clamp(q + 128, 0, 255))];
  }
  return fbb.CreateVector(hist);
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::RefSlot>>> build_refs(
    flatbuffers::FlatBufferBuilder& fbb, const RefTable& refs) {
  std::vector<flatbuffers::Offset<fb::RefSlot>> out;
  out.reserve(refs.slots().size());
  // RefSlot.frame stays (-1, -1): the host maps each slot's POC (HevcPu.ref_poc_l0/l1) to a decode index through
  // its DPB model (chapter 07 §7.6.2, §7.4.4).
  const fb::FrameId unknown(-1, -1);
  for (std::size_t k = 0; k < refs.slots().size(); ++k) {
    const RefTable::Slot& s = refs.slots()[k];
    out.push_back(fb::CreateRefSlotDirect(fbb, static_cast<uint8_t>(k), &unknown, s.name.c_str(), s.long_term,
                                          s.usage_cells, -1));
  }
  return fbb.CreateVector(out);
}

}  // namespace ovt::de265
