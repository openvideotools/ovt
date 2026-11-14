// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The `identity` metric (chapter 9 §9.4.6, §9.4.7; WI-3.308b) behind ovt_metric_v1: the lossless check.
//
//   values (per frame) = [y, u, v, all]   fraction of samples with ref == dist per plane, and over all planes
//                                         (sample-weighted); NaN for a plane the input does not have
//   values (finish)    = [frames_identical, frames_total, sha256_match]   (a FrameScore without `frame`, #394)
//
// sha256_match is 1.0 when the SHA-256 over all paired planes (frame by frame, plane by plane, row by row, at the
// working depth, little-endian, without row padding) is equal for the reference and the distorted stream. Two such
// byte streams have equal digests exactly when they are equal, and the digests are not part of the output, so the
// kernel compares the streams sample by sample as it goes and reports the verdict instead of hashing 2 x 3 MB per
// 1080p frame (the 500 fps of FR-VQ-023 leaves 2 ms per frame). SPEC-ASSUMPTION(#419).
//
// The first differing sample of the sequence is reported once, as VQ-DIFF-0001 (Info) through the DiagSink
// (diag_sink.h), in the form "first differing sample: frame {display_index}, plane {plane}, ({x},{y}): ref {a} dist
// {b}"; the plane is 0 for Y, 1 for U, 2 for V.

#include "metrics/kernels/identity.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "metrics/kernels/diag_sink.h"
#include "metrics/kernels/map_sink.h"
#include "metrics/kernels/plane_check.h"
#include "ovt_metrics_generated.h"

namespace ovt::metrics {

namespace {
std::atomic<DiagSink*> g_diag_sink{nullptr};
}  // namespace

void set_diag_sink(DiagSink* sink) noexcept {
  g_diag_sink.store(sink, std::memory_order_release);
}

DiagSink* diag_sink() noexcept {
  return g_diag_sink.load(std::memory_order_acquire);
}

}  // namespace ovt::metrics

namespace {

constexpr std::array<std::string_view, 3> kComponentNames = {"y", "u", "v"};
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Options {
  uint32_t map_block = 0;  // 0 = no maps
};

bool parse_options(const char* text, Options& out) {
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) {
    return false;
  }
  for (const auto& [key, value] : j.items()) {
    if (key != "map_block") {
      return false;  // rule 6: unknown keys are rejected
    }
    if (value.is_null()) {
      out.map_block = 0;
    } else if (value.is_number_integer() && (value == 8 || value == 16 || value == 32 || value == 64)) {
      out.map_block = value.get<uint32_t>();
    } else {
      return false;
    }
  }
  return true;
}

