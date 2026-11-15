// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the AV1 uncompressed_header() sub-structures of chapter 7 §7.3.6 rows 20–27 (WI-1.118 parts a and b):
// loop_filter_params, cdef_params, lr_params, read_tx_mode, frame_reference_mode, skip_mode_params,
// global_motion_params (with the subexp decoding) and film_grain_params (with load_grain_params). The frame OBU,
// the tile group (rows 28–29) and the uncompressed_header() wiring are WI-1.118 part c.
//
// Expected values come from recorded oracle runs, cited at each fixture:
//   - FFmpeg 6.1.1 `-bsf:v trace_headers` (bit offsets counted from the first OBU header bit, as the trace prints
//     them) and GStreamer 1.24.2 codecparsers through tests/oracles/gst_parse_oracle.c (`--codec av1 --ivf`), which
//     prints the reconstructed values (gm_params, CdefDamping, the 3 → 4 secondary strengths, FrameRestorationType,
//     LoopRestorationSize, SkipModeFrame) that trace_headers leaves raw;
//   - or a hand-worked transcription of the AV1 §5.9.x equations given in the comments.
// The "excerpt" byte arrays are bits [a, b) of a real OBU, copied left-aligned (zero padded to a byte): the
// chapter 14 synthetic streams syn_av1_tiles_cdef_lr (recipe 13) and syn_av1_hidden_altref_showexisting (recipe
// 14) regenerate bit-exactly (sha256 equal to tests/corpora/manifest.json); syn_av1_globalmotion (recipe 74) was
// regenerated locally with sha256 26fb68f7…f5b7, which differs from the manifest's c92d15f7…81bf, so its excerpt
// stands on its own. Full-header parity over whole streams is part c (parse_frame_header) and WI-1.572.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <span>
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
  // ns(n) (AV1 §4.10.7).
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
  // A one bit and zeros to the byte boundary, so that over-reads are visible.
  BitWriter& trailing() {
    put(1, 1);
    while (nbits_ % 8 != 0) {
      put(0, 1);
    }
    return *this;
  }
  [[nodiscard]] const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
  unsigned nbits_ = 0;
};

// ---- sequence headers ----------------------------------------------------------------------------------------

// syn_av1_tiles_cdef_lr: trace_headers 75 use_128x128_superblock = 1, 82 enable_order_hint = 1, 87
// order_hint_bits_minus_1 = 6, enable_cdef = 1, enable_restoration = 1, 4:2:0 8-bit, film_grain_params_present = 0.
SequenceHeader seq_720p_sb128() {
  SequenceHeader seq;
  seq.max_frame_width = 1280;
  seq.max_frame_height = 720;
  seq.use_128x128_superblock = true;
  seq.enable_order_hint = true;
  seq.order_hint_bits_minus_1 = 6;
  seq.OrderHintBits = 7;
  seq.enable_cdef = true;
  seq.enable_restoration = true;
  seq.color_config.NumPlanes = 3;
  return seq;
}

// av1-1-b8-23-film_grain-50: trace_headers 72 use_128x128_superblock = 0, 84 order_hint_bits_minus_1 = 6, 88
// enable_cdef = 1, 89 enable_restoration = 1, 91 mono_chrome = 0, 97 film_grain_params_present = 1; 352x288 4:2:0.
SequenceHeader seq_cif_film_grain() {
  SequenceHeader seq;
  seq.max_frame_width = 352;
  seq.max_frame_height = 288;
  seq.use_128x128_superblock = false;
  seq.enable_order_hint = true;
  seq.order_hint_bits_minus_1 = 6;
  seq.OrderHintBits = 7;
  seq.enable_cdef = true;
  seq.enable_restoration = true;
  seq.film_grain_params_present = true;
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

int count_named(const SyntaxNodeData* n, std::string_view name) {
  if (n == nullptr) {
    return 0;
  }
  int c = n->name == name ? 1 : 0;
  for (const SyntaxNodeData* k = n->first_child; k != nullptr; k = k->next_sibling) {
    c += count_named(k, name);
  }
  return c;
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

// The check_* helpers below take `first_bit`: the trace_headers offset of the reader's bit 0 (0 for a whole OBU, the
// excerpt start for an excerpt), so that the comparisons read against the trace.

// Runs `parse` over `bytes` with a builder, returns the tree; `bits_read` receives the position.
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

Bytes read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE(in.good());
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The `index`-th OBU_FRAME / OBU_FRAME_HEADER of an IVF file, header byte included.
Bytes nth_frame_obu(const Bytes& file, int index) {
  REQUIRE(file.size() >= 32);
  const size_t header_size = file[6] | (static_cast<size_t>(file[7]) << 8U);
  size_t pos = header_size;
  int n = -1;
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
      if (h.type() == ObuType::Frame || h.type() == ObuType::FrameHeader) {
        if (++n == index) {
          return {frame.begin() + static_cast<std::ptrdiff_t>(off),
                  frame.begin() + static_cast<std::ptrdiff_t>(off + total)};
        }
      }
      off += total;
    }
    pos += frame_size;
  }
  FAIL("frame OBU not found");
  return {};
}

// ---- row 20: loop_filter_params --------------------------------------------------------------------------------

TEST_CASE("av1_loop_filter_params_lossless_and_intrabc_defaults", "[syntax][av1][fast]") {
  // AV1 §5.9.11: CodedLossless || allow_intrabc reads nothing; levels 0 and the default deltas replace whatever the
  // primary reference had.
  const SequenceHeader seq = seq_720p_sb128();
  const std::array<int8_t, kTotalRefsPerFrame> prev{5, 5, 5, 5, 5, 5, 5, 5};
  const std::array<int8_t, 2> prev_mode{3, 3};
  for (const bool lossless : {true, false}) {
    BitWriter w;
    w.put(0xFF, 8);
    LoopFilterParams lf;
    uint64_t bits = 99;
    const SyntaxNodeBuffer tree = with_tree(
        w.bytes(),
        [&](BitReader& br, SyntaxBuilder& b) {
          lf = parse_loop_filter_params(br, b, seq.color_config, lossless, !lossless, prev, prev_mode);
        },
        &bits);
    CHECK(bits == 0);
    CHECK_FALSE(lf.coded);
    CHECK(lf.loop_filter_level == std::array<uint8_t, 4>{0, 0, 0, 0});
    CHECK(lf.loop_filter_ref_deltas == kDefaultLoopFilterRefDeltas);
    CHECK(lf.loop_filter_mode_deltas == std::array<int8_t, 2>{0, 0});
    CHECK(lf.span.bit_length == 0);
    CHECK(find(tree.root(), "loop_filter_level") == nullptr);
  }
}

TEST_CASE("av1_loop_filter_params_delta_update", "[syntax][av1][fast]") {
  // Levels {10, 0} (NumPlanes 3: [2] and [3] are read because [0] != 0), sharpness 5, delta_enabled 1,
  // delta_update 1; update_ref_delta set for INTRA (+7) and ALTREF (-63); update_mode_delta set for [1] (-2).
  // The other deltas keep the previous values {1,0,0,0,-1,0,-1,-1} / {4, 4}.
  const SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(10, 6).put(0, 6).put(12, 6).put(13, 6).put(5, 3).put(1, 1).put(1, 1);
  w.put(1, 1).su(7, 7);
  for (int i = 1; i < 7; ++i) {
    w.put(0, 1);
  }
  w.put(1, 1).su(-63, 7);
  w.put(0, 1).put(1, 1).su(-2, 7).trailing();
  LoopFilterParams lf;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        lf = parse_loop_filter_params(br, b, seq.color_config, false, false, kDefaultLoopFilterRefDeltas, {4, 4});
      },
      &bits);
  CHECK(bits == 6 * 4 + 3 + 2 + 8 + 7 + 7 + 2 + 7);
  CHECK(lf.coded);
  CHECK(lf.loop_filter_level == std::array<uint8_t, 4>{10, 0, 12, 13});
  CHECK(lf.loop_filter_sharpness == 5);
  CHECK(lf.loop_filter_delta_enabled);
  CHECK(lf.loop_filter_delta_update);
  CHECK(lf.update_ref_delta == std::array<bool, 8>{true, false, false, false, false, false, false, true});
  CHECK(lf.loop_filter_ref_deltas == std::array<int8_t, 8>{7, 0, 0, 0, -1, 0, -1, -63});
  CHECK(lf.update_mode_delta == std::array<bool, 2>{false, true});
  CHECK(lf.loop_filter_mode_deltas == std::array<int8_t, 2>{4, -2});
  CHECK(count_named(tree.root(), "update_ref_delta") == 8);
  const SyntaxNodeData* d = find(tree.root(), "loop_filter_ref_deltas");
  REQUIRE(d != nullptr);
  CHECK(d->ivalue == 7);
  CHECK(d->descriptor == "su(1+6)");
  CHECK(lf.span.bit_length == bits);
}

