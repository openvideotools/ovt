// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// libde265 metadata -> Block tree (CTU -> CU -> PU / TU), MotionVector, RefSlot, partitions and HevcFilterParams
// (chapter 07 §7.6.2 mapping table, rows available with patch 0001; WI-2.164 steps 1 to 5). The tree follows
// chapter 06 §6.3.2: CTUs in tile scan, CUs in z-scan within their CTU, and for each CU its PUs (partIdx order) then
// its root TU with the nested transform tree.

#include "map_blocks.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "map_grid.hpp"

namespace ovt::de265 {

namespace {

namespace fb = ovt::fb;

constexpr int16_t kMvdUnavailable = -32768;
constexpr int kMaxTrafoDepth = 4;  // tu_info has split bits for trafoDepth 0..4
constexpr int kMaxTbLog2 = 5;      // H.265: MaxTbLog2SizeY <= 5

// libde265 / H.265 Table 7-10 PartMode values.
enum PartMode : uint8_t {
  k2Nx2N = 0,
  k2NxN = 1,
  kNx2N = 2,
  kNxN = 3,
  k2NxnU = 4,
  k2NxnD = 5,
  knLx2N = 6,
  knRx2N = 7,
};

// The prediction blocks of a CU of size @p n at (x, y) for @p part (H.265 7.3.8.5 prediction_unit() calls).
std::vector<fb::Rect> pu_rects(int32_t x, int32_t y, int32_t n, uint8_t part) {
  const int32_t h = n / 2;
  const int32_t q = n / 4;
  switch (part) {
    case k2NxN:
      return {{x, y, n, h}, {x, y + h, n, h}};
    case kNx2N:
      return {{x, y, h, n}, {x + h, y, h, n}};
    case kNxN:
      return {{x, y, h, h}, {x + h, y, h, h}, {x, y + h, h, h}, {x + h, y + h, h, h}};
    case k2NxnU:
      return {{x, y, n, q}, {x, y + q, n, n - q}};
    case k2NxnD:
      return {{x, y, n, n - q}, {x, y + n - q, n, q}};
    case knLx2N:
      return {{x, y, q, n}, {x + q, y, n - q, n}};
    case knRx2N:
      return {{x, y, n - q, n}, {x + n - q, y, q, n}};
    default:
      return {{x, y, n, n}};
  }
}

// One Block of the output tree before serialisation.
struct OutBlock {
  int32_t parent = -1;
  std::vector<uint32_t> children;
  fb::BlockKind kind = fb::BlockKind::Other;
  fb::Rect rect;
  uint8_t level = 0;
  uint8_t partition_type = kNotApplicable;
  uint8_t pred = kPredNone;
  bool skip = false;
  bool merge = false;
  int16_t qp = 0;
  const char* ext_type = "";
  std::vector<uint8_t> ext;
};

// What the tree needs to know about one CU while emitting its PUs and TUs.
struct CuContext {
  int32_t x = 0;
  int32_t y = 0;
  int32_t log2 = 0;
  const CbInfo* cb = nullptr;
  uint8_t pred = kPredNone;
  std::size_t slice = 0;
};

// Per slice segment: its CTBs in tile scan.
struct SegmentSpan {
  int32_t first_ts = std::numeric_limits<int32_t>::max();
  int32_t first_rs = -1;
  int32_t last_ts = -1;
  uint32_t count = 0;
  int32_t x0 = std::numeric_limits<int32_t>::max();  // bounding box in CTBs
  int32_t y0 = std::numeric_limits<int32_t>::max();
  int32_t x1 = -1;
  int32_t y1 = -1;

  void add(int32_t ts, int32_t rs, int32_t cx, int32_t cy) {
    if (ts < first_ts) {
      first_ts = ts;
      first_rs = rs;
    }
    last_ts = std::max(last_ts, ts);
    ++count;
    x0 = std::min(x0, cx);
    y0 = std::min(y0, cy);
    x1 = std::max(x1, cx);
    y1 = std::max(y1, cy);
  }
};

class Mapper {
 public:
  Mapper(const BlockData& d, const PictureFormat& fmt, GridCells& g, RefTable& refs)
      : d_(d), fmt_(fmt), g_(g), refs_(refs) {}

  std::vector<OutBlock> blocks;
  struct Mv {
    uint32_t block_id;
    uint8_t list;
    int8_t ref;
    fb::Rect rect;
    int16_t x;
    int16_t y;
    uint16_t flags;
  };
  std::vector<Mv> mvs;
  uint32_t skipped_cus = 0;  // CB metadata that does not form a CU inside its CTU (corrupt input)

