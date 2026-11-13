// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300 step 4: the MetricJob state machine (chapter 9 §9.2.3) with its events, Probing validation,
// cancellation and the skeleton's OVT_E_UNSUPPORTED answer once a metric would have to be scored.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

#include "metrics/host/metric_job.h"
#include "metrics/host/plugin_registry.h"
#include "ovt/cxx/jobs.hpp"
#include "ovt/jobs.h"

namespace {

using ovt::metrics::JobEvent;
using ovt::metrics::JobSpec;
using ovt::metrics::JobState;
using ovt::metrics::MetricJob;
using ovt::metrics::PluginRegistry;

std::string plugin_dir(const char* sub) {
  return std::string(OVT_EXAMPLE_METRIC_ROOT) + "/" + sub;
}

PluginRegistry::Options only(std::string metric_path) {
  PluginRegistry::Options o;
  o.ovt_metric_path = std::move(metric_path);
  o.ovt_plugin_path = std::string();
  o.defaults = std::vector<std::string>{};
  return o;
}

std::vector<JobState> states_of(const std::vector<JobEvent>& events) {
  std::vector<JobState> out;
  out.reserve(events.size());
  for (const auto& e : events) {
    out.push_back(e.state);
  }
  return out;
}

struct Fixture {
  ovt::jobs::Pool pool{1};
  PluginRegistry registry{only(plugin_dir("example"))};
  std::vector<JobEvent> events;
  std::unique_ptr<MetricJob> make(const JobSpec& spec) {
    return MetricJob::create(spec, pool, [this](const JobEvent& e) { events.push_back(e); }, registry);
  }
};

}  // namespace

TEST_CASE("a job without metrics walks every state to Done", "[metrics][job][fast]") {
  Fixture f;
  JobSpec spec;
  spec.dist_path = "dist.mp4";
  auto job = f.make(spec);
  CHECK(job->state() == JobState::Created);
  CHECK(job->status() == OVT_OK);
  REQUIRE(job->run(nullptr) == OVT_OK);
  CHECK(job->state() == JobState::Done);
  CHECK(states_of(f.events) == std::vector<JobState>{JobState::Probing, JobState::Aligning, JobState::Scoring,
                                                     JobState::Pooling, JobState::Done});
  CHECK(job->results().empty());
  CHECK(job->normalization().rfind("{\"bit_depth\"", 0) == 0);
  CHECK(job->alignment().status == "fixed");
  CHECK(job->alignment().offset_frames == 0);
  // A job runs once.
  CHECK(job->run(nullptr) == OVT_E_INVALID_ARG);
  CHECK(job->state() == JobState::Done);
}

TEST_CASE("Probing resolves the NormalizationConfig and seeds the alignment from it", "[metrics][job][fast]") {
  Fixture f;
  JobSpec spec;
  spec.dist_path = "dist.mp4";
  spec.normalization_json = R"({"temporal":{"mode":"fixed","offset_frames":7},"spatial":{"shift_x":1}})";
  auto job = f.make(spec);
  REQUIRE(job->run(nullptr) == OVT_OK);
  CHECK(job->alignment().offset_frames == 7);
  CHECK(job->alignment().shift_x == 1);
  CHECK(job->alignment().shift_y == 0);
  const auto a = nlohmann::json::parse(ovt::metrics::alignment_json(job->alignment()));
  CHECK(a.at("offset_frames") == 7);
  CHECK(a.at("status") == "fixed");
  CHECK(ovt::metrics::alignment_json(job->alignment()).rfind("{\"confidence\":", 0) == 0);  // sorted keys
}

