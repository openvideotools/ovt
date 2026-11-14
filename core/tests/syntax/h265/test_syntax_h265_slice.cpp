// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 slice_segment_header(), ref_pic_lists_modification(), pred_weight_table() and entry-point tests (chapter 7
// §7.3.5 rows 10–12, WI-1.113a and WI-1.113b).
//
// Expected values of the corpus tests are hard-coded from the recorded oracle outputs committed as goldens:
//   tests/golden/h265/jctvc_WP_A_Toshiba_3.trace_headers.json.gz    (FFmpeg trace_headers, acceptance test 1)
//   tests/golden/h265/jctvc_RPS_{A..F}_*.gstparse.json.gz            (GStreamer gst_parse_oracle, acceptance test 2)
//   tests/golden/h265/jctvc_ENTP_A_QUALCOMM_1.trace_headers.json.gz (FFmpeg trace_headers, acceptance test 3)
//   tests/golden/h265/jctvc_DSLICE_A_HHI_5.trace_headers.json.gz    (FFmpeg trace_headers, acceptance test 4)
// FFmpeg's trace calls delta_chroma_offset_lX "chroma_offset_lX". The streams are fetched by tools/fetch_corpora.py;
// the [needs-corpus] cases skip when absent. Acceptance test 1 also runs on the first P slice rebuilt bit for bit
// from the trace_headers golden (SPS, PPS and slice header; byte_alignment() and filler appended), so it never
// skips.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "h265_test_util.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/st_ref_pic_set.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"
#include "test_support/corpus.h"

namespace {

using ovt::BitReader;
using ovt::ParseError;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
using ovt::syntax::SyntaxNodeData;
using ovt::test::h265::BitWriter;
using ovt::test::h265::find_child;
using ovt::test::h265::parse_nal;
namespace h265 = ovt::syntax::h265;

// ---- helpers ------------------------------------------------------------------------------------------------------

std::vector<uint8_t> from_hex(std::string_view hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

// H.265 §7.4.2 emulation prevention: a 0x03 after every 00 00 that is followed by a byte <= 0x03.
std::vector<uint8_t> add_emulation_prevention(const std::vector<uint8_t>& rbsp) {
  std::vector<uint8_t> out;
  unsigned zeros = 0;
  for (const uint8_t byte : rbsp) {
    if (zeros >= 2 && byte <= 3) {
      out.push_back(3);
      zeros = 0;
    }
    out.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  return out;
}

// Every diagnostic code in the tree, depth first.
void collect_codes(const SyntaxNodeData* n, std::vector<std::string>& out) {
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    out.emplace_back(d->code);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    collect_codes(c, out);
  }
}

std::vector<std::string> codes(const SyntaxNodeBuffer& tree) {
  std::vector<std::string> out;
  collect_codes(tree.root(), out);
  return out;
}

bool has_code(const SyntaxNodeBuffer& tree, std::string_view code) {
  return std::ranges::any_of(codes(tree), [code](const std::string& c) { return c == code; });
}

// Parses SPS, PPS and slice segment NAL units in order, keeping the parameter sets alive and activated, and records
// every slice segment header with the unit facts the substream mapping needs.
class SliceWalker {
 public:
  struct Slice {
    size_t unit = 0;  // index among the walked NAL units
    h265::NalHeader nal;
    h265::SliceSegmentHeader sh;
    std::vector<uint32_t> epb_positions;
    uint64_t nal_size = 0;
  };

  void add(std::span<const uint8_t> nal_bytes) {
    const std::vector<uint8_t> nal(nal_bytes.begin(), nal_bytes.end());
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
    BitReader br(rbsp.rbsp);
    SyntaxBuilder b(br);
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    if (h.nal_unit_type == h265::kSpsNut) {
      auto sps = std::make_unique<h265::Sps>(h265::parse_sps(br, b, h.nuh_layer_id));
      const uint8_t id = sps->sps_seq_parameter_set_id;
      sps_[id] = std::move(sps);
      reactivate();
    } else if (h.nal_unit_type == h265::kPpsNut) {
      auto pps = std::make_unique<h265::Pps>(h265::parse_pps(br, b));
      const uint8_t id = pps->pps_pic_parameter_set_id;
      pps_[id] = std::move(pps);
      reactivate();
    } else if (h.is_vcl) {
      Slice s;
      s.unit = count_;
      s.nal = h;
      s.sh = h265::parse_slice_segment_header(br, b, h, table_, prev_independent_ ? &*prev_independent_ : nullptr);
      if (!s.sh.dependent_slice_segment_flag) {
        prev_independent_ = s.sh;
      }
      s.epb_positions = rbsp.epb_positions;
      s.nal_size = nal.size();
      slices.push_back(std::move(s));
    }
    ++count_;
  }

  void add_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::vector<uint8_t>> units;
    ovt::syntax::scan_annexb(std::span<const uint8_t>(data), 0, ovt::syntax::kNoContainerSample,
                             [&units](const ovt::syntax::ScannedUnit& u) {
                               if (u.bytes.size() >= 2) {
                                 units.emplace_back(u.bytes.begin(), u.bytes.end());
                               }
                             });
    for (const auto& u : units) {
      add(u);
    }
  }

  // The first slice segment of each of the first `n` pictures.
  [[nodiscard]] std::vector<const Slice*> first_slices(size_t n) const {
    std::vector<const Slice*> out;
    for (const Slice& s : slices) {
      if (s.sh.first_slice_segment_in_pic_flag && out.size() < n) {
        out.push_back(&s);
      }
    }
    return out;
  }

  [[nodiscard]] const h265::Sps& sps_of(const Slice& s) const {
    return *table_.entries[s.sh.slice_pic_parameter_set_id]->sps;
  }

  std::vector<Slice> slices;

 private:
  void reactivate() {
    for (size_t id = 0; id < pps_.size(); ++id) {
      table_.entries[id] = nullptr;
      if (pps_[id] == nullptr || sps_[pps_[id]->pps_seq_parameter_set_id] == nullptr) {
        continue;
      }
      active_[id] =
          std::make_unique<h265::ActivePps>(h265::activate_pps(*pps_[id], *sps_[pps_[id]->pps_seq_parameter_set_id]));
      table_.entries[id] = active_[id].get();
    }
  }

  std::array<std::unique_ptr<h265::Sps>, 16> sps_;
  std::array<std::unique_ptr<h265::Pps>, 64> pps_;
  std::array<std::unique_ptr<h265::ActivePps>, 64> active_;
  h265::PpsTable table_;
  std::optional<h265::SliceSegmentHeader> prev_independent_;
  size_t count_ = 0;
};

// A hand-made parameter-set pair for the bit-level tests: 64x64 luma samples in 16x16 CTBs (4x4 CTBs), 4:2:0,
// 8-bit POC LSBs, two SPS short-term sets and two SPS long-term candidates. Built as structs, not parsed, so each
// test changes only the fields it is about.
struct Fixture {
  h265::Sps sps;
  h265::Pps pps;
  std::optional<h265::ActivePps> active;
  h265::PpsTable table;

