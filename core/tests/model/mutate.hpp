// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fixture mutators for the validator tests (chapter 06 WI-0.086 step 6): each one takes a WI-0.093 golden
// fixture through the object API, breaks exactly one §6.9 rule with a minimal edit, and repacks it. The
// edits are chosen so that no other rule notices them (e.g. a mutation that moves a cell to another block
// also clears grid.bits_x8, which V-020 would otherwise check); test_validator asserts that only the intended
// rule fires.

#ifndef OVT_TESTS_MODEL_MUTATE_HPP
#define OVT_TESTS_MODEL_MUTATE_HPP

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::test {

namespace fb = ovt::fb;

using Bytes = std::vector<std::uint8_t>;
using Mutator = void (*)(fb::FrameAnalysisT&);

/// Golden fixture stems of core/tests/golden/ (WI-0.093).
enum class Fixture : std::uint8_t { Hevc16, H264_32, Av1_64 };

inline constexpr std::array<std::string_view, 3> kFixtureFiles{"tiny16_hevc.fa", "tiny32_h264.fa", "tiny64_av1.fa"};

/// The stream hash every fixture's PlaneRef.blob_id names (fixtures_common.hpp, recon_blob_id).
inline constexpr std::string_view kFixtureSourceHash =
    "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0";

/// Unpacks @p fixture, applies @p fn and repacks it (canonical object-API build, identifier OVTA).
inline Bytes mutate(const Bytes& fixture, Mutator fn) {
  fb::FrameAnalysisT fa;
  fb::GetFrameAnalysis(fixture.data())->UnPackTo(&fa);
  fn(fa);
  flatbuffers::FlatBufferBuilder builder;
  fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &fa));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

/// Replaces the width of a struct-typed rect (structs have no mutators without flatc --gen-mutable).
inline void set_width(std::unique_ptr<fb::Rect>& rect, std::int32_t w) {
  rect = std::make_unique<fb::Rect>(rect->x(), rect->y(), w, rect->h());
}

