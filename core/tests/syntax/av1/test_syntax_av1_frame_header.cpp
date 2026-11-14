// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the AV1 uncompressed_header() sub-structures (chapter 7 §7.3.6 rows 12–19, WI-1.117): frame_size
// with superres_params and compute_image_size, render_size, frame_size_with_refs, tile_info, quantization_params,
// segmentation_params, delta_q_params and delta_lf_params, plus get_qindex() and the lossless derivations. The
// uncompressed_header() orchestration (row 11) and its corpus tests are WI-1.117c.
//
// Expected values come from a recorded oracle run cited at each fixture (FFmpeg 6.1.1 `-bsf:v trace_headers` on
// the chapter 14 synthetic stream syn_av1_tiles_cdef_lr, recipe 13; bit offsets are those the trace prints,
// counted from the first OBU header bit) or from a hand-worked transcription of the AV1 §5.9.x equations given in
// the comments. GStreamer's av1parse (acceptance test 2) is not available on the build hosts: the sizes of the
// frame_size_with_refs tests are the §5.9.7 / §5.9.8 / §5.9.9 equations worked by hand on the slot values (issue
// #513, ruled: oracle substitution). Parity over the aom vectors is WI-1.572.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/frame_header.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"
#include "test_support/corpus.h"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)
using ovt::BitReader;
using ovt::syntax::DiagnosticData;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::syntax::ValueKind;
using Bytes = std::vector<uint8_t>;

// ---- bit writer with the AV1 descriptors -------------------------------------------------------------------

class BitWriter {
 public:
  BitWriter& put(uint64_t value, unsigned bits) {
    for (unsigned i = bits; i-- > 0;) {
      if (nbits_ % 8 == 0) {
        bytes_.push_back(0);
      }
      if (((value >> i) & 1U) != 0) {
        bytes_.back() = static_cast<uint8_t>(bytes_.back() | (0x80U >> (nbits_ % 8)));
      }
      ++nbits_;
    }
    return *this;
  }
  // su(n) (AV1 §4.10.6): two's complement in n bits.
  BitWriter& su(int64_t value, unsigned bits) {
    return put(static_cast<uint64_t>(value) & ((uint64_t{1} << bits) - 1U), bits);
  }
  // ns(n) (AV1 §4.10.7): w = FloorLog2(n) + 1; m = (1 << w) - n; v < m is written in w - 1 bits, otherwise
  // (v + m) >> 1 in w - 1 bits followed by (v + m) & 1.
  BitWriter& ns(uint32_t v, uint32_t n) {
    unsigned w = 0;
    for (uint32_t x = n; x != 0; x >>= 1U) {
      ++w;
    }
    const uint32_t m = (1U << w) - n;
    if (v < m) {
      return put(v, w - 1U);
    }
    put((v + m) >> 1U, w - 1U);
    return put((v + m) & 1U, 1);
  }
  // read_delta_q() (AV1 §5.9.13): delta_coded f(1), delta_q su(1+6).
  BitWriter& delta_q(int32_t v) {
    if (v == 0) {
      return put(0, 1);
    }
    put(1, 1);
    return su(v, 7);
  }
  // AV1 trailing_bits(): a one bit, then zeros to the byte boundary.
  BitWriter& trailing() {
    put(1, 1);
    while (nbits_ % 8 != 0) {
      put(0, 1);
    }
    return *this;
  }
  [[nodiscard]] const Bytes& bytes() const { return bytes_; }
  [[nodiscard]] unsigned bit_count() const { return nbits_; }

 private:
  Bytes bytes_;
  unsigned nbits_ = 0;
};

// ---- sequence headers the sub-parsers consult -----------------------------------------------------------------

// syn_av1_tiles_cdef_lr: trace_headers 45 frame_width_bits_minus_1 = 10, 49 frame_height_bits_minus_1 = 9,
// 53 max_frame_width_minus_1 = 1279, 64 max_frame_height_minus_1 = 719, 75 use_128x128_superblock = 1,
// 82 enable_order_hint = 1, 87 order_hint_bits_minus_1 = 6, 90 enable_superres = 0, 99 separate_uv_delta_q = 0.
SequenceHeader seq_720p_sb128() {
  SequenceHeader seq;
  seq.frame_width_bits_minus_1 = 10;
  seq.frame_height_bits_minus_1 = 9;
  seq.max_frame_width_minus_1 = 1279;
  seq.max_frame_height_minus_1 = 719;
  seq.max_frame_width = 1280;
  seq.max_frame_height = 720;
  seq.use_128x128_superblock = true;
  seq.enable_order_hint = true;
  seq.order_hint_bits_minus_1 = 6;
  seq.OrderHintBits = 7;
  seq.enable_superres = false;
  seq.color_config.NumPlanes = 3;
  seq.color_config.separate_uv_delta_q = false;
  return seq;
}

// A 352x288 sequence with 64x64 superblocks and superres enabled (the shape of the aom av1-1-b8-* vectors:
// trace_headers 45 frame_width_bits_minus_1 = 8, 49 frame_height_bits_minus_1 = 8, 53 max_frame_width_minus_1 =
// 351, 62 max_frame_height_minus_1 = 287, 72 use_128x128_superblock = 0), with enable_superres switched on.
SequenceHeader seq_cif_superres() {
  SequenceHeader seq;
  seq.frame_width_bits_minus_1 = 8;
  seq.frame_height_bits_minus_1 = 8;
  seq.max_frame_width_minus_1 = 351;
  seq.max_frame_height_minus_1 = 287;
  seq.max_frame_width = 352;
  seq.max_frame_height = 288;
  seq.use_128x128_superblock = false;
  seq.enable_superres = true;
  seq.color_config.NumPlanes = 3;
  return seq;
}

// ---- tree helpers --------------------------------------------------------------------------------------------

const SyntaxNodeData* find(const SyntaxNodeData* n, std::string_view name, int array_index = -1) {
  if (n == nullptr) {
    return nullptr;
  }
  if (n->name == name && (array_index < 0 || n->array_index == array_index)) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const SyntaxNodeData* f = find(c, name, array_index); f != nullptr) {
      return f;
    }
  }
  return nullptr;
}

const SyntaxNodeData& node(const SyntaxNodeBuffer& buf, std::string_view name, int array_index = -1) {
  const SyntaxNodeData* n = find(buf.root(), name, array_index);
  REQUIRE(n != nullptr);
  return *n;
}

// The value node of an array whose element wrapper carries the same name (begin_array_element(name, i)).
const SyntaxNodeData& leaf(const SyntaxNodeBuffer& buf, std::string_view name, int array_index) {
  const SyntaxNodeData& wrapper = node(buf, name, array_index);
  const SyntaxNodeData* n = wrapper.kind == ValueKind::None ? find(wrapper.first_child, name, array_index) : &wrapper;
  REQUIRE(n != nullptr);
  return *n;
}

