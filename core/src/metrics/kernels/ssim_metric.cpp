// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `ssim` metric (chapter 9 §9.4.3, §9.4.7; WI-3.307) behind ovt_metric_v1: Wang et al. SSIM with the 11 x 11
// Gaussian window (sigma 1.5), per plane, float32 convolutions with a double reduction, and per-block maps.
//
//   values (per frame) = [y, u, v]       (descriptor.outputs, in this order; NaN for planes not computed)
//   values (finish)    = none            (sequence pooling is WI-3.311)
//
// Border policy (option "border"): "valid" is Wang's ssim.m on the whole plane, output (W-10) x (H-10); "libvmaf"
// (the default) is libvmaf 3.0's float_ssim: the same valid convolution after libvmaf's down-scaling,
// f = max(1, round(min(W, H) / 256)), which averages f x f boxes (mirror border, output size
// (W/f + (W & 1)) x (H/f + (H & 1)) as in iqa/decimate.c) before filtering, and libvmaf's 6-digit tap table. Both
// work on samples normalised to the 8-bit range with L = 255, which is Wang's definition with
// C1 = (0.01 * peak)^2, C2 = (0.03 * peak)^2 on the original code values ("valid": factor 255 / peak; "libvmaf":
// libvmaf's divide by 2^(bit_depth - 8)). docs/metrics_differences.md lists every difference to libvmaf.
//
// SPEC-ASSUMPTION(#407): "border" has the two values "valid" and "libvmaf", "libvmaf" is the default, and the
// descriptor version string names the default policy because ovt_MetricApi.descriptor() takes no options.
// SPEC-ASSUMPTION(#407): the default for "planes" is ["y"] ("Luma by default", §9.4.3); a plane smaller than the
// 11 x 11 window (after libvmaf's down-scaling) makes score() fail with OVT_E_INVALID_ARG.

#include "metrics/kernels/ssim_metric.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/ssim_kernels.h"
#include "ovt_metrics_generated.h"

namespace {

enum class Border : uint8_t { kLibvmaf, kValid };

constexpr std::array<std::string_view, 3> kComponentNames = {"y", "u", "v"};
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Options {
  std::array<bool, 3> planes = {true, false, false};
  Border border = Border::kLibvmaf;
  uint32_t map_block = 0;  // 0 = no maps
};

bool parse_planes(const nlohmann::json& value, std::array<bool, 3>& planes) {
  if (!value.is_array() || value.empty()) {
    return false;
  }
  planes = {false, false, false};
  for (const auto& p : value) {
    if (!p.is_string()) {
      return false;
    }
    const auto* const it = std::ranges::find(kComponentNames, p.get<std::string>());
    if (it == kComponentNames.end()) {
      return false;
    }
    planes[static_cast<size_t>(it - kComponentNames.begin())] = true;
  }
  return true;
}

bool parse_map_block(const nlohmann::json& value, uint32_t& block) {
  // "ctu" is resolved by the host to a number before the options reach the kernel (§9.4.7).
  if (value.is_null()) {
    block = 0;
    return true;
  }
  if (!value.is_number_integer()) {
    return false;
  }
  const auto v = value.get<int64_t>();
  if (v != 8 && v != 16 && v != 32 && v != 64) {
    return false;
  }
  block = static_cast<uint32_t>(v);
  return true;
}

bool parse_options(const char* text, Options& out) {
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) {
    return false;
  }
  for (const auto& [key, value] : j.items()) {
    if (key == "planes") {
      if (!parse_planes(value, out.planes)) {
        return false;
      }
    } else if (key == "border") {
      if (!value.is_string() || (value.get<std::string>() != "valid" && value.get<std::string>() != "libvmaf")) {
        return false;
      }
      out.border = value.get<std::string>() == "valid" ? Border::kValid : Border::kLibvmaf;
    } else if (key == "map_block") {
      if (!parse_map_block(value, out.map_block)) {
        return false;
      }
    } else {
      return false;  // rule 6: unknown keys are rejected
    }
  }
  return true;
}

