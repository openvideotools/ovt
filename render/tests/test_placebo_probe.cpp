// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Probe of the libplacebo third-party build (WI-0.015, chapter 13 §13.3): tp::placebo links and a log context works.

#include <catch2/catch_test_macros.hpp>

#include <libplacebo/config.h>
#include <libplacebo/log.h>

TEST_CASE("libplacebo creates and destroys a log context", "[fast][third_party]") {
  struct pl_log_params params = {};
  params.log_level = PL_LOG_NONE;
  pl_log log = pl_log_create(PL_API_VER, &params);
  REQUIRE(log != nullptr);
  pl_log_destroy(&log);
  CHECK(log == nullptr);
  CHECK(PL_API_VER > 0);
}