TEST_CASE("av1_loop_filter_params_monochrome_and_zero_levels", "[syntax][av1][fast]") {
  // [2] / [3] are read only when NumPlanes > 1 and [0] || [1]: neither for monochrome, nor for zero levels.
  SequenceHeader mono = seq_720p_sb128();
  mono.color_config.NumPlanes = 1;
  BitWriter w;
  w.put(9, 6).put(9, 6).put(0, 3).put(0, 1).trailing();
  LoopFilterParams lf;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        lf = parse_loop_filter_params(br, b, mono.color_config, false, false, kDefaultLoopFilterRefDeltas, {0, 0});
      },
      &bits);
  CHECK(bits == 16);
  CHECK(lf.loop_filter_level == std::array<uint8_t, 4>{9, 9, 0, 0});
  CHECK_FALSE(lf.loop_filter_delta_enabled);

  const SequenceHeader seq = seq_720p_sb128();
  BitWriter z;
  z.put(0, 6).put(0, 6).put(7, 3).put(0, 1).trailing();
  (void)with_tree(
      z.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        lf = parse_loop_filter_params(br, b, seq.color_config, false, false, kDefaultLoopFilterRefDeltas, {0, 0});
      },
      &bits);
  CHECK(bits == 16);
  CHECK(lf.loop_filter_sharpness == 7);
}

// ---- row 21: cdef_params ---------------------------------------------------------------------------------------

TEST_CASE("av1_cdef_params_skip_branches", "[syntax][av1][fast]") {
  SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(0xFF, 8);
  for (int c = 0; c < 3; ++c) {
    seq.enable_cdef = c != 2;
    CdefParams cd;
    uint64_t bits = 99;
    (void)with_tree(
        w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { cd = parse_cdef_params(br, b, seq, c == 0, c == 1); }, &bits);
    CHECK(bits == 0);
    CHECK_FALSE(cd.coded);
    CHECK(cd.cdef_bits == 0);
    CHECK(cd.CdefDamping == 3);
    CHECK(cd.cdef_y_pri_strength[0] == 0);
    CHECK(cd.cdef_uv_sec_strength[0] == 0);
  }
}

TEST_CASE("av1_cdef_params_secondary_strength_3_is_4_and_monochrome", "[syntax][av1][fast]") {
  // cdef_damping_minus_3 = 3 → CdefDamping 6; cdef_bits = 1; i = 0: y_pri 15, y_sec 3 → 4, uv_pri 1, uv_sec 3 → 4;
  // i = 1: 2, 1, 9, 2. Monochrome: the uv strengths are not read.
  const SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(3, 2).put(1, 2).put(15, 4).put(3, 2).put(1, 4).put(3, 2).put(2, 4).put(1, 2).put(9, 4).put(2, 2).trailing();
  CdefParams cd;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { cd = parse_cdef_params(br, b, seq, false, false); }, &bits);
  CHECK(bits == 28);
  CHECK(cd.coded);
  CHECK(cd.CdefDamping == 6);
  CHECK(cd.cdef_bits == 1);
  CHECK(cd.cdef_y_pri_strength[0] == 15);
  CHECK(cd.cdef_y_sec_strength[0] == 4);
  CHECK(cd.cdef_uv_pri_strength[0] == 1);
  CHECK(cd.cdef_uv_sec_strength[0] == 4);
  CHECK(cd.cdef_y_pri_strength[1] == 2);
  CHECK(cd.cdef_y_sec_strength[1] == 1);
  CHECK(cd.cdef_uv_pri_strength[1] == 9);
  CHECK(cd.cdef_uv_sec_strength[1] == 2);
  const SyntaxNodeData* sec = find(tree.root(), "cdef_y_sec_strength");
  REQUIRE(sec != nullptr);
  CHECK(sec->uvalue == 3);  // the node keeps the coded value; the struct holds the mapped one

  SequenceHeader mono = seq;
  mono.color_config.NumPlanes = 1;
  BitWriter m;
  m.put(0, 2).put(0, 2).put(4, 4).put(3, 2).trailing();
  (void)with_tree(
      m.bytes(), [&](BitReader& br, SyntaxBuilder& b) { cd = parse_cdef_params(br, b, mono, false, false); }, &bits);
  CHECK(bits == 10);
  CHECK(cd.cdef_y_pri_strength[0] == 4);
  CHECK(cd.cdef_y_sec_strength[0] == 4);
  CHECK(cd.cdef_uv_pri_strength[0] == 0);
}

// ---- row 22: lr_params -----------------------------------------------------------------------------------------

TEST_CASE("av1_lr_params_skip_branches_and_count_units", "[syntax][av1][fast]") {
  SequenceHeader seq = seq_720p_sb128();
  BitWriter w;
  w.put(0xFF, 8);
  for (int c = 0; c < 3; ++c) {
    seq.enable_restoration = c != 2;
    LrParams lr;
    uint64_t bits = 99;
    (void)with_tree(
        w.bytes(),
        [&](BitReader& br, SyntaxBuilder& b) { lr = parse_lr_params(br, b, seq, c == 0, c == 1, 1280, 720); }, &bits);
    CHECK(bits == 0);
    CHECK_FALSE(lr.coded);
    CHECK_FALSE(lr.UsesLr);
    CHECK(lr.FrameRestorationType[0] == RestorationType::None);
  }
  // count_units_in_frame (AV1 §5.11.57): Max((frameSize + unitSize / 2) / unitSize, 1).
  CHECK(count_units_in_frame(64, 288) == 5);  // (288 + 32) / 64
  CHECK(count_units_in_frame(256, 100) == 1);
  CHECK(count_units_in_frame(256, 0) == 1);
  CHECK(count_units_in_frame(0, 100) == 1);
  CHECK(restoration_type_name(RestorationType::Sgrproj) == "RESTORE_SGRPROJ");
  CHECK(restoration_type_name(kRemapLrType[1]) == "RESTORE_SWITCHABLE");
}

