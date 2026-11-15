// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 quantization_params() (chapter 7 §7.3.6 row 16; WI-1.117), get_qindex() (AV1 §7.12.2) and the lossless
// derivations of uncompressed_header() (AV1 §5.9.2: CodedLossless, LosslessArray, SegQMLevel, AllLossless).

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecQuantization = "AV1 5.9.12";
constexpr int32_t kMaxQIndex = 255;

}  // namespace

QuantizationParams parse_quantization_params(BitReader& br, SyntaxBuilder& b, const ColorConfig& cc) {
  // AV1 §5.9.12
  QuantizationParams q;
  const auto start = br.mark();
  auto s = b.scoped("quantization_params", kSpecQuantization);
  q.base_q_idx = static_cast<uint8_t>(b.read_u("base_q_idx", 8));
  q.DeltaQYDc = b.read_delta_q("DeltaQYDc");
  if (cc.NumPlanes > 1) {
    if (cc.separate_uv_delta_q) {
      q.diff_uv_delta = b.read_flag("diff_uv_delta");
    } else {
      q.diff_uv_delta = false;
    }
    q.DeltaQUDc = b.read_delta_q("DeltaQUDc");
    q.DeltaQUAc = b.read_delta_q("DeltaQUAc");
    if (q.diff_uv_delta) {
      q.DeltaQVDc = b.read_delta_q("DeltaQVDc");
      q.DeltaQVAc = b.read_delta_q("DeltaQVAc");
    } else {
      q.DeltaQVDc = q.DeltaQUDc;
      q.DeltaQVAc = q.DeltaQUAc;
    }
  } else {
    q.DeltaQUDc = 0;
    q.DeltaQUAc = 0;
    q.DeltaQVDc = 0;
    q.DeltaQVAc = 0;
  }
  b.derived("delta_q", fmt::format("DeltaQYDc = {}, DeltaQUDc = {}, DeltaQUAc = {}, DeltaQVDc = {}, DeltaQVAc = {}",
                                   q.DeltaQYDc, q.DeltaQUDc, q.DeltaQUAc, q.DeltaQVDc, q.DeltaQVAc));
  q.using_qmatrix = b.read_flag("using_qmatrix");
  if (q.using_qmatrix) {
    q.qm_y = static_cast<uint8_t>(b.read_u("qm_y", 4));
    q.qm_u = static_cast<uint8_t>(b.read_u("qm_u", 4));
    if (!cc.separate_uv_delta_q) {
      q.qm_v = q.qm_u;
      b.derived("qm_v", q.qm_v);
    } else {
      q.qm_v = static_cast<uint8_t>(b.read_u("qm_v", 4));
    }
  }
  q.span = br.span_since(start);
  return q;
}

int32_t get_qindex(bool ignoreDeltaQ, int segmentId, const QuantizationParams& q, const SegmentationParams& seg,
                   int32_t CurrentQIndex) noexcept {
  // AV1 §7.12.2 get_qindex(ignoreDeltaQ, segmentId)
  const auto sid = static_cast<size_t>(std::clamp(segmentId, 0, kMaxSegments - 1));
  if (seg.segmentation_enabled && seg.FeatureEnabled[sid][kSegLvlAltQ]) {
    const int32_t data = seg.FeatureData[sid][kSegLvlAltQ];
    int32_t qindex = static_cast<int32_t>(q.base_q_idx) + data;
    if (!ignoreDeltaQ) {
      qindex = CurrentQIndex + data;
    }
    return std::clamp(qindex, 0, kMaxQIndex);
  }
  if (!ignoreDeltaQ) {
    return CurrentQIndex;
  }
  return static_cast<int32_t>(q.base_q_idx);
}

Lossless compute_lossless(const QuantizationParams& q, const SegmentationParams& seg,
                          bool frame_width_equals_upscaled_width) noexcept {
  // AV1 §5.9.2, after delta_lf_params()
  Lossless l;
  l.CodedLossless = true;
  for (int segmentId = 0; segmentId < kMaxSegments; ++segmentId) {
    const auto sid = static_cast<size_t>(segmentId);
    const int32_t qindex = get_qindex(true, segmentId, q, seg, q.base_q_idx);
    l.LosslessArray[sid] =
        qindex == 0 && q.DeltaQYDc == 0 && q.DeltaQUAc == 0 && q.DeltaQUDc == 0 && q.DeltaQVAc == 0 && q.DeltaQVDc == 0;
    if (!l.LosslessArray[sid]) {
      l.CodedLossless = false;
    }
    if (q.using_qmatrix) {
      if (l.LosslessArray[sid]) {
        l.SegQMLevel[0][sid] = kQmLevelLossless;
        l.SegQMLevel[1][sid] = kQmLevelLossless;
        l.SegQMLevel[2][sid] = kQmLevelLossless;
      } else {
        l.SegQMLevel[0][sid] = q.qm_y;
        l.SegQMLevel[1][sid] = q.qm_u;
        l.SegQMLevel[2][sid] = q.qm_v;
      }
    }
  }
  l.AllLossless = l.CodedLossless && frame_width_equals_upscaled_width;
  return l;
}

}  // namespace ovt::syntax::av1
