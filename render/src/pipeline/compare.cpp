// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Compare modes (chapter 10 §10.2.4, §10.6.4; WI-1.410): layout, flicker phase, region PSNR and the GPU passes of
// diff.glsl, dispatched as libplacebo custom shaders on the raw plane textures, before any colour conversion.

#include "pipeline/compare.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include "luts/turbo.h"  // ovt::render::luts::kTurbo (WI-2.431)
#include "pipeline/render_params.h"
#include "shaders/diff_glsl.h"  // ovt::render::shaders::kDiff, generated from src/shaders/diff.glsl

namespace ovt::render::detail {

IRect intersect(const IRect& a, const IRect& b) {
  const IRect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
  return r.empty() ? IRect{} : r;
}

CompareLayout compare_layout(const CompareMode& mode, int width, int height, bool show_b) {
  CompareLayout l;
  const IRect full{0, 0, width, height};
  l.clip_a = l.clip_b = l.view_a = l.view_b = full;
  const bool vertical = mode.split_axis == CompareMode::Axis::Vertical;
  const int extent = vertical ? width : height;
  // Splits the viewport at `at` along the axis: first part [0, at), second [at, extent).
  auto part = [&](int from, int to) { return vertical ? IRect{from, 0, to, height} : IRect{0, from, width, to}; };
  switch (mode.kind) {
    case CompareMode::Kind::Single:
      break;
    case CompareMode::Kind::SideBySide: {
      // Each half is its own viewport (same zoom and pan, locked); the divider covers one pixel of each half.
      const int half = extent / 2;
      l.draw_b = true;
      l.view_a = part(0, half);
      l.view_b = part(half, extent);
      l.clip_a = part(0, std::max(half - 1, 0));
      l.clip_b = part(std::min(half + 1, extent), extent);
      l.divider = part(std::max(half - 1, 0), std::min(half + 1, extent));
      break;
    }
    case CompareMode::Kind::Split: {
      // One viewport; A before split_position, B from it on.
      const float pos = std::clamp(mode.split_position, 0.0F, 1.0F);
      const int at = static_cast<int>(std::lround(pos * static_cast<float>(extent)));
      l.draw_b = true;
      l.clip_a = part(0, at);
      l.clip_b = part(at, extent);
      l.draw_a = !l.clip_a.empty();
      l.draw_b = !l.clip_b.empty();
      if (at > 0 && at < extent) {
        l.divider = part(std::max(at - 1, 0), std::min(at + 1, extent));
      }
      break;
    }
    case CompareMode::Kind::Overlay:
    case CompareMode::Kind::Difference:
      l.draw_b = true;
      break;
    case CompareMode::Kind::Flicker:
      l.draw_a = !show_b;
      l.draw_b = show_b;
      break;
  }
  return l;
}

bool flicker_shows_b(int64_t elapsed_ms, uint16_t period_ms) {
  if (period_ms == 0 || elapsed_ms < 0) {
    return false;
  }
  return ((elapsed_ms / period_ms) % 2) == 1;
}

bool clip_rects(pl_rect2df& src, pl_rect2df& dst, const IRect& clip) {
  const float dw = dst.x1 - dst.x0;
  const float dh = dst.y1 - dst.y0;
  if (clip.empty() || dw <= 0.0F || dh <= 0.0F) {
    return false;
  }
  const float x0 = std::max(dst.x0, static_cast<float>(clip.x0));
  const float y0 = std::max(dst.y0, static_cast<float>(clip.y0));
  const float x1 = std::min(dst.x1, static_cast<float>(clip.x1));
  const float y1 = std::min(dst.y1, static_cast<float>(clip.y1));
  if (x1 <= x0 || y1 <= y0) {
    return false;
  }
  const float sx = (src.x1 - src.x0) / dw;
  const float sy = (src.y1 - src.y0) / dh;
  const pl_rect2df s = src;
  src = pl_rect2df{s.x0 + ((x0 - dst.x0) * sx), s.y0 + ((y0 - dst.y0) * sy), s.x0 + ((x1 - dst.x0) * sx),
                   s.y0 + ((y1 - dst.y0) * sy)};
  dst = pl_rect2df{x0, y0, x1, y1};
  return true;
}

pl_rect2df map_into(const pl_rect2df& src, const ViewState& view, const IRect& view_rect) {
  const pl_rect2df r = target_rect(src, view, view_rect.width(), view_rect.height());
  const auto ox = static_cast<float>(view_rect.x0);
  const auto oy = static_cast<float>(view_rect.y0);
  return pl_rect2df{r.x0 + ox, r.y0 + oy, r.x1 + ox, r.y1 + oy};
}

IRect visible_samples(const pl_rect2df& src, const ViewState& view, const IRect& view_rect) {
  const double zoom = view.zoom > 0.0 ? view.zoom : 1.0;
  const double half_w = view_rect.width() / 2.0;
  const double half_h = view_rect.height() / 2.0;
  const IRect seen{static_cast<int>(std::floor(view.pan_x - (half_w / zoom))),
                   static_cast<int>(std::floor(view.pan_y - (half_h / zoom))),
                   static_cast<int>(std::ceil(view.pan_x + (half_w / zoom))),
                   static_cast<int>(std::ceil(view.pan_y + (half_h / zoom)))};
  const IRect pic{static_cast<int>(src.x0), static_cast<int>(src.y0), static_cast<int>(src.x1),
                  static_cast<int>(src.y1)};
  return intersect(seen, pic);
}

float region_psnr(double sse, double count, int bit_depth) {
  if (count <= 0.0 || sse < 0.0) {
    return -1.0F;
  }
  if (sse == 0.0) {
    return 99.0F;  // +inf (§10.11 WI-1.410 acceptance test 1)
  }
  const double peak = std::ldexp(1.0, bit_depth) - 1.0;
  return static_cast<float>(10.0 * std::log10((peak * peak) / (sse / count)));
}

int shader_encoding(DisplayTarget::OutputEncoding encoding) {
  switch (encoding) {
    case DisplayTarget::OutputEncoding::ScRgbLinear16F:
      return 1;
    case DisplayTarget::OutputEncoding::Pq10:
      return 2;
    case DisplayTarget::OutputEncoding::Hlg10:
      return 3;
    case DisplayTarget::OutputEncoding::Srgb8:
      break;
  }
  return 0;
}

// ---- GPU passes -----------------------------------------------------------------------------------------------------

namespace {

using F = std::vector<float>;
using I = std::vector<int>;

// Collects the descriptors, variables and prelude of one custom pass. Values live in deques so their addresses stay
// valid until the dispatch.
class Pass {
 public:
  explicit Pass(const char* defines) : prelude_(defines) {}