TEST_CASE("Probing fails on an invalid NormalizationConfig", "[metrics][job][fast]") {
  Fixture f;
  JobSpec spec;
  spec.dist_path = "dist.mp4";
  spec.normalization_json = R"({"scale":{"kernel":"nearest"}})";
  auto job = f.make(spec);
  CHECK(job->run(nullptr) == OVT_E_INVALID_ARG);
  CHECK(job->state() == JobState::Failed);
  CHECK(job->status() == OVT_E_INVALID_ARG);
  REQUIRE(f.events.size() == 2);
  CHECK(f.events.back().state == JobState::Failed);
  CHECK(f.events.back().message.find("scale.kernel") != std::string::npos);
}

TEST_CASE("Probing rejects unknown metric ids and full-reference metrics without a reference", "[metrics][job][fast]") {
  Fixture f;
  JobSpec spec;
  spec.dist_path = "dist.mp4";
  SECTION("unknown id (VQ-JOB-0002)") {
    spec.metrics.push_back({"nosuch", "{}"});
    auto job = f.make(spec);
    CHECK(job->run(nullptr) == OVT_E_INVALID_ARG);
    CHECK(job->state() == JobState::Failed);
    CHECK(f.events.back().message.find("VQ-JOB-0002") != std::string::npos);
    CHECK(f.events.back().message.find("available: example") != std::string::npos);
  }
  SECTION("full-reference without reference (VQ-JOB-0003)") {
    spec.metrics.push_back({"example", "{}"});
    auto job = f.make(spec);
    CHECK(job->run(nullptr) == OVT_E_INVALID_ARG);
    CHECK(f.events.back().message.find("VQ-JOB-0003") != std::string::npos);
  }
  SECTION("options must be a JSON object") {
    spec.ref_path = "ref.y4m";
    spec.metrics.push_back({"example", "[1]"});
    auto job = f.make(spec);
    CHECK(job->run(nullptr) == OVT_E_INVALID_ARG);
    CHECK(f.events.back().message.find("options") != std::string::npos);
  }
  SECTION("subsample below 1") {
    spec.subsample = 0;
    auto job = f.make(spec);
    CHECK(job->run(nullptr) == OVT_E_INVALID_ARG);
  }
}

TEST_CASE("the skeleton stops with OVT_E_UNSUPPORTED once a metric would be scored", "[metrics][job][fast]") {
  Fixture f;
  JobSpec spec;
  spec.ref_path = "ref.y4m";
  spec.dist_path = "dist.mp4";
  spec.metrics.push_back({"example", "{}"});
  auto job = f.make(spec);
  CHECK(job->run(nullptr) == OVT_E_UNSUPPORTED);
  CHECK(job->state() == JobState::Failed);
  CHECK(states_of(f.events) == std::vector<JobState>{JobState::Probing, JobState::Failed});
  CHECK(f.events.back().message.find("WI-3.306") != std::string::npos);
}

TEST_CASE("a cancelled ovt_Job ends the job with OVT_E_CANCELLED", "[metrics][job][fast]") {
  Fixture f;
  JobSpec spec;
  spec.dist_path = "dist.mp4";
  ovt_Job* cancel = nullptr;
  REQUIRE(ovt_job_create(&cancel) == OVT_OK);
  ovt_job_cancel(cancel);
  auto job = f.make(spec);
  CHECK(job->run(cancel) == OVT_E_CANCELLED);
  CHECK(job->state() == JobState::Cancelled);
  CHECK(states_of(f.events) == std::vector<JobState>{JobState::Cancelled});
  ovt_job_free(cancel);
}

TEST_CASE("JobEvent JSON has sorted keys and the state name", "[metrics][job][fast]") {
  JobEvent e;
  e.state = JobState::Scoring;
  e.frames_done = 3;
  e.frames_total = 10;
  e.eta_seconds = 1.5;
  e.message = "x";
  CHECK(ovt::metrics::job_event_json(e) ==
        R"({"eta_seconds":1.5,"frames_done":3,"frames_total":10,"message":"x","state":"Scoring"})");
  CHECK(ovt::metrics::job_state_name(JobState::Created) == "Created");
  CHECK(ovt::metrics::job_state_name(JobState::Cancelled) == "Cancelled");
}
