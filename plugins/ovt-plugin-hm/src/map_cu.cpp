// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// TComDataCU -> CellGrid, Block tree, MotionVector and RefSlot (chapter 07 §7.6.4 mapping, chapter 06 §6.3,
// WI-4.105 part b). Reads the getters tests/oracles/hm_cu_dump.cpp dumps, with the same two readings of HM's storage:
// the cbf of a component is the bit at its transform-unit leaf, and a 4x4 luma transform in 4:2:0 / 4:2:2 has its
// chroma cbf at the parent depth.

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "analysis.hpp"

// HM's headers (system includes: HM's warnings are not OVT's, WI-4.104).
#include "TLibCommon/CommonDef.h"
#include "TLibCommon/TComChromaFormat.h"
#include "TLibCommon/TComDataCU.h"
#include "TLibCommon/TComPic.h"
#include "TLibCommon/TComPicSym.h"
#include "TLibCommon/TComRom.h"
#include "TLibCommon/TComSlice.h"

namespace ovt::hm {
namespace {

constexpr uint8_t kTxNone = 0xFF;
constexpr int16_t kModePcm = 64;  // CellGrid.intra_mode_y of a PCM CU (chapter 06 §6.3.1)
constexpr uint16_t kMvBi = 1U << 0;
constexpr uint16_t kMvWeighted = 1U << 1;

// z-scan index of the 4x4 unit (x4, y4) inside a CTU: even bits x, odd bits y.
UInt zscan(UInt x4, UInt y4) {
  UInt z = 0;
  for (UInt bit = 0; bit < 8; ++bit) {
    z |= ((x4 >> bit) & 1U) << (2 * bit);
    z |= ((y4 >> bit) & 1U) << ((2 * bit) + 1);
  }
  return z;
}

// QpY of unit z: HM keeps it as a signed char.
int16_t qp_of(const TComDataCU& cu, UInt z) {
  return static_cast<int16_t>(static_cast<int>(cu.getQP(z)));  // NOLINT(bugprone-signed-char-misuse,cert-str34-c)
}

int log2_of(UInt v) {
  int n = 0;
  while ((1U << (n + 1)) <= v) {
    ++n;
  }
  return n;
}

// PU rectangles of a CU of size s at (0, 0), Table 7-10 order of partIdx.
std::vector<fb::Rect> pu_rects(PartSize mode, int s) {
  const int h = s / 2;
  const int q = s / 4;
  switch (mode) {
    case SIZE_2NxN:
      return {{0, 0, s, h}, {0, h, s, h}};
    case SIZE_Nx2N:
      return {{0, 0, h, s}, {h, 0, h, s}};
    case SIZE_NxN:
      return {{0, 0, h, h}, {h, 0, h, h}, {0, h, h, h}, {h, h, h, h}};
    case SIZE_2NxnU:
      return {{0, 0, s, q}, {0, q, s, s - q}};
    case SIZE_2NxnD:
      return {{0, 0, s, s - q}, {0, s - q, s, q}};
    case SIZE_nLx2N:
      return {{0, 0, q, s}, {q, 0, s - q, s}};
    case SIZE_nRx2N:
      return {{0, 0, s - q, s}, {s - q, 0, q, s}};
    default:
      return {{0, 0, s, s}};
  }
}

template <class Builder>
std::vector<uint8_t> finish(flatbuffers::FlatBufferBuilder& fbb, Builder& b) {
  fbb.Finish(b.Finish());
  const flatbuffers::span<const uint8_t> buf = fbb.GetBufferSpan();
  std::vector<uint8_t> out(buf.begin(), buf.end());
  fbb.Clear();
  return out;
}

class Mapper {
 public:
  Mapper(TComPic& pic, const PicIndex& index, Analysis* out)
      : pic_(pic),
        index_(index),
        out_(*out),
        sps_(*pic.getSlice(0)->getSPS()),
        pps_(*pic.getSlice(0)->getPPS()),
        chroma_(sps_.getChromaFormatIdc()),
        width_(static_cast<int>(sps_.getPicWidthInLumaSamples())),
        height_(static_cast<int>(sps_.getPicHeightInLumaSamples())),
        ctu_(static_cast<int>(sps_.getMaxCUWidth())),
        total_parts_(static_cast<UInt>((ctu_ / 4) * (ctu_ / 4))) {}