  Fixture() {
    sps.pic_width_in_luma_samples = 64;
    sps.pic_height_in_luma_samples = 64;
    sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
    sps.MaxPicOrderCntLsb = 256;
    sps.CtbLog2SizeY = 4;
    sps.CtbSizeY = 16;
    sps.PicWidthInCtbsY = 4;
    sps.PicHeightInCtbsY = 4;
    sps.PicSizeInCtbsY = 16;
    sps.MaxDpbSizeMinus1 = 5;
    sps.sample_adaptive_offset_enabled_flag = true;
    sps.sps_temporal_mvp_enabled_flag = true;
    h265::StRefPicSet s0;  // S0: -1(used) -2(used)
    s0.NumNegativePics = 2;
    s0.NumDeltaPocs = 2;
    s0.DeltaPocS0 = {-1, -2};
    s0.UsedByCurrPicS0 = {true, true};
    h265::StRefPicSet s1 = s0;  // S0: -1(used) | S1: +2(used)
    s1.stRpsIdx = 1;
    s1.NumNegativePics = 1;
    s1.NumPositivePics = 1;
    s1.DeltaPocS0 = {-1};
    s1.DeltaPocS1 = {2};
    s1.UsedByCurrPicS1 = {true};
    sps.st_ref_pic_set = {s0, s1};
    sps.num_short_term_ref_pic_sets = 2;
    pps.num_ref_idx_l0_default_active_minus1 = 1;
    pps.num_ref_idx_l1_default_active_minus1 = 0;
    pps.init_qp_minus26 = 4;
    pps.pps_loop_filter_across_slices_enabled_flag = true;
    activate();
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&&) = delete;
  Fixture& operator=(Fixture&&) = delete;
  ~Fixture() = default;

