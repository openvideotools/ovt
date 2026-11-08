// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers shared by the tests that drive ovt-plugin-ffmpeg through the host's PluginRegistry (WI-2.169, WI-2.170):
// the loaded plugin, one decoder instance, FlatBuffer access and a bit writer for hand-made H.264 units.
#ifndef OVT_PLUGIN_FFMPEG_TESTS_PLUGIN_TEST_UTIL_HPP
#define OVT_PLUGIN_FFMPEG_TESTS_PLUGIN_TEST_UTIL_HPP

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "h264_synth.hpp"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"

namespace ovt_ffmpeg_test {

inline const ovt::analysis::LoadedPlugin& plugin() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_FFMPEG_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* p = registry.find("ffmpeg");
  REQUIRE(p != nullptr);
  return *p;
}

inline const ovt_AnalysisDecoderApi& plugin_api() {
  return *plugin().api;
}

// One decoder instance, destroyed at scope exit.
struct Instance {
  explicit Instance(const ovt_AnalysisDecoderApi& a, int threads = 1, const uint8_t* cp = nullptr, size_t len = 0)
      : api(a) {
    status = api.create(cp, len, threads, &dec);
  }
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

inline const ovt::fb::FrameAnalysis* as_fa(const ovt_Buffer& fb) {
  flatbuffers::Verifier verifier(fb.data, fb.size);
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
  return ovt::fb::GetFrameAnalysis(fb.data);
}

struct Nal {
  const uint8_t* data;
  size_t size;
};

inline std::vector<Nal> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<Nal> nals;
  size_t i = 0;
  size_t start = SIZE_MAX;
  while (i + 3 <= s.size()) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && s[end - 1] == 0) {
          --end;  // trailing_zero_8bits and the leading zero of a 4-byte start code
        }
        nals.push_back({s.data() + start, end - start});
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < s.size()) {
    nals.push_back({s.data() + start, s.size() - start});
  }
  return nals;
}

inline std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Pushes @p bytes NAL by NAL as the host does (decode_index_hint = the access unit index, pts = 1000 + index) and
// calls @p on_frame(FrameAnalysis*) for every frame, in pull() order, with only the FlatBuffer valid.
template <typename F>
void for_each_frame(const std::vector<uint8_t>& bytes, int threads, F&& on_frame) {
  const ovt_AnalysisDecoderApi& api = plugin_api();
  Instance in(api, threads);
  REQUIRE(in.status == OVT_OK);
  const auto drain = [&] {
    for (;;) {
      ovt_Buffer fb{};
      REQUIRE(api.pull(in.dec, 0, &fb) == OVT_OK);
      if (fb.data == nullptr) {
        return;
      }
      on_frame(as_fa(fb));
      api.release(in.dec, fb);
    }
  };
  int au = 0;
  bool seen_vcl = false;
  for (const Nal& n : split_annexb(bytes)) {
    if (n.size < 2) {
      continue;
    }
    const int type = n.data[0] & 0x1F;
    const bool vcl = type >= 1 && type <= 5;
    const bool starts_au = (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
    if (seen_vcl && ((vcl && (n.data[1] & 0x80) != 0) || starts_au)) {
      ++au;
      seen_vcl = false;
    }
    seen_vcl = seen_vcl || vcl;
    (void)api.push(in.dec, n.data, n.size, au, 1000 + au);
    drain();
  }
  REQUIRE(api.flush(in.dec) == OVT_OK);
  drain();
}

}  // namespace ovt_ffmpeg_test

#endif  // OVT_PLUGIN_FFMPEG_TESTS_PLUGIN_TEST_UTIL_HPP
