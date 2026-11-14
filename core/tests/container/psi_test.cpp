// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the MPEG-TS PSI/SI layer (chapter 08 §8.6.7, WI-4.205): section reassembly, CRC-32, PAT / PMT / CAT
// decoding, version replacement, the descriptor registry and the DVB SDT / EIT / TDT / TOT tables.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <pugixml.hpp>
#include <string>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "container/mpegts/descriptors.hpp"
#include "container/mpegts/psi.hpp"
#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

namespace ts = ovt::container::mpegts;
using ovt::container::DiagCode;
using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, unsigned v) {
  b.push_back(static_cast<uint8_t>(v >> 8U));
  b.push_back(static_cast<uint8_t>(v & 0xFFU));
}

// A long-form section: header, `body`, CRC_32 (corrupted when `bad_crc`).
Bytes section(uint8_t table_id, unsigned ext, unsigned version, const Bytes& body, uint8_t number = 0, uint8_t last = 0,
              bool bad_crc = false, bool current = true) {
  Bytes s{table_id};
  put16(s, 0xB000U | static_cast<unsigned>(5 + body.size() + 4));
  put16(s, ext);
  s.push_back(static_cast<uint8_t>(0xC0U | (version << 1U) | (current ? 1U : 0U)));
  s.push_back(number);
  s.push_back(last);
  s.insert(s.end(), body.begin(), body.end());
  const uint32_t crc = ts::crc32_mpeg2(s);
  put16(s, crc >> 16U);
  put16(s, crc & 0xFFFFU);
  if (bad_crc) {
    s.back() ^= 0x55;
  }
  return s;
}

Bytes pat_body(const std::vector<std::pair<unsigned, unsigned>>& programs) {
  Bytes b;
  for (const auto& [number, pid] : programs) {
    put16(b, number);
    put16(b, 0xE000U | pid);
  }
  return b;
}

Bytes descriptor(uint8_t tag, const Bytes& body) {
  Bytes d{tag, static_cast<uint8_t>(body.size())};
  d.insert(d.end(), body.begin(), body.end());
  return d;
}

struct Es {
  uint8_t type;
  unsigned pid;
  Bytes descriptors;
};

Bytes pmt_body(unsigned pcr_pid, const Bytes& program_info, const std::vector<Es>& streams) {
  Bytes b;
  put16(b, 0xE000U | pcr_pid);
  put16(b, 0xF000U | static_cast<unsigned>(program_info.size()));
  b.insert(b.end(), program_info.begin(), program_info.end());
  for (const Es& es : streams) {
    b.push_back(es.type);
    put16(b, 0xE000U | es.pid);
    put16(b, 0xF000U | static_cast<unsigned>(es.descriptors.size()));
    b.insert(b.end(), es.descriptors.begin(), es.descriptors.end());
  }
  return b;
}

