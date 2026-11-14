// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Extension-patch fields -> Av1Block / CellGrid / MotionVector / Av1FilterParams (chapter 07 §7.6.1 extension
// table, WI-2.159 step 5).

#include "map_mi_ext.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ovt::aom {

namespace {

namespace fb = ovt::fb;

constexpr int16_t kMvdNone = -32768;
constexpr uint8_t kNotApplicable = 0xFF;
constexpr int kNumTxTypes = 16;          // TX_TYPES
constexpr int16_t kCompoundWedge = 2;    // libaom COMPOUND_WEDGE
constexpr int16_t kCompoundDiffwtd = 3;  // libaom COMPOUND_DIFFWTD

template <typename T, size_t N>
std::vector<T> to_vector(const std::array<T, N>& a) {
  return {a.begin(), a.end()};
}

// Nested Av1Palette of a block with a palette (PaletteSizeY or PaletteSizeUV > 0); empty otherwise.
std::vector<uint8_t> palette_buffer(const MiInfo& m) {
  if (m.palette <= 0 && m.uv_palette <= 0) {
    return {};
  }
  const auto n_y = static_cast<size_t>(std::clamp<int>(m.palette, 0, 8));
  const auto n_uv = static_cast<size_t>(std::clamp<int>(m.uv_palette, 0, 8));
  const auto first = [&m](size_t plane, size_t n) {
    const auto& c = m.palette_colors.at(plane);
    return std::vector<uint16_t>(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(n));
  };
  const std::vector<uint16_t> y = first(0, n_y);
  const std::vector<uint16_t> u = first(1, n_uv);
  const std::vector<uint16_t> v = first(2, n_uv);
  flatbuffers::FlatBufferBuilder f(128);
  f.Finish(fb::CreateAv1PaletteDirect(f, &y, &u, &v));
  const std::span<const uint8_t> buf(f.GetBufferPointer(), f.GetSize());
  return {buf.begin(), buf.end()};
}

// One Av1LrUnit: unit @p u of plane @p p at unit row @p r, column @p c, covering @p rect.
flatbuffers::Offset<fb::Av1LrUnit> lr_unit(flatbuffers::FlatBufferBuilder& fbb, const LrUnit& u, size_t p, int32_t r,
                                           int32_t c, const fb::Rect& rect) {
  std::vector<int16_t> wiener;
  std::vector<int16_t> xqd;
  if (u.type == 1) {  // RESTORE_WIENER: pass 0 vertical, pass 1 horizontal (5.11.58)
    wiener = {u.vfilter[0], u.vfilter[1], u.vfilter[2], u.hfilter[0], u.hfilter[1], u.hfilter[2]};
  }
  if (u.type == 2) {  // RESTORE_SGRPROJ
    xqd = {u.xqd[0], u.xqd[1]};
  }
  return fb::CreateAv1LrUnitDirect(
      fbb, static_cast<uint8_t>(p), static_cast<uint16_t>(r), static_cast<uint16_t>(c), &rect,
      static_cast<fb::Av1RestorationType>(u.type), wiener.empty() ? nullptr : &wiener,
      u.type == 2 ? static_cast<uint8_t>(u.ep) : kNotApplicable, xqd.empty() ? nullptr : &xqd);
}

// The Av1LrUnit tables of plane @p p, appended to @p units. Units tile the (upscaled) plane; the last row and
// column take the remainder (7.17). Rects are in luma samples.
void add_lr_units(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, size_t p, int32_t frame_h,
                  std::vector<flatbuffers::Offset<fb::Av1LrUnit>>& units) {
  const FilterExt& f = d.filters;
  const std::vector<LrUnit>& lu = f.lr_units.at(p);
  const int32_t horz = f.lr_horz.at(p);
  const int32_t vert = f.lr_vert.at(p);
  if (horz <= 0 || lu.size() != static_cast<size_t>(horz) * static_cast<size_t>(vert)) {
    return;
  }
  const int32_t sx = p == 0 ? 0 : d.ss_x;
  const int32_t sy = p == 0 ? 0 : d.ss_y;
  const int32_t plane_w = (d.upscaled_width + sx) >> sx;
  const int32_t plane_h = (frame_h + sy) >> sy;
  const int32_t size = f.lr_unit_size.at(p);
  units.reserve(units.size() + lu.size());
  for (int32_t r = 0; r < vert; ++r) {
    for (int32_t c = 0; c < horz; ++c) {
      const LrUnit& u = lu[(static_cast<size_t>(r) * static_cast<size_t>(horz)) + static_cast<size_t>(c)];
      const int32_t x0 = c * size;
      const int32_t y0 = r * size;
      const int32_t x1 = c == horz - 1 ? plane_w : std::min(plane_w, x0 + size);
      const int32_t y1 = r == vert - 1 ? plane_h : std::min(plane_h, y0 + size);
      const fb::Rect rect(x0 << sx, y0 << sy, (x1 - x0) << sx, (y1 - y0) << sy);
      units.push_back(lr_unit(fbb, u, p, r, c, rect));
    }
  }
}

}  // namespace

