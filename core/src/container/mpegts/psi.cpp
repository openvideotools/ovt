// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MPEG-TS PSI section reassembly and table decoding (chapter 08 §8.6.7, WI-4.205a). Sources: ISO/IEC 13818-1
// §2.4.4 (section syntax, PAT, CAT, PMT, CRC_32 Annex A) and ETSI TR 101 290 §5.2 (PAT_error_2, PMT_error_2,
// PID_error, CRC_error, CAT_error).
//
// SPEC-ASSUMPTION(#434): see psi.hpp.

#include "container/mpegts/psi.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace ovt::container::mpegts {

namespace {

constexpr size_t kSectionHeader = 3;  // table_id, section_syntax_indicator .. section_length
constexpr size_t kLongHeader = 8;     // + table_id_extension, version byte, section_number, last_section_number
constexpr size_t kCrcBytes = 4;
constexpr uint8_t kStuffing = 0xFF;
constexpr size_t kMaxTrackedSections = 65536;  // distinct (table, extension, section) keys remembered for repeats

constexpr std::array<uint32_t, 256> make_crc_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i << 24U;
    for (int k = 0; k < 8; ++k) {
      c = (c & 0x80000000U) != 0 ? (c << 1U) ^ 0x04C11DB7U : c << 1U;
    }
    t[i] = c;
  }
  return t;
}
constexpr std::array<uint32_t, 256> kCrcTable = make_crc_table();

std::string hex_pid(uint16_t pid) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string s = "0x0000";
  for (size_t i = 0; i < 4; ++i) {
    s[5 - i] = digits[(pid >> (4U * i)) & 15U];
  }
  return s;
}

std::string hex_byte(uint8_t b) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  return {digits[b >> 4U], digits[b & 15U]};
}

uint16_t be16(std::span<const uint8_t> b, size_t at) {
  return static_cast<uint16_t>((b[at] << 8U) | b[at + 1]);
}

// Reads descriptors from `data` (the bytes of one loop, starting at byte `base` of the section). False when the
// loop ends in the middle of a descriptor; the complete ones before it are kept.
bool parse_descriptors(std::span<const uint8_t> data, uint32_t base, std::vector<Descriptor>& out) {
  size_t pos = 0;
  while (pos + 2 <= data.size()) {
    const size_t len = data[pos + 1];
    if (pos + 2 + len > data.size()) {
      return false;
    }
    Descriptor d;
    d.tag = data[pos];
    d.off = static_cast<uint32_t>(base + pos);
    d.body.assign(data.begin() + static_cast<std::ptrdiff_t>(pos + 2),
                  data.begin() + static_cast<std::ptrdiff_t>(pos + 2 + len));
    out.push_back(std::move(d));
    pos += 2 + len;
  }
  return pos == data.size();
}

// Descriptor loop of `length` bytes at `at`; advances `at`. False when it does not fit in `body`.
bool descriptor_loop(std::span<const uint8_t> b, size_t end, size_t& at, size_t length, std::vector<Descriptor>& out) {
  if (at + length > end) {
    return false;
  }
  const bool ok = parse_descriptors(b.subspan(at, length), static_cast<uint32_t>(at), out);
  at += length;
  return ok;
}

bool decode_pat(Section& s, size_t end) {
  for (size_t at = kLongHeader; at + 4 <= end; at += 4) {
    s.pat.push_back(
        {be16(s.bytes, at), static_cast<uint16_t>(be16(s.bytes, at + 2) & 0x1FFFU), static_cast<uint32_t>(at)});
  }
  return (end - kLongHeader) % 4 == 0;
}

bool decode_pmt(Section& s, size_t end) {
  const std::span<const uint8_t> b = s.bytes;
  s.pmt.number = s.table_id_extension;
  s.pmt.version = s.version;
  s.pmt.has_pmt = true;
  if (end < kLongHeader + 4) {
    return false;
  }
  s.pmt.pcr_pid = be16(b, kLongHeader) & 0x1FFFU;
  size_t at = kLongHeader + 4;
  bool ok = descriptor_loop(b, end, at, be16(b, kLongHeader + 2) & 0x0FFFU, s.pmt.descriptors);
  while (ok && at + 5 <= end) {
    EsInfo es;
    es.stream_type = b[at];
    es.pid = be16(b, at + 1) & 0x1FFFU;
    es.off = static_cast<uint32_t>(at);
    const size_t info = be16(b, at + 3) & 0x0FFFU;
    at += 5;
    ok = descriptor_loop(b, end, at, info, es.descriptors);
    s.pmt.streams.push_back(std::move(es));
  }
  return ok && at == end;
}

