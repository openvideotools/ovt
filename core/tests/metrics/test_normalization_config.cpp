// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-3.300 step 5: the NormalizationConfig resolver (chapter 9 §9.2.4). Acceptance test 1 (defaults equal
// golden/norm_defaults.json byte for byte), acceptance test 2 (a bad enum value names scale.kernel), unknown
// keys, the canonical form (sorted keys, no whitespace), determinism and the embedded schema.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

#include "metrics/normalize/normalization_config.h"
#include "test_support/golden.h"

using ovt::metrics::resolve_normalization_config;
using ovt::metrics::resolve_normalization_config_json;

namespace {
const std::filesystem::path kGoldenDir = OVT_METRICS_GOLDEN_DIR;
}  // namespace

TEST_CASE("NormalizationConfig {} resolves to the golden defaults byte for byte", "[metrics][normalize][fast]") {
  // Acceptance test 1. The golden holds the canonical string followed by one LF (end-of-file rule of the tree).
  const auto r = resolve_normalization_config_json("{}");
  REQUIRE(r.has_value());
  CHECK(ovt::test::compare_or_update(kGoldenDir / "norm_defaults.json", r.value() + "\n"));
  // "" means defaults too, and the result is byte-identical on a second run (§0.7.5).
  const auto again = resolve_normalization_config_json("");
  REQUIRE(again.has_value());
  CHECK(again.value() == r.value());
}

TEST_CASE("NormalizationConfig defaults have the schema's values", "[metrics][normalize][fast]") {
  const auto r = resolve_normalization_config("{}");
  REQUIRE(r.has_value());
  const nlohmann::json& v = r.value();
  CHECK(v.at("version") == 1);
  CHECK(v.at("scale").at("kernel") == "bicubic");
  CHECK(v.at("scale").at("lanczos_taps") == 3);
  CHECK(v.at("bit_depth").at("rule") == "promote_lower");
  CHECK(v.at("bit_depth").at("fixed_depth") == 10);
  CHECK(v.at("range") == "reference");
  CHECK(v.at("colour").at("dither") == "error_diffusion");
  CHECK(v.at("colour").at("assume_unknown_as").at("full_range") == 0);
  CHECK_FALSE(v.at("colour").contains("primaries"));  // no default: absent unless given
  CHECK(v.at("crop").at("ref").is_null());            // nullable rect: null when not given
  CHECK(v.at("display_model").at("preset") == "auto");
  CHECK_FALSE(v.at("display_model").contains("peak_nits"));
  CHECK(v.at("frame_rate").at("policy") == "match_timestamps");
  CHECK(v.at("frame_rate").at("epsilon_seconds") == 0.0005);
  CHECK(v.at("temporal").at("mode") == "auto");
  CHECK(v.at("temporal").at("offset_frames") == 0);
  CHECK(v.at("temporal").at("search_frames") == 30);
  CHECK(v.at("spatial").at("mode") == "none");
  CHECK(v.at("spatial").at("max_shift_px") == 2);
}

TEST_CASE("NormalizationConfig keeps the caller's values and fills the rest", "[metrics][normalize][fast]") {
  const auto r = resolve_normalization_config(
      R"({"scale":{"kernel":"lanczos","lanczos_taps":4},"crop":{"ref":{"x":0,"y":140,"w":1920,"h":800}},)"
      R"("colour":{"target":"explicit","primaries":9,"transfer":16,"matrix":9},"temporal":{"offset_frames":7}})");
  REQUIRE(r.has_value());
  const nlohmann::json& v = r.value();
  CHECK(v.at("scale").at("kernel") == "lanczos");
  CHECK(v.at("scale").at("lanczos_taps") == 4);
  CHECK(v.at("scale").at("bicubic_b") == 0.3333333333);
  CHECK(v.at("crop").at("ref").at("y") == 140);
  CHECK(v.at("crop").at("dist").is_null());
  CHECK(v.at("colour").at("primaries") == 9);
  CHECK(v.at("colour").at("dither") == "error_diffusion");
  CHECK(v.at("temporal").at("offset_frames") == 7);
  CHECK(v.at("temporal").at("mode") == "auto");
}

TEST_CASE("NormalizationConfig canonical form has sorted keys and no whitespace", "[metrics][normalize][fast]") {
  const auto r = resolve_normalization_config_json(R"({ "temporal": {"offset_frames": 3}, "range": "full" })");
  REQUIRE(r.has_value());
  const std::string& s = r.value();
  CHECK(s.find(' ') == std::string::npos);
  CHECK(s.find('\n') == std::string::npos);
  CHECK(s.rfind("{\"bit_depth\":{", 0) == 0);  // "bit_depth" sorts first
  CHECK(s.find("\"range\":\"full\"") != std::string::npos);
  CHECK(s.find("\"offset_frames\":3") != std::string::npos);
  CHECK(s.find("\"epsilon_seconds\":0.0005") != std::string::npos);  // shortest round-trip form
  CHECK(s.find("\"bicubic_b\":0.3333333333") != std::string::npos);
}

TEST_CASE("NormalizationConfig rejects a bad enum value naming scale.kernel", "[metrics][normalize][fast]") {
  // Acceptance test 2.
  const auto r = resolve_normalization_config_json(R"({"scale":{"kernel":"nearest"}})");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.status() == OVT_E_INVALID_ARG);
  CHECK(r.message().find("scale.kernel") != std::string::npos);
}

TEST_CASE("NormalizationConfig rejects unknown keys, wrong types and out-of-range values",
          "[metrics][normalize][fast]") {
  SECTION("unknown top-level key") {
    const auto r = resolve_normalization_config_json(R"({"scael":{}})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.status() == OVT_E_INVALID_ARG);
    CHECK(r.message().find("scael") != std::string::npos);
  }
  SECTION("unknown nested key") {
    const auto r = resolve_normalization_config_json(R"({"temporal":{"offset":3}})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.message().find("temporal") != std::string::npos);
    CHECK(r.message().find("offset") != std::string::npos);
  }
  SECTION("wrong type") {
    const auto r = resolve_normalization_config_json(R"({"temporal":{"offset_frames":"3"}})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.message().find("temporal.offset_frames") != std::string::npos);
  }
  SECTION("out of range") {
    const auto r = resolve_normalization_config_json(R"({"spatial":{"max_shift_px":9}})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.message().find("spatial.max_shift_px") != std::string::npos);
  }
  SECTION("wrong version") {
    const auto r = resolve_normalization_config_json(R"({"version":2})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.message().find("version") != std::string::npos);
  }
  SECTION("rect missing a field") {
    const auto r = resolve_normalization_config_json(R"({"crop":{"ref":{"x":0,"y":0,"w":16}}})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.message().find("crop.ref") != std::string::npos);
  }
  SECTION("not an object") {
    const auto r = resolve_normalization_config_json("[]");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.status() == OVT_E_INVALID_ARG);
  }
  SECTION("not JSON") {
    const auto r = resolve_normalization_config_json("{");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.status() == OVT_E_INVALID_ARG);
  }
}

TEST_CASE("The embedded schema is the file in core/schema", "[metrics][normalize][fast]") {
  const auto schema = nlohmann::json::parse(ovt::metrics::normalization_config_schema_text());
  CHECK(schema.at("$id") == "https://ovt.dev/schema/normalization_config.json");
  CHECK(schema.at("additionalProperties") == false);
  CHECK(schema.at("properties").size() == 11);
}
