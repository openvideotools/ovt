// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The view transform of chapter 10 §10.2.3 (WI-1.409). See view_transform.h.

#include "pipeline/view_transform.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <libplacebo/common.h>

#include "ovt/render/picture.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

namespace {

constexpr int kSnapSteps = 4;  // an inverse correction never moves more than one sample; the bound only stops a NaN

// R-ZOOM-4 colour #808080 at alpha 0x66, premultiplied: 0x80 * 0x66 / 255 = 51.2.
constexpr uint8_t kGridRgb = 51;
constexpr uint8_t kGridAlpha = 0x66;

int floor_int(double v) {
  return static_cast<int>(std::floor(v));
}

// The device coordinate of image coordinate `i` along one axis: (i - pan) * zoom + viewport / 2, snapped.
double edge(double i, double pan, double zoom, double half) {
  return snap_device(((i - pan) * zoom) + half, zoom);
}

// Sample index of device pixel `p` along one axis, the inverse of edge() with the same rounding.
int sample_axis(double pan, double zoom, double half, int p) {
  const double dev = static_cast<double>(p);
  int i = floor_int(pan + ((dev - half) / zoom));
  for (int k = 0; k < kSnapSteps && edge(static_cast<double>(i) + 1.0, pan, zoom, half) <= dev; ++k) {
    ++i;
  }
  for (int k = 0; k < kSnapSteps && edge(static_cast<double>(i), pan, zoom, half) > dev; ++k) {
    --i;
  }
  return i;
}

// The lines of one axis: the snapped device pixel of every sample edge in [lo, hi) that lies in [dev_lo, dev_hi),
// then the last device pixel of the far picture edge `hi`.
std::vector<int> axis_lines(double pan, double zoom, double half, double lo, double hi, double dev_lo, double dev_hi) {
  std::vector<int> out;
  if (!(dev_hi > dev_lo) || !(hi > lo)) {
    return out;
  }
  const double first = std::max(lo, std::floor(pan + ((dev_lo - half) / zoom)) - 1.0);
  const double last = std::min(hi, std::ceil(pan + ((dev_hi - half) / zoom)) + 1.0);
  for (double i = first; i <= last; i += 1.0) {
    const bool far_edge = i >= hi;
    if (far_edge && i > hi) {
      break;
    }
    double d = edge(i, pan, zoom, half);
    if (far_edge) {
      d -= 1.0;  // the picture's right / bottom edge: the last device pixel of the last sample
    }
    if (d >= dev_lo && d < dev_hi) {
      out.push_back(static_cast<int>(d));
    }
  }
  return out;
}

}  // namespace

double effective_zoom(const ViewState& view) {
  return view.zoom > 0.0 ? view.zoom : 1.0;
}

ZoomFilter zoom_filter(double zoom) {
  return zoom >= kNearestZoom ? ZoomFilter::Nearest : ZoomFilter::Downscale;
}

double snap_device(double device, double zoom) {
  return zoom >= kNearestZoom ? std::floor(device + 0.5) : device;
}

Point device_to_image(const ViewState& view, ViewportSize vp, double px, double py) {
  const double zoom = effective_zoom(view);
  return Point{view.pan_x + ((px - (vp.w / 2.0)) / zoom), view.pan_y + ((py - (vp.h / 2.0)) / zoom)};
}

Point image_to_device(const ViewState& view, ViewportSize vp, double ix, double iy) {
  const double zoom = effective_zoom(view);
  return Point{edge(ix, view.pan_x, zoom, vp.w / 2.0), edge(iy, view.pan_y, zoom, vp.h / 2.0)};
}

pl_rect2df snap_rect(const pl_rect2df& device, double zoom) {
  auto snap = [zoom](float v) { return static_cast<float>(snap_device(static_cast<double>(v), zoom)); };
  return pl_rect2df{snap(device.x0), snap(device.y0), snap(device.x1), snap(device.y1)};
}

SampleIndex sample_at_device(const ViewState& view, ViewportSize vp, int px, int py) {
  const double zoom = effective_zoom(view);
  return SampleIndex{sample_axis(view.pan_x, zoom, vp.w / 2.0, px), sample_axis(view.pan_y, zoom, vp.h / 2.0, py)};
}

double clamp_pan_axis(double pan, double origin, double extent, double viewport, double zoom) {
  if (viewport <= 0.0 || zoom <= 0.0) {
    return pan;
  }
  // device = (image - pan) * zoom + viewport / 2: the right edge stays at or right of m, the left edge at or left of
  // viewport - m.
  const double m = std::min({kMinVisiblePx, extent * zoom, viewport});
  const double lo = origin + ((m - (viewport / 2.0)) / zoom);
  const double hi = origin + extent - ((m - (viewport / 2.0)) / zoom);
  return lo <= hi ? std::clamp(pan, lo, hi) : origin + (extent / 2.0);
}

