// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.110: H.264 SEI framing and payload parsers (chapter 7 §7.3.4 rows 7–18); WI-1.110a covers the framing,
// buffering_period and pic_timing, WI-1.110b the other payloads.
//
// Fixtures: the SPS and SEI NAL units of an x264 stream (FFmpeg 6.1 libx264, testsrc2 320x240, nal-hrd=vbr,
// pic-struct=1, open-gop, bframes=2, keyint=4); expected values are what
// `ffmpeg -v trace -i s.h264 -c copy -bsf:v trace_headers -f null -` prints for the same NAL units. "Crafted" NAL
// units were assembled with the BitWriter below and run through the same command (values and bit offsets in the
// comments are its output); payloads with an SPS that x264 does not produce (VCL HRD, time_offset_length > 0, two
// CPBs) are worked by hand from H.264 (08/2021) D.1.2 / D.1.3. [needs-corpus] jvt_CVSE3_Sony_H uses the committed
// tests/golden/h264/jvt_CVSE3_Sony_H.trace_headers.json.gz; it stands in for CVSEFDFT3_Sony_E (acceptance test 2),
// which has no committed golden and whose host itu.int is unreachable from the build hosts.

#include "ovt/cxx/syntax/h264/sei.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
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
using namespace ovt::syntax::h264;

std::vector<uint8_t> from_hex(std::string_view hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

/// Writes spec descriptors MSB first for the hand-made payloads.
class BitWriter {
 public:
  void u(unsigned n, uint64_t v) {
    for (unsigned i = n; i-- > 0;) {
      bits_.push_back(((v >> i) & 1U) != 0);
    }
  }
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
  /// sei_payload(): payload_bit_equal_to_one / _zero up to the byte boundary.
  [[nodiscard]] std::vector<uint8_t> payload() {
    if (bits_.size() % 8 != 0) {
      u(1, 1);
      while (bits_.size() % 8 != 0) {
        u(1, 0);
      }
    }
    std::vector<uint8_t> out(bits_.size() / 8, 0);
    for (size_t i = 0; i < bits_.size(); ++i) {
      if (bits_[i]) {
        out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80U >> (i % 8)));
      }
    }
    return out;
  }

 private:
  std::vector<bool> bits_;
};

struct Msg {
  uint32_t type;
  std::vector<uint8_t> payload;
};

void put_ff_coded(std::vector<uint8_t>& out, uint32_t v) {
  while (v >= 255) {
    out.push_back(0xFF);
    v -= 255;
  }
  out.push_back(static_cast<uint8_t>(v));
}

/// A SEI NAL unit (nal_ref_idc 0, type 6) holding `msgs` followed by rbsp_trailing_bits(); no emulation prevention.
std::vector<uint8_t> sei_nal(const std::vector<Msg>& msgs) {
  std::vector<uint8_t> nal{0x06};
  for (const Msg& m : msgs) {
    put_ff_coded(nal, m.type);
    put_ff_coded(nal, static_cast<uint32_t>(m.payload.size()));
    nal.insert(nal.end(), m.payload.begin(), m.payload.end());
  }
  nal.push_back(0x80);
  return nal;
}

constexpr std::string_view kSps =
    "674d4015eca0a0fd808800000300080000030193140007a12000f425331807c58b6580";  // x264: NAL HRD, pic_struct_present
constexpr std::string_view kBp = "06000693c6782328c080";
constexpr std::string_view kPicTiming = "06010300101080";
constexpr std::string_view kMdcv = "06891833c286c41d4c0bb884d03e803d13404200989680000003000180";
constexpr std::string_view kCll = "06900403e8019080";
constexpr std::string_view kAtc = "0693011280";
constexpr std::string_view kFpa = "062d07818100000300012080";
constexpr std::string_view kRecovery = "060601c480";
constexpr std::string_view kUduHex =
    "0605ffffff5fdc45e9bde6d948b7962cd820d923eeef78323634202d20636f72652031363420723331303820333165313966"
    "39202d20482e3236342f4d5045472d342041564320636f646563202d20436f70796c65667420323030332d32303233202d20"
    "687474703a2f2f7777772e766964656f6c616e2e6f72672f783236342e68746d6c202d206f7074696f6e733a206361626163"
    "3d30207265663d31206465626c6f636b3d303a303a3020616e616c7973653d303a30206d653d646961207375626d653d3020"
    "7073793d31207073795f72643d312e30303a302e3030206d697865645f7265663d30206d655f72616e67653d313620636872"
    "6f6d615f6d653d31207472656c6c69733d30203878386463743d302063716d3d3020646561647a6f6e653d32312c31312066"
    "6173745f70736b69703d31206368726f6d615f71705f6f66667365743d3020746872656164733d36206c6f6f6b6168656164"
    "5f746872656164733d3120736c696365645f746872656164733d30206e723d3020646563696d6174653d3120696e7465726c"
    "616365643d3020626c757261795f636f6d7061743d3020636f6e73747261696e65645f696e7472613d3020626672616d6573"
    "3d3220625f707972616d69643d3220625f61646170743d3020625f626961733d30206469726563743d312077656967687462"
    "3d30206f70656e5f676f703d3120776569676874703d30206b6579696e743d34206b6579696e745f6d696e3d33207363656e"
    "656375743d3020696e7472615f726566726573683d302072635f6c6f6f6b61686561643d302072633d637266206d62747265"
    "653d30206372663d32332e302071636f6d703d302e36302071706d696e3d302071706d61783d3639207170737465703d3420"
    "7662765f6d6178726174653d32303030207662765f62756673697a653d34303030206372665f6d61783d302e30206e616c5f"
    "6872643d7662722066696c6c65723d30206d6173746572696e672d646973706c61793d472831333235302c33343530302942"
    "28373530302c3330303029522833343030302c31363030302957502831353633352c3136343530294c283130303030303030"
    "2c312920636c6c3d313030302c343030206672616d652d7061636b696e673d332069705f726174696f3d312e34302070625f"
    "726174696f3d312e33302061713d300080";
struct Parsed {
  std::vector<uint8_t> rbsp;
  SeiRbsp sei;
  SyntaxNodeBuffer tree;
  std::optional<ParseError> error;
};

Parsed parse_sei(const std::vector<uint8_t>& nal, SeiContext& ctx, bool recording = true) {
  Parsed p;
  p.rbsp = ovt::rbsp_from_nal(nal).rbsp;
  BitReader br(p.rbsp);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    (void)parse_nal_header(br, b);
    p.sei = parse_sei_rbsp(br, b, ctx);
  } catch (const ParseError& e) {
    p.error = e;
  }
  p.tree = b.finish();
  p.tree.set_root_name("nal_unit");
  return p;
}

struct Env {
  std::optional<Sps> sps;
  std::array<const Sps*, 32> table{};
  SeiContext ctx;

  explicit Env(const std::vector<uint8_t>& sps_nal = from_hex(kSps)) {
    const auto rbsp = ovt::rbsp_from_nal(sps_nal).rbsp;
    BitReader br(rbsp);
    SyntaxBuilder b(br);
    (void)parse_nal_header(br, b);
    sps = parse_sps(br, b);
    (void)b.finish();
    table[sps->seq_parameter_set_id] = &*sps;
    ctx.sps_by_id = table;
    ctx.active_sps = &*sps;
  }
};