  void run() {
    Grid& g = out_.grid;
    g.resize(static_cast<uint32_t>((width_ + 3) / 4), static_cast<uint32_t>((height_ + 3) / 4));
    BlockRec root;
    root.rect = fb::Rect(0, 0, width_, height_);
    out_.blocks.push_back(std::move(root));
    const TComPicSym& sym = *pic_.getPicSym();
    for (UInt ts = 0; ts < pic_.getNumberOfCtusInFrame(); ++ts) {
      ctu(ts, sym.getCtuTsToRsAddrMap(static_cast<Int>(ts)));
    }
    for (std::size_t i = 0; i < g.ref0.size(); ++i) {
      for (const int8_t r : {g.ref0[i], g.ref1[i]}) {
        if (r >= 0) {
          ++out_.refs[static_cast<std::size_t>(r)].usage_cells;
        }
      }
    }
    out_.valid = true;
  }

 private:
  TComPic& pic_;
  const PicIndex& index_;
  Analysis& out_;
  const TComSPS& sps_;
  const TComPPS& pps_;
  const ChromaFormat chroma_;
  const int width_;
  const int height_;
  const int ctu_;
  const UInt total_parts_;  // 4x4 units in a CTU
  flatbuffers::FlatBufferBuilder ext_{256};
  // Per slice segment (by its TComSlice): the RefSlot index of every L0 / L1 entry.
  std::map<const TComSlice*, std::array<std::vector<int8_t>, 2>> slots_;
  std::vector<std::pair<const TComSlice*, std::array<std::vector<int8_t>, 2>>> lists_seen_;
  int slice_count_ = 0;

  // ---- references (chapter 06 §6.3.1 RefSlot.name) ----

  const std::array<std::vector<int8_t>, 2>& slots_of(const TComSlice* s) {
    auto it = slots_.find(s);
    if (it != slots_.end()) {
      return it->second;
    }
    // A slice whose lists equal an earlier slice's shares its slots; otherwise it adds "S<n>:" slots.
    const int n = slice_count_++;
    for (const auto& [other, slots] : lists_seen_) {
      if (same_lists(*s, *other)) {
        return slots_.emplace(s, slots).first->second;
      }
    }
    std::array<std::vector<int8_t>, 2> slots;
    for (int l = 0; l < 2; ++l) {
      const auto list = static_cast<RefPicList>(l);
      for (Int i = 0; i < s->getNumRefIdx(list); ++i) {
        const TComPic* ref = s->getRefPic(list, i);
        RefRec r;
        r.name = (n == 0 ? std::string() : "S" + std::to_string(n) + ":") + (l == 0 ? "L0[" : "L1[") +
                 std::to_string(i) + "]";
        const auto found = ref != nullptr ? index_.find(ref) : index_.end();
        r.decode_index = found != index_.end() ? found->second : -1;
        r.long_term = ref != nullptr && ref->getIsLongTerm();
        slots.at(static_cast<std::size_t>(l)).push_back(static_cast<int8_t>(out_.refs.size()));
        out_.refs.push_back(std::move(r));
      }
    }
    lists_seen_.emplace_back(s, slots);
    return slots_.emplace(s, std::move(slots)).first->second;
  }

  static bool same_lists(const TComSlice& a, const TComSlice& b) {
    for (int l = 0; l < 2; ++l) {
      const auto list = static_cast<RefPicList>(l);
      if (a.getNumRefIdx(list) != b.getNumRefIdx(list)) {
        return false;
      }
      for (Int i = 0; i < a.getNumRefIdx(list); ++i) {
        if (a.getRefPic(list, i) != b.getRefPic(list, i)) {
          return false;
        }
      }
    }
    return true;
  }

  // ---- blocks ----

