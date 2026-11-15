// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `psnr` metric (chapter 9 §9.4.2, §9.4.7; WI-3.306) behind ovt_metric_v1: per-plane PSNR, the JVET and FFmpeg
// combinations, raw MSE, sequence APSNR (finish) and per-block maps.
//
//   values (per frame) = [y, u, v, jvet, ffmpeg_avg, mse_y, mse_u, mse_v]      (descriptor.outputs, in this order)
//   values (finish)    = [apsnr_y, apsnr_u, apsnr_v, apsnr_jvet]
//
// SPEC-ASSUMPTION(#385): for 4:0:0 input, or planes not selected by the `planes` option, the u/v/mse_u/mse_v entries
// are NaN so every output keeps its position; jvet and ffmpeg_avg equal Y for 4:0:0 (§9.4.1), and are NaN for a
// selection that is not all of the available planes.
// SPEC-ASSUMPTION(#385): a PSNR above psnr_max (a tiny non-zero MSE) is clipped to psnr_max like libvmaf's psnr
// feature, which §9.4.2 names as oracle "same clipping"; the literal formula only applies psnr_max at MSE == 0.

#include "metrics/kernels/psnr_metric.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "metrics/host/plugin_registry.h"
#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/psnr_kernels.h"
#include "metrics/kernels/ssim_metric.h"
#include "metrics/kernels/ssim_x264.h"
#include "ovt_metrics_generated.h"

