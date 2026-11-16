// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// CPU side of the MV arrow and intra direction passes (chapter 10 §10.5.1, §10.5.4, §10.5.8; WI-2.434 steps 1, 4, 5
// and 6): turns FrameAnalysis.mvs and CellGrid.intra_mode_y into the instance texel pair the arrow pass pulls its
// vertices from, and cuts that set down to a viewport for the R-8K-1 culled path. Internal to libovt-render.

#ifndef OVT_RENDER_SRC_OVERLAY_ARROW_INSTANCES_H
#define OVT_RENDER_SRC_OVERLAY_ARROW_INSTANCES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ovt/render/overlay_set.h"
#include "ovt/render/render_api.h"

namespace ovt::render::detail {

// Instance flags (bits 8.. of the attribute texel's R channel; §10.5.4 "bit 1 of the instance flags selects the dash").
// Bits 0, 2, 3 and 4 are MotionVector.flags bits 0, 2, 3 and 4; bit 1 is not MotionVector.flags bit 1 (weighted).
inline constexpr uint32_t kInstBi = 1U << 0;           // bi-predicted block (MotionVector.flags bit0)
inline constexpr uint32_t kInstDashed = 1U << 1;       // list 1: dashed shaft, 4 px on / 2 px off
inline constexpr uint32_t kInstGlobal = 1U << 2;       // global / warped (MotionVector.flags bit2): colour 21
inline constexpr uint32_t kInstSubBlock = 1U << 3;     // affine / OBMC sub-block (MotionVector.flags bit3)
inline constexpr uint32_t kInstField = 1U << 4;        // field MV (MotionVector.flags bit4), drawn unchanged
inline constexpr uint32_t kInstIntraStroke = 1U << 5;  // intra direction stroke: shaft only, no head, no dot

// §10.5.2 colour rows the arrow pass uses.
inline constexpr uint8_t kColorL0 = 1;
inline constexpr uint8_t kColorL1 = 2;
inline constexpr uint8_t kColorBi = 3;
inline constexpr uint8_t kColorIntraStroke = 20;
inline constexpr uint8_t kColorGlobal = 21;

// Instances are stored in five consecutive segments so that the §10.5.6 pass order is a few range draws:
//   [0, l1_bi)            list 1 arrows of uni-predicted blocks
//   [l1_bi, l0_bi)        list 1 arrows of bi-predicted blocks
//   [l0_bi, l0_uni)       list 0 arrows of bi-predicted blocks
//   [l0_uni, intra)       list 0 arrows of uni-predicted blocks
//   [intra, end)          intra direction strokes (pass 8)
// Pass 9 draws L1 [0, l0_bi), then L0 [l0_bi, intra) (skipping bi instances when MvBi is on), then, with MvBi, the bi
// instances [l1_bi, l0_uni) in cyan. Within a segment, instances keep the order of FrameAnalysis.mvs (or raster order
// of the strokes), so the build is deterministic whatever the thread count.
struct ArrowSegments {
  uint32_t l1_bi = 0;
  uint32_t l0_bi = 0;
  uint32_t l0_uni = 0;
  uint32_t intra = 0;
  uint32_t end = 0;
  bool operator==(const ArrowSegments&) const = default;
};

struct ArrowInstances {
  // RGBA32F texel per instance: {ox, oy, vx, vy}: origin and vector in luma samples (coded-picture coordinates), with
  // OverlayConfig.mv_scale already applied (the GPU overlay cache keys on it, WI-2.437).
  std::vector<float> geom;
  // RGBA32UI texel per instance: {color_idx | flags << 8, id, 0, 0}. id = index into FrameAnalysis.mvs for arrows,
  // Block.id (or the cell index | 0x80000000 when the stroke is per cell) for intra strokes.
  std::vector<uint32_t> attr;
  ArrowSegments seg;

  [[nodiscard]] size_t size() const { return attr.size() / 4; }
};

enum class IntraCodec : uint8_t { Unknown, H264, H265, AV1 };

// Direction of the intra prediction of codec-numbered `mode` (chapter 6 §6.3.1 CellGrid.intra_mode_y) as a unit
// vector in image space (y down) pointing towards the reference samples. AV1 adds `angle_delta` × 3° (5.11.42,
// 7.11.2). nullopt for non-directional modes (DC, planar, plane, smooth, Paeth, PCM, palette, IntraBC) and unknown
// values.
OVT_RENDER_API std::optional<std::array<float, 2>> intra_direction(IntraCodec codec, int mode, int angle_delta = 0);

// Codec of the frame for the intra-mode numbering (FrameAnalysis carries no codec field; plugin id, filter parameter
// table and block extension names tell).
OVT_RENDER_API IntraCodec intra_codec_of(const ovt::fb::FrameAnalysis& fa);

// The functions are exported internal symbols so that render/tests can call them (as kFillPalette).

// Builds the instances for cfg.layers (MvL0/MvL1/MvBi → arrows of every MotionVector; IntraDirs → strokes). Notes
// about unusable input go to `diag`; the build never fails.
OVT_RENDER_API void build_arrow_instances(const ovt::fb::FrameAnalysis& fa, const OverlayConfig& cfg,
                                          ArrowInstances& out, std::vector<std::string>& diag);

// R-8K-1 (§10.5.8): the instances whose extent (origin to origin + vector, or the stroke) intersects `rect` (image
// coordinates x0, y0, x1, y1), with the segment order kept.
OVT_RENDER_API void cull_instances(const ArrowInstances& in, const std::array<float, 4>& rect, ArrowInstances& out);

// Instance counts above which the GPU side switches to the viewport-culled path (R-8K-1; Android/ChromeOS lower).
#if defined(__ANDROID__)
inline constexpr uint64_t kCulledBuildCells = 262144;
#else
inline constexpr uint64_t kCulledBuildCells = 1048576;
#endif

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_OVERLAY_ARROW_INSTANCES_H
