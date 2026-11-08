// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Side data -> model mapping of the FFmpeg analysis plugin (chapter 07 §7.6.3, WI-2.170).

#include "map_h264_sidedata.hpp"

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

constexpr uint8_t kSeverityInfo = 0;  // ovt.fb.Severity
constexpr uint8_t kSeverityWarning = 1;
constexpr int32_t kMbSize = 16;
constexpr int32_t kCell = 4;
constexpr uint16_t kFlagBiPred = 1U << 0;  // ovt.fb.MotionVector.flags
constexpr uint16_t kFlagField = 1U << 4;
constexpr int32_t kQpHistBins = 256;
constexpr int32_t kQpHistBias = 128;  // qp_hist bin = qp + 128 for H.26x (chapter 06 §6.2.3)

bool is_interlaced(const AVFrame* f) {
#ifdef AV_FRAME_FLAG_INTERLACED
  return (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
#else
  return f->interlaced_frame != 0;  // FFmpeg 6.0
#endif
}

// The vector component in a short; true in *clamped when it had to be limited to +-32767.
int16_t clamp16(int64_t v, bool* clamped) {
  constexpr int64_t kMax = std::numeric_limits<int16_t>::max();
  *clamped = *clamped || v > kMax || v < -kMax;
  return static_cast<int16_t>(std::clamp(v, -kMax, kMax));
}

// The QP of every macroblock from AVVideoEncParams, QpBdOffsetY removed; `known` marks the macroblocks it covers.
// FFmpeg's blocks are 16x16, in raster order, with qp + delta_qp == QP'Y (n7.1.1 h264_picture.c).
void map_qp(const AVFrame* frame, int bd_offset, int32_t mb_w, int32_t mb_h, std::vector<MbBlock>* mbs,
            std::vector<uint8_t>* known) {
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
  if (sd == nullptr || sd->data == nullptr || sd->size < sizeof(AVVideoEncParams)) {
    return;
  }
  auto* par = reinterpret_cast<AVVideoEncParams*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast): FFmpeg API
  if (par->type != AV_VIDEO_ENC_PARAMS_H264) {
    return;
  }
  for (unsigned i = 0; i < par->nb_blocks; ++i) {
    const AVVideoBlockParams* b = av_video_enc_params_block(par, i);
    if (b->w != kMbSize || b->h != kMbSize || b->src_x < 0 || b->src_y < 0 || b->src_x % kMbSize != 0 ||
        b->src_y % kMbSize != 0 || b->src_x / kMbSize >= mb_w || b->src_y / kMbSize >= mb_h) {
      continue;
    }
    const size_t mb =
        (static_cast<size_t>(b->src_y / kMbSize) * static_cast<size_t>(mb_w)) + static_cast<size_t>(b->src_x / kMbSize);
    (*mbs)[mb].qp = static_cast<int16_t>(par->qp + b->delta_qp - bd_offset);
    (*known)[mb] = 1;
  }
}

struct Counts {
  uint32_t dropped = 0;  // vectors that do not fit the macroblock grid
  uint32_t clamped = 0;  // vectors with a component limited to +-32767
};

// True when the rect lies on the 4x4 grid inside one macroblock of the picture (FFmpeg's partitions always do).
bool fits_macroblock(const Box& r, int scale, int32_t mb_w, int32_t mb_h) {
  return scale > 0 && r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && (r.x % kCell) == 0 && (r.y % kCell) == 0 &&
         (r.x % kMbSize) + r.w <= kMbSize && (r.y % kMbSize) + r.h <= kMbSize && r.x / kMbSize < mb_w &&
         r.y / kMbSize < mb_h;
}

