// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// GPU HDR measurements of picture A (chapter 10 §10.7.1-§10.7.4 and §10.7.9, WI-4.452): the linear measurement
// texture M, the per-pixel quantities Q, the MaxCLL / FALL / min reduction chain, the 1024-bin PQ histogram with its
// 99.99th percentile, and the double-buffered, non-blocking readback behind VideoSurface::measure(). WI-4.453 adds the
// out-of-gamut flags and counts, the false-colour band fractions and the two analysis views drawn from Q (§10.7.5,
// §10.7.8). Internal to libovt-render; the CPU helpers are exported for the render tests.

#ifndef OVT_RENDER_SRC_MEASURE_MEASURE_H
#define OVT_RENDER_SRC_MEASURE_MEASURE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"
#include "pipeline/frame_upload.h"

namespace ovt::render::detail {

// ---- §10.7.3 constants and CPU helpers ----------------------------------------------------------------------------
inline constexpr uint32_t kHistogramBins = 1024;
inline constexpr uint64_t kMaxHistogramSamples = 2073600;  // 1920 x 1080
inline constexpr uint32_t kReduceStep = 8;                 // 8x8 -> 1 per reduction pass
inline constexpr int kFalseColorBands = 6;                 // Table 10.7-1
// Counters accumulated next to the histogram (accumulate.glsl ovt_counts): the six bands, then the gamut flags.
inline constexpr uint32_t kCountOutside709 = 6;
inline constexpr uint32_t kCountOutsideP3 = 7;
inline constexpr uint32_t kCounters = 8;

// s = max(1, ceil(sqrt(pixels / 2 073 600))): 1080p -> 1, 4K -> 2, 8K -> 4.
OVT_RENDER_API uint32_t subsample_factor(uint64_t pixels);
// Samples along one axis of `extent` pixels at factor s: one per s-pixel block, the last block may be partial.
OVT_RENDER_API uint32_t sampled_extent(uint32_t extent, uint32_t s);
// Texture sizes of the reduction chain over a w x h Q: each level is ceil(previous / 8), down to and including 1x1.
OVT_RENDER_API std::vector<std::array<int, 2>> reduction_chain(int width, int height);

// ST 2084: PQ code in [0, 1] -> nits (EOTF), and nits -> PQ code (inverse EOTF). Inputs are clamped to the domain.
OVT_RENDER_API double pq_eotf_nits(double code);
OVT_RENDER_API double pq_oetf(double nits);

// §10.7.3 percentile: the smallest bin i whose cumulative count is >= 0.9999 x sampled, reported as
// PQ_EOTF((i + 1) / 1024) x 10000. 0 when nothing was sampled.
OVT_RENDER_API float histogram_p9999_nits(const std::array<uint32_t, kHistogramBins>& histogram, uint32_t sampled);

// ---- §10.7.1 linear-light texture ---------------------------------------------------------------------------------
// The source colour space as measured: HLG without a mastering peak is linearised at Lw = 1000 (§10.7.1).
OVT_RENDER_API struct pl_color_space measurement_source_color(const struct pl_color_space& source);
// M's colour space: source primaries, linear transfer, peak 10000 nits (HLG: the source's Lw, which libplacebo uses
// for the HLG OOTF), and the source's own black level, so that libplacebo neither tone-maps nor adapts the black point.
OVT_RENDER_API struct pl_color_space measurement_target_color(const struct pl_color_space& source);

// SPEC-ASSUMPTION(#473): M is the full crop size (exact MaxCLL); s only picks the histogram's sample grid.
// Renders the conformance-window crop of `slot` 1:1 into *m (recreated as RGBA16F at the crop size): linear light,
// 1.0 = 203 nits. Returns false and sets `err` on failure. (linear_pass.cpp)
bool render_linear(pl_gpu gpu, pl_renderer renderer, const FrameSlot& slot, pl_tex* m, std::string& err);

// ---- §10.7.2 / §10.7.3 passes (reduce.cpp, histogram.cpp) -----------------------------------------------------------
// measure_prep.frag: M -> *q (RGBA16F, M's size). `source` is picture A's colour space (primaries for Y and the gamut
// flags, transfer and peak for gamut_peak()).
bool prep_pass(pl_dispatch dp, pl_gpu gpu, pl_tex m, const struct pl_color_space& source, pl_tex* q, std::string& err);
// SPEC-ASSUMPTION(#473): one RGBA32F target per level instead of RG32F + R32F.
// reduce8.frag over Q, then over each level, into `levels` (RGBA32F; the last is 1x1 and host-readable). The final
// texel is (max, sum, min, 0) of max(R,G,B) nits over every pixel of Q.
bool reduce_chain(pl_dispatch dp, pl_gpu gpu, pl_tex q, std::vector<pl_tex>& levels, std::string& err);
// accumulate.glsl: adds the PQ bins of Q's samples at factor s to `hist` (uint32[1024] at offset 0) and the band and
// gamut counts to the kCounters uint32 that follow (offset 4096); both cleared by the caller.
bool accumulate_histogram(pl_dispatch dp, pl_tex q, uint32_t s, pl_buf hist, std::string& err);
// Destroys the textures of `levels` and clears it.
void destroy_levels(pl_gpu gpu, std::vector<pl_tex>& levels);

// ---- §10.7.5 false colour and §10.7.8 gamut clip (false_color.cpp, WI-4.453) ------------------------------------
enum class AnalysisView : uint8_t { None, FalseColor, GamutClip };
// The view a ViewState asks for: false colour wins over the gamut-clip overlay (both replace the picture).
OVT_RENDER_API AnalysisView analysis_view(const ViewState& view);
// Table 10.7-1 band of max(R,G,B) nits (lower bounds inclusive) and its sRGB colour; band 1 is the log grey ramp.
OVT_RENDER_API int false_color_band(double nits);
OVT_RENDER_API std::array<uint8_t, 3> false_color_rgb(double nits);
// §10.7.8 overlay colour of a pixel: grey = PQ(Y), tinted cyan (outside P3) or magenta (outside BT.709) at 0x99.
OVT_RENDER_API std::array<uint8_t, 3> gamut_clip_rgb(double y_nits, bool outside_709, bool outside_p3);
// The container peak in M units (1.0 = 203 nits) above which a component counts as out of gamut: 1.0 for SDR, Lw/203
// for HLG, 10000/203 for PQ.
OVT_RENDER_API float gamut_peak(const struct pl_color_space& source);
// Linear RGB of `from` -> linear RGB of `to` via XYZ, column-major for a GLSL mat3.
OVT_RENDER_API std::array<float, 9> gamut_matrix(enum pl_color_primaries from, enum pl_color_primaries to);
// false_color.frag: Q -> *out (RGBA8, Q's size), sRGB-encoded BT.709 values of the view `mode`.
bool analysis_pass(pl_dispatch dp, pl_gpu gpu, pl_tex q, AnalysisView mode, pl_tex* out, std::string& err);

// ---- §10.7.9 orchestration (measurer.cpp) -------------------------------------------------------------------------
// Owns the measurement resources of one surface. Confined to the surface's render thread, like the surface.
class OVT_RENDER_API Measurer {
 public:
  Measurer() = default;
  Measurer(const Measurer&) = delete;
  Measurer& operator=(const Measurer&) = delete;
  Measurer(Measurer&&) = delete;
  Measurer& operator=(Measurer&&) = delete;
  ~Measurer();