/// Unpacks the nested FlatBuffer @p bytes (root table @p T), applies @p fn and repacks it in place.
template <class T, class Fn>
void edit_nested(std::vector<std::uint8_t>& bytes, Fn fn) {
  typename T::NativeTableType native;
  flatbuffers::GetRoot<T>(bytes.data())->UnPackTo(&native);
  fn(native);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(T::Pack(builder, &native));
  bytes.assign(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
}

/// One row of the table-driven rule test: applying @p mutate to @p fixture must make exactly @p rule fire.
struct RuleMutation {
  std::string_view rule;
  Fixture fixture;
  std::string_view what;
  Mutator mutate;
};

// Block ids used below. HEVC (§6.10): 0 root, 1 CTU, CUs 2/5/8/11, each followed by its PU and TU.
// H.264: 0 root, MBs 1/2/3/8, SubMBs 4..7 of MB 3 (8x8 each, from (0,16)). AV1: 0 root, 1 SB, Blocks + TXBs.
inline constexpr std::uint32_t kHevcCu = 2;
inline constexpr std::uint32_t kHevcPu = 3;
inline constexpr std::uint32_t kHevcTu = 4;
inline constexpr std::uint32_t kH264SubMb = 4;
inline constexpr std::uint32_t kH264Mb = 1;
inline constexpr std::uint32_t kAv1Block = 2;

inline const std::vector<RuleMutation>& rule_mutations() {
  static const std::vector<RuleMutation> rows{
      {"V-001", Fixture::Hevc16, "plugin_version empty", [](fb::FrameAnalysisT& fa) { fa.plugin_version.clear(); }},
      {"V-002", Fixture::Hevc16, "timebase_den 0", [](fb::FrameAnalysisT& fa) { fa.frame->timebase_den = 0; }},
      {"V-003", Fixture::Hevc16, "grid.intra_mode_c one element short",
       [](fb::FrameAnalysisT& fa) { fa.grid->intra_mode_c.pop_back(); }},
      {"V-004", Fixture::Hevc16, "root rect wider than coded_size",
       [](fb::FrameAnalysisT& fa) { set_width(fa.blocks[0]->rect, fa.blocks[0]->rect->w() + 16); }},
      {"V-005", Fixture::Hevc16, "PU listed twice in its CU's children",
       [](fb::FrameAnalysisT& fa) { fa.blocks[kHevcCu]->children.push_back(kHevcPu); }},
      {"V-006", Fixture::Hevc16, "TU width not a multiple of 4",
       [](fb::FrameAnalysisT& fa) { set_width(fa.blocks[kHevcTu]->rect, 6); }},
      {"V-007", Fixture::H264_32, "SubMB widened over its sibling",
       [](fb::FrameAnalysisT& fa) { set_width(fa.blocks[kH264SubMb]->rect, 16); }},
      {"V-008", Fixture::Hevc16, "TU turned into an AV1 TXB (no ext)",
       [](fb::FrameAnalysisT& fa) {
         auto& tu = *fa.blocks[kHevcTu];
         tu.kind = fb::BlockKind::TXB;
         tu.ext_type.clear();
         tu.ext.clear();
       }},
      {"V-009", Fixture::Hevc16, "TU level off by one", [](fb::FrameAnalysisT& fa) { fa.blocks[kHevcTu]->level = 4; }},
      {"V-010", Fixture::Hevc16, "cell 0 owned by the root",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->block_id[0] = 0;
         fa.grid->bits_x8.clear();
       }},
      {"V-011", Fixture::Hevc16, "cell 0 owned by the CTU",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->block_id[0] = 1;
         fa.grid->bits_x8.clear();
       }},
      {"V-012", Fixture::Hevc16, "cell 0 owned by the CU to its right",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->block_id[0] = 5;
         fa.grid->bits_x8.clear();
       }},
      {"V-013", Fixture::Hevc16, "grid.pred value outside PredKind",
       [](fb::FrameAnalysisT& fa) { fa.grid->pred[0] = 7; }},
      {"V-014", Fixture::H264_32, "H.264 RefSlot.slot 9", [](fb::FrameAnalysisT& fa) { fa.refs[0]->slot = 9; }},
      {"V-015", Fixture::H264_32, "MV x disagrees with grid.mv0_x",
       [](fb::FrameAnalysisT& fa) { fa.mvs[0]->mv_x += 1; }},
      {"V-016", Fixture::H264_32, "usage_cells off by one",
       [](fb::FrameAnalysisT& fa) { fa.refs[0]->usage_cells += 1; }},
      {"V-017", Fixture::Hevc16, "slice bit_count beyond its bytes",
       [](fb::FrameAnalysisT& fa) { fa.partitions[0]->bit_count = 100000; }},
      {"V-018", Fixture::H264_32, "field flag on a frame MB",
       [](fb::FrameAnalysisT& fa) { fa.mvs[0]->flags |= 0x10U; }},
      {"V-019", Fixture::Hevc16, "CU bits below PU + TU bits",
       [](fb::FrameAnalysisT& fa) {
         fa.blocks[kHevcCu]->bits_x8 = 200;
         fa.grid->bits_x8.clear();
       }},
      {"V-020", Fixture::Hevc16, "one bit moved between two cells of a CU",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->bits_x8[0] += 1;
         fa.grid->bits_x8[1] -= 1;
       }},
      {"V-021", Fixture::Hevc16, "PU ext kept but ext_type cleared",
       [](fb::FrameAnalysisT& fa) { fa.blocks[kHevcPu]->ext_type.clear(); }},
      {"V-022", Fixture::Hevc16, "HevcCu log2CbSize 4 on an 8x8 CU",
       [](fb::FrameAnalysisT& fa) {
         edit_nested<fb::HevcCu>(fa.blocks[kHevcCu]->ext, [](fb::HevcCuT& cu) { cu.log2CbSize = 4; });
       }},
      {"V-023", Fixture::Hevc16, "luma plane stride below its width",
       [](fb::FrameAnalysisT& fa) { fa.planes[0]->stride_bytes = 8; }},
      {"V-024", Fixture::Hevc16, "symbol cost without a symbol name",
       [](fb::FrameAnalysisT& fa) {
         auto symbol = std::make_unique<fb::SymbolCostT>();
         symbol->count = 1;
         fa.symbols.push_back(std::move(symbol));
       }},
      {"V-025", Fixture::H264_32, "first two motion vectors swapped",
       [](fb::FrameAnalysisT& fa) { std::swap(fa.mvs[0], fa.mvs[1]); }},
      {"V-026", Fixture::H264_32, "intra cell referencing L0[0] (usage_cells kept consistent)",
       [](fb::FrameAnalysisT& fa) {
         fa.grid->ref0[0] = 0;
         fa.refs[0]->usage_cells += 1;
       }},
      {"V-027", Fixture::Hevc16, "cbf 9", [](fb::FrameAnalysisT& fa) { fa.grid->cbf[0] = 9; }},
      {"V-028", Fixture::Hevc16, "qp_hist bin 0 counts a phantom cell",
       [](fb::FrameAnalysisT& fa) { fa.qp_hist[0] = 1; }},
      {"V-029", Fixture::Hevc16, "diagnostic code without family",
       [](fb::FrameAnalysisT& fa) {
         auto diag = std::make_unique<fb::DiagnosticT>();
         diag->severity = fb::Severity::Info;
         diag->code = "OOPS-1";
         diag->message = "malformed code";
         fa.diagnostics.push_back(std::move(diag));
       }},
      // Extra rows: the codec-specific branches of rules already covered above.
      {"V-014", Fixture::Av1_64, "AV1 refs[0] not named INTRA",
       [](fb::FrameAnalysisT& fa) { fa.refs[0]->name = "LAST0"; }},
      {"V-022", Fixture::Av1_64, "Av1Block MiCol off by one",
       [](fb::FrameAnalysisT& fa) {
         edit_nested<fb::Av1Block>(fa.blocks[kAv1Block]->ext, [](fb::Av1BlockT& b) { b.MiCol += 1; });
       }},
      {"V-022", Fixture::H264_32, "AvcMb mb_addr off by one",
       [](fb::FrameAnalysisT& fa) {
         edit_nested<fb::AvcMb>(fa.blocks[kH264Mb]->ext, [](fb::AvcMbT& mb) { mb.mb_addr += 1; });
       }},
      {"V-023", Fixture::Av1_64, "blob_id names another stream",
       [](fb::FrameAnalysisT& fa) { fa.planes[1]->blob_id[0] = '7'; }},
  };
  return rows;
}

