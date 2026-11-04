// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the dav1d third-party build (WI-0.011, chapter 13 §13.3.5): tp::dav1d links and runs.

#include <catch2/catch_test_macros.hpp>

#include <dav1d/dav1d.h>

#include <cstring>

TEST_CASE("dav1d_version reports the linked library", "[fast][third_party]") {
  const char* version = dav1d_version();
  REQUIRE(version != nullptr);
  CHECK(std::strlen(version) > 0);
  CHECK(version[0] >= '0');
  CHECK(version[0] <= '9');
}

TEST_CASE("dav1d opens and closes a decoder context", "[fast][third_party]") {
  Dav1dSettings settings;
  dav1d_default_settings(&settings);
  Dav1dContext* context = nullptr;
  REQUIRE(dav1d_open(&context, &settings) == 0);
  REQUIRE(context != nullptr);
  dav1d_close(&context);
  CHECK(context == nullptr);
}
