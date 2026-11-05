// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.081: core/schema/ovt_container.fbs compiles and matches chapter 06 §6.4.
//
// Part 1 (step 2): a ContainerRoot with one TrackInfo and a 3-sample SampleTable for the same track_id
// finishes with "OVTC", verifies and reads back unchanged.
// Part 2 (acceptance test 2): the tables and enums the compiled ovt_container.bfbs declares, minus the
// ones it pulls in from ovt_model.bfbs, equal the "Table list" paragraph of §6.4. The list below is
// literal on purpose: a table added to the schema without the spec list (or the reverse) fails here.

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/util.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ovt_container_generated.h"

namespace {

// §6.4 "Table list (for tools/check_schema_in_spec.py, WI-0.081)", in spec order. Enums are part of it.
const std::vector<std::string> kSpecTableList = {
    "Field",
    "BoxNode",
    "ContainerTree",
    "HandlerType",
    "CodecPrivate",
    "TrackReference",
    "TrackInfo",
    "GroupMembership",
    "SampleTable",
    "EditSegment",
    "EditList",
    "TrunInfo",
    "TrafInfo",
    "FragmentInfo",
    "SidxRef",
    "SidxInfo",
    "FragmentTimeline",
    "SubsampleRange",
    "SampleEncryption",
    "PsshInfo",
    "EncryptionInfo",
    "HdrKind",
    "HdrSignal",
    "HdrMetadata",
    "RawVideoFormat",
    "ItemReference",
    "ItemInfo",
    "TsPcrPoint",
    "TsPtsPoint",
    "TsPidBitrate",
    "TsTiming",
    "TsCheck",
    "TsReport",
    "ManifestSegment",
    "ManifestEncryption",
    "ManifestRepresentation",
    "ManifestAdaptationSet",
    "ManifestPeriod",
    "Manifest",
    "SegmentRef",
    "DashHlsContext",
    "ContainerRoot",
};

flatbuffers::DetachedBuffer build_container_root() {
  flatbuffers::FlatBufferBuilder fbb;

  const auto handler_fourcc = fbb.CreateString("vide");
  const auto entry_fourcc = fbb.CreateString("hvc1");
  ovt::fb::TrackInfoBuilder track(fbb);
  track.add_index(0);
  track.add_track_id(1);
  track.add_handler(ovt::fb::HandlerType::Video);
  track.add_handler_fourcc(handler_fourcc);
  track.add_codec(ovt::fb::Codec::H265);
  track.add_sample_entry_fourcc(entry_fourcc);
  track.add_timescale(90000);
  track.add_sample_count(3);
  const std::vector<flatbuffers::Offset<ovt::fb::TrackInfo>> tracks{track.Finish()};
  const auto tracks_vec = fbb.CreateVector(tracks);
  const auto format = fbb.CreateString("isobmff");
  ovt::fb::ContainerTreeBuilder tree(fbb);
  tree.add_format(format);
  tree.add_tracks(tracks_vec);
  const auto tree_off = tree.Finish();

  const std::vector<int64_t> dts{0, 3000, 6000};
  const std::vector<int32_t> cts_offset{3000, 6000, 0};
  const std::vector<uint32_t> duration{3000, 3000, 3000};
  const std::vector<uint64_t> offset{48, 1048, 1348};
  const std::vector<uint32_t> size{1000, 300, 200};
  const std::vector<uint8_t> is_sync{1, 0, 0};  // [bool] is stored as one byte per entry
  const auto dts_vec = fbb.CreateVector(dts);
  const auto cts_vec = fbb.CreateVector(cts_offset);
  const auto duration_vec = fbb.CreateVector(duration);
  const auto offset_vec = fbb.CreateVector(offset);
  const auto size_vec = fbb.CreateVector(size);
  const auto sync_vec = fbb.CreateVector(is_sync);
  ovt::fb::SampleTableBuilder table(fbb);
  table.add_track_id(1);
  table.add_timescale(90000);
  table.add_count(3);
  table.add_dts(dts_vec);
  table.add_cts_offset(cts_vec);
  table.add_duration(duration_vec);
  table.add_offset(offset_vec);
  table.add_size(size_vec);
  table.add_is_sync(sync_vec);
  const std::vector<flatbuffers::Offset<ovt::fb::SampleTable>> tables{table.Finish()};
  const auto tables_vec = fbb.CreateVector(tables);

  const auto file_name = fbb.CreateString("tiny.mp4");
  ovt::fb::ContainerRootBuilder root(fbb);
  root.add_schema_version(1);
  root.add_tree(tree_off);
  root.add_file_size(1548);
  root.add_file_name(file_name);
  root.add_sample_tables(tables_vec);
  ovt::fb::FinishContainerRootBuffer(fbb, root.Finish());
  return fbb.Release();
}

std::string load_file(const char* path) {
  std::string bytes;
  REQUIRE(flatbuffers::LoadFile(path, true, &bytes));
  return bytes;
}

// Names (without the ovt.fb. namespace) of every table, struct and enum a binary schema declares.
std::set<std::string> declared_types(const std::string& bfbs) {
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bfbs.data()), bfbs.size());
  REQUIRE(reflection::VerifySchemaBuffer(verifier));
  const reflection::Schema* schema = reflection::GetSchema(bfbs.data());
  std::set<std::string> names;
  constexpr std::string_view kNamespace = "ovt.fb.";
  const auto add = [&](const flatbuffers::String* name) {
    std::string_view n(name->c_str(), name->size());
    REQUIRE(n.substr(0, kNamespace.size()) == kNamespace);
    names.emplace(n.substr(kNamespace.size()));
  };
  for (const auto* object : *schema->objects()) {
    add(object->name());
  }
  for (const auto* e : *schema->enums()) {
    add(e->name());
  }
  return names;
}

}  // namespace

