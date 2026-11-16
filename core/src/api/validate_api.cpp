// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/validate.h entry points (chapter 12 §12.3.8, chapter 06 §6.9, WI-0.086 step 7): thin wrappers of
// ovt::model::validate_frame_analysis / validate_stream_index that return the violations as a DiagnosticList
// FlatBuffer (identifier "OVTD"). No exception crosses the ABI (§12.3.1 rule 8): std::bad_alloc becomes
// OVT_E_NOMEM, anything else OVT_E_INTERNAL. Caller-owned buffers come from ovt::api::make_buffer (rule 3a).
// SPEC-ASSUMPTION(#674): the table, the Diagnostic form of a Violation and the status semantics.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "api/buffer.hpp"
#include "ovt/cxx/log.hpp"
#include "ovt/cxx/model/validator.hpp"
#include "ovt/status.h"
#include "ovt/validate.h"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using ovt::model::Violation;

constexpr const char* kIdentifier = "OVTD";
constexpr const char* kSpecRef = "chapter 06 §6.9";

// "V-012" -> "MDL-VAL-0012" (§12.3.8: a bare "V-012" would fail V-029's code regex). A rule id of another
// shape is kept verbatim after the prefix, so nothing is ever dropped.
std::string diagnostic_code(const std::string& rule) {
  std::string code = "MDL-VAL-";
  if (rule.size() == 5 && rule[0] == 'V' && rule[1] == '-') {
    code += '0';
    code.append(rule, 2, 3);
  } else {
    code += rule;
  }
  return code;
}

// "V-012: <message>" plus " (at <path>)" when the rule names a location (Diagnostic has no path field).
std::string diagnostic_message(const Violation& v) {
  std::string message = v.rule;
  message += ": ";
  message += v.message;
  if (!v.path.empty()) {
    message += " (at ";
    message += v.path;
    message += ')';
  }
  return message;
}

ovt_status give_list(const std::vector<Violation>& violations, ovt_Buffer* out) {
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<fb::Diagnostic>> items;
  items.reserve(violations.size());
  for (const Violation& v : violations) {
    const auto code = builder.CreateString(diagnostic_code(v.rule));
    const auto message = builder.CreateString(diagnostic_message(v));
    const auto spec_ref = builder.CreateString(kSpecRef);
    items.push_back(fb::CreateDiagnostic(builder, v.severity, code, message, nullptr, spec_ref));
  }
  const auto vec = builder.CreateVector(items);
  builder.Finish(fb::CreateDiagnosticList(builder, vec), kIdentifier);
  return ovt::api::make_buffer(builder.GetBufferPointer(), builder.GetSize(), out);
}

template <class Fn>
ovt_status guarded(const char* what, Fn&& fn) noexcept {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (const std::exception& e) {
    try {
      OVT_LOG(ovt::log::Level::Debug, "api", "{}: {}", what, e.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch): logging must not change the status
    }
    return OVT_E_INTERNAL;
  } catch (...) {
    return OVT_E_INTERNAL;
  }
}

void zero(ovt_Buffer* out) noexcept {
  if (out != nullptr) {
    out->data = nullptr;
    out->size = 0;
  }
}

}  // namespace

extern "C" {

OVT_API ovt_status ovt_validate_frame_analysis(const uint8_t* fb_bytes, size_t len, const uint8_t* expected_source_hash,
                                               int codec_override, ovt_Buffer* out) {
  zero(out);
  if (fb_bytes == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  if (codec_override < -1 || codec_override > static_cast<int>(fb::Codec::MAX)) {
    return OVT_E_INVALID_ARG;
  }
  return guarded("ovt_validate_frame_analysis", [&]() -> ovt_status {
    const std::span<const std::uint8_t> buffer(fb_bytes, len);
    const std::optional<fb::Codec> codec =
        codec_override < 0 ? std::nullopt : std::optional<fb::Codec>(static_cast<fb::Codec>(codec_override));
    const std::vector<Violation> violations =
        expected_source_hash == nullptr
            ? ovt::model::validate_frame_analysis(buffer, codec)
            : ovt::model::validate_frame_analysis(buffer, std::span<const std::uint8_t, 32>(expected_source_hash, 32),
                                                  codec);
    return give_list(violations, out);
  });
}

OVT_API ovt_status ovt_validate_stream_index(const uint8_t* fb_bytes, size_t len, ovt_Buffer* out) {
  zero(out);
  if (fb_bytes == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  return guarded("ovt_validate_stream_index", [&]() -> ovt_status {
    const std::span<const std::uint8_t> buffer(fb_bytes, len);
    return give_list(ovt::model::validate_stream_index(buffer), out);
  });
}

}  // extern "C"