TEST_CASE("av1_lr_params_sb64_extra_shift_and_uv_shift", "[syntax][av1][fast]") {
  // 64x64 superblocks: lr_type {2 → WIENER, 1 → SWITCHABLE, 0 → NONE}; lr_unit_shift f(1) = 1, lr_unit_extra_shift
  // f(1) = 1 → 2 → LoopRestorationSize[0] = 256 >> 0 = 256; usesChromaLr with 4:2:0 → lr_uv_shift f(1) = 1 → 128.
  // Unit grid on 352x288: Y (288 + 128) / 256 = 1 row, (352 + 128) / 256 = 1 col; U: 144 / 176 with 128 → 1 x 1.
  SequenceHeader seq = seq_cif_film_grain();
  BitWriter w;
  w.put(2, 2).put(1, 2).put(0, 2).put(1, 1).put(1, 1).put(1, 1).trailing();
  LrParams lr;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { lr = parse_lr_params(br, b, seq, false, false, 352, 288); },
      &bits);
  CHECK(bits == 9);
  CHECK(lr.FrameRestorationType[0] == RestorationType::Wiener);
  CHECK(lr.FrameRestorationType[1] == RestorationType::Switchable);
  CHECK(lr.FrameRestorationType[2] == RestorationType::None);
  CHECK(lr.UsesLr);
  CHECK(lr.usesChromaLr);
  CHECK(lr.lr_unit_extra_shift);
  CHECK(lr.lr_unit_shift == 2);
  CHECK(lr.lr_uv_shift == 1);
  CHECK(lr.LoopRestorationSize == std::array<uint16_t, 3>{256, 128, 128});
  CHECK(lr.LrUnitRows == std::array<uint32_t, 3>{1, 1, 0});
  CHECK(lr.LrUnitCols == std::array<uint32_t, 3>{1, 1, 0});

  // lr_unit_shift = 0 reads no extra bit (64 → LoopRestorationSize 64); luma-only LR reads no lr_uv_shift; 4:4:4
  // never reads it.
  BitWriter z;
  z.put(3, 2).put(0, 2).put(0, 2).put(0, 1).trailing();
  (void)with_tree(
      z.bytes(), [&](BitReader& br, SyntaxBuilder& b) { lr = parse_lr_params(br, b, seq, false, false, 352, 288); },
      &bits);
  CHECK(bits == 7);
  CHECK(lr.FrameRestorationType[0] == RestorationType::Sgrproj);
  CHECK(lr.lr_unit_shift == 0);
  CHECK(lr.LoopRestorationSize == std::array<uint16_t, 3>{64, 64, 64});
  CHECK(lr.LrUnitRows[0] == 5);  // (288 + 32) / 64
  CHECK(lr.LrUnitCols[0] == 6);  // (352 + 32) / 64

  seq.color_config.subsampling_x = false;
  seq.color_config.subsampling_y = false;
  BitWriter f;
  f.put(0, 2).put(2, 2).put(0, 2).put(1, 1).put(0, 1).trailing();
  (void)with_tree(
      f.bytes(), [&](BitReader& br, SyntaxBuilder& b) { lr = parse_lr_params(br, b, seq, false, false, 352, 288); },
      &bits);
  CHECK(bits == 8);
  CHECK(lr.lr_unit_shift == 1);
  CHECK(lr.lr_uv_shift == 0);
  CHECK(lr.LoopRestorationSize == std::array<uint16_t, 3>{128, 128, 128});
}

// ---- rows 23–25: tx mode, reference mode, skip mode ------------------------------------------------------------

TEST_CASE("av1_tx_mode_and_frame_reference_mode", "[syntax][av1][fast]") {
  BitWriter w;
  w.put(1, 1).put(0, 1).put(1, 1).trailing();
  TxModeParams lossless;
  TxModeParams select;
  TxModeParams largest;
  bool ref_intra = true;
  bool ref_inter = false;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        lossless = parse_tx_mode(br, b, true);
        select = parse_tx_mode(br, b, false);
        largest = parse_tx_mode(br, b, false);
        ref_intra = parse_frame_reference_mode(br, b, true);
        ref_inter = parse_frame_reference_mode(br, b, false);
      },
      &bits);
  CHECK(bits == 3);
  CHECK(lossless.TxMode == TxModeType::Only4x4);
  CHECK(lossless.span.bit_length == 0);
  CHECK(select.TxMode == TxModeType::Select);
  CHECK(largest.TxMode == TxModeType::Largest);
  CHECK_FALSE(ref_intra);
  CHECK(ref_inter);
  CHECK(tx_mode_name(TxModeType::Select) == "TX_MODE_SELECT");
}

// A DecoderState whose slot ref_frame_idx[i] holds RefOrderHint hints[i].
DecoderState state_with_hints(const RefFrameIdx& idx, const std::array<uint32_t, kRefsPerFrame>& hints) {
  DecoderState st;
  for (size_t i = 0; i < idx.size(); ++i) {
    FrameState fs;
    fs.OrderHint = hints[i];
    st.slot(idx[i]).save(fs, static_cast<int64_t>(i));
  }
  return st;
}

TEST_CASE("av1_skip_mode_params_excerpt_syn_av1_hidden_altref_showexisting", "[syntax][av1][fast]") {
  // syn_av1_hidden_altref_showexisting (recipe 14), frame header 9 (OBU at IVF frame 16): trace_headers 33
  // order_hint = 6, 188 tx_mode 1 (= TX_MODE_SELECT), 189 reference_select 1, 190 skip_mode_present 1.
  // gst_parse_oracle: ref_frame_idx {3, 6, 4, 0, 2, 5, 1}, order_hints[LAST..ALTREF] {4, 3, 2, 0, 8, 1, 16},
  // skip_mode_frame {1, 5}. §5.9.22: the nearest forward hint is 4 (i = 0, LAST), the nearest backward 8 (i = 4,
  // BWDREF) → SkipModeFrame {LAST_FRAME, BWDREF_FRAME}. Excerpt: bits [188, 191).
  const Bytes excerpt{0xE0};
  const SequenceHeader seq = seq_720p_sb128();
  const RefFrameIdx idx{3, 6, 4, 0, 2, 5, 1};
  const DecoderState st = state_with_hints(idx, {4, 3, 2, 0, 8, 1, 16});
  TxModeParams tx;
  bool reference_select = false;
  SkipModeParams sm;
  uint64_t bits = 0;
  (void)with_tree(
      excerpt,
      [&](BitReader& br, SyntaxBuilder& b) {
        tx = parse_tx_mode(br, b, false);
        reference_select = parse_frame_reference_mode(br, b, false);
        sm = parse_skip_mode_params(br, b, seq, st, idx, 6, false, reference_select);
      },
      &bits);
  CHECK(bits == 3);
  CHECK(tx.TxMode == TxModeType::Select);
  CHECK(reference_select);
  CHECK(sm.skipModeAllowed);
  CHECK(sm.forwardIdx == 0);
  CHECK(sm.backwardIdx == 4);
  CHECK(sm.SkipModeFrame == std::array<int8_t, 2>{kLastFrame, kBwdrefFrame});
  CHECK(sm.skip_mode_present);
}

TEST_CASE("av1_skip_mode_params_second_forward_and_not_allowed", "[syntax][av1][fast]") {
  // No backward reference: OrderHint 10, hints {9, 9, 7, 9, 5, 9, 9} → forwardIdx 0 (hint 9, the first of the
  // equal ones), secondForwardIdx 2 (hint 7, the nearest before 9) → SkipModeFrame {LAST, LAST3}.
  const SequenceHeader seq = seq_720p_sb128();
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  const DecoderState st = state_with_hints(idx, {9, 9, 7, 9, 5, 9, 9});
  BitWriter w;
  w.put(0, 1).trailing();
  SkipModeParams sm;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) { sm = parse_skip_mode_params(br, b, seq, st, idx, 10, false, true); },
      &bits);
  CHECK(bits == 1);
  CHECK(sm.skipModeAllowed);
  CHECK(sm.forwardIdx == 0);
  CHECK(sm.backwardIdx == -1);
  CHECK(sm.secondForwardIdx == 2);
  CHECK(sm.SkipModeFrame == std::array<int8_t, 2>{kLastFrame, kLast3Frame});
  CHECK_FALSE(sm.skip_mode_present);

  // Every reference at the same hint: no second forward → not allowed, nothing read. Wrap-around: OrderHint 1 with
  // 7-bit hints 127 (= -2 away) and 3 (+2): forward 127, backward 3 → allowed.
  const DecoderState same = state_with_hints(idx, {9, 9, 9, 9, 9, 9, 9});
  const DecoderState wrap = state_with_hints(idx, {127, 127, 127, 127, 127, 127, 3});
  SkipModeParams sm_same;
  SkipModeParams sm_wrap;
  SkipModeParams sm_intra;
  SkipModeParams sm_noref;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        sm_same = parse_skip_mode_params(br, b, seq, same, idx, 10, false, true);
        sm_intra = parse_skip_mode_params(br, b, seq, same, idx, 10, true, true);
        sm_noref = parse_skip_mode_params(br, b, seq, same, idx, 10, false, false);
        sm_wrap = parse_skip_mode_params(br, b, seq, wrap, idx, 1, false, true);
      },
      &bits);
  CHECK(bits == 1);
  CHECK_FALSE(sm_same.skipModeAllowed);
  CHECK(sm_same.SkipModeFrame == std::array<int8_t, 2>{kNoneFrame, kNoneFrame});
  CHECK_FALSE(sm_intra.skipModeAllowed);
  CHECK_FALSE(sm_noref.skipModeAllowed);
  CHECK(sm_wrap.skipModeAllowed);
  CHECK(sm_wrap.SkipModeFrame == std::array<int8_t, 2>{kLastFrame, kAltrefFrame});
  CHECK_FALSE(sm_wrap.skip_mode_present);  // the one bit of the writer is 0
}

