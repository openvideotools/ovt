// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.109a: H.264 slice_header() with ref_pic_list_modification(), pred_weight_table() and dec_ref_pic_marking()
// (chapter 7 §7.3.4 rows 24–27). WI-1.109b adds the hand-made bit-string tests (field pictures, POC types, SP / SI,
// slice groups, MVC, range errors) and the AUD / filler / data partition tests to this file.
//
// Oracles:
// - x264 fixture (FFmpeg 6.1.1 libx264, 2026-11-14, lavfi testsrc2 176x144): `wp.264` (fade-in, weightp=2, ref=3,
//   no B frames); WI-1.109b adds `pyramid.264` (bframes=3, b-pyramid=normal: MMCO, idc 1). The expected syntax trees
//   are the `ffmpeg -i <file> -c:v copy -bsf:v trace_headers -f null -` lines of the same units (bit offset, name with
//   indices, bits, value), with the slice_data lines (cabac_alignment_one_bit) dropped. Slice NAL units are cut
//   after 40 bytes: slice_data() is opaque to the parser (§7.3.1 rule 2).
// - The acceptance tests (CVWP1_TOSHIBA_E, MR9_BT_B) use the recorded trace_headers goldens committed under
//   tests/golden/h264/ (WI-0.552): the fast variants rebuild the SPS, PPS and slice header bit-exactly from the
//   golden's (bit_offset, bit_length, value) records (each element re-encoded with its descriptor and checked
//   against the recorded length); the [needs-corpus] variants parse the conformance streams themselves.

#include "ovt/cxx/syntax/h264/slice_header.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
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
namespace h264 = ovt::syntax::h264;

// ---- x264 fixtures with their trace_headers oracle -------------------------------------------------------------

// wp.264 unit 0 (nal_unit_type 7, 23 bytes)
constexpr std::string_view kWpSps = "6764000bacb20589d808800000030080000019078a1524";
// wp.264 unit 1 (nal_unit_type 8, 6 bytes)
constexpr std::string_view kWpPps = "68ebc3cb22c0";
// wp.264 unit 5 (nal_unit_type 1, 600 bytes, first 40)
constexpr std::string_view kWpP = "419a52784210c86c0fc1f90020240080c0fc0840ffd2814049be9d2a255b2dfaaf5c040039f7dcdb";
constexpr std::string_view kWpPTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 10 = 2
3 nal_unit_type 00001 = 1
8 first_mb_in_slice 1 = 0
9 slice_type 00110 = 5
14 pic_parameter_set_id 1 = 0
15 frame_num 0010 = 2
19 num_ref_idx_active_override_flag 1 = 1
20 num_ref_idx_l0_active_minus1 00100 = 3
25 ref_pic_list_modification_flag_l0 1 = 1
26 modification_of_pic_nums_idc 1 = 0
27 abs_diff_pic_num_minus1 1 = 0
28 modification_of_pic_nums_idc 1 = 0
29 abs_diff_pic_num_minus1 000010000 = 15
38 modification_of_pic_nums_idc 1 = 0
39 abs_diff_pic_num_minus1 000010000 = 15
48 modification_of_pic_nums_idc 1 = 0
49 abs_diff_pic_num_minus1 1 = 0
50 modification_of_pic_nums_idc 00100 = 3
55 luma_log2_weight_denom 00110 = 5
60 chroma_log2_weight_denom 1 = 0
61 luma_weight_l0_flag[0] 1 = 1
62 luma_weight_l0[0] 0000001111110 = 63
75 luma_offset_l0[0] 000011111 = -15
84 chroma_weight_l0_flag[0] 1 = 1
85 chroma_weight_l0[0][0] 00100 = 2
90 chroma_offset_l0[0][0] 00000000100000001 = -128
107 chroma_weight_l0[0][1] 00100 = 2
112 chroma_offset_l0[0][1] 00000000100000001 = -128
129 luma_weight_l0_flag[1] 1 = 1
130 luma_weight_l0[1] 0000001111110 = 63
143 luma_offset_l0[1] 00000100001 = -16
154 chroma_weight_l0_flag[1] 0 = 0
155 luma_weight_l0_flag[2] 0 = 0
156 chroma_weight_l0_flag[2] 0 = 0
157 luma_weight_l0_flag[3] 0 = 0
158 chroma_weight_l0_flag[3] 0 = 0
159 adaptive_ref_pic_marking_mode_flag 0 = 0
160 cabac_init_idc 1 = 0
161 slice_qp_delta 1 = 0
162 disable_deblocking_filter_idc 1 = 0
163 slice_alpha_c0_offset_div2 1 = 0
164 slice_beta_offset_div2 1 = 0
)";