// Packets carrying `payload` (sections back to back) on `pid`: pointer_field 0 on the first, 0xFF stuffing.
// `cc` is advanced.
Bytes packetize(unsigned pid, const Bytes& payload, uint8_t& cc, bool scrambled = false) {
  Bytes out;
  size_t pos = 0;
  bool first = true;
  while (first || pos < payload.size()) {
    Bytes p{ts::kSyncByte, static_cast<uint8_t>((first ? 0x40U : 0U) | (pid >> 8U)), static_cast<uint8_t>(pid & 0xFFU),
            static_cast<uint8_t>((scrambled ? 0x80U : 0U) | 0x10U | (cc++ & 15U))};
    if (first) {
      p.push_back(0);  // pointer_field
    }
    const size_t room = 188 - p.size();
    const size_t take = std::min(room, payload.size() - pos);
    p.insert(p.end(), payload.begin() + static_cast<std::ptrdiff_t>(pos),
             payload.begin() + static_cast<std::ptrdiff_t>(pos + take));
    p.resize(188, 0xFF);
    pos += take;
    first = false;
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

// A stream of filler packets on PID 0x100 (never 0x47-free by construction: payload 0xFF).
Bytes filler(unsigned pid, unsigned count, uint8_t& cc) {
  Bytes out;
  for (unsigned i = 0; i < count; ++i) {
    const Bytes p = packetize(pid, Bytes{0x01, 0x02}, cc);
    Bytes q = p;
    q[1] = static_cast<uint8_t>(q[1] & 0xBFU);  // no payload_unit_start: plain data
    q[4] = 0xFF;
    out.insert(out.end(), q.begin(), q.end());
  }
  return out;
}

void append(Bytes& a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
}

std::optional<ts::File> parse_bytes(const Bytes& data) {
  const ovt::io::MemoryFile file(std::span<const uint8_t>(data.data(), data.size()));
  return ts::parse(file);
}

size_t count_code(const ts::File& f, DiagCode code) {
  return static_cast<size_t>(std::count_if(f.diagnostics.begin(), f.diagnostics.end(),
                                           [code](const ts::Diagnostic& d) { return d.code == code; }));
}

const ts::Program* find_program(const ts::File& f, unsigned number) {
  for (const auto& p : f.psi.programs) {
    if (p.number == number) {
      return &p;
    }
  }
  return nullptr;
}

std::vector<ts::DescField> dec(uint8_t tag, const Bytes& body) {
  return ts::decode_descriptor(tag, body);
}

const ts::DescField* field(const std::vector<ts::DescField>& v, const std::string& name) {
  for (const auto& f : v) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

// One program (1) with PMT PID 0x100 and two streams, a PAT and a PMT, then filler on the streams.
Bytes basic_stream() {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  uint8_t cc2 = 0;
  Bytes data;
  append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
  const Bytes avc = descriptor(0x28, {0x64, 0x00, 0x28, 0x3F});
  const Bytes lang = descriptor(0x0A, {'e', 'n', 'g', 0x00});
  append(data, packetize(0x100,
                         section(0x02, 1, 0,
                                 pmt_body(0x101, descriptor(0x05, {'H', 'D', 'M', 'V'}),
                                          {{0x1B, 0x101, avc}, {0x0F, 0x102, lang}})),
                         cc1));
  append(data, filler(0x101, 2, cc2));
  uint8_t cc3 = 0;
  append(data, filler(0x102, 2, cc3));
  return data;
}

}  // namespace

TEST_CASE("crc32_mpeg2: check value and zero over a whole section", "[container][mpegts][psi][fast]") {
  const std::string check = "123456789";
  CHECK(ts::crc32_mpeg2(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(check.data()), check.size())) ==
        0x0376E6E7U);
  const Bytes s = section(0x00, 1, 0, pat_body({{1, 0x100}}));
  CHECK(ts::crc32_mpeg2(s) == 0);
}

TEST_CASE("psi: PAT and PMT build the program map", "[container][mpegts][psi][fast]") {
  const auto f = parse_bytes(basic_stream());
  REQUIRE(f);
  CHECK(f->diagnostics.empty());
  REQUIRE(f->psi.has_pat);
  REQUIRE(f->psi.programs.size() == 1);
  const ts::Program& p = f->psi.programs[0];
  CHECK(p.number == 1);
  CHECK(p.pmt_pid == 0x100);
  CHECK(p.has_pmt);
  CHECK(p.pcr_pid == 0x101);
  REQUIRE(p.descriptors.size() == 1);
  CHECK(p.descriptors[0].tag == 0x05);
  REQUIRE(p.streams.size() == 2);
  CHECK(p.streams[0].stream_type == 0x1B);
  CHECK(p.streams[0].pid == 0x101);
  CHECK(p.streams[1].stream_type == 0x0F);
  CHECK(p.streams[1].pid == 0x102);
  CHECK(std::string(ts::stream_type_name(0x1B)) == "AVC video");
  // PAT, PMT: two distinct sections.
  CHECK(f->psi.sections.size() == 2);
  CHECK(f->psi.sections[0].table_id == 0x00);
  CHECK(f->psi.sections[0].table_id_extension == 7);
  CHECK(f->psi.sections[1].table_id == 0x02);
}

TEST_CASE("psi: a section that spans packets and several sections in one packet", "[container][mpegts][psi][fast]") {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  uint8_t cc2 = 0;
  Bytes data;
  // A PAT in two sections (section_number 0 and 1) back to back in one packet.
  Bytes both = section(0x00, 7, 3, pat_body({{1, 0x100}}), 0, 1);
  append(both, section(0x00, 7, 3, pat_body({{2, 0x200}}), 1, 1));
  append(data, packetize(0, both, cc0));
  // A PMT of 300 bytes of descriptors: two packets.
  Bytes big;
  for (unsigned i = 0; i < 20; ++i) {
    append(big, descriptor(0x0E, {0xC0, 0x00, static_cast<uint8_t>(i)}));  // maximum_bitrate, 5 bytes each
    append(big, descriptor(0xB0, Bytes(7, static_cast<uint8_t>(i))));
  }
  REQUIRE(big.size() > 188);
  append(data, packetize(0x100, section(0x02, 1, 0, pmt_body(0x101, big, {{0x24, 0x101, {}}})), cc1));
  append(data, packetize(0x200, section(0x02, 2, 0, pmt_body(0x201, {}, {{0x02, 0x201, {}}})), cc2));
  uint8_t cc3 = 0;
  append(data, filler(0x101, 1, cc3));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(count_code(*f, DiagCode::CNT_TS_2002) == 0);
  REQUIRE(f->psi.programs.size() == 2);
  const ts::Program* p1 = find_program(*f, 1);
  const ts::Program* p2 = find_program(*f, 2);
  REQUIRE((p1 != nullptr && p2 != nullptr));
  CHECK(p1->descriptors.size() == 40);
  REQUIRE(p1->streams.size() == 1);
  CHECK(p1->streams[0].stream_type == 0x24);
  CHECK(p2->pmt_pid == 0x200);
  CHECK(p2->streams[0].stream_type == 0x02);
}

TEST_CASE("psi: a corrupted CRC reports CNT-TS-2002 and the table is not applied", "[container][mpegts][psi][fast]") {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  Bytes data;
  append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
  append(data, packetize(0x100, section(0x02, 1, 0, pmt_body(0x101, {}, {{0x1B, 0x101, {}}}), 0, 0, true), cc1));
  uint8_t cc2 = 0;
  append(data, filler(0x101, 2, cc2));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(count_code(*f, DiagCode::CNT_TS_2002) == 1);
  CHECK(f->psi.crc_errors == 1);
  const ts::Program* p = find_program(*f, 1);
  REQUIRE(p != nullptr);
  CHECK_FALSE(p->has_pmt);
  CHECK(p->streams.empty());
  REQUIRE(f->psi.sections.size() == 2);
  CHECK_FALSE(f->psi.sections[1].crc_ok);
  CHECK_FALSE(f->psi.sections[1].decoded);
}

TEST_CASE("psi: a new version replaces the table, repeats are counted", "[container][mpegts][psi][fast]") {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  uint8_t cc2 = 0;
  Bytes data;
  const Bytes pat = section(0x00, 7, 0, pat_body({{1, 0x100}}));
  const Bytes pmt0 = section(0x02, 1, 0, pmt_body(0x101, {}, {{0x1B, 0x101, {}}}));
  const Bytes pmt1 = section(0x02, 1, 1, pmt_body(0x101, {}, {{0x24, 0x101, {}}, {0x0F, 0x102, {}}}));
  append(data, packetize(0, pat, cc0));
  append(data, packetize(0x100, pmt0, cc1));
  append(data, packetize(0, pat, cc0));
  append(data, packetize(0x100, pmt0, cc1));
  append(data, packetize(0x100, pmt1, cc1));
  append(data, filler(0x101, 1, cc2));
  uint8_t cc3 = 0;
  append(data, filler(0x102, 1, cc3));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->psi.section_count == 5);
  CHECK(f->psi.repeated_sections == 2);
  CHECK(f->psi.version_changes == 1);
  CHECK(f->psi.sections.size() == 3);
  const ts::Program* p = find_program(*f, 1);
  REQUIRE(p != nullptr);
  CHECK(p->version == 1);
  REQUIRE(p->streams.size() == 2);
  CHECK(p->streams[0].stream_type == 0x24);
}

