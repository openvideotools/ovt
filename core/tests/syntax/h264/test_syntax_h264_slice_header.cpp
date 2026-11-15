// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.109a–c: H.264 slice_header() with ref_pic_list_modification(), pred_weight_table() and dec_ref_pic_marking(),
// the access unit delimiter, end of sequence / stream, filler data, the slice data partitions, the SPS extension,
// the subset SPS (MVC extension) and the prefix NAL unit (chapter 7 §7.3.4 rows 19–28).
//
// Oracles:
// - x264 fixtures (FFmpeg 6.1.1 libx264, 2026-11-14, lavfi testsrc2 176x144): `wp.264` (fade-in, weightp=2, ref=3,
//   no B frames) and `pyramid.264` (bframes=3, b-pyramid=normal, ref=2, weightb=1). The expected syntax trees are
//   the `ffmpeg -i <file> -c:v copy -bsf:v trace_headers -f null -` lines of the same units (bit offset, name with
//   indices, bits, value), with the slice_data lines (cabac_alignment_one_bit) dropped. Slice NAL units are cut
//   after 40 bytes: slice_data() is opaque to the parser (§7.3.1 rule 2).
// - The acceptance tests (CVWP1_TOSHIBA_E, MR9_BT_B) use the recorded trace_headers goldens committed under
//   tests/golden/h264/ (WI-0.552): the fast variants rebuild the SPS, PPS and slice header bit-exactly from the
//   golden's (bit_offset, bit_length, value) records (each element re-encoded with its descriptor and checked
//   against the recorded length); the [needs-corpus] variants parse the conformance streams themselves.
// - SPS extension (WI-1.109c): NAL 13 units written by a script and inserted after the SPS of `wp.264`; the expected
//   trees are the FFmpeg 6.1.1 trace_headers lines of those units (2026-11-15).
// - Subset SPS (WI-1.109c, acceptance test 3): the first subset SPS of jvt_MVCNV_1 (hex and values recorded on the
//   Mac Mini with GStreamer 1.24.2 gst_h264_parser_parse_subset_sps(), probe
//   /mnt/project-files/wi1109-gst-subset-sps-probe.c, 2026-11-15) and two script-written MVC subset SPS units whose
//   expected values are GStreamer 1.24.2's parse of the same bytes (Ubuntu 24.04 libgstcodecparsers, 2026-11-15; the
//   same probe agreed with this parser on 700 random MVC subset SPS units).
// - Everything else uses hand-written bit strings.

#include "ovt/cxx/syntax/h264/slice_header.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/aud.hpp"
#include "ovt/cxx/syntax/h264/data_partition.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/prefix_nal.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h264/sps_ext.hpp"
#include "ovt/cxx/syntax/h264/subset_sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"
#include "test_support/corpus.h"

namespace {

using ovt::BitReader;
using ovt::BitSpan;
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
// wp.264 unit 3 (nal_unit_type 5, 46 bytes, first 40)
constexpr std::string_view kWpIdr = "6588840afffef6737c0a6b6db0952e0576f766a3e5d092e5fb59dde3385831f8589c96752001b800";
constexpr std::string_view kWpIdrTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 11 = 3
3 nal_unit_type 00101 = 5
8 first_mb_in_slice 1 = 0
9 slice_type 0001000 = 7
16 pic_parameter_set_id 1 = 0
17 frame_num 0000 = 0
21 idr_pic_id 1 = 0
22 no_output_of_prior_pics_flag 0 = 0
23 long_term_reference_flag 0 = 0
24 slice_qp_delta 000010101 = -10
33 disable_deblocking_filter_idc 1 = 0
34 slice_alpha_c0_offset_div2 1 = 0
35 slice_beta_offset_div2 1 = 0
)";
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
// pyramid.264 unit 0 (nal_unit_type 7, 24 bytes)
constexpr std::string_view kPyrSps = "6764000bacd942c4ec0440000003004000000c83c50a6580";
// pyramid.264 unit 1 (nal_unit_type 8, 6 bytes)
constexpr std::string_view kPyrPps = "68eae3cb22c0";
// pyramid.264 unit 8 (nal_unit_type 1, 1341 bytes, first 40)
constexpr std::string_view kPyrP = "419a685ea105265308eff3300fae05354e43e29d14ba1d7578a749248504eb757ff248d011700900";
constexpr std::string_view kPyrPTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 10 = 2
3 nal_unit_type 00001 = 1
8 first_mb_in_slice 1 = 0
9 slice_type 00110 = 5
14 pic_parameter_set_id 1 = 0
15 frame_num 0011 = 3
19 pic_order_cnt_lsb 010000 = 16
25 num_ref_idx_active_override_flag 1 = 1
26 num_ref_idx_l0_active_minus1 011 = 2
29 ref_pic_list_modification_flag_l0 1 = 1
30 modification_of_pic_nums_idc 1 = 0
31 abs_diff_pic_num_minus1 010 = 1
34 modification_of_pic_nums_idc 1 = 0
35 abs_diff_pic_num_minus1 000010000 = 15
44 modification_of_pic_nums_idc 010 = 1
47 abs_diff_pic_num_minus1 1 = 0
48 modification_of_pic_nums_idc 00100 = 3
53 luma_log2_weight_denom 1 = 0
54 chroma_log2_weight_denom 1 = 0
55 luma_weight_l0_flag[0] 0 = 0
56 chroma_weight_l0_flag[0] 0 = 0
57 luma_weight_l0_flag[1] 1 = 1
58 luma_weight_l0[1] 010 = 1
61 luma_offset_l0[1] 011 = -1
64 chroma_weight_l0_flag[1] 0 = 0
65 luma_weight_l0_flag[2] 0 = 0
66 chroma_weight_l0_flag[2] 0 = 0
67 adaptive_ref_pic_marking_mode_flag 0 = 0
68 cabac_init_idc 1 = 0
69 slice_qp_delta 0001110 = 7
76 disable_deblocking_filter_idc 1 = 0
77 slice_alpha_c0_offset_div2 1 = 0
78 slice_beta_offset_div2 1 = 0
)";
// pyramid.264 unit 9 (nal_unit_type 1, 769 bytes, first 40)
constexpr std::string_view kPyrBref =
    "419e8645112c25fffef51023d11cdf02d3ba02e41234ffe06d44e8e878114af818e308dfdf76de87";
constexpr std::string_view kPyrBrefTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 10 = 2
3 nal_unit_type 00001 = 1
8 first_mb_in_slice 1 = 0
9 slice_type 00111 = 6
14 pic_parameter_set_id 1 = 0
15 frame_num 0100 = 4
19 pic_order_cnt_lsb 001100 = 12
25 direct_spatial_mv_pred_flag 1 = 1
26 num_ref_idx_active_override_flag 0 = 0
27 ref_pic_list_modification_flag_l0 0 = 0
28 ref_pic_list_modification_flag_l1 0 = 0
29 adaptive_ref_pic_marking_mode_flag 1 = 1
30 memory_management_control_operation 010 = 1
33 difference_of_pic_nums_minus1 00100 = 3
38 memory_management_control_operation 010 = 1
41 difference_of_pic_nums_minus1 010 = 1
44 memory_management_control_operation 1 = 0
45 cabac_init_idc 1 = 0
46 slice_qp_delta 000010010 = 9
55 disable_deblocking_filter_idc 1 = 0
56 slice_alpha_c0_offset_div2 1 = 0
57 slice_beta_offset_div2 1 = 0
)";
// pyramid.264 unit 7 (nal_unit_type 1, 416 bytes, first 40)
constexpr std::string_view kPyrB = "019e63442dffdfc415cc9cfffc94cffce1fe5615904d13d8f42b7b1b6ff50f4fd00db73716e80151";
constexpr std::string_view kPyrBTrace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 00 = 0
3 nal_unit_type 00001 = 1
8 first_mb_in_slice 1 = 0
9 slice_type 00111 = 6
14 pic_parameter_set_id 1 = 0
15 frame_num 0011 = 3
19 pic_order_cnt_lsb 000110 = 6
25 direct_spatial_mv_pred_flag 1 = 1
26 num_ref_idx_active_override_flag 0 = 0
27 ref_pic_list_modification_flag_l0 0 = 0
28 ref_pic_list_modification_flag_l1 0 = 0
29 cabac_init_idc 1 = 0
30 slice_qp_delta 000010110 = 11
39 disable_deblocking_filter_idc 1 = 0
40 slice_alpha_c0_offset_div2 1 = 0
41 slice_beta_offset_div2 1 = 0
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