namespace ovt::metrics {

namespace {

std::atomic<MapSink*> g_map_sink{nullptr};

constexpr std::array<std::string_view, 3> kComponentNames = {"y", "u", "v"};
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Options {
  std::optional<double> psnr_max_db;  // nullopt = 60 + 6(b − 8)
  std::array<bool, 3> planes = {true, true, true};
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
    if (key == "psnr_max_db") {
      if (value.is_null()) {
        out.psnr_max_db.reset();
      } else if (value.is_number() && value.get<double>() > 0.0 && std::isfinite(value.get<double>())) {
        out.psnr_max_db = value.get<double>();
      } else {
        return false;
      }
    } else if (key == "planes") {
      if (!parse_planes(value, out.planes)) {
        return false;
      }
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
    const auto id = fbb.CreateString("psnr");
    const auto name = fbb.CreateString("PSNR");
    const auto outputs = fbb.CreateVectorOfStrings(std::vector<std::string>{"y", "u", "v", "jvet", "ffmpeg_avg",
                                                                            "mse_y", "mse_u", "mse_v", "map", "apsnr_y",
                                                                            "apsnr_u", "apsnr_v", "apsnr_jvet"});
    const auto license = fbb.CreateString("LGPL-2.1-or-later");
    const auto version = fbb.CreateString("1.0.0");
    ovt::fb::MetricDescriptorBuilder d(fbb);
    d.add_id(id);
    d.add_display_name(name);
    d.add_kind(ovt::fb::MetricKind::FullReference);
    d.add_domain(ovt::fb::WorkingDomain::CodeValue);
    d.add_per_plane(true);
    d.add_higher_is_better(true);
    d.add_range_min(0.0);
    d.add_range_max(108.0);  // psnr_max at 16 bits: 60 + 6 * (16 − 8)
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

double psnr_from_mse(double mse, double peak, double psnr_max) {
  if (!(mse > 0.0)) {
    return psnr_max;
  }
  return std::min(10.0 * std::log10(peak * peak / mse), psnr_max);
}

}  // namespace

void set_map_sink(MapSink* sink) noexcept {
  g_map_sink.store(sink, std::memory_order_release);
}

MapSink* map_sink() noexcept {
  return g_map_sink.load(std::memory_order_acquire);
}

}  // namespace ovt::metrics

struct ovt_Metric {
  ovt::metrics::Options options;
  std::array<double, 3> mse_sum = {0.0, 0.0, 0.0};  // Σ over frames of MSE per plane (APSNR)
  uint64_t frames = 0;
  uint32_t bit_depth = 0;
  uint8_t planes = 0;
  std::unique_ptr<flatbuffers::FlatBufferBuilder> last;  // the FrameScore of the last score()/finish() (rule 4)
};

namespace ovt::metrics {

namespace {

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

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): plane_sse and emit_map address rows of the strided
// pictures that pair_ok() validated (non-null planes, equal non-zero geometry).

// SSE of a plane in bands of kPsnrBandRows rows, summed in ascending order (§9.4.1).
uint64_t plane_sse(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, uint8_t p) {
  uint64_t sse = 0;
  for (uint32_t y0 = 0; y0 < r.height[p]; y0 += kPsnrBandRows) {
    const uint32_t rows = std::min(kPsnrBandRows, r.height[p] - y0);
    sse += psnr_sse_hwy(r.data[p] + (static_cast<ptrdiff_t>(y0) * r.stride_bytes[p]), r.stride_bytes[p],
                        d.data[p] + (static_cast<ptrdiff_t>(y0) * d.stride_bytes[p]), d.stride_bytes[p], r.width[p],
                        rows, r.sample_bytes, r.bit_depth);
  }
  return sse;
}

// "<metric_id>/<component>/<decode_index:06>/<B>" (§9.4.7).
std::string blob_id(size_t component, int32_t decode_index, uint32_t block) {
  std::string num = std::to_string(decode_index);
  if (num.size() < 6) {
    num.insert(0, 6 - num.size(), '0');
  }
  return "psnr/" + std::string(kComponentNames[component]) + "/" + num + "/" + std::to_string(block);
}

// Block map of plane p: PSNR of every B-luma-sample block (chroma blocks cover the same area), edge blocks use the
// samples that exist. B is in luma samples; the plane's block is B/sx × B/sy with sx, sy the subsampling factors.
void emit_map(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, uint8_t p, uint32_t block, int32_t decode_index,
              double peak, double psnr_max, MapSink& sink) {
  const uint32_t sx = r.width[0] > r.width[p] ? 2U : 1U;
  const uint32_t sy = r.height[0] > r.height[p] ? 2U : 1U;
  const uint32_t bw = std::max(1U, block / sx);
  const uint32_t bh = std::max(1U, block / sy);
  const uint32_t blocks_w = (r.width[0] + block - 1) / block;
  const uint32_t blocks_h = (r.height[0] + block - 1) / block;
  std::vector<float> values;
  values.reserve(static_cast<size_t>(blocks_w) * blocks_h);
  for (uint32_t by = 0; by < blocks_h; ++by) {
    const uint32_t y0 = by * bh;
    const uint32_t rows = std::min(bh, r.height[p] - std::min(y0, r.height[p]));
    for (uint32_t bx = 0; bx < blocks_w; ++bx) {
      const uint32_t x0 = bx * bw;
      const uint32_t cols = std::min(bw, r.width[p] - std::min(x0, r.width[p]));
      if (rows == 0 || cols == 0) {
        values.push_back(std::numeric_limits<float>::quiet_NaN());
        continue;
      }
      const ptrdiff_t ro =
          (static_cast<ptrdiff_t>(y0) * r.stride_bytes[p]) + (static_cast<ptrdiff_t>(x0) * r.sample_bytes);
      const ptrdiff_t dof =
          (static_cast<ptrdiff_t>(y0) * d.stride_bytes[p]) + (static_cast<ptrdiff_t>(x0) * d.sample_bytes);
      const uint64_t sse = psnr_sse_hwy(r.data[p] + ro, r.stride_bytes[p], d.data[p] + dof, d.stride_bytes[p], cols,
                                        rows, r.sample_bytes, r.bit_depth);
      const double mse = static_cast<double>(sse) / (static_cast<double>(cols) * rows);
      values.push_back(static_cast<float>(psnr_from_mse(mse, peak, psnr_max)));
    }
  }
  sink.put(blob_id(p, decode_index, block), encode_ovtmap(blocks_w, blocks_h, static_cast<uint16_t>(block), p, 0, 0.0F,
                                                          static_cast<float>(psnr_max), values));
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

ovt_Buffer psnr_descriptor() noexcept {
  try {
    const auto& b = descriptor_bytes();
    return ovt_Buffer{b.data(), b.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}

ovt_status psnr_create(const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    auto m = std::make_unique<ovt_Metric>();
    if (!parse_options(options_json, m->options)) {
      return OVT_E_INVALID_ARG;
    }
    *out = m.release();
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

void psnr_destroy(ovt_Metric* m) noexcept {
  delete m;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

// What one frame yields before it is packed into a FrameScore.
struct FrameStats {
  std::array<double, 3> mse = {kNaN, kNaN, kNaN};
  std::array<double, 3> psnr = {kNaN, kNaN, kNaN};
  double jvet = kNaN;
  double ffmpeg_avg = kNaN;
};

// Returns false when `planes` selects only planes the input does not have (4:0:0 with ["u"]).
bool compute_stats(const Options& opt, const ovt_MetricPlanes& ref, const ovt_MetricPlanes& dist, double peak,
                   double psnr_max, FrameStats& st) {
  uint64_t sse_all = 0;
  uint64_t samples_all = 0;
  bool all_selected = true;
  for (uint8_t p = 0; p < ref.planes; ++p) {
    if (!opt.planes[p]) {
      all_selected = false;
      continue;
    }
    const uint64_t sse = plane_sse(ref, dist, p);
    const uint64_t samples = static_cast<uint64_t>(ref.width[p]) * ref.height[p];
    st.mse[p] = static_cast<double>(sse) / static_cast<double>(samples);
    st.psnr[p] = psnr_from_mse(st.mse[p], peak, psnr_max);
    sse_all += sse;
    samples_all += samples;
  }
  if (samples_all == 0) {
    return false;
  }
  if (all_selected) {
    st.jvet = ref.planes == 1 ? st.psnr[0] : ((6.0 * st.psnr[0]) + st.psnr[1] + st.psnr[2]) / 8.0;
    st.ffmpeg_avg = psnr_from_mse(static_cast<double>(sse_all) / static_cast<double>(samples_all), peak, psnr_max);
  }
  return true;
}

// Writes the maps of the selected planes and returns the map_blob_id of the FrameScore (empty without a sink).
std::string emit_maps(const Options& opt, const ovt_MetricPlanes& ref, const ovt_MetricPlanes& dist,
                      int32_t decode_index, double peak, double psnr_max) {
  MapSink* sink = map_sink();
  if (opt.map_block == 0 || sink == nullptr) {
    return {};
  }
  size_t first = 3;
  for (uint8_t p = 0; p < ref.planes; ++p) {
    if (opt.planes[p]) {
      first = std::min<size_t>(first, p);
      emit_map(ref, dist, p, opt.map_block, decode_index, peak, psnr_max, *sink);
    }
  }
  // FrameScore carries one map_blob_id: the first selected component's; the others differ in the component only.
  return first < 3 ? blob_id(first, decode_index, opt.map_block) : std::string();
}

double psnr_max_for(const Options& opt, uint32_t bit_depth) {
  return opt.psnr_max_db.value_or(60.0 + (6.0 * (static_cast<double>(bit_depth) - 8.0)));
}

ovt_status psnr_score(ovt_Metric* m, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist, ovt_FrameId id,
                      ovt_Buffer* out) noexcept {
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
    const auto peak = static_cast<double>((uint64_t{1} << ref->bit_depth) - 1);
    const double psnr_max = psnr_max_for(m->options, ref->bit_depth);
    FrameStats st;
    if (!compute_stats(m->options, *ref, *dist, peak, psnr_max, st)) {
      return OVT_E_INVALID_ARG;
    }
    m->bit_depth = ref->bit_depth;
    m->planes = ref->planes;
    for (uint8_t p = 0; p < ref->planes; ++p) {
      if (m->options.planes[p]) {
        m->mse_sum[p] += st.mse[p];
      }
    }
    ++m->frames;

    const std::string map_id = emit_maps(m->options, *ref, *dist, id.decode_index, peak, psnr_max);
    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(512);
    const ovt::fb::FrameId fid(id.decode_index, id.display_index);
    const auto values = fbb->CreateVector(std::vector<double>{st.psnr[0], st.psnr[1], st.psnr[2], st.jvet,
                                                              st.ffmpeg_avg, st.mse[0], st.mse[1], st.mse[2]});
    const auto blob = map_id.empty() ? flatbuffers::Offset<flatbuffers::String>() : fbb->CreateString(map_id);
    fbb->Finish(ovt::fb::CreateFrameScore(*fbb, &fid, values, blob,
                                          static_cast<uint16_t>(map_id.empty() ? 0U : m->options.map_block)));
    m->last = std::move(fbb);
    *out = ovt_Buffer{m->last->GetBufferPointer(), m->last->GetSize()};
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

ovt_status psnr_finish(ovt_Metric* m, ovt_Buffer* out) noexcept {
  if (m == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  if (m->frames == 0) {
    return OVT_OK;  // nothing scored, no sequence score
  }
  try {
    const auto peak = static_cast<double>((uint64_t{1} << m->bit_depth) - 1);
    const double psnr_max = psnr_max_for(m->options, m->bit_depth);
    std::array<double, 3> apsnr = {kNaN, kNaN, kNaN};
    for (uint8_t p = 0; p < m->planes; ++p) {
      if (m->options.planes[p]) {
        apsnr[p] = psnr_from_mse(m->mse_sum[p] / static_cast<double>(m->frames), peak, psnr_max);
      }
    }
    double jvet = kNaN;
    bool all = m->options.planes[0];
    if (m->planes == 3) {
      all = all && m->options.planes[1] && m->options.planes[2];
      jvet = all ? ((6.0 * apsnr[0]) + apsnr[1] + apsnr[2]) / 8.0 : kNaN;
    } else {
      jvet = apsnr[0];
    }
    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(256);
    const ovt::fb::FrameId fid(-1, -1);  // a sequence-level score belongs to no frame
    const auto values = fbb->CreateVector(std::vector<double>{apsnr[0], apsnr[1], apsnr[2], jvet});
    fbb->Finish(ovt::fb::CreateFrameScore(*fbb, &fid, values));
    m->last = std::move(fbb);
    *out = ovt_Buffer{m->last->GetBufferPointer(), m->last->GetSize()};
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

const ovt_MetricApi kPsnrApi = {
    sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, psnr_descriptor, psnr_create, psnr_destroy, psnr_score, psnr_finish};

}  // namespace

const ovt_MetricApi* psnr_metric_api() noexcept {
  return &kPsnrApi;
}

void register_builtin_kernels(PluginRegistry& registry) {
  (void)registry.register_builtin(psnr_metric_api(), "(built-in)");
  (void)registry.register_builtin(ssim_metric_api(), "(built-in)");
  (void)registry.register_builtin(ssim_x264_metric_api(), "(built-in)");
}

}  // namespace ovt::metrics
