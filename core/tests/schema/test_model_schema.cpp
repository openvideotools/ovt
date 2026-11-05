// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.080: core/schema/ovt_model.fbs compiles and keeps the spec's numbering (chapter 06 §6.3, §6.1.1).
//
// Part 1 (WI-0.080 step 4): root buffers finish and verify with the §6.1.1 identifiers.
// Part 2 (WI-0.080 step 5): one assertion per enum value against the literal numbers of §6.3, so that a
// renumbered or reordered enum (forbidden by §6.1 P7) fails here before any golden file notices.
// The enum tables below are literal on purpose: do not regenerate them from the schema.

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "ovt_model_generated.h"

namespace {

bool verifies_with(const flatbuffers::FlatBufferBuilder& builder, const char* identifier) {
  flatbuffers::Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
  return verifier.VerifyBuffer<ovt::fb::FrameAnalysis>(identifier);
}

// SPEC-ASSUMPTION(#35): WI-0.080 step 4 names `VerifyStreamIndexBuffer(…, "OVTI")`, but flatc only
// generates `Verify<Root>Buffer` for the schema's root_type (FrameAnalysis). The secondary roots of
// §6.1.1 verify through Verifier::VerifyBuffer<T>(identifier), which is what this helper does.
template <typename Root>
bool VerifyRootBuffer(const flatbuffers::FlatBufferBuilder& builder, const char* identifier) {
  flatbuffers::Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
  return verifier.VerifyBuffer<Root>(identifier);
}

}  // namespace

TEST_CASE("test_model_schema: empty FrameAnalysis finishes and verifies as OVTA", "[schema][fast]") {
  CHECK(std::string_view(ovt::fb::FrameAnalysisIdentifier()) == "OVTA");
  CHECK(std::string_view(ovt::fb::FrameAnalysisExtension()) == "fa");

  flatbuffers::FlatBufferBuilder builder;
  ovt::fb::FrameAnalysisBuilder frame_analysis(builder);
  frame_analysis.add_schema_version(1);
  ovt::fb::FinishFrameAnalysisBuffer(builder, frame_analysis.Finish());

  CHECK(ovt::fb::FrameAnalysisBufferHasIdentifier(builder.GetBufferPointer()));
  flatbuffers::Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
  REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
  CHECK_FALSE(verifies_with(builder, "OVTI"));

  const auto* root = ovt::fb::GetFrameAnalysis(builder.GetBufferPointer());
  REQUIRE(root != nullptr);
  CHECK(root->schema_version() == 1U);
  CHECK(root->frame() == nullptr);
  CHECK(root->grid() == nullptr);
  CHECK(root->blocks() == nullptr);  // P6: an absent vector is "not provided"
}

TEST_CASE("test_model_schema: StreamIndex finishes with OVTI and verifies with it", "[schema][fast]") {
  flatbuffers::FlatBufferBuilder builder;
  ovt::fb::StreamIndexBuilder stream_index(builder);
  stream_index.add_codec(ovt::fb::Codec::H265);
  stream_index.add_schema_version(1);
  builder.Finish(stream_index.Finish(), "OVTI");

  REQUIRE(VerifyRootBuffer<ovt::fb::StreamIndex>(builder, "OVTI"));
  CHECK_FALSE(VerifyRootBuffer<ovt::fb::StreamIndex>(builder, "OVTA"));
  CHECK(flatbuffers::BufferHasIdentifier(builder.GetBufferPointer(), "OVTI"));

  const auto* root = flatbuffers::GetRoot<ovt::fb::StreamIndex>(builder.GetBufferPointer());
  CHECK(root->codec() == ovt::fb::Codec::H265);
  CHECK(root->schema_version() == 1U);
}