// ---- row 26: global_motion_params ------------------------------------------------------------------------------

TEST_CASE("av1_inverse_recenter", "[syntax][av1][fast]") {
  // AV1 §5.9.29
  CHECK(inverse_recenter(4096, 142) == 4167);  // even v ≤ 2r: r + v / 2
  CHECK(inverse_recenter(4096, 91) == 4050);   // odd v: r - (v + 1) / 2
  CHECK(inverse_recenter(3, 9) == 9);          // v > 2r: v
  CHECK(inverse_recenter(0, 0) == 0);
}

// Acceptance test 3. syn_av1_globalmotion (recipe 74, regenerated: see the file comment), frame header 1 (the first
// inter frame), OBU_FRAME of IVF frame 1: trace_headers 74 allow_high_precision_mv 1, primary_ref_frame 7 (so
// PrevGmParams = identity), 152 is_global[1] 1, 153 is_rot_zoom[1] 1, then subexp codes 142 (idx 2), 91 (3),
// 202 (0), 2387 (1), 212..217 is_global[2..7] 0. gst_parse_oracle (GstAV1GlobalMotionParams): gm_type[1] = 2
// (ROTZOOM), gm_params[1] = {103424, -1222656, 65678, -92, 92, 65678}. Hand check of idx 2: absBits 12, precBits 15,
// precDiff 1, r = (65536 >> 1) - 32768 = 0, decode_unsigned_subexp_with_ref(8193, 4096) = inverse_recenter(4096,
// 142) = 4167 → 4167 - 4096 = 71 → (71 << 1) + 65536 = 65678. Excerpt: bits [152, 218).
TEST_CASE("av1_global_motion_params_excerpt_syn_av1_globalmotion", "[syntax][av1][fast]") {
  const Bytes excerpt{0xFE, 0x1D, 0xE6, 0xFE, 0x95, 0xFF, 0x15, 0x30, 0x00};
  GlobalMotionParams g;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      excerpt,
      [&](BitReader& br, SyntaxBuilder& b) {
        g = parse_global_motion_params(br, b, false, true, identity_gm_params());
      },
      &bits);
  CHECK(bits == 218 - 152);
  CHECK(g.span.bit_length == 66);
  CHECK(g.is_global[kLastFrame]);
  CHECK(g.GmType[kLastFrame] == GlobalMotionType::RotZoom);
  CHECK(g.gm_params[kLastFrame] == std::array<int32_t, 6>{103424, -1222656, 65678, -92, 92, 65678});
  for (int ref = kLast2Frame; ref <= kAltrefFrame; ++ref) {
    CAPTURE(ref);
    CHECK_FALSE(g.is_global[static_cast<size_t>(ref)]);
    CHECK(g.GmType[static_cast<size_t>(ref)] == GlobalMotionType::Identity);
    CHECK(g.gm_params[static_cast<size_t>(ref)] == std::array<int32_t, 6>{0, 0, 65536, 0, 0, 65536});
  }
  // The subexp syntax elements as trace_headers prints them: 5, 4, 5 and 9 set subexp_more_bits, each closed by a
  // zero one, then subexp_bits.
  CHECK(count_named(tree.root(), "subexp_more_bits") == 5 + 1 + 4 + 1 + 5 + 1 + 9 + 1);
  CHECK(count_named(tree.root(), "subexp_bits") == 4);
  const SyntaxNodeData* sb = find(tree.root(), "subexp_bits");
  REQUIRE(sb != nullptr);
  CHECK(sb->uvalue == 14);  // trace_headers 160 subexp_bits 0001110 = 14 (idx 2: 128 + 14 = 142)
  CHECK(sb->span.bit_offset == 160 - 152);

  // Recording off returns the same values.
  GlobalMotionParams quiet;
  (void)with_tree(
      excerpt,
      [&](BitReader& br, SyntaxBuilder& b) {
        quiet = parse_global_motion_params(br, b, false, true, identity_gm_params());
      },
      nullptr, false);
  CHECK(quiet.gm_params == g.gm_params);
}

TEST_CASE("av1_global_motion_params_translation_affine_and_intra", "[syntax][av1][fast]") {
  // LAST: TRANSLATION without allow_high_precision_mv: absBits 9 - 1 = 8, precBits 3 - 1 = 2, precDiff 14,
  // mx 256, numSyms 513, r = 0 - 0 = 0 → (r << 1) = 0 <= 513: inverse_recenter(256, v) with r - low = 256.
  //   idx 0 coded v = 10 (even ≤ 512): 256 + 5 = 261 → 261 - 256 = 5 → 5 << 14 = 81920.
  //   idx 1 coded v = 3 (odd): 256 - 2 = 254 → -2 → -2 << 14 = -32768.
  // decode_subexp(513) for v = 10: i = 0, b2 = 3, a = 8: 513 > 24 → more_bits 1 (mk 8); i = 1, b2 = 3: more_bits 0,
  // subexp_bits f(3) = 2 → 10. For v = 3: more_bits 0, subexp_bits f(3) = 3.
  // LAST2: AFFINE (is_rot_zoom 0, is_translation 0): idx 2..5 then 0, 1, every code 0 with r from PrevGmParams
  // identity: idx 2 r = 0, decode_unsigned_subexp_with_ref(8193, 4096) with v = 0 → 4096 → 0 → 65536; idx 3 / 4
  // r = 0 - 0 = 0 → 0; idx 5 like idx 2; idx 0 / 1 (absBits 12, precBits 6, precDiff 10): 0.
  // The others: is_global 0.
  BitWriter w;
  w.put(1, 1).put(0, 1).put(1, 1);  // LAST: is_global, is_rot_zoom 0, is_translation 1
  w.put(1, 1).put(0, 1).put(2, 3);  // idx 0: v = 10
  w.put(0, 1).put(3, 3);            // idx 1: v = 3
  w.put(1, 1).put(0, 1).put(0, 1);  // LAST2: AFFINE
  for (int i = 0; i < 6; ++i) {
    w.put(0, 1).put(0, 3);  // v = 0
  }
  for (int ref = kLast3Frame; ref <= kAltrefFrame; ++ref) {
    w.put(0, 1);
  }
  w.trailing();
  GlobalMotionParams g;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        g = parse_global_motion_params(br, b, false, false, identity_gm_params());
      },
      &bits);
  CHECK(bits == 3 + 5 + 4 + 3 + 24 + 5);
  CHECK(g.GmType[kLastFrame] == GlobalMotionType::Translation);
  CHECK(g.gm_params[kLastFrame] == std::array<int32_t, 6>{81920, -32768, 65536, 0, 0, 65536});
  CHECK(g.GmType[kLast2Frame] == GlobalMotionType::Affine);
  CHECK(g.gm_params[kLast2Frame] == std::array<int32_t, 6>{0, 0, 65536, 0, 0, 65536});
  CHECK(gm_type_name(GlobalMotionType::Affine) == "AFFINE");

  // FrameIsIntra: nothing read, identity everywhere even when PrevGmParams is not.
  GmParams prev = identity_gm_params();
  prev[kLastFrame][0] = 12345;
  GlobalMotionParams intra;
  (void)with_tree(
      w.bytes(), [&](BitReader& br, SyntaxBuilder& b) { intra = parse_global_motion_params(br, b, true, true, prev); },
      &bits);
  CHECK(bits == 0);
  CHECK(intra.gm_params == identity_gm_params());
}

