// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300: ovt/metrics.h through libovt_core, the way a consumer links it. Acceptance test 3: with the example
// plugin on $OVT_METRIC_PATH, ovt_metrics_list_descriptors() contains id `example`. The environment variable is
// set before the first API call of this process, which is when the registry singleton scans.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ovt/jobs.h"
#include "ovt/metrics.h"
#include "ovt/status.h"
#include "ovt_metrics_generated.h"

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

struct Events {
  std::vector<std::string> states;
  static void on_event(const char* event_json, void* user) {
    auto* self = static_cast<Events*>(user);
    self->states.push_back(nlohmann::json::parse(event_json).at("state").get<std::string>());
  }
};

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

TEST_CASE("ovt_metrics_job_* runs the skeleton job and returns an OVTR report", "[metrics][c_api][fast]") {
  point_registry_at_example_plugin();
  Events events;
  ovt_MetricsJob* job = nullptr;
  REQUIRE(ovt_metrics_job_create(R"({"reference":"/tmp/ref.y4m","distorted":["/tmp/dist.mp4"],"metrics":[]})",
                                 &Events::on_event, &events, &job) == OVT_OK);
  REQUIRE(job != nullptr);
  ovt_Buffer report{};
  CHECK(ovt_metrics_job_results(job, &report) == OVT_E_INVALID_ARG);  // not Done yet
  CHECK(ovt_metrics_job_run(job, nullptr) == OVT_OK);
  CHECK(events.states == std::vector<std::string>{"Probing", "Aligning", "Scoring", "Pooling", "Done"});
  REQUIRE(ovt_metrics_job_results(job, &report) == OVT_OK);
  REQUIRE(report.data != nullptr);
  CHECK(flatbuffers::BufferHasIdentifier(report.data, ovt::fb::MetricsReportIdentifier(), /*size_prefixed=*/true));
  flatbuffers::Verifier verifier(report.data, report.size);
  CHECK(ovt::fb::VerifySizePrefixedMetricsReportBuffer(verifier));
  const auto* r = ovt::fb::GetSizePrefixedMetricsReport(report.data);
  CHECK(r->schema_version() == 1);
  REQUIRE(r->results() != nullptr);
  CHECK(r->results()->empty());
  CHECK(r->reference_name()->str() == "ref.y4m");
  CHECK(r->distorted_name()->str() == "dist.mp4");
  CHECK(r->ovt_version()->str().rfind("ovt ", 0) == 0);
  ovt_buffer_free(&report);

  ovt_Buffer alignment{};
  REQUIRE(ovt_metrics_job_alignment(job, &alignment) == OVT_OK);
  const auto a = nlohmann::json::parse(reinterpret_cast<const char*>(alignment.data));  // NOLINT
  CHECK(a.at("offset_frames") == 0);
  CHECK(a.at("status") == "fixed");
  ovt_buffer_free(&alignment);
  CHECK(ovt_metrics_job_run(job, nullptr) == OVT_E_INVALID_ARG);  // runs once
  ovt_metrics_job_release(job);
  ovt_metrics_job_release(nullptr);
}

TEST_CASE("ovt_metrics_job_create rejects bad specs and run reports probing failures", "[metrics][c_api][fast]") {
  point_registry_at_example_plugin();
  ovt_MetricsJob* job = nullptr;
  CHECK(ovt_metrics_job_create(nullptr, nullptr, nullptr, &job) == OVT_E_INVALID_ARG);
  CHECK(ovt_metrics_job_create("{}", nullptr, nullptr, nullptr) == OVT_E_INVALID_ARG);
  CHECK(ovt_metrics_job_create(R"({"distorted":"d.mp4","nope":1})", nullptr, nullptr, &job) == OVT_E_INVALID_ARG);
  CHECK(job == nullptr);
  CHECK(ovt_metrics_job_create(R"({"distorted":["a","b"]})", nullptr, nullptr, &job) == OVT_E_UNSUPPORTED);

  SECTION("a metric that would be scored is OVT_E_UNSUPPORTED in the skeleton") {
    REQUIRE(ovt_metrics_job_create(R"({"reference":"r","distorted":"d","metrics":["example"]})", nullptr, nullptr,
                                   &job) == OVT_OK);
    CHECK(ovt_metrics_job_run(job, nullptr) == OVT_E_UNSUPPORTED);
    ovt_metrics_job_release(job);
  }
  SECTION("an unknown metric id fails in Probing") {
    REQUIRE(ovt_metrics_job_create(R"({"distorted":"d","metrics":["nosuch"]})", nullptr, nullptr, &job) == OVT_OK);
    CHECK(ovt_metrics_job_run(job, nullptr) == OVT_E_INVALID_ARG);
    ovt_metrics_job_release(job);
  }
  SECTION("a bad NormalizationConfig fails in Probing") {
    REQUIRE(ovt_metrics_job_create(R"({"distorted":"d","normalize":{"scale":{"kernel":"nearest"}}})", nullptr, nullptr,
                                   &job) == OVT_OK);
    Events events;
    CHECK(ovt_metrics_job_run(job, nullptr) == OVT_E_INVALID_ARG);
    ovt_metrics_job_release(job);
  }
  SECTION("a cancelled job") {
    ovt_Job* cancel = nullptr;
    REQUIRE(ovt_job_create(&cancel) == OVT_OK);
    ovt_job_cancel(cancel);
    REQUIRE(ovt_metrics_job_create(R"({"distorted":"d"})", nullptr, nullptr, &job) == OVT_OK);
    CHECK(ovt_metrics_job_run(job, cancel) == OVT_E_CANCELLED);
    ovt_metrics_job_release(job);
    ovt_job_free(cancel);
  }
}