  // Re-derives the tile tables after a test changed the PPS or SPS.
  void activate() {
    active.emplace(h265::activate_pps(pps, sps));
    table.entries[0] = &*active;
  }
};

// nal_unit_header() for `type` (layer 0, TemporalId 0).
void write_nal_header(BitWriter& w, unsigned type) {
  w.u(1, 0);
  w.u(6, type);
  w.u(6, 0);
  w.u(3, 1);
}

// byte_alignment() and `data` bytes of slice_segment_data().
std::vector<uint8_t> finish_slice(BitWriter& w, std::initializer_list<uint8_t> data = {0x5A, 0x5A}) {
  w.trailing();  // alignment_bit_equal_to_one + zero bits: the same pattern as rbsp_trailing_bits()
  std::vector<uint8_t> out = w.bytes();
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

h265::SliceSegmentHeader parse_slice(const std::vector<uint8_t>& nal, const Fixture& f,
                                     const h265::SliceSegmentHeader* prev = nullptr, SyntaxNodeBuffer* tree = nullptr) {
  return parse_nal(
      nal,
      [&f, prev](BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h) {
        return h265::parse_slice_segment_header(br, b, h, f.table, prev);
      },
      tree);
}

}  // namespace

// ---- row 10: slice_segment_header (hand-built) -------------------------------------------------------------------

TEST_CASE("h265_slice: IDR I slice has no POC or RPS; byte_alignment and slice_segment_data follow the header",
          "[syntax][h265][fast]") {
  Fixture f;
  BitWriter w;
  write_nal_header(w, h265::kIdrWRadl);
  w.flag(true);   // first_slice_segment_in_pic_flag
  w.flag(false);  // no_output_of_prior_pics_flag (IRAP)
  w.ue(0);        // slice_pic_parameter_set_id
  w.ue(2);        // slice_type I
  w.flag(true);   // slice_sao_luma_flag
  w.flag(false);  // slice_sao_chroma_flag
  w.se(-3);       // slice_qp_delta
  w.flag(false);  // slice_loop_filter_across_slices_enabled_flag
  const size_t header_bits = w.size_bits();
  SyntaxNodeBuffer tree;
  const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
  CHECK(sh.slice_type == h265::kSliceI);
  CHECK(sh.SliceQpY == 26 + 4 - 3);
  CHECK(sh.NumPicTotalCurr == 0);
  CHECK(sh.slice_sao_luma_flag);
  CHECK_FALSE(sh.slice_loop_filter_across_slices_enabled_flag);
  CHECK(sh.MaxNumMergeCand == 5);
  CHECK(sh.slice_segment_data_rbsp_byte == (header_bits + 7) / 8);
  CHECK(sh.span.bit_offset == 16);
  CHECK(sh.span.bit_length == ((header_bits + 7) / 8) * 8 - 16);
  CHECK(codes(tree).empty());
  const SyntaxNodeData* hdr = find_child(tree.root(), "slice_segment_header");
  REQUIRE(hdr != nullptr);
  CHECK(find_child(hdr, "byte_alignment") != nullptr);
  CHECK(find_child(hdr, "slice_pic_order_cnt_lsb") == nullptr);
  const SyntaxNodeData* data = find_child(tree.root(), "slice_segment_data");
  REQUIRE(data != nullptr);
  CHECK(data->span.bit_offset == sh.slice_segment_data_rbsp_byte * 8);
  CHECK(data->span.bit_length == 16);
  const SyntaxNodeData* type = find_child(hdr, "slice_type");
  REQUIRE(type != nullptr);
  CHECK(type->enum_name == "I");
}

TEST_CASE("h265_slice: short_term_ref_pic_set_idx is u(v) with Ceil(Log2(n)) bits and absent when n == 1",
          "[syntax][h265][fast]") {
  Fixture f;
  auto write_p = [](BitWriter& w, bool with_idx, unsigned idx_bits, unsigned idx) {
    write_nal_header(w, h265::kTrailR);
    w.flag(true);  // first_slice_segment_in_pic_flag
    w.ue(0);
    w.ue(1);       // P
    w.u(8, 7);     // slice_pic_order_cnt_lsb
    w.flag(true);  // short_term_ref_pic_set_sps_flag
    if (with_idx) {
      w.u(idx_bits, idx);
    }
    w.flag(false);  // slice_temporal_mvp_enabled_flag
    w.flag(false);  // slice_sao_luma_flag
    w.flag(false);  // slice_sao_chroma_flag
    w.flag(false);  // num_ref_idx_active_override_flag
    w.ue(0);        // five_minus_max_num_merge_cand
    w.se(0);        // slice_qp_delta
    w.flag(true);   // slice_loop_filter_across_slices_enabled_flag
  };
  SECTION("two SPS sets: one bit selects set 1") {
    BitWriter w;
    write_p(w, true, 1, 1);
    SyntaxNodeBuffer tree;
    const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
    CHECK(sh.short_term_ref_pic_set_idx == 1);
    CHECK(sh.CurrRpsIdx == 1);
    CHECK(sh.NumPicTotalCurr == 2);               // -1(used), +2(used)
    CHECK(sh.num_ref_idx_l0_active_minus1 == 1);  // PPS default
    CHECK(h265::current_st_ref_pic_set(sh, f.sps) == &f.sps.st_ref_pic_set[1]);
    CHECK(codes(tree).empty());
  }
  SECTION("one SPS set: the index is absent and inferred 0") {
    f.sps.st_ref_pic_set.resize(1);
    f.sps.num_short_term_ref_pic_sets = 1;
    BitWriter w;
    write_p(w, false, 0, 0);
    SyntaxNodeBuffer tree;
    const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
    CHECK(sh.short_term_ref_pic_set_idx == 0);
    CHECK(sh.NumPicTotalCurr == 2);  // -1(used), -2(used)
    CHECK(find_child(find_child(tree.root(), "slice_segment_header"), "short_term_ref_pic_set_idx") == nullptr);
    CHECK(codes(tree).empty());
  }
  SECTION("three SPS sets: two bits, value 3 is out of range") {
    f.sps.st_ref_pic_set.push_back(f.sps.st_ref_pic_set[0]);
    f.sps.num_short_term_ref_pic_sets = 3;
    BitWriter w;
    write_p(w, true, 2, 3);
    try {
      (void)parse_slice(finish_slice(w), f);
      FAIL("expected ParseError");
    } catch (const ParseError& e) {
      CHECK(e.code == "SYN-H265-0007");
    }
  }
}

TEST_CASE("h265_slice: slice-local RPS, long-term entries, list modification and collocated_ref_idx",
          "[syntax][h265][fast]") {
  Fixture f;
  f.sps.long_term_ref_pics_present_flag = true;
  f.sps.num_long_term_ref_pics_sps = 3;
  f.sps.lt_ref_pic_poc_lsb_sps[0] = 10;
  f.sps.lt_ref_pic_poc_lsb_sps[2] = 30;
  f.sps.used_by_curr_pic_lt_sps_flag[2] = true;
  f.pps.lists_modification_present_flag = true;
  f.pps.cabac_init_present_flag = true;
  f.activate();
  BitWriter w;
  write_nal_header(w, h265::kTrailR);
  w.flag(true);
  w.ue(0);
  w.ue(0);        // B
  w.u(8, 40);     // slice_pic_order_cnt_lsb
  w.flag(false);  // short_term_ref_pic_set_sps_flag -> st_ref_pic_set(2)
  w.flag(true);   //   inter_ref_pic_set_prediction_flag
  w.ue(1);        //   delta_idx_minus1 -> RefRpsIdx 0 (S0: -1 -2)
  w.flag(true);   //   delta_rps_sign
  w.ue(0);        //   abs_delta_rps_minus1 -> deltaRps = -1
  for (int j = 0; j < 3; ++j) {
    w.flag(true);  //   used_by_curr_pic_flag[j]
  }
  const size_t rps_bits = 1 + 3 + 1 + 1 + 3;
  w.ue(1);        // num_long_term_sps
  w.ue(1);        // num_long_term_pics
  w.u(2, 2);      // lt_idx_sps[0] (Ceil(Log2(3)) = 2 bits) -> PocLsbLt 30, used
  w.flag(true);   // delta_poc_msb_present_flag[0]
  w.ue(2);        // delta_poc_msb_cycle_lt[0]
  w.u(8, 200);    // poc_lsb_lt[1]
  w.flag(false);  // used_by_curr_pic_lt_flag[1]
  w.flag(true);   // delta_poc_msb_present_flag[1]
  w.ue(5);        // delta_poc_msb_cycle_lt[1] -> DeltaPocMsbCycleLt[1] = 5 (i == num_long_term_sps restarts)
  w.flag(true);   // slice_temporal_mvp_enabled_flag
  w.flag(false);  // slice_sao_luma_flag
  w.flag(false);  // slice_sao_chroma_flag
  w.flag(true);   // num_ref_idx_active_override_flag
  w.ue(2);        // num_ref_idx_l0_active_minus1
  w.ue(1);        // num_ref_idx_l1_active_minus1
  // NumPicTotalCurr = 3 short-term (all used) + 1 long-term -> list_entry_lX are Ceil(Log2(4)) = 2 bits
  w.flag(true);  // ref_pic_list_modification_flag_l0
  w.u(2, 3);
  w.u(2, 0);
  w.u(2, 1);
  w.flag(false);  // ref_pic_list_modification_flag_l1
  w.flag(true);   // mvd_l1_zero_flag
  w.flag(true);   // cabac_init_flag
  w.flag(false);  // collocated_from_l0_flag -> list 1 has 2 entries, so collocated_ref_idx follows
  w.ue(1);        // collocated_ref_idx
  w.ue(1);        // five_minus_max_num_merge_cand
  w.se(2);        // slice_qp_delta
  w.flag(true);   // slice_loop_filter_across_slices_enabled_flag
  SyntaxNodeBuffer tree;
  const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
  CHECK(codes(tree).empty());
  // 7-61 with RefRpsIdx 0 (S0: -1, -2) and deltaRps -1: the reference's S1 (none), then deltaRps itself (-1),
  // then each S0 entry + deltaRps (-2, -3).
  REQUIRE(sh.st_ref_pic_set.NumNegativePics == 3);
  CHECK(sh.st_ref_pic_set.DeltaPocS0[0] == -1);
  CHECK(sh.st_ref_pic_set.DeltaPocS0[1] == -2);
  CHECK(sh.st_ref_pic_set.DeltaPocS0[2] == -3);
  CHECK(sh.st_ref_pic_set.NumPositivePics == 0);
  CHECK(sh.CurrRpsIdx == 2);
  CHECK(sh.NumBitsForShortTermRPSInSlice == rps_bits);
  CHECK(sh.PocLsbLt[0] == 30);
  CHECK(sh.UsedByCurrPicLt[0]);
  CHECK(sh.PocLsbLt[1] == 200);
  CHECK_FALSE(sh.UsedByCurrPicLt[1]);
  CHECK(sh.DeltaPocMsbCycleLt[0] == 2);
  CHECK(sh.DeltaPocMsbCycleLt[1] == 5);
  CHECK(sh.NumPicTotalCurr == 4);
  REQUIRE(sh.ref_pic_lists_modification.has_value());
  CHECK(sh.ref_pic_lists_modification->list_entry_l0[0] == 3);
  CHECK(sh.ref_pic_lists_modification->list_entry_l0[2] == 1);
  CHECK_FALSE(sh.ref_pic_lists_modification->ref_pic_list_modification_flag_l1);
  CHECK(sh.mvd_l1_zero_flag);
  CHECK(sh.cabac_init_flag);
  CHECK_FALSE(sh.collocated_from_l0_flag);
  CHECK(sh.collocated_ref_idx == 1);
  CHECK(sh.MaxNumMergeCand == 4);
  CHECK(sh.SliceQpY == 32);
}

TEST_CASE("h265_slice: collocated_ref_idx is absent when the collocated list has one entry", "[syntax][h265][fast]") {
  Fixture f;
  BitWriter w;
  write_nal_header(w, h265::kTrailR);
  w.flag(true);
  w.ue(0);
  w.ue(1);  // P: collocated_from_l0_flag is not read and inferred 1
  w.u(8, 3);
  w.flag(true);
  w.u(1, 0);
  w.flag(true);   // slice_temporal_mvp_enabled_flag
  w.flag(false);  // sao luma
  w.flag(false);  // sao chroma
  w.flag(true);   // num_ref_idx_active_override_flag
  w.ue(0);        // num_ref_idx_l0_active_minus1 = 0 -> no collocated_ref_idx
  w.ue(0);        // five_minus_max_num_merge_cand
  w.se(0);
  w.flag(true);
  SyntaxNodeBuffer tree;
  const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
  CHECK(sh.collocated_from_l0_flag);
  CHECK(sh.collocated_ref_idx == 0);
  CHECK(find_child(find_child(tree.root(), "slice_segment_header"), "collocated_ref_idx") == nullptr);
  CHECK(codes(tree).empty());
}

// ---- row 12: pred_weight_table (hand-built) ----------------------------------------------------------------------

TEST_CASE("h265_pred_weight_table: B slice with chroma weights; LumaWeight, ChromaWeight and 7-56 ChromaOffset",
          "[syntax][h265][fast]") {
  Fixture f;
  f.pps.weighted_bipred_flag = true;
  f.activate();
  BitWriter w;
  write_nal_header(w, h265::kTrailR);
  w.flag(true);
  w.ue(0);
  w.ue(0);  // B
  w.u(8, 9);
  w.flag(true);
  w.u(1, 1);      // set 1: -1(used) +2(used)
  w.flag(false);  // slice_temporal_mvp_enabled_flag
  w.flag(false);
  w.flag(false);
  w.flag(true);   // override
  w.ue(1);        // l0: 2 entries
  w.ue(0);        // l1: 1 entry
  w.flag(false);  // mvd_l1_zero_flag
  // pred_weight_table()
  w.ue(6);        // luma_log2_weight_denom
  w.se(-2);       // delta_chroma_log2_weight_denom -> ChromaLog2WeightDenom 4
  w.flag(true);   // luma_weight_l0_flag[0]
  w.flag(false);  // luma_weight_l0_flag[1]
  w.flag(false);  // chroma_weight_l0_flag[0]
  w.flag(true);   // chroma_weight_l0_flag[1]
  w.se(-5);       // delta_luma_weight_l0[0]
  w.se(7);        // luma_offset_l0[0]
  w.se(3);        // delta_chroma_weight_l0[1][0]
  w.se(-20);      // delta_chroma_offset_l0[1][0]
  w.se(-128);     // delta_chroma_weight_l0[1][1]
  w.se(511);      // delta_chroma_offset_l0[1][1]
  w.flag(false);  // luma_weight_l1_flag[0]
  w.flag(false);  // chroma_weight_l1_flag[0]
  w.ue(0);        // five_minus_max_num_merge_cand
  w.se(0);
  w.flag(true);
  SyntaxNodeBuffer tree;
  const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
  CHECK(codes(tree).empty());
  REQUIRE(sh.pred_weight_table.has_value());
  const h265::PredWeightTable& t = *sh.pred_weight_table;
  CHECK(t.ChromaLog2WeightDenom == 4);
  CHECK(t.l0.LumaWeight[0] == 64 - 5);
  CHECK(t.l0.luma_offset[0] == 7);
  CHECK(t.l0.LumaWeight[1] == 64);
  CHECK(t.l0.ChromaWeight[0][0] == 16);
  CHECK(t.l0.ChromaOffset[0][1] == 0);
  CHECK(t.l0.ChromaWeight[1][0] == 19);
  // 7-56: Clip3(-128, 127, (128 - ((128 * 19) >> 4) - 20)) = 128 - 152 - 20 = -44
  CHECK(t.l0.ChromaOffset[1][0] == -44);
  CHECK(t.l0.ChromaWeight[1][1] == 16 - 128);
  // 128 - ((128 * -112) >> 4) + 511 = 128 + 896 + 511, clipped to 127
  CHECK(t.l0.ChromaOffset[1][1] == 127);
  CHECK(t.l1.LumaWeight[0] == 64);
  CHECK(t.l1.ChromaWeight[0][1] == 16);
}

TEST_CASE("h265_pred_weight_table: out-of-range weights throw; SCC current-picture references raise SYN-H265-0040",
          "[syntax][h265][fast]") {
  Fixture f;
  f.pps.weighted_pred_flag = true;
  auto write_p = [](BitWriter& w, uint32_t denom, int32_t delta_luma) {
    write_nal_header(w, h265::kTrailR);
    w.flag(true);
    w.ue(0);
    w.ue(1);  // P
    w.u(8, 9);
    w.flag(true);
    w.u(1, 0);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    w.ue(0);  // one L0 entry
    w.ue(denom);
    w.se(0);
    w.flag(true);  // luma_weight_l0_flag[0]
    w.flag(false);
    w.se(delta_luma);
    w.se(0);
    w.ue(0);
    w.se(0);
    w.flag(true);
  };
  SECTION("luma_log2_weight_denom 8") {
    f.activate();
    BitWriter w;
    write_p(w, 8, 0);
    CHECK_THROWS_AS(parse_slice(finish_slice(w), f), ParseError);
  }
  SECTION("delta_luma_weight_l0 128") {
    f.activate();
    BitWriter w;
    write_p(w, 3, 128);
    CHECK_THROWS_AS(parse_slice(finish_slice(w), f), ParseError);
  }
  SECTION("pps_curr_pic_ref_enabled_flag") {
    f.pps.pps_scc_extension_flag = true;
    f.pps.scc_extension.pps_curr_pic_ref_enabled_flag = true;
    f.activate();
    BitWriter w;
    write_p(w, 3, -2);
    SyntaxNodeBuffer tree;
    const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
    CHECK(has_code(tree, "SYN-H265-0040"));
    CHECK(sh.NumPicTotalCurr == 3);  // -1, -2 and the current picture
    CHECK(sh.pred_weight_table->l0.LumaWeight[0] == 8 - 2);
  }
}

// ---- dependent slice segments and entry points (hand-built) -------------------------------------------------------

TEST_CASE("h265_slice: a dependent slice segment copies the independent part from prev_independent",
          "[syntax][h265][fast]") {
  Fixture f;
  f.pps.dependent_slice_segments_enabled_flag = true;
  f.activate();
  BitWriter w1;
  write_nal_header(w1, h265::kTrailR);
  w1.flag(false);  // first_slice_segment_in_pic_flag
  w1.ue(0);
  w1.flag(false);  // dependent_slice_segment_flag
  w1.u(4, 4);      // slice_segment_address (Ceil(Log2(16)) = 4 bits)
  w1.ue(1);        // P
  w1.u(8, 12);
  w1.flag(true);
  w1.u(1, 1);
  w1.flag(false);
  w1.flag(true);   // sao luma
  w1.flag(false);  // sao chroma
  w1.flag(false);
  w1.ue(2);   // five_minus_max_num_merge_cand
  w1.se(-6);  // slice_qp_delta
  w1.flag(true);
  SyntaxNodeBuffer t1;
  const h265::SliceSegmentHeader indep = parse_slice(finish_slice(w1), f, nullptr, &t1);
  CHECK(codes(t1).empty());
  CHECK(indep.SliceAddrRs == 4);

  BitWriter w2;
  write_nal_header(w2, h265::kTrailR);
  w2.flag(false);
  w2.ue(0);
  w2.flag(true);  // dependent_slice_segment_flag
  w2.u(4, 9);     // slice_segment_address
  SECTION("with the independent segment") {
    SyntaxNodeBuffer t2;
    const h265::SliceSegmentHeader dep = parse_slice(finish_slice(w2), f, &indep, &t2);
    CHECK(codes(t2).empty());
    CHECK(dep.dependent_slice_segment_flag);
    CHECK(dep.copied_from_independent);
    CHECK(dep.slice_segment_address == 9);
    CHECK(dep.SliceAddrRs == 4);
    CHECK(dep.slice_type == h265::kSliceP);
    CHECK(dep.SliceQpY == indep.SliceQpY);
    CHECK(dep.SliceQpY == 24);
    CHECK(dep.MaxNumMergeCand == 3);
    CHECK(dep.slice_pic_order_cnt_lsb == 12);
    CHECK(dep.slice_sao_luma_flag);
    CHECK_FALSE(dep.first_slice_segment_in_pic_flag);
    const SyntaxNodeData* hdr = find_child(t2.root(), "slice_segment_header");
    CHECK(find_child(hdr, "slice_type") == nullptr);
  }
  SECTION("without it: SYN-H265-0061 and parsing continues") {
    SyntaxNodeBuffer t2;
    const h265::SliceSegmentHeader dep = parse_slice(finish_slice(w2), f, nullptr, &t2);
    CHECK(has_code(t2, "SYN-H265-0061"));
    CHECK_FALSE(dep.copied_from_independent);
    CHECK(dep.SliceAddrRs == 9);
    CHECK(find_child(t2.root(), "slice_segment_data") != nullptr);
  }
}

TEST_CASE("h265_slice: entry points map to raw substreams that count emulation prevention bytes",
          "[syntax][h265][fast]") {
  Fixture f;
  f.pps.tiles_enabled_flag = true;
  f.pps.num_tile_columns_minus1 = 1;
  f.pps.num_tile_rows_minus1 = 1;
  f.pps.uniform_spacing_flag = true;
  f.activate();
  BitWriter w;
  write_nal_header(w, h265::kIdrNLp);
  w.flag(true);
  w.flag(false);
  w.ue(0);
  w.ue(2);  // I
  w.flag(false);
  w.flag(false);
  w.se(0);
  w.flag(true);
  w.ue(3);     // num_entry_point_offsets (2x2 tiles allow 3)
  w.ue(15);    // offset_len_minus1 -> 16 bits, so the zero offsets below put 00 00 0x into the header
  w.u(16, 0);  // substream 0: 1 byte
  w.u(16, 0);  // substream 1: 1 byte
  w.u(16, 1);  // substream 2: 2 bytes
  w.trailing();
  const std::vector<uint8_t> header_rbsp = w.bytes();
  const std::vector<uint8_t> header_raw = add_emulation_prevention(header_rbsp);
  const uint64_t header_epbs = header_raw.size() - header_rbsp.size();
  REQUIRE(header_epbs > 0);
  // slice_segment_data in raw bytes, with an emulation-prevented 00 00 03 01 across substreams 2 and 3: the offsets
  // count the EPB as a byte of substream 3.
  const std::vector<uint8_t> raw_data = {0x11, 0x22, 0x00, 0x00, 0x03, 0x01, 0x55, 0x66};
  std::vector<uint8_t> nal = header_raw;
  nal.insert(nal.end(), raw_data.begin(), raw_data.end());
  const ovt::Rbsp r = ovt::rbsp_from_nal(nal);
  REQUIRE(r.epb_positions.size() == header_epbs + 1);
  SyntaxNodeBuffer tree;
  const h265::SliceSegmentHeader sh = parse_slice(nal, f, nullptr, &tree);
  CHECK(codes(tree).empty());
  CHECK(sh.num_entry_point_offsets == 3);
  CHECK(sh.entry_point_offset_minus1 == std::vector<uint32_t>{0, 0, 1});
  CHECK(sh.slice_segment_data_rbsp_byte == header_rbsp.size());
  const auto subs = h265::entry_point_substreams(sh, r.epb_positions, nal.size());
  REQUIRE(subs.has_value());
  REQUIRE(subs->size() == 4);
  const uint64_t data_raw = header_raw.size();  // the header's EPBs move slice_segment_data in raw bytes
  CHECK((*subs)[0].first_byte == data_raw);
  CHECK((*subs)[0].size == 1);
  CHECK((*subs)[1].first_byte == data_raw + 1);
  CHECK((*subs)[1].size == 1);
  CHECK((*subs)[2].first_byte == data_raw + 2);
  CHECK((*subs)[2].size == 2);
  CHECK((*subs)[3].first_byte == data_raw + 4);  // 03 01 55 66: the EPB of 00 00 03 01 counts in substream 3
  CHECK((*subs)[3].size == 4);
  CHECK((*subs)[3].first_byte + (*subs)[3].size == nal.size());

  SECTION("offsets that overrun the unit") {
    CHECK_FALSE(h265::entry_point_substreams(sh, r.epb_positions, data_raw + 4).has_value());
  }
}

TEST_CASE("h265_slice: entry point counts beyond the tile/row limit warn; impossible ones throw",
          "[syntax][h265][fast]") {
  Fixture f;
  f.pps.entropy_coding_sync_enabled_flag = true;  // WPP: at most PicHeightInCtbsY - 1 = 3 offsets
  f.activate();
  auto write_i = [](BitWriter& w, uint32_t num, uint32_t len_minus1) {
    write_nal_header(w, h265::kIdrNLp);
    w.flag(true);
    w.flag(false);
    w.ue(0);
    w.ue(2);
    w.flag(false);
    w.flag(false);
    w.se(0);
    w.flag(true);
    w.ue(num);
    if (num > 0) {
      w.ue(len_minus1);
      for (uint32_t i = 0; i < num; ++i) {
        w.u(len_minus1 + 1, 0);
      }
    }
  };
  SECTION("5 offsets with WPP on a 4-row picture: SYN-H265-0031") {
    BitWriter w;
    write_i(w, 5, 0);
    SyntaxNodeBuffer tree;
    std::vector<uint8_t> nal = finish_slice(w, {1, 2, 3, 4, 5, 6, 7});
    const h265::SliceSegmentHeader sh = parse_slice(nal, f, nullptr, &tree);
    CHECK(has_code(tree, "SYN-H265-0031"));
    CHECK(sh.entry_point_offset_minus1.size() == 5);
  }
  SECTION("16 offsets on a 16-CTB picture: ParseError") {
    BitWriter w;
    write_i(w, 16, 0);
    CHECK_THROWS_AS(parse_slice(finish_slice(w), f), ParseError);
  }
  SECTION("offset_len_minus1 32: ParseError") {
    BitWriter w;
    write_i(w, 1, 32);
    CHECK_THROWS_AS(parse_slice(finish_slice(w), f), ParseError);
  }
}

TEST_CASE("h265_slice: missing PPS, bad slice_type and QP out of range are ParseErrors", "[syntax][h265][fast]") {
  Fixture f;
  auto expect_code = [&f](const std::vector<uint8_t>& nal, std::string_view code) {
    try {
      (void)parse_slice(nal, f);
      FAIL("expected ParseError");
    } catch (const ParseError& e) {
      CHECK(e.code == code);
    }
  };
  SECTION("PPS 5 was never received") {
    BitWriter w;
    write_nal_header(w, h265::kIdrNLp);
    w.flag(true);
    w.flag(false);
    w.ue(5);
    expect_code(finish_slice(w), "SYN-H265-0011");
  }
  SECTION("slice_pic_parameter_set_id 64") {
    BitWriter w;
    write_nal_header(w, h265::kIdrNLp);
    w.flag(true);
    w.flag(false);
    w.ue(64);
    expect_code(finish_slice(w), "SYN-H265-0007");
  }
  SECTION("slice_type 3") {
    BitWriter w;
    write_nal_header(w, h265::kIdrNLp);
    w.flag(true);
    w.flag(false);
    w.ue(0);
    w.ue(3);
    expect_code(finish_slice(w), "SYN-H265-0007");
  }
  SECTION("SliceQpY 52") {
    BitWriter w;
    write_nal_header(w, h265::kIdrNLp);
    w.flag(true);
    w.flag(false);
    w.ue(0);
    w.ue(2);
    w.flag(false);
    w.flag(false);
    w.se(22);  // 26 + 4 + 22
    expect_code(finish_slice(w), "SYN-H265-0007");
  }
  SECTION("slice_segment_address beyond PicSizeInCtbsY") {
    f.sps.PicSizeInCtbsY = 12;  // 4 bits, value 13 is out of range
    f.activate();
    BitWriter w;
    write_nal_header(w, h265::kTrailR);
    w.flag(false);
    w.ue(0);
    w.u(4, 13);
    expect_code(finish_slice(w), "SYN-H265-0007");
  }
}

TEST_CASE("h265_slice: a P slice in an IRAP picture is a Diagnostic, not an error", "[syntax][h265][fast]") {
  Fixture f;
  BitWriter w;
  write_nal_header(w, h265::kCraNut);
  w.flag(true);
  w.flag(false);
  w.ue(0);
  w.ue(1);  // P in a CRA
  w.u(8, 0);
  w.flag(true);
  w.u(1, 0);
  w.flag(false);
  w.flag(false);
  w.flag(false);
  w.flag(false);
  w.ue(0);
  w.se(0);
  w.flag(true);
  SyntaxNodeBuffer tree;
  const h265::SliceSegmentHeader sh = parse_slice(finish_slice(w), f, nullptr, &tree);
  CHECK(sh.slice_type == h265::kSliceP);
  CHECK(codes(tree) == std::vector<std::string>{"SYN-H265-0007"});
}

// ---- acceptance test 1: WP_A_Toshiba_3, first P slice --------------------------------------------------------------

namespace {

// tests/golden/h265/jctvc_WP_A_Toshiba_3.trace_headers.json.gz, units 1 (SPS), 2 (PPS) and 5 (first P slice), each
// rebuilt from the recorded elements (ue/se re-encoded, emulation prevention re-applied); rbsp_trailing_bits() and,
// for the slice, byte_alignment() plus two filler bytes are appended because the golden does not record them.
constexpr std::string_view kWpaSps =
    "4201010160000003000003000003000003003ca00d080f1fe5979246d871792493f9e7f3cbffffff3f9fcfcfe764";
constexpr std::string_view kWpaPps = "4401c19095a112";
constexpr std::string_view kWpaSlice5 = "0201d00d3e10e78730";

void check_wpa_first_p(const h265::SliceSegmentHeader& sh) {
  CHECK(sh.slice_type == h265::kSliceP);
  CHECK(sh.slice_pic_order_cnt_lsb == 1);
  CHECK(sh.short_term_ref_pic_set_idx == 4);
  CHECK(sh.num_ref_idx_active_override_flag);
  CHECK(sh.num_ref_idx_l0_active_minus1 == 0);
  REQUIRE(sh.pred_weight_table.has_value());
  const h265::PredWeightTable& t = *sh.pred_weight_table;
  CHECK(t.luma_log2_weight_denom == 7);
  CHECK(t.delta_chroma_log2_weight_denom == -1);
  CHECK(t.ChromaLog2WeightDenom == 6);
  CHECK(t.l0.luma_weight_flag[0]);
  CHECK_FALSE(t.l0.chroma_weight_flag[0]);
  CHECK(t.l0.delta_luma_weight[0] == -1);
  CHECK(t.l0.luma_offset[0] == 0);
  CHECK(t.l0.LumaWeight[0] == 127);
  CHECK(t.l0.ChromaWeight[0][0] == 64);
  CHECK(sh.five_minus_max_num_merge_cand == 0);
  CHECK(sh.slice_qp_delta == 14);
}

}  // namespace

TEST_CASE("h265_slice_wp_a_fixture: first P slice pred_weight_table equals trace_headers (rebuilt units)",
          "[syntax][h265][fast]") {
  SliceWalker walk;
  walk.add(from_hex(kWpaSps));
  walk.add(from_hex(kWpaPps));
  std::vector<uint8_t> slice = from_hex(kWpaSlice5);
  slice.push_back(0x5A);
  slice.push_back(0x5A);
  walk.add(slice);
  REQUIRE(walk.slices.size() == 1);
  check_wpa_first_p(walk.slices[0].sh);
}

TEST_CASE("h265_slice_wp_a: pred_weight_table of the P slices equals trace_headers",
          "[syntax][h265][fast][needs-corpus]") {
  SliceWalker walk;
  walk.add_file(ovt::test::corpus_or_skip("jctvc_WP_A_Toshiba_3"));
  std::vector<const SliceWalker::Slice*> p;
  for (const auto& s : walk.slices) {
    if (s.sh.slice_type == h265::kSliceP) {
      p.push_back(&s);
    }
  }
  REQUIRE(p.size() >= 5);
  check_wpa_first_p(p[0]->sh);
  // unit 7: two L0 entries, only entry 1 weighted
  const h265::SliceSegmentHeader& s7 = p[1]->sh;
  CHECK(s7.num_ref_idx_l0_active_minus1 == 1);
  REQUIRE(s7.pred_weight_table.has_value());
  CHECK(s7.pred_weight_table->luma_log2_weight_denom == 6);
  CHECK(s7.pred_weight_table->delta_chroma_log2_weight_denom == 1);
  CHECK_FALSE(s7.pred_weight_table->l0.luma_weight_flag[0]);
  CHECK(s7.pred_weight_table->l0.luma_weight_flag[1]);
  CHECK(s7.pred_weight_table->l0.delta_luma_weight[1] == 0);
  CHECK(s7.pred_weight_table->l0.luma_offset[1] == -2);
  // unit 13: PPS default of four L0 entries, all chroma-weighted with the extreme offsets
  const h265::SliceSegmentHeader& s13 = p[4]->sh;
  CHECK_FALSE(s13.num_ref_idx_active_override_flag);
  CHECK(s13.num_ref_idx_l0_active_minus1 == 3);
  REQUIRE(s13.pred_weight_table.has_value());
  const h265::PredWeightTable& t = *s13.pred_weight_table;
  CHECK(t.luma_log2_weight_denom == 1);
  CHECK(t.ChromaLog2WeightDenom == 1);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(t.l0.luma_weight_flag[i] == (i == 2));
    CHECK(t.l0.chroma_weight_flag[i]);
    for (unsigned j = 0; j < 2; ++j) {
      CHECK(t.l0.delta_chroma_weight[i][j] == -127);
      CHECK(t.l0.delta_chroma_offset[i][j] == -512);
      CHECK(t.l0.ChromaWeight[i][j] == 2 - 127);
      CHECK(t.l0.ChromaOffset[i][j] == 127);  // 128 - ((128 * -125) >> 1) - 512, clipped
    }
  }
  CHECK(t.l0.luma_offset[2] == -2);
}

