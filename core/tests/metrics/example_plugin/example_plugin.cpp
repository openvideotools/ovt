// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libovt_metric_example: the dummy metric plugin of WI-3.300 acceptance test 3 and the registry tests. Like every
// plugin it links no part of libovt-core (chapter 0 §0.6.3): the ABI header and the generated FlatBuffers headers
// only. It scores nothing useful: score() returns a FrameScore whose single value is the decode index, finish()
// returns no sequence score. Knobs for the broken variants: OVT_EXAMPLE_ID (descriptor id), OVT_EXAMPLE_ABI_VERSION,
// OVT_EXAMPLE_API_SIZE (struct_size), OVT_EXAMPLE_NULL_SCORE (a NULL score entry), OVT_EXAMPLE_BAD_DESCRIPTOR
// (descriptor() returns garbage bytes), OVT_EXAMPLE_BAD_UTF8 (a display_name that is not valid UTF-8),
// OVT_EXAMPLE_BOTH_ENTRY_POINTS (also exports ovt_analysis_decoder_v1).

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "ovt/abi/metric_v1.h"
#include "ovt_metrics_generated.h"

#ifndef OVT_EXAMPLE_ID
#define OVT_EXAMPLE_ID "example"
#endif
#ifndef OVT_EXAMPLE_ABI_VERSION
#define OVT_EXAMPLE_ABI_VERSION OVT_METRIC_ABI_VERSION
#endif
#ifndef OVT_EXAMPLE_API_SIZE
#define OVT_EXAMPLE_API_SIZE sizeof(ovt_MetricApi)
#endif

struct ovt_Metric {
  std::string options;
  std::unique_ptr<flatbuffers::FlatBufferBuilder> last;  // the FrameScore of the last score() (rule 4)
};

namespace {

// Unused when OVT_EXAMPLE_BAD_DESCRIPTOR is defined.
[[maybe_unused]] const std::vector<uint8_t>& descriptor_bytes() {
  static const std::vector<uint8_t> bytes = [] {
    flatbuffers::FlatBufferBuilder fbb(512);
    const auto id = fbb.CreateString(OVT_EXAMPLE_ID);
#ifdef OVT_EXAMPLE_BAD_UTF8
    const auto name = fbb.CreateString("Example metric \xff\xfe");  // a valid FlatBuffer, not valid UTF-8
#else
    const auto name = fbb.CreateString("Example metric (test plugin)");
#endif
    const auto outputs = fbb.CreateVectorOfStrings(std::vector<std::string>{"score"});
    const auto license = fbb.CreateString("LGPL-2.1-or-later");
    const auto version = fbb.CreateString("1.0.0");
    ovt::fb::MetricDescriptorBuilder d(fbb);
    d.add_id(id);
    d.add_display_name(name);
    d.add_kind(ovt::fb::MetricKind::FullReference);
    d.add_domain(ovt::fb::WorkingDomain::CodeValue);
    d.add_per_plane(false);
    d.add_higher_is_better(true);
    d.add_range_min(0.0);
    d.add_range_max(1.0);
    d.add_temporal_unit_frames(1);
    d.add_outputs(outputs);
    d.add_license(license);
    d.add_version(version);
    fbb.Finish(d.Finish());
    return std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
  }();
  return bytes;
}

ovt_Buffer example_descriptor() noexcept {
#ifdef OVT_EXAMPLE_BAD_DESCRIPTOR
  static const std::array<uint8_t, 7> garbage = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x02};
  return ovt_Buffer{garbage.data(), garbage.size()};
#else
  try {
    const auto& b = descriptor_bytes();
    return ovt_Buffer{b.data(), b.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
#endif
}

ovt_status example_create(const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  // Rule 6: the only accepted option object is {} (whitespace allowed); anything else is an unknown key.
  for (const char* p = options_json; *p != '\0'; ++p) {
    if (*p != '{' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') {
      return OVT_E_INVALID_ARG;
    }
  }
  *out = new (std::nothrow) ovt_Metric();
  if (*out == nullptr) {
    return OVT_E_NOMEM;
  }
  try {
    (*out)->options = options_json;
  } catch (...) {
    delete *out;
    *out = nullptr;
    return OVT_E_NOMEM;
  }
  return OVT_OK;
}

void example_destroy(ovt_Metric* m) noexcept {
  delete m;  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

// Unused when OVT_EXAMPLE_NULL_SCORE is defined.
[[maybe_unused]] ovt_status example_score(ovt_Metric* m, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist,
                                          ovt_FrameId id, ovt_Buffer* out) noexcept {
  if (m == nullptr || dist == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  if (ref == nullptr) {
    return OVT_E_INVALID_ARG;  // a full-reference metric needs both pictures
  }
  try {
    auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(256);
    const ovt::fb::FrameId fid(id.decode_index, id.display_index);
    const auto values = fbb->CreateVector(std::vector<double>{static_cast<double>(id.decode_index)});
    fbb->Finish(ovt::fb::CreateFrameScore(*fbb, &fid, values));
    m->last = std::move(fbb);
    *out = ovt_Buffer{m->last->GetBufferPointer(), m->last->GetSize()};
    return OVT_OK;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

ovt_status example_finish(ovt_Metric* m, ovt_Buffer* out) noexcept {
  if (m == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};  // no sequence-level score
  return OVT_OK;
}

const ovt_MetricApi kApi = {
    static_cast<uint32_t>(OVT_EXAMPLE_API_SIZE),
    OVT_EXAMPLE_ABI_VERSION,
    example_descriptor,
    example_create,
    example_destroy,
#ifdef OVT_EXAMPLE_NULL_SCORE
    nullptr,
#else
    example_score,
#endif
    example_finish,
};

}  // namespace

extern "C" {

OVT_PLUGIN_EXPORT const ovt_MetricApi* ovt_metric_v1(void) {
  return &kApi;
}

#ifdef OVT_EXAMPLE_BOTH_ENTRY_POINTS
// A file that exports both entry points is rejected (chapter 12 §12.3.11); the symbol only has to exist.
OVT_PLUGIN_EXPORT const void* ovt_analysis_decoder_v1(void) {
  return nullptr;
}
#endif

}  // extern "C"