TEST_CASE("av1_global_motion_params_reference_from_prev_gm_params", "[syntax][av1][fast]") {
  // ROTZOOM on LAST with PrevGmParams[LAST] = {0, 0, 65678, -92, 92, 65678} (the values of the excerpt above) and
  // every code v = 0: idx 2: r = (65678 >> 1) - 32768 = 71, r - low = 4167; (4167 << 1) = 8334 > 8193 → mx - 1 -
  // inverse_recenter(8192 - 4167 = 4025, 0) = 8192 - 4025 = 4167 → 71 → 65678 (a zero code repeats the reference).
  // idx 3: r = (-92 >> 1) - 0 = -46 → r - low = 4050 ≤ 4096: inverse_recenter(4050, 0) = 4050 → -46 → -92.
  // idx 0 / 1 (absBits 12, precBits 6, precDiff 10): r = 0 → 0.
  GmParams prev = identity_gm_params();
  prev[kLastFrame] = {0, 0, 65678, -92, 92, 65678};
  BitWriter w;
  w.put(1, 1).put(1, 1);
  for (int i = 0; i < 4; ++i) {
    w.put(0, 1).put(0, 3);
  }
  for (int ref = kLast2Frame; ref <= kAltrefFrame; ++ref) {
    w.put(0, 1);
  }
  w.trailing();
  GlobalMotionParams g;
  (void)with_tree(w.bytes(),
                  [&](BitReader& br, SyntaxBuilder& b) { g = parse_global_motion_params(br, b, false, true, prev); });
  CHECK(g.gm_params[kLastFrame] == std::array<int32_t, 6>{0, 0, 65678, -92, 92, 65678});
}

// ---- row 27: film_grain_params ---------------------------------------------------------------------------------

TEST_CASE("av1_film_grain_params_reset_branches", "[syntax][av1][fast]") {
  // Not present, or hidden and not showable: nothing read; apply_grain 0: one bit; all three reset (every field 0,
  // update_grain 0 included).
  SequenceHeader seq = seq_cif_film_grain();
  const DecoderState st;
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(0, 1).trailing();
  FilmGrainParams a;
  FilmGrainParams c;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        a = parse_film_grain_params(br, b, seq, st, idx, FrameType::Inter, false, false);
        c = parse_film_grain_params(br, b, seq, st, idx, FrameType::Inter, true, false);
      },
      &bits);
  CHECK(bits == 1);
  CHECK_FALSE(c.apply_grain);
  CHECK_FALSE(c.update_grain);
  FilmGrainParams zero;
  zero.update_grain = false;
  CHECK(a == zero);
  CHECK(c == zero);
  seq.film_grain_params_present = false;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        a = parse_film_grain_params(br, b, seq, st, idx, FrameType::Key, true, false);
      },
      &bits);
  CHECK(bits == 0);
  CHECK(a == zero);
}

// Acceptance test 2, second half (oracle substitution: no frame of av1-1-b8-23-film_grain-50 or of the b10 twin has
// update_grain = 0, and the chapter 14 recipes have none either, so the bits are hand-assembled from AV1 §5.9.30):
// an INTER frame with apply_grain 1, grain_seed 0xBEEF, update_grain 0, film_grain_params_ref_idx 5 copies slot 5's
// SavedFilmGrainParams and keeps its own grain_seed.
TEST_CASE("av1_film_grain_params_update_grain_0_loads_slot", "[syntax][av1][fast]") {
  const SequenceHeader seq = seq_cif_film_grain();
  DecoderState st;
  FrameState saved;
  saved.film_grain_params.apply_grain = true;
  saved.film_grain_params.grain_seed = 1111;
  saved.film_grain_params.num_y_points = 2;
  saved.film_grain_params.point_y_value = {16, 235};
  saved.film_grain_params.point_y_scaling = {40, 80};
  saved.film_grain_params.ar_coeff_lag = 1;
  saved.film_grain_params.overlap_flag = true;
  st.slot(5).save(saved, 3);
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(1, 1).put(0xBEEF, 16).put(0, 1).put(5, 3).trailing();
  FilmGrainParams fg;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        fg = parse_film_grain_params(br, b, seq, st, idx, FrameType::Inter, true, false);
      },
      &bits);
  CHECK(bits == 21);
  CHECK(fg.apply_grain);
  CHECK(fg.grain_seed == 0xBEEF);
  CHECK_FALSE(fg.update_grain);
  CHECK(fg.film_grain_params_ref_idx == 5);
  CHECK(fg.num_y_points == 2);
  CHECK(fg.point_y_value[1] == 235);
  CHECK(fg.point_y_scaling[1] == 80);
  CHECK(fg.ar_coeff_lag == 1);
  CHECK(fg.overlap_flag);
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).empty());
  CHECK(load_grain_params(st, 5).grain_seed == 1111);

  // A film_grain_params_ref_idx that is not one of ref_frame_idx[] is SYN-AV1-0015; the load still happens.
  const RefFrameIdx other{0, 1, 2, 3, 4, 6, 7};
  const SyntaxNodeBuffer bad = with_tree(w.bytes(), [&](BitReader& br, SyntaxBuilder& b) {
    fg = parse_film_grain_params(br, b, seq, st, other, FrameType::Inter, true, false);
  });
  CHECK(diags(bad.root(), kDiagFrameHeaderRange).size() == 1);
  CHECK(fg.num_y_points == 2);
}

TEST_CASE("av1_film_grain_params_conformance_diagnostics", "[syntax][av1][fast]") {
  // KEY frame (update_grain implied 1): num_y_points 15 (> 14) with values 0..14 except a repeat at i = 3; 4:2:0
  // chroma: num_cb_points 11 (> 10), num_cr_points 0 → four SYN-AV1-0015 findings; every value stands as read and
  // the parse stays in step: ar_coeff_lag 0 → numPosLuma 0, numPosChroma 1 → one cb coefficient, no cr ones.
  const SequenceHeader seq = seq_cif_film_grain();
  const DecoderState st;
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(1, 1).put(7, 16);
  w.put(15, 4);
  for (int i = 0; i < 15; ++i) {
    w.put(i == 3 ? 2U : static_cast<unsigned>(i), 8).put(64, 8);
  }
  w.put(0, 1);  // chroma_scaling_from_luma
  w.put(11, 4);
  for (int i = 0; i < 11; ++i) {
    w.put(static_cast<uint64_t>(i) * 20U, 8).put(32, 8);
  }
  w.put(0, 4);                            // num_cr_points
  w.put(2, 2).put(0, 2);                  // grain_scaling_minus_8, ar_coeff_lag
  w.put(140, 8);                          // ar_coeffs_cb_plus_128[0]
  w.put(1, 2).put(2, 2);                  // ar_coeff_shift_minus_6, grain_scale_shift
  w.put(128, 8).put(192, 8).put(256, 9);  // cb_mult, cb_luma_mult, cb_offset
  w.put(1, 1).put(1, 1).trailing();       // overlap_flag, clip_to_restricted_range
  FilmGrainParams fg;
  uint64_t bits = 0;
  const SyntaxNodeBuffer tree = with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        fg = parse_film_grain_params(br, b, seq, st, idx, FrameType::Key, true, false);
      },
      &bits);
  CHECK(bits == 1 + 16 + 4 + 15 * 16 + 1 + 4 + 11 * 16 + 4 + 4 + 8 + 4 + 25 + 2);
  CHECK(fg.num_y_points == 15);
  CHECK(fg.point_y_value[14] == 14);
  CHECK(fg.num_cb_points == 11);
  CHECK(fg.point_cb_value[10] == 200);
  CHECK(fg.num_cr_points == 0);
  CHECK(fg.ar_coeffs_cb_plus_128[0] == 140);
  CHECK(fg.cb_offset == 256);
  CHECK(fg.cr_offset == 0);
  CHECK(fg.clip_to_restricted_range);
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).size() == 4);
}