// Motion vectors: the rect from the block centre, the vector in 1/16 sample, the list from the sign of `source`,
// then the canonical order of chapter 06 §6.3.7 and the bi-prediction flag (both lists for the same rect).
void map_motion(const AVFrame* frame, int32_t mb_w, int32_t mb_h, bool interlaced, std::vector<MbMotion>* mvs,
                Counts* counts) {
  const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
  if (sd == nullptr || sd->data == nullptr) {
    return;
  }
  const size_t count = sd->size / sizeof(AVMotionVector);
  const auto* in = reinterpret_cast<const AVMotionVector*>(sd->data);  // NOLINT(*-pro-type-reinterpret-cast)
  for (size_t i = 0; i < count; ++i) {
    const AVMotionVector& m = in[i];  // NOLINT(*-pro-bounds-pointer-arithmetic)
    const Box rect{m.dst_x - (m.w / 2), m.dst_y - (m.h / 2), m.w, m.h};
    if (!fits_macroblock(rect, m.motion_scale, mb_w, mb_h)) {
      ++counts->dropped;
      continue;
    }
    MbMotion mv;
    mv.block_id = 1U + static_cast<uint32_t>(((rect.y / kMbSize) * mb_w) + (rect.x / kMbSize));
    mv.list = m.source < 0 ? 0 : 1;
    mv.ref = static_cast<int8_t>(mv.list);
    mv.rect = rect;
    bool clamped = false;
    mv.mv_x = clamp16((int64_t{m.motion_x} * 16) / m.motion_scale, &clamped);
    mv.mv_y = clamp16((int64_t{m.motion_y} * 16) / m.motion_scale, &clamped);
    counts->clamped += clamped ? 1 : 0;
    mv.flags = interlaced ? kFlagField : 0;
    mvs->push_back(mv);
  }
  std::sort(mvs->begin(), mvs->end(), [](const MbMotion& a, const MbMotion& b) {
    return std::tie(a.block_id, a.list, a.rect.y, a.rect.x) < std::tie(b.block_id, b.list, b.rect.y, b.rect.x);
  });
  for (MbMotion& a : *mvs) {
    const auto other = std::find_if(mvs->begin(), mvs->end(), [&a](const MbMotion& b) {
      return a.block_id == b.block_id && a.list != b.list && a.rect == b.rect;
    });
    if (other != mvs->end()) {
      a.flags |= kFlagBiPred;
    }
  }
}

// Classification (no mb_type in the side data): a macroblock with a vector is Inter, one without it is Intra when the
// frame carries its QP, else unknown. True when any macroblock was inferred Intra.
bool classify(SideData* out, const std::vector<uint8_t>& qp_known) {
  std::vector<uint8_t> has_mv(out->mbs.size(), 0);
  for (const MbMotion& m : out->mvs) {
    has_mv[m.block_id - 1U] = 1;
  }
  bool inferred_intra = false;
  for (size_t mb = 0; mb < out->mbs.size(); ++mb) {
    MbBlock& blk = out->mbs[mb];
    if (has_mv[mb] != 0) {
      blk.pred = kPredInter;
    } else if (qp_known[mb] != 0) {
      blk.pred = kPredIntra;
      inferred_intra = true;
    }
    blk.qp = qp_known[mb] != 0 ? blk.qp : int16_t{-1};
  }
  return inferred_intra;
}