// ---- fixtures rebuilt from the committed trace_headers goldens (tests/golden/h264/*.trace_headers.json.gz) ------
// Each element of the golden's units re-encoded with its descriptor (ue/se/u(n)) at the recorded bit offset and
// length, then rbsp_stop_one_bit and alignment (the golden stops before them), emulation prevention added.

// jvt_CVWP1_TOSHIBA_E units 0 (SPS), 3 (PPS id 0 with weighted_pred_flag 1, re-sent before the P picture) and 4 (the
// first P slice, 113 header bits).
constexpr std::string_view kCvwp1Sps = "274d4014d981609440";
constexpr std::string_view kCvwp1Pps = "28cf1b88";
constexpr std::string_view kCvwp1P = "219a218f02d81be02cc3802000e4c0";
constexpr std::string_view kCvwp1PTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 01 = 1
3 nal_unit_type 00001 = 1
8 first_mb_in_slice 1 = 0
9 slice_type 00110 = 5
14 pic_parameter_set_id 1 = 0
15 frame_num 0001 = 1
19 num_ref_idx_active_override_flag 0 = 0
20 ref_pic_list_modification_flag_l0 0 = 0
21 luma_log2_weight_denom 00110 = 5
26 chroma_log2_weight_denom 00111 = 6
31 luma_weight_l0_flag[0] 1 = 1
32 luma_weight_l0[0] 0000001011011 = -45
45 luma_offset_l0[0] 0000001101111 = -55
58 chroma_weight_l0_flag[0] 1 = 1
59 chroma_weight_l0[0][0] 000000010110011 = -89
74 chroma_offset_l0[0][0] 000011100 = 14
83 chroma_weight_l0[0][1] 000000010000000 = 64
98 chroma_offset_l0[0][1] 0000001110010 = 57
111 adaptive_ref_pic_marking_mode_flag 0 = 0
112 slice_qp_delta 1 = 0
)";
// jvt_MR9_BT_B units 0 (SPS: POC type 1, MBAFF, VUI), 1 (PPS) and 3 (the P slice with
// memory_management_control_operation 4, 33 header bits).
constexpr std::string_view kMr9Sps = "674d4015d0a90c38611a01e385a110";
constexpr std::string_view kMr9Pps = "68f9608a20";
constexpr std::string_view kMr9P = "41e2f4afc0";
constexpr std::string_view kMr9PTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 10 = 2
3 nal_unit_type 00001 = 1
8 first_mb_in_slice 1 = 0
9 slice_type 1 = 0
10 pic_parameter_set_id 1 = 0
11 frame_num 0001 = 1
15 field_pic_flag 0 = 0
16 delta_pic_order_cnt[0] 1 = 0
17 delta_pic_order_cnt[1] 1 = 0
18 num_ref_idx_active_override_flag 1 = 1
19 num_ref_idx_l0_active_minus1 1 = 0
20 ref_pic_list_modification_flag_l0 0 = 0
21 adaptive_ref_pic_marking_mode_flag 1 = 1
22 memory_management_control_operation 00101 = 4
27 max_long_term_frame_idx_plus1 011 = 2
30 memory_management_control_operation 1 = 0
31 cabac_init_idc 1 = 0
32 slice_qp_delta 1 = 0
)";

std::vector<uint8_t> from_hex(std::string_view hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

const SyntaxNodeData* find_node(const SyntaxNodeData* n, std::string_view name, int array_index = -1) {
  if (n == nullptr) {
    return nullptr;
  }
  if (n->name == name && !n->array_element && (array_index < 0 || n->array_index == array_index)) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const SyntaxNodeData* hit = find_node(c, name, array_index); hit != nullptr) {
      return hit;
    }
  }
  return nullptr;
}

std::vector<std::string> diag_codes(const SyntaxNodeData* n) {
  std::vector<std::string> out;
  if (n == nullptr) {
    return out;
  }
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    out.emplace_back(d->code);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    const auto sub = diag_codes(c);
    out.insert(out.end(), sub.begin(), sub.end());
  }
  return out;
}