std::vector<const DiagnosticData*> diags(const SyntaxNodeData* n, std::string_view code) {
  std::vector<const DiagnosticData*> out;
  if (n == nullptr) {
    return out;
  }
  for (const DiagnosticData* d = n->first_diag; d != nullptr; d = d->next) {
    if (d->code == code) {
      out.push_back(d);
    }
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    const auto sub = diags(c, code);
    out.insert(out.end(), sub.begin(), sub.end());
  }
  return out;
}

// Runs `parse` over `bytes` with a recording builder, returns the tree; `bits_read` receives the position.
template <class Parse>
SyntaxNodeBuffer with_tree(const Bytes& bytes, Parse&& parse, uint64_t* bits_read = nullptr, bool recording = true) {
  BitReader br(bytes);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  parse(br, b);
  if (bits_read != nullptr) {
    *bits_read = br.bit_pos();
  }
  return b.finish();
}

// ---- row 12 / 13: frame_size, superres_params, render_size ----------------------------------------------------

TEST_CASE("av1_frame_size_from_sequence_header", "[syntax][av1][fast]") {
  // frame_size_override_flag = 0: no bits for the size; superres disabled: no bits at all; render_size reads one 0.
  const SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(0, 1).trailing();  // render_and_frame_size_different = 0
  FrameSize fs;
  RenderSize rs;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        fs = parse_frame_size(br, b, seq, false);
        rs = parse_render_size(br, b, fs);
      },
      &bits);
  CHECK(bits == 1);
  CHECK(fs.size_from_sequence_header);
  CHECK(fs.frame_width_minus_1 == 1279);
  CHECK(fs.frame_height_minus_1 == 719);
  CHECK(fs.UpscaledWidth == 1280);
  CHECK(fs.FrameWidth == 1280);
  CHECK(fs.FrameHeight == 720);
  CHECK(fs.SuperresDenom == kSuperresNum);
  CHECK_FALSE(fs.use_superres);
  // AV1 §5.9.9: MiCols = 2 * ((1280 + 7) >> 3) = 320, MiRows = 2 * ((720 + 7) >> 3) = 180.
  CHECK(fs.MiCols == 320);
  CHECK(fs.MiRows == 180);
  CHECK(fs.span.bit_offset == 0);
  CHECK(fs.span.bit_length == 0);
  CHECK_FALSE(rs.render_and_frame_size_different);
  CHECK(rs.RenderWidth == 1280);
  CHECK(rs.RenderHeight == 720);
  CHECK(rs.span.bit_offset == 0);
  CHECK(rs.span.bit_length == 1);
  CHECK(node(tree, "frame_size").kind == ValueKind::None);
  CHECK(find(tree.root(), "frame_size_from_sequence_header") != nullptr);
  CHECK(node(tree, "render_and_frame_size_different").kind == ValueKind::Bool);
}

TEST_CASE("av1_frame_size_override_superres_and_render_size", "[syntax][av1][fast]") {
  // frame_size_override_flag = 1 on seq_cif_superres: frame_width_minus_1 f(9) = 351, frame_height_minus_1 f(9) =
  // 215 (a 352x216 frame), use_superres = 1, coded_denom f(3) = 3 → SuperresDenom = 12 (AV1 §5.9.8),
  // UpscaledWidth = 352, FrameWidth = (352 * 8 + 6) / 12 = 235 (2822 / 12 = 235.17), MiCols = 2 * ((235 + 7) >> 3)
  // = 60, MiRows = 2 * ((216 + 7) >> 3) = 54 (AV1 §5.9.9). render_and_frame_size_different = 1,
  // render_width_minus_1 f(16) = 319, render_height_minus_1 f(16) = 199 → 320x200.
  const SequenceHeader seq = seq_cif_superres();
  BitWriter w;
  w.put(351, 9).put(215, 9).put(1, 1).put(3, 3).put(1, 1).put(319, 16).put(199, 16).trailing();
  FrameSize fs;
  RenderSize rs;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        fs = parse_frame_size(br, b, seq, true);
        rs = parse_render_size(br, b, fs);
      },
      &bits);
  CHECK(bits == 9 + 9 + 1 + 3 + 1 + 32);
  CHECK_FALSE(fs.size_from_sequence_header);
  CHECK(fs.frame_width_minus_1 == 351);
  CHECK(fs.frame_height_minus_1 == 215);
  CHECK(fs.use_superres);
  CHECK(fs.coded_denom == 3);
  CHECK(fs.SuperresDenom == 12);
  CHECK(fs.UpscaledWidth == 352);
  CHECK(fs.FrameWidth == 235);
  CHECK(fs.FrameHeight == 216);
  CHECK(fs.MiCols == 60);
  CHECK(fs.MiRows == 54);
  CHECK(fs.span.bit_offset == 0);
  CHECK(fs.span.bit_length == 22);
  CHECK(rs.render_and_frame_size_different);
  CHECK(rs.render_width_minus_1 == 319);
  CHECK(rs.RenderWidth == 320);
  CHECK(rs.RenderHeight == 200);
  CHECK(rs.span.bit_offset == 22);
  CHECK(rs.span.bit_length == 33);
  const SyntaxNodeData& width = node(tree, "frame_width_minus_1");
  CHECK(width.uvalue == 351);
  CHECK(width.span.bit_length == 9);
  CHECK(node(tree, "coded_denom").uvalue == 3);
  CHECK(find(tree.root(), "superres_params") != nullptr);
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).empty());
}

TEST_CASE("av1_frame_size_exceeding_sequence_maximum_is_syn_av1_0015", "[syntax][av1][fast]") {
  // frame_width_minus_1 f(9) = 400 > max_frame_width_minus_1 = 351 (AV1 §5.9.5 conformance requirement). The
  // values stand; the finding is Diagnostic Error SYN-AV1-0015 (SPEC-ASSUMPTION, no §7.9 code yet).
  const SequenceHeader seq = seq_cif_superres();
  BitWriter w;
  w.put(400, 9).put(287, 9).put(0, 1).trailing();
  FrameSize fs;
  const SyntaxNodeBuffer tree =
      with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { fs = parse_frame_size(br, b, seq, true); });
  CHECK(fs.FrameWidth == 401);
  CHECK(fs.MiCols == 2 * ((401 + 7) >> 3));
  const auto found = diags(tree.root(), kDiagFrameHeaderRange);
  REQUIRE(found.size() == 1);
  CHECK(found[0]->severity == ovt::syntax::Severity::Error);
}

// ---- row 14: frame_size_with_refs ----------------------------------------------------------------------------

