// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the MetricsJobSpec JSON parser (chapter 12 §12.3.13, chapter 14 §14.7.1, WI-3.300). The input
// is the JSON text. Any input either parses into a JobSpec with a non-empty distorted path and a normalisation
// object, or fails with OVT_E_INVALID_ARG / OVT_E_UNSUPPORTED; anything else is a finding.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "metrics/jobspec_json.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the fuzzer hands out raw bytes
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto r = ovt::metrics::parse_job_spec(text);
  if (!r) {
    if (r.status() != OVT_E_INVALID_ARG && r.status() != OVT_E_UNSUPPORTED) {
      std::abort();
    }
    return 0;
  }
  const auto& spec = r.value();
  if (spec.dist_path.empty() || spec.normalization_json.empty() || spec.subsample < 1 || spec.threads < 0) {
    std::abort();
  }
  if (spec.frame_begin && spec.frame_end && *spec.frame_end < *spec.frame_begin) {
    std::abort();
  }
  return 0;
}
