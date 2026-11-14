// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `ssim_x264` metric (chapter 9 §9.4.4, §9.4.7; WI-3.308) behind ovt_metric_v1: the fast SSIM that x264 and x265
// print with --ssim, written from the normative algorithm of §9.4.4 (clean room: x264 is GPL and only the test oracle
// of tests/oracles/x264_ssim contains its code).
//
//   plane (W x H) -> origin (2, 2), size W' x H' = (W - 2) x (H - 2)
//   4 x 4 blocks (W'/4 x H'/4), sums s1 = sum x, s2 = sum y, ss = sum x^2 + y^2, s12 = sum x*y
//   8 x 8 windows stepping by 4 (four blocks), (W'/4 - 1) x (H'/4 - 1) of them
//   values (per frame) = [y, u, v, ssim_db_y]   (NaN for planes not computed; finish() returns no score)
//
// Window formula: bit depth <= 9 follows x264's integer branch (constants rounded to int, int products), deeper
// samples its float branch (float constants without rounding, float products); the final division is a double
// division in both, and the plane score is the double mean of the window values in raster order.
//
// SPEC-ASSUMPTION(#419): a plane with fewer than two block columns or rows after the offset (no window) makes
// score() fail with OVT_E_INVALID_ARG; the default for "planes" is ["y"] (x264 prints luma only); map windows are
// attributed to the block that holds the window centre.

#include "metrics/kernels/ssim_x264.h"

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
#include "metrics/kernels/plane_check.h"
#include "ovt_metrics_generated.h"