TEST_CASE("test_model_schema: the other §6.1.1 root tables verify with their identifiers", "[schema][fast]") {
  flatbuffers::FlatBufferBuilder builder;
  const auto name = builder.CreateString("sps_video_parameter_set_id");
  ovt::fb::SyntaxNodeBuilder node(builder);
  node.add_name(name);
  builder.Finish(node.Finish(), "OVTS");
  CHECK(VerifyRootBuffer<ovt::fb::SyntaxNode>(builder, "OVTS"));

  flatbuffers::FlatBufferBuilder envelope_builder;
  ovt::fb::CliEnvelopeBuilder envelope(envelope_builder);
  envelope_builder.Finish(envelope.Finish(), "OVTE");
  CHECK(VerifyRootBuffer<ovt::fb::CliEnvelope>(envelope_builder, "OVTE"));

  flatbuffers::FlatBufferBuilder plugins_builder;
  ovt::fb::PluginListBuilder plugins(plugins_builder);
  plugins_builder.Finish(plugins.Finish(), "OVTP");
  CHECK(VerifyRootBuffer<ovt::fb::PluginList>(plugins_builder, "OVTP"));
}

TEST_CASE("test_model_schema: frozen structs keep their §6.1 P7 layout", "[schema][fast]") {
  STATIC_CHECK(sizeof(ovt::fb::Rect) == 16);
  STATIC_CHECK(sizeof(ovt::fb::ByteSpan) == 16);
  STATIC_CHECK(sizeof(ovt::fb::BitSpan) == 16);  // ulong + uint, padded to 8
  STATIC_CHECK(sizeof(ovt::fb::FrameId) == 8);
}

TEST_CASE("test_model_schema: enum underlying types follow the schema", "[schema][fast]") {
  STATIC_CHECK(std::is_same_v<std::underlying_type_t<ovt::fb::Av1RefFrame>, int8_t>);  // `: byte`
  STATIC_CHECK(std::is_same_v<std::underlying_type_t<ovt::fb::AvcMbType>, uint8_t>);
  STATIC_CHECK(std::is_same_v<std::underlying_type_t<ovt::fb::Codec>, uint8_t>);
}

// ---- WI-0.080 step 5: spec numbering of every enum value (literal tables) -------------------------

TEST_CASE("test_model_schema: enum Codec numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Codec::Unknown) == 0);
  CHECK(static_cast<int>(ovt::fb::Codec::H264) == 1);
  CHECK(static_cast<int>(ovt::fb::Codec::H265) == 2);
  CHECK(static_cast<int>(ovt::fb::Codec::AV1) == 3);
  CHECK(static_cast<int>(ovt::fb::Codec::VP9) == 4);
  CHECK(static_cast<int>(ovt::fb::Codec::H266) == 5);
  CHECK(static_cast<int>(ovt::fb::Codec::MPEG2) == 6);
}

TEST_CASE("test_model_schema: enum SyntaxValueKind numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::None) == 0);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::UInt) == 1);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::Int) == 2);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::Bool) == 3);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::Bytes) == 4);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::String) == 5);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::Enum) == 6);
  CHECK(static_cast<int>(ovt::fb::SyntaxValueKind::Fixed) == 7);
}

TEST_CASE("test_model_schema: enum Severity numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Severity::Info) == 0);
  CHECK(static_cast<int>(ovt::fb::Severity::Warning) == 1);
  CHECK(static_cast<int>(ovt::fb::Severity::Error) == 2);
  CHECK(static_cast<int>(ovt::fb::Severity::Fatal) == 3);
}

TEST_CASE("test_model_schema: enum FrameType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::FrameType::I) == 0);
  CHECK(static_cast<int>(ovt::fb::FrameType::P) == 1);
  CHECK(static_cast<int>(ovt::fb::FrameType::B) == 2);
  CHECK(static_cast<int>(ovt::fb::FrameType::Key) == 3);
  CHECK(static_cast<int>(ovt::fb::FrameType::Inter) == 4);
  CHECK(static_cast<int>(ovt::fb::FrameType::IntraOnly) == 5);
  CHECK(static_cast<int>(ovt::fb::FrameType::Switch) == 6);
  CHECK(static_cast<int>(ovt::fb::FrameType::Unknown) == 7);
}

