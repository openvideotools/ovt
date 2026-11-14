// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// fuzz_plugin_push_aom (WI-2.581, chapter 14 §14.7.1): push() of ovt-plugin-aom. Input byte 0 picks the thread count
// (1..4) and whether pull() asks for the reconstruction planes; the rest is a Section 5 OBU stream. It is cut into OBUs
// by the size fields as far as they are well formed (the rest is one more chunk) and pushed OBU by OBU the way the host
// does (a frame OBU opens the next decode index, the other OBUs carry the current one). Every status must be OVT_OK,
// OVT_E_CORRUPT (an OBU or a frame libaom rejects) or OVT_E_PLUGIN (the frame-count check against the temporal
// delimiters, which a damaged stream breaks), and every FrameAnalysis the plugin returns must pass the FlatBuffers
// verifier; anything else, like a crash or a sanitizer report, is a finding. A crash inside libaom itself is reported
// upstream and the input kept under core/tests/fuzz/regressions/plugin_push_aom/ with the replay marked
// [xfail-upstream] (§14.7.1). Seeds: core/tests/fuzz/corpus/plugin_push_aom/ (the temporal units of the libaom vectors
// av1-1-b8-01-size-16x16 and -size-66x66 (also cut in half), with one byte of settings in front).

#include <flatbuffers/flatbuffers.h>

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
constexpr unsigned kObuFrameHeader = 3;
constexpr unsigned kObuFrame = 6;

// A finding that is not a crash of the decoder: the plugin broke its own contract.
[[noreturn]] void contract_violation() {
  std::abort();
}

const ovt_AnalysisDecoderApi& plugin_api() {
  static ovt::analysis::PluginRegistry registry(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_AOM_PLUGIN_DIR), std::vector<std::string>{}});
  const ovt::analysis::LoadedPlugin* plugin = registry.find("aom");
  if (plugin == nullptr) {
    contract_violation();  // a misconfigured fuzz target must not pass silently
  }
  return *plugin->api;
}

void expect_ok_or_known(ovt_status st) {
  if (st != OVT_OK && st != OVT_E_CORRUPT && st != OVT_E_PLUGIN) {
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

// The length of the OBU at data[0..size): header, optional extension, leb128 size field and payload; the whole rest
// when the header has no size field or the size does not fit.
size_t obu_length(const uint8_t* data, size_t size) {
  const bool extension = (data[0] & 0x04U) != 0;  // NOLINT(*-pro-bounds-pointer-arithmetic)
  const bool has_size = (data[0] & 0x02U) != 0;   // NOLINT(*-pro-bounds-pointer-arithmetic)
  size_t pos = extension ? 2 : 1;
  if (!has_size) {
    return size;
  }
  uint64_t payload = 0;
  for (unsigned i = 0; i < 8; ++i) {
    if (pos >= size) {
      return size;
    }
    const uint8_t b = data[pos++];  // NOLINT(*-pro-bounds-pointer-arithmetic)
    payload |= uint64_t{b & 0x7FU} << (7U * i);
    if ((b & 0x80U) == 0) {
      return payload > size - pos ? size : pos + static_cast<size_t>(payload);
    }
  }
  return size;
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
  const uint8_t* rest = data + 1;  // NOLINT(*-pro-bounds-pointer-arithmetic)
  size_t left = size - 1;
  int32_t next = 0;
  while (left > 0) {
    const size_t n = obu_length(rest, left);
    const unsigned type = (rest[0] >> 3U) & 0x0FU;  // NOLINT(*-pro-bounds-pointer-arithmetic)
    int32_t hint = next;
    if (type == kObuFrame || type == kObuFrameHeader) {
      hint = next++;
    }
    expect_ok_or_known(api.push(dec, rest, n, hint, hint));
    drain(api, dec, planes);
    rest += n;  // NOLINT(*-pro-bounds-pointer-arithmetic)
    left -= n;
  }
  expect_ok_or_known(api.flush(dec));
  drain(api, dec, planes);
  api.destroy(dec);
  return 0;
}