namespace {

constexpr std::array<std::string_view, 3> kComponentNames = {"y", "u", "v"};
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr uint32_t kOffset = 2;  // x264_pixel_ssim_wxh is called on pix + 2 + 2 * stride, (W - 2) x (H - 2)
constexpr uint32_t kBlock = 4;

struct Options {
  std::array<bool, 3> planes = {true, false, false};
  uint32_t map_block = 0;  // 0 = no maps
};

bool parse_planes(const nlohmann::json& value, std::array<bool, 3>& planes) {
  if (!value.is_array() || value.empty()) {
    return false;
  }
  planes = {false, false, false};
  for (const auto& p : value) {
    const auto* const it =
        p.is_string() ? std::ranges::find(kComponentNames, p.get<std::string>()) : kComponentNames.end();
    if (it == kComponentNames.end()) {
      return false;
    }
    planes[static_cast<size_t>(it - kComponentNames.begin())] = true;
  }
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
    } else if (key == "map_block") {
      if (value.is_null()) {
        out.map_block = 0;
      } else if (value.is_number_integer() && (value == 8 || value == 16 || value == 32 || value == 64)) {
        out.map_block = value.get<uint32_t>();
      } else {
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
    const auto id = fbb.CreateString("ssim_x264");
    const auto name = fbb.CreateString("SSIM (x264)");
    const auto outputs = fbb.CreateVectorOfStrings(std::vector<std::string>{"y", "u", "v", "ssim_db_y", "map"});
    const auto license = fbb.CreateString("LGPL-2.1-or-later");
    const auto version = fbb.CreateString("ssim_x264 1.0");
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

// Sums of one 4 x 4 block (§9.4.4 step 1). int64 holds every depth up to 16 bits exactly.
struct BlockSums {
  int64_t s1 = 0;
  int64_t s2 = 0;
  int64_t ss = 0;
  int64_t s12 = 0;
};

// One window value (§9.4.4 step 3) from the summed sums of its four blocks. `bits` selects the integer branch
// (<= 9) or the float branch (> 9) of x264's ssim_end1.
double window_ssim(const BlockSums& s, uint32_t bits) {
  const auto peak = static_cast<double>((uint64_t{1} << bits) - 1U);
  if (bits <= 9) {
    // x264: int constants, int products (they do not overflow up to 9 bits), then float factors; here double.
    const int64_t c1 = std::lround(0.01 * 0.01 * peak * peak * 64.0);
    const int64_t c2 = std::lround(0.03 * 0.03 * peak * peak * 64.0 * 63.0);
    const int64_t vars = (s.ss * 64) - (s.s1 * s.s1) - (s.s2 * s.s2);
    const int64_t covar = (s.s12 * 64) - (s.s1 * s.s2);
    const double num = static_cast<double>((2 * s.s1 * s.s2) + c1) * static_cast<double>((2 * covar) + c2);
    const double den = static_cast<double>((s.s1 * s.s1) + (s.s2 * s.s2) + c1) * static_cast<double>(vars + c2);
    return num / den;
  }
  // x264's `#if BIT_DEPTH > 9` branch: float constants (no rounding) and float products, in x264's order of operations.
  const auto c1 = static_cast<float>(0.01 * 0.01 * peak * peak * 64.0);
  const auto c2 = static_cast<float>(0.03 * 0.03 * peak * peak * 64.0 * 63.0);
  const auto fs1 = static_cast<float>(s.s1);
  const auto fs2 = static_cast<float>(s.s2);
  const auto fss = static_cast<float>(s.ss);
  const auto fs12 = static_cast<float>(s.s12);
  const float vars = (fss * 64.0F) - (fs1 * fs1) - (fs2 * fs2);
  const float covar = (fs12 * 64.0F) - (fs1 * fs2);
  const float n1 = (2.0F * fs1 * fs2) + c1;
  const float n2 = (2.0F * covar) + c2;
  const float d1 = (fs1 * fs1) + (fs2 * fs2) + c1;
  const float d2 = vars + c2;
  return (static_cast<double>(n1) * static_cast<double>(n2)) / (static_cast<double>(d1) * static_cast<double>(d2));
}

// ssim_db (§9.4.4 step 5): -10 log10(1 - ssim), exactly 100 when 1 - ssim <= 1e-10.
double ssim_db(double ssim) {
  const double inv = 1.0 - ssim;
  return inv <= 1e-10 ? 100.0 : -10.0 * std::log10(inv);
}

// "ssim_x264/<component>/<decode_index:06>/<B>" (§9.4.7).
std::string blob_id(size_t component, int32_t decode_index, uint32_t block) {
  std::string num = std::to_string(decode_index);
  if (num.size() < 6) {
    num.insert(0, 6 - num.size(), '0');
  }
  return "ssim_x264/" + std::string(kComponentNames[component]) + "/" + num + "/" + std::to_string(block);
}

// Block means of the window values whose centre falls in each block (§9.4.7). The window (wx, wy) covers plane
// samples [2 + 4 wx, 2 + 4 wx + 8) and its centre is at 2 + 4 wx + 4. A block is B / subsampling wide.
struct MapAccumulator {
  uint32_t blocks_w;
  uint32_t blocks_h;
  uint32_t bw;
  uint32_t bh;
  std::vector<double> sum;
  std::vector<uint32_t> count;

  MapAccumulator(const ovt_MetricPlanes& r, uint8_t p, uint32_t block)
      : blocks_w((r.width[0] + block - 1) / block),
        blocks_h((r.height[0] + block - 1) / block),
        bw(std::max(1U, block / (r.width[0] > r.width[p] ? 2U : 1U))),
        bh(std::max(1U, block / (r.height[0] > r.height[p] ? 2U : 1U))),
        sum(static_cast<size_t>(blocks_w) * blocks_h, 0.0),
        count(sum.size(), 0) {}
  void add(uint32_t wx, uint32_t wy, double v) {
    const uint32_t bx = std::min(blocks_w - 1, (kOffset + (kBlock * wx) + kBlock) / bw);
    const uint32_t by = std::min(blocks_h - 1, (kOffset + (kBlock * wy) + kBlock) / bh);
    const size_t i = (static_cast<size_t>(by) * blocks_w) + bx;
    sum[i] += v;
    ++count[i];
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
};

// The instance behind the opaque ovt_Metric* of the ABI. psnr_metric.cpp defines the global struct ovt_Metric for its
// own state, so this one has another name (two definitions of one name are an ODR violation) and is converted at the
// ABI boundary by state().
struct X264State {
  Options options;
  uint32_t bit_depth = 0;
  uint8_t planes = 0;
  uint64_t frames = 0;
  std::array<std::vector<BlockSums>, 2> rows;            // block sums of two consecutive block rows
  std::unique_ptr<flatbuffers::FlatBufferBuilder> last;  // the FrameScore of the last score() (rule 4)
};

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): ovt_Metric is opaque to the ABI's callers.
ovt_Metric* handle(X264State* s) noexcept {
  return reinterpret_cast<ovt_Metric*>(s);
}
X264State* state(ovt_Metric* m) noexcept {
  return reinterpret_cast<X264State*>(m);
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace

namespace ovt::metrics {

namespace {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): rows of the strided pictures that
// code_value_pair_ok() validated (non-null planes, equal non-zero geometry).

// The 4 x 4 sums of block row `by` (plane coordinates offset by 2) into `out` (one entry per block column).
void block_row_sums(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, uint8_t p, uint32_t by, uint32_t blocks,
                    std::vector<BlockSums>& out) {
  out.assign(blocks, BlockSums{});
  const uint32_t sb = r.sample_bytes;
  for (uint32_t y = 0; y < kBlock; ++y) {
    const size_t row = kOffset + (static_cast<size_t>(by) * kBlock) + y;
    const uint8_t* const ra = r.data[p] + (row * static_cast<size_t>(r.stride_bytes[p]));
    const uint8_t* const rb = d.data[p] + (row * static_cast<size_t>(d.stride_bytes[p]));
    for (uint32_t bx = 0; bx < blocks; ++bx) {
      BlockSums& s = out[bx];
      for (uint32_t x = 0; x < kBlock; ++x) {
        const size_t col = kOffset + (static_cast<size_t>(bx) * kBlock) + x;
        const auto a = static_cast<int64_t>(load_sample(ra, col, sb));
        const auto b = static_cast<int64_t>(load_sample(rb, col, sb));
        s.s1 += a;
        s.s2 += b;
        s.ss += (a * a) + (b * b);
        s.s12 += a * b;
      }
    }
  }
}

// SSIM of plane p (§9.4.4 step 4). Returns false when the plane holds no window.
bool x264_plane(X264State& m, const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, uint8_t p, int32_t decode_index,
                MapSink* sink, double& score) {
  const uint32_t w = r.width[p];
  const uint32_t h = r.height[p];
  if (w < kOffset || h < kOffset) {
    return false;
  }
  const uint32_t blocks_x = (w - kOffset) / kBlock;
  const uint32_t blocks_y = (h - kOffset) / kBlock;
  if (blocks_x < 2 || blocks_y < 2) {
    return false;
  }
  std::optional<MapAccumulator> map;
  if (sink != nullptr && m.options.map_block != 0) {
    map.emplace(r, p, m.options.map_block);
  }
  double total = 0.0;
  block_row_sums(r, d, p, 0, blocks_x, m.rows[0]);
  for (uint32_t wy = 0; wy + 1 < blocks_y; ++wy) {
    auto& upper = m.rows[wy & 1U];
    auto& lower = m.rows[(wy & 1U) ^ 1U];
    block_row_sums(r, d, p, wy + 1, blocks_x, lower);
    for (uint32_t wx = 0; wx + 1 < blocks_x; ++wx) {
      BlockSums s;
      for (const BlockSums* b : {&upper[wx], &upper[wx + 1], &lower[wx], &lower[wx + 1]}) {
        s.s1 += b->s1;
        s.s2 += b->s2;
        s.ss += b->ss;
        s.s12 += b->s12;
      }
      const double v = window_ssim(s, r.bit_depth);
      total += v;
      if (map) {
        map->add(wx, wy, v);
      }
    }
  }
  score = total / (static_cast<double>(blocks_x - 1) * static_cast<double>(blocks_y - 1));
  if (map) {
    sink->put(blob_id(p, decode_index, m.options.map_block),
              encode_ovtmap(map->blocks_w, map->blocks_h, static_cast<uint16_t>(m.options.map_block), p, 0, -1.0F, 1.0F,
                            map->means()));
  }
  return true;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

ovt_Buffer x264_descriptor() noexcept {
  try {
    const auto& b = descriptor_bytes();
    return ovt_Buffer{b.data(), b.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}

ovt_status x264_create(const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    auto m = std::make_unique<X264State>();
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

void x264_destroy(ovt_Metric* m) noexcept {
  delete state(m);  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

ovt_status x264_score(ovt_Metric* handle_in, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist, ovt_FrameId id,
                      ovt_Buffer* out) noexcept {
  X264State* const m = state(handle_in);
  if (m == nullptr || ref == nullptr || dist == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  if (!code_value_pair_ok(*ref, *dist)) {
    return OVT_E_INVALID_ARG;
  }
  if (m->frames != 0 && (m->bit_depth != ref->bit_depth || m->planes != ref->planes)) {
    return OVT_E_INVALID_ARG;  // the stream's format must not change mid-sequence
  }
  try {
    MapSink* const sink = map_sink();
    std::array<double, 4> values = {kNaN, kNaN, kNaN, kNaN};
    size_t first_map = 3;
    for (uint8_t p = 0; p < ref->planes; ++p) {
      if (!m->options.planes[p]) {
        continue;
      }
      if (!x264_plane(*m, *ref, *dist, p, id.decode_index, sink, values[p])) {
        return OVT_E_INVALID_ARG;
      }
      first_map = std::min<size_t>(first_map, p);
    }
    if (first_map == 3) {
      return OVT_E_INVALID_ARG;  // planes selects only planes the input does not have (4:0:0 with ["u"])
    }
    if (!std::isnan(values[0])) {
      values[3] = ssim_db(values[0]);
    }
    m->bit_depth = ref->bit_depth;
    m->planes = ref->planes;
    ++m->frames;

    const bool mapped = sink != nullptr && m->options.map_block != 0;
    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(512);
    const ovt::fb::FrameId fid(id.decode_index, id.display_index);
    const auto vec = fbb->CreateVector(std::vector<double>(values.begin(), values.end()));
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

ovt_status x264_finish(ovt_Metric* m, ovt_Buffer* out) noexcept {
  if (m == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};  // no sequence score: pooling of the per-frame series is WI-3.311
  return OVT_OK;
}

const ovt_MetricApi kX264Api = {
    sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, x264_descriptor, x264_create, x264_destroy, x264_score, x264_finish};

}  // namespace

const ovt_MetricApi* ssim_x264_metric_api() noexcept {
  return &kX264Api;
}

}  // namespace ovt::metrics