Av1BlockExt make_block_ext(flatbuffers::FlatBufferBuilder& fbb, const MiInfo& m, bool inter, bool delta_lf_present) {
  Av1BlockExt v;
  if (delta_lf_present) {
    std::vector<int8_t> dlf(m.delta_lf.size());
    std::transform(m.delta_lf.begin(), m.delta_lf.end(), dlf.begin(), [](int16_t x) { return static_cast<int8_t>(x); });
    v.delta_lf = fbb.CreateVector(dlf);
  }
  if (inter) {
    // Mvd: Mv order (row, col per list), 1/8 units, -32768 where the list did not code NEWMV; mv_joint: one entry
    // per list that did.
    std::vector<int16_t> mvd;
    std::vector<uint8_t> joints;
    mvd.reserve(4);
    joints.reserve(2);
    for (size_t list = 0; list < 2; ++list) {
      mvd.push_back(m.mvd.at(list)[0]);
      mvd.push_back(m.mvd.at(list)[1]);
      if (m.mv_joint.at(list) >= 0) {
        joints.push_back(static_cast<uint8_t>(m.mv_joint.at(list)));
      }
    }
    v.mvd = fbb.CreateVector(mvd);
    v.mv_joint = fbb.CreateVector(joints);
    if (m.wm_type >= 0) {
      v.warp = fbb.CreateVector(to_vector(m.wm_params));
    }
  }
  const std::vector<uint8_t> pal = palette_buffer(m);
  if (!pal.empty()) {
    v.palette = fbb.CreateVector(pal);
  }
  return v;
}

void add_block_ext(fb::Av1BlockBuilder& b, const MiInfo& m, const Av1BlockExt& v, bool inter) {
  b.add_skip_mode(static_cast<uint8_t>(m.skip_mode != 0));
  b.add_seg_id_predicted(static_cast<uint8_t>(m.seg_id_predicted != 0));
  if (!v.delta_lf.IsNull()) {
    b.add_DeltaLF(v.delta_lf);
  }
  if (!v.palette.IsNull()) {
    b.add_palette(v.palette);
  }
  if (m.tx_type >= 0 && m.tx_type < kNumTxTypes) {
    b.add_TxType(static_cast<fb::Av1TxType>(m.tx_type));
  }
  if (!inter) {
    if (m.intrabc == 0) {  // intra: angle deltas and filter intra (5.11.24, 5.11.42)
      b.add_AngleDeltaY(static_cast<int8_t>(m.angle_delta[0]));
      b.add_AngleDeltaUV(static_cast<int8_t>(m.angle_delta[1]));
      b.add_use_filter_intra(static_cast<uint8_t>(m.filter_intra_mode >= 0));
      if (m.filter_intra_mode >= 0) {
        b.add_filter_intra_mode(static_cast<fb::Av1FilterIntraMode>(m.filter_intra_mode));
      }
    }
    return;
  }
  b.add_drl_mode_idx(static_cast<uint8_t>(std::max<int16_t>(m.ref_mv_idx, 0)));
  b.add_Mvd(v.mvd);
  b.add_mv_joint(v.mv_joint);
  if (m.interintra_mode >= 0) {  // 5.11.28
    b.add_interintra_mode(static_cast<fb::Av1InterIntraMode>(m.interintra_mode));
    b.add_wedge_interintra(static_cast<uint8_t>(m.wedge_interintra != 0));
    if (m.wedge_interintra > 0 && m.interintra_wedge_index >= 0) {
      b.add_wedge_index(static_cast<uint8_t>(m.interintra_wedge_index));
    }
  }
  if (m.ref_frame[1] > 0) {  // compound (5.11.31)
    b.add_comp_group_idx(static_cast<uint8_t>(std::max<int16_t>(m.comp_group_idx, 0)));
    b.add_compound_idx(static_cast<uint8_t>(std::max<int16_t>(m.compound_idx, 0)));
    if (m.compound_type == kCompoundWedge && m.wedge_index >= 0) {
      b.add_wedge_index(static_cast<uint8_t>(m.wedge_index));
      b.add_wedge_sign(static_cast<uint8_t>(m.wedge_sign));
    }
    if (m.compound_type == kCompoundDiffwtd && m.mask_type >= 0) {
      b.add_mask_type(static_cast<uint8_t>(m.mask_type));
    }
  }
  if (m.wm_type >= 0) {  // motion_mode == WARPED_CAUSAL: LocalValid and LocalWarpParams (7.11.3.8)
    b.add_LocalValid(static_cast<uint8_t>(m.wm_invalid == 0));
    b.add_LocalWarpParams(v.warp);
  }
}