// Acceptance test 2 (hand-worked form, see the file comment): a frame whose size comes from a reference slot.
TEST_CASE("av1_frame_size_with_refs_found_ref", "[syntax][av1][fast]") {
  // Slot 2 holds a 352x288 frame rendered at 320x240 (RefUpscaledWidth = 352, RefFrameHeight = 288,
  // RefRenderWidth = 320, RefRenderHeight = 240). ref_frame_idx = {0, 1, 2, 3, 4, 5, 6}; found_ref[0] = 0,
  // found_ref[1] = 0, found_ref[2] = 1 → sizes of slot 2 (AV1 §5.9.7), then superres_params(): use_superres = 1,
  // coded_denom = 7 → SuperresDenom = 16, FrameWidth = (352 * 8 + 8) / 16 = 176 (AV1 §5.9.8), MiCols = 2 * ((176 +
  // 7) >> 3) = 44, MiRows = 2 * ((288 + 7) >> 3) = 72 (AV1 §5.9.9). RenderWidth / RenderHeight stay 320x240.
  const SequenceHeader seq = seq_cif_superres();
  DecoderState state;
  FrameState ref;
  ref.UpscaledWidth = 352;
  ref.FrameWidth = 352;
  ref.FrameHeight = 288;
  ref.RenderWidth = 320;
  ref.RenderHeight = 240;
  ref.MiCols = 88;
  ref.MiRows = 72;
  state.slot(2).save(ref, 7);
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(0, 1).put(0, 1).put(1, 1).put(1, 1).put(7, 3).trailing();
  FrameSize fs;
  RenderSize rs;
  FrameSizeWithRefs r;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) { r = parse_frame_size_with_refs(br, b, seq, state, idx, true, fs, rs); },
      &bits);
  CHECK(bits == 3 + 1 + 3);
  CHECK(r.found_ref_index == 2);
  CHECK(r.found_ref == std::array<bool, 7>{false, false, true, false, false, false, false});
  CHECK(r.span.bit_offset == 0);
  CHECK(r.span.bit_length == 7);
  CHECK(fs.UpscaledWidth == 352);
  CHECK(fs.FrameWidth == 176);
  CHECK(fs.FrameHeight == 288);
  CHECK(fs.MiCols == 44);
  CHECK(fs.MiRows == 72);
  CHECK(fs.use_superres);
  CHECK(fs.SuperresDenom == 16);
  CHECK(fs.frame_width_minus_1 == 351);
  CHECK(fs.frame_height_minus_1 == 287);
  CHECK(rs.RenderWidth == 320);
  CHECK(rs.RenderHeight == 240);
  CHECK(rs.render_and_frame_size_different);
  CHECK(leaf(tree, "found_ref", 2).bvalue);
  CHECK(find(tree.root(), "found_ref", 3) == nullptr);  // the loop breaks at the first found reference
  CHECK(find(tree.root(), "size_from_ref") != nullptr);
  CHECK(find(tree.root(), "frame_size") == nullptr);  // frame_size() is not read when a reference is found
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).empty());
}

TEST_CASE("av1_frame_size_with_refs_none_found_reads_frame_size", "[syntax][av1][fast]") {
  // Seven found_ref = 0, then frame_size() with frame_size_override_flag = 1: 319 / 239 (320x240), use_superres =
  // 0, render_size(): render_and_frame_size_different = 0 → RenderWidth = UpscaledWidth = 320.
  const SequenceHeader seq = seq_cif_superres();
  const DecoderState state;
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(0, 7).put(319, 9).put(239, 9).put(0, 1).put(0, 1).trailing();
  FrameSize fs;
  RenderSize rs;
  FrameSizeWithRefs r;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) { r = parse_frame_size_with_refs(br, b, seq, state, idx, true, fs, rs); },
      &bits);
  CHECK(bits == 7 + 9 + 9 + 1 + 1);
  CHECK(r.found_ref_index == -1);
  CHECK(fs.FrameWidth == 320);
  CHECK(fs.UpscaledWidth == 320);
  CHECK(fs.FrameHeight == 240);
  CHECK(fs.MiCols == 80);
  CHECK(fs.MiRows == 60);
  CHECK(rs.RenderWidth == 320);
  CHECK(rs.RenderHeight == 240);
  CHECK(find(tree.root(), "found_ref", 6) != nullptr);
  CHECK(find(tree.root(), "frame_size") != nullptr);
  CHECK(find(tree.root(), "render_size") != nullptr);
}

TEST_CASE("av1_frame_size_with_refs_empty_slot_is_syn_av1_0015", "[syntax][av1][fast]") {
  // found_ref[0] = 1 into slot 5, which is empty: Diagnostic Error SYN-AV1-0015 and the stale (zero) sizes.
  const SequenceHeader seq = seq_cif_superres();
  const DecoderState state;
  const RefFrameIdx idx{5, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(1, 1).put(0, 1).trailing();  // found_ref[0] = 1, use_superres = 0
  FrameSize fs;
  RenderSize rs;
  FrameSizeWithRefs r;
  const SyntaxNodeBuffer tree = with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) {
    r = parse_frame_size_with_refs(br, b, seq, state, idx, true, fs, rs);
  });
  CHECK(r.found_ref_index == 0);
  CHECK(fs.FrameWidth == 0);
  CHECK(fs.MiCols == 0);
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).size() == 1);
}

// ---- row 15: tile_info ----------------------------------------------------------------------------------------

TEST_CASE("av1_tile_log2", "[syntax][av1][fast]") {
  // AV1 §5.9.15 tile_log2(blkSize, target): smallest k with (blkSize << k) >= target.
  CHECK(tile_log2(1, 1) == 0);
  CHECK(tile_log2(1, 2) == 1);
  CHECK(tile_log2(1, 3) == 2);
  CHECK(tile_log2(1, 4) == 2);
  CHECK(tile_log2(1, 64) == 6);
  CHECK(tile_log2(32, 10) == 0);
  CHECK(tile_log2(32, 33) == 1);
  CHECK(tile_log2(576, 60) == 0);
  CHECK(tile_log2(64, 65) == 1);
  CHECK(tile_log2(1, 0) == 0);
  CHECK(tile_log2(0, 5) == 0);
}

