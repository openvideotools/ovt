// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Luminance waveform, vectorscope and CIE xy scopes of picture A (chapter 10 §10.7.6-§10.7.7, WI-4.454a): the
// coordinate mappings (CPU reference of render/src/shaders/scopes.glsl) and ScopeSet, which accumulates the scopes from
// the measurement texture M on the GPU and hands them to the host as ScopeTexture (§10.2.6) through the surface's
// interop. The overlay point tables, the CIE 1931 locus and the ExportKind::Scope*Png export are WI-4.454b. Internal to
// libovt-render; the CPU helpers are exported for the render tests.
//
// Texture content (SPEC-ASSUMPTION(#512)): ExternalTexture formats have no R32F, so each scope is an RGBA16F texture
// holding log2(1 + count) per channel (R = G = B for a single-channel scope; R, G, B = the three parade channels) and
// A = 1. The GUI's display LUT (§10.7.6) works on that value directly.

#ifndef OVT_RENDER_SRC_MEASURE_SCOPES_H
#define OVT_RENDER_SRC_MEASURE_SCOPES_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"
#include "platform/offscreen.h"

namespace ovt::render::detail {

// ---- §10.7.6 / §10.7.7 sizes and configuration ----------------------------------------------------------------------
inline constexpr int kWaveformWidth = 1024;
inline constexpr int kWaveformHeight = 512;
inline constexpr int kScopeSize = 512;  // vectorscope and CIE xy are square

enum class ScopeKind : uint8_t { Waveform, Vectorscope, CieXy };
inline constexpr int kScopeKinds = 3;

// SPEC-ASSUMPTION(#512): MeasureConfig (§10.7.6 "ViewState-adjacent") is not part of the §10.2 public interface yet;
// the surface measures with the defaults until a public setter exists. The tests set it through Measurer.
enum class WaveformAxis : uint8_t {
  LogNits,  // row = 511 x (log10(clamp(v, 1e-4, 1e4)) + 4) / 8 (default)
  PqCode    // row = 511 x PQ(v)
};
enum class WaveformChannels : uint8_t {
  Luminance,  // CIE Y in nits
  RgbParade   // R, G and B in nits, one texture channel each
};
struct ScopeConfig {
  WaveformAxis waveform_axis = WaveformAxis::LogNits;
  WaveformChannels waveform_channels = WaveformChannels::Luminance;
  float vectorscope_gain = 1.0F;  // 1 or 2 (§10.7.7)
};

// ---- CPU reference of the coordinate mappings (scopes.glsl computes the same in float) -----------------------------
// §10.7.6 column of luma sample x (0 <= x < crop_w): floor(x / crop_w x 1024), in integers.
OVT_RENDER_API int waveform_column(uint32_t x, uint32_t crop_w);
// §10.7.6 row of a value in nits, 0 (darkest) ... 511 (brightest), floor of the spec's real-valued row.
OVT_RENDER_API int waveform_row(double nits, WaveformAxis axis);
// SPEC-ASSUMPTION(#512): the texture puts the brightest row at the top, like the vectorscope and CIE xy images, whose
// v axis also grows downwards: texel y = 511 - row.
OVT_RENDER_API int waveform_texel_y(int row);
// §10.7.7 BT.2020 non-constant-luminance (Cb, Cr) of linear BT.2020 RGB in nits: R'G'B' = PQ(rgb / 10000).
OVT_RENDER_API std::array<double, 2> vectorscope_cbcr(const std::array<double, 3>& rgb2020_nits);
// §10.7.7 plot position (u, v) of (Cb, Cr): u = 256 + Cb x 512 x gain, v = 256 - Cr x 512 x gain.
OVT_RENDER_API std::array<double, 2> vectorscope_uv(double cb, double cr, float gain);
// §10.7.7 plot position (u, v) of a chromaticity: u = x / 0.8 x 512, v = (1 - y / 0.9) x 512.
OVT_RENDER_API std::array<double, 2> cie_uv(double x, double y);
// The texel a plot position lands in, or {-1, -1} when it is off the plot. Positions within one texel of the edge are
// clamped onto it, so a 100 % colour (|Cb| or |Cr| = 0.5 at gain 1) is not lost to float rounding; anything further
// out (gain 2) is dropped, as a point outside the viewport would be.
OVT_RENDER_API std::array<int, 2> scope_texel(double u, double v, int size);

// Rows of the content RGB -> XYZ matrix and of the content RGB -> BT.2020 RGB matrix (via XYZ), row-major. BT.2020
// content gets the exact identity.
OVT_RENDER_API std::array<float, 9> rgb_to_xyz_rows(enum pl_color_primaries primaries);
OVT_RENDER_API std::array<float, 9> rgb_to_bt2020_rows(enum pl_color_primaries primaries);

// ---- GPU accumulation (scopes.cpp) ----------------------------------------------------------------------------------
// The scope textures of one surface: two sets, one per readback slot of the Measurer, so the textures measure() hands
// out belong to the frame its numbers describe. Confined to the render thread.
class OVT_RENDER_API ScopeSet {
 public:
  ScopeSet() = default;
  ScopeSet(const ScopeSet&) = delete;
  ScopeSet& operator=(const ScopeSet&) = delete;
  ScopeSet(ScopeSet&&) = delete;
  ScopeSet& operator=(ScopeSet&&) = delete;
  ~ScopeSet();

  // `host` creates the textures with the target's interop (nullptr: plain textures on `gpu`, for tests). Nothing is
  // allocated until the first run().
  void init(pl_gpu gpu, OffscreenGpu* host);
  void destroy();

  // Accumulates the three scopes of M (linear light, `primaries`, 1.0 = 203 nits) sampled at factor s into texture set
  // `set` (0 or 1). Returns false and sets `err` on failure.
  bool run(pl_dispatch dp, pl_tex m, enum pl_color_primaries primaries, uint32_t s, int set, const ScopeConfig& config,
           std::string& err);
  // Whether `set` holds a completed accumulation, and its textures as the host sees them.
  [[nodiscard]] bool valid(int set) const;
  [[nodiscard]] ScopeTexture texture(int set, ScopeKind kind) const;
  // The pl_tex behind a scope texture (for the readback of the tests and export_png); nullptr before the first run of
  // `set`.
  [[nodiscard]] pl_tex tex(int set, ScopeKind kind) const;

 private:
  bool ensure(std::string& err);
  bool create_texture(HostTexture& t, int w, int h, std::string& err);
  void destroy_texture(HostTexture& t);

  pl_gpu gpu_ = nullptr;
  OffscreenGpu* host_ = nullptr;
  pl_buf counts_ = nullptr;  // uint32 counts: 3 waveform planes, the vectorscope, the CIE xy plot
  std::array<std::array<HostTexture, kScopeKinds>, 2> sets_{};
  std::array<bool, 2> valid_{};
};

// Sets the scope options of `surface` (the VideoSurface of create()) for its next measurements; false for another
// implementation. Defined in render/src/surface/video_surface_impl.cpp.
OVT_RENDER_API bool set_scope_config(VideoSurface& surface, const ScopeConfig& config);
// Waits for the GPU and copies the `kind` scope texture of the newest measure() result of `surface` (RGBA16F texels,
// rows top first). Returns false when there is none. Defined in render/src/surface/video_surface_impl.cpp.
OVT_RENDER_API bool download_scope(VideoSurface& surface, ScopeKind kind, std::vector<uint16_t>& rgba16hf, int& width,
                                   int& height);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_MEASURE_SCOPES_H