/// One parsed NAL unit.
struct Unit {
  std::vector<uint8_t> rbsp;
  h264::NalHeader header;
  std::optional<h264::SliceHeader> slice;
  SyntaxNodeBuffer tree;
  std::optional<ParseError> error;
  [[nodiscard]] std::string trace() const { return ovt::syntax::to_trace(tree, rbsp); }
  [[nodiscard]] const SyntaxNodeData* node(std::string_view name, int idx = -1) const {
    return find_node(tree.root(), name, idx);
  }
};

/// A decoder-side parameter-set store: the SPS / PPS parsed so far, as ActiveSets (WI-1.111) will hold them.
class Stream {
 public:
  Stream() {
    sets_.on_activate = [this](const h264::NalHeader&, const h264::Pps& pps, const h264::Sps& sps) {
      activations.push_back({pps.pic_parameter_set_id, sps.seq_parameter_set_id});
    };
  }

  /// Parses an SPS (type 7, or 15 stored as subset SPS when `subset`) or a PPS (type 8) and stores it.
  void add(const std::vector<uint8_t>& nal, bool subset = false) {
    const auto rbsp = ovt::rbsp_from_nal(nal).rbsp;
    BitReader br(rbsp);
    SyntaxBuilder b(br);
    const auto h = h264::parse_nal_header(br, b);
    if (h.nal_unit_type == 8) {
      const auto& p = ppss_.emplace_back(h264::parse_pps(br, b, sets_.sps));
      sets_.pps.entries.at(p.pic_parameter_set_id) = &p;
    } else {
      const auto& s = spss_.emplace_back(h264::parse_sps(br, b));
      (subset ? sets_.subset_sps : sets_.sps).entries.at(s.seq_parameter_set_id) = &s;
    }
  }
  void add_hex(std::string_view hex) { add(from_hex(hex)); }

  /// Parses a slice NAL unit (NAL header, then slice_layer_*_rbsp()).
  [[nodiscard]] Unit parse(const std::vector<uint8_t>& nal) const {
    Unit u;
    u.rbsp = ovt::rbsp_from_nal(nal).rbsp;
    BitReader br(u.rbsp);
    SyntaxBuilder b(br);
    try {
      u.header = h264::parse_nal_header(br, b);
      u.slice = h264::parse_slice_layer(br, b, u.header, sets_);
    } catch (const ParseError& e) {
      u.error = e;
    }
    u.tree = b.finish();
    u.tree.set_root_name("nal_unit");
    return u;
  }
  [[nodiscard]] Unit parse_hex(std::string_view hex) const { return parse(from_hex(hex)); }

  struct Activation {
    uint32_t pps_id;
    uint32_t sps_id;
  };
  std::vector<Activation> activations;

 private:
  std::deque<h264::Sps> spss_;
  std::deque<h264::Pps> ppss_;
  h264::ParameterSets sets_;
};

}  // namespace

// ---- x264 streams against trace_headers -------------------------------------------------------------------------