  uint32_t add_block(uint32_t parent, BlockRec b) {
    const auto id = static_cast<uint32_t>(out_.blocks.size());
    b.parent = static_cast<int32_t>(parent);
    b.level = static_cast<uint8_t>(out_.blocks[parent].level + 1);
    out_.blocks[parent].children.push_back(id);
    out_.blocks.push_back(std::move(b));
    return id;
  }

  template <class F>
  void for_cells(const fb::Rect& r, F&& f) {
    const Grid& g = out_.grid;
    for (int y = r.y() / 4; y < (r.y() + r.h()) / 4 && y < static_cast<int>(g.h4); ++y) {
      for (int x = r.x() / 4; x < (r.x() + r.w()) / 4 && x < static_cast<int>(g.w4); ++x) {
        f((static_cast<std::size_t>(y) * g.w4) + static_cast<std::size_t>(x), static_cast<UInt>(x),
          static_cast<UInt>(y));
      }
    }
  }

  void ctu(UInt ts, UInt rs) {
    TComDataCU* cu = pic_.getCtu(rs);
    const auto x0 = static_cast<int>(cu->getCUPelX());
    const auto y0 = static_cast<int>(cu->getCUPelY());
    const TComSlice* slice = cu->getSlice();
    BlockRec b;
    b.kind = fb::BlockKind::CTU;
    b.rect = fb::Rect(x0, y0, std::min(ctu_, width_ - x0), std::min(ctu_, height_ - y0));
    b.ext_type = "HevcCtu";
    const uint8_t sao_luma = ctu_ext(ts, rs, *slice, &b.ext);
    const uint32_t id = add_block(0, std::move(b));
    for_cells(out_.blocks[id].rect, [&](std::size_t i, UInt, UInt) { out_.grid.filter_idx[i] = sao_luma; });
    const auto& slots = slots_of(slice);
    coding_quadtree(*cu, slots, id, 0, 0, x0, y0, ctu_);
  }

  // SAO of one CTU after HM resolved merges (TComSampleAdaptiveOffset::reconstructBlkSAOParams), per component.
  struct Sao {
    std::vector<fb::HevcSaoType> type;
    std::vector<int16_t> offsets;  // SaoOffsetVal, 4 per component
    std::vector<uint8_t> band;
    std::vector<uint8_t> eo_class;

    void add(const SAOOffset& o, bool on) {
      const bool bo = on && o.typeIdc == SAO_TYPE_START_BO;
      type.push_back(!on ? fb::HevcSaoType::SAO_NOT_APPLIED
                         : (bo ? fb::HevcSaoType::SAO_BAND : fb::HevcSaoType::SAO_EDGE));
      band.push_back(bo ? static_cast<uint8_t>(o.typeAuxInfo) : 0);
      eo_class.push_back(on && !bo ? static_cast<uint8_t>(o.typeIdc) : 0);
      for (int k = 0; k < 4; ++k) {
        // EO offsets sit at edgeIdx classes 0, 1, 3, 4; BO offsets at the four bands from sao_band_position.
        const int at = bo ? (o.typeAuxInfo + k) % NUM_SAO_BO_CLASSES : (k < 2 ? k : k + 1);
        offsets.push_back(
            on ? static_cast<int16_t>(std::span<const Int, MAX_NUM_SAO_CLASSES>(o.offset)[static_cast<std::size_t>(at)])
               : int16_t{0});
      }
    }
  };

  Sao sao_of(UInt rs, const TComSlice& slice) {
    Sao sao;
    if (!sps_.getUseSAO()) {
      return sao;
    }
    TComPicSym& sym = *pic_.getPicSym();
    SAOBlkParam& p = std::span(sym.getSAOBlkParam(), sym.getNumberOfCtusInFrame())[rs];
    for (UInt c = 0; c < 3; ++c) {
      const auto comp = static_cast<ComponentID>(c);
      const bool on = c < getNumberValidComponents(chroma_) && slice.getSaoEnabledFlag(toChannelType(comp)) &&
                      p[comp].modeIdc != SAO_MODE_OFF;
      sao.add(p[comp], on);
    }
    return sao;
  }