TEST_CASE("test_container_schema: ContainerRoot finishes as OVTC, verifies and reads back", "[schema][fast]") {
  CHECK(std::string_view(ovt::fb::ContainerRootIdentifier()) == "OVTC");
  CHECK(std::string_view(ovt::fb::ContainerRootExtension()) == "fc");

  const auto buf = build_container_root();
  CHECK(ovt::fb::ContainerRootBufferHasIdentifier(buf.data()));
  flatbuffers::Verifier verifier(buf.data(), buf.size());
  REQUIRE(ovt::fb::VerifyContainerRootBuffer(verifier));
  // The root_type/file_identifier of the including file win over those of ovt_model.fbs (WI-0.081 notes).
  flatbuffers::Verifier as_fa(buf.data(), buf.size());
  CHECK_FALSE(as_fa.VerifyBuffer<ovt::fb::FrameAnalysis>("OVTA"));

  const auto* root = ovt::fb::GetContainerRoot(buf.data());
  CHECK(root->schema_version() == 1U);
  CHECK(root->file_size() == 1548U);
  CHECK(root->file_name()->str() == "tiny.mp4");
  CHECK(root->raw_format() == nullptr);  // null for non-y4m files
  CHECK(root->dash_hls() == nullptr);
  CHECK(root->edit_lists() == nullptr);  // absent vector = "not built yet"

  REQUIRE(root->tree() != nullptr);
  CHECK(root->tree()->format()->str() == "isobmff");
  REQUIRE(root->tree()->tracks() != nullptr);
  REQUIRE(root->tree()->tracks()->size() == 1);
  const auto* track = root->tree()->tracks()->Get(0);
  CHECK(track->track_id() == 1U);
  CHECK(track->index() == 0U);
  CHECK(track->handler() == ovt::fb::HandlerType::Video);
  CHECK(track->handler_fourcc()->str() == "vide");
  CHECK(track->codec() == ovt::fb::Codec::H265);
  CHECK(track->avstream_index() == -1);  // schema default: unmatched
  CHECK(track->sample_count() == 3U);

  REQUIRE(root->sample_tables() != nullptr);
  REQUIRE(root->sample_tables()->size() == 1);
  const auto* table = root->sample_tables()->Get(0);
  CHECK(table->track_id() == track->track_id());
  CHECK(table->timescale() == 90000U);
  CHECK(table->count() == 3U);
  REQUIRE(table->dts() != nullptr);
  REQUIRE(table->dts()->size() == 3);
  CHECK(table->dts()->Get(0) == 0);
  CHECK(table->dts()->Get(1) == 3000);
  CHECK(table->dts()->Get(2) == 6000);
  REQUIRE(table->cts_offset()->size() == 3);
  CHECK(table->cts_offset()->Get(0) == 3000);
  CHECK(table->duration()->Get(2) == 3000U);
  CHECK(table->offset()->Get(1) == 1048U);
  CHECK(table->size()->Get(0) == 1000U);
  REQUIRE(table->is_sync()->size() == 3);
  CHECK(table->is_sync()->Get(0));
  CHECK_FALSE(table->is_sync()->Get(1));
  CHECK(table->is_partial_sync() == nullptr);
}

TEST_CASE("test_container_schema: §6.4 table list equals the tables of the compiled .bfbs", "[schema][fast]") {
  const std::set<std::string> container = declared_types(load_file(OVT_CONTAINER_BFBS_PATH));
  const std::set<std::string> model = declared_types(load_file(OVT_MODEL_BFBS_PATH));
  REQUIRE(container.contains("FrameAnalysis"));  // the include resolved

  std::set<std::string> own;
  for (const auto& name : container) {
    if (!model.contains(name)) {
      own.insert(name);
    }
  }
  const std::set<std::string> spec(kSpecTableList.begin(), kSpecTableList.end());
  CHECK(spec.size() == kSpecTableList.size());  // no duplicates in the literal list
  CHECK(own == spec);
  // No §6.4 name may shadow a §6.3 one (FlatBuffers would reject it; this keeps the subtraction honest).
  for (const auto& name : spec) {
    CHECK_FALSE(model.contains(name));
  }
}