TEST_CASE("h264_slice_header_weighted_p_x264", "[syntax][h264][fast]") {
  Stream s;
  s.add_hex(kWpSps);
  s.add_hex(kWpPps);

  SECTION("P slice with ref_pic_list_modification and pred_weight_table") {
    const Unit u = s.parse_hex(kWpP);
    REQUIRE_FALSE(u.error.has_value());
    REQUIRE(u.slice.has_value());
    CHECK(u.trace() == kWpPTrace);
    const auto& sh = *u.slice;
    CHECK_FALSE(sh.IdrPicFlag);
    CHECK(sh.frame_num == 2);
    CHECK(sh.CurrPicNum == 2);
    CHECK(sh.num_ref_idx_active_override_flag);
    CHECK(sh.num_ref_idx_l0_active_minus1 == 3);
    const auto& m = sh.ref_pic_list_modification;
    CHECK_FALSE(m.mvc);
    CHECK(m.ref_pic_list_modification_flag_l0);
    REQUIRE(m.ops_l0.size() == 4);  // the terminating idc 3 is not stored
    CHECK(m.ops_l0[1].abs_diff_pic_num_minus1 == 15);
    CHECK(m.ops_l1.empty());
    REQUIRE(sh.has_pred_weight_table);
    const auto& t = sh.pred_weight_table;
    CHECK(t.luma_log2_weight_denom == 5);
    CHECK(t.chroma_log2_weight_denom == 0);
    CHECK(t.l0.luma_weight[0] == 63);
    CHECK(t.l0.luma_offset[0] == -15);
    CHECK(t.l0.chroma_offset[0][1] == -128);
    // inferred values (§7.4.3.2): weight 2^denom, offset 0
    CHECK_FALSE(t.l0.chroma_weight_flag[1]);
    CHECK(t.l0.chroma_weight[1][0] == 1);
    CHECK(t.l0.chroma_offset[1][1] == 0);
    CHECK_FALSE(t.l0.luma_weight_flag[3]);
    CHECK(t.l0.luma_weight[3] == 32);
    CHECK(t.l0.luma_offset[3] == 0);
    CHECK_FALSE(sh.dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag);
    CHECK_FALSE(sh.dec_ref_pic_marking.MaxLongTermFrameIdx.has_value());
    CHECK(sh.SliceQPY == 23);
    CHECK(sh.header_bits == 165);
    const auto* w = u.node("chroma_offset_l0");
    REQUIRE(w != nullptr);
    CHECK(w->array_index == 0);
    CHECK(diag_codes(u.tree.root()).empty());
  }
  SECTION("activation callback fires once per slice with the PPS and SPS ids") {
    (void)s.parse_hex(kWpP);
    REQUIRE(s.activations.size() == 1);
    CHECK(s.activations[0].pps_id == 0);
    CHECK(s.activations[0].sps_id == 0);
  }
}

// ---- acceptance tests 1 and 2 ----------------------------------------------------------------------------------