// ---- acceptance test 2: RPS_A..F, st_ref_pic_set of the first pictures ---------------------------------------------

namespace {

struct ExpectedRps {
  std::vector<int32_t> s0;
  std::vector<bool> used0;
  std::vector<int32_t> s1;
  std::vector<bool> used1;
};

// SPEC-ASSUMPTION(#391): AT2 names HM's trace as the oracle and the first 8 pictures; the committed goldens are the
// gstparse ones (GstH265ShortTermRefPicSet of the slice, or of the SPS set short_term_ref_pic_set_idx), which cover
// the first 15 NAL units: pictures 1..5 after the IDR (picture 0, empty RPS).
void check_rps_stream(std::string_view id, const std::vector<ExpectedRps>& expected) {
  SliceWalker walk;
  walk.add_file(ovt::test::corpus_or_skip(id));
  const auto firsts = walk.first_slices(expected.size() + 1);
  REQUIRE(firsts.size() == expected.size() + 1);
  CHECK(firsts[0]->nal.is_irap);
  for (size_t p = 0; p < expected.size(); ++p) {
    CAPTURE(id, p + 1);
    const SliceWalker::Slice& s = *firsts[p + 1];
    const h265::StRefPicSet* rps = h265::current_st_ref_pic_set(s.sh, walk.sps_of(s));
    REQUIRE(rps != nullptr);
    const ExpectedRps& e = expected[p];
    REQUIRE(rps->NumNegativePics == e.s0.size());
    REQUIRE(rps->NumPositivePics == e.s1.size());
    for (size_t i = 0; i < e.s0.size(); ++i) {
      CHECK(rps->DeltaPocS0[i] == e.s0[i]);
      CHECK(rps->UsedByCurrPicS0[i] == e.used0[i]);
    }
    for (size_t i = 0; i < e.s1.size(); ++i) {
      CHECK(rps->DeltaPocS1[i] == e.s1[i]);
      CHECK(rps->UsedByCurrPicS1[i] == e.used1[i]);
    }
  }
}

}  // namespace

