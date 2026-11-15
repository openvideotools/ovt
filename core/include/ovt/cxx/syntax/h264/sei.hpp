// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 SEI messages (Annex D): sei_rbsp() framing and the payload parsers of chapter 7 §7.3.4 rows 7–9 and 18
// (WI-1.110a: buffering_period, pic_timing, every other payload kept as named Bytes; WI-1.110b adds rows 10–17 and the
// film grain / tone mapping payloads). C++ helper header (D22): semi-public, no ABI promise, never included from a C
// header.
//
// The reader stands after the NAL header (parse_nal_header()). Every message becomes a node `sei_message[i]`
// whose child is the payload node named after the payload (`buffering_period`, `pic_timing`, …, or
// `reserved_sei_message` for an unlisted type); payload types without a parser keep their bytes under the
// Annex D name (Bytes). A payload that fails to parse is reported as an Error Diagnostic on its message and the
// next message is still parsed; a payloadSize beyond the RBSP stops the loop (ParseError BR-0001, one Diagnostic).

#ifndef OVT_CXX_SYNTAX_H264_SEI_HPP
#define OVT_CXX_SYNTAX_H264_SEI_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

/// buffering_period() (D.1.2, type 0).
struct BufferingPeriod {
  uint32_t seq_parameter_set_id = 0;
  std::vector<uint32_t> nal_initial_cpb_removal_delay;  // per SchedSelIdx; empty without NAL HRD
  std::vector<uint32_t> nal_initial_cpb_removal_delay_offset;
  std::vector<uint32_t> vcl_initial_cpb_removal_delay;  // empty without VCL HRD
  std::vector<uint32_t> vcl_initial_cpb_removal_delay_offset;
};

/// One clock timestamp of pic_timing() (D.1.3).
struct ClockTimestamp {
  bool clock_timestamp_flag = false;
  uint32_t ct_type = 0;
  bool nuit_field_based_flag = false;
  uint32_t counting_type = 0;
  bool full_timestamp_flag = false;
  bool discontinuity_flag = false;
  bool cnt_dropped_flag = false;
  uint32_t n_frames = 0;
  bool seconds_flag = false;
  bool minutes_flag = false;
  bool hours_flag = false;
  uint32_t seconds_value = 0;
  uint32_t minutes_value = 0;
  uint32_t hours_value = 0;
  int32_t time_offset = 0;
  std::string text;  // "hh:mm:ss:ff" (D.2.3 fields; a field that was not sent is 0)
};

/// pic_timing() (D.1.3, type 1).
struct PicTiming {
  bool sps_known = false;  // false: no SPS was available, only the framing was recorded (Diagnostic SYN-H264-0031)
  bool cpb_dpb_delays_present = false;
  uint32_t cpb_removal_delay = 0;
  uint32_t dpb_output_delay = 0;
  bool pic_struct_present = false;
  uint32_t pic_struct = 0;
  std::string_view pic_struct_name;  // Table D-1
  uint32_t NumClockTS = 0;
  std::vector<ClockTimestamp> clock_timestamps;
};

/// The parsed form of one message; std::monostate when the payload is kept as bytes.
using SeiPayload = std::variant<std::monostate, BufferingPeriod, PicTiming>;

struct SeiMessage {
  uint32_t payload_type = 0;
  uint32_t payload_size = 0;
  std::string_view name;  // Annex D name (Table of 0..56) or "reserved_sei_message"
  BitSpan payload_span{0, 0};
  bool parsed = false;  // false: kept as Bytes (no parser, SPS missing) or the payload failed to parse
  SeiPayload payload;
};

/// The parameter sets the SEI parsers look at (D.2.2): buffering_period() names its SPS by id, pic_timing()
/// uses the SPS of the previous picture unless a buffering_period() of the same access unit names one.
struct SeiContext {
  std::span<const Sps* const> sps_by_id;  // indexed by seq_parameter_set_id (0..31); null entries are unknown
  const Sps* active_sps = nullptr;        // SPS of the previous picture
  /// Set by parse_sei_rbsp() from a buffering_period() of this access unit; the caller clears it at the next one.
  std::optional<uint32_t> buffering_period_sps_id;
};

struct SeiRbsp {
  std::vector<SeiMessage> messages;
  bool complete = true;  // false when the framing stopped early (payloadSize beyond the RBSP)
  BitSpan span{0, 0};
};

/// Table of Annex D payload names for payloadType 0..56 (+ 137, 144, 147); "reserved_sei_message" otherwise.
[[nodiscard]] std::string_view sei_payload_name(uint32_t payload_type) noexcept;

/// sei_rbsp() (§7.3.2.3). Node `sei_rbsp`. Only a failure to read the framing itself stops the loop; it is not
/// thrown but recorded (SeiRbsp::complete, Diagnostic on the message).
SeiRbsp parse_sei_rbsp(BitReader& br, SyntaxBuilder& b, SeiContext& ctx);

}  // namespace ovt::syntax::h264

#endif  // OVT_CXX_SYNTAX_H264_SEI_HPP
