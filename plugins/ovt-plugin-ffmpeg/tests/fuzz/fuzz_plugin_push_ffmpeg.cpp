// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// fuzz_plugin_push_ffmpeg (WI-2.583, chapter 14 §14.7.1): push() of ovt-plugin-ffmpeg with a canned valid parameter-set
// prefix. Input byte 0 picks the thread count (1..4) and whether pull() asks for the reconstruction planes; the rest is
// split at Annex B start codes (the whole rest is one unit without a start code) and pushed unit by unit, one access
// unit per VCL unit. Every status must be OVT_OK or OVT_E_CORRUPT (a decoder error is OVT_E_CORRUPT), and every
// FrameAnalysis the plugin returns must pass the FlatBuffers verifier; anything else, like a crash or a sanitizer
// report, is a finding. A crash inside FFmpeg itself is reported upstream and the input kept under
// core/tests/fuzz/regressions/plugin_push_ffmpeg/ with the replay marked [xfail-upstream] (§14.7.1).
// Seeds: core/tests/fuzz/corpus/plugin_push_ffmpeg/ (hand-made I_PCM pictures of tests/h264_synth.hpp and two 32x16
// x264 clips, one CAVLC baseline, one CABAC main with P and B pictures).

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "../h264_synth.hpp"
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
      ovt::analysis::PluginRegistry::Options{std::string(OVT_FFMPEG_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("ffmpeg");
  if (plugin == nullptr) {
    contract_violation();  // a misconfigured fuzz target must not pass silently
  }
  return *plugin->api;
}

void expect_ok_or_corrupt(ovt_status st) {
  if (st != OVT_OK && st != OVT_E_CORRUPT) {
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

// Offsets of the NAL units after each 00 00 01 (a 4-byte start code's leading zero is not part of the previous unit).
void split_units(const uint8_t* data, size_t size, std::vector<std::pair<size_t, size_t>>* units) {
  size_t start = SIZE_MAX;
  for (size_t i = 0; i + 3 <= size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {  // NOLINT(*-pro-bounds-pointer-arithmetic)
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && data[end - 1] == 0) {  // NOLINT(*-pro-bounds-pointer-arithmetic)
          --end;
        }
        units->emplace_back(start, end - start);
      }
      start = i + 3;
      i += 2;
    }
  }
  if (start == SIZE_MAX) {
    units->emplace_back(0, size);
  } else if (start < size) {
    units->emplace_back(start, size - start);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const ovt_AnalysisDecoderApi& api = plugin_api();
  const int threads = 1 + (data[0] & 3);
  const uint32_t planes = (data[0] & 4U) != 0 ? kReconMask : 0U;
  ovt_AnalysisDecoder* dec = nullptr;
  if (api.create(nullptr, 0, threads, &dec) != OVT_OK || dec == nullptr) {
    contract_violation();
  }
  const std::vector<uint8_t> sps = ovt_ffmpeg_test::pcm_sps();
  const std::vector<uint8_t> pps = ovt_ffmpeg_test::pcm_pps();
  expect_ok_or_corrupt(api.push(dec, sps.data(), sps.size(), 0, 0));
  expect_ok_or_corrupt(api.push(dec, pps.data(), pps.size(), 0, 0));

  std::vector<std::pair<size_t, size_t>> units;
  split_units(data + 1, size - 1, &units);  // NOLINT(*-pro-bounds-pointer-arithmetic)
  int au = 0;
  for (const auto& [offset, length] : units) {
    if (length == 0) {
      continue;
    }
    const uint8_t* nal = data + 1 + offset;                                // NOLINT(*-pro-bounds-pointer-arithmetic)
    const int type = nal[0] & 0x1F;                                        // NOLINT(*-pro-bounds-pointer-arithmetic)
    if ((type >= 1 && type <= 5) && length > 1 && (nal[1] & 0x80) != 0) {  // NOLINT(*-pro-bounds-pointer-arithmetic)
      ++au;  // first_mb_in_slice == 0: a new access unit, as the host's splitter counts them
    }
    expect_ok_or_corrupt(api.push(dec, nal, length, au, 1000 + au));
    drain(api, dec, planes);
  }
  expect_ok_or_corrupt(api.flush(dec));
  drain(api, dec, planes);
  api.destroy(dec);
  return 0;
}