// ---- StreamIndex (V-031) -------------------------------------------------------------------------------------

using IndexMutator = void (*)(fb::StreamIndexT&);

/// A small consistent StreamIndex: 3 frames in decode order with display order 0, 2, 1, two units each (a
/// parameter set or AUD, then the slice), the first unit listed as a parameter set.
inline fb::StreamIndexT tiny_stream_index() {
  fb::StreamIndexT si;
  si.codec = fb::Codec::H265;
  si.schema_version = 1;
  constexpr std::array<std::int32_t, 3> kDisplay{0, 2, 1};
  std::uint64_t offset = 0;
  for (std::uint32_t u = 0; u < 6; ++u) {
    auto unit = std::make_unique<fb::UnitT>();
    unit->index = u;
    unit->prefix_span = std::make_unique<fb::ByteSpan>(offset, 4U);
    unit->file_span = std::make_unique<fb::ByteSpan>(offset + 4, 10U + u);
    offset += 14U + u;
    unit->frame = std::make_unique<fb::FrameId>(static_cast<std::int32_t>(u / 2), kDisplay.at(u / 2));
    si.units.push_back(std::move(unit));
  }
  for (std::uint32_t f = 0; f < 3; ++f) {
    auto frame = std::make_unique<fb::FrameInfoT>();
    frame->id = std::make_unique<fb::FrameId>(static_cast<std::int32_t>(f), kDisplay.at(f));
    frame->units = {2 * f, (2 * f) + 1};
    frame->size_bytes = 28U + (4U * f) + 1U;  // (4 + 10 + 2f) + (4 + 10 + 2f + 1)
    if (f > 0) {
      frame->declared_refs = {0};
    }
    si.frames.push_back(std::move(frame));
  }
  si.display_order = {0, 2, 1};
  auto ps = std::make_unique<fb::UnitT>();
  ps->index = 0;
  si.parameter_sets.push_back(std::move(ps));
  return si;
}

inline Bytes pack_index(const fb::StreamIndexT& si) {
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(fb::StreamIndex::Pack(builder, &si), "OVTI");
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

struct IndexMutation {
  std::string_view rule;
  std::string_view what;
  IndexMutator mutate;
};

inline const std::vector<IndexMutation>& index_mutations() {
  static const std::vector<IndexMutation> rows{
      {"V-001", "schema_version 2", [](fb::StreamIndexT& si) { si.schema_version = 2; }},
      {"V-029", "diagnostic without message",
       [](fb::StreamIndexT& si) {
         auto diag = std::make_unique<fb::DiagnosticT>();
         diag->code = "SYN-H265-0007";
         si.diagnostics.push_back(std::move(diag));
       }},
      {"V-031", "unit index gap", [](fb::StreamIndexT& si) { si.units[3]->index = 7; }},
      {"V-031", "display_order swapped",
       [](fb::StreamIndexT& si) { std::swap(si.display_order[1], si.display_order[2]); }},
      {"V-031", "size_bytes off by one", [](fb::StreamIndexT& si) { si.frames[1]->size_bytes += 1; }},
      {"V-031", "frame lists another frame's unit", [](fb::StreamIndexT& si) { si.frames[2]->units[0] = 0; }},
      {"V-031", "declared ref names the frame itself", [](fb::StreamIndexT& si) { si.frames[1]->declared_refs = {1}; }},
      {"V-031", "parameter set outside units", [](fb::StreamIndexT& si) { si.parameter_sets[0]->index = 6; }},
  };
  return rows;
}

}  // namespace ovt::test

#endif  // OVT_TESTS_MODEL_MUTATE_HPP
