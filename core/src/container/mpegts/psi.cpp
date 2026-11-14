// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MPEG-TS PSI/SI section reassembly and table decoding (chapter 08 §8.6.7, WI-4.205). Sources: ISO/IEC 13818-1
// §2.4.4 (section syntax, PAT, CAT, PMT, CRC_32 Annex A), ETSI EN 300 468 §5.2 (SDT, EIT, TDT, TOT) and
// ETSI TR 101 290 §5.2 (PAT_error_2, PMT_error_2, PID_error, CRC_error, CAT_error).
//
// SPEC-ASSUMPTION(#434): see psi.hpp.

#include "container/mpegts/psi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

#include "container/tree_fb.hpp"

namespace ovt::container::mpegts {

namespace {

constexpr size_t kSectionHeader = 3;  // table_id, section_syntax_indicator .. section_length
constexpr size_t kLongHeader = 8;     // + table_id_extension, version byte, section_number, last_section_number
constexpr size_t kCrcBytes = 4;
constexpr uint8_t kStuffing = 0xFF;
constexpr size_t kMaxTrackedSections = 65536;  // distinct (table, extension, section) keys remembered for repeats
constexpr uint8_t kTableTdt = 0x70;
constexpr uint8_t kTableTot = 0x73;

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

uint16_t be16(std::span<const uint8_t> b, size_t at) {
  return static_cast<uint16_t>((b[at] << 8U) | b[at + 1]);
}

bool is_eit(uint8_t table_id) {
  return table_id >= 0x4E && table_id <= 0x6F;
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

bool decode_sdt(Section& s, size_t end) {
  const std::span<const uint8_t> b = s.bytes;
  if (end < kLongHeader + 3) {
    return false;
  }
  s.original_network_id = be16(b, kLongHeader);
  size_t at = kLongHeader + 3;
  bool ok = true;
  while (ok && at + 5 <= end) {
    Service v;
    v.id = be16(b, at);
    v.eit_schedule = (b[at + 2] & 2U) != 0;
    v.eit_present_following = (b[at + 2] & 1U) != 0;
    v.running_status = b[at + 3] >> 5U;
    v.free_ca = (b[at + 3] & 0x10U) != 0;
    v.off = static_cast<uint32_t>(at);
    const size_t len = be16(b, at + 3) & 0x0FFFU;
    at += 5;
    ok = descriptor_loop(b, end, at, len, v.descriptors);
    s.services.push_back(std::move(v));
  }
  return ok && at == end;
}

bool decode_eit(Section& s, size_t end) {
  const std::span<const uint8_t> b = s.bytes;
  if (end < kLongHeader + 6) {
    return false;
  }
  s.transport_stream_id = be16(b, kLongHeader);
  s.original_network_id = be16(b, kLongHeader + 2);
  s.segment_last_section_number = b[kLongHeader + 4];
  s.last_table_id = b[kLongHeader + 5];
  size_t at = kLongHeader + 6;
  bool ok = true;
  while (ok && at + 12 <= end) {
    Event e;
    e.id = be16(b, at);
    for (size_t i = 0; i < 5; ++i) {
      e.start_time = (e.start_time << 8U) | b[at + 2 + i];
    }
    e.duration = (static_cast<uint32_t>(b[at + 7]) << 16U) | (static_cast<uint32_t>(b[at + 8]) << 8U) | b[at + 9];
    e.running_status = b[at + 10] >> 5U;
    e.free_ca = (b[at + 10] & 0x10U) != 0;
    e.off = static_cast<uint32_t>(at);
    const size_t len = be16(b, at + 10) & 0x0FFFU;
    at += 12;
    ok = descriptor_loop(b, end, at, len, e.descriptors);
    s.events.push_back(std::move(e));
  }
  return ok && at == end;
}

// TDT: 5 bytes of UTC_time after the 3-byte header. TOT: the same, then descriptors_loop_length and descriptors.
bool decode_time(Section& s) {
  const std::span<const uint8_t> b = s.bytes;
  const size_t end = s.has_crc ? s.bytes.size() - kCrcBytes : s.bytes.size();
  if (end < kSectionHeader + 5) {
    return false;
  }
  for (size_t i = 0; i < 5; ++i) {
    s.utc_time = (s.utc_time << 8U) | b[kSectionHeader + i];
  }
  if (s.table_id == kTableTdt) {
    return true;
  }
  if (end < kSectionHeader + 7) {
    return false;
  }
  size_t at = kSectionHeader + 7;
  return descriptor_loop(b, end, at, be16(b, kSectionHeader + 5) & 0x0FFFU, s.descriptors) && at == end;
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
  s.has_crc = s.syntax_indicator || s.table_id == kTableTot;
  return s;
}

size_t min_section_size(const Section& s) {
  if (s.syntax_indicator) {
    return kLongHeader + kCrcBytes;
  }
  return s.has_crc ? kSectionHeader + kCrcBytes : kSectionHeader;
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
  const size_t end = s.has_crc ? s.bytes.size() - kCrcBytes : s.bytes.size();
  if (s.table_id == 0x00) {
    s.decoded = decode_pat(s, end);
  } else if (s.table_id == 0x02) {
    s.decoded = decode_pmt(s, end);
  } else if (s.table_id == 0x01) {
    size_t at = kLongHeader;
    s.decoded = descriptor_loop(s.bytes, end, at, end - kLongHeader, s.descriptors);
  } else if (s.table_id == 0x42 || s.table_id == 0x46) {
    s.decoded = decode_sdt(s, end);
  } else if (is_eit(s.table_id)) {
    s.decoded = decode_eit(s, end);
  } else if (s.table_id == kTableTdt || s.table_id == kTableTot) {
    s.decoded = decode_time(s);
  }
}

const char* ext_name(uint8_t table_id) {
  switch (table_id) {
    case 0x00:
    case 0x42:
    case 0x46:
      return "transport_stream_id";
    case 0x02:
      return "program_number";
    case 0x40:
    case 0x41:
      return "network_id";
    case 0x4A:
      return "bouquet_id";
    default:
      return is_eit(table_id) ? "service_id" : "table_id_extension";
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
    case 0x40:
    case 0x41:
      return "NIT";
    case 0x42:
    case 0x46:
      return "SDT";
    case 0x4A:
      return "BAT";
    case kTableTdt:
      return "TDT";
    case kTableTot:
      return "TOT";
    default:
      return is_eit(table_id) ? "EIT" : "SEC";
  }
}

const char* stream_type_name(uint8_t stream_type) noexcept {
  switch (stream_type) {
    case 0x01:
      return "MPEG-1 video";
    case 0x02:
      return "MPEG-2 video";
    case 0x03:
      return "MPEG-1 audio";
    case 0x04:
      return "MPEG-2 audio";
    case 0x05:
      return "private sections";
    case 0x06:
      return "PES private data";
    case 0x0F:
      return "AAC audio (ADTS)";
    case 0x11:
      return "AAC audio (LATM)";
    case 0x15:
      return "metadata in PES";
    case 0x1B:
      return "AVC video";
    case 0x24:
      return "HEVC video";
    case 0x33:
      return "VVC video";
    case 0x81:
      return "AC-3 audio (ATSC)";
    case 0x87:
      return "E-AC-3 audio (ATSC)";
    default:
      return stream_type >= 0x80 ? "user private" : "other";
  }
}

std::string format_dvb_time(uint64_t mjd_bcd) noexcept {
  const auto bcd = [](uint32_t byte, int& out) {
    if ((byte >> 4U) > 9U || (byte & 15U) > 9U) {
      return false;
    }
    out = static_cast<int>((byte >> 4U) * 10U + (byte & 15U));
    return true;
  };
  int h = 0;
  int m = 0;
  int sec = 0;
  if (!bcd((mjd_bcd >> 16U) & 0xFFU, h) || !bcd((mjd_bcd >> 8U) & 0xFFU, m) || !bcd(mjd_bcd & 0xFFU, sec) || h > 23 ||
      m > 59 || sec > 59) {
    return "invalid";
  }
  const auto mjd = static_cast<double>((mjd_bcd >> 24U) & 0xFFFFU);  // EN 300 468 Annex C
  const int yp = static_cast<int>((mjd - 15078.2) / 365.25);
  const int mp = static_cast<int>((mjd - 14956.1 - std::floor(yp * 365.25)) / 30.6001);
  const int day = static_cast<int>(mjd) - 14956 - static_cast<int>(std::floor(yp * 365.25)) -
                  static_cast<int>(std::floor(mp * 30.6001));
  const int k = (mp == 14 || mp == 15) ? 1 : 0;
  const int year = 1900 + yp + k;
  const int month = mp - 1 - k * 12;
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return "invalid";
  }
  const auto two = [](int n) { return std::string(n < 10 ? "0" : "") + std::to_string(n); };
  return std::to_string(year) + "-" + two(month) + "-" + two(day) + " " + two(h) + ":" + two(m) + ":" + two(sec);
}

// ===================================================== parser =====================================================

PsiParser::PsiParser() : pmt_mask_(8192, 0) {}

bool PsiParser::wants(uint16_t pid) const noexcept {
  return pid == kPatPid || pid == kCatPid || pid == 0x0010 || pid == kSdtPid || pid == kEitPid || pid == kTimePid ||
         (pid < pmt_mask_.size() && pmt_mask_[pid] != 0);
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

// Counts a section that repeats the last one of its table (TDT and TOT change every second and are kept once);
// otherwise remembers it and counts a version change.
bool PsiParser::repeated(const Section& s) {
  const bool clock = s.table_id == kTableTdt || s.table_id == kTableTot;
  const Key key{s.pid, s.table_id, s.table_id_extension, s.section_number};
  const auto old = last_.find(key);
  if (old != last_.end() && (clock || old->second == s.bytes)) {
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
    table_error(s, "`table_id` `0x" + to_hex(std::span<const uint8_t>(&s.table_id, 1)) + "` not allowed");
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

// ====================================================== tree ======================================================

namespace {

using treefb::Fbb;
using FieldVec = std::vector<flatbuffers::Offset<fb::Field>>;

// Builds fields of one node; a field gets a BitSpan only when its bits lie inside `limit_bits` (the part of the
// section that is contiguous in the file).
class FieldSink {
 public:
  FieldSink(Fbb& fbb, uint64_t limit_bits) : fbb_(fbb), limit_(limit_bits) {}

  void u(std::string_view name, uint64_t value, uint64_t bit_offset, uint32_t bits) {
    if (bit_offset + bits <= limit_) {
      out_.push_back(treefb::uint_field(fbb_, name, value, bit_offset, bits));
    } else {
      out_.push_back(
          fb::CreateField(fbb_, fbb_.CreateString(name.data(), name.size()), fb::SyntaxValueKind::UInt, value));
    }
  }

  void s(std::string_view name, std::string_view text, uint64_t bit_offset, uint32_t bits) {
    if (bit_offset + bits <= limit_ && bits != 0) {
      out_.push_back(treefb::string_field(fbb_, name, text, bit_offset, bits));
    } else {
      out_.push_back(fb::CreateField(fbb_, fbb_.CreateString(name.data(), name.size()), fb::SyntaxValueKind::String, 0,
                                     0, fbb_.CreateString(text.data(), text.size())));
    }
  }

  FieldVec& fields() { return out_; }

 private:
  Fbb& fbb_;
  uint64_t limit_;
  FieldVec out_;
};

struct NodeCtx {
  Fbb& fbb;
  const Section& sec;
};

// The part of [off, off + length) of the section that lies in the first packet, where the bytes are contiguous in
// the file; the continuation packets of a long section are not part of any node span.
uint32_t clipped(const Section& s, uint32_t off, uint32_t length) {
  return off >= s.first_chunk ? 0 : std::min(length, s.first_chunk - off);
}

flatbuffers::Offset<fb::BoxNode> make_node(NodeCtx& c, std::string_view type, uint32_t off, uint32_t length,
                                           const FieldVec& fields,
                                           const std::vector<flatbuffers::Offset<fb::BoxNode>>& kids,
                                           std::string_view spec, std::string_view description) {
  const fb::ByteSpan span(c.sec.file_offset + std::min(off, c.sec.first_chunk), clipped(c.sec, off, length));
  return fb::CreateBoxNode(c.fbb, c.fbb.CreateString(type.data(), type.size()), 0, &span, 0, -1, -1,
                           c.fbb.CreateVector(fields), nullptr, c.fbb.CreateVector(kids),
                           c.fbb.CreateString(spec.data(), spec.size()),
                           c.fbb.CreateString(description.data(), description.size()));
}

flatbuffers::Offset<fb::BoxNode> descriptor_node(NodeCtx& c, const Descriptor& d) {
  const DescriptorInfo* info = find_descriptor(d.tag);
  const auto length = static_cast<uint32_t>(2 + d.body.size());
  FieldSink f(c.fbb, uint64_t{clipped(c.sec, d.off, length)} * 8U);
  f.u("descriptor_tag", d.tag, 0, 8);
  f.u("descriptor_length", d.body.size(), 8, 8);
  const std::string_view name = info != nullptr ? info->name : "unknown_descriptor";
  for (const DescField& v : decode_descriptor(d.tag, d.body)) {
    if (v.is_text) {
      f.s(v.name, v.text, v.bit_offset, v.bit_length);
    } else {
      f.u(v.name, v.value, v.bit_offset, v.bit_length);
    }
  }
  return make_node(c, "DESC", d.off, length, f.fields(), {}, info != nullptr ? info->spec_ref : "", name);
}

std::vector<flatbuffers::Offset<fb::BoxNode>> descriptor_nodes(NodeCtx& c, const std::vector<Descriptor>& list) {
  std::vector<flatbuffers::Offset<fb::BoxNode>> out;
  out.reserve(list.size());
  for (const Descriptor& d : list) {
    out.push_back(descriptor_node(c, d));
  }
  return out;
}

using NodeVec = std::vector<flatbuffers::Offset<fb::BoxNode>>;

NodeVec pat_nodes(NodeCtx& c, const Section& s) {
  NodeVec kids;
  for (const PatEntry& e : s.pat) {
    FieldSink pf(c.fbb, uint64_t{clipped(s, e.off, 4)} * 8U);
    pf.u("program_number", e.program_number, 0, 16);
    pf.u(e.program_number == 0 ? "network_PID" : "program_map_PID", e.pid, 19, 13);
    kids.push_back(make_node(c, "PROG", e.off, 4, pf.fields(), {}, "ISO/IEC 13818-1 §2.4.4.3", "program"));
  }
  return kids;
}

NodeVec pmt_nodes(NodeCtx& c, const Section& s, FieldSink& f) {
  f.u("PCR_PID", s.pmt.pcr_pid, 67, 13);
  f.u("program_info_length", be16(s.bytes, 10) & 0x0FFFU, 84, 12);
  NodeVec kids = descriptor_nodes(c, s.pmt.descriptors);
  for (const EsInfo& es : s.pmt.streams) {
    FieldSink ef(c.fbb, uint64_t{clipped(s, es.off, 5)} * 8U);
    ef.u("stream_type", es.stream_type, 0, 8);
    ef.s("stream_type_name", stream_type_name(es.stream_type), 0, 0);
    ef.u("elementary_PID", es.pid, 11, 13);
    kids.push_back(make_node(c, "ES", es.off, 5, ef.fields(), descriptor_nodes(c, es.descriptors),
                             "ISO/IEC 13818-1 §2.4.4.8", "elementary stream"));
  }
  return kids;
}

NodeVec sdt_nodes(NodeCtx& c, const Section& s, FieldSink& f) {
  f.u("original_network_id", s.original_network_id, 64, 16);
  NodeVec kids;
  for (const Service& v : s.services) {
    FieldSink sf(c.fbb, uint64_t{clipped(s, v.off, 5)} * 8U);
    sf.u("service_id", v.id, 0, 16);
    sf.u("EIT_schedule_flag", v.eit_schedule ? 1U : 0U, 22, 1);
    sf.u("EIT_present_following_flag", v.eit_present_following ? 1U : 0U, 23, 1);
    sf.u("running_status", v.running_status, 24, 3);
    sf.u("free_CA_mode", v.free_ca ? 1U : 0U, 27, 1);
    kids.push_back(make_node(c, "SVC", v.off, 5, sf.fields(), descriptor_nodes(c, v.descriptors),
                             "ETSI EN 300 468 §5.2.3", "service"));
  }
  return kids;
}

NodeVec eit_nodes(NodeCtx& c, const Section& s, FieldSink& f) {
  f.u("transport_stream_id", s.transport_stream_id, 64, 16);
  f.u("original_network_id", s.original_network_id, 80, 16);
  f.u("segment_last_section_number", s.segment_last_section_number, 96, 8);
  f.u("last_table_id", s.last_table_id, 104, 8);
  NodeVec kids;
  for (const Event& e : s.events) {
    FieldSink ef(c.fbb, uint64_t{clipped(s, e.off, 12)} * 8U);
    ef.u("event_id", e.id, 0, 16);
    ef.u("start_time", e.start_time, 16, 40);
    ef.s("start_time_utc", format_dvb_time(e.start_time), 0, 0);
    ef.u("duration", e.duration, 56, 24);
    ef.u("running_status", e.running_status, 80, 3);
    ef.u("free_CA_mode", e.free_ca ? 1U : 0U, 83, 1);
    kids.push_back(make_node(c, "EVNT", e.off, 12, ef.fields(), descriptor_nodes(c, e.descriptors),
                             "ETSI EN 300 468 §5.2.4", "event"));
  }
  return kids;
}

// The fields every section starts with (and ends with: CRC_32) and the table specific children.
flatbuffers::Offset<fb::BoxNode> section_node(Fbb& fbb, const Section& s) {
  NodeCtx c{fbb, s};
  FieldSink f(fbb, uint64_t{s.first_chunk} * 8U);
  const size_t size = s.bytes.size();
  f.u("table_id", s.table_id, 0, 8);
  f.u("section_syntax_indicator", s.syntax_indicator ? 1U : 0U, 8, 1);
  f.u("section_length", size - kSectionHeader, 12, 12);
  if (s.syntax_indicator) {
    f.u(ext_name(s.table_id), s.table_id_extension, 24, 16);
    f.u("version_number", s.version, 42, 5);
    f.u("current_next_indicator", s.current_next ? 1U : 0U, 47, 1);
    f.u("section_number", s.section_number, 48, 8);
    f.u("last_section_number", s.last_section_number, 56, 8);
  }
  if (s.has_crc) {
    f.u("CRC_32",
        (uint64_t{s.bytes[size - 4]} << 24U) | (uint64_t{s.bytes[size - 3]} << 16U) |
            (uint64_t{s.bytes[size - 2]} << 8U) | s.bytes[size - 1],
        (size - 4) * 8U, 32);
    f.u("CRC_ok", s.crc_ok ? 1U : 0U, 0, 0);
  }
  NodeVec kids;
  if (s.decoded) {
    if (s.table_id == 0x00) {
      kids = pat_nodes(c, s);
    } else if (s.table_id == 0x02) {
      kids = pmt_nodes(c, s, f);
    } else if (s.table_id == 0x42 || s.table_id == 0x46) {
      kids = sdt_nodes(c, s, f);
    } else if (is_eit(s.table_id)) {
      kids = eit_nodes(c, s, f);
    } else {
      if (s.table_id == kTableTdt || s.table_id == kTableTot) {
        f.u("UTC_time", s.utc_time, 24, 40);
        f.s("UTC_time_text", format_dvb_time(s.utc_time), 0, 0);
      }
      kids = descriptor_nodes(c, s.descriptors);  // CAT, TOT
    }
  }
  return make_node(c, table_name(s.table_id), 0, static_cast<uint32_t>(size), f.fields(), kids,
                   "ISO/IEC 13818-1 §2.4.4", "PID " + hex_pid(s.pid) + " section");
}

}  // namespace

flatbuffers::Offset<fb::BoxNode> build_psi_node(flatbuffers::FlatBufferBuilder& fbb, const PsiTables& tables,
                                                uint64_t file_size) {
  std::vector<flatbuffers::Offset<fb::BoxNode>> kids;
  kids.reserve(tables.sections.size());
  for (const Section& s : tables.sections) {
    kids.push_back(section_node(fbb, s));
  }
  const auto plain = [&fbb](const char* name, uint64_t value) {
    return fb::CreateField(fbb, fbb.CreateString(name), fb::SyntaxValueKind::UInt, value);
  };
  const FieldVec fields = {
      plain("section_count", tables.section_count),       plain("repeated_sections", tables.repeated_sections),
      plain("version_changes", tables.version_changes),   plain("crc_errors", tables.crc_errors),
      plain("dropped_sections", tables.dropped_sections), plain("program_count", tables.programs.size())};
  const fb::ByteSpan span(0, file_size);
  return fb::CreateBoxNode(fbb, fbb.CreateString("PSIT"), 0, &span, 0, -1, -1, fbb.CreateVector(fields), nullptr,
                           fbb.CreateVector(kids), fbb.CreateString("ISO/IEC 13818-1 §2.4.4"),
                           fbb.CreateString("PSI/SI tables"));
}

}  // namespace ovt::container::mpegts
