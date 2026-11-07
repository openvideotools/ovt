// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the libvmaf third-party build (WI-0.015, chapter 13 §13.3): tp::vmaf links and the built-in model loads.

#include <catch2/catch_test_macros.hpp>

#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>

#include <cstring>

TEST_CASE("vmaf_version reports the linked library", "[fast][third_party]") {
  const char* version = vmaf_version();
  REQUIRE(version != nullptr);
  CHECK(std::strlen(version) > 0);
}

TEST_CASE("libvmaf loads the built-in vmaf_v0.6.1 model", "[fast][third_party]") {
  VmafModel* model = nullptr;
  VmafModelConfig config{};
  config.name = "vmaf";
  config.flags = VMAF_MODEL_FLAGS_DEFAULT;
  REQUIRE(vmaf_model_load(&model, &config, "vmaf_v0.6.1") == 0);
  REQUIRE(model != nullptr);
  vmaf_model_destroy(model);
}
