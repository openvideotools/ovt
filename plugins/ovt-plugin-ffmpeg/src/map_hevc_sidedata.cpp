// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// HEVC side data -> model mapping of the ffmpeg_hevc plugin (chapter 07 §7.8 WI-4.107).

#include "map_hevc_sidedata.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ovt::ffmpeg {

namespace {

constexpr uint8_t kSeverityWarning = 1;  // ovt.fb.Severity
constexpr int32_t kCell = 4;
constexpr uint16_t kFlagBiPred = 1U << 0;  // ovt.fb.MotionVector.flags
constexpr int32_t kQpHistBins = 256;
constexpr int32_t kQpHistBias = 128;  // qp_hist bin = qp + 128 for H.26x (chapter 06 §6.2.3)
#if OVT_FFMPEG_HEVC_ENC_PARAMS
constexpr int kHevcEncParams = AV_VIDEO_ENC_PARAMS_HEVC;
#else
// A libavcodec without patch 0002 (OVT_SYSTEM_FFMPEG) has no AV_VIDEO_ENC_PARAMS_HEVC and its hevc decoder exports
// nothing to map; the H.264 plugin compiles this file too. No AVVideoEncParamsType has this value.
constexpr int kHevcEncParams = -2;
#endif

int16_t clamp16(int64_t v, bool* clamped) {
  constexpr int64_t kMax = std::numeric_limits<int16_t>::max();
  *clamped = *clamped || v > kMax || v < -kMax;
  return static_cast<int16_t>(std::clamp(v, -kMax, kMax));
}

// A rect on the 4x4 grid inside a picture of cells_w x cells_h cells.
bool on_grid(const Box& r, uint32_t cells_w, uint32_t cells_h) {
  return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x % kCell == 0 && r.y % kCell == 0 && r.w % kCell == 0 &&
         r.h % kCell == 0 && static_cast<uint32_t>((r.x + r.w) / kCell) <= cells_w &&
         static_cast<uint32_t>((r.y + r.h) / kCell) <= cells_h;
}

struct Counts {
  uint32_t dropped = 0;  // blocks or vectors off the picture's 4x4 grid
  uint32_t clamped = 0;  // vectors with a component limited to +-32767
};

// The coding units of AV_VIDEO_ENC_PARAMS_HEVC into out->mbs and out->block_id (cells of no CU stay 0). False without
// that side data.
bool map_cus(const AVFrame* frame, SideData* out, Counts* counts) {
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
  if (sd == nullptr || sd->data == nullptr || sd->size < sizeof(AVVideoEncParams)) {
    return false;
  }
  const auto* par = reinterpret_cast<const AVVideoEncParams*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
  if (par->type != kHevcEncParams) {
    return false;
  }
  out->block_id.assign(static_cast<size_t>(out->cells_w) * out->cells_h, 0);
  out->mbs.reserve(par->nb_blocks);
  for (unsigned i = 0; i < par->nb_blocks; ++i) {
    // av_video_enc_params_block() takes a non-const pointer; it only computes an address.
    const AVVideoBlockParams* b = av_video_enc_params_block(const_cast<AVVideoEncParams*>(par), i);  // NOLINT
    const Box rect{b->src_x, b->src_y, b->w, b->h};
    if (!on_grid(rect, out->cells_w, out->cells_h)) {
      ++counts->dropped;
      continue;
    }
    MbBlock cu;
    cu.rect = rect;
    cu.qp = static_cast<int16_t>(std::clamp(par->qp + b->delta_qp, -128, 127));
    cu.pred = kPredIntra;  // until a vector lands in it
    out->mbs.push_back(cu);
    const auto id = static_cast<uint32_t>(out->mbs.size());  // blocks[0] is the root
    for (int32_t cy = rect.y / kCell; cy < (rect.y + rect.h) / kCell; ++cy) {
      for (int32_t cx = rect.x / kCell; cx < (rect.x + rect.w) / kCell; ++cx) {
        out->block_id[(static_cast<size_t>(cy) * out->cells_w) + static_cast<size_t>(cx)] = id;
      }
    }
  }
  return true;
}

// Merges the per-unit vectors: first runs along a row, then runs of equal x/w on consecutive rows (one rect per
// uniform PU, or wider when neighbours agree). Returns them in canonical order (block_id, list, y, x).
std::vector<MbMotion> merge(std::vector<MbMotion> units) {
  const auto key = [](const MbMotion& a) { return std::tie(a.block_id, a.list, a.rect.y, a.rect.x); };
  const auto by_key = [&](const MbMotion& a, const MbMotion& b) { return key(a) < key(b); };
  const auto same = [](const MbMotion& a, const MbMotion& b) {
    return a.block_id == b.block_id && a.list == b.list && a.mv_x == b.mv_x && a.mv_y == b.mv_y;
  };
  std::sort(units.begin(), units.end(), by_key);
  std::vector<MbMotion> rows;
  for (const MbMotion& u : units) {
    MbMotion* last = rows.empty() ? nullptr : &rows.back();
    if (last != nullptr && same(*last, u) && last->rect.y == u.rect.y && last->rect.h == u.rect.h &&
        last->rect.x + last->rect.w == u.rect.x) {
      last->rect.w += u.rect.w;
    } else {
      rows.push_back(u);
    }
  }
  std::sort(rows.begin(), rows.end(), [](const MbMotion& a, const MbMotion& b) {
    return std::tie(a.block_id, a.list, a.rect.x, a.rect.w, a.rect.y) <
           std::tie(b.block_id, b.list, b.rect.x, b.rect.w, b.rect.y);
  });
  std::vector<MbMotion> out;
  for (const MbMotion& r : rows) {
    MbMotion* last = out.empty() ? nullptr : &out.back();
    if (last != nullptr && same(*last, r) && last->rect.x == r.rect.x && last->rect.w == r.rect.w &&
        last->rect.y + last->rect.h == r.rect.y) {
      last->rect.h += r.rect.h;
    } else {
      out.push_back(r);
    }
  }
  std::sort(out.begin(), out.end(), by_key);
  return out;
}

// The vectors of AV_FRAME_DATA_MOTION_VECTORS, in 1/16 sample, each in the CU that holds its top-left cell.
void map_motion(const AVFrame* frame, SideData* out, Counts* counts) {
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
  if (sd == nullptr || sd->data == nullptr) {
    return;
  }
  const size_t count = sd->size / sizeof(AVMotionVector);
  const auto* in = reinterpret_cast<const AVMotionVector*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
  std::vector<MbMotion> units;
  units.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const AVMotionVector& m = in[i];  // NOLINT(*-pro-bounds-pointer-arithmetic)
    const Box rect{m.dst_x - (m.w / 2), m.dst_y - (m.h / 2), m.w, m.h};
    if (m.motion_scale <= 0 || !on_grid(rect, out->cells_w, out->cells_h)) {
      ++counts->dropped;
      continue;
    }
    const uint32_t id =
        out->block_id[(static_cast<size_t>(rect.y / kCell) * out->cells_w) + static_cast<size_t>(rect.x / kCell)];
    if (id == 0) {
      ++counts->dropped;
      continue;
    }
    MbMotion mv;
    mv.block_id = id;
    mv.list = m.source < 0 ? 0 : 1;
    mv.ref = static_cast<int8_t>(mv.list);
    mv.rect = rect;
    bool clamped = false;
    mv.mv_x = clamp16((int64_t{m.motion_x} * 16) / m.motion_scale, &clamped);
    mv.mv_y = clamp16((int64_t{m.motion_y} * 16) / m.motion_scale, &clamped);
    counts->clamped += clamped ? 1 : 0;
    units.push_back(mv);
    out->mbs[id - 1U].pred = kPredInter;
  }