  void tex(const char* name, pl_tex t) {
    struct pl_shader_desc d = {};
    d.desc.name = name;
    d.desc.type = PL_DESC_SAMPLED_TEX;
    d.binding.object = t;
    d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
    descs_.push_back(d);
  }
  // Uniforms: float (1-3 components) and int (1, 2 or 4 components) variables of diff.glsl.
  void var(struct pl_var v, std::vector<float> data) { add(v, floats_.emplace_back(std::move(data)).data()); }
  void var(struct pl_var v, std::vector<int> data) { add(v, ints_.emplace_back(std::move(data)).data()); }

  // Runs `body` into `target` over `rect`; ovt_pos takes the corner values `corners` (TL, TR, BL, BR as x, y pairs).
  bool run(pl_dispatch dp, pl_gpu gpu, const char* body, pl_tex target, const IRect& rect,
           const std::array<float, 8>& corners) {
    pl_shader sh = pl_dispatch_begin(dp);
    struct pl_shader_va va = {};
    va.attr.name = "ovt_pos";
    va.attr.fmt = pl_find_vertex_fmt(gpu, PL_FMT_FLOAT, 2);
    for (size_t c = 0; c < 4; ++c) {
      va.data[c] = &corners.at(c * 2);
    }
    struct pl_custom_shader cs = {};
    cs.prelude = prelude_.c_str();
    cs.header = ovt::render::shaders::kDiff;
    cs.description = "ovt compare";
    cs.body = body;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_COLOR;
    cs.descriptors = descs_.data();
    cs.num_descriptors = static_cast<int>(descs_.size());
    cs.variables = vars_.data();
    cs.num_variables = static_cast<int>(vars_.size());
    cs.vertex_attribs = &va;
    cs.num_vertex_attribs = 1;
    if (va.attr.fmt == nullptr || !pl_shader_custom(sh, &cs)) {
      pl_dispatch_abort(dp, &sh);
      return false;
    }
    struct pl_dispatch_params dpp = {};
    dpp.shader = &sh;
    dpp.target = target;
    dpp.rect = pl_rect2d{rect.x0, rect.y0, rect.x1, rect.y1};
    return pl_dispatch_finish(dp, &dpp);
  }

 private:
  void add(struct pl_var var, const void* data) {
    struct pl_shader_var v = {};
    v.var = var;
    v.data = data;
    v.dynamic = true;
    vars_.push_back(v);
  }