/// Writes spec descriptors into a byte vector (MSB first) for the hand-made fixtures.
class BitWriter {
 public:
  void u(unsigned n, uint64_t v) {
    for (unsigned i = n; i-- > 0;) {
      bits_.push_back(((v >> i) & 1U) != 0);
    }
  }
  void flag(bool v) { u(1, v ? 1 : 0); }
  void ue(uint32_t v) {
    const uint64_t code = uint64_t{v} + 1;
    unsigned len = 0;
    while ((code >> len) > 1) {
      ++len;
    }
    u(len, 0);
    u(len + 1, code);
  }
  void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * int64_t{v})); }
  void nal(unsigned ref_idc, unsigned type) {
    u(1, 0);
    u(2, ref_idc);
    u(5, type);
  }
  void trailing() {
    u(1, 1);
    while (bits_.size() % 8 != 0) {
      u(1, 0);
    }
  }
  /// The bytes with emulation prevention added (a NAL unit as found in the stream).
  [[nodiscard]] std::vector<uint8_t> nal_bytes() const {
    std::vector<uint8_t> raw((bits_.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits_.size(); ++i) {
      if (bits_[i]) {
        raw[i / 8] = static_cast<uint8_t>(raw[i / 8] | (0x80U >> (i % 8)));
      }
    }
    std::vector<uint8_t> out;
    unsigned zeros = 0;
    for (const uint8_t b : raw) {
      if (zeros >= 2 && b <= 3) {
        out.push_back(3);
        zeros = 0;
      }
      out.push_back(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
  }

 private:
  std::vector<bool> bits_;
};

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
  uint64_t end_bit = 0;  // reader position after the parse
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
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

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

  /// Parses a VCL / non-VCL unit with the dispatch the index module will use.
  [[nodiscard]] Unit parse(const std::vector<uint8_t>& nal, bool recording = true) const {
    Unit u;
    u.rbsp = ovt::rbsp_from_nal(nal).rbsp;
    BitReader br(u.rbsp);
    SyntaxBuilder b(br);
    b.set_recording(recording);
    try {
      u.header = h264::parse_nal_header(br, b);
      switch (u.header.nal_unit_type) {
        case 1:
        case 5:
        case 20:
        case 21:
          u.slice = h264::parse_slice_layer(br, b, u.header, sets_);
          break;
        case 2:
          u.slice = h264::parse_slice_data_partition_a(br, b, u.header, sets_).slice_header;
          break;
        default:
          break;
      }
    } catch (const ParseError& e) {
      u.error = e;
    }
    u.end_bit = br.bit_pos();
    u.tree = b.finish();
    u.tree.set_root_name("nal_unit");
    return u;
  }
  [[nodiscard]] Unit parse_hex(std::string_view hex) const { return parse(from_hex(hex)); }

  [[nodiscard]] const h264::ParameterSets& sets() const { return sets_; }
  [[nodiscard]] h264::ParameterSets& sets() { return sets_; }

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

// ---- hand-made parameter sets ---------------------------------------------------------------------------------

struct SpsOpts {
  uint32_t profile_idc = 77;
  uint32_t id = 0;
  uint32_t chroma_format_idc = 1;  // profiles with the high-profile block only
  bool separate_colour_plane = false;
  uint32_t log2_max_frame_num_minus4 = 0;
  uint32_t poc_type = 0;
  uint32_t log2_max_poc_lsb_minus4 = 2;
  bool delta_pic_order_always_zero = false;
  uint32_t max_num_ref_frames = 4;
  uint32_t width_mbs_minus1 = 10;  // 176x144
  uint32_t height_map_units_minus1 = 8;
  bool frame_mbs_only = true;
  bool mbaff = false;
};

std::vector<uint8_t> make_sps(const SpsOpts& o) {
  BitWriter w;
  w.nal(3, 7);
  w.u(8, o.profile_idc);
  w.u(8, 0);
  w.u(8, 30);
  w.ue(o.id);
  if (o.profile_idc == 100 || o.profile_idc == 244 || o.profile_idc == 118) {
    w.ue(o.chroma_format_idc);
    if (o.chroma_format_idc == 3) {
      w.flag(o.separate_colour_plane);
    }
    w.ue(0);
    w.ue(0);
    w.flag(false);
    w.flag(false);
  }
  w.ue(o.log2_max_frame_num_minus4);
  w.ue(o.poc_type);
  if (o.poc_type == 0) {
    w.ue(o.log2_max_poc_lsb_minus4);
  } else if (o.poc_type == 1) {
    w.flag(o.delta_pic_order_always_zero);
    w.se(0);
    w.se(0);
    w.ue(1);
    w.se(2);
  }
  w.ue(o.max_num_ref_frames);
  w.flag(false);
  w.ue(o.width_mbs_minus1);
  w.ue(o.height_map_units_minus1);
  w.flag(o.frame_mbs_only);
  if (!o.frame_mbs_only) {
    w.flag(o.mbaff);
  }
  w.flag(true);   // direct_8x8_inference_flag
  w.flag(false);  // frame_cropping_flag
  w.flag(false);  // vui_parameters_present_flag
  w.trailing();
  return w.nal_bytes();
}

struct PpsOpts {
  uint32_t id = 0;
  uint32_t sps_id = 0;
  bool cabac = false;
  bool bottom_field_pic_order = false;
  uint32_t num_slice_groups_minus1 = 0;
  uint32_t slice_group_map_type = 4;
  uint32_t slice_group_change_rate_minus1 = 3;
  uint32_t l0_default = 0;
  uint32_t l1_default = 0;
  bool weighted_pred = false;
  uint32_t weighted_bipred_idc = 0;
  int32_t pic_init_qp_minus26 = 0;
  int32_t pic_init_qs_minus26 = 0;
  bool deblocking_control = false;
  bool redundant_pic_cnt_present = false;
};

std::vector<uint8_t> make_pps(const PpsOpts& o) {
  BitWriter w;
  w.nal(3, 8);
  w.ue(o.id);
  w.ue(o.sps_id);
  w.flag(o.cabac);
  w.flag(o.bottom_field_pic_order);
  w.ue(o.num_slice_groups_minus1);
  if (o.num_slice_groups_minus1 > 0) {
    w.ue(o.slice_group_map_type);  // 3..5 only in these tests
    w.flag(false);
    w.ue(o.slice_group_change_rate_minus1);
  }
  w.ue(o.l0_default);
  w.ue(o.l1_default);
  w.flag(o.weighted_pred);
  w.u(2, o.weighted_bipred_idc);
  w.se(o.pic_init_qp_minus26);
  w.se(o.pic_init_qs_minus26);
  w.se(0);
  w.flag(o.deblocking_control);
  w.flag(false);
  w.flag(o.redundant_pic_cnt_present);
  w.trailing();
  return w.nal_bytes();
}

/// A slice NAL unit up to pic_parameter_set_id; the caller writes the rest of the header.
BitWriter slice_start(unsigned ref_idc, unsigned type, uint32_t first_mb, uint32_t slice_type, uint32_t pps_id = 0) {
  BitWriter w;
  w.nal(ref_idc, type);
  w.ue(first_mb);
  w.ue(slice_type);
  w.ue(pps_id);
  return w;
}

/// Ends a hand-made slice: one stand-in slice_data byte.
std::vector<uint8_t> slice_end(BitWriter& w) {
  w.trailing();
  w.u(8, 0xA5);
  return w.nal_bytes();
}

}  // namespace

// ---- x264 streams against trace_headers -------------------------------------------------------------------------

TEST_CASE("h264_slice_header_weighted_p_x264", "[syntax][h264][fast]") {
  Stream s;
  s.add_hex(kWpSps);
  s.add_hex(kWpPps);

  SECTION("IDR slice: tree equals trace_headers, derived values") {
    const Unit u = s.parse_hex(kWpIdr);
    REQUIRE_FALSE(u.error.has_value());
    REQUIRE(u.slice.has_value());
    CHECK(u.trace() == kWpIdrTrace);
    const auto& sh = *u.slice;
    CHECK(sh.IdrPicFlag);
    CHECK(sh.slice_type == 7);
    CHECK(sh.slice_type_mod5 == h264::kSliceI);
    CHECK(sh.slice_type_name == "I");
    CHECK(u.node("slice_type")->enum_name == "I");
    CHECK(sh.idr_pic_id == 0);
    REQUIRE(sh.has_dec_ref_pic_marking);
    CHECK(sh.dec_ref_pic_marking.idr);
    CHECK_FALSE(sh.dec_ref_pic_marking.no_output_of_prior_pics_flag);
    CHECK(sh.dec_ref_pic_marking.MaxLongTermFrameIdx == h264::kNoLongTermFrameIndices);
    CHECK(sh.SliceQPY == 26 - 3 - 10);  // pic_init_qp_minus26 = -3, slice_qp_delta = -10
    CHECK(sh.FilterOffsetA == 0);
    CHECK(sh.structure == h264::PictureStructure::Frame);
    CHECK(sh.PicSizeInMbs == 99);
    CHECK(sh.MaxPicNum == 16);
    CHECK(sh.header_bits == 36);
    const auto* data = u.node("slice_data");
    REQUIRE(data != nullptr);
    CHECK(data->span.bit_offset == 36);
    CHECK(u.end_bit == u.rbsp.size() * 8);
    CHECK(diag_codes(u.tree.root()).empty());
  }
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
    CHECK(m.ops_l0[1].modification_of_pic_nums_idc == 0);
    CHECK(m.ops_l0[1].abs_diff_pic_num_minus1 == 15);
    CHECK(m.ops_l0[3].abs_diff_pic_num_minus1 == 0);
    CHECK(m.ops_l1.empty());
    REQUIRE(sh.has_pred_weight_table);
    const auto& t = sh.pred_weight_table;
    CHECK(t.luma_log2_weight_denom == 5);
    CHECK(t.chroma_log2_weight_denom == 0);
    CHECK(t.l0.luma_weight[0] == 63);
    CHECK(t.l0.luma_offset[0] == -15);
    CHECK(t.l0.chroma_weight[0][1] == 2);
    CHECK(t.l0.chroma_offset[0][0] == -128);
    CHECK(t.l0.chroma_offset[0][1] == -128);
    CHECK(t.l0.luma_offset[1] == -16);
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
    (void)s.parse_hex(kWpIdr);
    (void)s.parse_hex(kWpP);
    REQUIRE(s.activations.size() == 2);
    CHECK(s.activations[1].pps_id == 0);
    CHECK(s.activations[1].sps_id == 0);
  }
}