TEST_CASE("av1_film_grain_params_chroma_from_luma_and_monochrome", "[syntax][av1][fast]") {
  // chroma_scaling_from_luma 1: no chroma points, but cb and cr AR coefficients (numPosChroma = 2·1·2 + 1 = 5 with
  // luma points); no cb/cr mult fields. Monochrome: chroma_scaling_from_luma not read, no chroma at all.
  const SequenceHeader seq = seq_cif_film_grain();
  const DecoderState st;
  const RefFrameIdx idx{0, 1, 2, 3, 4, 5, 6};
  BitWriter w;
  w.put(1, 1).put(9, 16).put(1, 4).put(128, 8).put(50, 8).put(1, 1).put(0, 2).put(1, 2);
  for (int i = 0; i < 4 + 5 + 5; ++i) {
    w.put(128U + static_cast<unsigned>(i), 8);
  }
  w.put(0, 2).put(0, 2).put(0, 1).put(0, 1).trailing();
  FilmGrainParams fg;
  uint64_t bits = 0;
  (void)with_tree(
      w.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        fg = parse_film_grain_params(br, b, seq, st, idx, FrameType::IntraOnly, false, true);
      },
      &bits);
  CHECK(bits == 1 + 16 + 4 + 16 + 1 + 4 + 14 * 8 + 6);
  CHECK(fg.chroma_scaling_from_luma);
  CHECK(fg.num_cb_points == 0);
  CHECK(fg.ar_coeffs_y_plus_128[3] == 131);
  CHECK(fg.ar_coeffs_cb_plus_128[4] == 136);
  CHECK(fg.ar_coeffs_cr_plus_128[4] == 141);

  SequenceHeader mono = seq;
  mono.color_config.mono_chrome = true;
  mono.color_config.NumPlanes = 1;
  BitWriter m;
  m.put(1, 1).put(9, 16).put(0, 4).put(0, 2).put(0, 2).put(0, 2).put(0, 2).put(0, 1).put(0, 1).trailing();
  (void)with_tree(
      m.bytes(),
      [&](BitReader& br, SyntaxBuilder& b) {
        fg = parse_film_grain_params(br, b, mono, st, idx, FrameType::Key, true, false);
      },
      &bits);
  CHECK(bits == 1 + 16 + 4 + 8 + 2);
  CHECK_FALSE(fg.chroma_scaling_from_luma);
}

// ---- acceptance test 1 and the film grain corpus vector --------------------------------------------------------

// syn_av1_tiles_cdef_lr frame header 0 (KEY), trace_headers / gst_parse_oracle: 75 loop_filter_level {1, 1, 3, 1},
// 99 loop_filter_sharpness 0, 102 loop_filter_delta_enabled 1, 103 loop_filter_delta_update 0, 104
// cdef_damping_minus_3 0 (cdef_damping 3), 106 cdef_bits 2, y_pri {0, 4, 0, 8}, y_sec {0, 2, 0, 0}, uv_pri {2, 8, 0,
// 8}, uv_sec {2, 2, 0, 2}, 156/158/160 lr_type 0 (frame_restoration_type {0, 0, 0}), 162 tx_mode 1 (= 2,
// TX_MODE_SELECT), 163 reduced_tx_set 0; KEY frames read no reference_select / skip_mode / allow_warped_motion and no
// global motion; film_grain_params_present = 0.
void check_tiles_cdef_lr_frame0(BitReader& br, SyntaxBuilder& b, uint64_t first_bit) {
  const SequenceHeader seq = seq_720p_sb128();
  const LoopFilterParams lf =
      parse_loop_filter_params(br, b, seq.color_config, false, false, kDefaultLoopFilterRefDeltas, {0, 0});
  CHECK(lf.span.bit_offset + first_bit == 75);
  CHECK(lf.loop_filter_level == std::array<uint8_t, 4>{1, 1, 3, 1});
  CHECK(lf.loop_filter_delta_enabled);
  CHECK_FALSE(lf.loop_filter_delta_update);
  CHECK(lf.loop_filter_ref_deltas == kDefaultLoopFilterRefDeltas);
  const CdefParams cd = parse_cdef_params(br, b, seq, false, false);
  CHECK(cd.span.bit_offset + first_bit == 104);
  CHECK(cd.CdefDamping == 3);
  CHECK(cd.cdef_bits == 2);
  CHECK(cd.cdef_y_pri_strength == std::array<uint8_t, 8>{0, 4, 0, 8, 0, 0, 0, 0});
  CHECK(cd.cdef_y_sec_strength == std::array<uint8_t, 8>{0, 2, 0, 0, 0, 0, 0, 0});
  CHECK(cd.cdef_uv_pri_strength == std::array<uint8_t, 8>{2, 8, 0, 8, 0, 0, 0, 0});
  CHECK(cd.cdef_uv_sec_strength == std::array<uint8_t, 8>{2, 2, 0, 2, 0, 0, 0, 0});
  const LrParams lr = parse_lr_params(br, b, seq, false, false, 1280, 720);
  CHECK(lr.span.bit_offset + first_bit == 156);
  CHECK(lr.FrameRestorationType[0] == RestorationType::None);
  CHECK(lr.FrameRestorationType[1] == RestorationType::None);
  CHECK(lr.FrameRestorationType[2] == RestorationType::None);
  CHECK_FALSE(lr.UsesLr);
  CHECK(lr.LoopRestorationSize[0] == kRestorationTileSizeMax);
  const TxModeParams tx = parse_tx_mode(br, b, false);
  CHECK(tx.span.bit_offset + first_bit == 162);
  CHECK(tx.TxMode == TxModeType::Select);
  CHECK_FALSE(parse_frame_reference_mode(br, b, true));
  CHECK_FALSE(parse_skip_mode_params(br, b, seq, DecoderState{}, RefFrameIdx{}, 0, true, false).skipModeAllowed);
  CHECK_FALSE(b.read_flag("reduced_tx_set"));
  const GlobalMotionParams gm = parse_global_motion_params(br, b, true, false, identity_gm_params());
  CHECK(gm.span.bit_length == 0);
  const FilmGrainParams fg =
      parse_film_grain_params(br, b, seq, DecoderState{}, RefFrameIdx{}, FrameType::Key, true, false);
  CHECK_FALSE(fg.apply_grain);
  CHECK(br.bit_pos() + first_bit == 164);
}