// The CellGrid vectors: macroblock-wide values first, then the vectors over their rects.
void fill_cells(SideData* out, int32_t mb_w) {
  const size_t cells = static_cast<size_t>(out->cells_w) * out->cells_h;
  out->block_id.assign(cells, 0);
  out->pred.assign(cells, kPredNone);
  out->qp.assign(cells, -1);
  for (size_t l = 0; l < 2; ++l) {
    out->mv_x.at(l).assign(cells, 0);
    out->mv_y.at(l).assign(cells, 0);
    out->ref.at(l).assign(cells, -1);
  }
  for (uint32_t cy = 0; cy < out->cells_h; ++cy) {
    for (uint32_t cx = 0; cx < out->cells_w; ++cx) {
      const size_t mb = (static_cast<size_t>(cy / 4) * static_cast<size_t>(mb_w)) + (cx / 4);
      const size_t cell = (static_cast<size_t>(cy) * out->cells_w) + cx;
      out->block_id[cell] = 1U + static_cast<uint32_t>(mb);
      out->pred[cell] = out->mbs[mb].pred;
      out->qp[cell] = out->mbs[mb].qp;
    }
  }
  for (const MbMotion& m : out->mvs) {
    const auto l = static_cast<size_t>(m.list);
    const auto x_end = std::min<uint32_t>(static_cast<uint32_t>((m.rect.x + m.rect.w) / kCell), out->cells_w);
    const auto y_end = std::min<uint32_t>(static_cast<uint32_t>((m.rect.y + m.rect.h) / kCell), out->cells_h);
    for (auto cy = static_cast<uint32_t>(m.rect.y / kCell); cy < y_end; ++cy) {
      for (auto cx = static_cast<uint32_t>(m.rect.x / kCell); cx < x_end; ++cx) {
        const size_t cell = (static_cast<size_t>(cy) * out->cells_w) + cx;
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
}

void add_diagnostics(bool interlaced, bool inferred_intra, const Counts& counts, std::vector<Diag>* diags) {
  // SPEC-ASSUMPTION(#303): the text of ANA-H264-0002 is the registry's, and the code is emitted only for a frame with
  // an inferred intra macroblock; bit 4 and ANA-H264-0001 follow the frame's INTERLACED flag.
  if (interlaced) {
    diags->push_back(Diag{kSeverityInfo, "ANA-H264-0001", "field MV semantics approximate (FFmpeg side data)"});
  }
  if (inferred_intra) {
    diags->push_back(
        Diag{kSeverityInfo, "ANA-H264-0002", "intra MB: prediction modes not available without the FFmpeg patch"});
  }
  if (counts.clamped != 0) {
    // SPEC-ASSUMPTION(#261): the registry template says "cells"; the count is of clamped vectors.
    diags->push_back(Diag{kSeverityWarning, "MDL-GEN-0003",
                          "motion vector clamped to \xC2\xB1"
                          "32767 (" +
                              std::to_string(counts.clamped) + " cells)"});
  }
  if (counts.dropped != 0) {
    diags->push_back(Diag{kSeverityWarning, "",
                          std::to_string(counts.dropped) + " motion vectors outside the macroblock grid were dropped"});
  }
}

}  // namespace

SideData map_h264_sidedata(const AVFrame* frame, int bit_depth_luma, std::vector<Diag>* diags) {
  SideData out;
  const int32_t w = frame->width;
  const int32_t h = frame->height;
  if (w <= 0 || h <= 0) {
    return out;
  }
  const int32_t mb_w = (w + kMbSize - 1) / kMbSize;
  const int32_t mb_h = (h + kMbSize - 1) / kMbSize;
  out.valid = true;
  out.cells_w = static_cast<uint32_t>((w + kCell - 1) / kCell);
  out.cells_h = static_cast<uint32_t>((h + kCell - 1) / kCell);
  const auto mb_count = static_cast<size_t>(mb_w) * static_cast<size_t>(mb_h);

  out.mbs.resize(mb_count);
  for (size_t mb = 0; mb < mb_count; ++mb) {
    out.mbs[mb].rect = Box{static_cast<int32_t>(mb % static_cast<size_t>(mb_w)) * kMbSize,
                           static_cast<int32_t>(mb / static_cast<size_t>(mb_w)) * kMbSize, kMbSize, kMbSize};
  }
  std::vector<uint8_t> qp_known(mb_count, 0);
  map_qp(frame, 6 * (bit_depth_luma - 8), mb_w, mb_h, &out.mbs, &qp_known);
  const bool interlaced = is_interlaced(frame);
  Counts counts;
  map_motion(frame, mb_w, mb_h, interlaced, &out.mvs, &counts);
  const bool inferred_intra = classify(&out, qp_known);
  fill_cells(&out, mb_w);
  if (std::find(qp_known.begin(), qp_known.end(), uint8_t{1}) != qp_known.end()) {
    out.qp_hist.assign(kQpHistBins, 0);
    for (const int16_t q : out.qp) {
      ++out.qp_hist[static_cast<size_t>(std::clamp<int32_t>(q + kQpHistBias, 0, kQpHistBins - 1))];
    }
  }
  add_diagnostics(interlaced, inferred_intra, counts, diags);
  return out;
}

}  // namespace ovt::ffmpeg
