// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// §10.7.6 luminance waveform and §10.7.7 vectorscope / CIE xy (WI-4.454): three GPU passes per measured render,
// all reading the measurement texture M at the sample grid of §10.7.3:
//   scope_clear.glsl   (compute)  zero the uint32 counts,
//   scopes.glsl        (compute)  add one count per sampled pixel to each scope (integer atomics,
//   SPEC-ASSUMPTION(#473)), scope_resolve.frag (fragment) counts -> the RGBA16F scope textures the host samples
//   (ScopeTexture, §10.7.9).
// Also the CPU reference of the mappings and the PNG of ExportKind::Scope*Png (§10.8).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include "measure/measure.h"
#include "measure/scopes.h"
#include "scope_shaders.h"  // generated from render/src/shaders/ (render/CMakeLists.txt)

namespace ovt::render::detail {

namespace {

constexpr uint32_t kWaveformPlane = static_cast<uint32_t>(kWaveformWidth) * kWaveformHeight;
constexpr uint32_t kPlotPlane = static_cast<uint32_t>(kScopeSize) * kScopeSize;
constexpr uint32_t kVectorscopeBase = 3 * kWaveformPlane;
constexpr uint32_t kCieBase = kVectorscopeBase + kPlotPlane;
constexpr uint32_t kCountsLength = kCieBase + kPlotPlane;  // 2 097 152 counts, 8 MiB
constexpr int kGroupSize = 16;                             // scopes.glsl: 16 x 16 work groups
constexpr int kClearGroup = 256;                           // scope_clear.glsl: 256 x 1 work groups
constexpr double kLog10Of2 = 0.30102999566398120;

std::array<float, 9> rows_of(const pl_matrix3x3& m) {
  return {m.m[0][0], m.m[0][1], m.m[0][2], m.m[1][0], m.m[1][1], m.m[1][2], m.m[2][0], m.m[2][1], m.m[2][2]};
}

std::array<std::array<float, 3>, 3> split_rows(const std::array<float, 9>& m) {
  return {{{m[0], m[1], m[2]}, {m[3], m[4], m[5]}, {m[6], m[7], m[8]}}};
}

struct pl_shader_var dyn_var(struct pl_var v, const void* data) {
  struct pl_shader_var sv = {};
  sv.var = v;
  sv.data = data;
  sv.dynamic = true;
  return sv;
}

struct pl_buffer_var counts_var() {
  struct pl_buffer_var counts = {};
  counts.var = pl_var_uint("ovt_counts");
  counts.var.dim_a = static_cast<int>(kCountsLength);
  counts.layout = pl_std430_layout(0, &counts.var);
  return counts;
}

// The storage-buffer descriptor of the counts; the block name differs from its member (pl_shader_custom #defines).
struct pl_shader_desc counts_desc(pl_buf buf, struct pl_buffer_var* var, enum pl_desc_access access) {
  struct pl_shader_desc d = {};
  d.desc.name = "ovt_counts_block";
  d.desc.type = PL_DESC_BUF_STORAGE;
  d.desc.access = access;
  d.binding.object = buf;
  d.buffer_vars = var;
  d.num_buffer_vars = 1;
  return d;
}

bool clear_pass(pl_dispatch dp, pl_buf counts, std::string& err) {
  const auto n = static_cast<int>(kCountsLength);
  struct pl_buffer_var var = counts_var();
  const struct pl_shader_desc desc = counts_desc(counts, &var, PL_DESC_ACCESS_WRITEONLY);
  const std::array<struct pl_shader_var, 2> vars = {dyn_var(pl_var_int("ovt_n"), &n),
                                                    dyn_var(pl_var_int("ovt_row"), &n)};

  pl_shader sh = pl_dispatch_begin(dp);
  struct pl_custom_shader custom = {};
  custom.header = static_cast<const char*>(kShaderScopeClear);
  custom.body = "ovt_scope_clear();";
  custom.input = PL_SHADER_SIG_NONE;
  custom.output = PL_SHADER_SIG_NONE;
  custom.descriptors = &desc;
  custom.num_descriptors = 1;
  custom.variables = vars.data();
  custom.num_variables = static_cast<int>(vars.size());
  custom.compute = true;
  custom.compute_group_size[0] = kClearGroup;
  custom.compute_group_size[1] = 1;
  custom.description = "ovt scope clear (§10.7.6)";
  if (!pl_shader_custom(sh, &custom)) {
    pl_dispatch_abort(dp, &sh);
    err = "measurement: scope clear shader rejected";
    return false;
  }
  struct pl_dispatch_compute_params cparams = {};
  cparams.shader = &sh;
  cparams.dispatch_size[0] = (n + kClearGroup - 1) / kClearGroup;
  cparams.dispatch_size[1] = 1;
  cparams.dispatch_size[2] = 1;
  if (!pl_dispatch_compute(dp, &cparams)) {
    err = "measurement: scope clear pass failed";
    return false;
  }
  return true;
}

bool accumulate_pass(pl_dispatch dp, pl_tex m, enum pl_color_primaries primaries, uint32_t s, const ScopeConfig& config,
                     pl_buf counts, std::string& err) {
  const auto step = static_cast<int>(s);
  const std::array<int, 2> grid = {static_cast<int>(sampled_extent(static_cast<uint32_t>(m->params.w), s)),
                                   static_cast<int>(sampled_extent(static_cast<uint32_t>(m->params.h), s))};
  const int crop_w = m->params.w;
  const std::array<std::array<float, 3>, 3> xyz = split_rows(rgb_to_xyz_rows(primaries));
  const std::array<std::array<float, 3>, 3> b2020 = split_rows(rgb_to_bt2020_rows(primaries));
  const int axis = config.waveform_axis == WaveformAxis::PqCode ? 1 : 0;
  const int parade = config.waveform_channels == WaveformChannels::RgbParade ? 1 : 0;
  const float gain = config.vectorscope_gain;

  std::array<struct pl_shader_desc, 2> descs = {};
  descs[0].desc.name = "ovt_m";
  descs[0].desc.type = PL_DESC_SAMPLED_TEX;
  descs[0].binding.object = m;
  descs[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
  struct pl_buffer_var var = counts_var();
  descs[1] = counts_desc(counts, &var, PL_DESC_ACCESS_READWRITE);

  const std::array<struct pl_shader_var, 12> vars = {dyn_var(pl_var_int("ovt_step"), &step),
                                                     dyn_var(pl_var_ivec2("ovt_grid"), grid.data()),
                                                     dyn_var(pl_var_int("ovt_crop_w"), &crop_w),
                                                     dyn_var(pl_var_vec3("ovt_xyz_x"), xyz[0].data()),
                                                     dyn_var(pl_var_vec3("ovt_xyz_y"), xyz[1].data()),
                                                     dyn_var(pl_var_vec3("ovt_xyz_z"), xyz[2].data()),
                                                     dyn_var(pl_var_vec3("ovt_b2020_r"), b2020[0].data()),
                                                     dyn_var(pl_var_vec3("ovt_b2020_g"), b2020[1].data()),
                                                     dyn_var(pl_var_vec3("ovt_b2020_b"), b2020[2].data()),
                                                     dyn_var(pl_var_int("ovt_wf_axis"), &axis),
                                                     dyn_var(pl_var_int("ovt_wf_parade"), &parade),
                                                     dyn_var(pl_var_float("ovt_vs_gain"), &gain)};

  pl_shader sh = pl_dispatch_begin(dp);
  struct pl_custom_shader custom = {};
  custom.header = static_cast<const char*>(kShaderScopes);
  custom.body = "ovt_scopes();";
  custom.input = PL_SHADER_SIG_NONE;
  custom.output = PL_SHADER_SIG_NONE;
  custom.descriptors = descs.data();
  custom.num_descriptors = static_cast<int>(descs.size());
  custom.variables = vars.data();
  custom.num_variables = static_cast<int>(vars.size());
  custom.compute = true;
  custom.compute_group_size[0] = kGroupSize;
  custom.compute_group_size[1] = kGroupSize;
  custom.description = "ovt scopes (§10.7.6-§10.7.7)";
  if (!pl_shader_custom(sh, &custom)) {
    pl_dispatch_abort(dp, &sh);
    err = "measurement: scopes shader rejected";
    return false;
  }
  struct pl_dispatch_compute_params cparams = {};
  cparams.shader = &sh;
  cparams.dispatch_size[0] = (grid[0] + kGroupSize - 1) / kGroupSize;
  cparams.dispatch_size[1] = (grid[1] + kGroupSize - 1) / kGroupSize;
  cparams.dispatch_size[2] = 1;
  if (!pl_dispatch_compute(dp, &cparams)) {
    err = "measurement: scopes pass failed";
    return false;
  }
  return true;
}

bool resolve_pass(pl_dispatch dp, pl_buf counts, uint32_t base, int planes, uint32_t stride, pl_tex out,
                  std::string& err) {
  const auto base_i = static_cast<int>(base);
  const int width = out->params.w;
  const auto stride_i = static_cast<int>(stride);
  struct pl_buffer_var var = counts_var();
  const struct pl_shader_desc desc = counts_desc(counts, &var, PL_DESC_ACCESS_READONLY);
  const std::array<struct pl_shader_var, 4> vars = {
      dyn_var(pl_var_int("ovt_base"), &base_i), dyn_var(pl_var_int("ovt_width"), &width),
      dyn_var(pl_var_int("ovt_planes"), &planes), dyn_var(pl_var_int("ovt_stride"), &stride_i)};

  pl_shader sh = pl_dispatch_begin(dp);
  struct pl_custom_shader custom = {};
  custom.header = static_cast<const char*>(kShaderScopeResolve);
  custom.body = "color = ovt_scope_resolve();";
  custom.input = PL_SHADER_SIG_NONE;
  custom.output = PL_SHADER_SIG_COLOR;
  custom.descriptors = &desc;
  custom.num_descriptors = 1;
  custom.variables = vars.data();
  custom.num_variables = static_cast<int>(vars.size());
  custom.output_w = out->params.w;
  custom.output_h = out->params.h;
  custom.description = "ovt scope resolve (§10.7.6-§10.7.7)";
  if (!pl_shader_custom(sh, &custom)) {
    pl_dispatch_abort(dp, &sh);
    err = "measurement: scope resolve shader rejected";
    return false;
  }
  struct pl_dispatch_params dparams = {};
  dparams.shader = &sh;
  dparams.target = out;
  if (!pl_dispatch_finish(dp, &dparams)) {
    err = "measurement: scope resolve pass failed";
    return false;
  }
  return true;
}

// IEEE binary16 -> float (normal, subnormal, inf and NaN).
float half_to_float(uint16_t h) {
  const uint32_t sign = (h >> 15U) & 1U;
  const uint32_t exp = (h >> 10U) & 0x1FU;
  const uint32_t mant = h & 0x3FFU;
  float v = 0;
  if (exp == 0) {
    v = std::ldexp(static_cast<float>(mant), -24);
  } else if (exp == 31) {
    v = mant == 0 ? INFINITY : NAN;
  } else {
    v = std::ldexp(static_cast<float>(mant | 0x400U), static_cast<int>(exp) - 25);
  }
  return sign != 0 ? -v : v;
}

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      }
      t.at(i) = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc = table.at((crc ^ data[i]) & 0xFFU) ^ (crc >> 8U);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    out.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(&out[start], out.size() - start, 0));
}

}  // namespace