// syn_av1_tiles_cdef_lr frame header 1 (INTER, primary_ref_frame 7), trace_headers / gst_parse_oracle: 74
// allow_high_precision_mv 1, 104 loop_filter_level {1, 1, 7, 5}, delta_enabled 1, update 0, 133
// cdef_damping_minus_3 1 (cdef_damping 4), cdef_bits 1, y_pri {0, 14}, y_sec {2, 2}, uv_pri {2, 14}, uv_sec {2, 2},
// 161 lr_type[0] 3 (frame_restoration_type {2, 0, 0}: SGRPROJ), 167 lr_unit_shift 2 (loop_restoration_size 256),
// 168 tx_mode 2, 169 reference_select 0, 170 allow_warped_motion 1, 171 reduced_tx_set 0, 172..178 is_global 0.
void check_tiles_cdef_lr_frame1(BitReader& br, SyntaxBuilder& b, uint64_t first_bit) {
  const SequenceHeader seq = seq_720p_sb128();
  const LoopFilterParams lf =
      parse_loop_filter_params(br, b, seq.color_config, false, false, kDefaultLoopFilterRefDeltas, {0, 0});
  CHECK(lf.span.bit_offset + first_bit == 104);
  CHECK(lf.loop_filter_level == std::array<uint8_t, 4>{1, 1, 7, 5});
  const CdefParams cd = parse_cdef_params(br, b, seq, false, false);
  CHECK(cd.span.bit_offset + first_bit == 133);
  CHECK(cd.CdefDamping == 4);
  CHECK(cd.cdef_bits == 1);
  CHECK(cd.cdef_y_pri_strength[1] == 14);
  CHECK(cd.cdef_y_sec_strength[0] == 2);
  CHECK(cd.cdef_uv_pri_strength[1] == 14);
  const LrParams lr = parse_lr_params(br, b, seq, false, false, 1280, 720);
  CHECK(lr.span.bit_offset + first_bit == 161);
  CHECK(lr.FrameRestorationType[0] == RestorationType::Sgrproj);
  CHECK(lr.FrameRestorationType[1] == RestorationType::None);
  CHECK(lr.lr_unit_shift == 2);
  CHECK(lr.lr_uv_shift == 0);
  CHECK(lr.LoopRestorationSize == std::array<uint16_t, 3>{256, 256, 256});
  CHECK(lr.LrUnitRows[0] == 3);  // (720 + 128) / 256
  CHECK(lr.LrUnitCols[0] == 5);  // (1280 + 128) / 256
  CHECK(parse_tx_mode(br, b, false).TxMode == TxModeType::Select);
  const bool reference_select = parse_frame_reference_mode(br, b, false);
  CHECK_FALSE(reference_select);
  const SkipModeParams sm =
      parse_skip_mode_params(br, b, seq, DecoderState{}, RefFrameIdx{0, 0, 0, 0, 0, 0, 0}, 1, false, reference_select);
  CHECK_FALSE(sm.skipModeAllowed);
  CHECK(b.read_flag("allow_warped_motion"));
  CHECK_FALSE(b.read_flag("reduced_tx_set"));
  const GlobalMotionParams gm = parse_global_motion_params(br, b, false, true, identity_gm_params());
  CHECK(gm.span.bit_offset + first_bit == 172);
  CHECK(gm.GmType == std::array<GlobalMotionType, 8>{});
  CHECK(br.bit_pos() + first_bit == 179);
}

TEST_CASE("av1_fh2_unit_excerpt_syn_av1_tiles_cdef_lr", "[syntax][av1][fast]") {
  // Acceptance test 1 on the excerpts: frame 0 bits [75, 164), frame 1 bits [104, 179).
  const Bytes f0{0x04, 0x10, 0xC1, 0x11, 0x00, 0x52, 0x51, 0x00, 0x04, 0x11, 0x01, 0x00};
  (void)with_tree(f0, [&](BitReader& br, SyntaxBuilder& b) { check_tiles_cdef_lr_frame0(br, b, 75); });
  const Bytes f1{0x04, 0x11, 0xC5, 0x12, 0x84, 0x57, 0x5D, 0x61, 0xA0, 0x00};
  (void)with_tree(f1, [&](BitReader& br, SyntaxBuilder& b) { check_tiles_cdef_lr_frame1(br, b, 104); });
}

TEST_CASE("av1_fh2_unit", "[syntax][av1][fast][needs-corpus]") {
  // Acceptance test 1 on the corpus file itself (same bits as the excerpt test).
  const auto path = ovt::test::corpus_or_skip("syn_av1_tiles_cdef_lr");
  const Bytes file = read_file(path);
  const Bytes obu0 = nth_frame_obu(file, 0);
  (void)with_tree(obu0, [&](BitReader& br, SyntaxBuilder& b) {
    br.skip_bits(75);
    check_tiles_cdef_lr_frame0(br, b, 0);
  });
  const Bytes obu1 = nth_frame_obu(file, 1);
  (void)with_tree(obu1, [&](BitReader& br, SyntaxBuilder& b) {
    br.skip_bits(104);
    check_tiles_cdef_lr_frame1(br, b, 0);
  });
}