TEST_CASE("psi: a section with current_next_indicator 0 is recorded but not applied",
          "[container][mpegts][psi][fast]") {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  Bytes data;
  append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
  append(data,
         packetize(0x100, section(0x02, 1, 4, pmt_body(0x101, {}, {{0x1B, 0x101, {}}}), 0, 0, false, false), cc1));
  uint8_t cc2 = 0;
  append(data, filler(0x101, 2, cc2));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  REQUIRE(f->psi.sections.size() == 2);
  CHECK_FALSE(f->psi.sections[1].current_next);
  const ts::Program* p = find_program(*f, 1);
  REQUIRE(p != nullptr);
  CHECK_FALSE(p->has_pmt);
}

TEST_CASE("psi: PAT_error_2 and PMT_error_2", "[container][mpegts][psi][fast]") {
  SECTION("wrong table_id on PID 0, then a scrambled PMT") {
    uint8_t cc0 = 0;
    uint8_t cc1 = 0;
    Bytes data;
    append(data, packetize(0, section(0x42, 7, 0, {0, 0, 0}), cc0));  // an SDT on the PAT PID
    append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
    append(data, packetize(0x100, section(0x02, 1, 0, pmt_body(0x101, {}, {})), cc1, true));
    uint8_t cc2 = 0;
    append(data, filler(0x101, 1, cc2));
    const auto f = parse_bytes(data);
    REQUIRE(f);
    CHECK(count_code(*f, DiagCode::CNT_TS_1003) == 1);
    CHECK(count_code(*f, DiagCode::CNT_TS_1005) == 1);
  }
  SECTION("a PMT PID that carries another table") {
    uint8_t cc0 = 0;
    uint8_t cc1 = 0;
    Bytes data;
    append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
    append(data, packetize(0x100, section(0x01, 0, 0, {}), cc1));
    uint8_t cc2 = 0;
    append(data, filler(0x101, 1, cc2));
    const auto f = parse_bytes(data);
    REQUIRE(f);
    CHECK(count_code(*f, DiagCode::CNT_TS_1005) == 1);
  }
}

