// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 slice_segment_header(), ref_pic_lists_modification() and pred_weight_table() tests (chapter 7 §7.3.5
// rows 10–12, WI-1.113a). WI-1.113b adds the bit-level edge cases, the entry-point substream mapping and acceptance
// tests 2 (RPS_A..F) and 3 (ENTP_A).
//
// Expected values of the corpus tests are hard-coded from the recorded oracle outputs committed as goldens:
//   tests/golden/h265/jctvc_WP_A_Toshiba_3.trace_headers.json.gz    (FFmpeg trace_headers, acceptance test 1)
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

// ---- dependent slice segments (hand-built) -------------------------------------------------------------------------

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