const std::vector<uint8_t>& descriptor_bytes() {
  static const std::vector<uint8_t> bytes = [] {
    flatbuffers::FlatBufferBuilder fbb(1024);
    const auto id = fbb.CreateString("identity");
    const auto name = fbb.CreateString("Identity (lossless check)");
    const auto outputs = fbb.CreateVectorOfStrings(std::vector<std::string>{"y", "u", "v", "all", "map"});
    const auto license = fbb.CreateString("LGPL-2.1-or-later");
    const auto version = fbb.CreateString("identity 1.0");
    ovt::fb::MetricDescriptorBuilder d(fbb);
    d.add_id(id);
    d.add_display_name(name);
    d.add_kind(ovt::fb::MetricKind::FullReference);
    d.add_domain(ovt::fb::WorkingDomain::CodeValue);
    d.add_per_plane(true);
    d.add_higher_is_better(true);
    d.add_range_min(0.0);
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

// The instance behind the opaque ovt_Metric* of the ABI (see the note at X264State in ssim_x264.cpp).
struct IdentityState {
  Options options;
  uint32_t bit_depth = 0;
  uint8_t planes = 0;
  uint64_t frames = 0;
  uint64_t frames_identical = 0;
  bool reported = false;                                 // VQ-DIFF-0001 is raised once per instance
  std::unique_ptr<flatbuffers::FlatBufferBuilder> last;  // the FrameScore of the last score() / finish() (rule 4)
};

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): ovt_Metric is opaque to the ABI's callers.
ovt_Metric* handle(IdentityState* s) noexcept {
  return reinterpret_cast<ovt_Metric*>(s);
}
IdentityState* state(ovt_Metric* m) noexcept {
  return reinterpret_cast<IdentityState*>(m);
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace

namespace ovt::metrics {

namespace {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): rows of the strided pictures that
// code_value_pair_ok() validated (non-null planes, equal non-zero geometry).

// "identity/<component>/<decode_index:06>/<B>" (§9.4.7).
std::string blob_id(size_t component, int32_t decode_index, uint32_t block) {
  std::string num = std::to_string(decode_index);
  if (num.size() < 6) {
    num.insert(0, 6 - num.size(), '0');
  }
  return "identity/" + std::string(kComponentNames[component]) + "/" + num + "/" + std::to_string(block);
}

struct FirstDiff {
  uint8_t plane;
  uint32_t x;
  uint32_t y;
  uint32_t ref;
  uint32_t dist;
};

struct PlaneResult {
  uint64_t equal = 0;
  uint64_t total = 0;
};

// Fraction identical per block from the equal-sample counts: the block's intersection with the plane, in blocks of
// bw x bh samples (edge blocks hold fewer); a block outside the plane stays NaN.
void block_fractions(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, const std::vector<uint32_t>& equal_in_block,
                     uint32_t blocks_w, uint32_t blocks_h, std::vector<float>& map) {
  map.assign(equal_in_block.size(), std::numeric_limits<float>::quiet_NaN());
  for (uint32_t by = 0; by < blocks_h; ++by) {
    for (uint32_t bx = 0; bx < blocks_w; ++bx) {
      const uint32_t cw = (bx * bw) < w ? std::min(bw, w - (bx * bw)) : 0;
      const uint32_t ch = (by * bh) < h ? std::min(bh, h - (by * bh)) : 0;
      if (cw != 0 && ch != 0) {
        const size_t i = (static_cast<size_t>(by) * blocks_w) + bx;
        map[i] = static_cast<float>(equal_in_block[i]) / static_cast<float>(cw * ch);
      }
    }
  }
}

// Counts the equal samples of plane p (and fills `map`: fraction identical per block, edge blocks over the samples
// they have). Finds the first differing sample in raster order when `first` is not yet set.
PlaneResult compare_plane(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, uint8_t p, uint32_t block,
                          std::vector<float>* map, uint32_t* blocks_w, uint32_t* blocks_h,
                          std::optional<FirstDiff>& first) {
  const uint32_t w = r.width[p];
  const uint32_t h = r.height[p];
  const uint32_t sb = r.sample_bytes;
  const uint32_t bw = std::max(1U, block / (r.width[0] > w ? 2U : 1U));
  const uint32_t bh = std::max(1U, block / (r.height[0] > h ? 2U : 1U));
  std::vector<uint32_t> equal_in_block;
  if (map != nullptr) {
    *blocks_w = (r.width[0] + block - 1) / block;
    *blocks_h = (r.height[0] + block - 1) / block;
    equal_in_block.assign(static_cast<size_t>(*blocks_w) * *blocks_h, 0);
  }
  PlaneResult res;
  res.total = static_cast<uint64_t>(w) * h;
  const size_t row_bytes = static_cast<size_t>(w) * sb;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* const ra = r.data[p] + (static_cast<size_t>(y) * static_cast<size_t>(r.stride_bytes[p]));
    const uint8_t* const rb = d.data[p] + (static_cast<size_t>(y) * static_cast<size_t>(d.stride_bytes[p]));
    const bool same_row = std::memcmp(ra, rb, row_bytes) == 0;  // the fast path of a lossless pair
    if (same_row && map == nullptr) {
      res.equal += w;
      continue;
    }
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t a = load_sample(ra, x, sb);
      const uint32_t b = load_sample(rb, x, sb);
      if (a == b) {
        ++res.equal;
        if (map != nullptr) {
          ++equal_in_block[(static_cast<size_t>(std::min(*blocks_h - 1, y / bh)) * *blocks_w) +
                           std::min(*blocks_w - 1, x / bw)];
        }
      } else if (!first) {
        first = FirstDiff{p, x, y, a, b};
      }
    }
  }
  if (map != nullptr) {
    block_fractions(w, h, bw, bh, equal_in_block, *blocks_w, *blocks_h, *map);
  }
  return res;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

ovt_Buffer identity_descriptor() noexcept {
  try {
    const auto& b = descriptor_bytes();
    return ovt_Buffer{b.data(), b.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}

ovt_status identity_create(const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    auto m = std::make_unique<IdentityState>();
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

void identity_destroy(ovt_Metric* m) noexcept {
  delete state(m);  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

void report_first_mismatch(IdentityState& m, ovt_FrameId id, const FirstDiff& at) {
  m.reported = true;
  DiagSink* const sink = diag_sink();
  if (sink == nullptr) {
    return;
  }
  sink->report(KernelDiagnostic{"identity", 0, "VQ-DIFF-0001",
                                "first differing sample: frame " + std::to_string(id.display_index) + ", plane " +
                                    std::to_string(at.plane) + ", (" + std::to_string(at.x) + "," +
                                    std::to_string(at.y) + "): ref " + std::to_string(at.ref) + " dist " +
                                    std::to_string(at.dist)});
}

ovt_status identity_score(ovt_Metric* handle_in, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist,
                          ovt_FrameId id, ovt_Buffer* out) noexcept {
  IdentityState* const m = state(handle_in);
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
    const bool mapped = sink != nullptr && m->options.map_block != 0;
    std::array<double, 4> values = {kNaN, kNaN, kNaN, kNaN};
    uint64_t equal = 0;
    uint64_t total = 0;
    std::optional<FirstDiff> first;
    for (uint8_t p = 0; p < ref->planes; ++p) {
      std::vector<float> map;
      uint32_t bw = 0;
      uint32_t bh = 0;
      const PlaneResult r =
          compare_plane(*ref, *dist, p, m->options.map_block, mapped ? &map : nullptr, &bw, &bh, first);
      values[p] = static_cast<double>(r.equal) / static_cast<double>(r.total);
      equal += r.equal;
      total += r.total;
      if (mapped) {
        sink->put(blob_id(p, id.decode_index, m->options.map_block),
                  encode_ovtmap(bw, bh, static_cast<uint16_t>(m->options.map_block), p, 0, 0.0F, 1.0F, map));
      }
    }
    values[3] = static_cast<double>(equal) / static_cast<double>(total);
    if (first && !m->reported) {
      report_first_mismatch(*m, id, *first);
    }
    if (equal == total) {
      ++m->frames_identical;
    }
    m->bit_depth = ref->bit_depth;
    m->planes = ref->planes;
    ++m->frames;

    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(512);
    const ovt::fb::FrameId fid(id.decode_index, id.display_index);
    const auto vec = fbb->CreateVector(std::vector<double>(values.begin(), values.end()));
    const auto blob = mapped ? fbb->CreateString(blob_id(0, id.decode_index, m->options.map_block))
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

ovt_status identity_finish(ovt_Metric* handle_in, ovt_Buffer* out) noexcept {
  IdentityState* const m = state(handle_in);
  if (m == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  if (m->frames == 0) {
    return OVT_OK;  // nothing scored, nothing to report
  }
  try {
    // The sequence score: a FrameScore without `frame` (#394 point 2).
    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(256);
    const std::vector<double> values = {static_cast<double>(m->frames_identical), static_cast<double>(m->frames),
                                        m->frames_identical == m->frames ? 1.0 : 0.0};
    const auto vec = fbb->CreateVector(values);
    fbb->Finish(ovt::fb::CreateFrameScore(*fbb, nullptr, vec));
    m->last = std::move(fbb);
    *out = ovt_Buffer{m->last->GetBufferPointer(), m->last->GetSize()};
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

const ovt_MetricApi kIdentityApi = {sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, identity_descriptor, identity_create,
                                    identity_destroy,      identity_score,         identity_finish};

}  // namespace

const ovt_MetricApi* identity_metric_api() noexcept {
  return &kIdentityApi;
}

}  // namespace ovt::metrics