TEST_CASE("psi: PID_error for a PMT stream without packets", "[container][mpegts][psi][fast]") {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  uint8_t cc2 = 0;
  Bytes data;
  append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
  append(data, packetize(0x100, section(0x02, 1, 0, pmt_body(0x101, {}, {{0x1B, 0x101, {}}, {0x0F, 0x102, {}}})), cc1));
  append(data, filler(0x101, 2, cc2));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(count_code(*f, DiagCode::CNT_TS_1007) == 1);
}

TEST_CASE("psi: CAT_error when scrambled packets have no CAT", "[container][mpegts][psi][fast]") {
  const auto build = [](bool with_cat) {
    uint8_t cc0 = 0;
    uint8_t cc1 = 0;
    uint8_t cc2 = 0;
    uint8_t cc3 = 0;
    Bytes data;
    append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
    append(data, packetize(0x100, section(0x02, 1, 0, pmt_body(0x101, {}, {{0x1B, 0x101, {}}})), cc1));
    if (with_cat) {
      append(data, packetize(1, section(0x01, 0, 0, descriptor(0x09, {0x06, 0x04, 0xE1, 0x23, 0xAA})), cc3));
    }
    append(data, packetize(0x101, Bytes{1, 2, 3}, cc2, true));
    append(data, filler(0x101, 1, cc2));
    return data;
  };
  const auto without = parse_bytes(build(false));
  REQUIRE(without);
  CHECK(count_code(*without, DiagCode::CNT_TS_2007) == 1);
  const auto with = parse_bytes(build(true));
  REQUIRE(with);
  CHECK(count_code(*with, DiagCode::CNT_TS_2007) == 0);
  REQUIRE(with->psi.cat_descriptors.size() == 1);
  const auto fields = dec(0x09, with->psi.cat_descriptors[0].body);
  REQUIRE(field(fields, "CA_system_ID") != nullptr);
  CHECK(field(fields, "CA_system_ID")->value == 0x0604);
  CHECK(field(fields, "CA_PID")->value == 0x0123);
}