Section make_section(uint16_t pid, std::span<const uint8_t> raw, uint64_t packet_index, uint64_t file_offset,
                     uint32_t first_chunk) {
  Section s;
  s.pid = pid;
  s.table_id = raw[0];
  s.syntax_indicator = (raw[1] & 0x80U) != 0;
  s.packet_index = packet_index;
  s.file_offset = file_offset;
  s.first_chunk = std::min<uint32_t>(first_chunk, static_cast<uint32_t>(raw.size()));
  s.bytes.assign(raw.begin(), raw.end());
  s.has_crc = s.syntax_indicator;
  return s;
}

size_t min_section_size(const Section& s) {
  return s.syntax_indicator ? kLongHeader + kCrcBytes : kSectionHeader;
}

void read_long_header(Section& s) {
  if (!s.syntax_indicator) {
    return;
  }
  s.table_id_extension = be16(s.bytes, 3);
  s.version = (s.bytes[5] >> 1U) & 0x1FU;
  s.current_next = (s.bytes[5] & 1U) != 0;
  s.section_number = s.bytes[6];
  s.last_section_number = s.bytes[7];
}

// Interprets the body of a section whose CRC is good; sets `decoded`.
void decode_section(Section& s) {
  const size_t end = s.bytes.size() - kCrcBytes;
  if (s.table_id == 0x00) {
    s.decoded = decode_pat(s, end);
  } else if (s.table_id == 0x02) {
    s.decoded = decode_pmt(s, end);
  } else if (s.table_id == 0x01) {
    size_t at = kLongHeader;
    s.decoded = descriptor_loop(s.bytes, end, at, end - kLongHeader, s.descriptors);
  }
}

}  // namespace

uint32_t crc32_mpeg2(std::span<const uint8_t> bytes) noexcept {
  uint32_t crc = 0xFFFFFFFFU;
  for (const uint8_t b : bytes) {
    crc = (crc << 8U) ^ kCrcTable[((crc >> 24U) ^ b) & 0xFFU];
  }
  return crc;
}

const char* table_name(uint8_t table_id) noexcept {
  switch (table_id) {
    case 0x00:
      return "PAT";
    case 0x01:
      return "CAT";
    case 0x02:
      return "PMT";
    default:
      return "SEC";
  }
}

// ===================================================== parser =====================================================

PsiParser::PsiParser() : pmt_mask_(8192, 0) {}

bool PsiParser::wants(uint16_t pid) const noexcept {
  return pid == kPatPid || pid == kCatPid || (pid < pmt_mask_.size() && pmt_mask_[pid] != 0);
}

void PsiParser::diag(DiagCode code, std::string message, uint64_t offset) {
  if (tables_.diagnostics.size() < 10000) {
    tables_.diagnostics.push_back({code, std::move(message), offset});
  }
}

void PsiParser::discontinuity(uint16_t pid) {
  const auto it = pending_.find(pid);
  if (it != pending_.end()) {
    it->second.active = false;
  }
}

