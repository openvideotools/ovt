// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The MPEG-TS packet layer (chapter 08 §8.6.7, WI-4.204): packet size detection (188 / 192 / 204), sync and resync,
// header and adaptation-field decoding (ISO/IEC 13818-1 §2.4.3), per-PID statistics and the lazy packet tree.
// PSI, PES and the timing table are the work items that follow (WI-4.205 to WI-4.207).
//
// SPEC-ASSUMPTION(#337): resync rule, node names, continuity-counter rule and the M2TS prefix are not fixed by
// §8.6.7; the proposed answers are in the issue and in the comments below.

#ifndef OVT_CONTAINER_MPEGTS_TS_PACKETS_HPP
#define OVT_CONTAINER_MPEGTS_TS_PACKETS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::mpegts {

inline constexpr uint8_t kSyncByte = 0x47;
inline constexpr uint32_t kPacketBytes = 188;  // the transport packet proper, from the sync byte
inline constexpr uint32_t kM2tsPrefix = 4;     // TP_extra_header of a 192-byte packet (phase 4)
inline constexpr size_t kChildWindow = 65536;  // packet nodes materialised per expand (§8.6.7)
inline constexpr uint16_t kNullPid = 0x1FFF;
inline constexpr size_t kPidCount = 8192;
inline constexpr size_t kMaxDiagnostics = 10000;  // the container diagnostics list is capped; counts stay exact

// The decoded adaptation field (13818-1 §2.4.3.4). Fields past the bytes that `adaptation_field_length` leaves are
// zero and `truncated` is set.
struct AdaptationField {
  uint8_t length = 0;  // adaptation_field_length as written
  bool discontinuity_indicator = false;
  bool random_access_indicator = false;
  bool elementary_stream_priority_indicator = false;
  bool pcr_flag = false;
  bool opcr_flag = false;
  bool splicing_point_flag = false;
  bool transport_private_data_flag = false;
  bool adaptation_field_extension_flag = false;
  uint64_t pcr_base = 0;       // 33 bits, 90 kHz
  uint16_t pcr_extension = 0;  // 9 bits, 27 MHz remainder
  uint64_t opcr_base = 0;
  uint16_t opcr_extension = 0;
  int8_t splice_countdown = 0;
  uint8_t transport_private_data_length = 0;
  bool truncated = false;

  // PCR in 27 MHz units: base * 300 + extension.
  [[nodiscard]] uint64_t pcr() const noexcept { return pcr_base * 300U + pcr_extension; }
  [[nodiscard]] uint64_t opcr() const noexcept { return opcr_base * 300U + opcr_extension; }
};

struct PacketHeader {
  bool transport_error_indicator = false;
  bool payload_unit_start_indicator = false;
  bool transport_priority = false;
  uint16_t pid = 0;
  uint8_t transport_scrambling_control = 0;
  uint8_t adaptation_field_control = 0;  // 1 payload, 2 adaptation field, 3 both, 0 reserved
  uint8_t continuity_counter = 0;
  std::optional<AdaptationField> adaptation;  // present when adaptation_field_control has bit 1
  uint32_t payload_offset = 4;                // from the sync byte; payload_size 0 when there is none
  uint32_t payload_size = 0;

  [[nodiscard]] bool has_payload() const noexcept { return (adaptation_field_control & 1U) != 0; }
};

// Decodes a packet that starts at its sync byte. At most 188 bytes of `packet` are looked at; nullopt when it
// is shorter than the 4-byte header or the first byte is not 0x47. Never reads past `packet`.
[[nodiscard]] std::optional<PacketHeader> decode_packet(std::span<const uint8_t> packet) noexcept;

// Packet layout of the file: sync bytes every `stride` bytes; `prefix` is 4 for the M2TS timestamp in front of
// each packet (stride 192, first sync at phase 4), otherwise 0.
struct Geometry {
  uint32_t stride = kPacketBytes;
  uint32_t prefix = 0;
  uint64_t first_sync = 0;  // file offset of the first sync byte
};

// Looks for the first position in the first 64 KiB where up to five sync bytes follow each other at stride 188,
// 192 or 204 (in that order on ties); nullopt when there is none.
[[nodiscard]] std::optional<Geometry> detect_geometry(std::span<const uint8_t> head, uint64_t file_size);

struct PidStats {
  uint16_t pid = 0;
  uint64_t packets = 0;
  uint64_t payload_packets = 0;
  uint64_t scrambled_packets = 0;  // transport_scrambling_control != 0
  uint64_t transport_errors = 0;   // transport_error_indicator set
  uint64_t continuity_errors = 0;
  uint64_t first_packet = 0;  // packet index of the first packet of the PID
};

// A stretch of consecutive packets on the stride grid; packet indices count only packets with a sync byte.
struct Run {
  uint64_t first_packet = 0;
  uint64_t sync_offset = 0;  // file offset of the sync byte of the first packet
  uint64_t count = 0;
};

struct Diagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;
};

struct File {
  Geometry geometry;
  uint64_t file_size = 0;
  uint64_t packet_count = 0;
  uint64_t sync_loss_events = 0;  // CNT-TS-1001
  uint64_t sync_byte_errors = 0;  // CNT-TS-1002
  std::vector<Run> runs;
  std::vector<PidStats> pids;  // PIDs with at least one packet, ascending
  std::vector<Diagnostic> diagnostics;

  // File offset of the first byte of packet `index` (the prefix of an M2TS packet included), index < packet_count.
  [[nodiscard]] uint64_t packet_offset(uint64_t index) const noexcept;
  [[nodiscard]] uint64_t sync_offset(uint64_t index) const noexcept;
};

// Walks the whole file. nullopt when no sync pattern is found (not a transport stream). Never throws except for an
// I/O failure of `file` (io::IoError).
[[nodiscard]] std::optional<File> parse(const io::RandomAccessFile& file);

// nullptr when `file` is not a transport stream.
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file);

}  // namespace ovt::container::mpegts

#endif  // OVT_CONTAINER_MPEGTS_TS_PACKETS_HPP