const SyntaxNodeData* find_node(const SyntaxNodeData* n, std::string_view name, int array_index = -1) {
  if (n == nullptr) {
    return nullptr;
  }
  if (n->name == name && !n->array_element && (array_index < 0 || n->array_index == array_index)) {
    return n;
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    if (const auto* hit = find_node(c, name, array_index)) {
      return hit;
    }
  }
  return nullptr;
}

void collect_diags(const SyntaxNodeData* n, std::vector<std::string>& codes) {
  for (const auto* d = n->first_diag; d != nullptr; d = d->next) {
    codes.emplace_back(d->code);
  }
  for (const SyntaxNodeData* c = n->first_child; c != nullptr; c = c->next_sibling) {
    collect_diags(c, codes);
  }
}

std::vector<std::string> diag_codes(const Parsed& p) {
  std::vector<std::string> codes;
  if (p.tree.root() != nullptr) {
    collect_diags(p.tree.root(), codes);
  }
  return codes;
}

template <class T>
const T& payload_as(const SeiMessage& m) {
  REQUIRE(std::holds_alternative<T>(m.payload));
  return std::get<T>(m.payload);
}

}  // namespace

// ---- the x264 stream (oracle: trace_headers) ---------------------------------------------------------------------

TEST_CASE("h264_sei_buffering_period_x264", "[syntax][h264][sei][fast]") {
  Env env;
  auto p = parse_sei(from_hex(kBp), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sei.complete);
  REQUIRE(p.sei.messages.size() == 1);
  const SeiMessage& m = p.sei.messages[0];
  CHECK(m.payload_type == 0);
  CHECK(m.payload_size == 6);
  CHECK(m.name == "buffering_period");
  CHECK(m.parsed);
  const auto& bp = payload_as<BufferingPeriod>(m);
  // trace_headers: seq_parameter_set_id 0, initial_cpb_removal_delay[0] 161999, ..._offset[0] 18001
  CHECK(bp.seq_parameter_set_id == 0);
  REQUIRE(bp.nal_initial_cpb_removal_delay.size() == 1);
  CHECK(bp.nal_initial_cpb_removal_delay[0] == 161999);
  CHECK(bp.nal_initial_cpb_removal_delay_offset[0] == 18001);
  CHECK(bp.vcl_initial_cpb_removal_delay.empty());
  CHECK(env.ctx.buffering_period_sps_id == 0);
  // trace_headers bit offsets: the payload starts at bit 24 and carries two 20-bit values
  CHECK(find_node(p.tree.root(), "initial_cpb_removal_delay", 0)->span.bit_offset == 25);
  CHECK(find_node(p.tree.root(), "initial_cpb_removal_delay_offset", 0)->span.bit_offset == 45);
  const auto* one = find_node(p.tree.root(), "payload_bit_equal_to_one");
  REQUIRE(one != nullptr);
  CHECK(one->span.bit_offset == 65);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_pic_timing_x264", "[syntax][h264][sei][fast]") {
  Env env;
  auto p = parse_sei(from_hex(kPicTiming), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sei.messages.size() == 1);
  const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
  // trace_headers: cpb_removal_delay 0 (7 bits), dpb_output_delay 4 (7 bits), pic_struct 0, clock_timestamp_flag[0] 0
  CHECK(pt.sps_known);
  CHECK(pt.cpb_dpb_delays_present);
  CHECK(pt.cpb_removal_delay == 0);
  CHECK(pt.dpb_output_delay == 4);
  CHECK(pt.pic_struct_present);
  CHECK(pt.pic_struct == 0);
  CHECK(pt.pic_struct_name == "frame");
  CHECK(pt.NumClockTS == 1);
  REQUIRE(pt.clock_timestamps.size() == 1);
  CHECK_FALSE(pt.clock_timestamps[0].clock_timestamp_flag);
  CHECK(find_node(p.tree.root(), "dpb_output_delay")->span.bit_offset == 31);
  CHECK(find_node(p.tree.root(), "pic_struct")->span.bit_offset == 38);
  CHECK(find_node(p.tree.root(), "clock_timestamp_flag")->span.bit_offset == 42);
  CHECK(diag_codes(p).empty());
}

// ---- crafted payloads verified with trace_headers -----------------------------------------------------------------