  std::string prelude_;
  std::vector<struct pl_shader_desc> descs_;
  std::vector<struct pl_shader_var> vars_;
  std::deque<std::vector<float>> floats_;
  std::deque<std::vector<int>> ints_;
};

std::array<float, 8> corners_of(const IRect& r) {
  const auto x0 = static_cast<float>(r.x0);
  const auto y0 = static_cast<float>(r.y0);
  const auto x1 = static_cast<float>(r.x1);
  const auto y1 = static_cast<float>(r.y1);
  return {x0, y0, x1, y0, x0, y1, x1, y1};
}

float texture_scale(const FrameSlot& s) {
  return static_cast<float>(std::ldexp(1.0, s.frame.repr.bits.sample_depth) - 1.0);
}

bool make_tex(pl_gpu gpu, pl_tex& tex, const char* fmt_name, int w, int h, bool readable) {
  pl_fmt fmt = pl_find_named_fmt(gpu, fmt_name);
  auto need = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE);
  if (readable) {
    need = static_cast<enum pl_fmt_caps>(need | PL_FMT_CAP_HOST_READABLE);
  }
  if (fmt == nullptr || (fmt->caps & need) != need) {
    return false;
  }
  struct pl_tex_params p = {};
  p.w = w;
  p.h = h;
  p.format = fmt;
  p.sampleable = true;
  p.renderable = true;
  p.host_readable = readable;
  p.blit_dst = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
  return pl_tex_recreate(gpu, &tex, &p);
}

}  // namespace

void ComparePasses::init(pl_log log, pl_gpu gpu) {
  destroy();
  gpu_ = gpu;
  dp_ = pl_dispatch_create(log, gpu);
  // Turbo LUT for diff_false_color (§10.6.4): 256 RGBA8 entries, index 0 = minimum (§10.5).
  pl_fmt fmt = pl_find_named_fmt(gpu, "rgba8");
  if (fmt != nullptr) {
    struct pl_tex_params p = {};
    p.w = 256;
    p.h = 1;
    p.format = fmt;
    p.sampleable = true;
    p.initial_data = ovt::render::luts::kTurbo.data();
    lut_ = pl_tex_create(gpu, &p);
  }
}

void ComparePasses::destroy() {
  if (gpu_ != nullptr) {
    pl_tex_destroy(gpu_, &lut_);
    pl_tex_destroy(gpu_, &ia_);
    pl_tex_destroy(gpu_, &ib_);
    pl_tex_destroy(gpu_, &rows_);
  }
  pl_dispatch_destroy(&dp_);
  gpu_ = nullptr;
}

bool ComparePasses::difference(pl_tex target, const IRect& rect, const FrameSlot& a, const FrameSlot& b,
                               const pl_rect2df& src, const ViewState& view, const IRect& view_rect,
                               const DiffParams& p) {
  if (dp_ == nullptr || rect.empty()) {
    return false;
  }
  const bool chroma = a.frame.num_planes == 3 && b.frame.num_planes == 3;
  const bool false_color = p.false_color && lut_ != nullptr;
  std::string defines = "#define OVT_PASS_DIFF\n";
  if (chroma) {
    defines += "#define OVT_HAS_CHROMA\n";
  }
  if (false_color) {
    defines += "#define OVT_FALSE_COLOR\n";
  }
  Pass pass(defines.c_str());
  pass.tex("ovt_a0", a.tex[0]);
  pass.tex("ovt_b0", b.tex[0]);
  if (chroma) {
    pass.tex("ovt_a1", a.tex[1]);
    pass.tex("ovt_a2", a.tex[2]);
    pass.tex("ovt_b1", b.tex[1]);
    pass.tex("ovt_b2", b.tex[2]);
  }
  if (false_color) {
    pass.tex("ovt_lut", lut_);
  }
  const double zoom = view.zoom > 0.0 ? view.zoom : 1.0;
  pass.var(pl_var_vec2("ovt_pan"), F{static_cast<float>(view.pan_x), static_cast<float>(view.pan_y)});
  pass.var(pl_var_vec2("ovt_centre"), F{static_cast<float>(view_rect.x0 + (view_rect.width() / 2.0)),
                                        static_cast<float>(view_rect.y0 + (view_rect.height() / 2.0))});
  pass.var(pl_var_float("ovt_zoom"), F{static_cast<float>(zoom)});
  pass.var(pl_var_float("ovt_sa"), F{texture_scale(a)});
  pass.var(pl_var_float("ovt_sb"), F{texture_scale(b)});
  pass.var(pl_var_float("ovt_m"), F{static_cast<float>(std::ldexp(1.0, a.bit_depth) - 1.0)});
  pass.var(pl_var_float("ovt_gain"), F{p.gain});
  pass.var(pl_var_ivec4("ovt_crop"),
           I{static_cast<int>(src.x0), static_cast<int>(src.y0), static_cast<int>(src.x1), static_cast<int>(src.y1)});
  pass.var(pl_var_ivec2("ovt_sub"),
           I{(a.chroma_format == 1 || a.chroma_format == 2) ? 2 : 1, a.chroma_format == 1 ? 2 : 1});
  pass.var(pl_var_int("ovt_plane"), I{p.plane});
  pass.var(pl_var_int("ovt_signed"), I{p.signed_view ? 1 : 0});
  pass.var(pl_var_int("ovt_enc"), I{p.encoding});
  pass.var(pl_var_int("ovt_hatch"), I{p.hatch ? 1 : 0});
  return pass.run(dp_, gpu_, "color = ovt_difference();", target, rect, corners_of(rect));
}

