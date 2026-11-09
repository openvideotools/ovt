// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Shared helpers of the ovt-plugin-aom tests (WI-2.155, WI-2.156): the plugin loaded through the host's
// PluginRegistry, one decoder instance, and IVF reading split into Section 5 OBUs as the host index delivers them.
// Not a .hpp on purpose: it needs Catch2 and the plugin ABI headers, which clang-tidy-diff cannot resolve when it
// parses a header on its own (same as ovt-plugin-ffmpeg's plugin_test_util.inc).
#ifndef OVT_PLUGIN_AOM_TESTS_AOM_TEST_UTIL_INC
#define OVT_PLUGIN_AOM_TESTS_AOM_TEST_UTIL_INC

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"

namespace ovt::aom::test {

inline const ovt::analysis::LoadedPlugin& plugin() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_AOM_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* p = registry.find("aom");
  REQUIRE(p != nullptr);
  return *p;
}

// One decoder instance, destroyed at scope exit.
struct Instance {
  Instance() : api(*plugin().api) { status = api.create(nullptr, 0, 2, &dec); }
  ~Instance() {
    if (dec != nullptr) {
      api.destroy(dec);
    }
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  const ovt_AnalysisDecoderApi& api;
  ovt_AnalysisDecoder* dec = nullptr;
  ovt_status status = OVT_E_INTERNAL;
};

// ---- IVF / OBU reading (the host's index does this in production) -----------------------------------------------

struct Obu {
  std::vector<uint8_t> bytes;
  unsigned type = 0;
  bool show_existing = false;  // OBU_FRAME_HEADER with show_existing_frame = 1
};
using TemporalUnit = std::vector<Obu>;

inline uint32_t le32(std::span<const uint8_t> b, size_t at) {
  return uint32_t{b[at]} | (uint32_t{b[at + 1]} << 8U) | (uint32_t{b[at + 2]} << 16U) | (uint32_t{b[at + 3]} << 24U);
}

// Splits Section 5 OBUs (obu_has_size_field = 1, as libaom's vectors are written). The libaom vectors have
// reduced_still_picture_header = 0, so show_existing_frame is the first bit of a frame header.
inline TemporalUnit split_obus(std::span<const uint8_t> tu) {
  TemporalUnit out;
  size_t pos = 0;
  while (pos < tu.size()) {
    Obu o;
    const uint8_t h = tu[pos];
    REQUIRE((h & 0x02U) != 0);
    o.type = (h >> 3U) & 0x0FU;
    size_t p = pos + ((h & 0x04U) != 0 ? 2 : 1);
    uint64_t size = 0;
    for (unsigned i = 0;; ++i) {
      REQUIRE(p < tu.size());
      const uint8_t b = tu[p++];
      size |= uint64_t{b & 0x7FU} << (7U * i);
      if ((b & 0x80U) == 0) {
        break;
      }
    }
    REQUIRE(p + size <= tu.size());
    o.show_existing = o.type == 3 && size > 0 && (tu[p] & 0x80U) != 0;
    o.bytes.assign(tu.begin() + static_cast<std::ptrdiff_t>(pos), tu.begin() + static_cast<std::ptrdiff_t>(p + size));
    out.push_back(std::move(o));
    pos = p + size;
  }
  return out;
}

inline std::vector<TemporalUnit> read_ivf(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::span<const uint8_t> b(file);
  REQUIRE(b.size() >= 32);
  REQUIRE(std::string(b.begin(), b.begin() + 4) == "DKIF");
  std::vector<TemporalUnit> tus;
  size_t pos = 32;
  while (pos + 12 <= b.size()) {
    const uint32_t size = le32(b, pos);
    pos += 12;
    REQUIRE(pos + size <= b.size());
    tus.push_back(split_obus(b.subspan(pos, size)));
    pos += size;
  }
  return tus;
}

}  // namespace ovt::aom::test

#endif  // OVT_PLUGIN_AOM_TESTS_AOM_TEST_UTIL_INC
