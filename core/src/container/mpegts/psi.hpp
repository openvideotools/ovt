// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MPEG-TS PSI/SI tables (chapter 08 §8.6.7, WI-4.205): section reassembly per PID (ISO/IEC 13818-1 §2.4.4 with
// `pointer_field`), CRC-32 check, PAT / PMT / CAT decoding into a program map, and read-only DVB SDT / EIT / TDT /
// TOT decoding (ETSI EN 300 468 §5.2). A table whose `version_number` or content changes is recorded again and
// replaces the table in the program map; identical repetitions are only counted.
//
// SPEC-ASSUMPTION(#434): the tree layout (second root `PSIT` with one node per distinct section), the PID_error
// rule (an ES PID of the final program map without any packet in the file) and the CAT_error rule (scrambled
// packets outside PID 1 but no valid CAT section) are the answers proposed in that issue.

#ifndef OVT_CONTAINER_MPEGTS_PSI_HPP
#define OVT_CONTAINER_MPEGTS_PSI_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "container/diagnostics.h"
#include "container/mpegts/descriptors.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::mpegts {

inline constexpr uint16_t kPatPid = 0x0000;
inline constexpr uint16_t kCatPid = 0x0001;
inline constexpr uint16_t kSdtPid = 0x0011;
inline constexpr uint16_t kEitPid = 0x0012;
inline constexpr uint16_t kTimePid = 0x0014;  // TDT and TOT
inline constexpr size_t kMaxSections = 4096;  // recorded sections per file; the counters stay exact

// CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final XOR). The CRC of a section that
// includes its own CRC_32 field is 0.
[[nodiscard]] uint32_t crc32_mpeg2(std::span<const uint8_t> bytes) noexcept;

// Name of a table by PID and table_id ("PAT", "PMT", "CAT", "SDT", "BAT", "NIT", "EIT", "TDT", "TOT", "SEC").
[[nodiscard]] const char* table_name(uint8_t table_id) noexcept;

// ISO/IEC 13818-1 Table 2-34 stream_type names ("AVC video", "HEVC video", ...); "reserved" or "user private".
[[nodiscard]] const char* stream_type_name(uint8_t stream_type) noexcept;

// "YYYY-MM-DD hh:mm:ss" for the 40-bit MJD + BCD time of EN 300 468 Annex C; "invalid" for a malformed value.
[[nodiscard]] std::string format_dvb_time(uint64_t mjd_bcd);

struct Descriptor {
  uint8_t tag = 0;
  uint32_t off = 0;  // byte offset of the tag byte inside the section
  std::vector<uint8_t> body;
};

struct PatEntry {
  uint16_t program_number = 0;
  uint16_t pid = 0;  // PMT PID, or the network PID for program 0
  uint32_t off = 0;
};

struct EsInfo {
  uint8_t stream_type = 0;
  uint16_t pid = 0;
  uint32_t off = 0;
  std::vector<Descriptor> descriptors;
};

struct Program {
  uint16_t number = 0;
  uint16_t pmt_pid = 0;
  uint16_t pcr_pid = 0x1FFF;
  uint8_t version = 0;
  bool has_pmt = false;
  std::vector<Descriptor> descriptors;  // program_info
  std::vector<EsInfo> streams;
};

struct Service {  // SDT
  uint16_t id = 0;
  bool eit_schedule = false;
  bool eit_present_following = false;
  uint8_t running_status = 0;
  bool free_ca = false;
  uint32_t off = 0;
  std::vector<Descriptor> descriptors;
};

struct Event {  // EIT
  uint16_t id = 0;
  uint64_t start_time = 0;  // 40 bits, MJD + BCD
  uint32_t duration = 0;    // 24 bits, BCD hhmmss
  uint8_t running_status = 0;
  bool free_ca = false;
  uint32_t off = 0;
  std::vector<Descriptor> descriptors;
};