TEST_CASE("h264_slice_header_pyramid_mmco_x264", "[syntax][h264][fast]") {
  Stream s;
  s.add_hex(kPyrSps);
  s.add_hex(kPyrPps);

  SECTION("P slice: modification idc 1, weighted prediction against duplicate references") {
    const Unit u = s.parse_hex(kPyrP);
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.trace() == kPyrPTrace);
    const auto& m = u.slice->ref_pic_list_modification;
    REQUIRE(m.ops_l0.size() == 3);
    CHECK(m.ops_l0[2].modification_of_pic_nums_idc == 1);
    CHECK(m.ops_l0[2].abs_diff_pic_num_minus1 == 0);
    CHECK(u.node("modification_of_pic_nums_idc")->enum_name == "subtract from picture number prediction");
    CHECK(u.slice->pred_weight_table.l0.luma_weight[1] == 1);
    CHECK(u.slice->pred_weight_table.l0.luma_weight[0] == 1);  // inferred 2^0
    CHECK(u.slice->pic_order_cnt_lsb == 16);
  }
  SECTION("reference B slice: MMCO op list") {
    const Unit u = s.parse_hex(kPyrBref);
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.trace() == kPyrBrefTrace);
    const auto& sh = *u.slice;
    CHECK(sh.slice_type_mod5 == h264::kSliceB);
    CHECK(sh.direct_spatial_mv_pred_flag);
    CHECK_FALSE(sh.num_ref_idx_active_override_flag);
    CHECK(sh.num_ref_idx_l0_active_minus1 == 1);  // PPS defaults
    CHECK(sh.num_ref_idx_l1_active_minus1 == 0);
    CHECK_FALSE(sh.has_pred_weight_table);  // weighted_bipred_idc 2 (implicit)
    const auto& d = sh.dec_ref_pic_marking;
    REQUIRE(d.adaptive_ref_pic_marking_mode_flag);
    REQUIRE(d.mmco.size() == 2);
    CHECK(d.mmco[0].memory_management_control_operation == 1);
    CHECK(d.mmco[0].difference_of_pic_nums_minus1 == 3);
    CHECK(d.mmco[1].difference_of_pic_nums_minus1 == 1);
    CHECK_FALSE(d.has_mmco5);
    CHECK(u.node("memory_management_control_operation")->enum_name == "short-term unused");
  }
  SECTION("non-reference B slice: no dec_ref_pic_marking") {
    const Unit u = s.parse_hex(kPyrB);
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.trace() == kPyrBTrace);
    CHECK_FALSE(u.slice->has_dec_ref_pic_marking);
    CHECK(u.node("dec_ref_pic_marking") == nullptr);
    CHECK(u.slice->SliceQPY == 26 - 3 + 11);
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

// ---- hand-made headers ------------------------------------------------------------------------------------------

TEST_CASE("h264_slice_header_fields_and_poc", "[syntax][h264][fast]") {
  SECTION("field picture, POC type 0: bottom_field_flag, no delta_pic_order_cnt_bottom in a field") {
    Stream s;
    s.add(make_sps({.frame_mbs_only = false, .mbaff = true}));
    s.add(make_pps({.bottom_field_pic_order = true}));
    BitWriter w = slice_start(2, 1, 3, 0);
    w.u(4, 9);      // frame_num
    w.flag(true);   // field_pic_flag
    w.flag(true);   // bottom_field_flag
    w.u(6, 33);     // pic_order_cnt_lsb (log2_max_pic_order_cnt_lsb_minus4 = 2)
    w.flag(false);  // num_ref_idx_active_override_flag
    w.flag(false);  // ref_pic_list_modification_flag_l0
    w.flag(false);  // adaptive_ref_pic_marking_mode_flag
    w.se(-2);       // slice_qp_delta
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    const auto& sh = *u.slice;
    CHECK(sh.field_pic_flag);
    CHECK(sh.bottom_field_flag);
    CHECK(sh.structure == h264::PictureStructure::BottomField);
    CHECK_FALSE(sh.MbaffFrameFlag);
    CHECK(sh.pic_order_cnt_lsb == 33);
    CHECK(u.node("delta_pic_order_cnt_bottom") == nullptr);
    CHECK(sh.PicHeightInMbs == 9);  // FrameHeightInMbs 18 / 2
    CHECK(sh.PicSizeInMbs == 99);
    CHECK(sh.MaxPicNum == 32);
    CHECK(sh.CurrPicNum == 19);
    CHECK(sh.SliceQPY == 24);
    CHECK(u.node("pic_order_cnt_lsb")->descriptor == "u(v)");
    CHECK(u.node("frame_num")->descriptor == "u(v)");
    CHECK(diag_codes(u.tree.root()).empty());
  }
  SECTION("frame picture, POC type 0 with delta_pic_order_cnt_bottom; top field") {
    Stream s;
    s.add(make_sps({.frame_mbs_only = false}));
    s.add(make_pps({.bottom_field_pic_order = true}));
    BitWriter w = slice_start(0, 1, 0, 2);
    w.u(4, 1);
    w.flag(false);  // field_pic_flag
    w.u(6, 4);
    w.se(-1);  // delta_pic_order_cnt_bottom
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->delta_pic_order_cnt_bottom == -1);
    CHECK(u.slice->structure == h264::PictureStructure::Frame);
    CHECK(u.slice->PicHeightInMbs == 18);
  }
  SECTION("POC type 1: delta_pic_order_cnt[0..1]; redundant_pic_cnt") {
    Stream s;
    s.add(make_sps({.poc_type = 1}));
    s.add(make_pps({.bottom_field_pic_order = true, .redundant_pic_cnt_present = true}));
    BitWriter w = slice_start(1, 1, 0, 7);
    w.u(4, 2);
    w.se(5);   // delta_pic_order_cnt[0]
    w.se(-3);  // delta_pic_order_cnt[1]
    w.ue(1);   // redundant_pic_cnt
    w.flag(false);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->delta_pic_order_cnt[0] == 5);
    CHECK(u.slice->delta_pic_order_cnt[1] == -3);
    CHECK(u.slice->redundant_pic_cnt == 1);
    CHECK(u.trace().find("delta_pic_order_cnt[1] ") != std::string::npos);
  }
  SECTION("POC type 1 with delta_pic_order_always_zero_flag: nothing read") {
    Stream s;
    s.add(make_sps({.poc_type = 1, .delta_pic_order_always_zero = true}));
    s.add(make_pps({}));
    BitWriter w = slice_start(1, 1, 0, 7);
    w.u(4, 2);
    w.flag(false);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.node("delta_pic_order_cnt") == nullptr);
  }
  SECTION("separate colour planes: colour_plane_id") {
    Stream s;
    s.add(make_sps({.profile_idc = 244, .chroma_format_idc = 3, .separate_colour_plane = true, .poc_type = 2}));
    s.add(make_pps({}));
    BitWriter w = slice_start(1, 5, 0, 7);
    w.u(2, 2);  // colour_plane_id
    w.u(4, 0);
    w.ue(7);  // idr_pic_id
    w.flag(false);
    w.flag(true);  // long_term_reference_flag
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->colour_plane_id == 2);
    CHECK(u.slice->idr_pic_id == 7);
    CHECK(u.slice->dec_ref_pic_marking.long_term_reference_flag);
    CHECK(u.slice->dec_ref_pic_marking.MaxLongTermFrameIdx == 0);
  }
}

