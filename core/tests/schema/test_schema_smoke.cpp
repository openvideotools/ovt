// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.003 schema-level smoke test: build -> verify -> JSON -> build on the smoke Record record.
// It proves the code generation target works end to end (generated header, object API, comparison
// operators, reflection-free JSON through the flatbuffers parser). The model-level round trip is
// test_model_roundtrip of chapter 6 WI-0.093.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/util.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "smoke_generated.h"

namespace {

// flatbuffers::GenerateText returns bool up to 2.x and an error string (null on success) from 24.x on.
[[maybe_unused]] bool succeeded(bool ok) {
  return ok;
}
[[maybe_unused]] bool succeeded(const char* error) {
  return error == nullptr;
}

flatbuffers::DetachedBuffer build_record() {
  flatbuffers::FlatBufferBuilder fbb;
  const ovt::fb::smoke::Span span(100, 24);
  const std::vector<uint8_t> payload{1, 2, 3, 250};
  auto note = fbb.CreateString("smoke");
  auto data = fbb.CreateVector(payload);
  ovt::fb::smoke::RecordBuilder b(fbb);
  b.add_stream_index(7);
  b.add_display_order(-3);
  b.add_codec(ovt::fb::smoke::Codec::Av1);
  b.add_span(&span);
  b.add_note(note);
  b.add_payload(data);
  ovt::fb::smoke::FinishRecordBuffer(fbb, b.Finish());
  return fbb.Release();
}

}  // namespace

TEST_CASE("smoke Record builds, verifies and carries its identifier", "[fast][schema]") {
  const auto buf = build_record();
  flatbuffers::Verifier verifier(buf.data(), buf.size());
  REQUIRE(ovt::fb::smoke::VerifyRecordBuffer(verifier));
  REQUIRE(ovt::fb::smoke::RecordBufferHasIdentifier(buf.data()));
  REQUIRE(std::string(ovt::fb::smoke::RecordIdentifier()) == "OVTS");

  const auto* fa = ovt::fb::smoke::GetRecord(buf.data());
  CHECK(fa->schema_version() == 1);
  CHECK(fa->stream_index() == 7);
  CHECK(fa->display_order() == -3);
  CHECK(fa->codec() == ovt::fb::smoke::Codec::Av1);
  CHECK(fa->span()->offset() == 100);
  CHECK(fa->span()->size() == 24);
  CHECK(fa->note()->str() == "smoke");
  CHECK(fa->payload()->size() == 4);
}

TEST_CASE("smoke Record survives JSON and a rebuild", "[fast][schema]") {
  const auto buf = build_record();

  std::string schema_text;
  REQUIRE(flatbuffers::LoadFile(OVT_SMOKE_FBS_PATH, false, &schema_text));
  flatbuffers::Parser parser;
  std::array<const char*, 2> include_dirs{OVT_SMOKE_DIR, nullptr};
  REQUIRE(parser.Parse(schema_text.c_str(), include_dirs.data(), OVT_SMOKE_FBS_PATH));

  std::string json;
  REQUIRE(succeeded(flatbuffers::GenerateText(parser, buf.data(), &json)));
  CHECK(json.find("stream_index: 7") != std::string::npos);
  CHECK(json.find("codec: \"Av1\"") != std::string::npos);

  flatbuffers::Parser reparse;
  REQUIRE(reparse.Parse(schema_text.c_str(), include_dirs.data(), OVT_SMOKE_FBS_PATH));
  REQUIRE(reparse.Parse(json.c_str()));
  flatbuffers::Verifier verifier(reparse.builder_.GetBufferPointer(), reparse.builder_.GetSize());
  REQUIRE(ovt::fb::smoke::VerifyRecordBuffer(verifier));

  // Object-API comparison (--gen-compare): the rebuilt record equals the original, field by field.
  ovt::fb::smoke::RecordT original;
  ovt::fb::smoke::GetRecord(buf.data())->UnPackTo(&original);
  ovt::fb::smoke::RecordT rebuilt;
  ovt::fb::smoke::GetRecord(reparse.builder_.GetBufferPointer())->UnPackTo(&rebuilt);
  CHECK(original == rebuilt);
}
