// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the MPEG-TS PSI layer (chapter 08 §8.6.7, WI-4.205a): section reassembly, CRC-32, PAT / PMT / CAT
// decoding, version replacement and the PAT_error_2 / PMT_error_2 / PID_error / CRC_error / CAT_error diagnostics.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "container/diagnostics.h"
#include "container/mpegts/psi.hpp"
#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"
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
  CHECK(with->psi.cat_descriptors[0].tag == 0x09);
  CHECK(with->psi.cat_descriptors[0].body == Bytes({0x06, 0x04, 0xE1, 0x23, 0xAA}));
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
    for (const unsigned pid : {0x100U, 0x1U}) {
      Bytes sec(5 + next() % 120);
      for (auto& b : sec) {
        b = next();
      }
      sec[0] = pid == 0x100U ? 0x02 : 0x01;
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

TEST_CASE("psi: a short-form section on a PSI PID is malformed, not decoded", "[container][mpegts][psi][fast]") {
  // section_syntax_indicator 0 with table_id 0x00 / 0x02 / 0x01 and 1..5 body bytes: no CRC_32 to subtract.
  for (const uint8_t table_id : {uint8_t{0x00}, uint8_t{0x02}, uint8_t{0x01}}) {
    for (uint8_t len = 0; len <= 5; ++len) {
      uint8_t cc0 = 0;
      uint8_t cc1 = 0;
      Bytes data;
      append(data, packetize(0, section(0x00, 7, 0, pat_body({{1, 0x100}})), cc0));
      Bytes sec{table_id, 0x30, len};
      sec.resize(3 + len, 0x55);
      append(data, packetize(table_id == 0x01 ? 1 : (table_id == 0x00 ? 0 : 0x100), sec, table_id == 0x00 ? cc0 : cc1));
      uint8_t cc2 = 0;
      append(data, filler(0x101, 2, cc2));
      const auto f = parse_bytes(data);
      REQUIRE(f);
      for (const auto& s : f->psi.sections) {
        CHECK((s.syntax_indicator || !s.decoded));
      }
    }
  }
}
