// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MV arrow and intra stroke instances (chapter 10 §10.5.1, §10.5.4, §10.5.8 R-8K-1; WI-2.434 steps 1, 4, 5, 6).

#include "overlay/arrow_instances.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "overlay/heatmap_source.h"  // parallel_ranges

namespace ovt::render::detail {
namespace {

constexpr unsigned kChunks = 4;  // §10.5.8: 4 threads, SoA -> texel

constexpr uint32_t bit(OverlayLayer l) {
  return static_cast<uint32_t>(l);
}

constexpr uint32_t kMvLayers = bit(OverlayLayer::MvL0) | bit(OverlayLayer::MvL1) | bit(OverlayLayer::MvBi);

// MotionVector.flags bits copied into the instance flags (bit1, weighted prediction, is replaced by the dash bit).
constexpr uint32_t kCopiedMvFlags = kInstBi | kInstGlobal | kInstSubBlock | kInstField;

// Segment class of a MotionVector (ArrowSegments order); kSkip = not drawn.
enum Cls : uint8_t { kL1Uni = 0, kL1Bi = 1, kL0Bi = 2, kL0Uni = 3, kSkip = 0xFF };

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

// H.265 Table 8-5 intraPredAngle for IntraPredModeY 2..34.
constexpr std::array<int, 33> kHevcAngle = {32,  26,  21,  17,  13, 9,  5,  2, 0, -2, -5, -9, -13, -17, -21, -26, -32,
                                            -26, -21, -17, -13, -9, -5, -2, 0, 2, 5,  9,  13, 17,  21,  26,  32};

std::array<float, 2> unit(float x, float y) {
  const float n = std::hypot(x, y);
  return {x / n, y / n};
}

std::optional<std::array<float, 2>> h264_direction(int mode) {
  // Intra4x4PredMode / Intra8x8PredMode 0..8 (H.264 Table 8-2 / 8-3); 16 + Intra16x16PredMode (Table 8-4).
  switch (mode) {
    case 0:   // Vertical
    case 16:  // Intra_16x16 vertical
      return unit(0.0F, -1.0F);
    case 1:   // Horizontal
    case 17:  // Intra_16x16 horizontal
      return unit(-1.0F, 0.0F);
    case 3:  // Diagonal_Down_Left: references above-right
      return unit(1.0F, -1.0F);
    case 4:  // Diagonal_Down_Right
      return unit(-1.0F, -1.0F);
    case 5:  // Vertical_Right
      return unit(-1.0F, -2.0F);
    case 6:  // Horizontal_Down
      return unit(-2.0F, -1.0F);
    case 7:  // Vertical_Left
      return unit(1.0F, -2.0F);
    case 8:  // Horizontal_Up
      return unit(-2.0F, 1.0F);
    default:  // DC (2, 18), plane (19), I_PCM (32)
      return std::nullopt;
  }
}

std::optional<std::array<float, 2>> hevc_direction(int mode) {
  if (mode < 2 || mode > 34) {
    return std::nullopt;  // planar 0, DC 1, PCM 64, palette 65
  }
  const auto a = static_cast<float>(kHevcAngle.at(static_cast<size_t>(mode - 2)));
  // Modes 2..17 are horizontal (reference column on the left), 18..34 vertical (reference row above), §8.4.4.2.6.
  return mode < 18 ? unit(-32.0F, a) : unit(a, -32.0F);
}

std::optional<std::array<float, 2>> av1_direction(int mode, int angle_delta) {
  // AV1 YMode 1..8 base angles (7.11.2 Mode_To_Angle); pAngle = base + angle_delta * ANGLE_STEP (3).
  constexpr std::array<int, 9> kBase = {0, 90, 180, 45, 135, 113, 157, 203, 67};
  if (mode < 1 || mode > 8) {
    return std::nullopt;  // DC 0, SMOOTH* 9..11, PAETH 12, IntraBC 32, palette 33
  }
  const double deg = kBase.at(static_cast<size_t>(mode)) + (3.0 * std::clamp(angle_delta, -3, 3));
  const double rad = deg * std::numbers::pi / 180.0;
  // 90° is the row above, 180° the column on the left: towards the references in y-down image space.
  return unit(static_cast<float>(std::cos(rad)), static_cast<float>(-std::sin(rad)));
}

// AngleDeltaY of an Av1Block extension, 0 when absent or not verifiable.
int av1_angle_delta(const ovt::fb::Block& b) {
  if (b.ext_type() == nullptr || b.ext_type()->string_view() != "Av1Block" || b.ext() == nullptr || b.ext()->empty()) {
    return 0;
  }
  flatbuffers::Verifier v(b.ext()->data(), b.ext()->size());
  if (!v.VerifyBuffer<ovt::fb::Av1Block>(nullptr)) {
    return 0;
  }
  return flatbuffers::GetRoot<ovt::fb::Av1Block>(b.ext()->data())->AngleDeltaY();
}

void put(ArrowInstances& out, size_t i, const std::array<float, 4>& g, uint32_t color, uint32_t flags, uint32_t id) {
  for (size_t c = 0; c < 4; ++c) {
    out.geom[(i * 4) + c] = g.at(c);
  }
  out.attr[i * 4] = color | (flags << 8U);
  out.attr[(i * 4) + 1] = id;
  out.attr[(i * 4) + 2] = 0;
  out.attr[(i * 4) + 3] = 0;
}

void put_mv(ArrowInstances& out, size_t at, const ovt::fb::MotionVector& mv, float scale, uint32_t id) {
  const ovt::fb::Rect& r = *mv.rect();
  const float ox = static_cast<float>(r.x()) + (static_cast<float>(r.w()) / 2.0F);
  const float oy = static_cast<float>(r.y()) + (static_cast<float>(r.h()) / 2.0F);
  const float vx = static_cast<float>(mv.mv_x()) / 16.0F * scale;
  const float vy = static_cast<float>(mv.mv_y()) / 16.0F * scale;
  uint32_t flags = mv.flags() & kCopiedMvFlags;
  if (mv.list() == 1) {
    flags |= kInstDashed;
  }
  uint32_t color = mv.list() == 1 ? kColorL1 : kColorL0;
  if ((flags & kInstGlobal) != 0) {
    color = kColorGlobal;
  }
  put(out, at, {ox, oy, vx, vy}, color, flags, id);
}

Cls class_of(const ovt::fb::MotionVector& mv) {
  const bool bi = (mv.flags() & kInstBi) != 0;
  if (mv.rect() == nullptr || mv.list() > 1) {
    return kSkip;
  }
  if (mv.list() == 1) {
    return bi ? kL1Bi : kL1Uni;
  }
  return bi ? kL0Bi : kL0Uni;
}

// Segment sizes and, per chunk, where the chunk's instances of each segment start (stable: chunk order = mvs order).
using ChunkCounts = std::array<std::array<size_t, 4>, kChunks>;
ChunkCounts chunk_starts(const ChunkCounts& counts, std::array<size_t, 4>& total) {
  total = {};
  for (const auto& c : counts) {
    for (size_t k = 0; k < 4; ++k) {
      total.at(k) += c.at(k);
    }
  }
  ChunkCounts start{};
  size_t seg_begin = 0;
  for (size_t k = 0; k < 4; ++k) {
    size_t at = seg_begin;
    for (size_t ch = 0; ch < kChunks; ++ch) {
      start.at(ch).at(k) = at;
      at += counts.at(ch).at(k);
    }
    seg_begin += total.at(k);
  }
  return start;
}

void build_mvs(const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::MotionVector>>& mvs, float scale,
               ArrowInstances& out, std::vector<std::string>& diag) {
  const size_t n = mvs.size();
  std::vector<uint8_t> cls(n);
  const size_t step = std::max<size_t>(1, (n + kChunks - 1) / kChunks);
  // counts[chunk][class]; parallel_ranges runs chunk k on [k * step, (k + 1) * step) or everything as chunk 0.
  ChunkCounts counts{};
  parallel_ranges(n, kChunks, [&](size_t begin, size_t end) {
    auto& cnt = counts.at(std::min<size_t>(begin / step, kChunks - 1));
    for (size_t i = begin; i < end; ++i) {
      cls[i] = class_of(*mvs.Get(static_cast<flatbuffers::uoffset_t>(i)));
      if (cls[i] != kSkip) {
        ++cnt.at(cls[i]);
      }
    }
  });
  std::array<size_t, 4> total{};
  const ChunkCounts start = chunk_starts(counts, total);
  const size_t drawn = total[0] + total[1] + total[2] + total[3];
  if (drawn < n) {
    diag.push_back(std::to_string(n - drawn) + " MotionVector(s) without rect or with list > 1 not drawn");
  }
  out.seg.l1_bi = static_cast<uint32_t>(total[0]);
  out.seg.l0_bi = static_cast<uint32_t>(total[0] + total[1]);
  out.seg.l0_uni = static_cast<uint32_t>(total[0] + total[1] + total[2]);
  out.seg.intra = static_cast<uint32_t>(drawn);
  out.geom.resize(drawn * 4);
  out.attr.resize(drawn * 4);
  parallel_ranges(n, kChunks, [&](size_t begin, size_t end) {
    auto pos = start.at(std::min<size_t>(begin / step, kChunks - 1));
    for (size_t i = begin; i < end; ++i) {
      if (cls[i] == kSkip) {
        continue;
      }
      put_mv(out, pos.at(cls[i])++, *mvs.Get(static_cast<flatbuffers::uoffset_t>(i)), scale, static_cast<uint32_t>(i));
    }
  });
}

struct Region {
  float x = 0, y = 0, w = 0, h = 0;  // NOLINT(readability-isolate-declaration)
};

void push_stroke(ArrowInstances& out, const Region& r, const std::array<float, 2>& d, uint32_t id) {
  const float half = 0.4F * std::min(r.w, r.h);  // the stroke spans 80 % of the shorter side, centred
  const float cx = r.x + (r.w / 2.0F);
  const float cy = r.y + (r.h / 2.0F);
  const std::array<float, 4> g = {cx - (half * d[0]), cy - (half * d[1]), 2.0F * half * d[0], 2.0F * half * d[1]};
  out.geom.insert(out.geom.end(), g.begin(), g.end());
  out.attr.insert(out.attr.end(), {kColorIntraStroke | (kInstIntraStroke << 8U), id, 0U, 0U});
}

// The leaf Block owning each cell (null when CellGrid.block_id is absent or names no usable Block).
class CellBlocks {
 public:
  CellBlocks(const ovt::fb::FrameAnalysis& fa, size_t cells) : blocks_(fa.blocks()) {
    ids_ = fa.grid()->block_id();
    if (ids_ != nullptr && ids_->size() != cells) {
      ids_ = nullptr;
    }
    count_ = blocks_ != nullptr ? blocks_->size() : 0;
  }
  [[nodiscard]] size_t count() const { return count_; }
  [[nodiscard]] const ovt::fb::Block* at(size_t cell) const {
    if (ids_ == nullptr) {
      return nullptr;
    }
    const uint32_t id = ids_->Get(static_cast<flatbuffers::uoffset_t>(cell));
    if (id >= count_ || blocks_->Get(id)->id() != id || blocks_->Get(id)->rect() == nullptr) {
      return nullptr;
    }
    return blocks_->Get(id);
  }

