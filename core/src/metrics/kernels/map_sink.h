// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Where the own kernels deliver their per-block maps (chapter 9 §9.4.7). The MapStore itself is WI-3.316; until it
// exists the kernels write to a MapSink the host installs, and WI-3.316 implements the sink on the store. A kernel
// finding no sink installed computes no maps. Also the .ovtmap encoder, which is the on-disk layout of §9.4.7.
// SPEC-ASSUMPTION(#385): the sink is a process-wide hook because ovt_MetricApi.create() takes only options_json.

#ifndef OVT_SRC_METRICS_KERNELS_MAP_SINK_H
#define OVT_SRC_METRICS_KERNELS_MAP_SINK_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ovt::metrics {

// Receives one finished map: `blob_id` is "<metric_id>/<component>/<decode_index:06>/<B>" (§9.4.7), `ovtmap` the
// complete .ovtmap image (ovt_MapBlobHeader followed by blocks_w*blocks_h float32). May be called from the job's
// worker threads; the implementation must be thread-safe.
class MapSink {
 public:
  MapSink() = default;
  MapSink(const MapSink&) = delete;
  MapSink& operator=(const MapSink&) = delete;
  MapSink(MapSink&&) = delete;
  MapSink& operator=(MapSink&&) = delete;
  virtual ~MapSink() = default;
  virtual void put(const std::string& blob_id, std::vector<uint8_t> ovtmap) = 0;
};

// Installs (or, with nullptr, removes) the process-wide sink. The caller keeps the sink alive until removed.
void set_map_sink(MapSink* sink) noexcept;
[[nodiscard]] MapSink* map_sink() noexcept;

inline constexpr size_t kOvtMapHeaderSize = 32;

// Encodes an .ovtmap image: the 32-byte little-endian header of §9.4.7 and `values` as float32, row-major.
// `component`: 0 Y, 1 U, 2 V, 3 combined. `value_kind`: 0 higher is better, 1 lower is better.
[[nodiscard]] inline std::vector<uint8_t> encode_ovtmap(uint32_t blocks_w, uint32_t blocks_h, uint16_t block_size,
                                                        uint8_t component, uint8_t value_kind, float value_min,
                                                        float value_max, const std::vector<float>& values) {
  std::vector<uint8_t> out(kOvtMapHeaderSize + (values.size() * sizeof(float)), 0);
  auto put32 = [&out](size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) {
      out[at + i] = static_cast<uint8_t>(v >> (8U * i));
    }
  };
  auto putf = [&put32](size_t at, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    put32(at, bits);
  };
  std::memcpy(out.data(), "OVTMAP01", 8);
  put32(8, blocks_w);
  put32(12, blocks_h);
  out[16] = static_cast<uint8_t>(block_size & 0xFFU);
  out[17] = static_cast<uint8_t>(block_size >> 8U);
  out[18] = component;
  out[19] = value_kind;
  putf(20, value_min);
  putf(24, value_max);
  // bytes 28..31: reserved, zero
  for (size_t i = 0; i < values.size(); ++i) {
    putf(kOvtMapHeaderSize + (i * sizeof(float)), values[i]);
  }
  return out;
}

}  // namespace ovt::metrics

#endif  // OVT_SRC_METRICS_KERNELS_MAP_SINK_H