void PsiParser::feed(uint16_t pid, bool payload_unit_start, bool scrambled, std::span<const uint8_t> payload,
                     uint64_t packet_index, uint64_t payload_offset) {
  if (scrambled) {  // a scrambled PSI payload cannot be read (TR 101 290 1.3, 1.5)
    discontinuity(pid);
    const bool pat = pid == kPatPid;
    const bool pmt = pid < pmt_mask_.size() && pmt_mask_[pid] != 0;
    if ((pat || pmt) && std::find(bad_pids_.begin(), bad_pids_.end(), pid) == bad_pids_.end()) {
      bad_pids_.push_back(pid);
      diag(pat ? DiagCode::CNT_TS_1003 : DiagCode::CNT_TS_1005,
           std::string(pat ? "PAT_error_2" : "PMT_error_2") + ": scrambled packet on PID `" + hex_pid(pid) +
               "` at packet `" + std::to_string(packet_index) + "`",
           payload_offset);
    }
    return;
  }
  if (payload.empty()) {
    return;
  }
  Pending& p = pending_[pid];
  if (!payload_unit_start) {
    if (p.active) {
      append(p, payload, pid);
    }
    return;
  }
  const size_t pointer = payload[0];
  if (1 + pointer > payload.size()) {  // pointer_field beyond the packet: nothing can be located
    p.active = false;
    return;
  }
  if (p.active) {  // the tail of the section that began in an earlier packet
    append(p, payload.subspan(1, pointer), pid);
    p.active = false;
  }
  size_t pos = 1 + pointer;
  while (pos < payload.size() && payload[pos] != kStuffing) {
    const size_t avail = payload.size() - pos;
    const size_t need = avail >= kSectionHeader ? kSectionHeader + (be16(payload, pos + 1) & 0x0FFFU) : 0;
    if (need != 0 && need <= avail) {  // a whole section inside this packet; more may follow
      take(pid, payload.subspan(pos, need), packet_index, payload_offset + pos, static_cast<uint32_t>(need));
      pos += need;
      continue;
    }
    p.buf.assign(payload.begin() + static_cast<std::ptrdiff_t>(pos), payload.end());
    p.active = true;
    p.packet_index = packet_index;
    p.file_offset = payload_offset + pos;
    p.first_chunk = static_cast<uint32_t>(avail);
    break;
  }
}

void PsiParser::append(Pending& p, std::span<const uint8_t> data, uint16_t pid) {
  p.buf.insert(p.buf.end(), data.begin(), data.end());
  if (p.buf.size() < kSectionHeader) {
    return;
  }
  const size_t need = kSectionHeader + (be16(p.buf, 1) & 0x0FFFU);
  if (p.buf.size() >= need) {
    take(pid, std::span<const uint8_t>(p.buf).first(need), p.packet_index, p.file_offset, p.first_chunk);
    p.active = false;
  }
}

void PsiParser::table_error(const Section& s, const std::string& what) {
  const bool pat = s.pid == kPatPid;
  const bool pmt = pmt_mask_[s.pid] != 0;
  if (!pat && !pmt && s.pid != kCatPid) {
    return;
  }
  const char* name = pat ? "PAT_error_2" : (pmt ? "PMT_error_2" : "CAT_error");
  diag(pat ? DiagCode::CNT_TS_1003 : (pmt ? DiagCode::CNT_TS_1005 : DiagCode::CNT_TS_2007),
       std::string(name) + ": " + what + " on PID `" + hex_pid(s.pid) + "` at packet `" +
           std::to_string(s.packet_index) + "`",
       s.file_offset);
}

// A section that the PID must not carry (TR 101 290 1.3, 1.5, 2.6).
bool PsiParser::wrong_table(const Section& s) const {
  return (s.pid == kPatPid && s.table_id != 0x00) || (pmt_mask_[s.pid] != 0 && s.table_id != 0x02) ||
         (s.pid == kCatPid && s.table_id != 0x01);
}

// Counts a section that repeats the last one of its table; otherwise remembers it and counts a version change.
bool PsiParser::repeated(const Section& s) {
  const Key key{s.pid, s.table_id, s.table_id_extension, s.section_number};
  const auto old = last_.find(key);
  if (old != last_.end() && old->second == s.bytes) {
    ++tables_.repeated_sections;
    return true;
  }
  if (old != last_.end() && s.syntax_indicator && old->second.size() > 5 &&
      ((old->second[5] >> 1U) & 0x1FU) != s.version) {
    ++tables_.version_changes;
  }
  if (old != last_.end() || last_.size() < kMaxTrackedSections) {
    last_[key] = s.bytes;
  }
  return false;
}

