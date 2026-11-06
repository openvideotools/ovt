// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.084 acceptance test 1 (labels part): label_for against the §6.3.1 tables and every enum of
// ovt_model.fbs, as a snapshot of core/tests/model/labels_golden.txt.
//
// Golden format: one `<enum>.<value>\t<label>` line per label. `<enum>` is a schema enum name ("AvcMbType")
// or, for the codec-dependent §6.3.1 tables, `<field>.<Codec>` ("intra_mode_y.H265").
// SPEC-ASSUMPTION(#98): the `<field>.<Codec>` key form. Regenerate with OVT_UPDATE_GOLDEN=1 and review the
// diff by hand.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <flatbuffers/reflection.h>
#include <catch2/catch_test_macros.hpp>

#include "ovt/cxx/model/labels.hpp"
#include "ovt/cxx/model/schema_id.hpp"
#include "test_support/golden.h"

namespace {

using ovt::fb::Codec;
using ovt::model::label_for;

constexpr std::array<Codec, 3> kCodecs = {Codec::H264, Codec::H265, Codec::AV1};
// Every §6.3.1 value is a ubyte or a short in 0..65; -1 covers the "not applicable" sentinel.
constexpr std::int32_t kFirstValue = -1;
constexpr std::int32_t kLastValue = 255;

std::string snapshot() {
  std::string out;
  for (const std::string_view field : ovt::model::label_fields()) {
    for (const Codec codec : kCodecs) {
      for (std::int32_t v = kFirstValue; v <= kLastValue; ++v) {
        const std::string_view label = label_for(field, codec, v);
        if (!label.empty()) {
          out += std::string(field) + "." + ovt::fb::EnumNameCodec(codec) + "." + std::to_string(v) + "\t" +
                 std::string(label) + "\n";
        }
      }
    }
  }
  const auto bytes = ovt::model::bfbs(ovt::model::SchemaId::Model);
  REQUIRE_FALSE(bytes.empty());
  const reflection::Schema* schema = reflection::GetSchema(bytes.data());
  constexpr std::string_view kPrefix = "ovt.fb.";
  for (const reflection::Enum* e : *schema->enums()) {
    std::string_view name = e->name()->string_view();
    REQUIRE(name.starts_with(kPrefix));
    name.remove_prefix(kPrefix.size());
    for (const reflection::EnumVal* ev : *e->values()) {
      const auto value = static_cast<std::int32_t>(ev->value());
      const std::string_view label = label_for(name, Codec::Unknown, value);
      INFO(std::string(name) << "." << value);
      CHECK(label == ev->name()->string_view());
      out += std::string(name) + "." + std::to_string(value) + "\t" + std::string(label) + "\n";
    }
  }
  return out;
}

}  // namespace

TEST_CASE("test_labels: §6.3.1 examples", "[fast][model]") {
  CHECK(label_for("intra_mode_y", Codec::H265, 26) == "INTRA_ANGULAR26");
  CHECK(label_for("intra_mode_y", Codec::H265, 0) == "INTRA_PLANAR");
  CHECK(label_for("intra_mode_y", Codec::H265, 1) == "INTRA_DC");
  CHECK(label_for("intra_mode_y", Codec::H265, 34) == "INTRA_ANGULAR34");
  CHECK(label_for("intra_mode_y", Codec::H265, 64) == "PCM");
  CHECK(label_for("intra_mode_y", Codec::H265, 35).empty());
  CHECK(label_for("intra_mode_y", Codec::H264, 2) == "DC");
  CHECK(label_for("intra_mode_y", Codec::H264, 18) == "I16_DC");
  CHECK(label_for("intra_mode_y", Codec::H264, 32) == "I_PCM");
  CHECK(label_for("intra_mode_y", Codec::H264, 9).empty());
  CHECK(label_for("intra_mode_y", Codec::AV1, 12) == "PAETH_PRED");
  CHECK(label_for("intra_mode_y", Codec::AV1, 13).empty());  // an inter YMode is not an intra mode
  CHECK(label_for("intra_mode_y", Codec::AV1, 32) == "INTRABC");
  CHECK(label_for("intra_mode_c", Codec::AV1, 13) == "UV_CFL_PRED");
  CHECK(label_for("intra_mode_c", Codec::H264, 3) == "Chroma_Plane");
  CHECK(label_for("tx_type", Codec::H265, 1) == "DST_VII");
  CHECK(label_for("tx_type", Codec::AV1, 16) == "WHT_WHT");
  CHECK(label_for("tx_type", Codec::AV1, 255).empty());
  CHECK(label_for("filter_idx", Codec::H265, 2) == "SAO_EDGE");
  CHECK(label_for("filter_idx", Codec::AV1, 0xFE) == "cdef_idx=-1");
  CHECK(label_for("pred", Codec::AV1, 4) == "IntraBC");
  CHECK(label_for("mb_type", Codec::H264, 54) == "P_Skip");
  CHECK(label_for("mb_type", Codec::H265, 54).empty());
  CHECK(label_for("part_mode", Codec::H265, 3) == "PART_NxN");
  CHECK(label_for("partition", Codec::AV1, 8) == "PARTITION_HORZ_4");
  // Sentinels, unknown codecs and unknown fields have no label.
  CHECK(label_for("intra_mode_y", Codec::H265, -1).empty());
  CHECK(label_for("intra_mode_y", Codec::VP9, 1).empty());
  CHECK(label_for("no_such_field", Codec::H265, 1).empty());
  CHECK(label_for("", Codec::H265, 1).empty());
}

TEST_CASE("test_labels: enum names come from the schema", "[fast][model]") {
  CHECK(label_for("AvcMbType", Codec::Unknown, 26) == "P_L0_16x16");
  CHECK(label_for("AvcMbType", Codec::Unknown, 57).empty());  // gap before AvcMbType_Unknown
  CHECK(label_for("AvcMbType", Codec::Unknown, 255) == "AvcMbType_Unknown");
  CHECK(label_for("HevcPredMode", Codec::H265, 1) == "MODE_INTRA");
  CHECK(label_for("Av1YMode", Codec::AV1, 13) == "NEARESTMV");
  CHECK(label_for("Av1RefFrame", Codec::AV1, -1) == "NONE");
  CHECK(label_for("Codec", Codec::Unknown, 3) == "AV1");
  // Labels are the generated enum names.
  CHECK(label_for("HevcPartMode", Codec::Unknown, 4) ==
        std::string_view(ovt::fb::EnumNameHevcPartMode(ovt::fb::HevcPartMode::PART_2NxnU)));
  CHECK(label_for("avcmbtype", Codec::Unknown, 26).empty());  // names are case-sensitive
}

TEST_CASE("test_labels: every label equals labels_golden.txt", "[fast][model]") {
  const std::string actual = snapshot();
  CHECK(ovt::test::compare_or_update(OVT_MODEL_LABELS_GOLDEN, actual));
}