  out->mvs = merge(std::move(units));
  // Bi-prediction: a vector of the other list over the same rect in the same CU. Searched per CU.
  for (auto run = out->mvs.begin(); run != out->mvs.end();) {
    const auto run_end =
        std::find_if(run, out->mvs.end(), [run](const MbMotion& b) { return b.block_id != run->block_id; });
    for (auto a = run; a != run_end; ++a) {
      if (std::any_of(run, run_end, [a](const MbMotion& b) { return a->list != b.list && a->rect == b.rect; })) {
        a->flags |= kFlagBiPred;
      }
    }
    run = run_end;
  }
}

void fill_cells(SideData* out) {
  const size_t cells = static_cast<size_t>(out->cells_w) * out->cells_h;
  out->pred.assign(cells, kPredNone);
  out->qp.assign(cells, -1);
  for (size_t l = 0; l < 2; ++l) {
    out->mv_x.at(l).assign(cells, 0);
    out->mv_y.at(l).assign(cells, 0);
    out->ref.at(l).assign(cells, -1);
  }
  for (size_t cell = 0; cell < cells; ++cell) {
    if (const uint32_t id = out->block_id[cell]; id != 0) {
      out->pred[cell] = out->mbs[id - 1U].pred;
      out->qp[cell] = out->mbs[id - 1U].qp;
    }
  }
  for (const MbMotion& m : out->mvs) {
    const auto l = static_cast<size_t>(m.list);
    for (int32_t cy = m.rect.y / kCell; cy < (m.rect.y + m.rect.h) / kCell; ++cy) {
      for (int32_t cx = m.rect.x / kCell; cx < (m.rect.x + m.rect.w) / kCell; ++cx) {
        const size_t cell = (static_cast<size_t>(cy) * out->cells_w) + static_cast<size_t>(cx);
        out->mv_x[l][cell] = m.mv_x;
        out->mv_y[l][cell] = m.mv_y;
        out->ref[l][cell] = m.ref;
      }
    }
  }
  for (size_t l = 0; l < 2; ++l) {
    out->usage_cells.at(l) =
        static_cast<uint32_t>(std::count(out->ref[l].begin(), out->ref[l].end(), static_cast<int8_t>(l)));
  }
  out->qp_hist.assign(kQpHistBins, 0);
  for (size_t cell = 0; cell < cells; ++cell) {
    if (out->block_id[cell] != 0) {
      ++out->qp_hist[static_cast<size_t>(std::clamp<int32_t>(out->qp[cell] + kQpHistBias, 0, kQpHistBins - 1))];
    }
  }
}

}  // namespace

SideData map_hevc_sidedata(const AVFrame* frame, std::vector<Diag>* diags) {
  SideData out;
  if (frame->width <= 0 || frame->height <= 0) {
    return out;
  }
  out.cells_w = static_cast<uint32_t>((frame->width + kCell - 1) / kCell);
  out.cells_h = static_cast<uint32_t>((frame->height + kCell - 1) / kCell);
  Counts counts;
  if (!map_cus(frame, &out, &counts)) {
    return SideData{};
  }
  out.valid = true;
  map_motion(frame, &out, &counts);
  fill_cells(&out);
  if (counts.clamped != 0) {
    // SPEC-ASSUMPTION(#261): the registry template says "cells"; the count is of clamped vectors.
    diags->push_back(Diag{kSeverityWarning, "MDL-GEN-0003",
                          "motion vector clamped to \xC2\xB1"
                          "32767 (" +
                              std::to_string(counts.clamped) + " cells)"});
  }
  if (counts.dropped != 0) {
    diags->push_back(Diag{kSeverityWarning, "",
                          std::to_string(counts.dropped) + " blocks or motion vectors off the picture were dropped"});
  }
  return out;
}

}  // namespace ovt::ffmpeg