TEST_CASE("psi: DVB SDT, EIT, TDT and TOT are decoded read-only", "[container][mpegts][psi][fast]") {
  uint8_t cc0 = 0;
  uint8_t cc1 = 0;
  uint8_t cc2 = 0;
  uint8_t cc3 = 0;
  Bytes data;
  append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
  // SDT actual: original_network_id, one service named "News" by "Prov".
  Bytes svc{0x64, 0x04, 'P', 'r', 'o', 'v', 0x04, 'N', 'e', 'w', 's'};
  const Bytes sd = descriptor(0x48, svc);
  Bytes sdt;
  put16(sdt, 0x1234);
  sdt.push_back(0xFF);
  put16(sdt, 1);
  sdt.push_back(0xFD);  // EIT_schedule_flag 0, EIT_present_following_flag 1
  put16(sdt, 0x8000U | 0x1000U | static_cast<unsigned>(sd.size()));  // running_status 4, free_CA_mode 1
  append(sdt, sd);
  append(data, packetize(ts::kSdtPid, section(0x42, 7, 2, sdt), cc1));
  // EIT present: one event at MJD 0xC079 12:45:00, 1 h, "Hello" / "World".
  Bytes ev;
  put16(ev, 0x2468);
  ev.insert(ev.end(), {0xC0, 0x79, 0x12, 0x45, 0x00, 0x01, 0x00, 0x00});
  Bytes se{'e', 'n', 'g', 0x05, 'H', 'e', 'l', 'l', 'o', 0x05, 'W', 'o', 'r', 'l', 'd'};
  const Bytes sed = descriptor(0x4D, se);
  put16(ev, 0x4000U | static_cast<unsigned>(sed.size()));
  append(ev, sed);
  Bytes eit;
  put16(eit, 7);
  put16(eit, 0x1234);
  eit.push_back(0);
  eit.push_back(0x4E);
  append(eit, ev);
  append(data, packetize(ts::kEitPid, section(0x4E, 1, 0, eit), cc2));
  // TDT and TOT with the same time.
  Bytes tdt{0x70, 0x70, 0x05, 0xC0, 0x79, 0x12, 0x45, 0x00};
  Bytes tot{0x73, 0xF0, 0x00, 0xC0, 0x79, 0x12, 0x45, 0x00, 0xF0, 0x00};
  tot[2] = static_cast<uint8_t>(tot.size() - 3 + 4);
  const uint32_t crc = ts::crc32_mpeg2(tot);
  put16(tot, crc >> 16U);
  put16(tot, crc & 0xFFFFU);
  Bytes both = tdt;
  append(both, tot);
  append(data, packetize(ts::kTimePid, both, cc3));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->psi.crc_errors == 0);

  const ts::Section* sdt_s = nullptr;
  const ts::Section* eit_s = nullptr;
  const ts::Section* tdt_s = nullptr;
  const ts::Section* tot_s = nullptr;
  for (const auto& s : f->psi.sections) {
    sdt_s = s.table_id == 0x42 ? &s : sdt_s;
    eit_s = s.table_id == 0x4E ? &s : eit_s;
    tdt_s = s.table_id == 0x70 ? &s : tdt_s;
    tot_s = s.table_id == 0x73 ? &s : tot_s;
  }
  REQUIRE(sdt_s != nullptr);
  CHECK(sdt_s->decoded);
  CHECK(sdt_s->original_network_id == 0x1234);
  REQUIRE(sdt_s->services.size() == 1);
  CHECK(sdt_s->services[0].id == 1);
  CHECK(sdt_s->services[0].eit_present_following);
  CHECK_FALSE(sdt_s->services[0].eit_schedule);
  CHECK(sdt_s->services[0].running_status == 4);
  CHECK(sdt_s->services[0].free_ca);
  const auto name = dec(0x48, sdt_s->services[0].descriptors[0].body);
  REQUIRE(field(name, "service_name") != nullptr);
  CHECK(field(name, "service_name")->text == "News");
  CHECK(field(name, "service_provider_name")->text == "Prov");
  CHECK(field(name, "service_type")->value == 0x64);

  REQUIRE(eit_s != nullptr);
  CHECK(eit_s->decoded);
  CHECK(eit_s->table_id_extension == 1);
  CHECK(eit_s->transport_stream_id == 7);
  REQUIRE(eit_s->events.size() == 1);
  CHECK(eit_s->events[0].id == 0x2468);
  CHECK(eit_s->events[0].duration == 0x010000);
  CHECK(ts::format_dvb_time(eit_s->events[0].start_time) == "1993-10-13 12:45:00");
  const auto ev_fields = dec(0x4D, eit_s->events[0].descriptors[0].body);
  CHECK(field(ev_fields, "event_name")->text == "Hello");
  CHECK(field(ev_fields, "text")->text == "World");

  REQUIRE((tdt_s != nullptr && tot_s != nullptr));
  CHECK(tdt_s->decoded);
  CHECK(tot_s->decoded);
  CHECK(tdt_s->utc_time == 0xC079124500ULL);
  CHECK(tot_s->crc_ok);
}

