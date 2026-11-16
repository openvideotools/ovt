// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PES header decoding and PES reassembly of the MPEG-TS reader (chapter 08 §8.6.7, WI-4.206a). Source of truth:
// ISO/IEC 13818-1 §2.4.3.6 (PES packet) and §2.4.3.7 (semantics). One PES packet becomes one sample of the PID's
// SampleTable: `offset` is the file offset of its first payload byte, `size` the reassembled payload size.
//
// SPEC-ASSUMPTION(#569): reassembly starts at the first payload_unit_start_indicator of the PID, also before the PMT
// that names it (as libavformat, which reads the PMT first and then demuxes from the start), and only PIDs that
// some PMT names as elementary streams are kept; a
// PES cut by a continuity error, a transport error or an early next PES keeps its bytes and is marked incomplete;
// one duplicate packet (same continuity_counter) is dropped from the payload.

#ifndef OVT_CONTAINER_MPEGTS_PES_HPP
#define OVT_CONTAINER_MPEGTS_PES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ovt::container::mpegts {

inline constexpr uint64_t kTimestampWrap = uint64_t{1} << 33U;  // PTS / DTS / PCR base modulus (90 kHz)
inline constexpr uint64_t kPcrWrap = kTimestampWrap * 300U;     // PCR modulus in 27 MHz units
inline constexpr std::array<uint8_t, 3> kPesStartCode = {0x00, 0x00, 0x01};
inline constexpr size_t kMaxPesHeader = 9U + 255U;  // fixed part plus the largest header_data_length

struct PesHeader {
  uint8_t stream_id = 0;
  uint16_t packet_length = 0;  // PES_packet_length; 0 = unbounded (legal for video in a transport stream)
  bool has_optional_header = false;
  uint8_t scrambling_control = 0;
  bool data_alignment_indicator = false;
  uint8_t pts_dts_flags = 0;
  bool escr_flag = false;
  uint8_t header_data_length = 0;
  int64_t pts = -1;  // 33 bits, 90 kHz; -1 absent
  int64_t dts = -1;
  int64_t escr_base = -1;  // 33 bits, 90 kHz; -1 absent
  uint16_t escr_extension = 0;
  bool syntax_error = false;    // a marker bit, the '10' prefix or PTS_DTS_flags '01' is wrong, or a field overruns
  uint32_t payload_offset = 6;  // bytes from the packet_start_code_prefix to the first payload byte
};

enum class PesParse : uint8_t { Ok, NeedMore, NotPes };

// Decodes the PES header at the start of `bytes`. NotPes when the bytes do not start with 00 00 01; NeedMore when
// they are too short for the header they announce. Never reads past `bytes`.
[[nodiscard]] PesParse decode_pes_header(std::span<const uint8_t> bytes, PesHeader& out) noexcept;

struct PesPacket {
  uint64_t first_packet = 0;  // transport packet index of the packet with payload_unit_start_indicator
  uint64_t last_packet = 0;   // transport packet index of the last packet that contributed payload bytes
  uint64_t offset = 0;        // file offset of the first payload byte (after the PES header)
  uint32_t size = 0;          // reassembled payload bytes
  int64_t pts = -1;           // 90 kHz, as written (33 bits); -1 absent
  int64_t dts = -1;
  uint8_t stream_id = 0;
  bool random_access = false;  // random_access_indicator of the first packet
  bool complete = true;        // false when cut by a continuity / transport error, the next PES or the end of file
};

struct PesStream {
  uint16_t pid = 0;
  uint8_t stream_type = 0;
  std::vector<PesPacket> packets;
};

// Collects PES packets. Feed every payload packet of the candidate PIDs in file order; a PID whose unit starts are
// not PES start codes produces nothing.
class PesAssembler {
 public:
  PesAssembler();

  // `payload` is the packet payload, `payload_offset` its file offset. Returns the PES that this packet started
  // once its header is decoded (possibly in a later packet), else nullptr; the pointer is valid until the next call.
  const PesPacket* feed(uint16_t pid, bool payload_unit_start, bool random_access, uint8_t continuity_counter,
                        std::span<const uint8_t> payload, uint64_t packet_index, uint64_t payload_offset);

  // A continuity or transport error on `pid`: the PES being collected is incomplete.
  void damage(uint16_t pid);

  // End of file. Streams that produced at least one PES, ascending PID.
  [[nodiscard]] std::vector<PesStream> finish();

 private:
  struct State {
    PesStream stream;
    bool active = false;     // inside a PES
    bool in_header = false;  // header bytes still being collected
    bool bounded = false;    // PES_packet_length != 0
    bool dup_ok = false;     // the last payload packet may be repeated once
    uint8_t last_cc = 0;
    uint64_t expected = 0;  // payload bytes announced by PES_packet_length
    std::vector<uint8_t> header;
    PesPacket cur;
  };
  std::vector<State> states_;  // indexed by PID

  static void close(State& s, bool cut);
  static const PesPacket* take_header(State& s, std::span<const uint8_t> payload, uint64_t payload_offset,
                                      uint64_t packet_index);
  static void take_payload(State& s, std::span<const uint8_t> bytes, uint64_t packet_index);
};

}  // namespace ovt::container::mpegts

#endif  // OVT_CONTAINER_MPEGTS_PES_HPP