// Acceptance test 2, first half: av1-1-b8-23-film_grain-50 (corpus id aom_av1_1_b8_23_film_grain_50), frame header 0
// (KEY, shown; base_q_idx > 0 so CodedLossless = 0), trace_headers / gst_parse_oracle: 60 loop_filter_level {0, 0}
// ([2] / [3] not read), 75 delta_enabled 1, 76 delta_update 1 with every update_*_delta 0 (deltas {1, 0, 0, 0, -1,
// 0, -1, -1} / {0, 0}), 87 cdef_damping_minus_3 0, 89 cdef_bits 3, cdef_y_pri {0, 0, 0, 0, 0, 0, 0, 1}, y_sec {0, 1,
// 1, 2, 1, 0, 0, 0}, uv_pri {0, 1, 5, 2, 0, 0, 2, 3}, uv_sec coded {3, 0, 0, 0, 3, 1, 0, 0} = {4, 0, 0, 0, 4, 1, 0,
// 0}, 187 lr_type {3, 2, 2} (frame_restoration_type {2, 1, 1}), 193 lr_unit_shift 1 + lr_unit_extra_shift 0
// (loop_restoration_size {128, 128, 128}), 195 lr_uv_shift 0, 196 tx_mode 0 (= TX_MODE_LARGEST), 197
// reduced_tx_set 0, 198 apply_grain 1, 199 grain_seed 7391, 215 num_y_points 7, point_y_value {0, 107, 161, 188,
// 201, 228, 255}, point_y_scaling {105, 104, 97, 112, 112, 112, 112}, chroma_scaling_from_luma 0, num_cb_points 5,
// point_cb_value {0, 107, 188, 228, 255}, scaling {68, 68, 77, 77, 77}, num_cr_points 6, point_cr_value {0, 107, 174,
// 201, 228, 255}, scaling {56, 56, 64, 66, 66, 66}, grain_scaling_minus_8 3, ar_coeff_lag 3, ar_coeffs_y_plus_128
// [0] 126 … [23] 185, ar_coeffs_cb_plus_128 [0] 133 … [24] 126, ar_coeffs_cr_plus_128 [0] 130 … [24] 127,
// ar_coeff_shift_minus_6 1, grain_scale_shift 0, cb_mult 128, cb_luma_mult 192, cb_offset 256, cr_mult 128,
// cr_luma_mult 192, 1157 cr_offset 256, 1166 overlap_flag 1, 1167 clip_to_restricted_range 0.
void check_film_grain_50_frame0(BitReader& br, SyntaxBuilder& b, uint64_t first_bit) {
  const SequenceHeader seq = seq_cif_film_grain();
  const LoopFilterParams lf =
      parse_loop_filter_params(br, b, seq.color_config, false, false, kDefaultLoopFilterRefDeltas, {0, 0});
  CHECK(lf.span.bit_offset + first_bit == 60);
  CHECK(lf.loop_filter_level == std::array<uint8_t, 4>{0, 0, 0, 0});
  CHECK(lf.loop_filter_delta_update);
  CHECK(lf.update_ref_delta == std::array<bool, 8>{});
  CHECK(lf.loop_filter_ref_deltas == kDefaultLoopFilterRefDeltas);
  const CdefParams cd = parse_cdef_params(br, b, seq, false, false);
  CHECK(cd.span.bit_offset + first_bit == 87);
  CHECK(cd.CdefDamping == 3);
  CHECK(cd.cdef_bits == 3);
  CHECK(cd.cdef_y_pri_strength == std::array<uint8_t, 8>{0, 0, 0, 0, 0, 0, 0, 1});
  CHECK(cd.cdef_y_sec_strength == std::array<uint8_t, 8>{0, 1, 1, 2, 1, 0, 0, 0});
  CHECK(cd.cdef_uv_pri_strength == std::array<uint8_t, 8>{0, 1, 5, 2, 0, 0, 2, 3});
  CHECK(cd.cdef_uv_sec_strength == std::array<uint8_t, 8>{4, 0, 0, 0, 4, 1, 0, 0});
  const LrParams lr = parse_lr_params(br, b, seq, false, false, 352, 288);
  CHECK(lr.span.bit_offset + first_bit == 187);
  CHECK(lr.FrameRestorationType[0] == RestorationType::Sgrproj);
  CHECK(lr.FrameRestorationType[1] == RestorationType::Wiener);
  CHECK(lr.FrameRestorationType[2] == RestorationType::Wiener);
  CHECK(lr.lr_unit_shift == 1);
  CHECK_FALSE(lr.lr_unit_extra_shift);
  CHECK(lr.lr_uv_shift == 0);
  CHECK(lr.LoopRestorationSize == std::array<uint16_t, 3>{128, 128, 128});
  CHECK(lr.LrUnitRows == std::array<uint32_t, 3>{2, 1, 1});  // (288 + 64) / 128, (144 + 64) / 128
  CHECK(lr.LrUnitCols == std::array<uint32_t, 3>{3, 1, 1});  // (352 + 64) / 128, (176 + 64) / 128
  CHECK(parse_tx_mode(br, b, false).TxMode == TxModeType::Largest);
  CHECK_FALSE(b.read_flag("reduced_tx_set"));
  (void)parse_global_motion_params(br, b, true, false, identity_gm_params());
  CHECK(br.bit_pos() + first_bit == 198);
  const FilmGrainParams fg = parse_film_grain_params(
      br, b, seq, DecoderState{}, RefFrameIdx{-1, -1, -1, -1, -1, -1, -1}, FrameType::Key, true, false);
  CHECK(fg.apply_grain);
  CHECK(fg.grain_seed == 7391);
  CHECK(fg.update_grain);
  CHECK(fg.num_y_points == 7);
  CHECK(fg.point_y_value == std::array<uint8_t, 16>{0, 107, 161, 188, 201, 228, 255});
  CHECK(fg.point_y_scaling == std::array<uint8_t, 16>{105, 104, 97, 112, 112, 112, 112});
  CHECK_FALSE(fg.chroma_scaling_from_luma);
  CHECK(fg.num_cb_points == 5);
  CHECK(fg.point_cb_value == std::array<uint8_t, 16>{0, 107, 188, 228, 255});
  CHECK(fg.point_cb_scaling == std::array<uint8_t, 16>{68, 68, 77, 77, 77});
  CHECK(fg.num_cr_points == 6);
  CHECK(fg.point_cr_value == std::array<uint8_t, 16>{0, 107, 174, 201, 228, 255});
  CHECK(fg.point_cr_scaling == std::array<uint8_t, 16>{56, 56, 64, 66, 66, 66});
  CHECK(fg.grain_scaling_minus_8 == 3);
  CHECK(fg.ar_coeff_lag == 3);
  CHECK(fg.ar_coeffs_y_plus_128 == std::array<uint8_t, 24>{126, 127, 129, 148, 140, 122, 131, 136, 124, 135, 80, 108,
                                                           140, 121, 117, 141, 111, 202, 149, 116, 135, 159, 88, 185});
  CHECK(fg.ar_coeffs_cb_plus_128 == std::array<uint8_t, 25>{133, 119, 133, 133, 134, 126, 126, 130, 133,
                                                            122, 110, 117, 130, 127, 115, 106, 151, 169,
                                                            143, 119, 132, 146, 97,  223, 126});
  CHECK(fg.ar_coeffs_cr_plus_128 == std::array<uint8_t, 25>{130, 124, 126, 139, 130, 127, 126, 129, 135,
                                                            127, 108, 123, 126, 126, 109, 103, 152, 168,
                                                            136, 123, 133, 142, 124, 208, 127});
  CHECK(fg.ar_coeff_shift_minus_6 == 1);
  CHECK(fg.grain_scale_shift == 0);
  CHECK(fg.cb_mult == 128);
  CHECK(fg.cb_luma_mult == 192);
  CHECK(fg.cb_offset == 256);
  CHECK(fg.cr_mult == 128);
  CHECK(fg.cr_luma_mult == 192);
  CHECK(fg.cr_offset == 256);
  CHECK(fg.overlap_flag);
  CHECK_FALSE(fg.clip_to_restricted_range);
  CHECK(br.bit_pos() + first_bit == 1168);
}

TEST_CASE("av1_film_grain_params_excerpt_av1_1_b8_23_film_grain_50", "[syntax][av1][fast]") {
  // Bits [60, 1168) of frame header 0.
  const Bytes excerpt{0x00, 0x01, 0x80, 0x06, 0x00, 0x60, 0x88, 0x0A, 0x81, 0x10, 0x08, 0x60, 0x02, 0x01, 0x02, 0x19,
                      0xD4, 0x23, 0x9B, 0xEE, 0x00, 0xD2, 0xD6, 0xD1, 0x42, 0xC3, 0x78, 0xE1, 0x92, 0xE1, 0xC8, 0xE1,
                      0xFE, 0xE0, 0x50, 0x04, 0x46, 0xB4, 0x4B, 0xC4, 0xDE, 0x44, 0xDF, 0xF4, 0xD6, 0x00, 0x38, 0x6B,
                      0x38, 0xAE, 0x40, 0xC9, 0x42, 0xE4, 0x42, 0xFF, 0x42, 0xF7, 0xE7, 0xF8, 0x19, 0x48, 0xC7, 0xA8,
                      0x38, 0x87, 0xC8, 0x75, 0x06, 0xC8, 0xC7, 0x97, 0x58, 0xD6, 0xFC, 0xA9, 0x57, 0x48, 0x79, 0xF5,
                      0x8B, 0x98, 0x57, 0x78, 0x58, 0x58, 0x67, 0xE7, 0xE8, 0x28, 0x57, 0xA6, 0xE7, 0x58, 0x27, 0xF7,
                      0x36, 0xA9, 0x7A, 0x98, 0xF7, 0x78, 0x49, 0x26, 0x1D, 0xF7, 0xE8, 0x27, 0xC7, 0xE8, 0xB8, 0x27,
                      0xF7, 0xE8, 0x18, 0x77, 0xF6, 0xC7, 0xB7, 0xE7, 0xE6, 0xD6, 0x79, 0x8A, 0x88, 0x87, 0xB8, 0x58,
                      0xE7, 0xCD, 0x07, 0xF4, 0x80, 0xC0, 0x80, 0x40, 0x60, 0x40, 0x20};
  const SyntaxNodeBuffer tree =
      with_tree(excerpt, [&](BitReader& br, SyntaxBuilder& b) { check_film_grain_50_frame0(br, b, 60); });
  CHECK(diags(tree.root(), kDiagFrameHeaderRange).empty());
  CHECK(count_named(tree.root(), "ar_coeffs_y_plus_128") >= 24);
}

TEST_CASE("av1_film_grain_params_corpus_av1_1_b8_23_film_grain_50", "[syntax][av1][fast][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_23_film_grain_50");
  const Bytes obu = nth_frame_obu(read_file(path), 0);
  (void)with_tree(obu, [&](BitReader& br, SyntaxBuilder& b) {
    br.skip_bits(60);
    check_film_grain_50_frame0(br, b, 0);
  });
}

}  // namespace