// Acceptance test 3, first half, on the inline bits of syn_av1_tiles_cdef_lr's first frame header (the corpus
// test below re-runs it on the stream): trace_headers 50 uniform_tile_spacing_flag = 1, 51 tile_cols_log2 110 = 2
// (two increment_tile_cols_log2 = 1 then 0), 54 tile_rows_log2 10 = 1, 56 context_update_tile_id 110 = 6,
// 59 tile_size_bytes_minus1 01 = 1. Expected per §5.9.15 for 1280x720 with 128x128 superblocks (MiCols = 320,
// MiRows = 180, sbCols = 10, sbRows = 6, sbShift = 5): tileWidthSb = (10 + 3) >> 2 = 3 → MiColStarts = {0, 96,
// 192, 288, 320}; tileHeightSb = (6 + 1) >> 1 = 3 → MiRowStarts = {0, 96, 180}; TileCols = 4, TileRows = 2.
TEST_CASE("av1_tile_info_uniform_720p_sb128", "[syntax][av1][fast]") {
  const SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(1, 1).put(1, 1).put(1, 1).put(0, 1).put(1, 1).put(0, 1).put(6, 3).put(1, 2).trailing();
  TileInfo t;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree =
      with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { t = parse_tile_info(br, b, seq, 320, 180); }, &bits);
  CHECK(bits == 11);
  CHECK(t.uniform_tile_spacing_flag);
  CHECK(t.sbCols == 10);
  CHECK(t.sbRows == 6);
  CHECK(t.sbShift == 5);
  CHECK(t.minLog2TileCols == 0);
  CHECK(t.maxLog2TileCols == 4);
  CHECK(t.maxLog2TileRows == 3);
  CHECK(t.minLog2Tiles == 0);
  CHECK(t.TileColsLog2 == 2);
  CHECK(t.TileRowsLog2 == 1);
  CHECK(t.TileCols == 4);
  CHECK(t.TileRows == 2);
  CHECK(t.NumTiles == 8);
  CHECK(t.MiColStarts == std::vector<uint32_t>{0, 96, 192, 288, 320});
  CHECK(t.MiRowStarts == std::vector<uint32_t>{0, 96, 180});
  CHECK(t.context_update_tile_id == 6);
  CHECK(t.tile_size_bytes_minus_1 == 1);
  CHECK(t.TileSizeBytes == 2);
  CHECK(t.width_in_sbs_minus_1.empty());
  CHECK(t.span.bit_offset == 0);
  CHECK(t.span.bit_length == 11);
  // Tile rectangles in luma samples (MiColStarts * 4): the last column is 128 wide (288..320 MI), the last row
  // 336 high (96..180 MI).
  const TileRect r00 = t.tile_rect(0, 0);
  CHECK(r00.x == 0);
  CHECK(r00.width == 384);
  CHECK(r00.height == 384);
  const TileRect r13 = t.tile_rect(1, 3);
  CHECK(r13.x == 1152);
  CHECK(r13.y == 384);
  CHECK(r13.width == 128);
  CHECK(r13.height == 336);
  CHECK(t.tile_rect(2, 0).width == 0);
  // Tree: the increment flags, context_update_tile_id with its 3-bit span, derived nodes.
  CHECK(node(tree, "uniform_tile_spacing_flag").bvalue);
  const SyntaxNodeData& ctx = node(tree, "context_update_tile_id");
  CHECK(ctx.uvalue == 6);
  CHECK(ctx.span.bit_offset == 6);
  CHECK(ctx.span.bit_length == 3);
  REQUIRE(find(tree.root(), "MiColStarts") != nullptr);
  REQUIRE(find(tree.root(), "MiRowStarts") != nullptr);
  CHECK(find(tree.root(), "MiColStarts")->derived == "MiColStarts = {0, 96, 192, 288, 320}");
  CHECK(find(tree.root(), "MiRowStarts")->derived == "MiRowStarts = {0, 96, 180}");
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).empty());
}

// Acceptance test 3, second half: uniform_tile_spacing_flag = 0 with tile widths {2, 5, 3} superblocks on the
// same sequence header. §5.9.15: maxTileWidthSb = 4096 >> 7 = 32; width_in_sbs_minus_1 = ns(min(10, 32)) = 1,
// ns(min(8, 32)) = 4, ns(min(3, 32)) = 2 → MiColStarts = {0, 64, 224, 320}, TileCols = 3, TileColsLog2 =
// tile_log2(1, 3) = 2, widestTileSb = 5; maxTileAreaSb = 60 (minLog2Tiles = 0), maxTileHeightSb = max(60 / 5, 1)
// = 12; one tile row: height_in_sbs_minus_1 = ns(min(6, 12)) = 5 → MiRowStarts = {0, 180}, TileRows = 1,
// TileRowsLog2 = 0; context_update_tile_id f(2) = 2, tile_size_bytes_minus_1 f(2) = 3.
TEST_CASE("av1_tile_info_explicit_spacing_720p_sb128", "[syntax][av1][fast]") {
  const SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(0, 1).ns(1, 10).ns(4, 8).ns(2, 3).ns(5, 6).put(2, 2).put(3, 2).trailing();
  TileInfo t;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree =
      with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { t = parse_tile_info(br, b, seq, 320, 180); }, &bits);
  // 1 + ns(10) 3 bits + ns(8) 3 bits + ns(3) 2 bits + ns(6) 3 bits + 2 + 2.
  CHECK(bits == 16);
  CHECK_FALSE(t.uniform_tile_spacing_flag);
  CHECK(t.MiColStarts == std::vector<uint32_t>{0, 64, 224, 320});
  CHECK(t.MiRowStarts == std::vector<uint32_t>{0, 180});
  CHECK(t.width_in_sbs_minus_1 == std::vector<uint32_t>{1, 4, 2});
  CHECK(t.height_in_sbs_minus_1 == std::vector<uint32_t>{5});
  CHECK(t.widestTileSb == 5);
  CHECK(t.TileCols == 3);
  CHECK(t.TileRows == 1);
  CHECK(t.TileColsLog2 == 2);
  CHECK(t.TileRowsLog2 == 0);
  CHECK(t.NumTiles == 3);
  CHECK(t.context_update_tile_id == 2);
  CHECK(t.tile_size_bytes_minus_1 == 3);
  CHECK(t.TileSizeBytes == 4);
  const SyntaxNodeData& w1 = leaf(tree, "width_in_sbs_minus_1", 1);
  CHECK(w1.uvalue == 4);
  CHECK(w1.descriptor == "ns(8)");
  CHECK(leaf(tree, "height_in_sbs_minus_1", 0).uvalue == 5);
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).empty());
}