// ---- CPU reference --------------------------------------------------------------------------------------------------

int waveform_column(uint32_t x, uint32_t crop_w) {
  if (crop_w == 0) {
    return 0;
  }
  return static_cast<int>(std::min<uint64_t>((static_cast<uint64_t>(x) * kWaveformWidth) / crop_w,
                                             static_cast<uint64_t>(kWaveformWidth - 1)));
}

int waveform_row(double nits, WaveformAxis axis) {
  double f = 0;
  if (axis == WaveformAxis::LogNits) {
    f = 511.0 * ((std::log2(std::clamp(nits, 1.0e-4, 1.0e4)) * kLog10Of2) + 4.0) / 8.0;
  } else {
    f = 511.0 * pq_oetf(nits);
  }
  return static_cast<int>(std::clamp(std::floor(f), 0.0, 511.0));
}

int waveform_texel_y(int row) {
  return (kWaveformHeight - 1) - row;
}

std::array<double, 2> vectorscope_cbcr(const std::array<double, 3>& rgb2020_nits) {
  std::array<double, 3> e{};
  for (size_t i = 0; i < 3; ++i) {
    e.at(i) = pq_oetf(std::max(rgb2020_nits.at(i), 0.0));
  }
  const double yp = (0.2627 * e[0]) + (0.6780 * e[1]) + (0.0593 * e[2]);
  return {(e[2] - yp) / 1.8814, (e[0] - yp) / 1.4746};
}