TEST_CASE("h264_slice_header_sp_si_deblocking_slice_groups", "[syntax][h264][fast]") {
  SECTION("SP slice: sp_for_switch_flag, slice_qs_delta, QSY; deblocking offsets") {
    Stream s;
    s.add(make_sps({.poc_type = 2}));
    s.add(make_pps({.pic_init_qs_minus26 = -2, .deblocking_control = true}));
    BitWriter w = slice_start(1, 1, 0, 3);
    w.u(4, 1);
    w.flag(false);  // num_ref_idx_active_override_flag
    w.flag(false);  // ref_pic_list_modification_flag_l0
    w.flag(false);  // adaptive_ref_pic_marking_mode_flag
    w.se(3);        // slice_qp_delta
    w.flag(true);   // sp_for_switch_flag
    w.se(4);        // slice_qs_delta
    w.ue(0);        // disable_deblocking_filter_idc
    w.se(-6);
    w.se(6);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    const auto& sh = *u.slice;
    CHECK(sh.slice_type_name == "SP");
    CHECK(sh.sp_for_switch_flag);
    CHECK(sh.QSY == 28);
    CHECK(sh.FilterOffsetA == -12);
    CHECK(sh.FilterOffsetB == 12);
    CHECK(sh.SliceQPY == 29);
  }
  SECTION("SI slice: no reference fields, slice_qs_delta; disable_deblocking_filter_idc 1 has no offsets") {
    Stream s;
    s.add(make_sps({.poc_type = 2}));
    s.add(make_pps({.cabac = true, .deblocking_control = true}));
    BitWriter w = slice_start(1, 1, 0, 9);
    w.u(4, 1);
    w.flag(false);  // adaptive_ref_pic_marking_mode_flag
    w.se(0);        // slice_qp_delta (no cabac_init_idc in SI)
    w.se(-1);       // slice_qs_delta
    w.ue(1);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->slice_type_name == "SI");
    CHECK(u.node("num_ref_idx_active_override_flag") == nullptr);
    CHECK(u.node("cabac_init_idc") == nullptr);
    CHECK(u.node("slice_alpha_c0_offset_div2") == nullptr);
    CHECK(u.slice->QSY == 25);
  }
  SECTION("slice groups map type 4: slice_group_change_cycle width from PicSizeInMapUnits ÷ SliceGroupChangeRate") {
    // PicSizeInMapUnits 99, SliceGroupChangeRate 4: Ceil(Log2(99 ÷ 4 + 1)) = Ceil(Log2(25.75)) = 5 bits.
    Stream s;
    s.add(make_sps({.poc_type = 2}));
    s.add(make_pps({.num_slice_groups_minus1 = 1, .slice_group_map_type = 4, .slice_group_change_rate_minus1 = 3}));
    BitWriter w = slice_start(1, 1, 0, 7);
    w.u(4, 1);
    w.flag(false);
    w.se(0);
    w.u(5, 25);  // slice_group_change_cycle = Ceil(99 ÷ 4), the maximum
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->slice_group_change_cycle_bits == 5);
    CHECK(u.slice->slice_group_change_cycle == 25);
    CHECK(u.node("slice_group_change_cycle")->descriptor == "u(v)");
    CHECK(diag_codes(u.tree.root()).empty());
  }
  SECTION("slice groups with a rate dividing PicSizeInMapUnits exactly") {
    // PicSizeInMapUnits 99, SliceGroupChangeRate 33: Ceil(Log2(3 + 1)) = 2 bits.
    Stream s;
    s.add(make_sps({.poc_type = 2}));
    s.add(make_pps({.num_slice_groups_minus1 = 1, .slice_group_map_type = 3, .slice_group_change_rate_minus1 = 32}));
    BitWriter w = slice_start(1, 1, 0, 7);
    w.u(4, 1);
    w.flag(false);
    w.se(0);
    w.u(2, 3);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->slice_group_change_cycle_bits == 2);
    CHECK(u.slice->slice_group_change_cycle == 3);
  }
}

TEST_CASE("h264_slice_header_lists_and_marking", "[syntax][h264][fast]") {
  Stream s;
  s.add(make_sps({.poc_type = 2, .max_num_ref_frames = 4}));
  s.add(make_pps({.l0_default = 1, .l1_default = 1, .weighted_bipred_idc = 1}));

  SECTION("B slice with explicit weights for both lists and long-term modification") {
    BitWriter w = slice_start(1, 1, 0, 6);
    w.u(4, 3);
    w.flag(false);  // direct_spatial_mv_pred_flag
    w.flag(true);   // num_ref_idx_active_override_flag
    w.ue(2);
    w.ue(0);
    w.flag(true);  // ref_pic_list_modification_flag_l0
    w.ue(2);       // long_term_pic_num
    w.ue(5);
    w.ue(3);
    w.flag(true);  // ref_pic_list_modification_flag_l1
    w.ue(1);
    w.ue(0);
    w.ue(3);
    w.ue(4);  // luma_log2_weight_denom
    w.ue(3);  // chroma_log2_weight_denom
    for (int i = 0; i < 3; ++i) {
      w.flag(i == 2);
      if (i == 2) {
        w.se(-128);
        w.se(127);
      }
      w.flag(false);
    }
    w.flag(false);  // luma_weight_l1_flag[0]
    w.flag(true);   // chroma_weight_l1_flag[0]
    w.se(1);
    w.se(2);
    w.se(3);
    w.se(4);
    w.flag(true);  // adaptive_ref_pic_marking_mode_flag
    w.ue(2);       // long_term_pic_num
    w.ue(0);
    w.ue(3);  // short-term to long-term
    w.ue(1);
    w.ue(2);
    w.ue(4);
    w.ue(3);
    w.ue(6);
    w.ue(0);
    w.ue(5);
    w.ue(0);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    const auto& sh = *u.slice;
    REQUIRE(sh.ref_pic_list_modification.ops_l0.size() == 1);
    CHECK(sh.ref_pic_list_modification.ops_l0[0].modification_of_pic_nums_idc == 2);
    CHECK(sh.ref_pic_list_modification.ops_l0[0].long_term_pic_num == 5);
    REQUIRE(sh.ref_pic_list_modification.ops_l1.size() == 1);
    CHECK(sh.ref_pic_list_modification.ops_l1[0].modification_of_pic_nums_idc == 1);
    REQUIRE(sh.has_pred_weight_table);
    const auto& t = sh.pred_weight_table;
    CHECK(t.l0.luma_weight[2] == -128);
    CHECK(t.l0.luma_offset[2] == 127);
    CHECK(t.l0.luma_weight[0] == 16);
    CHECK(t.l0.chroma_weight[1][1] == 8);
    CHECK(t.l1.luma_weight[0] == 16);
    CHECK(t.l1.chroma_weight[0][0] == 1);
    CHECK(t.l1.chroma_offset[0][0] == 2);
    CHECK(t.l1.chroma_weight[0][1] == 3);
    CHECK(t.l1.chroma_offset[0][1] == 4);
    CHECK(u.trace().find("chroma_offset_l1[0][1] ") != std::string::npos);
    const auto& d = sh.dec_ref_pic_marking;
    REQUIRE(d.mmco.size() == 5);
    CHECK(d.mmco[0].long_term_pic_num == 0);
    CHECK(d.mmco[1].memory_management_control_operation == 3);
    CHECK(d.mmco[1].difference_of_pic_nums_minus1 == 1);
    CHECK(d.mmco[1].long_term_frame_idx == 2);
    CHECK(d.mmco[2].max_long_term_frame_idx_plus1 == 3);
    CHECK(d.mmco[3].long_term_frame_idx == 0);
    CHECK(d.mmco[4].memory_management_control_operation == 5);
    CHECK(d.has_mmco5);
    CHECK(d.MaxLongTermFrameIdx == h264::kNoLongTermFrameIndices);  // op 5 after op 4
    CHECK(diag_codes(u.tree.root()).empty());
  }
  SECTION("MMCO 4 with max_long_term_frame_idx_plus1 0: no long-term frame indices") {
    BitWriter w = slice_start(1, 1, 0, 5);
    w.u(4, 3);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    w.ue(4);
    w.ue(0);
    w.ue(0);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->dec_ref_pic_marking.MaxLongTermFrameIdx == h264::kNoLongTermFrameIndices);
  }
}