// One distinct section (first occurrence of its content). `bytes` holds the whole section including the CRC.
struct Section {
  uint16_t pid = 0;
  uint8_t table_id = 0;
  bool syntax_indicator = false;
  uint16_t table_id_extension = 0;
  uint8_t version = 0;
  bool current_next = true;
  uint8_t section_number = 0;
  uint8_t last_section_number = 0;
  bool has_crc = false;
  bool crc_ok = true;
  bool decoded = false;  // the body was interpreted (a bad CRC or a short section leaves it raw)
  uint64_t packet_index = 0;
  uint64_t file_offset = 0;    // file offset of the table_id byte
  uint32_t first_chunk = 0;    // bytes of the section inside its first packet
  std::vector<uint8_t> bytes;  // includes CRC_32
  // Decoded body, by table.
  std::vector<PatEntry> pat;
  Program pmt;
  std::vector<Descriptor> descriptors;  // CAT, TOT
  std::vector<Service> services;
  std::vector<Event> events;
  uint16_t original_network_id = 0;  // SDT, EIT
  uint16_t transport_stream_id = 0;  // EIT
  uint8_t segment_last_section_number = 0;
  uint8_t last_table_id = 0;
  uint64_t utc_time = 0;  // TDT, TOT; 40 bits
};

struct PsiDiagnostic {
  DiagCode code;
  std::string message;
  uint64_t offset = 0;
};

struct PsiTables {
  std::vector<Program> programs;  // the current program map, in PAT order
  uint16_t network_pid = 0x0010;
  bool has_pat = false;
  std::vector<Descriptor> cat_descriptors;  // CA_descriptors of the current CAT
  bool has_cat = false;
  std::vector<Section> sections;  // distinct sections in order of first appearance (at most kMaxSections)
  uint64_t section_count = 0;     // every complete section, repeats included
  uint64_t repeated_sections = 0;
  uint64_t version_changes = 0;
  uint64_t crc_errors = 0;
  uint64_t dropped_sections = 0;  // distinct sections beyond kMaxSections
  std::vector<PsiDiagnostic> diagnostics;
};

// Section reassembly and table decoding for the PSI/SI PIDs. Feed every packet in file order; `wants(pid)` says
// which ones are of interest.
class PsiParser {
 public:
  PsiParser();

  [[nodiscard]] bool wants(uint16_t pid) const noexcept;

  // `payload` is the packet payload; `payload_offset` its file offset.
  void feed(uint16_t pid, bool payload_unit_start, bool scrambled, std::span<const uint8_t> payload,
            uint64_t packet_index, uint64_t payload_offset);

  // A continuity error on `pid` abandons a partly collected section.
  void discontinuity(uint16_t pid);

  // End of file: PID_error and CAT_error checks. `packets_of(pid)` is the packet count of a PID;
  // `scrambled_elsewhere` the number of scrambled packets on PIDs other than 1.
  void finish(const std::function<uint64_t(uint16_t)>& packets_of, uint64_t scrambled_elsewhere);

  [[nodiscard]] PsiTables& tables() noexcept { return tables_; }

 private:
  struct Pending {
    std::vector<uint8_t> buf;
    bool active = false;
    uint64_t packet_index = 0;
    uint64_t file_offset = 0;
    uint32_t first_chunk = 0;
  };
  using Key = std::tuple<uint16_t, uint8_t, uint16_t, uint8_t>;  // pid, table_id, extension, section_number

  PsiTables tables_;
  std::map<uint16_t, Pending> pending_;
  std::map<Key, std::vector<uint8_t>> last_;  // last content per table section
  std::map<uint8_t, std::vector<PatEntry>> pat_sections_;
  uint8_t pat_version_ = 0xFF;
  std::vector<uint8_t> pmt_mask_;   // PIDs that the PAT names as PMT PIDs
  std::vector<uint16_t> bad_pids_;  // PIDs that already reported a scrambled PSI packet

  void take(uint16_t pid, std::span<const uint8_t> section, uint64_t packet_index, uint64_t file_offset,
            uint32_t first_chunk);
  void append(Pending& p, std::span<const uint8_t> data, uint16_t pid);
  void table_error(const Section& s, const std::string& what);
  [[nodiscard]] bool wrong_table(const Section& s) const;
  [[nodiscard]] bool repeated(const Section& s);
  void apply(const Section& s);
  void rebuild_programs();
  void diag(DiagCode code, std::string message, uint64_t offset);
};

// Nodes of the section tree (root `PSIT`, one child per recorded section), appended to `fbb`.
[[nodiscard]] flatbuffers::Offset<fb::BoxNode> build_psi_node(flatbuffers::FlatBufferBuilder& fbb,
                                                              const PsiTables& tables, uint64_t file_size);

}  // namespace ovt::container::mpegts

#endif  // OVT_CONTAINER_MPEGTS_PSI_HPP