std::array<double, 2> vectorscope_uv(double cb, double cr, float gain) {
  const double g = static_cast<double>(gain) * kScopeSize;
  return {256.0 + (cb * g), 256.0 - (cr * g)};
}

std::array<double, 2> cie_uv(double x, double y) {
  return {x / 0.8 * kScopeSize, (1.0 - (y / 0.9)) * kScopeSize};
}

std::array<int, 2> scope_texel(double u, double v, int size) {
  const auto edge = static_cast<double>(size);
  if (std::isnan(u) || std::isnan(v) || u < -1.0 || v < -1.0 || u > edge + 1.0 || v > edge + 1.0) {
    return {-1, -1};
  }
  return {std::clamp(static_cast<int>(std::floor(u)), 0, size - 1),
          std::clamp(static_cast<int>(std::floor(v)), 0, size - 1)};
}

std::array<float, 9> rgb_to_xyz_rows(enum pl_color_primaries primaries) {
  return rows_of(pl_get_rgb2xyz_matrix(pl_raw_primaries_get(primaries)));
}

std::array<float, 9> rgb_to_bt2020_rows(enum pl_color_primaries primaries) {
  if (primaries == PL_COLOR_PRIM_BT_2020) {
    return {1, 0, 0, 0, 1, 0, 0, 0, 1};  // exact: no rounding of a matrix product moves BT.2020 red off its point
  }
  pl_matrix3x3 to2020 = pl_get_xyz2rgb_matrix(pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020));
  const pl_matrix3x3 to_xyz = pl_get_rgb2xyz_matrix(pl_raw_primaries_get(primaries));
  pl_matrix3x3_mul(&to2020, &to_xyz);  // to2020 := XYZ->2020 x content->XYZ
  return rows_of(to2020);
}

