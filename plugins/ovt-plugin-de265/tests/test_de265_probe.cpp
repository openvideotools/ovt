// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Probe of the patched libde265 build (WI-0.014, chapter 13 §13.3.6): tp::de265 links and runs.

#include <catch2/catch_test_macros.hpp>

#include <libde265/de265.h>

#include <string_view>

TEST_CASE("de265_get_version reports the linked library", "[fast][third_party]") {
  const char* version = de265_get_version();
  REQUIRE(version != nullptr);
  CHECK(!std::string_view(version).empty());
#ifdef OVT_DE265_PINNED_VERSION
  CHECK(std::string_view(version) == OVT_DE265_PINNED_VERSION);
#endif
}

TEST_CASE("libde265 creates and frees a decoder context", "[fast][third_party]") {
  de265_decoder_context* context = de265_new_decoder();
  REQUIRE(context != nullptr);
  CHECK(de265_free_decoder(context) == DE265_OK);
}