TEST_CASE("h264_slice_header_mvc_extension", "[syntax][h264][fast]") {
  // nal_unit_type 20 with the MVC extension header: slice_header() activates the subset SPS (H.7.4.1.2.1) and reads
  // ref_pic_list_mvc_modification() with the inter-view idc 4 / 5.
  Stream s;
  s.add(make_sps({.profile_idc = 118, .id = 1, .poc_type = 2}), true);
  s.add(make_pps({.id = 3, .sps_id = 1, .l0_default = 1}));
  auto mvc_slice = [](bool non_idr) {
    BitWriter w;
    w.nal(3, 20);
    w.flag(false);  // svc_extension_flag
    w.flag(non_idr);
    w.u(6, 0);   // priority_id
    w.u(10, 2);  // view_id
    w.u(3, 0);   // temporal_id
    w.flag(true);
    w.flag(false);
    w.u(1, 1);
    w.ue(0);
    w.ue(non_idr ? 5 : 7);
    w.ue(3);
    w.u(4, 0);
    if (!non_idr) {
      w.ue(0);  // idr_pic_id
    } else {
      w.flag(false);
      w.flag(true);  // ref_pic_list_modification_flag_l0
      w.ue(4);
      w.ue(0);
      w.ue(5);
      w.ue(1);
      w.ue(3);
    }
    if (non_idr) {
      w.flag(false);  // adaptive_ref_pic_marking_mode_flag
    } else {
      w.flag(false);
      w.flag(false);
    }
    w.se(0);
    return slice_end(w);
  };
  SECTION("anchor IDR view component: IdrPicFlag from non_idr_flag") {
    const Unit u = s.parse(mvc_slice(false));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->IdrPicFlag);
    CHECK(u.slice->uses_subset_sps);
    CHECK(u.slice->seq_parameter_set_id == 1);
    CHECK(u.node("slice_layer_extension_rbsp") != nullptr);
  }
  SECTION("non-IDR: inter-view modification ops") {
    const Unit u = s.parse(mvc_slice(true));
    REQUIRE_FALSE(u.error.has_value());
    const auto& m = u.slice->ref_pic_list_modification;
    CHECK(m.mvc);
    REQUIRE(m.ops_l0.size() == 2);
    CHECK(m.ops_l0[0].modification_of_pic_nums_idc == 4);
    CHECK(m.ops_l0[0].abs_diff_view_idx_minus1 == 0);
    CHECK(m.ops_l0[1].abs_diff_view_idx_minus1 == 1);
    CHECK(u.node("ref_pic_list_mvc_modification") != nullptr);
  }
  SECTION("the subset SPS table is not consulted for NAL 1, the SPS table not for NAL 20") {
    BitWriter w = slice_start(3, 5, 0, 7, 3);
    const Unit u = s.parse(slice_end(w));
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
    CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("SPS 1 is missing"));
  }
}

TEST_CASE("h264_slice_header_errors", "[syntax][h264][fast]") {
  Stream s;
  s.add(make_sps({.poc_type = 2}));
  s.add(make_pps({.l0_default = 0}));

  SECTION("missing PPS: SYN-H264-0010, partial tree kept") {
    BitWriter w = slice_start(1, 1, 0, 5, 7);
    const Unit u = s.parse(slice_end(w));
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
    CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("missing PPS 7"));
    CHECK(u.node("pic_parameter_set_id") != nullptr);
    CHECK(s.activations.empty());
  }
  SECTION("pic_parameter_set_id 256 and slice_type 10 are range errors") {
    for (const auto& [type, pps] : {std::pair{10U, 0U}, std::pair{0U, 256U}}) {
      BitWriter w = slice_start(1, 1, 0, type, pps);
      const Unit u = s.parse(slice_end(w));
      REQUIRE(u.error.has_value());
      CHECK(u.error->code == "SYN-H264-0002");
    }
  }
  SECTION("num_ref_idx_l0_active_minus1 32 throws; 16 in a frame is a Diagnostic") {
    for (const uint32_t n : {32U, 16U}) {
      BitWriter w = slice_start(1, 1, 0, 5);
      w.u(4, 1);
      w.flag(true);
      w.ue(n);
      w.flag(false);
      w.flag(false);
      w.se(0);
      const Unit u = s.parse(slice_end(w));
      if (n == 32) {
        REQUIRE(u.error.has_value());
        CHECK(u.error->code == "SYN-H264-0002");
        CHECK(u.node("ref_pic_list_modification") == nullptr);
      } else {
        REQUIRE_FALSE(u.error.has_value());
        CHECK(diag_codes(u.tree.root()) == std::vector<std::string>{"SYN-H264-0002"});
      }
    }
  }
  SECTION("modification_of_pic_nums_idc 4 outside MVC, memory_management_control_operation 7") {
    {
      BitWriter w = slice_start(1, 1, 0, 5);
      w.u(4, 1);
      w.flag(false);
      w.flag(true);
      w.ue(4);
      const Unit u = s.parse(slice_end(w));
      REQUIRE(u.error.has_value());
      CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("modification_of_pic_nums_idc out of range"));
    }
    {
      BitWriter w = slice_start(1, 1, 0, 5);
      w.u(4, 1);
      w.flag(false);
      w.flag(false);
      w.flag(true);
      w.ue(7);
      const Unit u = s.parse(slice_end(w));
      REQUIRE(u.error.has_value());
      CHECK_THAT(u.error->what(),
                 Catch::Matchers::ContainsSubstring("memory_management_control_operation out of range"));
    }
  }
  SECTION("more modification ops than references: reported once, not stored") {
    BitWriter w = slice_start(1, 1, 0, 5);
    w.u(4, 1);
    w.flag(false);
    w.flag(true);
    for (int i = 0; i < 40; ++i) {
      w.ue(0);
      w.ue(0);
    }
    w.ue(3);
    w.flag(false);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->ref_pic_list_modification.ops_l0.size() == 1);
    CHECK(diag_codes(u.tree.root()) == std::vector<std::string>{"SYN-H264-0002"});
    CHECK(u.slice->SliceQPY == 26);  // the elements after the loop are still read
  }
  SECTION("more than 66 MMCO ops: reported once, 66 stored") {
    BitWriter w = slice_start(1, 1, 0, 5);
    w.u(4, 1);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    for (int i = 0; i < 70; ++i) {
      w.ue(1);
      w.ue(0);
    }
    w.ue(0);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->dec_ref_pic_marking.mmco.size() == h264::kMaxMmcoOps);
    CHECK(diag_codes(u.tree.root()) == std::vector<std::string>{"SYN-H264-0002"});
  }
  SECTION("SliceQPY outside [-QpBdOffsetY, 51] throws") {
    BitWriter w = slice_start(1, 1, 0, 7);
    w.u(4, 1);
    w.flag(false);
    w.se(26);
    const Unit u = s.parse(slice_end(w));
    REQUIRE(u.error.has_value());
    CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("SliceQPY out of range"));
  }
  SECTION("weights outside -128..127 and first_mb_in_slice outside the picture are Diagnostics") {
    Stream wp;
    wp.add(make_sps({.poc_type = 2}));
    wp.add(make_pps({.weighted_pred = true}));
    BitWriter w = slice_start(1, 1, 99, 5);
    w.u(4, 1);
    w.flag(false);
    w.flag(false);
    w.ue(0);
    w.ue(0);
    w.flag(true);
    w.se(200);
    w.se(0);
    w.flag(false);
    w.flag(false);
    w.se(0);
    const Unit u = wp.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->pred_weight_table.l0.luma_weight[0] == 200);
    CHECK(diag_codes(u.tree.root()).size() == 2);
  }
  SECTION("an IDR picture with a P slice and a non-zero frame_num are Diagnostics") {
    BitWriter w = slice_start(1, 5, 0, 5);
    w.u(4, 3);
    w.ue(0);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.se(0);
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(diag_codes(u.tree.root()).size() == 2);
  }
  SECTION("SVC / 3D-AVC slice extension: kept as bytes with SYN-H264-0070 (WI-4.111, #529)") {
    // nal_unit_header_svc_extension() ends with reserved_three_2bits = 3; nal_unit_header_3davc_extension() is 15
    // bits of zero-valid fields.
    for (const auto& [type, ext_bits, ext_value] : {std::tuple{20U, 23U, 3U}, std::tuple{21U, 15U, 0U}}) {
      BitWriter w;
      w.nal(3, type);
      w.flag(true);  // svc_extension_flag / avc_3d_extension_flag
      w.u(ext_bits, ext_value);
      w.ue(0);
      const Unit u = s.parse(slice_end(w));
      CAPTURE(type);
      REQUIRE_FALSE(u.error.has_value());
      CHECK_FALSE(u.slice.has_value());
      CHECK(u.node("slice_layer_extension_bytes") != nullptr);
      CHECK(u.node("slice_header") == nullptr);
      CHECK(diag_codes(u.tree.root()) == std::vector<std::string>{"SYN-H264-0070"});
      CHECK(s.activations.empty());
      // parse_slice_header() itself throws SYN-H264-0002 for these units (§7.9 table).
      const auto rbsp = ovt::rbsp_from_nal(slice_end(w)).rbsp;
      BitReader br(rbsp);
      SyntaxBuilder b(br);
      const auto h = h264::parse_nal_header(br, b);
      try {
        (void)h264::parse_slice_header(br, b, h, s.sets());
        FAIL("parse_slice_header() accepted an enhancement-layer slice");
      } catch (const ParseError& e) {
        CHECK(e.code == "SYN-H264-0002");
        CHECK_THAT(e.what(), Catch::Matchers::ContainsSubstring("not defined for nal_unit_type"));
      }
    }
  }
  SECTION("PPS present, its SPS missing: SYN-H264-0010") {
    Stream wp;
    wp.add_hex(kWpPps);  // parsed provisionally
    const Unit u = wp.parse_hex(kWpP);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
    CHECK_THAT(u.error->what(), Catch::Matchers::ContainsSubstring("whose SPS 0 is missing"));
  }
  SECTION("the subset SPS table serves NAL 20 / 21 only") {
    Stream wp;
    wp.add(from_hex(kWpSps), true);
    wp.add_hex(kWpPps);
    const Unit u = wp.parse_hex(kWpP);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "SYN-H264-0010");
  }
  SECTION("a header cut short: BR-0001 with the partial tree") {
    auto bytes = from_hex(kWpP);
    bytes.resize(5);
    Stream wp;
    wp.add_hex(kWpSps);
    wp.add_hex(kWpPps);
    const Unit u = wp.parse(bytes);
    REQUIRE(u.error.has_value());
    CHECK(u.error->code == "BR-0001");
    CHECK(u.node("ref_pic_list_modification") != nullptr);
  }
}