  // HevcCtu; returns SaoTypeIdx[0] for CellGrid.filter_idx.
  uint8_t ctu_ext(UInt ts, UInt rs, const TComSlice& slice, std::vector<uint8_t>* ext) {
    const Sao sao = sao_of(rs, slice);
    const auto type_v = ext_.CreateVector(sao.type);
    const auto offsets_v = ext_.CreateVector(sao.offsets);
    const auto band_v = ext_.CreateVector(sao.band);
    const auto eo_v = ext_.CreateVector(sao.eo_class);
    fb::HevcCtuBuilder h(ext_);
    h.add_CtbAddrInRs(rs);
    h.add_CtbAddrInTs(ts);
    h.add_tile_idx(static_cast<uint16_t>(pic_.getPicSym()->getTileIdxMap(static_cast<Int>(rs))));
    if (!sao.type.empty()) {
      h.add_SaoTypeIdx(type_v);
      h.add_SaoOffsetVal(offsets_v);
      h.add_sao_band_position(band_v);
      h.add_sao_eo_class(eo_v);
    }
    *ext = finish(ext_, h);
    return sao.type.empty() ? 0 : static_cast<uint8_t>(sao.type[0]);
  }

  // CU quadtree: (x, y) of a square of size s at depth d, z its first partition in the CTU.
  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by the CTU size (at most 4 levels below a 64x64 CTU)
  void coding_quadtree(TComDataCU& cu, const std::array<std::vector<int8_t>, 2>& slots, uint32_t parent, UInt z, UInt d,
                       int x, int y, int s) {
    if (x >= width_ || y >= height_) {
      return;
    }
    if (cu.getDepth(z) > d || x + s > width_ || y + s > height_) {
      const UInt quarter = cu.getTotalNumPart() >> (2 * (d + 1));
      const int h = s / 2;
      for (UInt k = 0; k < 4; ++k) {
        coding_quadtree(cu, slots, parent, z + (k * quarter), d + 1, x + (static_cast<int>(k & 1U) * h),
                        y + (static_cast<int>(k >> 1U) * h), h);
      }
      return;
    }
    coding_unit(cu, slots, parent, z, d, x, y, s);
  }

  static fb::PredKind pred_of(const TComDataCU& cu, UInt z) {
    if (cu.getIPCMFlag(z)) {
      return fb::PredKind::PCM;
    }
    if (cu.getSkipFlag(z)) {
      return fb::PredKind::Skip;
    }
    return cu.isIntra(z) ? fb::PredKind::Intra : fb::PredKind::Inter;
  }

  // IntraPredModeC of unit z (8.4.3): DM resolved to the co-located luma mode, then Table 8-3 for 4:2:2.
  [[nodiscard]] int16_t chroma_mode(const TComDataCU& cu, UInt z) const {
    if (chroma_ == CHROMA_400) {
      return -1;
    }
    UInt mode = cu.getIntraDir(CHANNEL_TYPE_CHROMA, z);
    if (mode == DM_CHROMA_IDX) {
      const auto parts_per_min_cu = static_cast<Int>(
          1U << (2 * (sps_.getMaxTotalCUDepth() - static_cast<UInt>(sps_.getLog2DiffMaxMinCodingBlockSize()))));
      mode = cu.getIntraDir(CHANNEL_TYPE_LUMA, getChromasCorrespondingPULumaIdx(z, chroma_, parts_per_min_cu));
    }
    if (chroma_ == CHROMA_422) {
      mode = g_chroma422IntraAngleMappingTable[mode];
    }
    return static_cast<int16_t>(mode);
  }