TEST_CASE("test_model_schema: enum PictureStructure numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::PictureStructure::Frame) == 0);
  CHECK(static_cast<int>(ovt::fb::PictureStructure::TopField) == 1);
  CHECK(static_cast<int>(ovt::fb::PictureStructure::BottomField) == 2);
  CHECK(static_cast<int>(ovt::fb::PictureStructure::Mbaff) == 3);
}

TEST_CASE("test_model_schema: enum PredKind numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::PredKind::None) == 0);
  CHECK(static_cast<int>(ovt::fb::PredKind::Intra) == 1);
  CHECK(static_cast<int>(ovt::fb::PredKind::Inter) == 2);
  CHECK(static_cast<int>(ovt::fb::PredKind::Skip) == 3);
  CHECK(static_cast<int>(ovt::fb::PredKind::IntraBC) == 4);
  CHECK(static_cast<int>(ovt::fb::PredKind::PCM) == 5);
  CHECK(static_cast<int>(ovt::fb::PredKind::Palette) == 6);
}

TEST_CASE("test_model_schema: enum BlockKind numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::BlockKind::MB) == 0);
  CHECK(static_cast<int>(ovt::fb::BlockKind::SubMB) == 1);
  CHECK(static_cast<int>(ovt::fb::BlockKind::CTU) == 2);
  CHECK(static_cast<int>(ovt::fb::BlockKind::CU) == 3);
  CHECK(static_cast<int>(ovt::fb::BlockKind::PU) == 4);
  CHECK(static_cast<int>(ovt::fb::BlockKind::TU) == 5);
  CHECK(static_cast<int>(ovt::fb::BlockKind::SB) == 6);
  CHECK(static_cast<int>(ovt::fb::BlockKind::Block) == 7);
  CHECK(static_cast<int>(ovt::fb::BlockKind::TXB) == 8);
  CHECK(static_cast<int>(ovt::fb::BlockKind::Other) == 9);
}

TEST_CASE("test_model_schema: enum PlaneStage numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::PlaneStage::Reconstruction) == 0);
  CHECK(static_cast<int>(ovt::fb::PlaneStage::Prediction) == 1);
  CHECK(static_cast<int>(ovt::fb::PlaneStage::Residual) == 2);
  CHECK(static_cast<int>(ovt::fb::PlaneStage::PreDeblock) == 3);
  CHECK(static_cast<int>(ovt::fb::PlaneStage::PreSaoOrCdef) == 4);
  CHECK(static_cast<int>(ovt::fb::PlaneStage::PreLoopRestoration) == 5);
  CHECK(static_cast<int>(ovt::fb::PlaneStage::Coefficients) == 6);
}

TEST_CASE("test_model_schema: enum PartitionKind numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::PartitionKind::Slice) == 0);
  CHECK(static_cast<int>(ovt::fb::PartitionKind::SliceSegment) == 1);
  CHECK(static_cast<int>(ovt::fb::PartitionKind::Tile) == 2);
  CHECK(static_cast<int>(ovt::fb::PartitionKind::TileGroup) == 3);
  CHECK(static_cast<int>(ovt::fb::PartitionKind::Substream) == 4);
}