TEST_CASE("h264_sei_pic_timing_clock_timestamps", "[syntax][h264][sei][fast]") {
  Env env;
  // pic_struct 3 (NumClockTS 2): ts0 full 12:44:33 frame 14 (counting_type 2, cnt_dropped), ts1 absent. NAL
  // 0601070a0cec543a1b1880; trace_headers (bit offsets from the NAL start): cpb_removal_delay 5 @24, dpb_output_delay 3
  // @31, pic_struct 3 @38, clock_timestamp_flag[0] 1 @42, ct_type 1 @43, nuit_field_based_flag 1 @45, counting_type 2
  // @46, full_timestamp_flag 1 @51, discontinuity_flag 0 @52, cnt_dropped_flag 1 @53, n_frames 14 @54, seconds_value 33
  // @62, minutes_value 44 @68, hours_value 12 @74, clock_timestamp_flag[1] 0 @79, rbsp_stop_one_bit @80.
  auto p = parse_sei(from_hex("0601070a0cec543a1b1880"), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
  CHECK(pt.cpb_removal_delay == 5);
  CHECK(pt.dpb_output_delay == 3);
  CHECK(pt.pic_struct == 3);
  CHECK(pt.pic_struct_name == "top field, bottom field, in that order");
  CHECK(pt.NumClockTS == 2);
  REQUIRE(pt.clock_timestamps.size() == 2);
  const ClockTimestamp& c = pt.clock_timestamps[0];
  CHECK(c.clock_timestamp_flag);
  CHECK(c.ct_type == 1);
  CHECK(c.nuit_field_based_flag);
  CHECK(c.counting_type == 2);
  CHECK(c.full_timestamp_flag);
  CHECK_FALSE(c.discontinuity_flag);
  CHECK(c.cnt_dropped_flag);
  CHECK(c.n_frames == 14);
  CHECK(c.seconds_value == 33);
  CHECK(c.minutes_value == 44);
  CHECK(c.hours_value == 12);
  CHECK(c.text == "12:44:33:14");
  CHECK_FALSE(pt.clock_timestamps[1].clock_timestamp_flag);
  CHECK(find_node(p.tree.root(), "n_frames")->span.bit_offset == 54);
  CHECK(find_node(p.tree.root(), "hours_value")->span.bit_offset == 74);
  CHECK(find_node(p.tree.root(), "clock_timestamp_flag", 1)->span.bit_offset == 79);
  CHECK(find_node(p.tree.root(), "clockTimestamp")->derived == "12:44:33:14");
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_pic_timing_partial_timestamp", "[syntax][h264][sei][fast]") {
  Env env;
  // NAL 060107020830080e3c8480; trace_headers: cpb_removal_delay 1, dpb_output_delay 2, pic_struct 0,
  // clock_timestamp_flag[0] 1, ct_type 2 @43, counting_type 0, full_timestamp_flag 0 @51, discontinuity_flag 1,
  // cnt_dropped_flag 0, n_frames 3 @54, seconds_flag 1 @62, seconds_value 7 @63, minutes_flag 1 @69,
  // minutes_value 8 @70, hours_flag 0 @76, bit_equal_to_one @77.
  auto p = parse_sei(from_hex("060107020830080e3c8480"), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
  CHECK(pt.cpb_removal_delay == 1);
  CHECK(pt.dpb_output_delay == 2);
  REQUIRE(pt.clock_timestamps.size() == 1);
  const ClockTimestamp& c = pt.clock_timestamps[0];
  CHECK(c.ct_type == 2);
  CHECK_FALSE(c.full_timestamp_flag);
  CHECK(c.discontinuity_flag);
  CHECK(c.n_frames == 3);
  CHECK(c.seconds_flag);
  CHECK(c.seconds_value == 7);
  CHECK(c.minutes_flag);
  CHECK(c.minutes_value == 8);
  CHECK_FALSE(c.hours_flag);
  CHECK(c.text == "00:08:07:03");
  CHECK(find_node(p.tree.root(), "seconds_flag")->span.bit_offset == 62);
  CHECK(find_node(p.tree.root(), "minutes_value")->span.bit_offset == 70);
  CHECK(find_node(p.tree.root(), "hours_flag")->span.bit_offset == 76);
  CHECK(find_node(p.tree.root(), "payload_bit_equal_to_one")->span.bit_offset == 77);
}

// ---- hand-worked payloads (H.264 Annex D) -------------------------------------------------------------------------

TEST_CASE("h264_sei_pic_timing_time_offset", "[syntax][h264][sei][fast]") {
  // A VCL-only HRD with time_offset_length 5, cpb/dpb delay lengths 4 and 3: the delay fields come from the VCL HRD
  // when there is no NAL HRD (E.2.1), and time_offset is i(5).
  Sps sps;
  sps.vui_parameters_present_flag = true;
  sps.vui.pic_struct_present_flag = true;
  sps.vui.vcl_hrd_parameters_present_flag = true;
  sps.vui.vcl_hrd.cpb_removal_delay_length_minus1 = 3;
  sps.vui.vcl_hrd.dpb_output_delay_length_minus1 = 2;
  sps.vui.vcl_hrd.time_offset_length = 5;
  std::array<const Sps*, 32> table{};
  table[0] = &sps;
  SeiContext ctx;
  ctx.sps_by_id = table;
  ctx.active_sps = &sps;
  BitWriter w;
  w.u(4, 9);        // cpb_removal_delay
  w.u(3, 5);        // dpb_output_delay
  w.u(4, 0);        // pic_struct 0: one clock timestamp
  w.u(1, 1);        // clock_timestamp_flag
  w.u(2, 0);        // ct_type
  w.u(1, 0);        // nuit_field_based_flag
  w.u(5, 1);        // counting_type
  w.u(1, 0);        // full_timestamp_flag
  w.u(1, 0);        // discontinuity_flag
  w.u(1, 0);        // cnt_dropped_flag
  w.u(8, 25);       // n_frames
  w.u(1, 0);        // seconds_flag: no seconds, minutes or hours follow
  w.u(5, 0b11101);  // time_offset = -3 as 5-bit two's complement
  auto p = parse_sei(sei_nal({{1, w.payload()}}), ctx);
  REQUIRE_FALSE(p.error.has_value());
  const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
  CHECK(pt.cpb_removal_delay == 9);
  CHECK(pt.dpb_output_delay == 5);
  REQUIRE(pt.clock_timestamps.size() == 1);
  CHECK(pt.clock_timestamps[0].n_frames == 25);
  CHECK(pt.clock_timestamps[0].time_offset == -3);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_pic_timing_without_hrd_or_pic_struct", "[syntax][h264][sei][fast]") {
  // No VUI at all: pic_timing() is empty (D.1.3), so a payload of one byte is only the payload extension.
  Sps sps;
  std::array<const Sps*, 32> table{};
  table[0] = &sps;
  SeiContext ctx;
  ctx.sps_by_id = table;
  ctx.active_sps = &sps;
  auto p = parse_sei(sei_nal({{1, {0xA5}}}), ctx);
  REQUIRE_FALSE(p.error.has_value());
  const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
  CHECK_FALSE(pt.cpb_dpb_delays_present);
  CHECK_FALSE(pt.pic_struct_present);
  CHECK(pt.clock_timestamps.empty());
  CHECK(find_node(p.tree.root(), "reserved_payload_extension_data") != nullptr);
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H264-0032"});
}

TEST_CASE("h264_sei_pic_timing_sps_selection", "[syntax][h264][sei][fast]") {
  // D.2.2: pic_timing() uses the SPS of the previous picture unless a buffering_period() of the same access unit names
  // another one. SPS 0 has a 4-bit cpb delay, SPS 1 an 8-bit one.
  Sps a;
  a.vui_parameters_present_flag = true;
  a.vui.nal_hrd_parameters_present_flag = true;
  a.vui.nal_hrd.cpb_removal_delay_length_minus1 = 3;
  a.vui.nal_hrd.dpb_output_delay_length_minus1 = 3;
  a.vui.nal_hrd.initial_cpb_removal_delay_length_minus1 = 7;
  Sps b = a;
  b.seq_parameter_set_id = 1;
  b.vui.nal_hrd.cpb_removal_delay_length_minus1 = 7;
  std::array<const Sps*, 32> table{};
  table[0] = &a;
  table[1] = &b;
  SeiContext ctx;
  ctx.sps_by_id = table;
  ctx.active_sps = &a;
  BitWriter delays;
  delays.u(8, 0x5A);  // cpb_removal_delay 0x5A with SPS 1, or 5 and a dpb delay of 0xA with SPS 0
  delays.u(4, 3);
  BitWriter bp;
  bp.ue(1);      // seq_parameter_set_id 1
  bp.u(8, 100);  // initial_cpb_removal_delay
  bp.u(8, 7);    // initial_cpb_removal_delay_offset
  const auto timing = delays.payload();
  {
    SeiContext solo = ctx;
    auto p = parse_sei(sei_nal({{1, timing}}), solo);
    const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
    CHECK(pt.cpb_removal_delay == 5);  // 4-bit field of SPS 0
    CHECK(pt.dpb_output_delay == 0xA);
  }
  {
    SeiContext with_bp = ctx;
    auto p = parse_sei(sei_nal({{0, bp.payload()}, {1, timing}}), with_bp);
    REQUIRE(p.sei.messages.size() == 2);
    CHECK(payload_as<BufferingPeriod>(p.sei.messages[0]).seq_parameter_set_id == 1);
    const auto& pt = payload_as<PicTiming>(p.sei.messages[1]);
    CHECK(pt.cpb_removal_delay == 0x5A);  // 8-bit field of SPS 1
    CHECK(pt.dpb_output_delay == 3);
    CHECK(with_bp.buffering_period_sps_id == 1);
  }
}

TEST_CASE("h264_sei_buffering_period_nal_and_vcl", "[syntax][h264][sei][fast]") {
  // NAL HRD with two CPBs (16-bit fields) then VCL HRD with one CPB (12-bit fields): the NAL pairs come first (D.1.2).
  Sps sps;
  sps.vui_parameters_present_flag = true;
  sps.vui.nal_hrd_parameters_present_flag = true;
  sps.vui.nal_hrd.cpb_cnt_minus1 = 1;
  sps.vui.nal_hrd.initial_cpb_removal_delay_length_minus1 = 15;
  sps.vui.vcl_hrd_parameters_present_flag = true;
  sps.vui.vcl_hrd.initial_cpb_removal_delay_length_minus1 = 11;
  std::array<const Sps*, 32> table{};
  table[0] = &sps;
  SeiContext ctx;
  ctx.sps_by_id = table;
  BitWriter w;
  w.ue(0);
  w.u(16, 1000);
  w.u(16, 1);
  w.u(16, 2000);
  w.u(16, 2);
  w.u(12, 300);
  w.u(12, 3);
  auto p = parse_sei(sei_nal({{0, w.payload()}}), ctx);
  REQUIRE_FALSE(p.error.has_value());
  const auto& bp = payload_as<BufferingPeriod>(p.sei.messages[0]);
  CHECK(bp.nal_initial_cpb_removal_delay == std::vector<uint32_t>{1000, 2000});
  CHECK(bp.nal_initial_cpb_removal_delay_offset == std::vector<uint32_t>{1, 2});
  CHECK(bp.vcl_initial_cpb_removal_delay == std::vector<uint32_t>{300});
  CHECK(bp.vcl_initial_cpb_removal_delay_offset == std::vector<uint32_t>{3});
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_buffering_period_without_sps", "[syntax][h264][sei][fast]") {
  SeiContext ctx;  // no SPS known
  BitWriter w;
  w.ue(0);
  w.u(20, 5);
  w.u(20, 6);
  auto p = parse_sei(sei_nal({{0, w.payload()}}), ctx);
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sei.messages.size() == 1);
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(std::holds_alternative<std::monostate>(p.sei.messages[0].payload));
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H264-0030"});
  const auto* n = find_node(p.tree.root(), "buffering_period");
  REQUIRE(n != nullptr);
  CHECK(n->kind == ovt::syntax::ValueKind::Bytes);
  CHECK_FALSE(ctx.buffering_period_sps_id.has_value());
}

TEST_CASE("h264_sei_pic_timing_without_sps", "[syntax][h264][sei][fast]") {
  SeiContext ctx;
  auto p = parse_sei(sei_nal({{1, {0x01, 0x02, 0x03}}}), ctx);
  REQUIRE_FALSE(p.error.has_value());
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H264-0031"});
}

// ---- framing ------------------------------------------------------------------------------------------------------

TEST_CASE("h264_sei_framing_two_messages_and_ff_bytes", "[syntax][h264][sei][fast]") {
  Env env;
  // Recovery point, then an unknown payload type 300 (ff_byte + 45) of 260 bytes (ff_byte + 5).
  std::vector<uint8_t> big(260, 0x11);
  auto p = parse_sei(sei_nal({{6, {0xC4}}, {300, big}}), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sei.complete);
  REQUIRE(p.sei.messages.size() == 2);
  CHECK(p.sei.messages[0].name == "recovery_point");
  CHECK(p.sei.messages[1].payload_type == 300);
  CHECK(p.sei.messages[1].payload_size == 260);
  CHECK(p.sei.messages[1].name == "reserved_sei_message");
  CHECK_FALSE(p.sei.messages[1].parsed);
  CHECK(find_node(p.tree.root(), "payloadType", 0)->derived == "payloadType = 6");
  CHECK(find_node(p.tree.root(), "payloadType", 1)->derived == "payloadType = 300");
  CHECK(find_node(p.tree.root(), "payloadSize", 1)->derived == "payloadSize = 260");
  // sei_message[1]: ff_byte at bit 24 + 8 * 2 (message 0 is 3 bytes: type, size, payload), then last_payload_type_byte
  CHECK(find_node(p.tree.root(), "last_payload_type_byte", 1)->uvalue == 45);
  CHECK(find_node(p.tree.root(), "last_payload_size_byte", 1)->uvalue == 5);
  CHECK(find_node(p.tree.root(), "rbsp_trailing_bits") != nullptr);
  CHECK(diag_codes(p).empty());
  CHECK(p.sei.messages[0].payload_span.bit_offset == 24);
  CHECK(p.sei.messages[0].payload_span.bit_length == 8);
}

TEST_CASE("h264_sei_other_payload_types_keep_bytes", "[syntax][h264][sei][fast]") {
  Env env;
  // Until WI-1.110b, every payload except 0 and 1 is kept as bytes under its Annex D name.
  auto p = parse_sei(sei_nal({{2, {1, 2, 3, 4}}, {3, {0xFF, 0xFF}}, {56, {9}}}), env.ctx);
  REQUIRE(p.sei.messages.size() == 3);
  CHECK(p.sei.messages[0].name == "pan_scan_rect");
  CHECK(p.sei.messages[1].name == "filler_payload");
  CHECK(p.sei.messages[2].name == "green_metadata");
  for (const SeiMessage& m : p.sei.messages) {
    CHECK_FALSE(m.parsed);
  }
  const auto* n = find_node(p.tree.root(), "pan_scan_rect");
  REQUIRE(n != nullptr);
  CHECK(n->kind == ovt::syntax::ValueKind::Bytes);
  CHECK(n->bytes.size() == 4);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_payload_size_beyond_rbsp", "[syntax][h264][sei][fast]") {
  Env env;
  // Acceptance test 3: payloadSize 200 with 3 bytes left. BR-0001 is caught by the framing, one Diagnostic, and the
  // second message (which would be a valid recovery point) is not parsed.
  std::vector<uint8_t> nal{0x06, 0x03, 200, 1, 2, 3};
  auto p = parse_sei(nal, env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  CHECK_FALSE(p.sei.complete);
  CHECK(p.sei.messages.empty());
  CHECK(diag_codes(p) == std::vector<std::string>{"BR-0001"});
  // The same with an earlier valid message: it stays, the later one is dropped.
  std::vector<uint8_t> nal2{0x06, 0x06, 0x01, 0xC4, 0x03, 200, 6, 1, 0xC4, 0x80};
  auto q = parse_sei(nal2, env.ctx);
  REQUIRE(q.sei.messages.size() == 1);
  CHECK(q.sei.messages[0].name == "recovery_point");
  CHECK_FALSE(q.sei.complete);
  CHECK(diag_codes(q) == std::vector<std::string>{"BR-0001"});
  CHECK(find_node(q.tree.root(), "rbsp_trailing_bits") == nullptr);
}

TEST_CASE("h264_sei_payload_overrun_does_not_reach_next_message", "[syntax][h264][sei][fast]") {
  Env env;
  // pic_timing needs 7 + 7 + 4 bits of delays and pic_struct, but payloadSize is 1: the read of dpb_output_delay
  // crosses the payload end. The next message (the pic_timing of the x264 stream) is still parsed from its own bytes.
  auto p = parse_sei(sei_nal({{1, {0xAA}}, {1, {0x00, 0x10, 0x10}}}), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  CHECK(p.sei.complete);
  REQUIRE(p.sei.messages.size() == 2);
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(p.sei.messages[1].parsed);
  CHECK(payload_as<PicTiming>(p.sei.messages[1]).dpb_output_delay == 4);
  CHECK(diag_codes(p) == std::vector<std::string>{"BR-0001"});
  CHECK(find_node(p.tree.root(), "rbsp_trailing_bits") != nullptr);
}

TEST_CASE("h264_sei_unaligned_payload_alignment_bits", "[syntax][h264][sei][fast]") {
  Env env;
  // The buffering_period of the stream ends after 41 bits: payload_bit_equal_to_one then six
  // payload_bit_equal_to_zero.
  auto p = parse_sei(from_hex(kBp), env.ctx);
  const auto* one = find_node(p.tree.root(), "payload_bit_equal_to_one");
  REQUIRE(one != nullptr);
  CHECK(one->span.bit_offset == 24 + 41);
  const auto* zero = find_node(p.tree.root(), "payload_bit_equal_to_zero");
  REQUIRE(zero != nullptr);
  CHECK(zero->span.bit_offset == 24 + 42);
}

TEST_CASE("h264_sei_bad_alignment_bits_are_diagnostics", "[syntax][h264][sei][fast]") {
  Env env;
  // The last payload byte 0xC1 ends in a one bit where payload_bit_equal_to_zero requires 0.
  auto p = parse_sei(sei_nal({{0, {0x93, 0xC6, 0x78, 0x23, 0x28, 0xC1}}}), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  CHECK(p.sei.messages[0].parsed);
  CHECK(diag_codes(p) == std::vector<std::string>{"SYN-GEN-0001"});
}

TEST_CASE("h264_sei_recording_off_gives_the_same_values", "[syntax][h264][sei][fast]") {
  Env env;
  const auto nal = sei_nal({{0, {0x93, 0xC6, 0x78, 0x23, 0x28, 0xC0}}, {1, {0x00, 0x10, 0x10}}});
  SeiContext on = env.ctx;
  SeiContext off = env.ctx;
  auto a = parse_sei(nal, on, true);
  auto b = parse_sei(nal, off, false);
  REQUIRE(a.sei.messages.size() == b.sei.messages.size());
  for (size_t i = 0; i < a.sei.messages.size(); ++i) {
    CHECK(a.sei.messages[i].payload.index() == b.sei.messages[i].payload.index());
  }
  CHECK(b.tree.empty());
  CHECK(payload_as<PicTiming>(a.sei.messages[1]).dpb_output_delay ==
        payload_as<PicTiming>(b.sei.messages[1]).dpb_output_delay);
}

TEST_CASE("h264_sei_output_is_deterministic", "[syntax][h264][sei][fast]") {
  Env env;
  const auto nal = from_hex(kBp);
  SeiContext c1 = env.ctx;
  SeiContext c2 = env.ctx;
  const auto a = parse_sei(nal, c1);
  const auto b = parse_sei(nal, c2);
  CHECK(a.tree.to_flatbuffer() == b.tree.to_flatbuffer());
  CHECK(ovt::syntax::to_json(a.tree) == ovt::syntax::to_json(b.tree));
}

// ---- corpus -------------------------------------------------------------------------------------------------------

TEST_CASE("h264_sei_hdr_payloads_x264", "[syntax][h264][sei][fast]") {
  Env env;
  {  // mastering_display_colour_volume; trace_headers: G (13250, 34500) B (7500, 3000) R (34000, 16000) WP (15635,
     // 16450) max 10000000 min 1; the payload equals tests/corpora/sei_payloads.json
     // h264_mastering_display_colour_volume.
    auto p = parse_sei(from_hex(kMdcv), env.ctx);
    REQUIRE_FALSE(p.error.has_value());
    REQUIRE(p.sei.messages.size() == 1);
    const auto& m = payload_as<MasteringDisplayColourVolume>(p.sei.messages[0]);
    CHECK(m.display_primaries_x == std::array<uint32_t, 3>{13250, 7500, 34000});
    CHECK(m.display_primaries_y == std::array<uint32_t, 3>{34500, 3000, 16000});
    CHECK(m.white_point_x == 15635);
    CHECK(m.white_point_y == 16450);
    CHECK(m.max_display_mastering_luminance == 10000000);
    CHECK(m.min_display_mastering_luminance == 1);
    // R, G, B order for ovt_ColorDescription.mdcv_primaries (the SEI carries G, B, R)
    CHECK(m.mdcv_primaries_x == std::array<uint32_t, 3>{34000, 13250, 7500});
    CHECK(m.mdcv_primaries_y == std::array<uint32_t, 3>{16000, 34500, 3000});
    // 34000 * 0.00002 = 0.68, 16000 * 0.00002 = 0.32, 13250 -> 0.265, 34500 -> 0.69, 7500 -> 0.15, 3000 -> 0.06,
    // 15635 -> 0.3127, 16450 -> 0.329, 10000000 * 0.0001 = 1000, 1 * 0.0001 = 0.0001
    CHECK(m.text ==
          "R (0.680, 0.320) G (0.265, 0.690) B (0.150, 0.060) WP (0.3127, 0.3290) max 1000 cd/m² min 0.0001 cd/m²");
    CHECK(p.sei.messages[0].name == "mastering_display_colour_volume");
    CHECK(find_node(p.tree.root(), "mastering_display")->derived == m.text);
  }
  {  // content_light_level_info; trace_headers: 1000 / 400
    auto p = parse_sei(from_hex(kCll), env.ctx);
    REQUIRE(p.sei.messages.size() == 1);
    const auto& c = payload_as<ContentLightLevelInfo>(p.sei.messages[0]);
    CHECK(c.max_content_light_level == 1000);
    CHECK(c.max_pic_average_light_level == 400);
    CHECK(find_node(p.tree.root(), "content_light_level")->derived == "MaxCLL 1000 cd/m², MaxFALL 400 cd/m²");
  }
  {  // alternative_transfer_characteristics; trace_headers: preferred_transfer_characteristics 18 (HLG)
    auto p = parse_sei(from_hex(kAtc), env.ctx);
    REQUIRE(p.sei.messages.size() == 1);
    CHECK(payload_as<AlternativeTransferCharacteristics>(p.sei.messages[0]).preferred_transfer_characteristics == 18);
    CHECK(p.sei.messages[0].name == "alternative_transfer_characteristics");
  }
}

TEST_CASE("h264_sei_recovery_point_x264", "[syntax][h264][sei][fast]") {
  Env env;
  auto p = parse_sei(from_hex(kRecovery), env.ctx);
  REQUIRE(p.sei.messages.size() == 1);
  const auto& r = payload_as<RecoveryPoint>(p.sei.messages[0]);
  // trace_headers: recovery_frame_cnt 0; payload byte 0xC4 = tests/corpora/sei_payloads.json h264_recovery_point
  CHECK(r.recovery_frame_cnt == 0);
  CHECK(r.exact_match_flag);
  CHECK_FALSE(r.broken_link_flag);
  CHECK(r.changing_slice_group_idc == 0);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_frame_packing_x264", "[syntax][h264][sei][fast]") {
  Env env;
  auto p = parse_sei(from_hex(kFpa), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sei.messages.size() == 1);
  const auto& f = payload_as<FramePackingArrangement>(p.sei.messages[0]);
  // x264 frame-packing=3 (side by side). Payload bytes 81 81 00 00 00 01 20 decoded by hand from D.1.26:
  // ue 0 | cancel 0 | type 0000011 = 3 | quincunx 0 | content_interpretation 000001 = 1 | six flags 0 | four u(4) = 0 |
  // reserved byte 0 | repetition_period ue(010) = 1 | extension_flag 0 | payload_bit_equal_to_one.
  CHECK(f.frame_packing_arrangement_id == 0);
  CHECK_FALSE(f.cancel_flag);
  CHECK(f.type == 3);
  CHECK(f.type_name == "side by side");
  CHECK_FALSE(f.quincunx_sampling_flag);
  CHECK(f.content_interpretation_type == 1);
  CHECK(f.grid_position == std::array<uint32_t, 4>{0, 0, 0, 0});
  CHECK(f.reserved_byte == 0);
  CHECK(f.repetition_period == 1);
  CHECK_FALSE(f.extension_flag);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_user_data_unregistered_x264", "[syntax][h264][sei][fast]") {
  Env env;
  auto p = parse_sei(from_hex(kUduHex), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  REQUIRE(p.sei.messages.size() == 1);
  const SeiMessage& m = p.sei.messages[0];
  CHECK(m.payload_type == 5);
  CHECK(m.payload_size == 860);  // 255 * 3 + 95: three ff_byte elements
  const auto& u = payload_as<SeiUserDataUnregistered>(m);
  CHECK(u.uuid == "DC45E9BD-E6D9-48B7-962C-D820D923EEEF");
  CHECK(u.enum_name == "x264 options");
  REQUIRE(u.text.has_value());
  CHECK_THAT(*u.text, Catch::Matchers::ContainsSubstring("options: cabac=0 ref=1"));
  CHECK(u.payload_byte_count == 860 - 16);
  // trace_headers shows the three ff_byte elements before last_payload_size_byte
  CHECK(find_node(p.tree.root(), "x264_option[cabac]") != nullptr);
  CHECK(find_node(p.tree.root(), "x264_option[cabac]")->derived == "0");
  CHECK(find_node(p.tree.root(), "x264_option[keyint]")->derived == "4");
  CHECK(find_node(p.tree.root(), "x264_option[nal_hrd]")->derived == "vbr");
  CHECK(find_node(p.tree.root(), "uuid_iso_iec_11578")->enum_name == "x264 options");
  CHECK(diag_codes(p).empty());
}

// ---- crafted payloads verified with trace_headers -----------------------------------------------------------------

TEST_CASE("h264_sei_display_orientation", "[syntax][h264][sei][fast]") {
  Env env;
  // NAL 062f0348000d80; trace_headers: display_orientation_cancel_flag 0 @24, hor_flip 1, ver_flip 0,
  // anticlockwise_rotation 16384 @27, display_orientation_repetition_period 2 @43, display_orientation_extension_flag 0
  // @46.
  auto p = parse_sei(from_hex("062f0348000d80"), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  const auto& d = payload_as<DisplayOrientation>(p.sei.messages[0]);
  CHECK_FALSE(d.cancel_flag);
  CHECK(d.hor_flip);
  CHECK_FALSE(d.ver_flip);
  CHECK(d.anticlockwise_rotation == 16384);
  CHECK(d.degrees == 90.0);  // 16384 * 360 / 65536
  CHECK(d.repetition_period == 2);
  CHECK_FALSE(d.extension_flag);
  CHECK(find_node(p.tree.root(), "anticlockwise_rotation")->span.bit_offset == 27);
  CHECK(find_node(p.tree.root(), "display_orientation_repetition_period")->span.bit_offset == 43);
}

// ---- hand-worked payloads (H.264 Annex D) -------------------------------------------------------------------------

TEST_CASE("h264_sei_film_grain_characteristics", "[syntax][h264][sei][fast]") {
  Env env;
  // D.1.20, worked by hand: model 1, separate colour description (luma 10-bit, chroma 10-bit, full range, BT.2020 PQ),
  // blending 2, log2_scale_factor 5; only component 0 present with 2 intensity intervals and 3 model values.
  BitWriter w;
  w.u(1, 0);   // film_grain_characteristics_cancel_flag
  w.u(2, 1);   // film_grain_model_id
  w.u(1, 1);   // separate_colour_description_present_flag
  w.u(3, 2);   // film_grain_bit_depth_luma_minus8
  w.u(3, 2);   // film_grain_bit_depth_chroma_minus8
  w.u(1, 1);   // film_grain_full_range_flag
  w.u(8, 9);   // film_grain_colour_primaries
  w.u(8, 16);  // film_grain_transfer_characteristics
  w.u(8, 9);   // film_grain_matrix_coefficients
  w.u(2, 2);   // blending_mode_id
  w.u(4, 5);   // log2_scale_factor
  w.u(1, 1);   // comp_model_present_flag[0]
  w.u(1, 0);
  w.u(1, 0);
  w.u(8, 1);    // num_intensity_intervals_minus1[0]
  w.u(3, 2);    // num_model_values_minus1[0]
  w.u(8, 0);    // interval 0: lower bound
  w.u(8, 127);  // upper bound
  w.se(10);
  w.se(-4);
  w.se(0);
  w.u(8, 128);  // interval 1
  w.u(8, 255);
  w.se(-1);
  w.se(2);
  w.se(3);
  w.ue(7);  // film_grain_characteristics_repetition_period
  auto p = parse_sei(sei_nal({{19, w.payload()}}), env.ctx);
  REQUIRE_FALSE(p.error.has_value());
  CHECK(p.sei.messages[0].name == "film_grain_characteristics");
  const auto& f = payload_as<FilmGrainCharacteristics>(p.sei.messages[0]);
  CHECK_FALSE(f.cancel_flag);
  CHECK(f.film_grain_model_id == 1);
  CHECK(f.separate_colour_description_present_flag);
  CHECK(f.bit_depth_luma_minus8 == 2);
  CHECK(f.full_range_flag);
  CHECK(f.colour_primaries == 9);
  CHECK(f.transfer_characteristics == 16);
  CHECK(f.blending_mode_id == 2);
  CHECK(f.log2_scale_factor == 5);
  CHECK(f.comp_model_present_flag == std::array<bool, 3>{true, false, false});
  CHECK(f.comp[0].num_intensity_intervals_minus1 == 1);
  CHECK(f.comp[0].num_model_values_minus1 == 2);
  CHECK(f.comp[0].intensity_interval_lower_bound == std::vector<uint32_t>{0, 128});
  CHECK(f.comp[0].intensity_interval_upper_bound == std::vector<uint32_t>{127, 255});
  CHECK(f.comp[0].comp_model_value == std::vector<int32_t>{10, -4, 0, -1, 2, 3});
  CHECK(f.repetition_period == 7);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_film_grain_cancel", "[syntax][h264][sei][fast]") {
  Env env;
  BitWriter w;
  w.u(1, 1);  // cancel
  auto p = parse_sei(sei_nal({{19, w.payload()}}), env.ctx);
  CHECK(payload_as<FilmGrainCharacteristics>(p.sei.messages[0]).cancel_flag);
  CHECK(diag_codes(p).empty());
}

TEST_CASE("h264_sei_tone_mapping_info", "[syntax][h264][sei][fast]") {
  Env env;
  const auto parse = [&](const BitWriter& w) { return parse_sei(sei_nal({{23, BitWriter(w).payload()}}), env.ctx); };
  {  // model 0 (linear with clipping): tone_map_id 3, repetition 0, coded depth 10, target depth 8, min 16, max 235
    BitWriter w;
    w.ue(3);
    w.u(1, 0);
    w.ue(0);
    w.u(8, 10);
    w.u(8, 8);
    w.ue(0);
    w.u(32, 16);
    w.u(32, 235);
    auto p = parse(w);
    const auto& t = payload_as<ToneMappingInfo>(p.sei.messages[0]);
    CHECK(t.tone_map_id == 3);
    CHECK(t.coded_data_bit_depth == 10);
    CHECK(t.target_bit_depth == 8);
    CHECK(t.tone_map_model_id == 0);
    CHECK(t.values == std::vector<uint32_t>{16, 235});
    CHECK(diag_codes(p).empty());
  }
  {  // model 3 (piecewise linear): coded 10 bits -> u(16), target 8 bits -> u(8); two pivots
    BitWriter w;
    w.ue(0);
    w.u(1, 0);
    w.ue(1);
    w.u(8, 10);
    w.u(8, 8);
    w.ue(3);
    w.u(16, 2);
    w.u(16, 0);
    w.u(8, 0);
    w.u(16, 1023);
    w.u(8, 255);
    auto p = parse(w);
    const auto& t = payload_as<ToneMappingInfo>(p.sei.messages[0]);
    CHECK(t.repetition_period == 1);
    CHECK(t.values == std::vector<uint32_t>{0, 0, 1023, 255});
  }
  {  // model 2 (table): target depth 2 gives 4 entries of u(8) (coded depth 8)
    BitWriter w;
    w.ue(1);
    w.u(1, 0);
    w.ue(0);
    w.u(8, 8);
    w.u(8, 2);
    w.ue(2);
    w.u(8, 0);
    w.u(8, 64);
    w.u(8, 128);
    w.u(8, 192);
    auto p = parse(w);
    CHECK(payload_as<ToneMappingInfo>(p.sei.messages[0]).values == std::vector<uint32_t>{0, 64, 128, 192});
  }
  {  // model 4 (camera and exposure data, D.1.21): the three exposure_compensation_value fields are unconditional
    BitWriter w;
    w.ue(2);
    w.u(1, 0);
    w.ue(0);
    w.u(8, 8);
    w.u(8, 8);
    w.ue(4);
    w.u(8, 255);  // camera_iso_speed_idc: extended
    w.u(32, 3200);
    w.u(8, 5);  // exposure_index_idc
    w.u(1, 1);  // exposure_compensation_value_sign_flag
    w.u(16, 3);
    w.u(16, 4);
    w.u(32, 100);  // ref_screen_luminance_white
    w.u(32, 400);  // extended_range_white_level
    w.u(16, 16);
    w.u(16, 235);
    w.u(16, 254);
    auto p = parse(w);
    CHECK(p.sei.messages[0].parsed);
    CHECK(payload_as<ToneMappingInfo>(p.sei.messages[0]).tone_map_model_id == 4);
    CHECK(diag_codes(p).empty());
    const auto* n = find_node(p.tree.root(), "nominal_white_level_code_value");
    REQUIRE(n != nullptr);
    CHECK(n->uvalue == 235);
  }
  {  // cancel
    BitWriter w;
    w.ue(5);
    w.u(1, 1);
    auto p = parse(w);
    CHECK(payload_as<ToneMappingInfo>(p.sei.messages[0]).cancel_flag);
  }
  {  // target_bit_depth 17 is outside 1..16: the payload is reported, the framing goes on
    BitWriter w;
    w.ue(0);
    w.u(1, 0);
    w.ue(0);
    w.u(8, 8);
    w.u(8, 17);
    w.ue(2);
    auto p = parse(w);
    CHECK_FALSE(p.sei.messages[0].parsed);
    CHECK(diag_codes(p) == std::vector<std::string>{"SYN-H264-0002"});
  }
}

TEST_CASE("h264_sei_frame_packing_cancel_and_quincunx", "[syntax][h264][sei][fast]") {
  Env env;
  {
    BitWriter w;
    w.ue(4);
    w.u(1, 1);  // cancel: only the extension flag follows
    w.u(1, 0);
    auto p = parse_sei(sei_nal({{45, w.payload()}}), env.ctx);
    const auto& f = payload_as<FramePackingArrangement>(p.sei.messages[0]);
    CHECK(f.frame_packing_arrangement_id == 4);
    CHECK(f.cancel_flag);
    CHECK_FALSE(f.extension_flag);
    CHECK(diag_codes(p).empty());
  }
  {  // quincunx_sampling_flag 1 (and type 5) : no grid positions
    BitWriter w;
    w.ue(0);
    w.u(1, 0);
    w.u(7, 5);  // temporal interleaving: type_name "frame sequential"
    w.u(1, 1);  // quincunx
    w.u(6, 2);
    w.u(6, 0b101010);
    w.u(8, 0);
    w.ue(0);
    w.u(1, 0);
    auto p = parse_sei(sei_nal({{45, w.payload()}}), env.ctx);
    const auto& f = payload_as<FramePackingArrangement>(p.sei.messages[0]);
    CHECK(f.type == 5);
    CHECK(f.type_name == "frame sequential");
    CHECK(f.quincunx_sampling_flag);
    CHECK(f.content_interpretation_type == 2);
    CHECK(f.spatial_flipping_flag);
    CHECK_FALSE(f.frame0_flipped_flag);
    CHECK(f.field_views_flag);
    CHECK_FALSE(f.current_frame_is_frame0_flag);
    CHECK(f.frame0_self_contained_flag);
    CHECK_FALSE(f.frame1_self_contained_flag);
    CHECK(f.grid_position == std::array<uint32_t, 4>{0, 0, 0, 0});
    CHECK(diag_codes(p).empty());
  }
}

TEST_CASE("h264_sei_t35_and_unregistered_payload_family", "[syntax][h264][sei][fast]") {
  Env env;
  {  // tests/corpora/sei_payloads.json h264_ga94_cc: ATSC A/53 cc_data, two CEA-608 pairs
    auto p = parse_sei(sei_nal({{4, from_hex("b500314741393403c2fffcc849fd8080ff")}}), env.ctx);
    REQUIRE_FALSE(p.error.has_value());
    const auto& t = payload_as<UserDataRegisteredItuTT35>(p.sei.messages[0]);
    CHECK(t.itu_t_t35_country_code == 0xB5);
    CHECK(t.itu_t_t35_terminal_provider_code == 0x0031);
    CHECK(t.enum_name == "ATSC A/53 (GA94) CEA-708 captions");
    CHECK_FALSE(t.has_hdr10plus);
    CHECK(p.sei.messages[0].name == "user_data_registered_itu_t_t35");
    CHECK(find_node(p.tree.root(), "itu_t_t35_terminal_provider_code")->enum_name == t.enum_name);
    CHECK(diag_codes(p).empty());
  }
  {  // HDR10+ identification (country B5, provider 003C, oriented code 0001, application_identifier 4, version 1,
     // num_windows 1, targeted_system_display_maximum_luminance 0 ... 27 bits): only the identification is checked
    BitWriter w;
    w.u(8, 0xB5);
    w.u(16, 0x003C);
    w.u(16, 0x0001);
    w.u(8, 4);  // application_identifier
    w.u(8, 1);  // application_version
    w.u(2, 1);  // num_windows
    w.u(27, 4000);
    w.u(1, 0);  // targeted_system_display_actual_peak_luminance_flag
    for (int i = 0; i < 3; ++i) {
      w.u(17, 1000);  // maxscl
    }
    w.u(17, 500);  // average_maxrgb
    w.u(4, 0);     // num_distribution_maxrgb_percentiles
    w.u(10, 0);    // fraction_bright_pixels
    w.u(1, 0);     // mastering_display_actual_peak_luminance_flag
    w.u(1, 0);     // tone_mapping_flag
    w.u(1, 0);     // color_saturation_mapping_flag
    auto p = parse_sei(sei_nal({{4, w.payload()}}), env.ctx);
    REQUIRE_FALSE(p.error.has_value());
    REQUIRE(p.sei.messages[0].parsed);
    const auto& t = payload_as<UserDataRegisteredItuTT35>(p.sei.messages[0]);
    CHECK(t.itu_t_t35_terminal_provider_oriented_code == 0x0001);
    CHECK(t.has_hdr10plus);
    CHECK(t.enum_name == "HDR10+ (SMPTE ST 2094-40)");
    CHECK(diag_codes(p).empty());
  }
  {  // tests/corpora/sei_payloads.json h264_user_data_unregistered: an unknown UUID followed by "ovt"
    auto p = parse_sei(sei_nal({{5, from_hex("0d6f7674000000000000000053594e316f7674")}}), env.ctx);
    const auto& u = payload_as<SeiUserDataUnregistered>(p.sei.messages[0]);
    CHECK(u.uuid == "0D6F7674-0000-0000-0000-000053594E31");
    CHECK(u.enum_name == "user data (UUID 0D6F7674-0000-0000-0000-000053594E31)");
    CHECK(u.payload_byte_count == 3);
  }
  {  // fewer than 16 bytes: the payload fails, the message is reported, the loop goes on
    auto p = parse_sei(sei_nal({{5, {1, 2, 3}}, {6, {0xC4}}}), env.ctx);
    REQUIRE(p.sei.messages.size() == 2);
    CHECK_FALSE(p.sei.messages[0].parsed);
    CHECK(p.sei.messages[1].parsed);
    CHECK(diag_codes(p) == std::vector<std::string>{"BR-0001"});
  }
}

// ---- framing ------------------------------------------------------------------------------------------------------

TEST_CASE("h264_sei_payload_names", "[syntax][h264][sei][fast]") {
  CHECK(sei_payload_name(0) == "buffering_period");
  CHECK(sei_payload_name(3) == "filler_payload");
  CHECK(sei_payload_name(19) == "film_grain_characteristics");
  CHECK(sei_payload_name(23) == "tone_mapping_info");
  CHECK(sei_payload_name(30) == "scalable_nesting");
  CHECK(sei_payload_name(37) == "mvc_scalable_nesting");
  CHECK(sei_payload_name(45) == "frame_packing_arrangement");
  CHECK(sei_payload_name(47) == "display_orientation");
  CHECK(sei_payload_name(54) == "constrained_depth_parameter_set_identifier");
  CHECK(sei_payload_name(56) == "green_metadata");
  CHECK(sei_payload_name(137) == "mastering_display_colour_volume");
  CHECK(sei_payload_name(144) == "content_light_level_info");
  CHECK(sei_payload_name(147) == "alternative_transfer_characteristics");
  CHECK(sei_payload_name(55) == "reserved_sei_message");
  CHECK(sei_payload_name(57) == "reserved_sei_message");
  CHECK(sei_payload_name(200) == "reserved_sei_message");
}

TEST_CASE("h264_sei_variable_length_element_crossing_payload_end", "[syntax][h264][sei][fast]") {
  Env env;
  // recovery_frame_cnt is ue(v): 0x00 0x00 ... would run into the next message; payloadSize 1 makes it a BR-0001.
  auto p = parse_sei(sei_nal({{6, {0x00}}, {6, {0xC4}}}), env.ctx);
  REQUIRE(p.sei.messages.size() == 2);
  CHECK_FALSE(p.sei.messages[0].parsed);
  CHECK(p.sei.messages[1].parsed);
  CHECK(diag_codes(p) == std::vector<std::string>{"BR-0001"});
}

TEST_CASE("h264_sei_corpus_cvse3_sony_h", "[syntax][h264][sei][fast][needs-corpus]") {
  // Oracle: tests/golden/h264/jvt_CVSE3_Sony_H.trace_headers.json.gz (first 19 units): the SPS has a NAL and a VCL HRD
  // (cpb/dpb delay length 24, initial_cpb_removal_delay length 16) and pic_struct_present_flag 1. Unit 1 is a SEI NAL
  // with a buffering_period (initial_cpb_removal_delay 45000, offset 0 for the NAL then the VCL HRD) and a pic_timing
  // (cpb_removal_delay 0, dpb_output_delay 13, pic_struct 5, clock_timestamp_flag[0..2] 0); unit 4 has pic_timing
  // (3, 17, 6) and unit 7 (5, 23, 4).
  const auto path = ovt::test::corpus_or_skip("jvt_CVSE3_Sony_H");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  std::vector<std::vector<uint8_t>> sei_nals;
  std::vector<uint8_t> sps_nal;
  ovt::syntax::scan_annexb(*file, [&](const ovt::syntax::ScannedUnit& u) {
    if (u.bytes.empty()) {
      return;
    }
    const unsigned type = u.bytes[0] & 0x1FU;
    if (type == 7 && sps_nal.empty()) {
      sps_nal.assign(u.bytes.begin(), u.bytes.end());
    } else if (type == 6 && sei_nals.size() < 3) {
      sei_nals.emplace_back(u.bytes.begin(), u.bytes.end());
    }
  });
  REQUIRE_FALSE(sps_nal.empty());
  REQUIRE(sei_nals.size() == 3);
  Env env(sps_nal);
  auto first = parse_sei(sei_nals[0], env.ctx);
  REQUIRE(first.sei.messages.size() == 2);
  const auto& bp = payload_as<BufferingPeriod>(first.sei.messages[0]);
  CHECK(bp.nal_initial_cpb_removal_delay == std::vector<uint32_t>{45000});
  CHECK(bp.nal_initial_cpb_removal_delay_offset == std::vector<uint32_t>{0});
  CHECK(bp.vcl_initial_cpb_removal_delay == std::vector<uint32_t>{45000});
  CHECK(bp.vcl_initial_cpb_removal_delay_offset == std::vector<uint32_t>{0});
  CHECK(find_node(first.tree.root(), "initial_cpb_removal_delay", 0)->span.bit_offset == 25);
  const auto& pt0 = payload_as<PicTiming>(first.sei.messages[1]);
  CHECK(pt0.cpb_removal_delay == 0);
  CHECK(pt0.dpb_output_delay == 13);
  CHECK(pt0.pic_struct == 5);
  CHECK(pt0.NumClockTS == 3);
  CHECK(first.sei.messages[1].payload_span.bit_offset == 112);
  struct Expected {
    size_t nal;
    uint32_t cpb;
    uint32_t dpb;
    uint32_t pic_struct;
    uint32_t num_ts;
  };
  const std::array<Expected, 2> expected{{{1, 3, 17, 6, 3}, {2, 5, 23, 4, 2}}};
  for (const auto& e : expected) {
    auto p = parse_sei(sei_nals[e.nal], env.ctx);
    REQUIRE(p.sei.messages.size() == 1);
    const auto& pt = payload_as<PicTiming>(p.sei.messages[0]);
    CHECK(pt.cpb_removal_delay == e.cpb);
    CHECK(pt.dpb_output_delay == e.dpb);
    CHECK(pt.pic_struct == e.pic_struct);
    CHECK(pt.clock_timestamps.size() == e.num_ts);
    for (const auto& c : pt.clock_timestamps) {
      CHECK_FALSE(c.clock_timestamp_flag);
    }
    CHECK(diag_codes(p).empty());
  }
}