  void emit_ctu(int32_t rs, uint16_t tile_idx, bool end_of_segment) {
    const int32_t cx = rs % d_.ctbs_w;
    const int32_t cy = rs / d_.ctbs_w;
    const int32_t x0 = cx << d_.ctb_log2;
    const int32_t y0 = cy << d_.ctb_log2;
    const int32_t size = 1 << d_.ctb_log2;
    const auto ctb = static_cast<std::size_t>(rs);
    if (cb_at(x0, y0).log2CbSize == 0) {
      return;  // not decoded: no CTU (its cells keep block_id 0 and pred None)
    }
    OutBlock b;
    b.parent = 0;
    b.kind = fb::BlockKind::CTU;
    b.rect = {x0, y0, std::min(size, fmt_.coded_w - x0), std::min(size, fmt_.coded_h - y0)};
    b.level = 1;
    b.ext_type = "HevcCtu";
    b.ext = hevc_ctu(rs, d_.ctb_addr_ts[ctb], d_.ctb_slice[ctb], tile_idx, end_of_segment);
    const uint32_t id = add(std::move(b));
    quadtree(x0, y0, d_.ctb_log2, id);
  }

  uint32_t add(OutBlock b) {
    const auto id = static_cast<uint32_t>(blocks.size());
    if (b.parent >= 0) {
      blocks[static_cast<std::size_t>(b.parent)].children.push_back(id);
    }
    blocks.push_back(std::move(b));
    return id;
  }

 private:
  const BlockData& d_;
  const PictureFormat& fmt_;
  GridCells& g_;
  RefTable& refs_;
  flatbuffers::FlatBufferBuilder ext_fbb_{256};

  [[nodiscard]] std::size_t cell(int32_t x, int32_t y) const {
    return static_cast<std::size_t>(y >> 2) * d_.cells_w + static_cast<std::size_t>(x >> 2);
  }
  [[nodiscard]] const CbInfo& cb_at(int32_t x, int32_t y) const {
    return d_.cb[static_cast<std::size_t>(y >> d_.min_cb_log2) * static_cast<std::size_t>(d_.cbs_w) +
                 static_cast<std::size_t>(x >> d_.min_cb_log2)];
  }
  [[nodiscard]] std::size_t ctb_at(int32_t x, int32_t y) const {
    return static_cast<std::size_t>(y >> d_.ctb_log2) * static_cast<std::size_t>(d_.ctbs_w) +
           static_cast<std::size_t>(x >> d_.ctb_log2);
  }

  template <typename Builder>
  std::vector<uint8_t> finish(Builder& b) {
    ext_fbb_.Finish(b.Finish());
    const std::span<const uint8_t> buf(ext_fbb_.GetBufferPointer(), ext_fbb_.GetSize());
    std::vector<uint8_t> out(buf.begin(), buf.end());
    ext_fbb_.Clear();
    return out;
  }

  std::vector<uint8_t> hevc_ctu(int32_t rs, int32_t ts, uint16_t slice, uint16_t tile_idx, bool end_of_segment) {
    const SaoInfo& sao = d_.sao[static_cast<std::size_t>(rs)];
    const SliceHeader* sh = slice < d_.slices.size() ? &d_.slices[slice] : nullptr;
    // SAO vectors: 3 entries when the slice applies SAO, empty otherwise (ovt_model.fbs HevcCtu).
    const bool sao_on = sh != nullptr && (sh->slice_sao_luma_flag != 0 || sh->slice_sao_chroma_flag != 0);
    flatbuffers::Offset<flatbuffers::Vector<fb::HevcSaoType>> type;
    flatbuffers::Offset<flatbuffers::Vector<int16_t>> offset;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> band;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> eo;
    if (sao_on) {
      std::vector<fb::HevcSaoType> t(3);
      std::transform(sao.SaoTypeIdx.begin(), sao.SaoTypeIdx.end(), t.begin(), [](uint8_t v) {
        return v <= 2 ? static_cast<fb::HevcSaoType>(v) : fb::HevcSaoType::HevcSaoType_Unknown;
      });
      type = ext_fbb_.CreateVector(t);
      offset = ext_fbb_.CreateVector(std::vector<int16_t>(sao.SaoOffsetVal.begin(), sao.SaoOffsetVal.end()));
      band = ext_fbb_.CreateVector(std::vector<uint8_t>(sao.SaoBandPosition.begin(), sao.SaoBandPosition.end()));
      eo = ext_fbb_.CreateVector(std::vector<uint8_t>(sao.SaoEoClass.begin(), sao.SaoEoClass.end()));
    }
    fb::HevcCtuBuilder b(ext_fbb_);
    b.add_CtbAddrInRs(static_cast<uint32_t>(rs));
    b.add_CtbAddrInTs(static_cast<uint32_t>(ts));
    b.add_slice_segment_idx(slice);
    b.add_tile_idx(tile_idx);
    b.add_end_of_slice_segment_flag(static_cast<uint8_t>(end_of_segment));
    if (sao_on) {
      b.add_SaoTypeIdx(type);
      b.add_SaoOffsetVal(offset);
      b.add_sao_band_position(band);
      b.add_sao_eo_class(eo);
    }
    return finish(b);
  }