// ---- ScopeSet -------------------------------------------------------------------------------------------------------

ScopeSet::~ScopeSet() {
  destroy();
}

void ScopeSet::init(pl_gpu gpu, OffscreenGpu* host) {
  destroy();
  gpu_ = gpu;
  host_ = host;
}

void ScopeSet::destroy() {
  if (gpu_ != nullptr) {
    for (auto& set : sets_) {
      for (HostTexture& t : set) {
        destroy_texture(t);
      }
    }
    pl_buf_destroy(gpu_, &counts_);
  }
  valid_ = {};
  gpu_ = nullptr;
  host_ = nullptr;
}

bool ScopeSet::create_texture(HostTexture& t, int w, int h, std::string& err) {
  if (host_ != nullptr) {
    std::string reason;
    if (!host_->create_extra(t, w, h, DisplayTarget::OffscreenFormat::Rgba16F, reason)) {
      err = "measurement: cannot create a scope texture: " + reason;
      return false;
    }
    return true;
  }
  struct pl_tex_params tp = {};
  tp.w = w;
  tp.h = h;
  tp.format = pl_find_named_fmt(gpu_, "rgba16hf");
  tp.sampleable = true;
  tp.renderable = true;
  tp.host_readable = tp.format != nullptr && (tp.format->caps & PL_FMT_CAP_HOST_READABLE) != 0;
  if (tp.format == nullptr || !pl_tex_recreate(gpu_, &t.tex, &tp)) {
    err = "measurement: cannot create an RGBA16F scope texture";
    return false;
  }
  t.width = w;
  t.height = h;
  t.format = DisplayTarget::OffscreenFormat::Rgba16F;
  return true;
}

void ScopeSet::destroy_texture(HostTexture& t) {
  if (host_ != nullptr) {
    host_->destroy_extra(t);
  } else {
    pl_tex_destroy(gpu_, &t.tex);
  }
  t = HostTexture{};
}

bool ScopeSet::ensure(std::string& err) {
  if (counts_ == nullptr) {
    struct pl_buf_params bp = {};
    bp.size = static_cast<size_t>(kCountsLength) * sizeof(uint32_t);
    bp.storable = true;
    counts_ = pl_buf_create(gpu_, &bp);
    if (counts_ == nullptr) {
      err = "measurement: cannot create the scope count buffer";
      return false;
    }
  }
  for (auto& set : sets_) {
    if (set[0].tex == nullptr && (!create_texture(set[0], kWaveformWidth, kWaveformHeight, err) ||
                                  !create_texture(set[1], kScopeSize, kScopeSize, err) ||
                                  !create_texture(set[2], kScopeSize, kScopeSize, err))) {
      return false;
    }
  }
  return true;
}

bool ScopeSet::run(pl_dispatch dp, pl_tex m, enum pl_color_primaries primaries, uint32_t s, int set,
                   const ScopeConfig& config, std::string& err) {
  if (gpu_ == nullptr || set < 0 || set > 1) {
    err = "measurement: scopes not initialised";
    return false;
  }
  const auto i = static_cast<size_t>(set);
  valid_.at(i) = false;
  if (!ensure(err) || !clear_pass(dp, counts_, err) || !accumulate_pass(dp, m, primaries, s, config, counts_, err)) {
    return false;
  }
  const int wf_planes = config.waveform_channels == WaveformChannels::RgbParade ? 3 : 1;
  auto& textures = sets_.at(i);
  if (!resolve_pass(dp, counts_, 0, wf_planes, kWaveformPlane, textures[0].tex, err) ||
      !resolve_pass(dp, counts_, kVectorscopeBase, 1, 0, textures[1].tex, err) ||
      !resolve_pass(dp, counts_, kCieBase, 1, 0, textures[2].tex, err)) {
    return false;
  }
  valid_.at(i) = true;
  return true;
}

