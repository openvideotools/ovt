// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// insp_mi_data -> CellGrid / Block tree / MotionVector / RefSlot / tiles (chapter 07 §7.6.1 mapping table rows
// except accounting and the extension-patch fields; WI-2.156 steps 3 to 6).

#include "map_mi.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "enums.h"
#include "map_mi_ext.hpp"
#include "partition_reconstruct.hpp"

namespace ovt::aom {

namespace {

namespace fb = ovt::fb;

constexpr uint8_t kNotApplicable = 0xFF;
constexpr uint8_t kCdefNotSignalled = 0xFE;  // cdef_idx -1 (chapter 06 §6.3.1 CellGrid.filter_idx)
constexpr int32_t kCdefUnitMi = 16;          // CDEF works on 64x64 units whatever the superblock size

bool is_inter(const MiInfo& m) {
  return m.intrabc == 0 && m.ref_frame[0] > 0;
}

uint8_t pred_kind(const MiInfo& m) {
  if (m.intrabc != 0) {
    return kPredIntraBc;
  }
  if (m.ref_frame[0] > 0) {
    // skip_mode comes only with extension patch 0001; stock inspection data reads skip_mode blocks as Inter (§6.3.1).
    return m.skip_mode > 0 ? kPredSkip : kPredInter;
  }
  return m.palette > 0 || m.uv_palette > 0 ? kPredPalette : kPredIntra;
}

int16_t intra_mode_y(const MiInfo& m) {
  if (m.intrabc != 0) {
    return kIntraModeIntraBc;
  }
  if (is_inter(m) || m.mode < 0 || m.mode >= kIntraModes) {
    return -1;
  }
  return m.palette > 0 ? kIntraModePalette : m.mode;
}

int16_t intra_mode_c(const MiInfo& m, bool monochrome) {
  if (monochrome || m.intrabc != 0 || is_inter(m) || m.uv_mode < 0 || m.uv_mode >= kUvIntraModes) {
    return -1;
  }
  return m.uv_mode;
}

// Global or warped motion of an inter block (MotionVector.flags bit 2; Av1Block.is_global is the first part).
bool is_global(const MiInfo& m, const BlockData& d) {
  if (m.mode != kGlobalMv && m.mode != kGlobalGlobalMv) {
    return false;
  }
  const auto ref = static_cast<size_t>(std::clamp<int>(m.ref_frame[0], 0, 7));
  return d.gm_type.at(ref) > kGmTranslation;
}

// CfL alphas of AV1 §5.11.45 from cfl_alpha_signs and the packed cfl_alpha_idx (u << 4 | v).
std::array<int8_t, 2> cfl_alphas(const MiInfo& m) {
  const int signs = m.cfl_alpha_sign;
  const std::array<int, 2> sign = {(signs + 1) / 3, (signs + 1) % 3};  // 0 ZERO, 1 NEG, 2 POS
  const std::array<int, 2> mag = {((m.cfl_alpha_idx >> 4) & 15) + 1, (m.cfl_alpha_idx & 15) + 1};
  std::array<int8_t, 2> out{};
  for (size_t p = 0; p < 2; ++p) {
    out.at(p) = static_cast<int8_t>(sign.at(p) == 0 ? 0 : (sign.at(p) == 1 ? -mag.at(p) : mag.at(p)));
  }
  return out;
}

// cdef_idx of every 64x64 CDEF unit (raster over the MI grid). libaom keeps the strength only in the block at the
// unit's origin and ifd_inspect() reports it as (level, strength), so the index is found by matching against the
// frame's strength table: 0xFF when two entries match, 0xFE when the unit was not signalled (all blocks skip, or
// CDEF off).
// SPEC-ASSUMPTION(#360): the per-unit reading (origin MI) and the all-skip rule are not in §7.6.1.
// True when some MI of the CDEF unit at MI (r0, c0) is not skipped, i.e. the unit's cdef_idx was signalled.
bool cdef_unit_coded(const BlockData& d, int32_t r0, int32_t c0) {
  const int32_t r_end = std::min(r0 + kCdefUnitMi, d.mi_rows);
  const int32_t c_end = std::min(c0 + kCdefUnitMi, d.mi_cols);
  for (int32_t r = r0; r < r_end; ++r) {
    for (int32_t c = c0; c < c_end; ++c) {
      if (d.mi[static_cast<size_t>(r) * static_cast<size_t>(d.mi_cols) + static_cast<size_t>(c)].skip == 0) {
        return true;
      }
    }
  }
  return false;
}

// Index of the cdef_strengths entry equal to (level, strength); kNotApplicable when none or several match.
uint8_t match_cdef_strength(const BlockData& d, const MiInfo& o) {
  int match = -1;
  for (size_t i = 0; i < d.cdef_strengths.size(); ++i) {
    const int s = d.cdef_strengths[i];
    const int sec = s % 4 == 3 ? 4 : s % 4;
    if (s / 4 == o.cdef_level && sec == o.cdef_strength) {
      match = match < 0 ? static_cast<int>(i) : -2;
    }
  }
  return match >= 0 ? static_cast<uint8_t>(match) : kNotApplicable;
}

std::vector<uint8_t> cdef_unit_indices(const BlockData& d, bool enable_cdef, int32_t units_w, int32_t units_h) {
  std::vector<uint8_t> out(static_cast<size_t>(units_w) * static_cast<size_t>(units_h), kCdefNotSignalled);
  if (!enable_cdef) {
    return out;
  }
  for (int32_t uy = 0; uy < units_h; ++uy) {
    for (int32_t ux = 0; ux < units_w; ++ux) {
      const int32_t r0 = uy * kCdefUnitMi;
      const int32_t c0 = ux * kCdefUnitMi;
      if (cdef_unit_coded(d, r0, c0)) {
        const MiInfo& o = d.mi[static_cast<size_t>(r0) * static_cast<size_t>(d.mi_cols) + static_cast<size_t>(c0)];
        // With extension patch 0001 the unit's cdef_idx is read directly; stock data needs the strength match.
        out[static_cast<size_t>(uy) * static_cast<size_t>(units_w) + static_cast<size_t>(ux)] =
            d.ext ? ext_cdef_idx(o, d.cdef_strengths.size()) : match_cdef_strength(d, o);
      }
    }
  }
  return out;
}

// Rect in luma samples of the MI rectangle at (r, c) of w x h MIs.
fb::Rect rect_of(int32_t r, int32_t c, int32_t w, int32_t h) {
  return {c * 4, r * 4, w * 4, h * 4};
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

struct Mapper {
  Mapper(const BlockData& data, uint32_t w, uint32_t h) : d(data), cells_w(w), cells_h(h) {}
  const BlockData& d;
  uint32_t cells_w;
  uint32_t cells_h;
  int32_t cdef_units_w = 0;
  std::vector<uint8_t> cdef_idx;  // per 64x64 unit
  flatbuffers::FlatBufferBuilder ext_fbb{256};
  std::vector<OutBlock> blocks;
  std::vector<uint32_t> block_id;      // per cell
  std::vector<uint16_t> partition_id;  // per cell
  struct Mv {
    uint32_t block_id;
    uint8_t list;
    int8_t ref;
    fb::Rect rect;
    int16_t x;
    int16_t y;
    Mvd16 mvd;
    std::string predictor;
    uint16_t flags;
  };
  uint32_t partition_mismatch = 0;  // leaves whose extension `partition` disagrees with the reconstructed tree
  std::vector<Mv> mvs;
  uint32_t ambiguous = 0;
  std::string first_ambiguous;

  [[nodiscard]] const MiInfo& mi(int32_t r, int32_t c) const {
    return d.mi[static_cast<size_t>(r) * static_cast<size_t>(d.mi_cols) + static_cast<size_t>(c)];
  }

  [[nodiscard]] uint8_t cdef_at(int32_t r, int32_t c) const {
    return cdef_idx[static_cast<size_t>(r / kCdefUnitMi) * static_cast<size_t>(cdef_units_w) +
                    static_cast<size_t>(c / kCdefUnitMi)];
  }

  uint32_t add(OutBlock b) {
    const auto id = static_cast<uint32_t>(blocks.size());
    if (b.parent >= 0) {
      blocks[static_cast<size_t>(b.parent)].children.push_back(id);
    }
    blocks.push_back(std::move(b));
    return id;
  }

  std::vector<uint8_t> finish_ext() {
    const std::span<const uint8_t> buf(ext_fbb.GetBufferPointer(), ext_fbb.GetSize());
    std::vector<uint8_t> out(buf.begin(), buf.end());
    ext_fbb.Clear();
    return out;
  }

  // Av1Block of a partition node (a square that was split) or, with @p leaf, of a coded block.
  std::vector<uint8_t> av1_block(const PartNode& n, bool leaf, uint16_t tile) {
    const MiInfo& m = mi(n.mi_row, n.mi_col);
    const bool inter = is_inter(m);
    flatbuffers::Offset<flatbuffers::Vector<int8_t>> ref_frame;
    flatbuffers::Offset<flatbuffers::Vector<int16_t>> mv;
    flatbuffers::Offset<flatbuffers::Vector<fb::Av1InterpFilter>> interp;
    Av1BlockExt ext;
    if (leaf && d.ext) {
      ext = make_block_ext(ext_fbb, m, inter, d.filters.delta_lf_present);
    }
    if (leaf) {
      ref_frame = ext_fbb.CreateVector(
          std::vector<int8_t>{static_cast<int8_t>(m.ref_frame[0]), static_cast<int8_t>(m.ref_frame[1])});
      if (inter) {
        mv = ext_fbb.CreateVector(std::vector<int16_t>{m.mv_row[0], m.mv_col[0], m.mv_row[1], m.mv_col[1]});
        interp = ext_fbb.CreateVector(std::vector<fb::Av1InterpFilter>{static_cast<fb::Av1InterpFilter>(m.filter[0]),
                                                                       static_cast<fb::Av1InterpFilter>(m.filter[1])});
      }
    }
    fb::Av1BlockBuilder b(ext_fbb);
    b.add_MiRow(static_cast<uint32_t>(n.mi_row));
    b.add_MiCol(static_cast<uint32_t>(n.mi_col));
    b.add_bsize(static_cast<fb::Av1BlockSize>(n.bsize));
    b.add_partition(static_cast<fb::Av1Partition>(n.partition));
    b.add_is_superblock(n.level == 0 ? 1 : 0);
    b.add_tile_idx(tile);
    if (!leaf) {
      return finish(b);
    }
    b.add_skip(static_cast<uint8_t>(m.skip != 0));
    b.add_segment_id(static_cast<uint8_t>(m.segment_id));
    b.add_is_inter(static_cast<uint8_t>(inter));
    b.add_use_intrabc(static_cast<uint8_t>(m.intrabc != 0));
    b.add_CurrentQIndex(m.current_qindex);
    if (!d.delta_q_present && m.segment_id >= 0 && m.segment_id < 8) {  // the segment's quantizer is the block's
      const auto& q = d.y_dequant.at(static_cast<size_t>(m.segment_id));
      b.add_qindex_dc_y(q[0]);
      b.add_qindex_ac_y(q[1]);
    }
    if (m.mode >= 0 && m.mode < kNumModes) {
      b.add_YMode(static_cast<fb::Av1YMode>(m.mode));
    }
    if (!inter && m.intrabc == 0) {
      add_intra_fields(b, m);
    }
    if (m.intrabc != 0) {
      b.add_use_intrabc_mv_x(m.mv_col[0]);
      b.add_use_intrabc_mv_y(m.mv_row[0]);
    }
    b.add_RefFrame(ref_frame);
    if (inter) {
      b.add_Mv(mv);
      b.add_interp_filter(interp);
      add_inter_fields(b, m);
    }
    if (m.tx_size >= 0 && m.tx_size < kNumTxSizes) {
      b.add_TxSize(static_cast<fb::Av1TxSize>(m.tx_size));
    }
    const uint8_t cdef = cdef_at(n.mi_row, n.mi_col);
    b.add_cdef_idx(cdef == kCdefNotSignalled || cdef == kNotApplicable ? int8_t{-1} : static_cast<int8_t>(cdef));
    if (d.ext) {
      add_block_ext(b, m, ext, inter);
    }
    return finish(b);
  }

  static void add_intra_fields(fb::Av1BlockBuilder& b, const MiInfo& m) {
    if (m.uv_mode >= 0 && m.uv_mode < kUvIntraModes) {
      b.add_UVMode(static_cast<uint8_t>(m.uv_mode));
    }
    if (m.uv_mode == kUvCflPred) {
      const auto a = cfl_alphas(m);
      b.add_CflAlphaU(a[0]);
      b.add_CflAlphaV(a[1]);
      b.add_cfl_alpha_signs(static_cast<uint8_t>(m.cfl_alpha_sign));
    }
    b.add_PaletteSizeY(static_cast<uint8_t>(m.palette));
    b.add_PaletteSizeUV(static_cast<uint8_t>(m.uv_palette));
  }

  void add_inter_fields(fb::Av1BlockBuilder& b, const MiInfo& m) const {
    const bool compound = m.ref_frame[1] > 0;
    const bool interintra = m.ref_frame[1] == 0;
    b.add_comp_mode(static_cast<uint8_t>(compound));
    b.add_interintra(static_cast<uint8_t>(interintra));
    b.add_motion_mode(static_cast<fb::Av1MotionMode>(m.motion_mode));
    if (compound && m.compound_type >= 0 && m.compound_type < 4) {
      b.add_compound_type(
          static_cast<fb::Av1CompoundType>(kCompoundTypeToSpec.at(static_cast<size_t>(m.compound_type))));
    } else if (interintra) {
      b.add_compound_type(static_cast<fb::Av1CompoundType>(kCompoundIntra));
    }
    b.add_is_global(static_cast<uint8_t>(is_global(m, d)));
  }

  std::vector<uint8_t> finish(fb::Av1BlockBuilder& b) {
    ext_fbb.Finish(b.Finish());
    return finish_ext();
  }

  // Luma TXBs of a leaf, in raster order of their origins: the MIs of the block are grouped by the tx_size
  // ifd_inspect() reports per MI (per transform unit for inter and IntraBC blocks, via inter_tx_size[]).
  void add_txbs(uint32_t leaf_id, const PartNode& n) {
    const int32_t r_end = std::min(n.mi_row + n.mi_h, d.mi_rows);
    const int32_t c_end = std::min(n.mi_col + n.mi_w, d.mi_cols);
    std::vector<bool> done(static_cast<size_t>(n.mi_w) * static_cast<size_t>(n.mi_h), false);
    for (int32_t r = n.mi_row; r < r_end; ++r) {
      for (int32_t c = n.mi_col; c < c_end; ++c) {
        const auto at =
            static_cast<size_t>(r - n.mi_row) * static_cast<size_t>(n.mi_w) + static_cast<size_t>(c - n.mi_col);
        if (done[at]) {
          continue;
        }
        const int16_t tx = mi(r, c).tx_size;
        if (tx < 0 || tx >= kNumTxSizes) {
          return;  // no transform information: no TXBs for this block
        }
        const MiSize lg = kTxLog2.at(static_cast<size_t>(tx));
        const int32_t tw = std::min((1 << lg.w) / 4, n.mi_col + n.mi_w - c);
        const int32_t th = std::min((1 << lg.h) / 4, n.mi_row + n.mi_h - r);
        for (int32_t y = 0; y < th; ++y) {
          for (int32_t x = 0; x < tw; ++x) {
            done[at + static_cast<size_t>(y) * static_cast<size_t>(n.mi_w) + static_cast<size_t>(x)] = true;
          }
        }
        fb::Av1TxbBuilder t(ext_fbb);
        t.add_plane(0);
        t.add_startX(static_cast<uint32_t>(c * 4));
        t.add_startY(static_cast<uint32_t>(r * 4));
        t.add_txSz(static_cast<fb::Av1TxSize>(tx));
        if (d.ext && grid_tx_type(mi(r, c)) != kNotApplicable) {
          t.add_TxType(static_cast<fb::Av1TxType>(grid_tx_type(mi(r, c))));
        }
        ext_fbb.Finish(t.Finish());
        OutBlock txb;
        txb.parent = static_cast<int32_t>(leaf_id);
        txb.kind = fb::BlockKind::TXB;
        txb.rect = rect_of(r, c, tw, th);
        txb.level = static_cast<uint8_t>(blocks[leaf_id].level + 1);
        txb.partition_type = 0;  // plane Y
        txb.ext_type = "Av1Txb";
        txb.ext = finish_ext();
        add(std::move(txb));
      }
    }
  }

  void add_mvs(uint32_t leaf_id, const PartNode& n, const MiInfo& m) {
    if (!is_inter(m)) {
      return;  // IntraBC vectors stay in grid.mv0 and Av1Block: a MotionVector needs a RefSlot (V-015, V-026)
    }
    std::string predictor(m.mode >= 0 && m.mode < kNumModes ? kModeNames.at(static_cast<size_t>(m.mode)) : "?");
    const bool global = is_global(m, d);
    if (global) {
      predictor += "+global";
    }
    if (m.motion_mode == kMotionWarped) {
      predictor += "+warp";
    } else if (m.motion_mode == kMotionObmc) {
      predictor += "+obmc";
    }
    const bool compound = m.ref_frame[1] > 0;
    uint16_t flags = compound ? 1U : 0U;
    if (global || m.motion_mode == kMotionWarped) {
      flags |= 1U << 2U;
    }
    if (m.motion_mode == kMotionObmc) {
      flags |= 1U << 3U;
    }
    const size_t lists = compound ? 2 : 1;
    for (size_t list = 0; list < lists; ++list) {
      mvs.push_back(Mv{leaf_id, static_cast<uint8_t>(list), static_cast<int8_t>(m.ref_frame.at(list)),
                       rect_of(n.mi_row, n.mi_col, n.mi_w, n.mi_h), static_cast<int16_t>(m.mv_col.at(list) * 2),
                       static_cast<int16_t>(m.mv_row.at(list) * 2), d.ext ? mvd_of(m, static_cast<int>(list)) : Mvd16{},
                       predictor, flags});
    }
  }

  // Emits node @p i of @p nodes (and its subtree) under @p parent, pre-order.
  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by the partition tree (at most 6 levels below a 128x128 SB)
  void emit(const std::vector<PartNode>& nodes, uint32_t i, int32_t parent, uint16_t tile) {
    const PartNode& n = nodes[i];
    const bool leaf = n.children.empty() && n.partition == 0;
    OutBlock b;
    b.parent = parent;
    b.kind = n.level == 0 ? fb::BlockKind::SB : fb::BlockKind::Block;
    b.rect = rect_of(n.mi_row, n.mi_col, n.mi_w, n.mi_h);
    b.level = static_cast<uint8_t>(n.level + 1);
    b.partition_type = n.partition;
    b.ext_type = "Av1Block";
    if (n.ambiguous && ambiguous++ == 0) {
      first_ambiguous = "(" + std::to_string(n.mi_row) + ", " + std::to_string(n.mi_col) + ")";
    }
    const MiInfo& m = mi(n.mi_row, n.mi_col);
    // Extension patch 0001 gives each block the partition type libaom decoded it with: PARTITION_NONE for a square
    // read as NONE (a SPLIT quadrant, 4x4 included, or the whole superblock), else its parent's type. A
    // disagreement means the reconstructed tree is wrong; report it.
    if (leaf && d.ext && m.partition >= 0) {
      uint8_t expected = 0;
      if (n.parent >= 0) {
        const PartNode& p = nodes[static_cast<size_t>(n.parent)];
        expected = p.partition == static_cast<uint8_t>(Partition::Split) ? uint8_t{0} : p.partition;
      }
      partition_mismatch += static_cast<uint8_t>(m.partition) != expected ? 1U : 0U;
    }
    if (leaf) {
      b.pred = pred_kind(m);
      b.skip = m.skip != 0;
      b.merge = is_inter(m) && is_predicted_mv_mode(m.mode);
      b.qp = m.current_qindex;
    }
    b.ext = av1_block(n, leaf, tile);
    const uint32_t id = add(std::move(b));
    if (!leaf) {
      for (const uint32_t child : n.children) {
        emit(nodes, child, static_cast<int32_t>(id), tile);
      }
      return;
    }
    const auto y_end = std::min<uint32_t>(static_cast<uint32_t>(n.mi_row + n.mi_h), cells_h);
    const auto x_end = std::min<uint32_t>(static_cast<uint32_t>(n.mi_col + n.mi_w), cells_w);
    for (auto y = static_cast<uint32_t>(n.mi_row); y < y_end; ++y) {
      for (auto x = static_cast<uint32_t>(n.mi_col); x < x_end; ++x) {
        block_id[size_t{y} * cells_w + x] = id;
        partition_id[size_t{y} * cells_w + x] = tile;
      }
    }
    add_mvs(id, n, m);
    add_txbs(id, n);
  }
};

template <typename T>
flatbuffers::Offset<flatbuffers::Vector<T>> vec(flatbuffers::FlatBufferBuilder& fbb, const std::vector<T>& v) {
  return fbb.CreateVector(v);
}

flatbuffers::Offset<flatbuffers::Vector<uint8_t>> filter_params(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d,
                                                                bool enable_cdef, int32_t frame_h) {
  flatbuffers::FlatBufferBuilder f(512);
  std::vector<uint8_t> y_pri;
  std::vector<uint8_t> y_sec;
  std::vector<uint8_t> uv_pri;
  std::vector<uint8_t> uv_sec;
  for (const int32_t s : d.cdef_strengths) {
    y_pri.push_back(static_cast<uint8_t>(s / 4));
    y_sec.push_back(static_cast<uint8_t>(s % 4));
  }
  for (const int32_t s : d.cdef_uv_strengths) {
    uv_pri.push_back(static_cast<uint8_t>(s / 4));
    uv_sec.push_back(static_cast<uint8_t>(s % 4));
  }
  const auto cdef =
      fb::CreateAv1CdefParamsDirect(f, static_cast<uint8_t>(d.cdef_damping - 3), static_cast<uint8_t>(d.cdef_bits),
                                    static_cast<uint8_t>(d.cdef_damping), &y_pri, &y_sec, &uv_pri, &uv_sec);
  std::vector<flatbuffers::Offset<fb::Av1GmParams>> gm;
  if (!d.intra_frame) {
    for (size_t t = 1; t < 8; ++t) {
      const std::vector<int32_t> params(d.gm_params.at(t).begin(), d.gm_params.at(t).end());
      gm.push_back(fb::CreateAv1GmParamsDirect(f, static_cast<fb::Av1RefFrame>(t),
                                               static_cast<fb::Av1GmType>(d.gm_type.at(t)), &params));
    }
  }
  const auto gm_v = f.CreateVector(gm);
  const FilterParamsExt ext = d.ext ? make_filter_ext(f, d, frame_h) : FilterParamsExt{};
  uint8_t res_log2 = 0;
  while ((1 << res_log2) < d.delta_q_res && res_log2 < 3) {
    ++res_log2;
  }
  fb::Av1FilterParamsBuilder p(f);
  p.add_enable_cdef(static_cast<uint8_t>(enable_cdef));
  p.add_cdef(cdef);
  // Super-resolution (AV1 §5.9.8, WI-2.158): the Reconstruction planes are UpscaledWidth wide while the grid
  // covers FrameWidth; the renderer stretches overlays by UpscaledWidth / FrameWidth (chapter 06 V-023).
  constexpr int32_t kSuperresNum = 8;
  constexpr int32_t kSuperresDenomMin = 9;
  const bool use_superres = d.superres_denom != kSuperresNum;
  p.add_use_superres(static_cast<uint8_t>(use_superres));
  if (use_superres) {
    p.add_coded_denom(static_cast<uint8_t>(d.superres_denom - kSuperresDenomMin));
    p.add_SuperresDenom(static_cast<uint8_t>(d.superres_denom));
  }
  p.add_FrameWidth(static_cast<uint32_t>(d.frame_width));
  p.add_UpscaledWidth(static_cast<uint32_t>(d.upscaled_width));
  p.add_base_q_idx(static_cast<uint8_t>(d.base_qindex));
  p.add_delta_q_present(static_cast<uint8_t>(d.delta_q_present));
  p.add_delta_q_res(d.delta_q_present ? res_log2 : uint8_t{0});
  p.add_gm(gm_v);
  if (d.ext) {
    add_filter_ext(p, d, ext);
  }
  f.Finish(p.Finish());
  return fbb.CreateVector(f.GetBufferPointer(), f.GetSize());
}

bool block_data_usable(const BlockData& d, uint32_t cells_w, uint32_t cells_h) {
  return d.valid && d.mi_rows > 0 && d.mi_cols > 0 && cells_w <= static_cast<uint32_t>(d.mi_cols) &&
         cells_h <= static_cast<uint32_t>(d.mi_rows) &&
         d.mi.size() == static_cast<size_t>(d.mi_rows) * static_cast<size_t>(d.mi_cols) &&
         (d.sb_mi == 16 || d.sb_mi == 32) && d.tile_col_starts.size() >= 2 && d.tile_row_starts.size() >= 2;
}

// Emits the superblocks tile by tile (tile rows, then columns), raster within each tile (§6.3.2 rule 1), and
// returns the tiles' PartitionInfo entries.
std::vector<flatbuffers::Offset<fb::PartitionInfo>> emit_tiles(flatbuffers::FlatBufferBuilder& fbb, Mapper& mp,
                                                               int32_t coded_w, int32_t coded_h) {
  const BlockData& d = mp.d;
  std::vector<uint8_t> bsize(d.mi.size());
  std::transform(d.mi.begin(), d.mi.end(), bsize.begin(), [](const MiInfo& m) {
    return m.bsize >= 0 && m.bsize < kNumBlockSizes ? static_cast<uint8_t>(m.bsize) : uint8_t{255};
  });
  const BsizeGrid grid{bsize, d.mi_rows, d.mi_cols};
  const size_t tile_cols = d.tile_col_starts.size() - 1;
  const size_t tile_rows = d.tile_row_starts.size() - 1;
  std::vector<flatbuffers::Offset<fb::PartitionInfo>> tiles;
  std::vector<PartNode> nodes;
  for (size_t tr = 0; tr < tile_rows; ++tr) {
    for (size_t tc = 0; tc < tile_cols; ++tc) {
      const auto tile = static_cast<uint16_t>(tr * tile_cols + tc);
      const int32_t r0 = d.tile_row_starts[tr];
      const int32_t r1 = std::min(d.tile_row_starts[tr + 1], d.mi_rows);
      const int32_t c0 = d.tile_col_starts[tc];
      const int32_t c1 = std::min(d.tile_col_starts[tc + 1], d.mi_cols);
      for (int32_t r = r0; r < r1; r += d.sb_mi) {
        for (int32_t c = c0; c < c1; c += d.sb_mi) {
          nodes.clear();
          const uint32_t sb = reconstruct_superblock(grid, r, c, d.sb_mi, &nodes);
          mp.emit(nodes, sb, 0, tile);
        }
      }
      fb::PartitionInfoBuilder p(fbb);
      p.add_kind(fb::PartitionKind::Tile);
      p.add_index(tile);
      const fb::Rect rect(c0 * 4, r0 * 4, std::max(0, std::min(c1 * 4, coded_w) - c0 * 4),
                          std::max(0, std::min(r1 * 4, coded_h) - r0 * 4));
      p.add_rect(&rect);
      p.add_first_block_addr(static_cast<uint32_t>(r0 * d.mi_cols + c0));
      p.add_block_count(static_cast<uint32_t>((r1 - r0) * (c1 - c0)));
      p.add_qp(static_cast<int16_t>(d.base_qindex));
      p.add_qp_delta(0);
      p.add_tile_row(static_cast<uint16_t>(tr));
      p.add_tile_col(static_cast<uint16_t>(tc));
      tiles.push_back(p.Finish());
    }
  }
  return tiles;
}

// The per-cell CellGrid vectors, each from the cell's own MI (tx_size is per transform unit; the rest is equal
// within a block), plus the reference usage counts.
struct Cells {
  explicit Cells(size_t n)
      : pred(n),
        qp(n),
        mode_y(n),
        mode_c(n),
        mv0_x(n),
        mv0_y(n),
        mv1_x(n),
        mv1_y(n),
        ref0(n, -1),
        ref1(n, -1),
        tx_w(n, kNotApplicable),
        tx_h(n, kNotApplicable),
        tx_type(n, kNotApplicable),
        cbf(n),
        filter_idx(n) {}
  std::vector<uint8_t> pred;
  std::vector<int16_t> qp;
  std::vector<int16_t> mode_y;
  std::vector<int16_t> mode_c;
  std::vector<int16_t> mv0_x;
  std::vector<int16_t> mv0_y;
  std::vector<int16_t> mv1_x;
  std::vector<int16_t> mv1_y;
  std::vector<int8_t> ref0;
  std::vector<int8_t> ref1;
  std::vector<uint8_t> tx_w;
  std::vector<uint8_t> tx_h;
  std::vector<uint8_t> tx_type;  // extension patch 0001 only
  std::vector<uint8_t> cbf;
  std::vector<uint8_t> filter_idx;
  std::array<uint32_t, 8> usage{};

  void set_motion(size_t i, const MiInfo& m) {
    if (m.intrabc != 0 || is_inter(m)) {
      mv0_x[i] = static_cast<int16_t>(m.mv_col[0] * 2);  // 1/8 -> 1/16 luma samples
      mv0_y[i] = static_cast<int16_t>(m.mv_row[0] * 2);
    }
    if (!is_inter(m)) {
      return;
    }
    ref0[i] = static_cast<int8_t>(m.ref_frame[0]);
    ++usage.at(static_cast<size_t>(std::min<int>(m.ref_frame[0], 7)));
    if (m.ref_frame[1] > 0) {  // 0 (INTRA_FRAME) marks interintra, not a second reference
      ref1[i] = static_cast<int8_t>(m.ref_frame[1]);
      ++usage.at(static_cast<size_t>(std::min<int>(m.ref_frame[1], 7)));
      mv1_x[i] = static_cast<int16_t>(m.mv_col[1] * 2);
      mv1_y[i] = static_cast<int16_t>(m.mv_row[1] * 2);
    }
  }

  void set(size_t i, const MiInfo& m, bool monochrome, uint8_t cdef, bool ext) {
    if (ext) {
      tx_type[i] = grid_tx_type(m);
    }
    pred[i] = pred_kind(m);
    qp[i] = m.current_qindex;
    mode_y[i] = intra_mode_y(m);
    mode_c[i] = intra_mode_c(m, monochrome);
    set_motion(i, m);
    if (m.tx_size >= 0 && m.tx_size < kNumTxSizes) {
      tx_w[i] = kTxLog2.at(static_cast<size_t>(m.tx_size)).w;
      tx_h[i] = kTxLog2.at(static_cast<size_t>(m.tx_size)).h;
    }
    // Per-plane flags are unknown without the extension patch: all planes coded unless skip.
    cbf[i] = m.skip != 0 ? 0 : (monochrome ? 1 : 7);
    filter_idx[i] = cdef;
  }
};

flatbuffers::Offset<fb::CellGrid> build_grid(flatbuffers::FlatBufferBuilder& fbb, const Mapper& mp, const Cells& c) {
  const auto block_id_v = vec(fbb, mp.block_id);
  const auto partition_id_v = vec(fbb, mp.partition_id);
  const auto pred_v = vec(fbb, c.pred);
  const auto qp_v = vec(fbb, c.qp);
  const auto mode_y_v = vec(fbb, c.mode_y);
  const auto mode_c_v = vec(fbb, c.mode_c);
  const auto mv0_x_v = vec(fbb, c.mv0_x);
  const auto mv0_y_v = vec(fbb, c.mv0_y);
  const auto mv1_x_v = vec(fbb, c.mv1_x);
  const auto mv1_y_v = vec(fbb, c.mv1_y);
  const auto ref0_v = vec(fbb, c.ref0);
  const auto ref1_v = vec(fbb, c.ref1);
  const auto tx_w_v = vec(fbb, c.tx_w);
  const auto tx_h_v = vec(fbb, c.tx_h);
  const auto tx_type_v = mp.d.ext ? vec(fbb, c.tx_type) : flatbuffers::Offset<flatbuffers::Vector<uint8_t>>();
  const auto cbf_v = vec(fbb, c.cbf);
  const auto filter_v = vec(fbb, c.filter_idx);
  fb::CellGridBuilder g(fbb);
  g.add_cells_w(mp.cells_w);
  g.add_cells_h(mp.cells_h);
  g.add_block_id(block_id_v);
  g.add_partition_id(partition_id_v);
  g.add_pred(pred_v);
  g.add_qp(qp_v);
  g.add_intra_mode_y(mode_y_v);
  g.add_intra_mode_c(mode_c_v);
  g.add_mv0_x(mv0_x_v);
  g.add_mv0_y(mv0_y_v);
  g.add_mv1_x(mv1_x_v);
  g.add_mv1_y(mv1_y_v);
  g.add_ref0(ref0_v);
  g.add_ref1(ref1_v);
  g.add_tx_log2_w(tx_w_v);
  g.add_tx_log2_h(tx_h_v);
  if (!tx_type_v.IsNull()) {
    g.add_tx_type(tx_type_v);
  }
  g.add_cbf(cbf_v);
  g.add_filter_idx(filter_v);
  return g.Finish();
}

// Blocks and MotionVectors of the mapper into @p out.
void serialise_blocks(flatbuffers::FlatBufferBuilder& fbb, const Mapper& mp, MappedBlocks* out) {
  std::vector<flatbuffers::Offset<fb::Block>> blocks;
  blocks.reserve(mp.blocks.size());
  for (size_t id = 0; id < mp.blocks.size(); ++id) {
    const OutBlock& b = mp.blocks[id];
    blocks.push_back(fb::CreateBlockDirect(fbb, static_cast<uint32_t>(id), b.parent, &b.children, b.kind, &b.rect,
                                           b.level, b.partition_type, static_cast<fb::PredKind>(b.pred), b.skip,
                                           b.merge, b.qp, /*bits_x8=*/0, b.ext_type, b.ext.empty() ? nullptr : &b.ext));
  }
  out->blocks = fbb.CreateVector(blocks);
  std::vector<flatbuffers::Offset<fb::MotionVector>> mvs;
  mvs.reserve(mp.mvs.size());
  for (const auto& m : mp.mvs) {
    mvs.push_back(fb::CreateMotionVectorDirect(fbb, m.block_id, m.list, m.ref, &m.rect, m.x, m.y, m.mvd.x, m.mvd.y,
                                               m.predictor.c_str(), m.flags));
  }
  out->mvs = fbb.CreateVector(mvs);
}

// refs[0] is the INTRA placeholder; refs[t] = reference type t with its DPB slot. RefSlot.frame stays -1: the host's
// DPB model maps slots to decode indices (§7.5.3 A6).
flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::RefSlot>>> ref_slots(
    flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, const std::array<uint32_t, 8>& usage) {
  std::vector<flatbuffers::Offset<fb::RefSlot>> refs;
  for (size_t t = 0; t < 8; ++t) {
    const fb::FrameId none(-1, -1);
    const int8_t slot = t == 0 || d.intra_frame ? int8_t{-1} : d.ref_frame_idx.at(t - 1);
    refs.push_back(fb::CreateRefSlotDirect(fbb, static_cast<uint8_t>(t), &none, std::string(kRefNames.at(t)).c_str(),
                                           false, t == 0 ? 0U : usage.at(t), slot));
  }
  return fbb.CreateVector(refs);
}

}  // namespace

MappedBlocks map_blocks(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, int32_t coded_w, int32_t coded_h) {
  MappedBlocks out;
  const auto cells_w = static_cast<uint32_t>((coded_w + 3) / 4);
  const auto cells_h = static_cast<uint32_t>((coded_h + 3) / 4);
  const size_t n_cells = size_t{cells_w} * cells_h;
  if (!block_data_usable(d, cells_w, cells_h)) {
    if (d.valid) {
      out.warnings.emplace_back("libaom block data does not match the frame size; no block data for this frame");
    }
    out.grid = fb::CreateCellGrid(fbb, cells_w, cells_h);
    return out;
  }

  // libaom computes cdef only when the sequence enables it, outside lossless and IntraBC frames; BlockData carries
  // the result in cdef_bits / cdef_strengths (empty strengths = CDEF off).
  const bool enable_cdef = !d.cdef_strengths.empty();
  Mapper mp(d, cells_w, cells_h);
  mp.cdef_units_w = (d.mi_cols + kCdefUnitMi - 1) / kCdefUnitMi;
  mp.cdef_idx = cdef_unit_indices(d, enable_cdef, mp.cdef_units_w, (d.mi_rows + kCdefUnitMi - 1) / kCdefUnitMi);
  mp.block_id.assign(n_cells, 0);
  mp.partition_id.assign(n_cells, 0);

  OutBlock root;
  root.rect = {0, 0, coded_w, coded_h};
  mp.add(std::move(root));
  const auto tiles = emit_tiles(fbb, mp, coded_w, coded_h);
  out.partitions = fbb.CreateVector(tiles);

  Cells cells(n_cells);
  uint32_t uncovered = 0;
  for (uint32_t y = 0; y < cells_h; ++y) {
    for (uint32_t x = 0; x < cells_w; ++x) {
      const size_t i = size_t{y} * cells_w + x;
      const auto r = static_cast<int32_t>(y);
      const auto c = static_cast<int32_t>(x);
      uncovered += mp.block_id[i] == 0 ? 1U : 0U;
      cells.set(i, mp.mi(r, c), d.monochrome, mp.cdef_at(r, c), d.ext);
    }
  }
  if (uncovered > 0) {
    out.warnings.push_back(std::to_string(uncovered) + " cells are not covered by the reconstructed partition tree");
  }
  if (mp.partition_mismatch > 0) {
    out.warnings.push_back(std::to_string(mp.partition_mismatch) +
                           " blocks whose libaom partition type differs from the reconstructed partition tree");
  }
  if (mp.ambiguous > 0) {
    out.warnings.push_back("partition ambiguous at " + std::to_string(mp.ambiguous) + " nodes, first at MI " +
                           mp.first_ambiguous + "; resolved as PARTITION_SPLIT");
  }

  serialise_blocks(fbb, mp, &out);
  out.refs = ref_slots(fbb, d, cells.usage);
  out.filter_params = filter_params(fbb, d, enable_cdef, coded_h);
  out.grid = build_grid(fbb, mp, cells);
  return out;
}

}  // namespace ovt::aom
