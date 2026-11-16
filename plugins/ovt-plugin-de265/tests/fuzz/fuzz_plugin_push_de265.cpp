// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// fuzz_plugin_push_de265 (WI-2.582, chapter 14 §14.7.1): push() of ovt-plugin-de265. Input byte 0 picks the thread
// count (0..3; 0 lets libde265 choose) and whether pull() asks for the reconstruction planes; the rest is an Annex B
// stream, cut into NAL units at the start codes (the whole rest is one unit without a start code) and pushed unit by
// unit the way the host does (a VCL unit with first_slice_segment_in_pic_flag opens the next decode index). Every
// status must be OVT_OK, OVT_E_CORRUPT (a unit libde265 rejects), OVT_E_UNSUPPORTED (the profile check: SCC, RExt
// and enhancement-layer units are refused with DEC-H265-0010) or OVT_E_NOMEM (a declared picture size libde265 cannot
// allocate), and every FrameAnalysis the plugin returns must pass
// the FlatBuffers verifier; anything else, like a crash or a sanitizer report, is a finding. A crash inside libde265
// itself is reported upstream and the input kept under core/tests/fuzz/regressions/plugin_push_de265/ with the replay
// marked [xfail-upstream] (§14.7.1).
// Seeds: core/tests/fuzz/corpus/plugin_push_de265/ (tiny x265 clips: 8-bit and 10-bit intra and inter pictures, and
// one stream cut off inside a slice), each with one byte of settings in front.

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"

namespace {

constexpr uint32_t kReconMask = 1U << 0;  // ovt.fb.PlaneStage.Reconstruction

// A finding that is not a crash of the decoder: the plugin broke its own contract.
[[noreturn]] void contract_violation() {
  std::abort();
}

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_DE265_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("de265");
  if (plugin == nullptr) {
    contract_violation();  // a misconfigured fuzz target must not pass silently
  }
  return *plugin->api;
}

void expect_ok_or_known(ovt_status st) {
  if (st != OVT_OK && st != OVT_E_CORRUPT && st != OVT_E_UNSUPPORTED && st != OVT_E_NOMEM) {
    contract_violation();
  }
}

void drain(const ovt_AnalysisDecoderApi& api, ovt_AnalysisDecoder* dec, uint32_t planes) {
  for (;;) {
    ovt_Buffer fb{};
    if (api.pull(dec, planes, &fb) != OVT_OK || fb.data == nullptr) {
      return;
    }
    flatbuffers::Verifier verifier(fb.data, fb.size);
    if (!ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
      contract_violation();
    }
    api.release(dec, fb);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const ovt_AnalysisDecoderApi& api = plugin_api();
  const int threads = data[0] & 3;
  const uint32_t planes = (data[0] & 4U) != 0 ? kReconMask : 0U;
  ovt_AnalysisDecoder* dec = nullptr;
  if (api.create(nullptr, 0, threads, &dec) != OVT_OK || dec == nullptr) {
    contract_violation();
  }
  const uint8_t* s = data + 1;  // NOLINT(*-pro-bounds-pointer-arithmetic)
  const size_t n = size - 1;
  // Annex B split: a unit runs from after a 00 00 01 start code to the next one, trailing zeros trimmed.
  std::vector<std::pair<size_t, size_t>> units;  // offset, length
  size_t start = SIZE_MAX;
  const auto close = [&](size_t end) {
    while (end > start && s[end - 1] == 0) {  // NOLINT(*-pro-bounds-pointer-arithmetic)
      --end;
    }
    if (end > start) {
      units.emplace_back(start, end - start);
    }
  };
  for (size_t i = 0; i + 3 <= n;) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {  // NOLINT(*-pro-bounds-pointer-arithmetic)
      if (start != SIZE_MAX) {
        close(i);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start == SIZE_MAX) {
    start = 0;  // no start code: the whole rest is one unit
  }
  close(n);
  int32_t pic = -1;
  for (const auto& [offset, length] : units) {
    const uint8_t* nal = s + offset;  // NOLINT(*-pro-bounds-pointer-arithmetic)
    const int type = (nal[0] >> 1) & 0x3F;
    if (type < 32 && length > 2 && (nal[2] & 0x80U) != 0) {
      ++pic;
    }
    const int32_t index = std::max<int32_t>(pic, 0);
    expect_ok_or_known(api.push(dec, nal, length, index, index));
    drain(api, dec, planes);
  }
  expect_ok_or_known(api.flush(dec));
  drain(api, dec, planes);
  api.destroy(dec);
  return 0;
}
