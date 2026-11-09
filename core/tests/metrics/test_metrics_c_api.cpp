// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300: ovt/metrics.h through libovt_core, the way a consumer links it. Acceptance test 3: with the example
// plugin on $OVT_METRIC_PATH, ovt_metrics_list_descriptors() contains id `example`. The environment variable is
// set before the first API call of this process, which is when the registry singleton scans.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "ovt/metrics.h"
#include "ovt/status.h"

namespace {

void point_registry_at_example_plugin() {
  static const bool done = [] {
    const std::string dir = std::string(OVT_EXAMPLE_METRIC_ROOT) + "/example";
    // NOLINTNEXTLINE(concurrency-mt-unsafe): before any thread exists
    setenv("OVT_METRIC_PATH", dir.c_str(), 1);
    // Keep the platform defaults and any $OVT_PLUGIN_PATH of the machine out of the test.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    setenv("OVT_PLUGIN_PATH", "/nonexistent/ovt/plugins", 1);
    return true;
  }();
  (void)done;
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): Catch2 assertions count as branches
TEST_CASE("ovt_metrics_list_descriptors contains the example plugin", "[metrics][c_api][fast]") {
  // Acceptance test 3.
  point_registry_at_example_plugin();
  ovt_Buffer buf{};
  REQUIRE(ovt_metrics_list_descriptors(&buf) == OVT_OK);
  REQUIRE(buf.data != nullptr);
  REQUIRE(buf.size > 0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the documented terminator
  CHECK(buf.data[buf.size] == 0);
  const std::string text(reinterpret_cast<const char*>(buf.data), buf.size);  // NOLINT
  const auto doc = nlohmann::json::parse(text);
  REQUIRE(doc.is_array());
  bool found = false;
  for (const auto& d : doc) {
    if (d.at("id") == "example") {
      found = true;
      CHECK(d.at("kind") == "FullReference");
      CHECK(d.at("outputs") == nlohmann::json::array({"score"}));
    }
  }
  CHECK(found);
  ovt_buffer_free(&buf);
  CHECK(buf.data == nullptr);
  CHECK(buf.size == 0);
  CHECK(ovt_metrics_list_descriptors(nullptr) == OVT_E_INVALID_ARG);
}