TEST_CASE("h264_slice_header_deterministic_and_non_recording", "[syntax][h264][fast]") {
  Stream s;
  s.add_hex(kWpSps);
  s.add_hex(kWpPps);
  const auto bytes = from_hex(kWpP);
  const Unit a = s.parse(bytes);
  const Unit b = s.parse(bytes);
  CHECK(ovt::syntax::to_json(a.tree) == ovt::syntax::to_json(b.tree));
  const Unit off = s.parse(bytes, false);
  REQUIRE(off.slice.has_value());
  CHECK((off.tree.root() == nullptr || off.tree.root()->child_count() == 0));
  CHECK(off.end_bit == a.end_bit);
  CHECK(off.slice->header_bits == a.slice->header_bits);
  CHECK(off.slice->pred_weight_table.l0.luma_offset == a.slice->pred_weight_table.l0.luma_offset);
  CHECK(off.slice->ref_pic_list_modification.ops_l0.size() == a.slice->ref_pic_list_modification.ops_l0.size());
}

// ---- non-VCL units and data partitions --------------------------------------------------------------------------

namespace {

struct Plain {
  std::vector<uint8_t> rbsp;
  SyntaxNodeBuffer tree;
};

template <class Fn>
Plain parse_plain(const std::vector<uint8_t>& nal, Fn fn) {
  Plain p;
  p.rbsp = ovt::rbsp_from_nal(nal).rbsp;
  BitReader br(p.rbsp);
  SyntaxBuilder b(br);
  const auto h = h264::parse_nal_header(br, b);
  fn(br, b, h);
  p.tree = b.finish();
  return p;
}

}  // namespace

TEST_CASE("h264_aud_eos_filler", "[syntax][h264][fast]") {
  SECTION("access unit delimiter (x264 --aud: 09 f0, primary_pic_type 7)") {
    h264::AccessUnitDelimiter aud;
    const auto p = parse_plain(from_hex("09f0"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      aud = h264::parse_access_unit_delimiter(br, b);
    });
    CHECK(aud.primary_pic_type == 7);
    CHECK(aud.primary_pic_type_name == "I, SI, P, SP, B");
    CHECK(find_node(p.tree.root(), "primary_pic_type")->enum_name == "I, SI, P, SP, B");
    CHECK(diag_codes(p.tree.root()).empty());
    CHECK(ovt::syntax::to_trace(p.tree, p.rbsp) ==
          "0 forbidden_zero_bit 0 = 0\n1 nal_ref_idc 00 = 0\n3 nal_unit_type 01001 = 9\n8 primary_pic_type 111 = 7\n"
          "11 rbsp_stop_one_bit 1 = 1\n12 rbsp_alignment_zero_bit 0 = 0\n13 rbsp_alignment_zero_bit 0 = 0\n"
          "14 rbsp_alignment_zero_bit 0 = 0\n15 rbsp_alignment_zero_bit 0 = 0\n");
  }
  SECTION("end of sequence and end of stream") {
    BitSpan eos{};
    BitSpan eob{};
    const auto p = parse_plain(from_hex("0a"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      eos = h264::parse_end_of_seq(br, b);
    });
    (void)parse_plain(from_hex("0b"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      eob = h264::parse_end_of_stream(br, b);
    });
    CHECK(eos.bit_length == 0);
    CHECK(eob.bit_offset == 8);
    CHECK(find_node(p.tree.root(), "end_of_seq_rbsp") != nullptr);
    CHECK(diag_codes(p.tree.root()).empty());
    const auto junk = parse_plain(from_hex("0a1234"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      (void)h264::parse_end_of_seq(br, b);
    });
    CHECK(diag_codes(junk.tree.root()) == std::vector<std::string>{"SYN-H264-0003"});
    CHECK(find_node(junk.tree.root(), "trailing_data") != nullptr);
  }
  SECTION("filler data: the ff_byte run as one node") {
    h264::FillerData f;
    const auto p = parse_plain(
        from_hex("0cffffffffff80"),
        [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) { f = h264::parse_filler_data(br, b); });
    CHECK(f.ff_byte_count == 5);
    const auto* ff = find_node(p.tree.root(), "ff_byte");
    REQUIRE(ff != nullptr);
    CHECK(ff->descriptor == "f(8)*5");
    CHECK(ff->span.bit_length == 40);
    CHECK(diag_codes(p.tree.root()).empty());
    h264::FillerData empty;
    (void)parse_plain(from_hex("0c80"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      empty = h264::parse_filler_data(br, b);
    });
    CHECK(empty.ff_byte_count == 0);
  }
}