const std::vector<uint8_t>& descriptor_bytes() {
  static const std::vector<uint8_t> bytes = [] {
    flatbuffers::FlatBufferBuilder fbb(1024);
    const auto id = fbb.CreateString("ssim");
    const auto name = fbb.CreateString("SSIM");
    const auto outputs = fbb.CreateVectorOfStrings(std::vector<std::string>{"y", "u", "v", "map"});
    const auto license = fbb.CreateString("LGPL-2.1-or-later");
    const auto version = fbb.CreateString("ssim 1.0 border=libvmaf");  // the default policy (option "border")
    ovt::fb::MetricDescriptorBuilder d(fbb);
    d.add_id(id);
    d.add_display_name(name);
    d.add_kind(ovt::fb::MetricKind::FullReference);
    d.add_domain(ovt::fb::WorkingDomain::CodeValue);
    d.add_per_plane(true);
    d.add_higher_is_better(true);
    d.add_range_min(-1.0);
    d.add_range_max(1.0);
    d.add_temporal_unit_frames(1);
    d.add_outputs(outputs);
    d.add_license(license);
    d.add_version(version);
    fbb.Finish(d.Finish());
    return std::vector<uint8_t>(
        fbb.GetBufferPointer(),
        fbb.GetBufferPointer() + fbb.GetSize());  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }();
  return bytes;
}