  void coding_unit(TComDataCU& cu, const std::array<std::vector<int8_t>, 2>& slots, uint32_t parent, UInt z, UInt d,
                   int x, int y, int s) {
    const fb::PredKind pred = pred_of(cu, z);
    const auto qp = qp_of(cu, z);
    const bool transform = pred != fb::PredKind::PCM && pred != fb::PredKind::Skip &&
                           (pred == fb::PredKind::Intra || cu.getQtRootCbf(z) != 0);
    const PartSize part = cu.getPartitionSize(z);
    std::vector<uint8_t> modes_c;
    BlockRec b;
    b.kind = fb::BlockKind::CU;
    b.rect = fb::Rect(x, y, s, s);
    b.partition_type = static_cast<uint8_t>(part);
    b.pred = pred;
    b.skip = cu.getSkipFlag(z);
    b.merge = cu.getMergeFlag(z);
    b.qp = qp;
    b.ext_type = "HevcCu";
    const uint32_t id = add_block(parent, std::move(b));

    const UInt ctu_x4 = cu.getCUPelX() / 4;
    const UInt ctu_y4 = cu.getCUPelY() / 4;
    Grid& g = out_.grid;
    for_cells(fb::Rect(x, y, s, s), [&](std::size_t i, UInt cx, UInt cy) {
      const UInt u = zscan(cx - ctu_x4, cy - ctu_y4);
      g.block_id[i] = id;
      g.pred[i] = static_cast<uint8_t>(pred);
      g.qp[i] = qp;
      if (pred == fb::PredKind::PCM) {
        g.intra_y[i] = kModePcm;
      } else if (pred == fb::PredKind::Intra) {
        g.intra_y[i] = static_cast<int16_t>(cu.getIntraDir(CHANNEL_TYPE_LUMA, u));
        g.intra_c[i] = chroma_mode(cu, u);
      } else {
        motion_cell(cu, slots, u, i);
      }
    });

    const std::vector<fb::Rect> pus = pu_rects(part, s);
    for (std::size_t k = 0; k < pus.size(); ++k) {
      const fb::Rect r(x + pus[k].x(), y + pus[k].y(), pus[k].w(), pus[k].h());
      const UInt pz = zscan((static_cast<UInt>(r.x()) / 4) - ctu_x4, (static_cast<UInt>(r.y()) / 4) - ctu_y4);
      if (pred == fb::PredKind::Intra && (k == 0 || chroma_ == CHROMA_444)) {
        modes_c.push_back(static_cast<uint8_t>(chroma_mode(cu, pz)));
      }
      prediction_unit(cu, slots, id, static_cast<uint8_t>(k), pz, r, pred);
    }
    if (transform) {
      transform_tree(cu, id, z, d, 0, x, y, s, 0, pred);
    }
    out_.blocks[id].ext = cu_ext(cu, z, d, s, pred, transform, modes_c);
  }

  std::vector<uint8_t> cu_ext(const TComDataCU& cu, UInt z, UInt d, int s, fb::PredKind pred, bool transform,
                              const std::vector<uint8_t>& modes_c) {
    const auto modes_v = ext_.CreateVector(modes_c);
    fb::HevcCuBuilder h(ext_);
    h.add_CtDepth(static_cast<uint8_t>(d));
    h.add_log2CbSize(static_cast<uint8_t>(log2_of(static_cast<UInt>(s))));
    h.add_cu_transquant_bypass_flag(static_cast<uint8_t>(cu.getCUTransquantBypass(z)));
    h.add_cu_skip_flag(static_cast<uint8_t>(cu.getSkipFlag(z)));
    h.add_CuPredMode(pred == fb::PredKind::Skip ? fb::HevcPredMode::MODE_SKIP
                     : cu.isIntra(z)            ? fb::HevcPredMode::MODE_INTRA
                                                : fb::HevcPredMode::MODE_INTER);
    h.add_part_mode(static_cast<fb::HevcPartMode>(cu.getPartitionSize(z)));
    h.add_pcm_flag(static_cast<uint8_t>(cu.getIPCMFlag(z)));
    if (pred == fb::PredKind::Inter) {
      h.add_rqt_root_cbf(static_cast<uint8_t>(transform));
    }
    h.add_QpY(cu.getQP(z));
    if (!modes_c.empty()) {
      h.add_IntraPredModeC(modes_v);
    }
    return finish(ext_, h);
  }