TEST_CASE("h265_slice_rps: DeltaPocS0/S1 and UsedByCurrPic of RPS_A..F equal the recorded RPS",
          "[syntax][h265][fast][needs-corpus]") {
  const bool T = true;
  const bool F = false;
  SECTION("RPS_A_docomo_5 (SPS sets)") {
    check_rps_stream("jctvc_RPS_A_docomo_5", {{{-8}, {T}, {}, {}},
                                              {{-4}, {T}, {4}, {T}},
                                              {{-2}, {T}, {2, 6}, {T, T}},
                                              {{-1}, {T}, {1, 3, 7}, {T, T, T}},
                                              {{-1, -3}, {T, T}, {1, 5}, {T, T}}});
  }
  SECTION("RPS_B_qualcomm_5 (slice sets)") {
    check_rps_stream("jctvc_RPS_B_qualcomm_5", {{{-5}, {T}, {}, {}},
                                                {{-3, -8}, {T, T}, {}, {}},
                                                {{-1, -6}, {T, T}, {}, {}},
                                                {{-1}, {T}, {4, 5}, {T, T}},
                                                {{-1, -2, -6, -7}, {T, T, T, T}, {}, {}}});
  }
  SECTION("RPS_C_ericsson_5 (inter-RPS prediction, unused entries)") {
    check_rps_stream("jctvc_RPS_C_ericsson_5", {{{-16}, {T}, {}, {}},
                                                {{-8}, {T}, {8}, {T}},
                                                {{-4, -12}, {T, F}, {4}, {T}},
                                                {{-3}, {T}, {5, 9, 13}, {T, F, T}},
                                                {{-3, -7, -12, -15}, {F, F, F, T}, {1}, {T}}});
  }
  SECTION("RPS_D_ericsson_6") {
    check_rps_stream("jctvc_RPS_D_ericsson_6", {{{-10}, {T}, {}, {}},
                                                {{-10, -20}, {T, F}, {}, {}},
                                                {{-10, -30}, {T, F}, {}, {}},
                                                {{-10, -20, -40}, {T, T, T}, {}, {}},
                                                {{-20}, {T}, {}, {}}});
  }
  SECTION("RPS_E_qualcomm_5") {
    check_rps_stream("jctvc_RPS_E_qualcomm_5", {{{-5}, {T}, {}, {}},
                                                {{-3, -8}, {T, T}, {}, {}},
                                                {{-1, -6}, {T, T}, {2}, {F}},
                                                {{-1}, {T}, {4, 5, 7}, {T, T, T}},
                                                {{-1, -2, -6, -7}, {T, T, T, T}, {1}, {T}}});
  }
  SECTION("RPS_F_docomo_2") {
    check_rps_stream("jctvc_RPS_F_docomo_2", {{{-8}, {T}, {}, {}},
                                              {{-4}, {T}, {4}, {T}},
                                              {{-2}, {T}, {2, 6}, {T, T}},
                                              {{-1}, {T}, {1, 3, 7}, {T, T, T}},
                                              {{-1, -3}, {F, T}, {1, 5}, {F, F}}});
  }
}