TEST_CASE("test_model_schema: enum AvcMbType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_NxN) == 0);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_0_0_0) == 1);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_1_0_0) == 2);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_2_0_0) == 3);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_3_0_0) == 4);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_0_1_0) == 5);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_1_1_0) == 6);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_2_1_0) == 7);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_3_1_0) == 8);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_0_2_0) == 9);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_1_2_0) == 10);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_2_2_0) == 11);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_3_2_0) == 12);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_0_0_1) == 13);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_1_0_1) == 14);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_2_0_1) == 15);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_3_0_1) == 16);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_0_1_1) == 17);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_1_1_1) == 18);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_2_1_1) == 19);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_3_1_1) == 20);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_0_2_1) == 21);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_1_2_1) == 22);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_2_2_1) == 23);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_16x16_3_2_1) == 24);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::I_PCM) == 25);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::P_L0_16x16) == 26);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::P_L0_L0_16x8) == 27);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::P_L0_L0_8x16) == 28);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::P_8x8) == 29);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::P_8x8ref0) == 30);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Direct_16x16) == 31);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_16x16) == 32);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_16x16) == 33);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_16x16) == 34);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_L0_16x8) == 35);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_L0_8x16) == 36);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_L1_16x8) == 37);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_L1_8x16) == 38);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_L1_16x8) == 39);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_L1_8x16) == 40);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_L0_16x8) == 41);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_L0_8x16) == 42);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_Bi_16x8) == 43);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L0_Bi_8x16) == 44);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_Bi_16x8) == 45);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_L1_Bi_8x16) == 46);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_L0_16x8) == 47);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_L0_8x16) == 48);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_L1_16x8) == 49);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_L1_8x16) == 50);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_Bi_16x8) == 51);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Bi_Bi_8x16) == 52);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_8x8) == 53);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::P_Skip) == 54);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::B_Skip) == 55);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::SI) == 56);
  CHECK(static_cast<int>(ovt::fb::AvcMbType::AvcMbType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum AvcSubMbType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::P_L0_8x8) == 0);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::P_L0_8x4) == 1);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::P_L0_4x8) == 2);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::P_L0_4x4) == 3);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_Direct_8x8) == 4);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L0_8x8) == 5);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L1_8x8) == 6);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_Bi_8x8) == 7);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L0_8x4) == 8);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L0_4x8) == 9);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L1_8x4) == 10);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L1_4x8) == 11);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_Bi_8x4) == 12);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_Bi_4x8) == 13);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L0_4x4) == 14);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_L1_4x4) == 15);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::B_Bi_4x4) == 16);
  CHECK(static_cast<int>(ovt::fb::AvcSubMbType::AvcSubMbType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum AvcIntra4x4PredMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Vertical) == 0);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Horizontal) == 1);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::DC) == 2);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Diagonal_Down_Left) == 3);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Diagonal_Down_Right) == 4);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Vertical_Right) == 5);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Horizontal_Down) == 6);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Vertical_Left) == 7);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::Horizontal_Up) == 8);
  CHECK(static_cast<int>(ovt::fb::AvcIntra4x4PredMode::AvcIntra4x4_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum AvcIntra16x16PredMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::AvcIntra16x16PredMode::I16_Vertical) == 0);
  CHECK(static_cast<int>(ovt::fb::AvcIntra16x16PredMode::I16_Horizontal) == 1);
  CHECK(static_cast<int>(ovt::fb::AvcIntra16x16PredMode::I16_DC) == 2);
  CHECK(static_cast<int>(ovt::fb::AvcIntra16x16PredMode::I16_Plane) == 3);
  CHECK(static_cast<int>(ovt::fb::AvcIntra16x16PredMode::AvcIntra16x16_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum AvcIntraChromaPredMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::AvcIntraChromaPredMode::Chroma_DC) == 0);
  CHECK(static_cast<int>(ovt::fb::AvcIntraChromaPredMode::Chroma_Horizontal) == 1);
  CHECK(static_cast<int>(ovt::fb::AvcIntraChromaPredMode::Chroma_Vertical) == 2);
  CHECK(static_cast<int>(ovt::fb::AvcIntraChromaPredMode::Chroma_Plane) == 3);
  CHECK(static_cast<int>(ovt::fb::AvcIntraChromaPredMode::AvcIntraChroma_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum HevcPartMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_2Nx2N) == 0);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_2NxN) == 1);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_Nx2N) == 2);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_NxN) == 3);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_2NxnU) == 4);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_2NxnD) == 5);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_nLx2N) == 6);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::PART_nRx2N) == 7);
  CHECK(static_cast<int>(ovt::fb::HevcPartMode::HevcPartMode_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum HevcPredMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::HevcPredMode::MODE_INTER) == 0);
  CHECK(static_cast<int>(ovt::fb::HevcPredMode::MODE_INTRA) == 1);
  CHECK(static_cast<int>(ovt::fb::HevcPredMode::MODE_SKIP) == 2);
  CHECK(static_cast<int>(ovt::fb::HevcPredMode::HevcPredMode_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum HevcInterPredIdc numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::HevcInterPredIdc::PRED_L0) == 0);
  CHECK(static_cast<int>(ovt::fb::HevcInterPredIdc::PRED_L1) == 1);
  CHECK(static_cast<int>(ovt::fb::HevcInterPredIdc::PRED_BI) == 2);
  CHECK(static_cast<int>(ovt::fb::HevcInterPredIdc::HevcInterPredIdc_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum HevcSaoType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::HevcSaoType::SAO_NOT_APPLIED) == 0);
  CHECK(static_cast<int>(ovt::fb::HevcSaoType::SAO_BAND) == 1);
  CHECK(static_cast<int>(ovt::fb::HevcSaoType::SAO_EDGE) == 2);
  CHECK(static_cast<int>(ovt::fb::HevcSaoType::HevcSaoType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum HevcMergeCandSource numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_Unknown) == 0);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_SpatialA0) == 1);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_SpatialA1) == 2);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_SpatialB0) == 3);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_SpatialB1) == 4);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_SpatialB2) == 5);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_Temporal) == 6);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_CombinedBi) == 7);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_Zero) == 8);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_AmvpA) == 9);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_AmvpB) == 10);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_AmvpTemporal) == 11);
  CHECK(static_cast<int>(ovt::fb::HevcMergeCandSource::MC_AmvpZero) == 12);
}

