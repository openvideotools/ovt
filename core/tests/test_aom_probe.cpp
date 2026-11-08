// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the libaom third-party build (WI-0.013, chapter 13 §13.3.4): tp::aom links, is the pinned version
// and was built with CONFIG_INSPECTION (the AV1 analysis plugin's data source, chapter 07 §7.6.1).

#include <catch2/catch_test_macros.hpp>

#include <aom/aom_codec.h>
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

#include <string>

// aomdx.h declares the control id together with a type-check macro of the same name.
#ifndef AOM_CTRL_AV1_SET_INSPECTION_CALLBACK
#error "<aom/aomdx.h> does not declare AV1_SET_INSPECTION_CALLBACK"
#endif

#ifndef OVT_AOM_PINNED_VERSION
#error "OVT_AOM_PINNED_VERSION (the libaom row of third_party/VERSIONS.md) must be defined by the build"
#endif

namespace {

void on_frame(void* /*decoder*/, void* /*ctx*/) {}

}  // namespace

TEST_CASE("aom_codec_version_str matches the VERSIONS.md pin", "[fast][third_party]") {
  const char* version = aom_codec_version_str();
  REQUIRE(version != nullptr);
  std::string reported{version};
  // libaom reports the release as "3.12.1" from its CHANGELOG, or "v3.12.1" from `git describe`.
  if (!reported.empty() && reported.front() == 'v') {
    reported.erase(0, 1);
  }
  CHECK(reported == OVT_AOM_PINNED_VERSION);
}

TEST_CASE("libaom accepts AV1_SET_INSPECTION_CALLBACK", "[fast][third_party]") {
  aom_codec_ctx_t codec{};
  aom_codec_dec_cfg_t cfg{};
  cfg.allow_lowbitdepth = 1;
  REQUIRE(aom_codec_dec_init(&codec, aom_codec_av1_dx(), &cfg, 0) == AOM_CODEC_OK);
  aom_inspect_init init{};
  init.inspect_cb = on_frame;
  init.inspect_ctx = nullptr;
  // AOM_CODEC_INCAPABLE here means the library was built without CONFIG_INSPECTION (chapter 07 §7.6.1).
  const aom_codec_err_t err = aom_codec_control(&codec, AV1_SET_INSPECTION_CALLBACK, &init);
  CHECK(err == AOM_CODEC_OK);
  CHECK(aom_codec_destroy(&codec) == AOM_CODEC_OK);
}