// ---- acceptance test 3: ENTP_A_QUALCOMM_1, entry points
// --------------------------------------------------------------

TEST_CASE("h265_slice_entp_a: substream byte ranges tile slice_segment_data exactly in raw bytes",
          "[syntax][h265][fast][needs-corpus]") {
  SliceWalker walk;
  walk.add_file(ovt::test::corpus_or_skip("jctvc_ENTP_A_QUALCOMM_1"));
  // trace_headers: the first six pictures are single-slice I pictures on 2x2 tiles, offset_len_minus1 = 15.
  const std::array<std::array<uint32_t, 3>, 6> expected = {{{16163, 15324, 43860},
                                                            {16087, 15437, 43597},
                                                            {16148, 15247, 43382},
                                                            {16136, 15341, 43443},
                                                            {16084, 15415, 43226},
                                                            {16130, 15262, 43503}}};
  REQUIRE(walk.slices.size() >= expected.size());
  for (size_t p = 0; p < expected.size(); ++p) {
    CAPTURE(p);
    const SliceWalker::Slice& s = walk.slices[p];
    CHECK(s.sh.num_entry_point_offsets == 3);
    CHECK(s.sh.offset_len_minus1 == 15);
    REQUIRE(s.sh.entry_point_offset_minus1.size() == 3);
    for (size_t k = 0; k < 3; ++k) {
      CHECK(s.sh.entry_point_offset_minus1[k] == expected[p][k]);
    }
    const auto subs = h265::entry_point_substreams(s.sh, s.epb_positions, s.nal_size);
    REQUIRE(subs.has_value());
    REQUIRE(subs->size() == 4);
    // The first substream starts at the first raw byte of slice_segment_data(), each one starts where the previous
    // ends, the sizes are entry_point_offset_minus1 + 1 and the last one ends with the unit.
    const uint64_t data_first = ovt::rbsp_bit_to_raw_bit(s.sh.slice_segment_data_rbsp_byte * 8, s.epb_positions) / 8;
    CHECK((*subs)[0].first_byte == data_first);
    for (size_t k = 0; k < 3; ++k) {
      CHECK((*subs)[k].size == uint64_t{expected[p][k]} + 1);
      CHECK((*subs)[k + 1].first_byte == (*subs)[k].first_byte + (*subs)[k].size);
    }
    CHECK((*subs)[3].first_byte + (*subs)[3].size == s.nal_size);
    CHECK((*subs)[3].size > 0);
  }
}

