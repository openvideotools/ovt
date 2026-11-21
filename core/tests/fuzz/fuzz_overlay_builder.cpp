// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the overlay builder's CPU side (chapter 10 §10.11, chapter 14 §14.7.1; issue #649): untrusted
// FrameAnalysis bytes through detail::build_overlay_set, the body of OverlayBuilder::build(buf, size, …) without a
// VideoSurface (no GPU), with every layer bit set, then the R-8K-1 viewport cut. Byte 0 picks the heatmap source,
// byte 1 the arrow scale and cull rectangle; the rest is the FlatBuffer, copied so that it is aligned. A rejected
// buffer is a normal outcome; a crash, sanitizer report, timeout or a set that breaks its invariants is a finding.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

#include "overlay/arrow_instances.h"
#include "overlay/overlay_set_impl.h"

namespace {

using ovt::render::detail::ArrowInstances;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

void check_arrows(const ArrowInstances& a) {  // five consecutive segments (arrow_instances.h)
  const auto& s = a.seg;
  require(a.geom.size() == a.attr.size() && a.attr.size() % 4 == 0 && s.end == a.size());
  require(s.l1_bi <= s.l0_bi && s.l0_bi <= s.l0_uni && s.l0_uni <= s.intra && s.intra <= s.end);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> in(data, size);
  if (in.size() < 2) {
    return 0;
  }
  ovt::render::OverlayConfig cfg;
  cfg.layers = 0x3FFFFU;  // every OverlayLayer bit (§10.2.5); Custom (needs a host array) is left out of the sources
  cfg.heatmap = static_cast<ovt::render::HeatmapSource>(in[0] % static_cast<int>(ovt::render::HeatmapSource::Custom));
  constexpr std::array<float, 4> kScales = {1.0F, 0.25F, 4.0F, 0.0F};
  cfg.mv_scale = kScales.at(in[1] & 3U);
  const auto body = in.subspan(2);
  const std::vector<std::uint8_t> fb(body.begin(), body.end());  // operator new: aligned for the Verifier
  const auto set = ovt::render::detail::build_overlay_set(fb.data(), fb.size(), cfg, nullptr);
  if (set == nullptr) {
    return 0;
  }
  const std::size_t cells = static_cast<std::size_t>(set->cells_w) * set->cells_h;
  require(set->cell_attributes.empty() || set->cell_attributes.size() == cells * 4);
  require((set->heatmap.empty() || set->heatmap.size() == cells) && set->heat_valid.size() == set->heatmap.size());
  check_arrows(set->arrows);
  const auto span = static_cast<float>(4U << (in[1] >> 4U));
  const float x0 = static_cast<float>((in[1] >> 2U) & 3U) * span;
  ArrowInstances culled;
  ovt::render::detail::cull_instances(set->arrows, {x0, x0, x0 + span, x0 + span}, culled);
  check_arrows(culled);
  require(culled.size() <= set->arrows.size());
  return 0;
}