TEST_CASE("psi: ISO/IEC 13818-1 descriptors decode to named fields", "[container][mpegts][psi][fast]") {
  SECTION("HEVC video descriptor with HDR_WCG_idc") {
    const Bytes body{0x02, 0x60, 0x00, 0x00, 0x00, 0x80, 0, 0, 0, 0, 0, 0x7B, 0x0F};
    const auto v = dec(0x38, body);
    CHECK(field(v, "profile_idc")->value == 2);
    CHECK(field(v, "profile_compatibility_indication")->value == 0x60000000ULL);
    CHECK(field(v, "progressive_source_flag")->value == 1);
    CHECK(field(v, "level_idc")->value == 0x7B);
    CHECK(field(v, "HDR_WCG_idc")->value == 3);
  }
  SECTION("AVC video descriptor") {
    const auto v = dec(0x28, {0x64, 0x00, 0x28, 0x3F});
    CHECK(field(v, "profile_idc")->value == 0x64);
    CHECK(field(v, "level_idc")->value == 0x28);
    CHECK(field(v, "Frame_Packing_SEI_not_present_flag")->value == 1);
  }
  SECTION("registration, ISO 639, AAC") {
    const auto r = dec(0x05, {'H', 'E', 'V', 'C', 0x01});
    CHECK(field(r, "format_identifier")->text == "HEVC");
    CHECK(field(r, "additional_identification_info")->text == "01");
    const auto l = dec(0x0A, {'f', 'r', 'a', 0x01, 'e', 'n', 'g', 0x00});
    CHECK(l.size() == 4);
    CHECK(l[0].text == "fra");
    CHECK(l[2].text == "eng");
    const auto a = dec(0x2B, {0x01, 0x02, 0xFF});
    CHECK(field(a, "MPEG-2_AAC_channel_configuration")->value == 2);
  }
  SECTION("AC-3 and E-AC-3") {
    const auto a = dec(0x6A, {0xC0, 0x42, 0x08});  // component_type and bsid present
    CHECK(field(a, "component_type")->value == 0x42);
    CHECK(field(a, "bsid")->value == 8);
    CHECK(field(a, "mainid") == nullptr);
    const auto e = dec(0x7A, {0x24, 0x11, 0x22});  // mainid, substream1
    CHECK(field(e, "mainid")->value == 0x11);
    CHECK(field(e, "substream1")->value == 0x22);
  }
  SECTION("teletext and subtitling") {
    const auto t = dec(0x56, {'d', 'e', 'u', 0x09, 0x88});  // type 1, magazine 1, page 0x88
    CHECK(field(t, "teletext_type")->value == 1);
    CHECK(field(t, "teletext_magazine_number")->value == 1);
    CHECK(field(t, "teletext_page_number")->value == 0x88);
    const auto s = dec(0x59, {'e', 'n', 'g', 0x10, 0x00, 0x02, 0x00, 0x03});
    CHECK(field(s, "composition_page_id")->value == 2);
    CHECK(field(s, "ancillary_page_id")->value == 3);
  }
  SECTION("extension descriptor: HEVC timing and HRD") {
    const auto v = dec(0x3F, {0x03, 0x81, 0x80, 0x00, 0x00, 0x03, 0xE8});
    CHECK(field(v, "extension_descriptor_tag")->value == 3);
    CHECK(field(v, "picture_and_timing_info_present_flag")->value == 1);
    CHECK(field(v, "90kHz_flag")->value == 1);
    CHECK(field(v, "num_units_in_tick")->value == 1000);
    const auto d = dec(0x7F, {0x06, 0xAA});
    CHECK(field(d, "extension_payload")->text == "aa");
  }
  SECTION("unknown tag and a truncated body fall back to hex") {
    const auto u = dec(0xC5, {0xDE, 0xAD});
    REQUIRE(u.size() == 1);
    CHECK(u[0].name == "payload");
    CHECK(u[0].text == "dead");
    const auto t = dec(0x28, {0x64, 0x00});
    REQUIRE(t.size() == 1);
    CHECK(t[0].text == "6400");
    CHECK(ts::find_descriptor(0xC5) == nullptr);
    REQUIRE(ts::find_descriptor(0x38) != nullptr);
    CHECK(ts::find_descriptor(0x38)->name == "HEVC_video_descriptor");
  }
  SECTION("dvb_text") {
    CHECK(ts::dvb_text(Bytes{'A', 'B'}) == "AB");
    CHECK(ts::dvb_text(Bytes{0x05, 'A', 0xE9}) == "A\xC3\xA9");  // selector byte dropped, é as UTF-8
    CHECK(ts::dvb_text(Bytes{0x10, 0x00, 0x05, 'x'}) == "x");
  }
}