TEST_CASE("av1_tile_info_single_tile_and_minimum_columns", "[syntax][av1][fast]") {
  const SequenceHeader cif = seq_cif_superres();
  SECTION("352x288 with 64x64 superblocks, one tile") {
    // MiCols = 88, MiRows = 72, sbCols = 6, sbRows = 5, sbShift = 4; minLog2TileCols = tile_log2(64, 6) = 0,
    // maxLog2TileCols = tile_log2(1, 6) = 3, maxLog2TileRows = tile_log2(1, 5) = 3; uniform = 1, increments 0, 0 →
    // TileCols = TileRows = 1, nothing more is read (context_update_tile_id = 0).
    BitWriter w;
    w.put(1, 1).put(0, 1).put(0, 1).trailing();
    TileInfo t;
    uint64_t bits = 0;
    (void)with_tree(
        w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { t = parse_tile_info(br, b, cif, 88, 72); }, &bits);
    CHECK(bits == 3);
    CHECK(t.sbCols == 6);
    CHECK(t.sbRows == 5);
    CHECK(t.TileCols == 1);
    CHECK(t.TileRows == 1);
    CHECK(t.NumTiles == 1);
    CHECK(t.MiColStarts == std::vector<uint32_t>{0, 88});
    CHECK(t.MiRowStarts == std::vector<uint32_t>{0, 72});
    CHECK(t.context_update_tile_id == 0);
    CHECK(t.TileSizeBytes == 4);
  }
  SECTION("a 8192-wide frame forces at least two tile columns") {
    // 64x64 superblocks: maxTileWidthSb = 64, sbCols = (2048 + 15) >> 4 = 128 → minLog2TileCols = tile_log2(64,
    // 128) = 1: TileColsLog2 starts at 1 and the first bit read is increment_tile_cols_log2 for the step to 2.
    SequenceHeader wide = cif;
    wide.max_frame_width_minus_1 = 8191;
    wide.max_frame_width = 8192;
    BitWriter w;
    w.put(1, 1).put(0, 1).put(0, 1).put(0, 1).put(0, 2).trailing();  // uniform, no col increment, no row
                                                                     // increment, context_update_tile_id f(1),
                                                                     // tile_size_bytes_minus_1 f(2) = 0
    TileInfo t;
    (void)with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { t = parse_tile_info(br, b, wide, 2048, 72); });
    CHECK(t.minLog2TileCols == 1);
    CHECK(t.TileColsLog2 == 1);
    CHECK(t.TileCols == 2);
    CHECK(t.MiColStarts == std::vector<uint32_t>{0, 1024, 2048});
    CHECK(t.TileSizeBytes == 1);
  }
}

TEST_CASE("av1_tile_info_explicit_spacing_over_64_columns_is_syn_av1_0015", "[syntax][av1][fast]") {
  // 64x64 superblocks, MiCols = 2048 (sbCols = 128): 128 explicit one-superblock columns exceed MAX_TILE_COLS.
  // Each width_in_sbs_minus_1 = ns(min(128 - startSb, 64)) = 0.
  SequenceHeader wide = seq_cif_superres();
  wide.max_frame_width_minus_1 = 8191;
  wide.max_frame_width = 8192;
  BitWriter w;
  w.put(0, 1);
  for (uint32_t startSb = 0; startSb < 128; ++startSb) {
    w.ns(0, std::min(128U - startSb, 64U));
  }
  // rows: sbRows = 5; minLog2Tiles = max(1, tile_log2(1024, 640)) = 1 → maxTileAreaSb = 640 >> 2 = 160,
  // maxTileHeightSb = max(160 / 1, 1) = 160; one row: ns(min(5, 160)) = 4.
  w.ns(4, 5);
  w.put(0, 7).put(0, 2).trailing();  // context_update_tile_id f(7 + 0), tile_size_bytes_minus_1
  TileInfo t;
  const SyntaxNodeBuffer tree =
      with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { t = parse_tile_info(br, b, wide, 2048, 72); });
  CHECK(t.TileCols == 128);
  CHECK(t.TileColsLog2 == 7);
  CHECK(t.TileRows == 1);
  CHECK(t.MiColStarts.size() == 129);
  CHECK(t.MiColStarts[128] == 2048);
  CHECK(t.MiColStarts[1] == 16);
  CHECK(t.NumTiles == 128);
  const auto found = diags(tree.root(), kDiagFrameHeaderRange);
  REQUIRE(found.size() == 1);
  CHECK(found[0]->message == "TileCols = 128 exceeds 64");
}

TEST_CASE("av1_tile_info_recording_off_returns_same_values", "[syntax][av1][fast]") {
  const SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(0, 1).ns(1, 10).ns(4, 8).ns(2, 3).ns(5, 6).put(2, 2).put(3, 2).trailing();
  TileInfo on;
  TileInfo off;
  (void)with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { on = parse_tile_info(br, b, seq, 320, 180); });
  (void)with_tree(
      w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { off = parse_tile_info(br, b, seq, 320, 180); }, nullptr, false);
  CHECK(on.MiColStarts == off.MiColStarts);
  CHECK(on.MiRowStarts == off.MiRowStarts);
  CHECK(on.NumTiles == off.NumTiles);
  CHECK(on.context_update_tile_id == off.context_update_tile_id);
  CHECK(on.span.bit_length == off.span.bit_length);
}

// ---- row 16: quantization_params ------------------------------------------------------------------------------

TEST_CASE("av1_quantization_params_separate_uv_and_qmatrix", "[syntax][av1][fast]") {
  // NumPlanes = 3, separate_uv_delta_q = 1: base_q_idx f(8) = 100, DeltaQYDc = delta_coded 1 su(1+6) = -5,
  // diff_uv_delta = 1, DeltaQUDc = 3, DeltaQUAc = 0 (delta_coded 0), DeltaQVDc = -2, DeltaQVAc = 7,
  // using_qmatrix = 1, qm_y = 4, qm_u = 5, qm_v = 6.
  ColorConfig cc;
  cc.NumPlanes = 3;
  cc.separate_uv_delta_q = true;
  BitWriter w;
  w.put(100, 8).delta_q(-5).put(1, 1).delta_q(3).delta_q(0).delta_q(-2).delta_q(7).put(1, 1).put(4, 4).put(5, 4).put(6,
                                                                                                                     4);
  w.trailing();
  QuantizationParams q;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree =
      with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { q = parse_quantization_params(br, b, cc); }, &bits);
  CHECK(bits == 8 + 8 + 1 + 8 + 1 + 8 + 8 + 1 + 12);
  CHECK(q.base_q_idx == 100);
  CHECK(q.DeltaQYDc == -5);
  CHECK(q.diff_uv_delta);
  CHECK(q.DeltaQUDc == 3);
  CHECK(q.DeltaQUAc == 0);
  CHECK(q.DeltaQVDc == -2);
  CHECK(q.DeltaQVAc == 7);
  CHECK(q.using_qmatrix);
  CHECK(q.qm_y == 4);
  CHECK(q.qm_u == 5);
  CHECK(q.qm_v == 6);
  CHECK(q.span.bit_offset == 0);
  CHECK(q.span.bit_length == bits);
  CHECK(node(tree, "base_q_idx").uvalue == 100);
  const SyntaxNodeData& ydc = node(tree, "DeltaQYDc");
  CHECK(ydc.ivalue == -5);
  CHECK(ydc.span.bit_length == 8);
  CHECK(node(tree, "qm_v").uvalue == 6);
}