uint8_t grid_tx_type(const MiInfo& m) {
  return m.skip == 0 && m.tx_type >= 0 && m.tx_type < kNumTxTypes ? static_cast<uint8_t>(m.tx_type) : kNotApplicable;
}

Mvd16 mvd_of(const MiInfo& m, int list) {
  const auto& d = m.mvd.at(static_cast<size_t>(list));
  if (d[0] == kMvdNone || d[1] == kMvdNone) {
    return {};
  }
  return {static_cast<int16_t>(d[1] * 2), static_cast<int16_t>(d[0] * 2)};
}

uint8_t ext_cdef_idx(const MiInfo& origin, size_t num_strengths) {
  return origin.cdef_idx >= 0 && static_cast<size_t>(origin.cdef_idx) < num_strengths
             ? static_cast<uint8_t>(origin.cdef_idx)
             : kNotApplicable;
}

FilterParamsExt make_filter_ext(flatbuffers::FlatBufferBuilder& fbb, const BlockData& d, int32_t frame_h) {
  const FilterExt& f = d.filters;
  FilterParamsExt v;
  std::vector<uint8_t> lvl(f.lf_level.size());
  std::transform(f.lf_level.begin(), f.lf_level.end(), lvl.begin(), [](int32_t l) { return static_cast<uint8_t>(l); });
  v.lf_level = fbb.CreateVector(lvl);
  v.ref_deltas = fbb.CreateVector(to_vector(f.lf_ref_deltas));
  v.mode_deltas = fbb.CreateVector(to_vector(f.lf_mode_deltas));
  const size_t planes = d.monochrome ? 1 : 3;
  std::vector<fb::Av1RestorationType> types;
  std::vector<uint16_t> sizes;
  std::vector<flatbuffers::Offset<fb::Av1LrUnit>> units;
  types.reserve(planes);
  sizes.reserve(planes);
  for (size_t p = 0; p < planes; ++p) {
    types.push_back(static_cast<fb::Av1RestorationType>(f.lr_type.at(p)));
    sizes.push_back(static_cast<uint16_t>(f.lr_unit_size.at(p)));
    add_lr_units(fbb, d, p, frame_h, units);
  }
  v.lr_type = fbb.CreateVector(types);
  v.lr_size = fbb.CreateVector(sizes);
  v.lr_units = fbb.CreateVector(units);
  return v;
}

void add_filter_ext(fb::Av1FilterParamsBuilder& p, const BlockData& d, const FilterParamsExt& v) {
  const FilterExt& f = d.filters;
  p.add_loop_filter_level(v.lf_level);
  p.add_loop_filter_sharpness(static_cast<uint8_t>(f.lf_sharpness));
  p.add_loop_filter_delta_enabled(static_cast<uint8_t>(f.lf_delta_enabled));
  p.add_loop_filter_ref_deltas(v.ref_deltas);
  p.add_loop_filter_mode_deltas(v.mode_deltas);
  p.add_delta_lf_present(static_cast<uint8_t>(f.delta_lf_present));
  uint8_t res_log2 = 0;  // libaom keeps 1 << delta_lf_res
  while ((1 << res_log2) < f.delta_lf_res && res_log2 < 3) {
    ++res_log2;
  }
  p.add_delta_lf_res(f.delta_lf_present ? res_log2 : uint8_t{0});
  p.add_delta_lf_multi(static_cast<uint8_t>(f.delta_lf_multi));
  p.add_FrameRestorationType(v.lr_type);
  p.add_LoopRestorationSize(v.lr_size);
  p.add_lr_units(v.lr_units);
}

}  // namespace ovt::aom
