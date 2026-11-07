// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Canary fuzz target (chapter 13 §13.7.1, WI-0.033): proves that the fuzz CI lanes fail and upload `crash-*`
// artefacts when a target crashes. It is inert unless the environment variable OVT_FUZZ_CANARY is set; with it,
// an input that starts with the four bytes "OVT!" aborts. The committed seed corpus/canary/seed is such an input,
// so a run with the variable set crashes at once. The smoke workflow skips this target unless it is dispatched
// with the `canary` input, and the nightly lane never runs it.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (std::getenv("OVT_FUZZ_CANARY") != nullptr && size >= 4 && std::memcmp(data, "OVT!", 4) == 0) {
    std::abort();
  }
  return 0;
}