  // The coding quadtree of H.265 7.3.8.4 from the CB sizes: a node is a CU where the CB at its origin has the
  // node's size, and splits where that CB is smaller.
  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by CtbLog2SizeY - MinCbLog2SizeY <= 3
  void quadtree(int32_t x, int32_t y, int32_t log2, uint32_t parent) {
    if (x >= fmt_.coded_w || y >= fmt_.coded_h) {
      return;
    }
    const CbInfo& cb = cb_at(x, y);
    if (cb.log2CbSize == 0) {
      return;
    }
    if (cb.log2CbSize < log2 && log2 > d_.min_cb_log2) {
      const int32_t half = 1 << (log2 - 1);
      quadtree(x, y, log2 - 1, parent);
      quadtree(x + half, y, log2 - 1, parent);
      quadtree(x, y + half, log2 - 1, parent);
      quadtree(x + half, y + half, log2 - 1, parent);
      return;
    }
    const int32_t n = 1 << log2;
    if (cb.log2CbSize != log2 || x + n > fmt_.coded_w || y + n > fmt_.coded_h) {
      ++skipped_cus;
      return;
    }
    emit_cu(CuContext{x, y, log2, &cb, pred_of(cb), static_cast<std::size_t>(d_.ctb_slice[ctb_at(x, y)])}, parent);
  }

  static uint8_t pred_of(const CbInfo& cb) {
    switch (static_cast<De265PredMode>(cb.PredMode)) {
      case De265PredMode::Intra:
        return cb.pcm_flag != 0 ? kPredPcm : kPredIntra;
      case De265PredMode::Skip:
        return kPredSkip;
      default:
        return kPredInter;
    }
  }

  void emit_cu(const CuContext& cu, uint32_t parent) {
    const int32_t n = 1 << cu.log2;
    const bool skip = cu.pred == kPredSkip;
    const bool residual = cu.pred != kPredSkip && cu.pred != kPredPcm;
    bool any_cbf = false;
    for (int32_t y = cu.y; y < cu.y + n; y += 4) {
      for (int32_t x = cu.x; x < cu.x + n; x += 4) {
        any_cbf = any_cbf || (d_.tu[cell(x, y)] & kTuNonzeroCoeff) != 0;
      }
    }
    OutBlock b;
    b.parent = static_cast<int32_t>(parent);
    b.kind = fb::BlockKind::CU;
    b.rect = {cu.x, cu.y, n, n};
    b.level = 2;  // SPEC-ASSUMPTION(#461): the depth in the Block tree (V-009); ctDepth is HevcCu.CtDepth
    b.partition_type = cu.cb->PartMode;
    b.pred = cu.pred;
    b.skip = skip;
    b.merge = skip;  // a skipped CU is merged; merge_flag of other CUs needs patch 0002
    b.qp = cu.cb->QP_Y;
    b.ext_type = "HevcCu";
    b.ext = hevc_cu(cu, any_cbf);
    const uint32_t id = add(std::move(b));
    for (int32_t y = cu.y; y < cu.y + n; y += 4) {
      for (int32_t x = cu.x; x < cu.x + n; x += 4) {
        g_.block_id[cell(x, y)] = id;
      }
    }

    const std::vector<fb::Rect> pus = pu_rects(cu.x, cu.y, n, cu.cb->PartMode);
    for (std::size_t k = 0; k < pus.size(); ++k) {
      emit_pu(cu, id, pus[k], static_cast<uint8_t>(k));
    }
    // No transform tree for skipped and PCM CUs. libde265 records the split flags only while it reads a
    // transform_tree(), so an unsplit CU larger than the largest transform (64x64) had rqt_root_cbf = 0.
    const bool unsplit_64 = cu.log2 > kMaxTbLog2 && (d_.tu[cell(cu.x, cu.y)] & 1U) == 0;
    if (residual && !unsplit_64) {
      tu_tree(cu, cu.x, cu.y, cu.log2, 0, 0, id);
    }
  }

