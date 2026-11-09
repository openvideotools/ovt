// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300: the strict MetricsJobSpec parser behind ovt_metrics_job_create (chapter 12 §12.3.13, FR-API-010).

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <string>

#include "metrics/jobspec_json.h"

using ovt::metrics::parse_job_spec;

TEST_CASE("JobSpec: the full form parses", "[metrics][jobspec][fast]") {
  const auto r = parse_job_spec(
      R"({"reference":"ref.y4m","distorted":["dist.mp4"],"metrics":[{"id":"vmaf","options":{"model":"version=vmaf_v0.6.1"}},"psnr"],)"
      R"("normalize":{"scale":{"kernel":"lanczos"}},"align":"auto","maps":true,"subsample":2,"max_frames":0,"frames":"0-299","threads":4})");
  REQUIRE(r.has_value());
  const auto& s = r.value();
  REQUIRE(s.ref_path.has_value());
  CHECK(*s.ref_path == "ref.y4m");
  CHECK(s.dist_path == "dist.mp4");
  REQUIRE(s.metrics.size() == 2);
  CHECK(s.metrics[0].id == "vmaf");
  CHECK(nlohmann::json::parse(s.metrics[0].options_json).at("model") == "version=vmaf_v0.6.1");
  CHECK(s.metrics[1].id == "psnr");
  CHECK(s.metrics[1].options_json == "{}");
  CHECK(nlohmann::json::parse(s.normalization_json).at("scale").at("kernel") == "lanczos");
  CHECK(s.subsample == 2);
  CHECK(s.threads == 4);
  CHECK(s.frame_begin == 0);
  CHECK(s.frame_end == 300);  // "0-299" inclusive → [0, 300)
}

TEST_CASE("JobSpec: align writes the fixed offset into the normalisation (§9.3.1 step 8)", "[metrics][jobspec][fast]") {
  SECTION("integer") {
    const auto r = parse_job_spec(R"({"distorted":"d.mp4","align":-3})");
    REQUIRE(r.has_value());
    const auto n = nlohmann::json::parse(r.value().normalization_json);
    CHECK(n.at("temporal").at("mode") == "fixed");
    CHECK(n.at("temporal").at("offset_frames") == -3);
  }
  SECTION("none") {
    const auto r = parse_job_spec(R"({"distorted":"d.mp4","align":"none","normalize":{"range":"full"}})");
    REQUIRE(r.has_value());
    const auto n = nlohmann::json::parse(r.value().normalization_json);
    CHECK(n.at("temporal").at("mode") == "fixed");
    CHECK(n.at("temporal").at("offset_frames") == 0);
    CHECK(n.at("range") == "full");
  }
  SECTION("auto leaves the config alone") {
    const auto r = parse_job_spec(R"({"distorted":"d.mp4","align":"auto"})");
    REQUIRE(r.has_value());
    CHECK(r.value().normalization_json == "{}");
  }
}

TEST_CASE("JobSpec: max_frames and frames combine into [begin, end)", "[metrics][jobspec][fast]") {
  const auto r = parse_job_spec(R"({"distorted":"d.mp4","frames":"10-99","max_frames":20})");
  REQUIRE(r.has_value());
  CHECK(r.value().frame_begin == 10);
  CHECK(r.value().frame_end == 30);
  const auto only_max = parse_job_spec(R"({"distorted":"d.mp4","max_frames":5})");
  REQUIRE(only_max.has_value());
  CHECK(only_max.value().frame_begin == 0);
  CHECK(only_max.value().frame_end == 5);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): Catch2 assertions count as branches
TEST_CASE("JobSpec: errors name the field", "[metrics][jobspec][fast]") {
  struct Case {
    const char* json;
    const char* field;
    ovt_status status;
  };
  const std::array<Case, 16> cases = {{
      {R"({"distorted":"d.mp4","bogus":1})", "bogus", OVT_E_INVALID_ARG},
      {R"({"reference":"r.y4m"})", "distorted", OVT_E_INVALID_ARG},
      {R"({"distorted":["a.mp4","b.mp4"]})", "distorted", OVT_E_UNSUPPORTED},
      {R"({"distorted":"d.mp4","metrics":"psnr"})", "metrics", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","metrics":[{"id":"psnr","opts":{}}]})", "metrics[0].opts", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","metrics":[{"options":{}}]})", "metrics[0].id", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","metrics":["psnr",3]})", "metrics[1]", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","align":"later"})", "align", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","align":1.5})", "align", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","subsample":0})", "subsample", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","frames":"5-2"})", "frames", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","frames":"a-b"})", "frames", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","threads":-1})", "threads", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","maps":"yes"})", "maps", OVT_E_INVALID_ARG},
      {R"({"distorted":"d.mp4","normalize":[]})", "normalize", OVT_E_INVALID_ARG},
      {R"([])", "<root>", OVT_E_INVALID_ARG},
  }};
  for (const Case& c : cases) {
    INFO(c.json);
    const auto r = parse_job_spec(c.json);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.status() == c.status);
    CHECK(r.message().find(c.field) != std::string::npos);
  }
  const auto bad = parse_job_spec("{");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.status() == OVT_E_INVALID_ARG);
}