 private:
  const flatbuffers::Vector<uint32_t>* ids_ = nullptr;
  const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::Block>>* blocks_;
  size_t count_ = 0;
};

constexpr int32_t kUnset = INT32_MIN;
constexpr int32_t kMixed = INT32_MIN + 1;

// Per Block: its cells' one intra_mode_y, kMixed, or kUnset (no cell).
std::vector<int32_t> block_modes(const flatbuffers::Vector<int16_t>& modes, const CellBlocks& owner) {
  std::vector<int32_t> mode(owner.count(), kUnset);
  for (size_t i = 0; i < modes.size(); ++i) {
    const ovt::fb::Block* b = owner.at(i);
    if (b == nullptr) {
      continue;
    }
    int32_t& m = mode[b->id()];
    const int32_t v = modes.Get(static_cast<flatbuffers::uoffset_t>(i));
    m = m == kUnset ? v : (m == v ? m : kMixed);
  }
  return mode;
}

Region rect_of(const ovt::fb::Rect& r) {
  return {static_cast<float>(r.x()), static_cast<float>(r.y()), static_cast<float>(r.w()), static_cast<float>(r.h())};
}

// One stroke per leaf block whose cells share one intra_mode_y, else one per cell (the rule of the YUView export,
// chapter 6 §6.8.2 "per leaf block if uniform, else per cell"). Raster order of each block's first cell.
// SPEC-ASSUMPTION(#644): §10.5 says "short direction stroke per intra block" without a geometry or a
// mode-to-angle mapping; the stroke is a 1-px line through the block centre, 80 % of the shorter side long, along
// the prediction angle of the codec (H.264 Table 8-2, H.265 Table 8-5, AV1 Mode_To_Angle + AngleDeltaY);
// non-directional modes have none.
void build_intra(const ovt::fb::FrameAnalysis& fa, ArrowInstances& out, std::vector<std::string>& diag) {
  const ovt::fb::CellGrid& g = *fa.grid();
  const size_t cells = static_cast<size_t>(g.cells_w()) * g.cells_h();
  const auto* modes = g.intra_mode_y();
  if (modes == nullptr || modes->empty()) {
    return;
  }
  if (modes->size() != cells) {
    diag.push_back("CellGrid.intra_mode_y has " + std::to_string(modes->size()) + " entries, expected " +
                   std::to_string(cells) + "; no intra strokes");
    return;
  }
  const IntraCodec codec = intra_codec_of(fa);
  if (codec == IntraCodec::Unknown) {
    diag.emplace_back("codec of the frame unknown; no intra strokes");
    return;
  }
  const CellBlocks owner(fa, cells);
  const std::vector<int32_t> uniform = block_modes(*modes, owner);
  std::vector<uint8_t> emitted(owner.count(), 0);
  for (size_t i = 0; i < cells; ++i) {
    const int mode = modes->Get(static_cast<flatbuffers::uoffset_t>(i));
    const ovt::fb::Block* b = mode >= 0 ? owner.at(i) : nullptr;
    const int delta = (codec == IntraCodec::AV1 && b != nullptr) ? av1_angle_delta(*b) : 0;
    const auto d = mode >= 0 ? intra_direction(codec, mode, delta) : std::nullopt;
    if (!d) {
      continue;
    }
    if (b == nullptr || uniform[b->id()] != mode) {
      const size_t row = i / g.cells_w();
      const auto cx = static_cast<float>(i - (row * g.cells_w()));
      const auto cy = static_cast<float>(row);
      push_stroke(out, {cx * 4.0F, cy * 4.0F, 4.0F, 4.0F}, *d, static_cast<uint32_t>(i) | 0x80000000U);
    } else if (emitted[b->id()] == 0) {
      emitted[b->id()] = 1;
      push_stroke(out, rect_of(*b->rect()), *d, b->id());
    }
  }
}

}  // namespace

std::optional<std::array<float, 2>> intra_direction(IntraCodec codec, int mode, int angle_delta) {
  switch (codec) {
    case IntraCodec::H264:
      return h264_direction(mode);
    case IntraCodec::H265:
      return hevc_direction(mode);
    case IntraCodec::AV1:
      return av1_direction(mode, angle_delta);
    case IntraCodec::Unknown:
      break;
  }
  return std::nullopt;
}

IntraCodec intra_codec_of(const ovt::fb::FrameAnalysis& fa) {
  std::string_view names;
  std::string all;
  if (fa.filter_params_type() != nullptr) {
    all += fa.filter_params_type()->str();
  }
  if (fa.blocks() != nullptr && !fa.blocks()->empty()) {
    for (flatbuffers::uoffset_t i = 0; i < std::min<flatbuffers::uoffset_t>(fa.blocks()->size(), 4); ++i) {
      if (fa.blocks()->Get(i)->ext_type() != nullptr) {
        all += " " + fa.blocks()->Get(i)->ext_type()->str();
      }
    }
  }
  names = all;
  if (contains(names, "Av1")) {
    return IntraCodec::AV1;
  }
  if (contains(names, "Hevc")) {
    return IntraCodec::H265;
  }
  if (contains(names, "Avc")) {
    return IntraCodec::H264;
  }
  if (fa.plugin_id() != nullptr) {
    const std::string_view id = fa.plugin_id()->string_view();
    if (contains(id, "av1") || contains(id, "aom") || contains(id, "dav1d")) {
      return IntraCodec::AV1;
    }
    if (contains(id, "de265") || contains(id, "hevc") || contains(id, "265") || contains(id, ".hm")) {
      return IntraCodec::H265;
    }
    if (contains(id, "264") || contains(id, "avc") || contains(id, "jm")) {
      return IntraCodec::H264;
    }
  }
  return IntraCodec::Unknown;
}

void build_arrow_instances(const ovt::fb::FrameAnalysis& fa, const OverlayConfig& cfg, ArrowInstances& out,
                           std::vector<std::string>& diag) {
  out = ArrowInstances{};
  if ((cfg.layers & kMvLayers) != 0 && fa.mvs() != nullptr && !fa.mvs()->empty()) {
    const float scale = std::isfinite(cfg.mv_scale) ? cfg.mv_scale : 1.0F;
    build_mvs(*fa.mvs(), scale, out, diag);
  }
  out.seg.intra = static_cast<uint32_t>(out.size());
  if ((cfg.layers & bit(OverlayLayer::IntraDirs)) != 0 && fa.grid() != nullptr) {
    build_intra(fa, out, diag);
  }
  out.seg.end = static_cast<uint32_t>(out.size());
}

void cull_instances(const ArrowInstances& in, const std::array<float, 4>& rect, ArrowInstances& out) {
  out = ArrowInstances{};
  const std::array<uint32_t, 6> bounds = {0, in.seg.l1_bi, in.seg.l0_bi, in.seg.l0_uni, in.seg.intra, in.seg.end};
  std::array<uint32_t, 6> kept{};
  for (size_t s = 0; s + 1 < bounds.size(); ++s) {
    for (size_t i = bounds.at(s); i < bounds.at(s + 1); ++i) {
      const std::span<const float, 4> g(&in.geom[i * 4], 4);
      const float x0 = std::min(g[0], g[0] + g[2]);
      const float x1 = std::max(g[0], g[0] + g[2]);
      const float y0 = std::min(g[1], g[1] + g[3]);
      const float y1 = std::max(g[1], g[1] + g[3]);
      if (x1 < rect[0] || y1 < rect[1] || x0 > rect[2] || y0 > rect[3]) {
        continue;
      }
      out.geom.insert(out.geom.end(), g.begin(), g.end());
      out.attr.insert(out.attr.end(), in.attr.begin() + static_cast<std::ptrdiff_t>(i * 4),
                      in.attr.begin() + static_cast<std::ptrdiff_t>((i * 4) + 4));
    }
    kept.at(s + 1) = static_cast<uint32_t>(out.size());
  }
  out.seg = {kept[1], kept[2], kept[3], kept[4], kept[5]};
}

}  // namespace ovt::render::detail
