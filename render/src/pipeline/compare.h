// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Compare modes (chapter 10 §10.2.4, §10.6.4; WI-1.410): slot B preparation (RND-0010/RND-0011), the viewport layout
// of every CompareMode kind, the flicker phase, the region PSNR and the GPU passes (difference, PSNR row sums,
// overlay blend, dividers). Internal to libovt-render; the CPU parts are exported for the render tests.

#ifndef OVT_RENDER_SRC_PIPELINE_COMPARE_H
#define OVT_RENDER_SRC_PIPELINE_COMPARE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"
#include "pipeline/frame_upload.h"

namespace ovt::render::detail {

// Integer device-pixel rectangle [x0, x1) x [y0, y1).
struct IRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // NOLINT(readability-isolate-declaration)
  [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
  [[nodiscard]] int width() const { return x1 - x0; }
  [[nodiscard]] int height() const { return y1 - y0; }
};

OVT_RENDER_API IRect intersect(const IRect& a, const IRect& b);

// ---- slot B (§10.6.4 "Mismatch states") -------------------------------------------------------------------------
// The format of slot A that B is converted to.
struct SlotFormat {
  bool valid = false;              // false: slot A is empty, B is uploaded as given
  uint32_t width = 0, height = 0;  // NOLINT(readability-isolate-declaration)
  uint8_t chroma_format = 1;
  uint8_t bit_depth = 8;
  uint8_t sample_bytes = 1;
  ovt::fb::Rect crop;
};

OVT_RENDER_API SlotFormat slot_format(const FrameSlot& slot);

// What set_compare() uploads to slot B: the caller's picture or a converted copy that owns its samples.
struct PreparedB {
  Picture pic;
  std::vector<std::vector<uint8_t>> storage;
  bool size_mismatch = false;  // RND-0010: B is uploaded at its own size and the difference view shows a hatch
  bool converted = false;      // chroma format, bit depth or size changed to A's
  std::string message;         // the RND-0010 / RND-0011 line for last_error(), "" when B matches A
};

// Converts B into A's coded grid (§10.6.4): chroma format by nearest sampling (missing chroma = neutral), bit depth by
// shifting, size by bilinear resampling when allow_scale_b. Returns "" or the RND-0001 message of an invalid B.
OVT_RENDER_API std::string prepare_b(const Picture& b, const SlotFormat& a, bool allow_scale_b, PreparedB& out);

// ---- layout ------------------------------------------------------------------------------------------------------
// Where A and B are drawn. Each side has a clip rectangle and the viewport rectangle whose centre the §10.2.3 mapping
// uses (SideBySide: each half, else the whole viewport). Dividers: 2 device px of #FFFFFF. SPEC-ASSUMPTION(#478).
struct CompareLayout {
  bool draw_a = true;
  bool draw_b = false;
  IRect clip_a, clip_b;  // NOLINT(readability-isolate-declaration)
  IRect view_a, view_b;  // NOLINT(readability-isolate-declaration)
  IRect divider;         // empty when there is none
};

// `show_b` selects the Flicker phase (flicker_shows_b()); other kinds ignore it.
OVT_RENDER_API CompareLayout compare_layout(const CompareMode& mode, int width, int height, bool show_b);

// Flicker: A for one period, then B for one period (§10.6.4); `elapsed_ms` counts from set_compare().
OVT_RENDER_API bool flicker_shows_b(int64_t elapsed_ms, uint16_t period_ms);

// Narrows `dst` to `clip` and `src` by the same proportion (the scissor of §10.11 WI-1.410 step 2). False when empty.
OVT_RENDER_API bool clip_rects(pl_rect2df& src, pl_rect2df& dst, const IRect& clip);

// The §10.2.3 mapping of `src` into the viewport rectangle `view` (target_rect() offset to view's origin).
OVT_RENDER_API pl_rect2df map_into(const pl_rect2df& src, const ViewState& view, const IRect& view_rect);

// Luma samples of the picture rectangle `src` that are visible in `view_rect` (partially covered samples count).
OVT_RENDER_API IRect visible_samples(const pl_rect2df& src, const ViewState& view, const IRect& view_rect);

// PSNR_p = 10 log10((2^bd - 1)^2 / MSE) in code values; +inf (MSE 0) is reported as 99.0, no samples as -1.
OVT_RENDER_API float region_psnr(double sse, double count, int bit_depth);

// ---- GPU passes --------------------------------------------------------------------------------------------------
// Output encoding index of the shader (diff.glsl): 0 sRGB code values, 1 linear, 2 PQ, 3 HLG.
OVT_RENDER_API int shader_encoding(DisplayTarget::OutputEncoding encoding);

struct DiffParams {
  float gain = 1.0F;
  int plane = -1;
  bool signed_view = false;
  bool false_color = false;
  bool hatch = false;
  int encoding = 0;
};

class OVT_RENDER_API ComparePasses {
 public:
  ComparePasses() = default;
  ComparePasses(const ComparePasses&) = delete;
  ComparePasses& operator=(const ComparePasses&) = delete;
  ComparePasses(ComparePasses&&) = delete;
  ComparePasses& operator=(ComparePasses&&) = delete;
  ~ComparePasses() = default;

  void init(pl_log log, pl_gpu gpu);
  void destroy();

  // Difference of A and B into `target` over `rect`; the image mapping is the §10.2.3 one of `view_rect`.
  bool difference(pl_tex target, const IRect& rect, const FrameSlot& a, const FrameSlot& b, const pl_rect2df& src,
                  const ViewState& view, const IRect& view_rect, const DiffParams& p);

  // Region PSNR of A against B over the visible luma samples `vis` (per plane at plane resolution). Fills psnr[3];
  // planes that cannot be measured stay -1.
  bool psnr(const FrameSlot& a, const FrameSlot& b, const IRect& vis, std::array<float, 3>& psnr);

  // Linear FP16 intermediates of the overlay blend, the size of the target (§10.6.4 Overlay).
  bool intermediates(int width, int height, pl_tex& ia, pl_tex& ib);
  bool blend(pl_tex target, const IRect& rect, float alpha, int encoding);

  // Fills `rect` with SDR white in the output encoding.
  bool fill_white(pl_tex target, const IRect& rect, int encoding);

 private:
  pl_gpu gpu_ = nullptr;
  pl_dispatch dp_ = nullptr;
  pl_tex lut_ = nullptr;
  pl_tex ia_ = nullptr;
  pl_tex ib_ = nullptr;
  pl_tex rows_ = nullptr;
};

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PIPELINE_COMPARE_H
