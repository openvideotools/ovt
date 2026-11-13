// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the NormalizationConfig JSON parser and schema validator (chapter 9 §9.2.4, chapter 14 §14.7.1,
// WI-3.300). The input is the JSON text. Any input either resolves to a canonical string that resolves again to
// the same string (the resolved form is a fixed point), or fails with OVT_E_INVALID_ARG; anything else, a crash
// or a sanitizer report is a finding.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "metrics/normalize/normalization_config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the fuzzer hands out raw bytes
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto r = ovt::metrics::resolve_normalization_config_json(text);
  if (!r) {
    if (r.status() != OVT_E_INVALID_ARG) {
      std::abort();
    }
    return 0;
  }
  const auto again = ovt::metrics::resolve_normalization_config_json(r.value());
  if (!again || again.value() != r.value()) {
    std::abort();
  }
  return 0;
}