  // Grid motion of one 4x4 unit u of an inter CU at cell i.
  void motion_cell(const TComDataCU& cu, const std::array<std::vector<int8_t>, 2>& slots, UInt u, std::size_t i) {
    Grid& g = out_.grid;
    for (int l = 0; l < 2; ++l) {
      const TComCUMvField* f = cu.getCUMvField(static_cast<RefPicList>(l));
      const int ref = f->getRefIdx(static_cast<Int>(u));
      const auto& list = slots.at(static_cast<std::size_t>(l));
      if (ref < 0 || static_cast<std::size_t>(ref) >= list.size()) {
        continue;
      }
      const TComMv& mv = f->getMv(static_cast<Int>(u));
      (l == 0 ? g.mv0_x : g.mv1_x)[i] = static_cast<int16_t>(mv.getHor() * 4);  // 1/4 -> 1/16 luma sample
      (l == 0 ? g.mv0_y : g.mv1_y)[i] = static_cast<int16_t>(mv.getVer() * 4);
      (l == 0 ? g.ref0 : g.ref1)[i] = list[static_cast<std::size_t>(ref)];
    }
  }

  void prediction_unit(const TComDataCU& cu, const std::array<std::vector<int8_t>, 2>& slots, uint32_t parent,
                       uint8_t part_idx, UInt pz, const fb::Rect& r, fb::PredKind pred) {
    BlockRec b;
    b.kind = fb::BlockKind::PU;
    b.rect = r;
    b.partition_type = part_idx;
    b.pred = pred;
    b.skip = cu.getSkipFlag(pz);
    b.merge = cu.getMergeFlag(pz);
    b.qp = qp_of(cu, pz);
    b.ext_type = "HevcPu";
    const uint32_t id = add_block(parent, std::move(b));

    fb::HevcPuBuilder h(ext_);
    h.add_partIdx(part_idx);
    if (pred == fb::PredKind::Intra) {
      h.add_IntraPredModeY(cu.getIntraDir(CHANNEL_TYPE_LUMA, pz));
    } else if (pred == fb::PredKind::Inter || pred == fb::PredKind::Skip) {
      inter_pu(cu, slots, id, pz, r, &h);
    }
    out_.blocks[id].ext = finish(ext_, h);
  }

  // The HevcPu fields of list @p l (the AMVP syntax elements only when the PU is not merged).
  static void add_list_fields(fb::HevcPuBuilder* h, int l, bool merge, int ref, const TComMv& mv, const TComMv& mvd,
                              int mvp, int poc) {
    if (l == 0) {
      h->add_refIdxL0(static_cast<int8_t>(ref));
      h->add_mvL0_x(static_cast<int16_t>(mv.getHor()));
      h->add_mvL0_y(static_cast<int16_t>(mv.getVer()));
      h->add_ref_poc_l0(poc);
      if (!merge) {
        h->add_ref_idx_l0(static_cast<int8_t>(ref));
        h->add_mvd_l0_x(static_cast<int16_t>(mvd.getHor()));
        h->add_mvd_l0_y(static_cast<int16_t>(mvd.getVer()));
        h->add_mvp_l0_flag(static_cast<uint8_t>(mvp));
      }
      return;
    }
    h->add_refIdxL1(static_cast<int8_t>(ref));
    h->add_mvL1_x(static_cast<int16_t>(mv.getHor()));
    h->add_mvL1_y(static_cast<int16_t>(mv.getVer()));
    h->add_ref_poc_l1(poc);
    if (!merge) {
      h->add_ref_idx_l1(static_cast<int8_t>(ref));
      h->add_mvd_l1_x(static_cast<int16_t>(mvd.getHor()));
      h->add_mvd_l1_y(static_cast<int16_t>(mvd.getVer()));
      h->add_mvp_l1_flag(static_cast<uint8_t>(mvp));
    }
  }

