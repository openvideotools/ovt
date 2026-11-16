// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the ovt/syntax.h and ovt/validate.h entry points (#673, #674; chapter 14 §14.7.1): the C
// wrappers of from_json / to_json / validate_* sit behind `ovt cache import`, which reads untrusted text and
// records. Byte 0 selects the call, the rest is its input. Invariants, each an abort when violated: the status is
// one of OVT_OK, OVT_E_INVALID_ARG and OVT_E_CORRUPT (no exception reaches the ABI, no OVT_E_INTERNAL); a failed
// call leaves the out-buffer zeroed; a validate call that returns OVT_OK hands back a DiagnosticList that
// verifies with identifier "OVTD"; an accepted import exports again and re-imports to the same bytes.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ovt/status.h"
#include "ovt/syntax.h"
#include "ovt/validate.h"
#include "ovt_model_generated.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

void require_status(ovt_status st, const ovt_Buffer& out) {
  require(st == OVT_OK || st == OVT_E_INVALID_ARG || st == OVT_E_CORRUPT);
  if (st != OVT_OK) {
    require(out.data == nullptr && out.size == 0);
  } else {
    require(out.data != nullptr);
  }
}

void require_list(const ovt_Buffer& out) {
  require(out.size >= 8 && std::memcmp(flatbuffers::GetBufferIdentifier(out.data), "OVTD", 4) == 0);
  flatbuffers::Verifier verifier(out.data, out.size);
  require(verifier.VerifyBuffer<ovt::fb::DiagnosticList>("OVTD"));
  require(flatbuffers::GetRoot<ovt::fb::DiagnosticList>(out.data)->diagnostics() != nullptr);
}

const char* root_of(std::uint8_t selector) {
  switch (selector % 4) {
    case 0:
      return "FrameAnalysis";
    case 1:
      return "StreamIndex";
    case 2:
      return "ovt.fb.MetricsReport";
    default:
      return "ContainerRoot";
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::uint8_t selector = data[0];
  const std::uint8_t* payload = data + 1;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const std::size_t len = size - 1;
  ovt_Buffer out{nullptr, 0};
  switch (selector & 3U) {
    case 0: {  // JSON text -> buffer -> text -> buffer
      const std::string text(reinterpret_cast<const char*>(payload), len);
      const char* root = root_of(selector >> 2U);
      const ovt_status st = ovt_json_to_fb(text.c_str(), root, &out);
      require_status(st, out);
      if (st == OVT_OK) {
        ovt_Buffer exported{nullptr, 0};
        require(ovt_fb_to_json(out.data, out.size, root, static_cast<int>(selector >> 7U), &exported) == OVT_OK);
        require(exported.data[exported.size] == 0);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ovt_Buffer again{nullptr, 0};
        require(ovt_json_to_fb(reinterpret_cast<const char*>(exported.data), root, &again) == OVT_OK);
        require(again.size == out.size && std::memcmp(again.data, out.data, out.size) == 0);
        ovt_buffer_free(&again);
        ovt_buffer_free(&exported);
      }
      break;
    }
    case 1: {  // bytes -> JSON text
      const ovt_status st = ovt_fb_to_json(payload, len, root_of(selector >> 2U), 1, &out);
      require_status(st, out);
      break;
    }
    case 2: {  // FrameAnalysis validator, with the first 32 payload bytes as the expected hash when bit 2 is set
      const bool with_hash = ((selector >> 2U) & 1U) != 0 && len >= 32;
      const int codec = static_cast<int>(selector >> 3U) - 1;  // -1 .. 30: valid and invalid overrides
      const ovt_status st = ovt_validate_frame_analysis(payload, len, with_hash ? payload : nullptr, codec, &out);
      require_status(st, out);
      if (st == OVT_OK) {
        require_list(out);
      }
      break;
    }
    default: {  // StreamIndex validator
      const ovt_status st = ovt_validate_stream_index(payload, len, &out);
      require_status(st, out);
      if (st == OVT_OK) {
        require_list(out);
      }
      break;
    }
  }
  ovt_buffer_free(&out);
  require(out.data == nullptr && out.size == 0);
  return 0;
}