bool ScopeSet::valid(int set) const {
  return set >= 0 && set <= 1 && valid_.at(static_cast<size_t>(set));
}

pl_tex ScopeSet::tex(int set, ScopeKind kind) const {
  if (set < 0 || set > 1) {
    return nullptr;
  }
  return sets_.at(static_cast<size_t>(set)).at(static_cast<size_t>(kind)).tex;
}

ScopeTexture ScopeSet::texture(int set, ScopeKind kind) const {
  ScopeTexture st{};
  st.valid = valid(set);
  if (set < 0 || set > 1) {
    return st;
  }
  const HostTexture& t = sets_.at(static_cast<size_t>(set)).at(static_cast<size_t>(kind));
  if (host_ != nullptr) {
    st.tex = host_->describe(t);
  } else {
    st.tex.interop = DisplayTarget::Interop::None;
    st.tex.handle =
        const_cast<void*>(static_cast<const void*>(t.tex));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    st.tex.handle2 = nullptr;
    st.tex.width = t.width;
    st.tex.height = t.height;
    st.tex.format = t.format;
    st.tex.layout = 0;
  }
  st.valid = st.valid && st.tex.handle != nullptr;
  return st;
}

// ---- export ---------------------------------------------------------------------------------------------------------

std::vector<uint8_t> scope_png_rgb(const std::vector<uint16_t>& rgba16hf, int width, int height) {
  const auto pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  std::vector<uint8_t> rgb(pixels * 3, 0);
  if (rgba16hf.size() < pixels * 4) {
    return rgb;
  }
  float peak = 0;
  for (size_t p = 0; p < pixels; ++p) {
    for (size_t c = 0; c < 3; ++c) {
      const float v = half_to_float(rgba16hf[(p * 4) + c]);
      if (std::isfinite(v)) {
        peak = std::max(peak, v);
      }
    }
  }
  if (peak <= 0) {
    return rgb;
  }
  for (size_t p = 0; p < pixels; ++p) {
    for (size_t c = 0; c < 3; ++c) {
      const float v = std::clamp(half_to_float(rgba16hf[(p * 4) + c]) / peak, 0.0F, 1.0F);
      rgb[(p * 3) + c] = static_cast<uint8_t>(std::lround(v * 255.0F));
    }
  }
  return rgb;
}

bool write_png_rgb8(const char* path, const std::vector<uint8_t>& rgb, int width, int height) {
  if (path == nullptr || width <= 0 || height <= 0 ||
      rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3) {
    return false;
  }
  const size_t row = (static_cast<size_t>(width) * 3) + 1;  // filter byte 0 + RGB
  std::vector<uint8_t> raw;
  raw.reserve(row * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    const auto start = static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
    raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(start),
               rgb.begin() + static_cast<std::ptrdiff_t>(start + (static_cast<size_t>(width) * 3)));
  }
  // zlib stream of stored deflate blocks (RFC 1950/1951): no compression, so no zlib dependency.
  std::vector<uint8_t> z = {0x78, 0x01};
  constexpr size_t kBlock = 65535;
  for (size_t at = 0; at < raw.size() || at == 0; at += kBlock) {
    const size_t n = std::min(kBlock, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n & 0xFFU));
    z.push_back(static_cast<uint8_t>(n >> 8U));
    z.push_back(static_cast<uint8_t>(~n & 0xFFU));
    z.push_back(static_cast<uint8_t>((~n >> 8U) & 0xFFU));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
    if (last) {
      break;
    }
  }
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t v : raw) {
    a = (a + v) % 65521U;
    b = (b + a) % 65521U;
  }
  put32(z, (b << 16U) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put32(ihdr, static_cast<uint32_t>(width));
  put32(ihdr, static_cast<uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // bit depth 8, colour type 2 (RGB), deflate, filter 0, no interlace
  put_chunk(png, "IHDR", ihdr);
  put_chunk(png, "IDAT", z);
  put_chunk(png, "IEND", {});

  const std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path, "wb"), &std::fclose);
  if (f == nullptr) {
    return false;
  }
  return std::fwrite(png.data(), 1, png.size(), f.get()) == png.size();
}

}  // namespace ovt::render::detail