  void inter_pu(const TComDataCU& cu, const std::array<std::vector<int8_t>, 2>& slots, uint32_t id, UInt pz,
                const fb::Rect& r, fb::HevcPuBuilder* h) {
    const bool merge = cu.getMergeFlag(pz);
    const UInt dir = cu.getInterDir(pz);  // 1 L0, 2 L1, 3 Bi
    const TComSlice& slice = *cu.getSlice();
    const bool weighted =
        (slice.getSliceType() == P_SLICE && pps_.getUseWP()) || (slice.getSliceType() == B_SLICE && pps_.getWPBiPred());
    h->add_merge_flag(static_cast<uint8_t>(merge));
    if (merge) {
      h->add_merge_idx(cu.getMergeIndex(pz));
    }
    if (dir >= 1 && dir <= 3) {
      h->add_inter_pred_idc(static_cast<fb::HevcInterPredIdc>(dir - 1));
    }
    for (int l = 0; l < 2; ++l) {
      const auto list = static_cast<RefPicList>(l);
      const TComCUMvField* f = cu.getCUMvField(list);
      const int ref = f->getRefIdx(static_cast<Int>(pz));
      const bool used = (dir & (1U << l)) != 0 && ref >= 0 &&
                        static_cast<std::size_t>(ref) < slots.at(static_cast<std::size_t>(l)).size();
      const TComMv& mv = f->getMv(static_cast<Int>(pz));
      const TComMv& mvd = f->getMvd(static_cast<Int>(pz));
      if (l == 0) {
        h->add_predFlagL0(static_cast<uint8_t>(used));
      } else {
        h->add_predFlagL1(static_cast<uint8_t>(used));
      }
      if (!used) {
        continue;
      }
      const int mvp = cu.getMVPIdx(list, pz);
      add_list_fields(h, l, merge, ref, mv, mvd, mvp, slice.getRefPOC(list, ref));
      MvRec m;
      m.block_id = id;
      m.list = static_cast<uint8_t>(l);
      m.ref = slots.at(static_cast<std::size_t>(l))[static_cast<std::size_t>(ref)];
      m.rect = r;
      m.mv_x = static_cast<int16_t>(mv.getHor() * 4);
      m.mv_y = static_cast<int16_t>(mv.getVer() * 4);
      m.mvd_x = merge ? int16_t{0} : static_cast<int16_t>(mvd.getHor() * 4);
      m.mvd_y = merge ? int16_t{0} : static_cast<int16_t>(mvd.getVer() * 4);
      // SPEC-ASSUMPTION(#495): HM does not keep which candidate a merge index or mvp flag selected, so the
      // ":<source>" suffix of chapter 06 §6.3.1 is left out.
      m.predictor = merge ? "merge[" + std::to_string(cu.getMergeIndex(pz)) + "]" : "amvp[" + std::to_string(mvp) + "]";
      m.flags = static_cast<uint16_t>((dir == 3 ? kMvBi : 0U) | (weighted ? kMvWeighted : 0U));
      out_.mvs.push_back(std::move(m));
    }
  }

  // The cbf of component c at the transform-unit leaf of unit u at transform depth t (log2 size l2).
  [[nodiscard]] int leaf_cbf(const TComDataCU& cu, UInt u, ComponentID c, UInt t, int l2) const {
    if (c != COMPONENT_Y && chroma_ == CHROMA_400) {
      return 0;
    }
    if (c != COMPONENT_Y && l2 == 2 && chroma_ != CHROMA_444 && t > 0) {
      return cu.getCbf(u, c, t - 1);
    }
    return cu.getCbf(u, c, t);
  }