TEST_CASE("h264_slice_data_partitions", "[syntax][h264][fast]") {
  Stream s;
  s.add(make_sps({.poc_type = 2}));
  s.add(make_pps({.redundant_pic_cnt_present = true}));
  SECTION("partition A: slice_header() then slice_id") {
    BitWriter w = slice_start(2, 2, 0, 5);
    w.u(4, 1);
    w.ue(0);  // redundant_pic_cnt
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.se(0);
    w.ue(6);  // slice_id
    const Unit u = s.parse(slice_end(w));
    REQUIRE_FALSE(u.error.has_value());
    CHECK(u.slice->frame_num == 1);
    const auto* id = u.node("slice_id");
    REQUIRE(id != nullptr);
    CHECK(id->uvalue == 6);
    CHECK(u.node("slice_data_partition_a_layer_rbsp") != nullptr);
    CHECK(u.node("slice_data") != nullptr);
  }
  SECTION("partitions B and C: slice_id, redundant_pic_cnt") {
    for (const unsigned type : {3U, 4U}) {
      BitWriter w;
      w.nal(2, type);
      w.ue(6);
      w.ue(1);
      w.trailing();
      w.u(8, 0x55);
      h264::SliceDataPartitionBC part;
      const auto p = parse_plain(w.nal_bytes(), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader& h) {
        part = h264::parse_slice_data_partition_bc(br, b, h, *s.sets().sps.find(0), *s.sets().pps.find(0));
      });
      CHECK(part.partition_c == (type == 4));
      CHECK(part.slice_id == 6);
      CHECK(part.redundant_pic_cnt == 1);
      CHECK(find_node(p.tree.root(), type == 3 ? "slice_data_partition_b_layer_rbsp"
                                               : "slice_data_partition_c_layer_rbsp") != nullptr);
    }
  }
}

// ---- SPS extension, subset SPS, prefix NAL (WI-1.109c) ---------------------------------------------------------

namespace {

// FFmpeg 6.1.1 trace_headers of NAL 13 units inserted after the SPS of wp.264 (see the file comment).
constexpr std::string_view kSpsExt1 = "6da9fe0040";
constexpr std::string_view kSpsExt1Trace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 11 = 3
3 nal_unit_type 01101 = 13
8 seq_parameter_set_id 1 = 0
9 aux_format_idc 010 = 1
12 bit_depth_aux_minus8 1 = 0
13 alpha_incr_flag 0 = 0
14 alpha_opaque_value 011111111 = 255
23 alpha_transparent_value 000000000 = 0
32 additional_extension_flag 0 = 0
33 rbsp_stop_one_bit 1 = 1
34 rbsp_alignment_zero_bit 0 = 0
35 rbsp_alignment_zero_bit 0 = 0
36 rbsp_alignment_zero_bit 0 = 0
37 rbsp_alignment_zero_bit 0 = 0
38 rbsp_alignment_zero_bit 0 = 0
39 rbsp_alignment_zero_bit 0 = 0
)";
constexpr std::string_view kSpsExt2 = "6db2dffe0114";
constexpr std::string_view kSpsExt2Trace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 11 = 3
3 nal_unit_type 01101 = 13
8 seq_parameter_set_id 1 = 0
9 aux_format_idc 011 = 2
12 bit_depth_aux_minus8 00101 = 4
17 alpha_incr_flag 1 = 1
18 alpha_opaque_value 0111111111111 = 4095
31 alpha_transparent_value 0000000010001 = 17
44 additional_extension_flag 0 = 0
45 rbsp_stop_one_bit 1 = 1
46 rbsp_alignment_zero_bit 0 = 0
47 rbsp_alignment_zero_bit 0 = 0
)";
constexpr std::string_view kSpsExt3 = "6dd0";
constexpr std::string_view kSpsExt3Trace = R"(0 forbidden_zero_bit 0 = 0
1 nal_ref_idc 11 = 3
3 nal_unit_type 01101 = 13
8 seq_parameter_set_id 1 = 0
9 aux_format_idc 1 = 0
10 additional_extension_flag 0 = 0
11 rbsp_stop_one_bit 1 = 1
12 rbsp_alignment_zero_bit 0 = 0
13 rbsp_alignment_zero_bit 0 = 0
14 rbsp_alignment_zero_bit 0 = 0
15 rbsp_alignment_zero_bit 0 = 0
)";

// First subset SPS of jvt_MVCNV_1 (byte offset 19, 19 bytes), recorded with GStreamer 1.24.2 on the Mac Mini.
constexpr std::string_view kMvcnv1SubsetSps = "6f76001f4b0d30420a03d974d6956963d6e9b2";
// Script-written MVC subset SPS units; expected values from GStreamer 1.24.2 (see the file comment).
constexpr std::string_view kMvc4Views = "6f80002822ce80780226492b3ad515ba2ad69a2a2843a52a29c24ace42";
constexpr std::string_view kMvc2ViewsExt2 = "6f760028040b3a01e008994614ab599fc526e0";
// seq_parameter_set_data() then an SVC (83) / MVCD (138) extension, an MVC VUI extension, and High (100).
constexpr std::string_view kSvcSubsetSps = "6f5300286b3a01e008996a40";
constexpr std::string_view kMvcdSubsetSps = "6f8a00282ace807802265780";
constexpr std::string_view kMvcVuiSubsetSps = "6f76002832ce8078022679447c61";
constexpr std::string_view kHighSubsetSps = "6f6400283ace8078022620";

using Refs = std::vector<std::vector<uint32_t>>;

void check_mvcnv1(const h264::SubsetSps& s) {
  CHECK(s.sps.seq_parameter_set_id == 1);
  CHECK(s.sps.profile_idc == 118);
  REQUIRE(s.extension == h264::SubsetSpsExtension::Mvc);
  CHECK(s.mvc.num_views_minus1 == 2);
  CHECK(s.mvc.view_id == std::vector<uint32_t>{0, 1, 2});
  CHECK(s.mvc.anchor_ref_l0 == Refs{{}, {0}, {1}});
  CHECK(s.mvc.anchor_ref_l1 == Refs{{}, {}, {}});
  CHECK(s.mvc.non_anchor_ref_l0 == Refs{{}, {0}, {1}});
  CHECK(s.mvc.non_anchor_ref_l1 == Refs{{}, {}, {}});
  REQUIRE(s.mvc.level_values.size() == 1);
  CHECK(s.mvc.level_values[0].level_idc == 30);
  REQUIRE(s.mvc.level_values[0].ops.size() == 1);
  CHECK(s.mvc.level_values[0].ops[0].temporal_id == 3);
  CHECK(s.mvc.level_values[0].ops[0].target_view_id == std::vector<uint32_t>{0, 1, 2});
  CHECK(s.mvc.level_values[0].ops[0].num_views_minus1 == 2);
}

}  // namespace

TEST_CASE("h264_sps_extension", "[syntax][h264][fast]") {
  SECTION("the tree equals FFmpeg's trace_headers") {
    for (const auto& [hex, trace] :
         {std::pair{kSpsExt1, kSpsExt1Trace}, std::pair{kSpsExt2, kSpsExt2Trace}, std::pair{kSpsExt3, kSpsExt3Trace}}) {
      h264::SpsExtension e;
      const auto p = parse_plain(from_hex(hex), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
        e = h264::parse_sps_extension(br, b);
      });
      CAPTURE(hex);
      CHECK(ovt::syntax::to_trace(p.tree, p.rbsp) == trace);
      CHECK(diag_codes(p.tree.root()).empty());
    }
  }
  SECTION("fields and BitDepthAux") {
    h264::SpsExtension e;
    (void)parse_plain(from_hex(kSpsExt2), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      e = h264::parse_sps_extension(br, b);
    });
    CHECK(e.aux_format_idc == 2);
    CHECK(e.BitDepthAux == 12);
    CHECK(e.alpha_incr_flag);
    CHECK(e.alpha_opaque_value == 4095);
    CHECK(e.alpha_transparent_value == 17);
  }
  SECTION("aux_format_idc 4 throws; additional_extension_flag 1 is a Warning, the rest kept") {
    BitWriter w;
    w.nal(3, 13);
    w.ue(0);
    w.ue(4);
    try {
      (void)parse_plain(slice_end(w), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
        (void)h264::parse_sps_extension(br, b);
      });
      FAIL("aux_format_idc 4 accepted");
    } catch (const ParseError& e) {
      CHECK(e.code == "SYN-H264-0002");
    }
    BitWriter x;
    x.nal(3, 13);
    x.ue(0);
    x.ue(0);
    x.flag(true);  // additional_extension_flag
    x.u(8, 0xA5);
    const auto p = parse_plain(slice_end(x), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      (void)h264::parse_sps_extension(br, b);
    });
    CHECK(diag_codes(p.tree.root()) == std::vector<std::string>{"SYN-H264-0003"});
    CHECK(find_node(p.tree.root(), "additional_extension_data") != nullptr);
  }
}