bool ComparePasses::psnr(const FrameSlot& a, const FrameSlot& b, const IRect& vis, std::array<float, 3>& psnr) {
  psnr = {-1.0F, -1.0F, -1.0F};
  if (dp_ == nullptr || vis.empty()) {
    return false;
  }
  const int sx = (a.chroma_format == 1 || a.chroma_format == 2) ? 2 : 1;
  const int sy = a.chroma_format == 1 ? 2 : 1;
  const int planes = std::min(a.frame.num_planes, b.frame.num_planes);
  for (int i = 0; i < planes; ++i) {
    // The visible region at plane resolution (partially visible chroma samples count).
    const IRect r = i == 0 ? vis : IRect{vis.x0 / sx, vis.y0 / sy, (vis.x1 + sx - 1) / sx, (vis.y1 + sy - 1) / sy};
    if (r.empty() || !make_tex(gpu_, rows_, "r32f", 1, r.height(), true)) {
      return false;
    }
    Pass pass("#define OVT_PASS_PSNR\n");
    pass.tex("ovt_pa", a.tex.at(static_cast<size_t>(i)));
    pass.tex("ovt_pb", b.tex.at(static_cast<size_t>(i)));
    pass.var(pl_var_float("ovt_sa"), F{texture_scale(a)});
    pass.var(pl_var_float("ovt_sb"), F{texture_scale(b)});
    pass.var(pl_var_int("ovt_x0"), I{r.x0});
    pass.var(pl_var_int("ovt_x1"), I{r.x1});
    const IRect out{0, 0, 1, r.height()};
    const auto y0 = static_cast<float>(r.y0);
    const auto y1 = static_cast<float>(r.y1);
    if (!pass.run(dp_, gpu_, "color = ovt_psnr_row();", rows_, out, {0.0F, y0, 1.0F, y0, 0.0F, y1, 1.0F, y1})) {
      return false;
    }
    std::vector<float> sums(static_cast<size_t>(r.height()));
    struct pl_tex_transfer_params tp = {};
    tp.tex = rows_;
    tp.row_pitch = sizeof(float);
    tp.ptr = sums.data();
    if (!pl_tex_download(gpu_, &tp)) {
      return false;
    }
    double sse = 0.0;
    for (const float s : sums) {
      sse += static_cast<double>(s);
    }
    psnr.at(static_cast<size_t>(i)) =
        region_psnr(sse, static_cast<double>(r.width()) * static_cast<double>(r.height()), a.bit_depth);
  }
  return true;
}

bool ComparePasses::intermediates(int width, int height, pl_tex& ia, pl_tex& ib) {
  if (gpu_ == nullptr || !make_tex(gpu_, ia_, "rgba16hf", width, height, false) ||
      !make_tex(gpu_, ib_, "rgba16hf", width, height, false)) {
    return false;
  }
  const union pl_clear_color zero = {};
  pl_tex_clear_ex(gpu_, ia_, zero);
  pl_tex_clear_ex(gpu_, ib_, zero);
  ia = ia_;
  ib = ib_;
  return true;
}

bool ComparePasses::blend(pl_tex target, const IRect& rect, float alpha, int encoding) {
  if (dp_ == nullptr || ia_ == nullptr || ib_ == nullptr || rect.empty()) {
    return false;
  }
  Pass pass("#define OVT_PASS_BLEND\n");
  pass.tex("ovt_ia", ia_);
  pass.tex("ovt_ib", ib_);
  pass.var(pl_var_float("ovt_alpha"), F{std::clamp(alpha, 0.0F, 1.0F)});
  pass.var(pl_var_int("ovt_enc"), I{encoding});
  return pass.run(dp_, gpu_, "color = ovt_blend();", target, rect, corners_of(rect));
}

bool ComparePasses::fill_white(pl_tex target, const IRect& rect, int encoding) {
  if (dp_ == nullptr || rect.empty()) {
    return false;
  }
  Pass pass("#define OVT_PASS_FILL\n");
  pass.var(pl_var_int("ovt_enc"), I{encoding});
  return pass.run(dp_, gpu_, "color = ovt_fill();", target, rect, corners_of(rect));
}

}  // namespace ovt::render::detail