TEST_CASE("av1_quantization_params_shared_uv_and_monochrome", "[syntax][av1][fast]") {
  SECTION("separate_uv_delta_q = 0 copies U into V and qm_u into qm_v") {
    ColorConfig cc;
    cc.NumPlanes = 3;
    cc.separate_uv_delta_q = false;
    BitWriter w;
    w.put(12, 8).delta_q(0).delta_q(4).delta_q(-1).put(1, 1).put(9, 4).put(2, 4).trailing();
    QuantizationParams q;
    const SyntaxNodeBuffer tree =
        with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { q = parse_quantization_params(br, b, cc); });
    CHECK(q.base_q_idx == 12);
    CHECK_FALSE(q.diff_uv_delta);
    CHECK(q.DeltaQUDc == 4);
    CHECK(q.DeltaQUAc == -1);
    CHECK(q.DeltaQVDc == 4);
    CHECK(q.DeltaQVAc == -1);
    CHECK(q.qm_y == 9);
    CHECK(q.qm_u == 2);
    CHECK(q.qm_v == 2);
    CHECK(find(tree.root(), "diff_uv_delta") == nullptr);
    REQUIRE(find(tree.root(), "qm_v") != nullptr);  // derived node
    CHECK(find(tree.root(), "qm_v")->kind == ValueKind::None);
  }
  SECTION("NumPlanes = 1 reads only the luma delta") {
    ColorConfig cc;
    cc.NumPlanes = 1;
    cc.mono_chrome = true;
    BitWriter w;
    w.put(255, 8).delta_q(63).put(0, 1).trailing();
    QuantizationParams q;
    uint64_t bits = 0;
    (void)with_tree(
        w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { q = parse_quantization_params(br, b, cc); }, &bits);
    CHECK(bits == 8 + 8 + 1);
    CHECK(q.base_q_idx == 255);
    CHECK(q.DeltaQYDc == 63);
    CHECK(q.DeltaQUDc == 0);
    CHECK(q.DeltaQVAc == 0);
    CHECK_FALSE(q.using_qmatrix);
  }
}

// ---- row 17: segmentation_params ------------------------------------------------------------------------------

TEST_CASE("av1_segmentation_params_update_data_with_clipping", "[syntax][av1][fast]") {
  // primary_ref_frame = PRIMARY_REF_NONE: update_map = 1, temporal_update = 0, update_data = 1 inferred. Tables:
  // segment 0: ALT_Q enabled su(1+8) = -256 → clipped to -255 (Segmentation_Feature_Max[0] = 255); segment 1:
  // ALT_LF_Y_H enabled su(1+6) = 40 (limit 63, kept), REF_FRAME enabled f(3) = 5; segment 3: SKIP enabled (0
  // bits); every other feature_enabled = 0. Derived: LastActiveSegId = 3, SegIdPreSkip = 1 (REF_FRAME and SKIP are
  // >= SEG_LVL_REF_FRAME).
  BitWriter w;
  w.put(1, 1);  // segmentation_enabled
  // segment 0
  w.put(1, 1).su(-256, 9);
  w.put(0, 7);
  // segment 1
  w.put(0, 1);
  w.put(0, 1);
  w.put(1, 1).su(40, 7);
  w.put(0, 1).put(0, 1);
  w.put(1, 1).put(5, 3);
  w.put(0, 1).put(0, 1);
  // segment 2
  w.put(0, 8);
  // segment 3
  w.put(0, 6).put(1, 1).put(0, 1);
  // segments 4..7
  w.put(0, 32);
  w.trailing();
  SegmentationParams seg;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        seg = parse_segmentation_params(br, b, kPrimaryRefNone, FeatureEnabledTable{}, FeatureDataTable{});
      },
      &bits);
  CHECK(bits == 1 + 64 + 9 + 7 + 3);
  CHECK(seg.segmentation_enabled);
  CHECK(seg.segmentation_update_map);
  CHECK_FALSE(seg.segmentation_temporal_update);
  CHECK(seg.segmentation_update_data);
  CHECK(seg.FeatureEnabled[0][kSegLvlAltQ]);
  CHECK(seg.FeatureData[0][kSegLvlAltQ] == -255);
  CHECK(seg.FeatureEnabled[1][2]);
  CHECK(seg.FeatureData[1][2] == 40);
  CHECK(seg.FeatureEnabled[1][kSegLvlRefFrame]);
  CHECK(seg.FeatureData[1][kSegLvlRefFrame] == 5);
  CHECK(seg.FeatureEnabled[3][kSegLvlSkip]);
  CHECK(seg.FeatureData[3][kSegLvlSkip] == 0);
  CHECK_FALSE(seg.FeatureEnabled[2][0]);
  CHECK(seg.LastActiveSegId == 3);
  CHECK(seg.SegIdPreSkip);
  CHECK(seg.span.bit_length == bits);
  // Tree: the feature_value of segment 0 feature 0 is su(1+8) with the raw -256 and the enum name on the flag.
  const SyntaxNodeData& seg0 = node(tree, "segment", 0);
  const SyntaxNodeData* f0 = find(&seg0, "feature", 0);
  REQUIRE(f0 != nullptr);
  const SyntaxNodeData* enabled = find(f0, "feature_enabled");
  REQUIRE(enabled != nullptr);
  CHECK(enabled->enum_name == "SEG_LVL_ALT_Q");
  const SyntaxNodeData* value = find(f0, "feature_value");
  REQUIRE(value != nullptr);
  CHECK(value->ivalue == -256);
  CHECK(value->descriptor == "su(1+8)");
  CHECK(find(tree.root(), "segmentation_update_map") == nullptr);  // inferred, derived node instead
  CHECK(find(tree.root(), "segmentation_update") != nullptr);
}

TEST_CASE("av1_segmentation_params_update_data_zero_keeps_previous_tables", "[syntax][av1][fast]") {
  // primary_ref_frame = 2: segmentation_update_map f(1) = 1, segmentation_temporal_update f(1) = 1,
  // segmentation_update_data f(1) = 0 → the load_previous() tables stay.
  FeatureEnabledTable prev_enabled{};
  FeatureDataTable prev_data{};
  prev_enabled[4][kSegLvlAltQ] = true;
  prev_data[4][kSegLvlAltQ] = -20;
  prev_enabled[6][kSegLvlGlobalMv] = true;
  BitWriter w;
  w.put(1, 1).put(1, 1).put(1, 1).put(0, 1).trailing();
  SegmentationParams seg;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) { seg = parse_segmentation_params(br, b, 2, prev_enabled, prev_data); },
      &bits);
  CHECK(bits == 4);
  CHECK(seg.segmentation_update_map);
  CHECK(seg.segmentation_temporal_update);
  CHECK_FALSE(seg.segmentation_update_data);
  CHECK(seg.FeatureEnabled == prev_enabled);
  CHECK(seg.FeatureData == prev_data);
  CHECK(seg.LastActiveSegId == 6);
  CHECK(seg.SegIdPreSkip);
  SECTION("update_map = 0 skips temporal_update") {
    BitWriter w2;
    w2.put(1, 1).put(0, 1).put(0, 1).trailing();
    uint64_t bits2 = 0;
    (void)with_tree(
        w2.bytes(),
        [&](BitReader& br, SyntaxBuilder& b) { seg = parse_segmentation_params(br, b, 0, prev_enabled, prev_data); },
        &bits2);
    CHECK(bits2 == 3);
    CHECK_FALSE(seg.segmentation_update_map);
    CHECK_FALSE(seg.segmentation_temporal_update);
  }
}