TEST_CASE("psi: the tree carries the section nodes under a second root", "[container][mpegts][psi][fast]") {
  const Bytes data = basic_stream();
  const ovt::io::MemoryFile file(std::span<const uint8_t>(data.data(), data.size()));
  const auto reader = ts::make_reader(file);
  REQUIRE(reader != nullptr);
  const auto buf = reader->tree();
  const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data());
  REQUIRE(t->root()->size() == 2);
  CHECK(std::string(t->root()->Get(0)->fourcc()->c_str()) == "MPTS");
  const auto* psit = t->root()->Get(1);
  CHECK(std::string(psit->fourcc()->c_str()) == "PSIT");
  REQUIRE(psit->children()->size() == 2);
  const auto* pmt = psit->children()->Get(1);
  CHECK(std::string(pmt->fourcc()->c_str()) == "PMT");
  CHECK(pmt->file_span()->length() > 0);
  // Children: one program_info descriptor then two ES nodes, the first with an AVC descriptor.
  REQUIRE(pmt->children()->size() == 3);
  CHECK(std::string(pmt->children()->Get(0)->fourcc()->c_str()) == "DESC");
  const auto* es = pmt->children()->Get(1);
  CHECK(std::string(es->fourcc()->c_str()) == "ES");
  bool stream_type = false;
  for (const auto* fld : *es->fields()) {
    if (std::string(fld->name()->c_str()) == "stream_type") {
      stream_type = fld->uvalue() == 0x1B;
    }
  }
  CHECK(stream_type);
  REQUIRE(es->children()->size() == 1);
  CHECK(std::string(es->children()->Get(0)->description()->c_str()) == "AVC_video_descriptor");
  // A stream without PSI keeps the single root.
  Bytes plain;
  uint8_t cc = 0;
  append(plain, filler(0x200, 4, cc));
  const ovt::io::MemoryFile pf(std::span<const uint8_t>(plain.data(), plain.size()));
  const auto pr = ts::make_reader(pf);
  REQUIRE(pr != nullptr);
  const auto pbuf = pr->tree();
  CHECK(flatbuffers::GetRoot<ovt::fb::ContainerTree>(pbuf.data())->root()->size() == 1);
}

TEST_CASE("psi: random and truncated section bytes never read out of bounds", "[container][mpegts][psi][fast]") {
  uint32_t state = 12345;
  const auto next = [&state]() {
    state = state * 1664525U + 1013904223U;
    return static_cast<uint8_t>(state >> 24U);
  };
  for (unsigned round = 0; round < 200; ++round) {
    Bytes data;
    std::array<uint8_t, 4> cc{};
    Bytes pat = section(0x00, 7, 0, pat_body({{1, 0x100}}));
    append(data, packetize(0, pat, cc[0]));
    for (const unsigned pid : {0x100U, 0x11U, 0x12U, 0x14U}) {
      Bytes sec(5 + next() % 120);
      for (auto& b : sec) {
        b = next();
      }
      sec[0] = pid == 0x100U ? 0x02 : (pid == 0x11U ? 0x42 : (pid == 0x12U ? 0x4E : (round % 2 != 0 ? 0x70 : 0x73)));
      sec[1] = static_cast<uint8_t>((sec[1] & 0x0FU) | 0xB0U);
      sec[2] = static_cast<uint8_t>(sec.size() - 3 - (next() % 3));  // sometimes shorter than declared
      append(data, packetize(pid, sec, cc[pid & 3U]));
    }
    uint8_t c = 0;
    append(data, filler(0x101, 1, c));
    const auto f = parse_bytes(data);
    CHECK(f.has_value());
  }
}