void PsiParser::take(uint16_t pid, std::span<const uint8_t> section, uint64_t packet_index, uint64_t file_offset,
                     uint32_t first_chunk) {
  ++tables_.section_count;
  Section s = make_section(pid, section, packet_index, file_offset, first_chunk);
  const bool wrong = wrong_table(s);
  if (wrong) {
    table_error(s, "`table_id` `0x" + hex_byte(s.table_id) + "` not allowed");
  }
  if (s.bytes.size() < min_section_size(s)) {
    table_error(s, "section shorter than its header");
    return;
  }
  read_long_header(s);
  if (s.has_crc && crc32_mpeg2(s.bytes) != 0) {
    s.crc_ok = false;
    ++tables_.crc_errors;
    diag(DiagCode::CNT_TS_2002,
         "CRC_error in `" + std::string(table_name(s.table_id)) + "` section on PID `" + hex_pid(pid) +
             "` at packet `" + std::to_string(packet_index) + "`",
         file_offset);
  }
  if (repeated(s)) {
    return;
  }
  if (s.crc_ok && !wrong) {
    decode_section(s);
    if (!s.decoded && s.table_id <= 0x02) {
      table_error(s, "malformed `" + std::string(table_name(s.table_id)) + "` section");
    }
    if (s.decoded && s.current_next) {
      apply(s);
    }
  }
  if (tables_.sections.size() < kMaxSections) {
    tables_.sections.push_back(std::move(s));
  } else {
    ++tables_.dropped_sections;
  }
}

void PsiParser::apply(const Section& s) {
  if (s.table_id == 0x00 && s.pid == kPatPid) {
    if (!tables_.has_pat || s.version != pat_version_) {  // a new version replaces the whole PAT
      pat_sections_.clear();
      pat_version_ = s.version;
    }
    tables_.has_pat = true;
    pat_sections_[s.section_number] = s.pat;
    rebuild_programs();
  } else if (s.table_id == 0x02 && pmt_mask_[s.pid] != 0) {
    for (Program& p : tables_.programs) {
      if (p.number == s.table_id_extension && p.pmt_pid == s.pid) {
        const uint16_t pmt_pid = p.pmt_pid;
        p = s.pmt;
        p.pmt_pid = pmt_pid;
      }
    }
  } else if (s.table_id == 0x01 && s.pid == kCatPid) {
    tables_.has_cat = true;
    tables_.cat_descriptors = s.descriptors;
  }
}

void PsiParser::rebuild_programs() {
  std::vector<Program> next;
  std::fill(pmt_mask_.begin(), pmt_mask_.end(), uint8_t{0});
  for (const auto& [number, entries] : pat_sections_) {
    for (const PatEntry& e : entries) {
      if (e.program_number == 0) {
        tables_.network_pid = e.pid;
        continue;
      }
      pmt_mask_[e.pid] = 1;
      const auto same = std::find_if(tables_.programs.begin(), tables_.programs.end(), [&e](const Program& p) {
        return p.number == e.program_number && p.pmt_pid == e.pid;
      });
      if (same != tables_.programs.end()) {
        next.push_back(*same);  // the PMT stays valid while the PAT entry does
      } else {
        Program p;
        p.number = e.program_number;
        p.pmt_pid = e.pid;
        next.push_back(std::move(p));
      }
    }
  }
  tables_.programs = std::move(next);
}

void PsiParser::finish(const std::function<uint64_t(uint16_t)>& packets_of, uint64_t scrambled_elsewhere) {
  std::vector<uint16_t> seen;
  for (const Program& p : tables_.programs) {
    for (const EsInfo& es : p.streams) {
      if (packets_of(es.pid) == 0 && std::find(seen.begin(), seen.end(), es.pid) == seen.end()) {
        seen.push_back(es.pid);
        diag(DiagCode::CNT_TS_1007,
             "PID_error: PID `" + hex_pid(es.pid) + "` referenced by the PMT of program `" + std::to_string(p.number) +
                 "` but no packets in the file",
             0);
      }
    }
  }
  if (scrambled_elsewhere > 0 && !tables_.has_cat) {
    diag(DiagCode::CNT_TS_2007,
         "CAT_error: `" + std::to_string(scrambled_elsewhere) + "` scrambled packets but no CAT (PID 1)", 0);
  }
}

}  // namespace ovt::container::mpegts