// ---- acceptance test 4: DSLICE_A_HHI_5, dependent slice segments ---------------------------------------------------

TEST_CASE("h265_slice_dslice_a: dependent segments inherit slice_type and SliceQpY from the independent segment",
          "[syntax][h265][fast][needs-corpus]") {
  SliceWalker walk;
  walk.add_file(ovt::test::corpus_or_skip("jctvc_DSLICE_A_HHI_5"));
  // trace_headers: picture 0 (IDR) is three independent slices at CTB rows 0, 6 and 12 (addresses 0, 180, 360), each
  // followed by five dependent segments one CTB row (30 CTBs) apart; slice_type 2 and slice_qp_delta 6 with
  // init_qp_minus26 0. Picture 1 has the same layout with slice_type 0 (B) and slice_qp_delta 7.
  REQUIRE(walk.slices.size() >= 34);
  for (size_t i = 0; i < 34; ++i) {
    CAPTURE(i);
    const SliceWalker::Slice& s = walk.slices[i];
    const size_t in_pic = i % 17;
    const bool independent = in_pic % 6 == 0;
    const auto pic = static_cast<uint32_t>(i / 17);
    CHECK(s.sh.first_slice_segment_in_pic_flag == (in_pic == 0));
    CHECK(s.sh.dependent_slice_segment_flag == !independent);
    CHECK(s.sh.copied_from_independent == !independent);
    CHECK(s.sh.slice_segment_address == static_cast<uint32_t>(in_pic * 30));
    CHECK(s.sh.SliceAddrRs == static_cast<uint32_t>((in_pic / 6) * 180));
    CHECK(s.sh.slice_type == (pic == 0 ? h265::kSliceI : h265::kSliceB));
    CHECK(s.sh.SliceQpY == (pic == 0 ? 32 : 33));
  }
}
