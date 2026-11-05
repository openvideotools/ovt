// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.083 acceptance test 2: for each SchemaId the embedded .bfbs verifies as a reflection schema, its root
// table equals root_table_name(), and schema_major() is 1. Also: the embedded bytes are 8-byte aligned and
// kModelSchemaSha256 has the shape of a SHA-256 (its value is checked by tools/ci/test_regen.sh, which
// compares it with tools/schema_hash.py).

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/util.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/model/schema_id.hpp"
#include "ovt/cxx/model/schema_version.hpp"

namespace {

using ovt::model::SchemaId;

struct Expected {
  SchemaId id;
  std::string_view root;
  std::uint32_t major;
};

void check_schema(const Expected& e) {
  const auto bytes = ovt::model::bfbs(e.id);
  REQUIRE_FALSE(bytes.empty());
  CHECK(reinterpret_cast<std::uintptr_t>(bytes.data()) % 8 == 0);  // alignas(8), FlatBuffers' largest scalar
  CHECK(reflection::SchemaBufferHasIdentifier(bytes.data()));      // "BFBS"
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  REQUIRE(reflection::VerifySchemaBuffer(verifier));

  const reflection::Schema* schema = reflection::GetSchema(bytes.data());
  REQUIRE(schema->root_table() != nullptr);
  CHECK(schema->root_table()->name()->string_view() == e.root);
  CHECK(ovt::model::root_table_name(e.id) == e.root);
  CHECK(ovt::model::schema_major(e.id) == e.major);
}

}  // namespace

TEST_CASE("test_bfbs_resources: Model and Container .bfbs root tables, names and majors", "[model][fast]") {
  const Expected e = GENERATE(Expected{SchemaId::Model, "ovt.fb.FrameAnalysis", 1},
                              Expected{SchemaId::Container, "ovt.fb.ContainerRoot", 1});
  CAPTURE(e.root);
  check_schema(e);
}

TEST_CASE("test_bfbs_resources: --bfbs-comments keeps the /// documentation", "[model][fast]") {
  // ovt_model.fbs has no /// comments; ovt_container.fbs documents every table, e.g. ContainerRoot.
  const auto bytes = ovt::model::bfbs(SchemaId::Container);
  REQUIRE_FALSE(bytes.empty());
  const reflection::Schema* schema = reflection::GetSchema(bytes.data());
  const reflection::Object* root = schema->root_table();
  REQUIRE(root != nullptr);
  REQUIRE(root->documentation() != nullptr);
  CHECK(root->documentation()->size() > 0);
}

TEST_CASE("test_bfbs_resources: Metrics .bfbs root table, name and major", "[model][fast]") {
  CHECK(ovt::model::root_table_name(SchemaId::Metrics) == "ovt.fb.MetricsReport");
  CHECK(ovt::model::schema_major(SchemaId::Metrics) == 1U);
#ifdef OVT_EXPECT_METRICS_BFBS
  check_schema({SchemaId::Metrics, "ovt.fb.MetricsReport", 1});
#else
  CHECK(ovt::model::bfbs(SchemaId::Metrics).empty());
  SKIP("core/schema/ovt_metrics.fbs is not in the tree yet (WI-0.082); the .bfbs check runs once it is");
#endif
}

TEST_CASE("test_bfbs_resources: schema majors are the compile-time constants", "[model][fast]") {
  STATIC_CHECK(ovt::model::kModelSchemaMajor == 1U);
  STATIC_CHECK(ovt::model::kContainerSchemaMajor == 1U);
  STATIC_CHECK(ovt::model::kMetricsSchemaMajor == 1U);
  // An id outside the enum is not a schema.
  const auto unknown = static_cast<SchemaId>(0);
  CHECK(ovt::model::bfbs(unknown).empty());
  CHECK(ovt::model::schema_major(unknown) == 0U);
  CHECK(ovt::model::root_table_name(unknown).empty());
}

TEST_CASE("test_bfbs_resources: kModelSchemaSha256 is 64 lower-case hex digits", "[model][fast]") {
  const std::string_view hash = ovt::model::kModelSchemaSha256;
  REQUIRE(hash.size() == 64);
  for (const char c : hash) {
    CAPTURE(c);
    CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
  }
}