  std::vector<uint8_t> hevc_cu(const CuContext& cu, bool any_cbf) {
    const CbInfo& cb = *cu.cb;
    const bool intra = cu.pred == kPredIntra || cu.pred == kPredPcm;
    const SliceHeader* sh = cu.slice < d_.slices.size() ? &d_.slices[cu.slice] : nullptr;
    const bool i_slice = sh == nullptr || sh->slice_type == 2;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> mode_c;
    if (cu.pred == kPredIntra && fmt_.chroma_format != 0) {
      mode_c = ext_fbb_.CreateVector(std::vector<uint8_t>{d_.intra_c[cell(cu.x, cu.y)]});
    }
    fb::HevcCuBuilder b(ext_fbb_);
    b.add_CtDepth(cb.ctDepth);
    b.add_log2CbSize(cb.log2CbSize);
    b.add_cu_transquant_bypass_flag(cb.cu_transquant_bypass);
    if (!i_slice) {  // cu_skip_flag and pred_mode_flag are absent from I slices
      b.add_cu_skip_flag(static_cast<uint8_t>(cu.pred == kPredSkip));
      if (cu.pred != kPredSkip) {
        b.add_pred_mode_flag(static_cast<uint8_t>(intra));
      }
    }
    b.add_CuPredMode(cu.pred == kPredSkip ? fb::HevcPredMode::MODE_SKIP
                                          : (intra ? fb::HevcPredMode::MODE_INTRA : fb::HevcPredMode::MODE_INTER));
    b.add_part_mode(static_cast<fb::HevcPartMode>(cb.PartMode));
    if (intra) {
      b.add_pcm_flag(cb.pcm_flag);
    }
    // SPEC-ASSUMPTION(#461): rqt_root_cbf is known to be 1 when a luma TB has coefficients; otherwise unknown without
    // patch 0002 (the chroma cbf flags are not kept), so it stays 255.
    if (any_cbf && !intra) {
      b.add_rqt_root_cbf(1);
    }
    b.add_QpY(static_cast<int8_t>(cb.QP_Y));  // -QpBdOffsetY..51 fits int8
    b.add_QpY_prime(static_cast<int8_t>(cb.QP_Y + 6 * (fmt_.bit_depth_luma - 8)));
    if (cu.pred == kPredIntra && fmt_.chroma_format != 0) {
      b.add_IntraPredModeC(mode_c);
    }
    return finish(b);
  }

  void emit_pu(const CuContext& cu, uint32_t cu_id, const fb::Rect& r, uint8_t part_idx) {
    const std::size_t c = cell(r.x(), r.y());
    const PbInfo& pb = d_.pb[c];
    const bool inter = cu.pred == kPredInter || cu.pred == kPredSkip;
    std::array<int8_t, 2> slot{-1, -1};
    if (inter) {
      for (int l = 0; l < 2; ++l) {
        const auto ul = static_cast<std::size_t>(l);
        slot.at(ul) = pb.predFlag.at(ul) != 0 ? refs_.slot(cu.slice, l, pb.refIdx.at(ul)) : int8_t{-1};
      }
    }
    OutBlock b;
    b.parent = static_cast<int32_t>(cu_id);
    b.kind = fb::BlockKind::PU;
    b.rect = r;
    b.level = 3;
    b.partition_type = part_idx;
    b.pred = cu.pred;
    b.skip = cu.pred == kPredSkip;
    b.merge = b.skip;
    b.qp = cu.cb->QP_Y;
    b.ext_type = "HevcPu";
    b.ext = hevc_pu(cu, pb, c, part_idx, inter);
    const uint32_t id = add(std::move(b));
    const auto flags = static_cast<uint16_t>(slot[0] >= 0 && slot[1] >= 0 ? 1U : 0U);
    for (int l = 0; l < 2; ++l) {
      const auto ul = static_cast<std::size_t>(l);
      if (slot.at(ul) >= 0) {
        mvs.push_back(
            Mv{id, static_cast<uint8_t>(l), slot.at(ul), r, mv16(pb.mv_x.at(ul)), mv16(pb.mv_y.at(ul)), flags});
      }
    }
  }

  std::vector<uint8_t> hevc_pu(const CuContext& cu, const PbInfo& pb, std::size_t c, uint8_t part_idx, bool inter) {
    fb::HevcPuBuilder b(ext_fbb_);
    b.add_partIdx(part_idx);
    if (cu.pred == kPredIntra && d_.intra_y[c] != kNotApplicable) {
      b.add_IntraPredModeY(d_.intra_y[c]);
    }
    if (!inter) {
      return finish(b);
    }
    if (cu.pred == kPredSkip) {
      b.add_merge_flag(1);
    }
    const bool l0 = pb.predFlag[0] != 0;
    const bool l1 = pb.predFlag[1] != 0;
    if (l0 || l1) {
      b.add_inter_pred_idc(l0 && l1 ? fb::HevcInterPredIdc::PRED_BI
                                    : (l0 ? fb::HevcInterPredIdc::PRED_L0 : fb::HevcInterPredIdc::PRED_L1));
    }
    b.add_predFlagL0(static_cast<uint8_t>(l0));
    b.add_predFlagL1(static_cast<uint8_t>(l1));
    if (l0) {
      b.add_mvL0_x(pb.mv_x[0]);
      b.add_mvL0_y(pb.mv_y[0]);
      b.add_refIdxL0(pb.refIdx[0]);
      b.add_ref_poc_l0(pb.refPOC[0]);
    }
    if (l1) {
      b.add_mvL1_x(pb.mv_x[1]);
      b.add_mvL1_y(pb.mv_y[1]);
      b.add_refIdxL1(pb.refIdx[1]);
      b.add_ref_poc_l1(pb.refPOC[1]);
    }
    return finish(b);
  }