// The instance behind the opaque ovt_Metric* of the ABI. psnr_metric.cpp defines the global struct ovt_Metric for its
// own state, so this one has another name (two definitions of one name would be an ODR violation) and is converted at
// the ABI boundary by state().
struct SsimState {
  Options options;
  uint32_t bit_depth = 0;
  uint8_t planes = 0;
  uint64_t frames = 0;
  // Scratch, reused from frame to frame.
  std::vector<float> ref_f, dist_f, ref_d, dist_d;       // plane as float, and its libvmaf down-scaled copy
  std::array<std::vector<float>, 3> prod;                // x^2, y^2, x*y of one input row
  std::array<std::vector<float>, 5> band;                // horizontally filtered moments of one band
  std::vector<float> row;                                // one row of window values
  std::unique_ptr<flatbuffers::FlatBufferBuilder> last;  // the FrameScore of the last score() (rule 4)
};

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): ovt_Metric is opaque to the ABI's callers.
ovt_Metric* handle(SsimState* s) noexcept {
  return reinterpret_cast<ovt_Metric*>(s);
}
SsimState* state(ovt_Metric* m) noexcept {
  return reinterpret_cast<SsimState*>(m);
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace

namespace ovt::metrics {

namespace {

// libvmaf's 1-D window (iqa/ssim_tools.h g_gaussian_window_h), six decimals.
constexpr std::array<float, kSsimTaps> kLibvmafTaps = {0.001028F, 0.007599F, 0.036001F, 0.109361F, 0.213006F, 0.266012F,
                                                       0.213006F, 0.109361F, 0.036001F, 0.007599F, 0.001028F};

// Wang's window: g(k) = exp(-k^2 / (2 * 1.5^2)) / sum_j exp(-j^2 / (2 * 1.5^2)), k in [-5, 5].
const std::array<float, kSsimTaps>& exact_taps() {
  static const std::array<float, kSsimTaps> taps = [] {
    std::array<double, kSsimTaps> g{};
    double sum = 0.0;
    for (size_t i = 0; i < kSsimTaps; ++i) {
      const double k = static_cast<double>(i) - 5.0;
      g[i] = std::exp(-(k * k) / (2.0 * 1.5 * 1.5));
      sum += g[i];
    }
    std::array<float, kSsimTaps> t{};
    for (size_t i = 0; i < kSsimTaps; ++i) {
      t[i] = static_cast<float>(g[i] / sum);
    }
    return t;
  }();
  return taps;
}

bool plane_ok(const ovt_MetricPlanes& p) {
  return p.struct_size >= sizeof(ovt_MetricPlanes) && (p.planes == 1 || p.planes == 3) &&
         p.domain == OVT_METRIC_DOMAIN_CODE_VALUE && p.sample_format == OVT_METRIC_SAMPLE_UINT &&
         (p.sample_bytes == 1 || p.sample_bytes == 2) && p.bit_depth >= 8 && p.bit_depth <= 16 &&
         (p.sample_bytes == 2 || p.bit_depth == 8);
}

bool pair_ok(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d) {
  if (!plane_ok(r) || !plane_ok(d) || r.planes != d.planes || r.bit_depth != d.bit_depth ||
      r.sample_bytes != d.sample_bytes) {
    return false;
  }
  for (uint8_t p = 0; p < r.planes; ++p) {
    if (r.data[p] == nullptr || d.data[p] == nullptr || r.width[p] != d.width[p] || r.height[p] != d.height[p] ||
        r.width[p] == 0 || r.height[p] == 0) {
      return false;
    }
  }
  return true;
}

// libvmaf's _round (iqa/math_utils.c): halves round away from zero.
int libvmaf_round(float a) {
  const int sign = a > 0.0F ? 1 : -1;
  return (a - static_cast<float>(static_cast<int>(a))) >= 0.5F ? static_cast<int>(a) + sign : static_cast<int>(a);
}

// libvmaf's mirror border (KBND_SYMMETRIC): -1 - x below zero, 2 * w - x - 1 above.
int mirror(int x, int w) {
  if (x < 0) {
    return -1 - x;
  }
  if (x >= w) {
    return (w - (x - w)) - 1;
  }
  return x;
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): the functions below address rows of float images and
// of the strided pictures that pair_ok() validated (non-null planes, equal non-zero geometry).

// libvmaf's _iqa_decimate with the f x f box kernel of ssim.c: sample (x * f, y * f) of the mirrored image, the box
// spans [-f/2, f/2 - 1 + (f & 1)] around it, products in float and the sum in double. Returns the new size.
void decimate(const std::vector<float>& src, int w, int h, int f, std::vector<float>& dst, int& dw, int& dh) {
  dw = (w / f) + (w & 1);
  dh = (h / f) + (h & 1);
  dst.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh));
  const float weight = 1.0F / static_cast<float>(f * f);
  const int lo = -(f / 2);
  const int hi = (f / 2) - ((f & 1) == 0 ? 1 : 0);
  const auto taps = static_cast<size_t>(f);                  // hi - lo + 1 == f
  std::vector<size_t> cols(static_cast<size_t>(dw) * taps);  // mirrored source column of every (x, u)
  for (int x = 0; x < dw; ++x) {
    for (int u = lo; u <= hi; ++u) {
      cols[(static_cast<size_t>(x) * taps) + static_cast<size_t>(u - lo)] = static_cast<size_t>(mirror((x * f) + u, w));
    }
  }
  std::vector<const float*> rows(taps);
  for (int y = 0; y < dh; ++y) {
    for (int v = lo; v <= hi; ++v) {
      rows[static_cast<size_t>(v - lo)] =
          src.data() + (static_cast<size_t>(mirror((y * f) + v, h)) * static_cast<size_t>(w));
    }
    for (int x = 0; x < dw; ++x) {
      const size_t* const col = cols.data() + (static_cast<size_t>(x) * taps);
      double sum = 0.0;
      for (size_t v = 0; v < taps; ++v) {
        for (size_t u = 0; u < taps; ++u) {
          sum += static_cast<double>(rows[v][col[u]] * weight);
        }
      }
      dst[(static_cast<size_t>(y) * static_cast<size_t>(dw)) + static_cast<size_t>(x)] = static_cast<float>(sum);
    }
  }
}

// "ssim/<component>/<decode_index:06>/<B>" (§9.4.7).
std::string blob_id(size_t component, int32_t decode_index, uint32_t block) {
  std::string num = std::to_string(decode_index);
  if (num.size() < 6) {
    num.insert(0, 6 - num.size(), '0');
  }
  return "ssim/" + std::string(kComponentNames[component]) + "/" + num + "/" + std::to_string(block);
}

// Block means of the window values whose centre falls in each block (§9.4.7). Centres are mapped back to plane
// coordinates (window position + 5, times the libvmaf down-scale factor f) and the block is B / subsampling wide.
struct MapAccumulator {
  uint32_t blocks_w = 0;
  uint32_t blocks_h = 0;
  std::vector<double> sum;
  std::vector<uint32_t> count;
  std::vector<uint32_t> col;  // block column of window position x

  MapAccumulator(const ovt_MetricPlanes& r, uint8_t p, uint32_t block, uint32_t ow, uint32_t f)
      : blocks_w((r.width[0] + block - 1) / block), blocks_h((r.height[0] + block - 1) / block) {
    sum.assign(static_cast<size_t>(blocks_w) * blocks_h, 0.0);
    count.assign(sum.size(), 0);
    const uint32_t sx = r.width[0] > r.width[p] ? 2U : 1U;
    bw_ = std::max(1U, block / sx);
    bh_ = std::max(1U, block / (r.height[0] > r.height[p] ? 2U : 1U));
    f_ = f;
    col.resize(ow);
    for (uint32_t x = 0; x < ow; ++x) {
      col[x] = std::min(blocks_w - 1, ((x + (kSsimTaps / 2)) * f_) / bw_);
    }
  }
  void add_row(uint32_t y, const float* values, uint32_t ow) {
    const uint32_t by = std::min(blocks_h - 1, ((y + (kSsimTaps / 2)) * f_) / bh_);
    for (uint32_t x = 0; x < ow; ++x) {
      const size_t i = (static_cast<size_t>(by) * blocks_w) + col[x];
      sum[i] += static_cast<double>(values[x]);
      ++count[i];
    }
  }
  [[nodiscard]] std::vector<float> means() const {
    std::vector<float> out(sum.size(), std::numeric_limits<float>::quiet_NaN());
    for (size_t i = 0; i < sum.size(); ++i) {
      if (count[i] != 0) {
        out[i] = static_cast<float>(sum[i] / static_cast<double>(count[i]));
      }
    }
    return out;
  }

 private:
  uint32_t bw_ = 1;
  uint32_t bh_ = 1;
  uint32_t f_ = 1;
};

// SSIM of the float images `a` and `b` (w x h, pitch w): the mean of the window values over the valid region
// (output (w - 10) x (h - 10)), computed in bands of kSsimBandRows output rows. `map`, when non-null, receives the
// window values. The caller guarantees w, h >= 11. `f` is the libvmaf down-scale factor (for the map only).
double ssim_images(SsimState& m, const float* a, const float* b, uint32_t w, uint32_t h,
                   const std::array<float, kSsimTaps>& taps, MapAccumulator* map) {
  const uint32_t ow = w - kSsimMargin;
  const uint32_t oh = h - kSsimMargin;
  const float k1l = 0.01F * 255.0F;
  const float k2l = 0.03F * 255.0F;
  const SsimConstants k{k1l * k1l, k2l * k2l};
  for (auto& p : m.prod) {
    p.resize(w);
  }
  for (auto& bnd : m.band) {
    bnd.resize(static_cast<size_t>(kSsimBandRows + kSsimMargin) * ow);
  }
  m.row.resize(ow);
  double total = 0.0;
  for (uint32_t y0 = 0; y0 < oh; y0 += kSsimBandRows) {
    const uint32_t rows = std::min(kSsimBandRows, oh - y0);
    for (uint32_t r = 0; r < rows + kSsimMargin; ++r) {
      const float* ra = a + (static_cast<size_t>(y0 + r) * w);
      const float* rb = b + (static_cast<size_t>(y0 + r) * w);
      ssim_products_hwy(ra, rb, m.prod[0].data(), m.prod[1].data(), m.prod[2].data(), w);
      const size_t at = static_cast<size_t>(r) * ow;
      ssim_hfilter_hwy(ra, m.band[0].data() + at, ow, taps.data());
      ssim_hfilter_hwy(rb, m.band[1].data() + at, ow, taps.data());
      ssim_hfilter_hwy(m.prod[0].data(), m.band[2].data() + at, ow, taps.data());
      ssim_hfilter_hwy(m.prod[1].data(), m.band[3].data() + at, ow, taps.data());
      ssim_hfilter_hwy(m.prod[2].data(), m.band[4].data() + at, ow, taps.data());
    }
    for (uint32_t r = 0; r < rows; ++r) {
      const size_t at = static_cast<size_t>(r) * ow;
      const std::array<const float*, 5> mom = {m.band[0].data() + at, m.band[1].data() + at, m.band[2].data() + at,
                                               m.band[3].data() + at, m.band[4].data() + at};
      ssim_vwindow_hwy(mom.data(), ow, ow, taps.data(), k, m.row.data());
      for (uint32_t x = 0; x < ow; ++x) {  // the double reduction, in raster order (deterministic)
        total += static_cast<double>(m.row[x]);
      }
      if (map != nullptr) {
        map->add_row(y0 + r, m.row.data(), ow);
      }
    }
  }
  return total / (static_cast<double>(ow) * static_cast<double>(oh));
}

// SSIM of plane p of the pair. Writes the map into `sink` when requested. Returns false for a plane smaller than the
// window (after the libvmaf down-scale).
bool ssim_plane(SsimState& m, const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, uint8_t p, int32_t decode_index,
                MapSink* sink, double& score) {
  const bool libvmaf = m.options.border == Border::kLibvmaf;
  const uint32_t w0 = r.width[p];
  const uint32_t h0 = r.height[p];
  const auto peak = static_cast<float>((uint32_t{1} << r.bit_depth) - 1U);
  // Samples are normalised to the 8-bit range so that L = 255 (see the file comment).
  const auto scale = libvmaf ? 1.0F / static_cast<float>(uint32_t{1} << (r.bit_depth - 8U)) : 255.0F / peak;
  m.ref_f.resize(static_cast<size_t>(w0) * h0);
  m.dist_f.resize(m.ref_f.size());
  ssim_to_float_hwy(r.data[p], r.stride_bytes[p], w0, h0, r.sample_bytes, scale, m.ref_f.data(), w0);
  ssim_to_float_hwy(d.data[p], d.stride_bytes[p], w0, h0, d.sample_bytes, scale, m.dist_f.data(), w0);

  uint32_t w = w0;
  uint32_t h = h0;
  uint32_t f = 1;
  const float* a = m.ref_f.data();
  const float* b = m.dist_f.data();
  if (libvmaf) {
    f = static_cast<uint32_t>(
        std::max(1, libvmaf_round(static_cast<float>(std::min(w0, h0)) / 256.0F)));  // as ssim.c compute_ssim()
    if (f > 1) {
      int dw = 0;
      int dh = 0;
      decimate(m.ref_f, static_cast<int>(w0), static_cast<int>(h0), static_cast<int>(f), m.ref_d, dw, dh);
      decimate(m.dist_f, static_cast<int>(w0), static_cast<int>(h0), static_cast<int>(f), m.dist_d, dw, dh);
      w = static_cast<uint32_t>(dw);
      h = static_cast<uint32_t>(dh);
      a = m.ref_d.data();
      b = m.dist_d.data();
    }
  }
  if (w < kSsimTaps || h < kSsimTaps) {
    return false;
  }
  std::optional<MapAccumulator> map;
  if (sink != nullptr && m.options.map_block != 0) {
    map.emplace(r, p, m.options.map_block, w - kSsimMargin, f);
  }
  score = ssim_images(m, a, b, w, h, libvmaf ? kLibvmafTaps : exact_taps(), map ? &*map : nullptr);
  if (map) {
    sink->put(blob_id(p, decode_index, m.options.map_block),
              encode_ovtmap(map->blocks_w, map->blocks_h, static_cast<uint16_t>(m.options.map_block), p, 0, -1.0F, 1.0F,
                            map->means()));
  }
  return true;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

ovt_Buffer ssim_descriptor() noexcept {
  try {
    const auto& b = descriptor_bytes();
    return ovt_Buffer{b.data(), b.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}

ovt_status ssim_create(const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    auto m = std::make_unique<SsimState>();
    if (!parse_options(options_json, m->options)) {
      return OVT_E_INVALID_ARG;
    }
    *out = handle(m.release());
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

void ssim_destroy(ovt_Metric* m) noexcept {
  delete state(m);  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status ssim_score(ovt_Metric* handle_in, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist, ovt_FrameId id,
                      ovt_Buffer* out) noexcept {
  SsimState* const m = state(handle_in);
  if (m == nullptr || ref == nullptr || dist == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  if (!pair_ok(*ref, *dist)) {
    return OVT_E_INVALID_ARG;
  }
  if (m->frames != 0 && (m->bit_depth != ref->bit_depth || m->planes != ref->planes)) {
    return OVT_E_INVALID_ARG;  // the stream's format must not change mid-sequence
  }
  try {
    MapSink* const sink = map_sink();
    std::array<double, 3> values = {kNaN, kNaN, kNaN};
    size_t first_map = 3;
    bool any = false;
    for (uint8_t p = 0; p < ref->planes; ++p) {
      if (!m->options.planes[p]) {
        continue;
      }
      if (!ssim_plane(*m, *ref, *dist, p, id.decode_index, sink, values[p])) {
        return OVT_E_INVALID_ARG;
      }
      any = true;
      first_map = std::min<size_t>(first_map, p);
    }
    if (!any) {
      return OVT_E_INVALID_ARG;  // planes selects only planes the input does not have (4:0:0 with ["u"])
    }
    m->bit_depth = ref->bit_depth;
    m->planes = ref->planes;
    ++m->frames;

    const bool mapped = sink != nullptr && m->options.map_block != 0;
    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(512);
    const ovt::fb::FrameId fid(id.decode_index, id.display_index);
    const auto vec = fbb->CreateVector(std::vector<double>{values[0], values[1], values[2]});
    // FrameScore carries one map_blob_id: the first computed component's; the others differ in the component only.
    const auto blob = mapped ? fbb->CreateString(blob_id(first_map, id.decode_index, m->options.map_block))
                             : flatbuffers::Offset<flatbuffers::String>();
    fbb->Finish(
        ovt::fb::CreateFrameScore(*fbb, &fid, vec, blob, static_cast<uint16_t>(mapped ? m->options.map_block : 0U)));
    m->last = std::move(fbb);
    *out = ovt_Buffer{m->last->GetBufferPointer(), m->last->GetSize()};
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

ovt_status ssim_finish(ovt_Metric* m, ovt_Buffer* out) noexcept {
  if (m == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};  // no sequence score: pooling of the per-frame series is WI-3.311
  return OVT_OK;
}

const ovt_MetricApi kSsimApi = {
    sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, ssim_descriptor, ssim_create, ssim_destroy, ssim_score, ssim_finish};

}  // namespace

const ovt_MetricApi* ssim_metric_api() noexcept {
  return &kSsimApi;
}

}  // namespace ovt::metrics