namespace {

// AT1 expected values: tests/golden/h264/jvt_CVWP1_TOSHIBA_E.trace_headers.json.gz, unit 4 (first P slice).
void check_cvwp1_first_p(const h264::SliceHeader& sh) {
  CHECK(sh.slice_type_mod5 == h264::kSliceP);
  CHECK(sh.frame_num == 1);
  CHECK_FALSE(sh.num_ref_idx_active_override_flag);
  CHECK(sh.num_ref_idx_l0_active_minus1 == 0);  // PPS num_ref_idx_l0_default_active_minus1
  REQUIRE(sh.has_pred_weight_table);
  const auto& t = sh.pred_weight_table;
  CHECK(t.luma_log2_weight_denom == 5);
  CHECK(t.chroma_log2_weight_denom == 6);
  CHECK(t.l0.luma_weight_flag[0]);
  CHECK(t.l0.luma_weight[0] == -45);
  CHECK(t.l0.luma_offset[0] == -55);
  CHECK(t.l0.chroma_weight[0][0] == -89);
  CHECK(t.l0.chroma_offset[0][0] == 14);
  CHECK(t.l0.chroma_weight[0][1] == 64);
  CHECK(t.l0.chroma_offset[0][1] == 57);
  CHECK(sh.header_bits == 113);
}

// AT2 expected values: tests/golden/h264/jvt_MR9_BT_B.trace_headers.json.gz, unit 3 (the P slice with MMCO). The
// recorded JM trace (jvt_MR9_BT_B.jm-trace.json.gz) keeps no MMCO fields, so the op list comes from trace_headers;
// MaxLongTermFrameIdx = max_long_term_frame_idx_plus1 − 1 (§8.2.5.4.4).
void check_mr9_mmco(const h264::SliceHeader& sh) {
  CHECK(sh.slice_type_mod5 == h264::kSliceP);
  CHECK(sh.frame_num == 1);
  CHECK_FALSE(sh.field_pic_flag);
  CHECK(sh.MbaffFrameFlag);
  CHECK(sh.structure == h264::PictureStructure::Mbaff);
  CHECK(sh.delta_pic_order_cnt[0] == 0);
  CHECK(sh.num_ref_idx_l0_active_minus1 == 0);
  const auto& d = sh.dec_ref_pic_marking;
  REQUIRE(d.adaptive_ref_pic_marking_mode_flag);
  REQUIRE(d.mmco.size() == 1);
  CHECK(d.mmco[0].memory_management_control_operation == 4);
  CHECK(d.mmco[0].max_long_term_frame_idx_plus1 == 2);
  REQUIRE(d.MaxLongTermFrameIdx.has_value());
  CHECK(*d.MaxLongTermFrameIdx == 1);
  CHECK(sh.cabac_init_idc == 0);
  CHECK(sh.SliceQPY == 28);
}

/// Parses the stream until `pick` accepts a slice header, feeding every SPS and PPS before it.
template <class Pick>
std::optional<h264::SliceHeader> first_slice(std::string_view corpus_id, Pick pick) {
  const auto path = ovt::test::corpus_or_skip(corpus_id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  Stream s;
  std::optional<h264::SliceHeader> out;
  ovt::syntax::scan_annexb(*file, [&](const ovt::syntax::ScannedUnit& u) {
    if (out || u.bytes.empty()) {
      return;
    }
    const std::vector<uint8_t> nal(u.bytes.begin(), u.bytes.end());
    const unsigned type = nal[0] & 0x1FU;
    if (type == 7 || type == 8) {
      s.add(nal);
    } else if (type == 1 || type == 5) {
      const Unit parsed = s.parse(nal);
      REQUIRE_FALSE(parsed.error.has_value());
      if (pick(*parsed.slice)) {
        out = parsed.slice;
      }
    }
  });
  return out;
}

}  // namespace

TEST_CASE("h264_slice_pwt_unit", "[syntax][h264][fast]") {
  Stream s;
  s.add_hex(kCvwp1Sps);
  s.add_hex(kCvwp1Pps);
  const Unit u = s.parse_hex(kCvwp1P);
  REQUIRE_FALSE(u.error.has_value());
  CHECK(u.trace() == kCvwp1PTrace);
  check_cvwp1_first_p(*u.slice);
  CHECK(diag_codes(u.tree.root()).empty());
}

TEST_CASE("h264_slice_mmco_unit", "[syntax][h264][fast]") {
  Stream s;
  s.add_hex(kMr9Sps);
  s.add_hex(kMr9Pps);
  const Unit u = s.parse_hex(kMr9P);
  REQUIRE_FALSE(u.error.has_value());
  CHECK(u.trace() == kMr9PTrace);
  check_mr9_mmco(*u.slice);
  CHECK(diag_codes(u.tree.root()).empty());
}

TEST_CASE("h264_slice_pwt_unit_corpus_cvwp1", "[syntax][h264][fast][needs-corpus]") {
  const auto sh =
      first_slice("jvt_CVWP1_TOSHIBA_E", [](const h264::SliceHeader& h) { return h.slice_type_mod5 == h264::kSliceP; });
  REQUIRE(sh.has_value());
  check_cvwp1_first_p(*sh);
}

TEST_CASE("h264_slice_mmco_unit_corpus_mr9", "[syntax][h264][fast][needs-corpus]") {
  const auto sh = first_slice("jvt_MR9_BT_B", [](const h264::SliceHeader& h) {
    return h.has_dec_ref_pic_marking && !h.dec_ref_pic_marking.mmco.empty();
  });
  REQUIRE(sh.has_value());
  check_mr9_mmco(*sh);
}

TEST_CASE("h264_slice_header_activation_errors", "[syntax][h264][fast]") {
  SECTION("missing PPS: SYN-H264-0010, partial tree kept, no activation") {
    Stream s;
    s.add_hex(kWpSps);
    const Unit u = s.parse_hex(kWpP);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
    CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("missing PPS 0"));
    CHECK(u.node("pic_parameter_set_id") != nullptr);
    CHECK(u.node("frame_num") == nullptr);
    CHECK(s.activations.empty());
  }
  SECTION("PPS present, its SPS missing: SYN-H264-0010") {
    Stream s;
    s.add_hex(kWpPps);  // parsed provisionally
    const Unit u = s.parse_hex(kWpP);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
    CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("SPS 0 is missing"));
  }
  SECTION("the subset SPS table serves NAL 20 / 21 only") {
    Stream s;
    s.add(from_hex(kWpSps), true);
    s.add_hex(kWpPps);
    const Unit u = s.parse_hex(kWpP);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
  }
  SECTION("a header cut short: BR-0001 with the partial tree") {
    Stream s;
    s.add_hex(kWpSps);
    s.add_hex(kWpPps);
    auto bytes = from_hex(kWpP);
    bytes.resize(5);
    const Unit u = s.parse(bytes);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "BR-0001");
    CHECK(u.node("ref_pic_list_modification") != nullptr);
    CHECK(u.node("pred_weight_table") == nullptr);
  }
}