  // The transform tree of H.265 7.3.8.8 from libde265's split flags: bit trafoDepth of the tu_info byte at a node's
  // origin is its split_transform_flag (signalled or inferred; slice.cc read_transform_tree()).
  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by kMaxTrafoDepth
  void tu_tree(const CuContext& cu, int32_t x, int32_t y, int32_t log2, int depth, uint8_t blk, uint32_t parent) {
    const uint8_t info = d_.tu[cell(x, y)];
    const bool split = log2 > 2 && depth <= kMaxTrafoDepth && (info & (1U << static_cast<unsigned>(depth))) != 0;
    // SPEC-ASSUMPTION(#461): TU_FLAG_NONZERO_COEFF is cbf_luma (libde265 sets it for cIdx 0 only).
    const bool cbf_luma = (info & kTuNonzeroCoeff) != 0;
    const bool bypass = cu.cb->cu_transquant_bypass != 0;
    const bool dst = !split && cu.pred == kPredIntra && log2 == 2 && !bypass;
    OutBlock b;
    b.parent = static_cast<int32_t>(parent);
    b.kind = fb::BlockKind::TU;
    b.rect = {x, y, 1 << log2, 1 << log2};
    b.level = static_cast<uint8_t>(blocks[parent].level + 1);
    b.partition_type = static_cast<uint8_t>(depth);
    b.qp = cu.cb->QP_Y;
    b.ext_type = "HevcTu";
    {
      fb::HevcTuBuilder t(ext_fbb_);
      t.add_trafoDepth(static_cast<uint8_t>(depth));
      t.add_log2TrafoSize(static_cast<uint8_t>(log2));
      t.add_blkIdx(blk);
      t.add_split_transform_flag(static_cast<uint8_t>(split));
      if (!split) {
        t.add_cbf_luma(static_cast<uint8_t>(cbf_luma));
        // DST-VII is applied to 4x4 intra luma TBs with coefficients (H.265 8.6.4.2 trType = 1); transform skip
        // is not known without patch 0002.
        t.add_dst_used(static_cast<uint8_t>(dst && cbf_luma));
      }
      b.ext = finish(t);
    }
    const uint32_t id = add(std::move(b));
    if (split) {
      const int32_t half = 1 << (log2 - 1);
      tu_tree(cu, x, y, log2 - 1, depth + 1, 0, id);
      tu_tree(cu, x + half, y, log2 - 1, depth + 1, 1, id);
      tu_tree(cu, x, y + half, log2 - 1, depth + 1, 2, id);
      tu_tree(cu, x + half, y + half, log2 - 1, depth + 1, 3, id);
      return;
    }
    // CellGrid.tx_type (chapter 06 §6.3.1): 3 transquant bypass, 1 DST-VII (4x4 intra luma), else 0 (DCT-II).
    const uint8_t type = bypass ? 3 : (dst ? 1 : 0);
    const int32_t n = 1 << log2;
    for (int32_t yy = y; yy < y + n; yy += 4) {
      for (int32_t xx = x; xx < x + n; xx += 4) {
        const std::size_t i = cell(xx, yy);
        g_.tx_log2[i] = static_cast<uint8_t>(log2);
        g_.tx_type[i] = type;
        g_.cbf[i] = static_cast<uint8_t>(cbf_luma);  // bit0 = cbf_luma; Cb/Cr flags need patch 0002
      }
    }
  }
};

bool usable(const BlockData& d, const PictureFormat& fmt) {
  if (!d.valid || fmt.coded_w <= 0 || fmt.coded_h <= 0 || d.ctb_log2 < 4 || d.ctb_log2 > 6 || d.min_cb_log2 < 3 ||
      d.min_cb_log2 > d.ctb_log2 || d.ctbs_w <= 0 || d.ctbs_h <= 0 || d.slices.empty()) {
    return false;
  }
  const auto cells_w = static_cast<uint32_t>((fmt.coded_w + 3) / 4);
  const auto cells_h = static_cast<uint32_t>((fmt.coded_h + 3) / 4);
  const std::size_t cells = std::size_t{cells_w} * cells_h;
  const auto ctbs = static_cast<std::size_t>(d.ctbs_w) * static_cast<std::size_t>(d.ctbs_h);
  const bool sizes =
      d.cells_w == cells_w && d.cells_h == cells_h && (int64_t{d.ctbs_w} << d.ctb_log2) >= fmt.coded_w &&
      (int64_t{d.ctbs_h} << d.ctb_log2) >= fmt.coded_h && (int64_t{d.cbs_w} << d.min_cb_log2) >= int64_t{cells_w} * 4 &&
      (int64_t{d.cbs_h} << d.min_cb_log2) >= int64_t{cells_h} * 4 &&
      d.cb.size() == static_cast<std::size_t>(d.cbs_w) * static_cast<std::size_t>(d.cbs_h) && d.pb.size() == cells &&
      d.intra_y.size() == cells && d.intra_c.size() == cells && d.tu.size() == cells && d.deblk.size() == cells &&
      d.bs_v.size() == cells && d.ctb_slice.size() == ctbs && d.ctb_addr_ts.size() == ctbs &&
      d.ctb_tile.size() == ctbs && d.sao.size() == ctbs;
  if (!sizes) {
    return false;
  }
  return std::all_of(d.ctb_slice.begin(), d.ctb_slice.end(), [&](uint16_t s) { return s < d.slices.size(); });
}

// CTB raster addresses in tile scan; empty when CtbAddrInTs is not a permutation (corrupt metadata).
std::vector<int32_t> tile_scan(const BlockData& d) {
  std::vector<int32_t> order(d.ctb_addr_ts.size(), -1);
  for (std::size_t rs = 0; rs < d.ctb_addr_ts.size(); ++rs) {
    const int32_t ts = d.ctb_addr_ts[rs];
    if (ts < 0 || static_cast<std::size_t>(ts) >= order.size() || order[static_cast<std::size_t>(ts)] >= 0) {
      return {};
    }
    order[static_cast<std::size_t>(ts)] = static_cast<int32_t>(rs);
  }
  return order;
}

flatbuffers::Offset<flatbuffers::Vector<uint8_t>> filter_params(flatbuffers::FlatBufferBuilder& fbb,
                                                                const BlockData& d) {
  flatbuffers::FlatBufferBuilder f(256);
  std::vector<uint8_t> disabled;
  std::vector<int8_t> beta;
  std::vector<int8_t> tc;
  std::vector<uint8_t> sao_luma;
  std::vector<uint8_t> sao_chroma;
  for (const SliceHeader& sh : d.slices) {
    disabled.push_back(static_cast<uint8_t>(sh.slice_deblocking_filter_disabled_flag != 0));
    beta.push_back(static_cast<int8_t>(sh.slice_beta_offset_div2));
    tc.push_back(static_cast<int8_t>(sh.slice_tc_offset_div2));
    sao_luma.push_back(static_cast<uint8_t>(sh.slice_sao_luma_flag != 0));
    sao_chroma.push_back(static_cast<uint8_t>(sh.slice_sao_chroma_flag != 0));
  }
  // SPEC-ASSUMPTION(#461): only the per-slice-segment vectors; the PPS/SPS flags of HevcFilterParams are not in the
  // patch-0001 slice header table and stay at their defaults, and edge_flags_v/h stay empty (per-cell bS is in the
  // grid).
  const auto disabled_v = f.CreateVector(disabled);
  const auto beta_v = f.CreateVector(beta);
  const auto tc_v = f.CreateVector(tc);
  const auto sao_luma_v = f.CreateVector(sao_luma);
  const auto sao_chroma_v = f.CreateVector(sao_chroma);
  fb::HevcFilterParamsBuilder p(f);
  p.add_slice_deblocking_filter_disabled_flag(disabled_v);
  p.add_slice_beta_offset_div2(beta_v);
  p.add_slice_tc_offset_div2(tc_v);
  p.add_slice_sao_luma_flag(sao_luma_v);
  p.add_slice_sao_chroma_flag(sao_chroma_v);
  f.Finish(p.Finish());
  return fbb.CreateVector(f.GetBufferPointer(), f.GetSize());
}

fb::Rect ctb_box(const BlockData& d, const PictureFormat& fmt, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  const int32_t left = x0 << d.ctb_log2;
  const int32_t top = y0 << d.ctb_log2;
  return {left, top, std::min((x1 + 1) << d.ctb_log2, fmt.coded_w) - left,
          std::min((y1 + 1) << d.ctb_log2, fmt.coded_h) - top};
}

// One PartitionInfo per slice segment (index = libde265's slice header index, decode order), then one per tile
// when the picture has more than one. The host fills unit_index, file_span, the bit positions and the header from
// its index (chapter 07 §7.5.3 A6).
std::vector<flatbuffers::Offset<fb::PartitionInfo>> partitions(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d,
                                                               const PictureFormat& fmt,
                                                               const std::vector<SegmentSpan>& seg,
                                                               const std::vector<SegmentSpan>& tiles) {
  std::vector<flatbuffers::Offset<fb::PartitionInfo>> out;
  for (std::size_t s = 0; s < d.slices.size(); ++s) {
    const SliceHeader& sh = d.slices[s];
    const SegmentSpan& sp = seg[s];
    // A dependent slice segment starts after the first CTB of its slice (SliceAddrRs, H.265 7.4.7.1).
    const bool dependent = sp.count > 0 && sp.first_rs != sh.SliceAddrRs;
    int16_t parent = -1;
    for (std::size_t t = 0; dependent && t < s; ++t) {
      if (seg[t].count > 0 && seg[t].first_rs == sh.SliceAddrRs) {
        parent = static_cast<int16_t>(t);
      }
    }
    fb::PartitionInfoBuilder p(fbb);
    p.add_kind(dependent ? fb::PartitionKind::SliceSegment : fb::PartitionKind::Slice);
    p.add_index(static_cast<uint16_t>(s));
    p.add_parent(parent);
    p.add_slice_type(static_cast<uint8_t>(sh.slice_type));
    if (sp.count > 0) {
      const fb::Rect rect = ctb_box(d, fmt, sp.x0, sp.y0, sp.x1, sp.y1);
      p.add_rect(&rect);
      p.add_first_block_addr(static_cast<uint32_t>(sp.first_rs));
    }
    p.add_block_count(sp.count);
    p.add_qp(static_cast<int16_t>(sh.SliceQPY));
    p.add_qp_delta(static_cast<int16_t>(sh.slice_qp_delta));
    p.add_deblocking_disable(static_cast<uint8_t>(sh.slice_deblocking_filter_disabled_flag != 0));
    p.add_deblock_alpha_or_beta_offset_div2(static_cast<int8_t>(sh.slice_beta_offset_div2));
    p.add_deblock_beta_or_tc_offset_div2(static_cast<int8_t>(sh.slice_tc_offset_div2));
    p.add_sao_luma(static_cast<uint8_t>(sh.slice_sao_luma_flag != 0));
    p.add_sao_chroma(static_cast<uint8_t>(sh.slice_sao_chroma_flag != 0));
    if (sh.slice_type != 2) {
      p.add_max_num_merge_cand(static_cast<uint8_t>(sh.MaxNumMergeCand));
    }
    out.push_back(p.Finish());
  }
  if (tiles.size() < 2) {
    return out;
  }
  // TileId runs over the tile grid in raster order (H.265 6.5.1), so the column of a tile is the rank of its left
  // edge among the distinct left edges, and likewise for rows.
  std::vector<int32_t> xs;
  std::vector<int32_t> ys;
  for (const SegmentSpan& t : tiles) {
    xs.push_back(t.x0);
    ys.push_back(t.y0);
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
  for (std::size_t t = 0; t < tiles.size(); ++t) {
    const SegmentSpan& sp = tiles[t];
    const fb::Rect rect = ctb_box(d, fmt, sp.x0, sp.y0, sp.x1, sp.y1);
    fb::PartitionInfoBuilder p(fbb);
    p.add_kind(fb::PartitionKind::Tile);
    p.add_index(static_cast<uint16_t>(d.slices.size() + t));
    p.add_rect(&rect);
    p.add_first_block_addr(static_cast<uint32_t>(sp.first_rs));
    p.add_block_count(sp.count);
    p.add_tile_row(static_cast<uint16_t>(std::lower_bound(ys.begin(), ys.end(), sp.y0) - ys.begin()));
    p.add_tile_col(static_cast<uint16_t>(std::lower_bound(xs.begin(), xs.end(), sp.x0) - xs.begin()));
    out.push_back(p.Finish());
  }
  return out;
}

void serialise(flatbuffers::FlatBufferBuilder& fbb, const Mapper& mp, MappedBlocks* out) {
  std::vector<flatbuffers::Offset<fb::Block>> blocks;
  blocks.reserve(mp.blocks.size());
  for (std::size_t id = 0; id < mp.blocks.size(); ++id) {
    const OutBlock& b = mp.blocks[id];
    blocks.push_back(fb::CreateBlockDirect(fbb, static_cast<uint32_t>(id), b.parent, &b.children, b.kind, &b.rect,
                                           b.level, b.partition_type, static_cast<fb::PredKind>(b.pred), b.skip,
                                           b.merge, b.qp, /*bits_x8=*/0, b.ext_type, b.ext.empty() ? nullptr : &b.ext));
  }
  out->blocks = fbb.CreateVector(blocks);
  std::vector<flatbuffers::Offset<fb::MotionVector>> mvs;
  mvs.reserve(mp.mvs.size());
  for (const auto& m : mp.mvs) {
    // predictor "?": merge_flag, merge_idx and mvp_lX_flag need patch 0002 (chapter 07 §7.6.2).
    mvs.push_back(fb::CreateMotionVectorDirect(fbb, m.block_id, m.list, m.ref, &m.rect, m.x, m.y, kMvdUnavailable,
                                               kMvdUnavailable, "?", m.flags));
  }
  out->mvs = fbb.CreateVector(mvs);
}

}  // namespace

MappedBlocks map_blocks(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, const PictureFormat& fmt) {
  MappedBlocks out;
  const auto cells_w = static_cast<uint32_t>((fmt.coded_w + 3) / 4);
  const auto cells_h = static_cast<uint32_t>((fmt.coded_h + 3) / 4);
  std::vector<int32_t> order;
  if (usable(d, fmt)) {
    order = tile_scan(d);
  }
  if (order.empty()) {
    if (d.valid) {
      out.warnings.emplace_back("libde265 block metadata does not match the picture; no block data for this frame");
    }
    out.grid = fb::CreateCellGrid(fbb, cells_w, cells_h);
    return out;
  }

  // Slice segment and tile extents, and the last CTB of each segment in tile scan (end_of_slice_segment_flag).
  std::vector<SegmentSpan> seg(d.slices.size());
  int32_t max_tile = 0;
  for (const int32_t t : d.ctb_tile) {
    max_tile = std::max(max_tile, t);
  }
  std::vector<SegmentSpan> tiles(max_tile > 0 && max_tile < 0xFFFF ? static_cast<std::size_t>(max_tile) + 1 : 0);
  for (std::size_t rs = 0; rs < order.size(); ++rs) {
    const auto cx = static_cast<int32_t>(rs % static_cast<std::size_t>(d.ctbs_w));
    const auto cy = static_cast<int32_t>(rs / static_cast<std::size_t>(d.ctbs_w));
    const auto r = static_cast<int32_t>(rs);
    const int32_t x0 = cx << d.ctb_log2;
    const int32_t y0 = cy << d.ctb_log2;
    const auto& cb = d.cb[static_cast<std::size_t>(y0 >> d.min_cb_log2) * static_cast<std::size_t>(d.cbs_w) +
                          static_cast<std::size_t>(x0 >> d.min_cb_log2)];
    if (cb.log2CbSize != 0) {
      seg[d.ctb_slice[rs]].add(d.ctb_addr_ts[rs], r, cx, cy);
    }
    if (!tiles.empty() && d.ctb_tile[rs] >= 0) {
      tiles[static_cast<std::size_t>(d.ctb_tile[rs])].add(d.ctb_addr_ts[rs], r, cx, cy);
    }
  }

  GridCells g(std::size_t{cells_w} * cells_h);
  RefTable refs(d.slices);
  Mapper mp(d, fmt, g, refs);
  OutBlock root;
  root.rect = {0, 0, fmt.coded_w, fmt.coded_h};
  mp.add(std::move(root));
  for (const int32_t rs : order) {
    const auto ctb = static_cast<std::size_t>(rs);
    const uint16_t slice = d.ctb_slice[ctb];
    const auto tile_idx = tiles.empty()
                              ? uint16_t{0xFFFF}
                              : static_cast<uint16_t>(d.slices.size() + static_cast<std::size_t>(d.ctb_tile[ctb]));
    mp.emit_ctu(rs, tile_idx, seg[slice].last_ts == d.ctb_addr_ts[ctb]);
  }
  fill_cells(g, d, fmt, refs);
  out.mv_clamped_cells = g.mv_clamped_cells;

  const auto uncovered = std::count(g.block_id.begin(), g.block_id.end(), 0U);
  if (uncovered > 0) {
    out.warnings.push_back(std::to_string(uncovered) + " cells were not decoded (no coding unit)");
  }
  if (mp.skipped_cus > 0) {
    out.warnings.push_back(std::to_string(mp.skipped_cus) +
                           " coding blocks do not fit the coding quadtree of their CTU; left out");
  }

  serialise(fbb, mp, &out);
  out.refs = build_refs(fbb, refs);
  out.partitions = fbb.CreateVector(partitions(fbb, d, fmt, seg, tiles));
  out.filter_params = filter_params(fbb, d);
  out.qp_hist = build_qp_hist(fbb, g);
  out.grid = build_grid(fbb, cells_w, cells_h, g);
  return out;
}

}  // namespace ovt::de265