ViewState clamp_view(const ViewState& view, ViewportSize vp, const ImageArea& area) {
  ViewState out = view;
  const double zoom = effective_zoom(view);
  out.pan_x = clamp_pan_axis(view.pan_x, area.x, area.w, vp.w, zoom);
  out.pan_y = clamp_pan_axis(view.pan_y, area.y, area.h, vp.h, zoom);
  return out;
}

SampleGridLines sample_grid_lines(const ViewState& view, ViewportSize vp, const pl_rect2df& source,
                                  const pl_rect2df& picture) {
  SampleGridLines lines;
  const double zoom = effective_zoom(view);
  if (!sample_grid_visible(zoom) || vp.w <= 0 || vp.h <= 0) {
    return lines;
  }
  const double dx0 = std::max(0.0, std::floor(static_cast<double>(std::min(picture.x0, picture.x1))));
  const double dx1 =
      std::min(static_cast<double>(vp.w), std::ceil(static_cast<double>(std::max(picture.x0, picture.x1))));
  const double dy0 = std::max(0.0, std::floor(static_cast<double>(std::min(picture.y0, picture.y1))));
  const double dy1 =
      std::min(static_cast<double>(vp.h), std::ceil(static_cast<double>(std::max(picture.y0, picture.y1))));
  lines.columns = axis_lines(view.pan_x, zoom, vp.w / 2.0, source.x0, source.x1, dx0, dx1);
  lines.rows = axis_lines(view.pan_y, zoom, vp.h / 2.0, source.y0, source.y1, dy0, dy1);
  return lines;
}

std::vector<uint8_t> rasterize_sample_grid(const SampleGridLines& lines, ViewportSize vp, const pl_rect2df& picture) {
  std::vector<uint8_t> px;
  if (vp.w <= 0 || vp.h <= 0) {
    return px;
  }
  px.assign(static_cast<size_t>(vp.w) * static_cast<size_t>(vp.h) * 4, 0);
  const int x0 = std::max(0, floor_int(static_cast<double>(std::min(picture.x0, picture.x1))));
  const int x1 = std::min(vp.w, static_cast<int>(std::ceil(static_cast<double>(std::max(picture.x0, picture.x1)))));
  const int y0 = std::max(0, floor_int(static_cast<double>(std::min(picture.y0, picture.y1))));
  const int y1 = std::min(vp.h, static_cast<int>(std::ceil(static_cast<double>(std::max(picture.y0, picture.y1)))));
  auto put = [&px, &vp](int x, int y) {
    const size_t at = ((static_cast<size_t>(y) * static_cast<size_t>(vp.w)) + static_cast<size_t>(x)) * 4;
    px.at(at) = kGridRgb;
    px.at(at + 1) = kGridRgb;
    px.at(at + 2) = kGridRgb;
    px.at(at + 3) = kGridAlpha;
  };
  for (const int x : lines.columns) {
    if (x >= x0 && x < x1) {
      for (int y = y0; y < y1; ++y) {
        put(x, y);
      }
    }
  }
  for (const int y : lines.rows) {
    if (y >= y0 && y < y1) {
      for (int x = x0; x < x1; ++x) {
        put(x, y);
      }
    }
  }
  return px;
}

std::optional<uint32_t> sample_value(const Picture& pic, int plane, int x, int y) {
  if (plane < 0 || plane >= pic.num_planes || plane > 2 || x < 0 || y < 0 || pic.coded_width == 0 ||
      pic.coded_height == 0 || x >= static_cast<int>(pic.coded_width) || y >= static_cast<int>(pic.coded_height)) {
    return std::nullopt;
  }
  const PlaneData& pd = pic.planes[plane];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): plane checked
  if (pd.data == nullptr || pd.width == 0 || pd.height == 0 || pd.sample_bytes < 1 || pd.sample_bytes > 2) {
    return std::nullopt;
  }
  // The plane's sample covering luma sample (x, y): chroma planes are subsampled by plane size / luma size.
  const auto cx = static_cast<size_t>(x) * pd.width / pic.coded_width;
  const auto cy = static_cast<size_t>(y) * pd.height / pic.coded_height;
  const auto* base = static_cast<const uint8_t*>(pd.data);
  const size_t at = (cy * pd.stride_bytes) + (cx * pd.sample_bytes);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (pd.sample_bytes == 1) {
    return uint32_t{base[at]};
  }
  return uint32_t{base[at]} | (uint32_t{base[at + 1]} << 8U);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

}  // namespace ovt::render::detail