TEST_CASE("av1_segmentation_params_disabled_zeroes_tables", "[syntax][av1][fast]") {
  FeatureEnabledTable prev_enabled{};
  prev_enabled[1][1] = true;
  FeatureDataTable prev_data{};
  prev_data[1][1] = 9;
  BitWriter w;
  w.put(0, 1).trailing();
  SegmentationParams seg;
  (void)with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) {
    seg = parse_segmentation_params(br, b, 0, prev_enabled, prev_data);
  });
  CHECK_FALSE(seg.segmentation_enabled);
  CHECK(seg.FeatureEnabled == FeatureEnabledTable{});
  CHECK(seg.FeatureData == FeatureDataTable{});
  CHECK(seg.LastActiveSegId == 0);
  CHECK_FALSE(seg.SegIdPreSkip);
  CHECK(segmentation_feature_name(0) == "SEG_LVL_ALT_Q");
  CHECK(segmentation_feature_name(7) == "SEG_LVL_GLOBALMV");
  CHECK(segmentation_feature_name(8).empty());
}

// ---- rows 18 / 19: delta_q_params, delta_lf_params ------------------------------------------------------------

TEST_CASE("av1_delta_q_and_delta_lf_params", "[syntax][av1][fast]") {
  SECTION("base_q_idx = 0 reads nothing; delta_lf follows delta_q_present") {
    BitWriter w;
    w.put(0, 1).trailing();
    DeltaQParams dq;
    DeltaLfParams dlf;
    uint64_t bits = 0;
    const SyntaxNodeBuffer tree = with_tree(
        w.bytes(),
        [&](BitReader& br, SyntaxBuilder& b) {
          dq = parse_delta_q_params(br, b, 0);
          dlf = parse_delta_lf_params(br, b, dq.delta_q_present, false);
        },
        &bits);
    CHECK(bits == 0);
    CHECK_FALSE(dq.delta_q_present);
    CHECK(dq.delta_q_res == 0);
    CHECK_FALSE(dlf.delta_lf_present);
    CHECK(find(tree.root(), "delta_q_params") != nullptr);
    CHECK(find(tree.root(), "delta_lf_params") != nullptr);
    CHECK(node(tree, "delta_q_params").span.bit_length == 0);
  }
  SECTION("delta_q_present with delta_q_res, delta_lf_present with res and multi") {
    // delta_q_present = 1, delta_q_res f(2) = 2; delta_lf_present = 1, delta_lf_res f(2) = 3, delta_lf_multi = 1.
    BitWriter w;
    w.put(1, 1).put(2, 2).put(1, 1).put(3, 2).put(1, 1).trailing();
    DeltaQParams dq;
    DeltaLfParams dlf;
    uint64_t bits = 0;
    (void)with_tree(
        w.bytes(),
        [&](BitReader& br, SyntaxBuilder& b) {
          dq = parse_delta_q_params(br, b, 36);
          dlf = parse_delta_lf_params(br, b, dq.delta_q_present, false);
        },
        &bits);
    CHECK(bits == 7);
    CHECK(dq.delta_q_present);
    CHECK(dq.delta_q_res == 2);
    CHECK(dq.span.bit_length == 3);
    CHECK(dlf.delta_lf_present);
    CHECK(dlf.delta_lf_res == 3);
    CHECK(dlf.delta_lf_multi);
    CHECK(dlf.span.bit_offset == 3);
    CHECK(dlf.span.bit_length == 4);
  }
  SECTION("allow_intrabc suppresses delta_lf_present") {
    BitWriter w;
    w.put(1, 1).put(0, 2).trailing();
    DeltaQParams dq;
    DeltaLfParams dlf;
    uint64_t bits = 0;
    (void)with_tree(
        w.bytes(),
        [&](BitReader& br, SyntaxBuilder& b) {
          dq = parse_delta_q_params(br, b, 1);
          dlf = parse_delta_lf_params(br, b, dq.delta_q_present, true);
        },
        &bits);
    CHECK(bits == 3);
    CHECK(dq.delta_q_present);
    CHECK_FALSE(dlf.delta_lf_present);
    CHECK(dlf.span.bit_length == 0);
  }
}

// ---- get_qindex and the lossless derivations ------------------------------------------------------------------

TEST_CASE("av1_get_qindex_and_lossless", "[syntax][av1][fast]") {
  QuantizationParams q;
  SegmentationParams seg;
  SECTION("base_q_idx = 0 without deltas is CodedLossless and AllLossless") {
    const Lossless l = compute_lossless(q, seg, true);
    CHECK(l.CodedLossless);
    CHECK(l.AllLossless);
    CHECK(l.LosslessArray == std::array<bool, 8>{true, true, true, true, true, true, true, true});
    CHECK_FALSE(compute_lossless(q, seg, false).AllLossless);  // superres: FrameWidth != UpscaledWidth
  }
  SECTION("a chroma delta breaks losslessness") {
    q.DeltaQUAc = 1;
    const Lossless l = compute_lossless(q, seg, true);
    CHECK_FALSE(l.CodedLossless);
    CHECK_FALSE(l.LosslessArray[0]);
  }
  SECTION("ALT_Q feature per segment: get_qindex clips and selects") {
    // AV1 §7.12.2: segment 2 adds -10 to base_q_idx 10 → 0 (lossless), segment 3 adds +300 → clipped to 255,
    // segment 0 has no feature → base_q_idx.
    q.base_q_idx = 10;
    seg.segmentation_enabled = true;
    seg.FeatureEnabled[2][kSegLvlAltQ] = true;
    seg.FeatureData[2][kSegLvlAltQ] = -10;
    seg.FeatureEnabled[3][kSegLvlAltQ] = true;
    seg.FeatureData[3][kSegLvlAltQ] = 255;  // the parser clips to 255; +300 cannot be stored
    CHECK(get_qindex(true, 0, q, seg, 0) == 10);
    CHECK(get_qindex(true, 2, q, seg, 0) == 0);
    CHECK(get_qindex(true, 3, q, seg, 0) == 255);
    CHECK(get_qindex(false, 2, q, seg, 50) == 40);    // CurrentQIndex + delta
    CHECK(get_qindex(false, 0, q, seg, 50) == 50);    // no feature: CurrentQIndex
    CHECK(get_qindex(false, 3, q, seg, 250) == 255);  // clipped
    const Lossless l = compute_lossless(q, seg, true);
    CHECK_FALSE(l.CodedLossless);
    CHECK(l.LosslessArray[2]);
    CHECK_FALSE(l.LosslessArray[3]);
    CHECK_FALSE(l.AllLossless);
    // SegQMLevel only with using_qmatrix: 15 for the lossless segment, qm_y / qm_u / qm_v otherwise.
    CHECK(l.SegQMLevel[0][2] == 0);
    q.using_qmatrix = true;
    q.qm_y = 3;
    q.qm_u = 4;
    q.qm_v = 5;
    const Lossless lq = compute_lossless(q, seg, true);
    CHECK(lq.SegQMLevel[0][2] == kQmLevelLossless);
    CHECK(lq.SegQMLevel[2][2] == kQmLevelLossless);
    CHECK(lq.SegQMLevel[0][0] == 3);
    CHECK(lq.SegQMLevel[1][0] == 4);
    CHECK(lq.SegQMLevel[2][3] == 5);
  }
  SECTION("segmentation disabled ignores the tables") {
    q.base_q_idx = 10;
    seg.segmentation_enabled = false;
    seg.FeatureEnabled[2][kSegLvlAltQ] = true;
    seg.FeatureData[2][kSegLvlAltQ] = -10;
    CHECK(get_qindex(true, 2, q, seg, 0) == 10);
  }
}