TEST_CASE("corpus: tsduck-test PMT lists the program streams that ffprobe reports",
          "[container][mpegts][psi][needs-corpus][fast]") {
  // Reference: `ffprobe -show_programs -show_streams` on test-012.ts, recorded in docs/acceptance/wi/WI-4.205.md.
  const auto path = ovt::test::corpus_or_skip("ts_h264_dvb");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  CHECK(f->psi.crc_errors == 0);
  REQUIRE(f->psi.programs.size() == 1);
  const ts::Program& p = f->psi.programs[0];
  CHECK(p.number == 4006);
  CHECK(p.pmt_pid == 160);
  CHECK(p.pcr_pid == 1060);
  REQUIRE(p.streams.size() == 6);
  CHECK(p.streams[0].stream_type == 0x1B);
  CHECK(p.streams[0].pid == 1060);
  for (size_t i = 1; i <= 4; ++i) {
    CHECK(p.streams[i].stream_type == 0x04);
  }
  CHECK(p.streams[5].stream_type == 0x06);
  CHECK(p.streams[5].pid == 0x42C);
}

namespace {

unsigned long number_of(const pugi::xml_node& n, const char* attr) {
  return std::strtoul(n.attribute(attr).value(), nullptr, 0);
}

}  // namespace

TEST_CASE("oracle: programs, PIDs and stream types equal tstables --all-sections --xml",
          "[container][mpegts][psi][needs-corpus][needs-oracle:tstables][fast]") {
  const auto path = ovt::test::corpus_or_skip("ts_h264_dvb");
  const auto xml = std::filesystem::temp_directory_path() / "ovt_psi_test_tstables.xml";
  const std::string cmd =
      "tstables --all-sections --xml-output '" + xml.string() + "' '" + path.string() + "' >/dev/null 2>&1";
  // NOLINTNEXTLINE(cert-env33-c): test-only; the command is fixed and the path comes from the corpus cache
  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr || pclose(pipe) != 0) {
    SKIP("tstables is not available");
  }
  pugi::xml_document doc;
  REQUIRE(doc.load_file(xml.c_str()));
  std::filesystem::remove(xml);

  std::map<unsigned long, unsigned long> pmt_pid_of;  // service_id -> program_map_PID
  for (const auto& svc : doc.child("tsduck").child("PAT").children("service")) {
    pmt_pid_of[number_of(svc, "service_id")] = number_of(svc, "program_map_PID");
  }
  std::map<unsigned long, std::pair<unsigned long, std::vector<std::pair<unsigned long, unsigned long>>>> pmts;
  for (const auto& pmt : doc.child("tsduck").children("PMT")) {
    auto& entry = pmts[number_of(pmt, "service_id")];
    entry.first = number_of(pmt, "PCR_PID");
    entry.second.clear();
    for (const auto& c : pmt.children("component")) {
      entry.second.emplace_back(number_of(c, "elementary_PID"), number_of(c, "stream_type"));
    }
  }

  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  REQUIRE(f->psi.programs.size() == pmt_pid_of.size());
  for (const ts::Program& p : f->psi.programs) {
    REQUIRE(pmt_pid_of.count(p.number) == 1);
    CHECK(p.pmt_pid == pmt_pid_of[p.number]);
    REQUIRE(pmts.count(p.number) == 1);
    CHECK(p.pcr_pid == pmts[p.number].first);
    REQUIRE(p.streams.size() == pmts[p.number].second.size());
    for (size_t i = 0; i < p.streams.size(); ++i) {
      CHECK(p.streams[i].pid == pmts[p.number].second[i].first);
      CHECK(p.streams[i].stream_type == pmts[p.number].second[i].second);
    }
  }
}
