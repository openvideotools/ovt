// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.110a: H.264 SEI framing, buffering_period and pic_timing (chapter 7 §7.3.4 rows 7–9 and 18). WI-1.110b adds the
// other payload tests to this file.
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