TEST_CASE("h264_subset_sps_mvc_unit", "[syntax][h264][fast]") {
  SECTION("AT3: jvt_MVCNV_1's subset SPS equals GStreamer") {
    h264::SubsetSps s;
    const auto p = parse_plain(
        from_hex(kMvcnv1SubsetSps),
        [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) { s = h264::parse_subset_sps(br, b); });
    check_mvcnv1(s);
    CHECK_FALSE(s.mvc_vui_parameters_present_flag);
    CHECK(diag_codes(p.tree.root()).empty());
    CHECK(find_node(p.tree.root(), "seq_parameter_set_mvc_extension") != nullptr);
    CHECK(find_node(p.tree.root(), "rbsp_stop_one_bit") != nullptr);
  }
  SECTION("four views with inter-view references and two level values") {
    h264::SubsetSps s;
    (void)parse_plain(from_hex(kMvc4Views), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      s = h264::parse_subset_sps(br, b);
    });
    CHECK(s.sps.seq_parameter_set_id == 3);
    CHECK(s.sps.profile_idc == 128);
    CHECK(s.mvc.view_id == std::vector<uint32_t>{0, 4, 2, 6});
    CHECK(s.mvc.anchor_ref_l0 == Refs{{}, {0}, {0}, {2, 0}});
    CHECK(s.mvc.anchor_ref_l1 == Refs{{}, {}, {4}, {4}});
    CHECK(s.mvc.non_anchor_ref_l0 == Refs{{}, {0}, {0}, {2}});
    CHECK(s.mvc.non_anchor_ref_l1 == Refs{{}, {}, {}, {4}});
    REQUIRE(s.mvc.level_values.size() == 2);
    CHECK(s.mvc.level_values[0].level_idc == 40);
    REQUIRE(s.mvc.level_values[0].ops.size() == 2);
    CHECK(s.mvc.level_values[0].ops[1].temporal_id == 2);
    CHECK(s.mvc.level_values[0].ops[1].target_view_id == std::vector<uint32_t>{0, 4});
    CHECK(s.mvc.level_values[0].ops[1].num_views_minus1 == 1);
    CHECK(s.mvc.level_values[1].level_idc == 41);
    REQUIRE(s.mvc.level_values[1].ops.size() == 1);
    CHECK(s.mvc.level_values[1].ops[0].temporal_id == 4);
    CHECK(s.mvc.level_values[1].ops[0].target_view_id == std::vector<uint32_t>{0, 4, 2, 6});
    CHECK(s.mvc.level_values[1].ops[0].num_views_minus1 == 3);
  }
  SECTION("additional_extension2 data is read to the trailing bits") {
    h264::SubsetSps s;
    const auto p = parse_plain(from_hex(kMvc2ViewsExt2), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      s = h264::parse_subset_sps(br, b);
    });
    CHECK(s.sps.seq_parameter_set_id == 31);
    CHECK(s.mvc.view_id == std::vector<uint32_t>{5, 9});
    CHECK(s.mvc.anchor_ref_l0 == Refs{{}, {0}});
    CHECK(s.mvc.anchor_ref_l1 == Refs{{}, {0}});
    CHECK(s.mvc.non_anchor_ref_l0 == Refs{{}, {}});
    CHECK(s.mvc.non_anchor_ref_l1 == Refs{{}, {0}});
    REQUIRE(s.mvc.level_values.size() == 1);
    CHECK(s.mvc.level_values[0].level_idc == 51);
    CHECK(s.mvc.level_values[0].ops[0].target_view_id == std::vector<uint32_t>{9});
    CHECK(s.additional_extension2_flag);
    CHECK(s.additional_extension2_data_bits == 4);
    CHECK(diag_codes(p.tree.root()).empty());
  }
  SECTION("SVC, MVCD and the MVC VUI extension are kept as bytes with SYN-H264-0071 (#552)") {
    for (const auto hex : {kSvcSubsetSps, kMvcdSubsetSps, kMvcVuiSubsetSps}) {
      h264::SubsetSps s;
      const auto p = parse_plain(from_hex(hex), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
        s = h264::parse_subset_sps(br, b);
      });
      CAPTURE(hex);
      CHECK(s.sps.pic_width_in_mbs_minus1 == 119);
      CHECK(diag_codes(p.tree.root()) == std::vector<std::string>{"SYN-H264-0071"});
      CHECK(find_node(p.tree.root(), "subset_sps_extension") != nullptr);
    }
  }
  SECTION("a profile without an extension: seq_parameter_set_data() then additional_extension2_flag") {
    h264::SubsetSps s;
    const auto p = parse_plain(from_hex(kHighSubsetSps), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
      s = h264::parse_subset_sps(br, b);
    });
    CHECK(s.extension == h264::SubsetSpsExtension::None);
    CHECK(s.sps.seq_parameter_set_id == 6);
    CHECK_FALSE(s.additional_extension2_flag);
    CHECK(diag_codes(p.tree.root()).empty());
  }
  SECTION("num_views_minus1 1024 and num_anchor_refs_l0 above num_views_minus1 throw") {
    // Script-written: seq_parameter_set_data() (profile 118), bit_equal_to_one, then num_views_minus1 = 1024, or
    // num_views_minus1 = 1 with num_anchor_refs_l0[1] = 2 > Min(15, num_views_minus1).
    for (const auto& [hex, name] : {std::pair{"6f760028ace807802264008038", "num_views_minus1"},
                                    std::pair{"6f760028ace80780226553e0", "num_anchor_refs_l0"}}) {
      try {
        (void)parse_plain(from_hex(hex), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) {
          (void)h264::parse_subset_sps(br, b);
        });
        FAIL("out-of-range count accepted");
      } catch (const ParseError& e) {
        CHECK(e.code == "SYN-H264-0002");
        CHECK_THAT(e.what(), Catch::Matchers::ContainsSubstring(name));
      }
    }
  }
}

TEST_CASE("h264_subset_sps_mvc_unit_corpus_mvcnv1", "[syntax][h264][slow][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("jvt_MVCNV_1");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  std::optional<h264::SubsetSps> found;
  ovt::syntax::scan_annexb(*file, [&](const ovt::syntax::ScannedUnit& u) {
    if (found || u.bytes.empty() || (u.bytes[0] & 0x1FU) != 15) {
      return;
    }
    const auto p = parse_plain(
        std::vector<uint8_t>(u.bytes.begin(), u.bytes.end()),
        [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader&) { found = h264::parse_subset_sps(br, b); });
    CHECK(diag_codes(p.tree.root()).empty());
  });
  REQUIRE(found.has_value());
  check_mvcnv1(*found);
}

TEST_CASE("h264_prefix_nal", "[syntax][h264][fast]") {
  SECTION("an MVC prefix is empty") {
    h264::PrefixNalUnit pn;
    const auto p = parse_plain(from_hex("6e400007"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader& h) {
      pn = h264::parse_prefix_nal_unit(br, b, h);
    });
    CHECK_FALSE(pn.svc);
    CHECK(pn.span.bit_length == 0);
    CHECK(diag_codes(p.tree.root()).empty());
    CHECK(find_node(p.tree.root(), "prefix_nal_unit_rbsp") != nullptr);
  }
  SECTION("an SVC prefix payload is kept as bytes with SYN-H264-0071 (#552)") {
    h264::PrefixNalUnit pn;
    const auto p = parse_plain(from_hex("6e80800720"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader& h) {
      pn = h264::parse_prefix_nal_unit(br, b, h);
    });
    CHECK(pn.svc);
    CHECK(pn.svc_kept_as_bytes);
    CHECK(diag_codes(p.tree.root()) == std::vector<std::string>{"SYN-H264-0071"});
    CHECK(find_node(p.tree.root(), "prefix_nal_unit_svc") != nullptr);
  }
  SECTION("an SVC prefix without payload (nal_ref_idc 0) records nothing") {
    h264::PrefixNalUnit pn;
    const auto p = parse_plain(from_hex("0e808007"), [&](BitReader& br, SyntaxBuilder& b, const h264::NalHeader& h) {
      pn = h264::parse_prefix_nal_unit(br, b, h);
    });
    CHECK(pn.svc);
    CHECK_FALSE(pn.svc_kept_as_bytes);
    CHECK(diag_codes(p.tree.root()).empty());
  }
}