  [[nodiscard]] static uint8_t tx_type_of(const TComDataCU& cu, UInt u, int l2, fb::PredKind pred) {
    if (cu.getCUTransquantBypass(u)) {
      return 3;
    }
    if (cu.getTransformSkip(u, COMPONENT_Y) != 0) {
      return 2;
    }
    return pred == fb::PredKind::Intra && l2 == 2 ? 1 : 0;  // DST-VII for 4x4 intra luma (8.6.4.2)
  }

  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by the CTU size (at most 4 levels below a 64x64 CTU)
  void transform_tree(const TComDataCU& cu, uint32_t parent, UInt z, UInt d, UInt t, int x, int y, int s,
                      uint8_t blk_idx, fb::PredKind pred) {
    const int l2 = log2_of(static_cast<UInt>(s));
    const bool split = cu.getTransformIdx(z) > t;
    BlockRec b;
    b.kind = fb::BlockKind::TU;
    b.rect = fb::Rect(x, y, s, s);
    b.partition_type = static_cast<uint8_t>(t);
    b.pred = pred;
    b.qp = qp_of(cu, z);
    b.ext_type = "HevcTu";
    const uint32_t id = add_block(parent, std::move(b));
    if (split) {
      const UInt quarter = total_parts_ >> (2 * (d + t + 1));
      const int h = s / 2;
      for (UInt k = 0; k < 4; ++k) {
        transform_tree(cu, id, z + (k * quarter), d, t + 1, x + (static_cast<int>(k & 1U) * h),
                       y + (static_cast<int>(k >> 1U) * h), h, static_cast<uint8_t>(k), pred);
      }
    } else {
      const UInt ctu_x4 = cu.getCUPelX() / 4;
      const UInt ctu_y4 = cu.getCUPelY() / 4;
      Grid& g = out_.grid;
      for_cells(fb::Rect(x, y, s, s), [&](std::size_t i, UInt cx, UInt cy) {
        const UInt u = zscan(cx - ctu_x4, cy - ctu_y4);
        g.tx_log2[i] = static_cast<uint8_t>(l2);
        g.tx_type[i] = tx_type_of(cu, u, l2, pred);
        g.cbf[i] =
            static_cast<uint8_t>(leaf_cbf(cu, u, COMPONENT_Y, t, l2) | (leaf_cbf(cu, u, COMPONENT_Cb, t, l2) << 1) |
                                 (leaf_cbf(cu, u, COMPONENT_Cr, t, l2) << 2));
      });
    }
    out_.blocks[id].ext = tu_ext(cu, z, t, l2, blk_idx, split, pred);
  }

  std::vector<uint8_t> tu_ext(const TComDataCU& cu, UInt z, UInt t, int l2, uint8_t blk_idx, bool split,
                              fb::PredKind pred) {
    // Chroma flags of a node: none for 4:0:0, and a 4x4 luma node of 4:2:0 / 4:2:2 carries none of its own.
    const bool chroma = chroma_ != CHROMA_400 && (l2 > 2 || chroma_ == CHROMA_444);
    std::vector<uint8_t> cb;
    std::vector<uint8_t> cr;
    if (chroma) {
      cb.push_back(cu.getCbf(z, COMPONENT_Cb, t));
      cr.push_back(cu.getCbf(z, COMPONENT_Cr, t));
    }
    std::vector<uint8_t> skip;
    if (!split && pps_.getUseTransformSkip()) {
      const bool allowed = l2 <= static_cast<int>(pps_.getPpsRangeExtension().getLog2MaxTransformSkipBlockSize());
      for (UInt c = 0; c < 3; ++c) {
        const bool present = allowed && c < getNumberValidComponents(chroma_) && !cu.getCUTransquantBypass(z);
        skip.push_back(present ? cu.getTransformSkip(z, static_cast<ComponentID>(c)) : 255);
      }
    }
    const auto cb_v = ext_.CreateVector(cb);
    const auto cr_v = ext_.CreateVector(cr);
    const auto skip_v = ext_.CreateVector(skip);
    fb::HevcTuBuilder h(ext_);
    h.add_trafoDepth(static_cast<uint8_t>(t));
    h.add_log2TrafoSize(static_cast<uint8_t>(l2));
    h.add_blkIdx(blk_idx);
    h.add_split_transform_flag(static_cast<uint8_t>(split));
    h.add_cbf_luma(cu.getCbf(z, COMPONENT_Y, t));
    if (chroma) {
      h.add_cbf_cb(cb_v);
      h.add_cbf_cr(cr_v);
    }
    if (!skip.empty()) {
      h.add_transform_skip_flag(skip_v);
    }
    if (!split) {
      h.add_dst_used(static_cast<uint8_t>(tx_type_of(cu, z, l2, pred) == 1));
    }
    return finish(ext_, h);
  }
};

}  // namespace

void map_cu(TComPic& pic, const PicIndex& index, Analysis* out) {
  *out = Analysis();
  Mapper(pic, index, out).run();
}

}  // namespace ovt::hm
