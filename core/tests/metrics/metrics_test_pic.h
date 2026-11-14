// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Picture and score helpers of the ssim_x264 tests (WI-3.308a; WI-3.308b reuses them): a planar picture in the layout
// of ovt_MetricPlanes, the FrameScore decoder, a MapSink that keeps the blobs, and the deterministic synthetic planes
// of tests/oracles/run_x264_ssim.py (`synth_plane` mirrors `synth_luma` of run_libvmaf_ssim.py sample for sample).

#ifndef OVT_TESTS_METRICS_METRICS_TEST_PIC_H
#define OVT_TESTS_METRICS_METRICS_TEST_PIC_H

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "metrics/kernels/map_sink.h"
#include "ovt/abi/metric_v1.h"
#include "ovt_metrics_generated.h"

namespace ovt::test {

struct Pic {
  std::array<std::vector<uint8_t>, 3> buf;
  ovt_MetricPlanes planes{};
  // chroma: 0 = 4:0:0, 1 = 4:2:0, 3 = 4:4:4; `pad` extra bytes at the end of every row.
  Pic(uint32_t w, uint32_t h, int chroma, uint32_t bits, uint32_t pad = 0) {
    planes.struct_size = sizeof(ovt_MetricPlanes);
    planes.planes = chroma == 0 ? 1 : 3;
    planes.bit_depth = static_cast<uint8_t>(bits);
    planes.sample_bytes = bits > 8 ? 2 : 1;
    planes.sample_format = OVT_METRIC_SAMPLE_UINT;
    planes.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
    for (size_t p = 0; p < planes.planes; ++p) {
      const uint32_t pw = (p == 0 || chroma == 3) ? w : (w + 1) / 2;
      const uint32_t ph = (p == 0 || chroma == 3) ? h : (h + 1) / 2;
      const uint32_t stride = (pw * planes.sample_bytes) + pad;
      buf[p].assign(static_cast<size_t>(stride) * ph, 0);
      planes.data[p] = buf[p].data();
      planes.stride_bytes[p] = static_cast<int32_t>(stride);
      planes.width[p] = pw;
      planes.height[p] = ph;
    }
  }
  Pic(const Pic&) = delete;
  Pic& operator=(const Pic&) = delete;
  [[nodiscard]] size_t offset(size_t p, uint32_t x, uint32_t y) const {
    return (static_cast<size_t>(y) * static_cast<size_t>(planes.stride_bytes[p])) +
           (static_cast<size_t>(x) * planes.sample_bytes);
  }
  void set(size_t p, uint32_t x, uint32_t y, uint32_t v) {
    const size_t off = offset(p, x, y);
    buf[p][off] = static_cast<uint8_t>(v & 0xFFU);
    if (planes.sample_bytes == 2) {
      buf[p][off + 1] = static_cast<uint8_t>(v >> 8U);
    }
  }
  [[nodiscard]] uint32_t get(size_t p, uint32_t x, uint32_t y) const {
    const size_t off = offset(p, x, y);
    return planes.sample_bytes == 2
               ? (static_cast<uint32_t>(buf[p][off]) | (static_cast<uint32_t>(buf[p][off + 1]) << 8U))
               : buf[p][off];
  }
};

struct Scored {
  std::vector<double> values;
  std::string map_blob_id;
  uint16_t map_block_size = 0;
};

inline Scored decode_score(const ovt_Buffer& b) {
  const auto* fs = flatbuffers::GetRoot<ovt::fb::FrameScore>(b.data);
  Scored s;
  if (fs->values() != nullptr) {
    s.values.assign(fs->values()->begin(), fs->values()->end());
  }
  if (fs->map_blob_id() != nullptr) {
    s.map_blob_id = fs->map_blob_id()->str();
  }
  s.map_block_size = fs->map_block_size();
  return s;
}

class CaptureSink final : public ovt::metrics::MapSink {
 public:
  void put(const std::string& id, std::vector<uint8_t> bytes) override {
    const std::lock_guard lock(mu);
    maps[id] = std::move(bytes);
  }
  std::mutex mu;
  std::map<std::string, std::vector<uint8_t>> maps;
};

inline uint32_t synth_hash(uint32_t x, uint32_t y, uint32_t f, uint32_t s) {
  uint32_t v = (x * 73856093U) ^ (y * 19349663U) ^ (f * 83492791U) ^ (s * 2654435769U);
  v *= 2246822519U;
  v ^= v >> 15U;
  v *= 3266489917U;
  v ^= v >> 13U;
  return v;
}

// `synth_luma(w, h, bits, frame, distorted)` of run_libvmaf_ssim.py for one plane of Pic `pic`.
inline void synth_plane(Pic& pic, size_t p, uint32_t frame, bool distorted) {
  for (uint32_t y = 0; y < pic.planes.height[p]; ++y) {
    for (uint32_t x = 0; x < pic.planes.width[p]; ++x) {
      int64_t base = 40 + ((3 * x + 2 * y + 7 * frame) % 128) + (synth_hash(x, y, frame, 1) >> 27U);
      if (distorted) {
        const uint32_t cells = (((x / 4) + (y / 4) + frame) & 1U) * 6U;
        base += static_cast<int64_t>(synth_hash(x, y, frame, 2) >> 28U) - 8 + static_cast<int64_t>(cells);
      }
      if (pic.planes.bit_depth == 10) {
        base = (base * 4) + (synth_hash(x, y, frame, distorted ? 4 : 3) >> 30U);
      }
      pic.set(p, x, y, static_cast<uint32_t>(base));
    }
  }
}

// All planes as run_x264_ssim.py writes them: plane p of frame f is seeded with f + 16 * p.
inline void fill_synth(Pic& pic, uint32_t frame, bool distorted) {
  for (size_t p = 0; p < pic.planes.planes; ++p) {
    synth_plane(pic, p, frame + (16U * static_cast<uint32_t>(p)), distorted);
  }
}

}  // namespace ovt::test

#endif  // OVT_TESTS_METRICS_METRICS_TEST_PIC_H