// ---- corpus (acceptance test 3 on the stream; the chained rows 15–19 of its first frame header) ---------------

Bytes read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Calls `cb` with every OBU of every IVF frame (32-byte file header, 12-byte frame headers, little-endian).
void for_each_ivf_obu(const Bytes& file, const std::function<void(std::span<const uint8_t>)>& cb) {
  REQUIRE(file.size() >= 32);
  const size_t header_size = file[6] | (static_cast<size_t>(file[7]) << 8U);
  size_t pos = header_size;
  while (pos + 12 <= file.size()) {
    const size_t frame_size = file[pos] | (static_cast<size_t>(file[pos + 1]) << 8U) |
                              (static_cast<size_t>(file[pos + 2]) << 16U) | (static_cast<size_t>(file[pos + 3]) << 24U);
    pos += 12;
    REQUIRE(pos + frame_size <= file.size());
    const std::span<const uint8_t> frame(file.data() + pos, frame_size);
    size_t off = 0;
    while (off < frame.size()) {
      BitReader br(frame.subspan(off));
      SyntaxBuilder b(br);
      b.set_recording(false);
      const ObuHeader h = parse_obu_header(br, b);
      const size_t total = (h.header_bits / 8) + h.obu_size;
      REQUIRE(off + total <= frame.size());
      cb(frame.subspan(off, total));
      off += total;
    }
    pos += frame_size;
  }
}

// syn_av1_tiles_cdef_lr, first OBU_FRAME (obu_size 3-byte leb128 → the payload starts at trace bit 32). trace_headers:
// 32 show_existing_frame 0, 33 frame_type 00, 35 show_frame 1, 36 disable_cdf_update 0, 37
// allow_screen_content_tools 1, 38 force_integer_mv 0, 39 frame_size_override_flag 0, 40 order_hint 0000000,
// 47 render_and_frame_size_different 0, 48 allow_intrabc 0, 49 disable_frame_end_update_cdf 0, 50
// uniform_tile_spacing_flag 1 … 59 tile_size_bytes_minus1 01 = 1, 61 base_q_idx 00100100 = 36, 69/70/71
// delta_coded 0, 72 using_qmatrix 0, 73 segmentation_enabled 0, 74 delta_q_present 0, 75 loop_filter_level[0].
// The 18 bits before tile_info() (show_existing_frame … disable_frame_end_update_cdf) are skipped here; WI-1.117b
// parses them.
TEST_CASE("av1_tile_info_corpus_syn_av1_tiles_cdef_lr", "[syntax][av1][fast][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("syn_av1_tiles_cdef_lr");
  const Bytes file = read_file(path);
  SequenceHeader seq;
  bool have_seq = false;
  bool done = false;
  for_each_ivf_obu(file, [&](std::span<const uint8_t> unit) {
    if (done) {
      return;
    }
    BitReader br(unit);
    SyntaxBuilder b(br);
    const ObuHeader h = parse_obu_header(br, b);
    if (h.type() == ObuType::SequenceHeader) {
      seq = parse_sequence_header(br, b);
      have_seq = true;
      return;
    }
    if (h.type() != ObuType::Frame) {
      return;
    }
    REQUIRE(have_seq);
    CHECK(seq.use_128x128_superblock);
    CHECK(seq.max_frame_width == 1280);
    CHECK(seq.max_frame_height == 720);
    REQUIRE(h.header_bits == 32);
    br.skip_bits(18);
    const FrameSize fs = [&] {
      FrameSize f;
      f.frame_width_minus_1 = seq.max_frame_width_minus_1;
      f.frame_height_minus_1 = seq.max_frame_height_minus_1;
      f.FrameWidth = seq.max_frame_width;
      f.UpscaledWidth = seq.max_frame_width;
      f.FrameHeight = seq.max_frame_height;
      compute_image_size(f);
      return f;
    }();
    CHECK(fs.MiCols == 320);
    CHECK(fs.MiRows == 180);
    const TileInfo t = parse_tile_info(br, b, seq, fs.MiCols, fs.MiRows);
    CHECK(t.span.bit_offset == 50);
    CHECK(t.span.bit_length == 11);
    CHECK(t.uniform_tile_spacing_flag);
    CHECK(t.TileCols == 4);
    CHECK(t.TileRows == 2);
    CHECK(t.MiColStarts == std::vector<uint32_t>{0, 96, 192, 288, 320});
    CHECK(t.MiRowStarts == std::vector<uint32_t>{0, 96, 180});
    CHECK(t.context_update_tile_id == 6);
    CHECK(t.TileSizeBytes == 2);
    const QuantizationParams q = parse_quantization_params(br, b, seq.color_config);
    CHECK(q.span.bit_offset == 61);
    CHECK(q.base_q_idx == 36);
    CHECK(q.DeltaQYDc == 0);
    CHECK(q.DeltaQVAc == 0);
    CHECK_FALSE(q.using_qmatrix);
    const SegmentationParams seg =
        parse_segmentation_params(br, b, kPrimaryRefNone, FeatureEnabledTable{}, FeatureDataTable{});
    CHECK(seg.span.bit_offset == 73);
    CHECK_FALSE(seg.segmentation_enabled);
    const DeltaQParams dq = parse_delta_q_params(br, b, q.base_q_idx);
    CHECK(dq.span.bit_offset == 74);
    CHECK_FALSE(dq.delta_q_present);
    const DeltaLfParams dlf = parse_delta_lf_params(br, b, dq.delta_q_present, false);
    CHECK_FALSE(dlf.delta_lf_present);
    CHECK(br.bit_pos() == 75);  // loop_filter_level[0] follows (WI-1.118)
    const Lossless l = compute_lossless(q, seg, fs.FrameWidth == fs.UpscaledWidth);
    CHECK_FALSE(l.CodedLossless);
    done = true;
  });
  REQUIRE(done);
}

}  // namespace