TEST_CASE("test_model_schema: enum Av1Partition numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_NONE) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_HORZ) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_VERT) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_SPLIT) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_HORZ_A) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_HORZ_B) == 5);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_VERT_A) == 6);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_VERT_B) == 7);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_HORZ_4) == 8);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::PARTITION_VERT_4) == 9);
  CHECK(static_cast<int>(ovt::fb::Av1Partition::Av1Partition_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1BlockSize numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_4X4) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_4X8) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_8X4) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_8X8) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_8X16) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_16X8) == 5);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_16X16) == 6);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_16X32) == 7);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_32X16) == 8);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_32X32) == 9);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_32X64) == 10);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_64X32) == 11);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_64X64) == 12);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_64X128) == 13);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_128X64) == 14);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_128X128) == 15);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_4X16) == 16);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_16X4) == 17);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_8X32) == 18);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_32X8) == 19);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_16X64) == 20);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::BLOCK_64X16) == 21);
  CHECK(static_cast<int>(ovt::fb::Av1BlockSize::Av1BlockSize_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1YMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1YMode::DC_PRED) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::V_PRED) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::H_PRED) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::D45_PRED) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::D135_PRED) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::D113_PRED) == 5);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::D157_PRED) == 6);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::D203_PRED) == 7);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::D67_PRED) == 8);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::SMOOTH_PRED) == 9);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::SMOOTH_V_PRED) == 10);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::SMOOTH_H_PRED) == 11);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::PAETH_PRED) == 12);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEARESTMV) == 13);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEARMV) == 14);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::GLOBALMV) == 15);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEWMV) == 16);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEAREST_NEARESTMV) == 17);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEAR_NEARMV) == 18);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEAREST_NEWMV) == 19);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEW_NEARESTMV) == 20);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEAR_NEWMV) == 21);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEW_NEARMV) == 22);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::GLOBAL_GLOBALMV) == 23);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::NEW_NEWMV) == 24);
  CHECK(static_cast<int>(ovt::fb::Av1YMode::Av1YMode_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1FilterIntraMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1FilterIntraMode::FILTER_DC_PRED) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1FilterIntraMode::FILTER_V_PRED) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1FilterIntraMode::FILTER_H_PRED) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1FilterIntraMode::FILTER_D157_PRED) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1FilterIntraMode::FILTER_PAETH_PRED) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1FilterIntraMode::Av1FilterIntraMode_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1InterIntraMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1InterIntraMode::II_DC_PRED) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1InterIntraMode::II_V_PRED) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1InterIntraMode::II_H_PRED) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1InterIntraMode::II_SMOOTH_PRED) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1InterIntraMode::Av1InterIntraMode_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1MotionMode numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1MotionMode::SIMPLE) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1MotionMode::OBMC) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1MotionMode::LOCALWARP) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1MotionMode::Av1MotionMode_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1CompoundType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1CompoundType::COMPOUND_WEDGE) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1CompoundType::COMPOUND_DIFFWTD) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1CompoundType::COMPOUND_AVERAGE) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1CompoundType::COMPOUND_INTRA) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1CompoundType::COMPOUND_DISTANCE) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1CompoundType::Av1CompoundType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1CompRefType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1CompRefType::UNIDIR_COMP_REFERENCE) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1CompRefType::BIDIR_COMP_REFERENCE) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1CompRefType::Av1CompRefType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1InterpFilter numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1InterpFilter::EIGHTTAP) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1InterpFilter::EIGHTTAP_SMOOTH) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1InterpFilter::EIGHTTAP_SHARP) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1InterpFilter::BILINEAR) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1InterpFilter::SWITCHABLE) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1InterpFilter::Av1InterpFilter_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1TxSize numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_4X4) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_8X8) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_16X16) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_32X32) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_64X64) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_4X8) == 5);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_8X4) == 6);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_8X16) == 7);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_16X8) == 8);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_16X32) == 9);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_32X16) == 10);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_32X64) == 11);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_64X32) == 12);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_4X16) == 13);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_16X4) == 14);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_8X32) == 15);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_32X8) == 16);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_16X64) == 17);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::TX_64X16) == 18);
  CHECK(static_cast<int>(ovt::fb::Av1TxSize::Av1TxSize_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1TxType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1TxType::DCT_DCT) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::ADST_DCT) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::DCT_ADST) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::ADST_ADST) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::FLIPADST_DCT) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::DCT_FLIPADST) == 5);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::FLIPADST_FLIPADST) == 6);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::ADST_FLIPADST) == 7);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::FLIPADST_ADST) == 8);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::IDTX) == 9);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::V_DCT) == 10);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::H_DCT) == 11);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::V_ADST) == 12);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::H_ADST) == 13);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::V_FLIPADST) == 14);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::H_FLIPADST) == 15);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::WHT_WHT) == 16);
  CHECK(static_cast<int>(ovt::fb::Av1TxType::Av1TxType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1RefFrame numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::NONE) == -1);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::INTRA_FRAME) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::LAST_FRAME) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::LAST2_FRAME) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::LAST3_FRAME) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::GOLDEN_FRAME) == 4);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::BWDREF_FRAME) == 5);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::ALTREF2_FRAME) == 6);
  CHECK(static_cast<int>(ovt::fb::Av1RefFrame::ALTREF_FRAME) == 7);
}

TEST_CASE("test_model_schema: enum Av1RestorationType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1RestorationType::RESTORE_NONE) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1RestorationType::RESTORE_WIENER) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1RestorationType::RESTORE_SGRPROJ) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1RestorationType::RESTORE_SWITCHABLE) == 3);
  CHECK(static_cast<int>(ovt::fb::Av1RestorationType::Av1RestorationType_Unknown) == 255);
}

TEST_CASE("test_model_schema: enum Av1GmType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1GmType::IDENTITY) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1GmType::TRANSLATION) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1GmType::ROTZOOM) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1GmType::AFFINE) == 3);
}

TEST_CASE("test_model_schema: enum Av1FrameType numbering", "[schema][fast]") {
  CHECK(static_cast<int>(ovt::fb::Av1FrameType::KEY_FRAME) == 0);
  CHECK(static_cast<int>(ovt::fb::Av1FrameType::INTER_FRAME) == 1);
  CHECK(static_cast<int>(ovt::fb::Av1FrameType::INTRA_ONLY_FRAME) == 2);
  CHECK(static_cast<int>(ovt::fb::Av1FrameType::SWITCH_FRAME) == 3);
}