  // Creates the renderer, dispatch and buffers on `gpu`. Returns false (and leaves the measurer unavailable, so
  // measure() never becomes valid) when the GPU lacks a capability the passes need; error() says which.
  bool init(pl_log log, pl_gpu gpu);
  // Frees every GPU object. Must run before the pl_gpu is destroyed.
  void destroy();

  // measure() was called: the next two renders measure (§10.7.9).
  void request() { renders_since_request_ = 0; }
  // render(): true when a consumer asked within the last two renders; counts this render.
  bool due();
  // render() starts: M and Q are computed at most once per render, for the view and the measurement together.
  void begin_render() { prepared_ = false; }
  // §10.7.5 / §10.7.8: the analysis view of picture A, an RGBA8 texture of the crop size holding sRGB-encoded BT.709
  // (owned by the measurer, valid until the next call). nullptr when the measurer is unavailable or a pass failed;
  // error() says why.
  pl_tex analysis_view(const FrameSlot& slot, AnalysisView mode);
  // Queues the passes and the downloads for picture A after the display render. Skips (keeping the previous result)
  // when both readback buffers are still in flight.
  void run(const FrameSlot& slot);
  // Collects every completed readback without blocking and returns the newest result (valid = false until one).
  Measurements poll();

  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  // One readback in flight: the histogram (offset 0), the band and gamut counters (offset kCountsOffset) and the final
  // reduction texel (offset kTexelOffset) of a frame.
  struct Readback {
    pl_buf buf = nullptr;
    bool pending = false;
    uint64_t seq = 0;
    ovt::fb::FrameId frame;
    uint64_t pixels = 0;
    uint32_t sampled = 0;
  };
  static constexpr size_t kHistogramBytes = kHistogramBins * sizeof(uint32_t);
  static constexpr size_t kCountsOffset = kHistogramBytes;
  static constexpr size_t kCountsBytes = kCounters * sizeof(uint32_t);
  static constexpr size_t kTexelOffset = kCountsOffset + kCountsBytes;
  static constexpr size_t kReadbackBytes = kTexelOffset + (4 * sizeof(float));

  void collect(Readback& rb);
  // Renders M and Q for `slot` unless this render already did.
  bool prepare(const FrameSlot& slot, std::string& err);
  bool fail(const std::string& what);
  void report(const std::string& err);

  pl_gpu gpu_ = nullptr;
  pl_renderer renderer_ = nullptr;
  pl_dispatch dispatch_ = nullptr;
  pl_tex m_ = nullptr;
  pl_tex q_ = nullptr;
  pl_tex view_ = nullptr;  // analysis view (RGBA8)
  bool prepared_ = false;
  std::vector<pl_tex> levels_;
  std::array<Readback, 2> readbacks_{};
  uint64_t next_seq_ = 1;
  uint64_t shown_seq_ = 0;
  int renders_since_request_ = 2;  // nothing asked yet
  bool available_ = false;
  std::string error_;
  Measurements last_{};
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_MEASURE_MEASURE_H
