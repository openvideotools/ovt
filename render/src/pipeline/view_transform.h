// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The view transform of chapter 10 §10.2.3 (WI-1.409): device pixel <-> image coordinate, the whole-pixel snapping of
// the target rectangle at zoom >= 1, the host's pan clamp, the zoom filter rule R-ZOOM-1, the sample grid R-ZOOM-4 and
// the sample under the cursor (R-ZOOM-3's value, for the status bar / Selection Info). Pure CPU code, no libplacebo
// calls: the surface, the Picture panel's controller and the tests all use these functions, so there is one transform.
// Internal to libovt-render.

#ifndef OVT_RENDER_SRC_PIPELINE_VIEW_TRANSFORM_H
#define OVT_RENDER_SRC_PIPELINE_VIEW_TRANSFORM_H

#include <cstdint>
#include <optional>
#include <vector>

#include <libplacebo/common.h>

#include "ovt/render/picture.h"
#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

struct Point {
  double x = 0;
  double y = 0;
};

// A viewport in device pixels.
struct ViewportSize {
  int w = 0;
  int h = 0;
};

// The effective zoom: a non-positive value (the host's "fit" placeholder) counts as 1.0, as in target_rect().
OVT_RENDER_API double effective_zoom(const ViewState& view);

// Zoom thresholds (R-ZOOM-1, R-ZOOM-4, R-ZOOM-3).
inline constexpr double kNearestZoom = 1.0;       // zoom >= 1: nearest neighbour and whole-pixel snapping
inline constexpr double kSampleGridZoom = 8.0;    // zoom >= 8: 1-px sample grid
inline constexpr double kSampleValueZoom = 24.0;  // zoom >= 24: sample-value text (WI-2.433, not drawn here)

// R-ZOOM-1: nearest-neighbour from zoom 1.0 upwards, the ViewState downscale filter below.
enum class ZoomFilter : uint8_t { Nearest, Downscale };
OVT_RENDER_API ZoomFilter zoom_filter(double zoom);

// Whole-pixel snapping of a device coordinate: round-half-up at zoom >= 1, unchanged below. The forward transform
// (image_to_device, snap_rect) and the inverse (sample_at_device) share it, so a sample's device pixels and the sample
// a device pixel selects always agree.
OVT_RENDER_API double snap_device(double device, double zoom);

// §10.2.3: ix = pan_x + (px - w/2) / zoom. Continuous, no snapping: device pixel (px, py) is the point (px, py), not
// the pixel's centre.
OVT_RENDER_API Point device_to_image(const ViewState& view, ViewportSize vp, double px, double py);

// The forward mapping, device = (image - pan) * zoom + viewport / 2, snapped to whole device pixels at zoom >= 1.
OVT_RENDER_API Point image_to_device(const ViewState& view, ViewportSize vp, double ix, double iy);

// The same mapping applied to a rectangle (target.crop). At zoom >= 1 every edge is snapped, so nearest-neighbour
// sampling gives every sample the same whole number of device pixels (§10.2.3 notes, WI-1.409).
OVT_RENDER_API pl_rect2df snap_rect(const pl_rect2df& device, double zoom);

// The sample (luma coordinates) whose snapped device footprint contains device pixel (px, py): the inverse of
// image_to_device() with the same rounding. Always defined; the caller tests the result against the picture.
struct SampleIndex {
  int x = 0;
  int y = 0;
};
OVT_RENDER_API SampleIndex sample_at_device(const ViewState& view, ViewportSize vp, int px, int py);

// §10.2.3: the host keeps at least 64 device pixels of the picture visible (fewer when the picture or the viewport is
// smaller). `area` is the picture rectangle in luma samples. Returns `view` with pan_x/pan_y clamped; a viewport of
// zero size leaves the pan alone.
inline constexpr double kMinVisiblePx = 64.0;
struct ImageArea {
  double x = 0, y = 0, w = 0, h = 0;  // NOLINT(readability-isolate-declaration)
};
OVT_RENDER_API double clamp_pan_axis(double pan, double origin, double extent, double viewport, double zoom);
OVT_RENDER_API ViewState clamp_view(const ViewState& view, ViewportSize vp, const ImageArea& area);

// R-ZOOM-4: the 1-px sample grid, `#808080` at alpha 0x66, from zoom 8 up.
inline constexpr bool sample_grid_visible(double zoom) {
  return zoom >= kSampleGridZoom;
}

// Device pixel columns (x) and rows (y) of the sample grid lines inside `picture` (the picture's device rectangle,
// whole pixels, already clipped to the viewport): the left / top device pixel of every sample whose edge falls inside
// it, plus the last pixel line of the right / bottom picture edge. Empty below zoom 8.
struct SampleGridLines {
  std::vector<int> columns;
  std::vector<int> rows;
};
OVT_RENDER_API SampleGridLines sample_grid_lines(const ViewState& view, ViewportSize vp, const pl_rect2df& source,
                                                 const pl_rect2df& picture);

// Premultiplied RGBA8 of the grid for `vp` (R-ZOOM-4: #808080 at alpha 0x66 = 51 51 51 102; crossings are written
// once, not blended twice), rows top first, vp.w * vp.h * 4 bytes. All zero when no line is drawn.
OVT_RENDER_API std::vector<uint8_t> rasterize_sample_grid(const SampleGridLines& lines, ViewportSize vp,
                                                          const pl_rect2df& picture);

// The sample under the cursor (R-ZOOM-3 value, shown in the status bar / Selection Info): the code values of
// `plane` at luma sample (x, y), the chroma plane's sample that covers it for planes 1 and 2. nullopt when the sample
// lies outside the plane or the plane has no data. 8-bit planes (sample_bytes 1) and 9..16-bit planes (2, little
// endian) are read as stored.
OVT_RENDER_API std::optional<uint32_t> sample_value(const Picture& pic, int plane, int x, int y);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PIPELINE_VIEW_TRANSFORM_H
