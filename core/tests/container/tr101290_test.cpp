// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the TR 101 290 priority 1 and 2 report of the MPEG-TS reader (chapter 08 §8.6.7, WI-4.207).

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "container/mpegts/psi.hpp"
#include "container/mpegts/tr101290.hpp"
#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace ts = ovt::container::mpegts;

constexpr uint16_t kPmtPid = 0x0100;
constexpr uint16_t kVideoPid = 0x0101;
constexpr uint16_t kAudioPid = 0x0102;

Bytes with_crc(Bytes s) {
  const uint32_t crc = ts::crc32_mpeg2(s);
  for (int i = 3; i >= 0; --i) {
    s.push_back(static_cast<uint8_t>(crc >> (8U * static_cast<unsigned>(i))));
  }
  return s;
}

// A 188-byte packet: optional PCR, `payload` first (padded with 0xFF in the payload), `flags` OR-ed into byte 3.
Bytes make_packet(uint16_t pid, uint8_t cc, std::span<const uint8_t> payload, int64_t pcr = -1, uint8_t tsc = 0,
                  bool pusi = false) {
  Bytes out{ts::kSyncByte, static_cast<uint8_t>((pusi ? 0x40U : 0U) | (pid >> 8U)), static_cast<uint8_t>(pid), 0};
  Bytes af;
  if (pcr >= 0) {
    const auto base = static_cast<uint64_t>(pcr) / 300;
    const auto ext = static_cast<uint16_t>(static_cast<uint64_t>(pcr) % 300);
    af = {0x10,
          static_cast<uint8_t>(base >> 25U),
          static_cast<uint8_t>(base >> 17U),
          static_cast<uint8_t>(base >> 9U),
          static_cast<uint8_t>(base >> 1U),
          static_cast<uint8_t>(((base & 1U) << 7U) | 0x7EU | (ext >> 8U)),
          static_cast<uint8_t>(ext)};
  }
  const size_t room = 184 - (af.empty() ? 0 : 1 + af.size());
  const bool stuff = payload.size() < room;
  const bool has_af = !af.empty() || stuff;
  out[3] = static_cast<uint8_t>((static_cast<unsigned>(tsc) << 6U) |
                                ((has_af ? 2U : 0U) | (payload.empty() ? 0U : 1U)) << 4U | (cc & 15U));
  if (has_af) {
    const size_t len = 183 - payload.size();
    if (len > 0 && af.empty()) {
      af.push_back(0x00);
    }
    af.resize(len, 0xFF);
    out.push_back(static_cast<uint8_t>(len));
    out.insert(out.end(), af.begin(), af.end());
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

Bytes pat_section() {
  return with_crc({0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00});
}

// PMT of program 1: PCR on the video PID, video (H.264) and audio (AAC) elementary streams.
Bytes pmt_section() {
  return with_crc({0x02, 0xB0, 0x17, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0,
                   0x00, 0x1B, 0xE1, 0x01, 0xF0, 0x00, 0x0F, 0xE1, 0x02, 0xF0, 0x00});
}

Bytes cat_section() {
  return with_crc({0x01, 0xB0, 0x09, 0xFF, 0xFF, 0xC1, 0x00, 0x00});
}

Bytes with_pointer(const Bytes& section) {
  Bytes b{0x00};
  b.insert(b.end(), section.begin(), section.end());
  return b;
}

// What to leave out or break in the synthetic stream. Packet k is 1 ms after packet k - 1 (PCR = k × 27 000).
struct Plan {
  size_t packets = 1200;
  std::set<size_t> skip_pat;         // PAT slots (every 100th packet) that are left out
  std::set<size_t> skip_pmt;         // PMT slots (100k + 1)
  std::set<size_t> skip_cat;         // CAT slots (100k + 2)
  std::set<size_t> skip_audio;       // audio packets (every 20th, offset 12) left out; the counter still advances
  std::set<size_t> skip_pcr;         // video PCR packets (every 20th, offset 4) turned into PCR-free packets
  std::set<size_t> bad_sync;         // null packets whose sync byte is damaged
  std::set<size_t> error_indicator;  // null packets with transport_error_indicator
  size_t scrambled_from = SIZE_MAX;  // audio packets in [from, to] are scrambled
  size_t scrambled_to = 0;
  bool with_cat = true;
  bool with_pcr = true;
  size_t corrupt_crc_at = SIZE_MAX;  // the PAT at this slot gets a damaged CRC
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one flat table of slot rules
Bytes build(const Plan& p) {
  Bytes out;
  std::array<uint8_t, 8192> cc{};
  const auto next = [&cc](uint16_t pid) { return static_cast<uint8_t>(cc[pid]++ & 15U); };
  const Bytes pat = with_pointer(pat_section());
  const Bytes pmt = with_pointer(pmt_section());
  const Bytes cat = with_pointer(cat_section());
  const Bytes filler(184, 0xAB);
  const auto add = [&out](const Bytes& pk) { out.insert(out.end(), pk.begin(), pk.end()); };
  for (size_t k = 0; k < p.packets; ++k) {
    const auto pcr = static_cast<int64_t>(k) * 27000;
    if (k % 100 == 0) {
      if (!p.skip_pat.contains(k)) {
        const uint8_t c = next(0);
        Bytes body = pat;
        if (p.corrupt_crc_at == k) {
          body.back() ^= 0x55U;
        }
        add(make_packet(0, c, body, -1, 0, true));
        continue;
      }
    } else if (k % 100 == 1) {
      if (!p.skip_pmt.contains(k)) {
        add(make_packet(kPmtPid, next(kPmtPid), pmt, -1, 0, true));
        continue;
      }
    } else if (k % 100 == 2 && p.with_cat) {
      if (!p.skip_cat.contains(k)) {
        add(make_packet(1, next(1), cat, -1, 0, true));
        continue;
      }
    } else if (k % 20 == 4) {
      add(make_packet(kVideoPid, next(kVideoPid), Bytes(40, 0x11), p.with_pcr && !p.skip_pcr.contains(k) ? pcr : -1));
      continue;
    } else if (k % 20 == 12) {
      const uint8_t c = next(kAudioPid);
      if (!p.skip_audio.contains(k)) {
        add(make_packet(kAudioPid, c, Bytes(40, 0x22), -1,
                        k >= p.scrambled_from && k <= p.scrambled_to ? uint8_t{2} : uint8_t{0}));
        continue;
      }
    }
    Bytes n = make_packet(ts::kNullPid, 0, filler);
    if (p.bad_sync.contains(k)) {
      n[0] = 0x48;
    }
    if (p.error_indicator.contains(k)) {
      n[1] |= 0x80U;
    }
    add(n);
  }
  return out;
}

struct Report {
  std::vector<uint8_t> bytes;
  const ovt::fb::TsReport* r = nullptr;

  [[nodiscard]] const ovt::fb::TsCheck* check(std::string_view id) const {
    for (const auto* c : *r->checks()) {
      if (c->id()->string_view() == id) {
        return c;
      }
    }
    return nullptr;
  }
  [[nodiscard]] uint64_t count(std::string_view id) const { return check(id)->count(); }
};

Report report_of(const Bytes& b, const ts::Tr101290Options& options = {}) {
  const ovt::io::MemoryFile file{std::span<const uint8_t>(b)};
  const auto f = ts::parse(file);
  REQUIRE(f);
  Report rep;
  rep.bytes = ts::build_ts_report(*f, options);
  rep.r = flatbuffers::GetRoot<ovt::fb::TsReport>(rep.bytes.data());
  return rep;
}

void require_all_zero(const Report& rep, std::string_view except = {}) {
  for (const auto* c : *rep.r->checks()) {
    if (c->id()->string_view() != except) {
      INFO("check " << c->id()->str() << " " << c->name()->str());
      CHECK(c->count() == 0);
    }
  }
}

}  // namespace

TEST_CASE("tr101290: the report lists every priority 1 and 2 check in order", "[container][mpegts][tr101290][fast]") {
  const Report rep = report_of(build({}));
  constexpr std::array<std::string_view, 14> ids = {"1.1", "1.2", "1.3",  "1.4",  "1.5", "1.6", "2.1",
                                                    "2.2", "2.3", "2.3a", "2.3b", "2.4", "2.5", "2.6"};
  constexpr std::array<std::string_view, 14> names = {"TS_sync_loss",
                                                      "Sync_byte_error",
                                                      "PAT_error_2",
                                                      "Continuity_count_error",
                                                      "PMT_error_2",
                                                      "PID_error",
                                                      "Transport_error",
                                                      "CRC_error",
                                                      "PCR_error",
                                                      "PCR_repetition_error",
                                                      "PCR_discontinuity_indicator_error",
                                                      "PCR_accuracy_error",
                                                      "PTS_error",
                                                      "CAT_error"};
  REQUIRE(rep.r->checks()->size() == ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    CHECK(rep.r->checks()->Get(static_cast<flatbuffers::uoffset_t>(i))->id()->string_view() == ids[i]);
    CHECK(rep.r->checks()->Get(static_cast<flatbuffers::uoffset_t>(i))->name()->string_view() == names[i]);
  }
  CHECK(rep.r->packets() == 1200);
}

TEST_CASE("tr101290: a clean capture has no priority 1 or 2 error", "[container][mpegts][tr101290][fast]") {
  const Report rep = report_of(build({}));
  require_all_zero(rep);
  CHECK(rep.r->diagnostics()->empty());
  CHECK(rep.check("1.4")->first_offset() == 0);
}

TEST_CASE("tr101290: Continuity_count_error counts each lost packet once", "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.skip_audio = {12 + 20 * 5, 12 + 20 * 20, 12 + 20 * 40};
  const Report rep = report_of(build(p));
  CHECK(rep.count("1.4") == 3);
  // The first error is on the packet after the lost one (packet 12 + 20 × 6).
  CHECK(rep.check("1.4")->first_packet_index() == 12 + 20 * 6);
  CHECK(rep.check("1.4")->first_offset() == uint64_t{188} * (12 + 20 * 6));
  require_all_zero(rep, "1.4");
}

TEST_CASE("tr101290: sync byte and transport errors", "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.bad_sync = {503};
  p.error_indicator = {707, 907};
  const Report rep = report_of(build(p));
  CHECK(rep.count("1.2") == 1);
  CHECK(rep.check("1.2")->first_offset() == uint64_t{188} * 503);
  CHECK(rep.count("2.1") == 2);
  // Packet indices count only packets with a sync byte, so the damaged slot 503 shifts the index by one.
  CHECK(rep.check("2.1")->first_packet_index() == 706);
  CHECK(rep.check("2.1")->first_offset() == uint64_t{188} * 707);
  CHECK(rep.count("1.1") == 0);
}

TEST_CASE("tr101290: CRC_error", "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.corrupt_crc_at = 300;
  const Report rep = report_of(build(p));
  CHECK(rep.count("2.2") == 1);
  CHECK(rep.check("2.2")->first_packet_index() == 300);
}

TEST_CASE("tr101290: a PCR gap is one 2.3, 2.3a and 2.3b error", "[container][mpegts][tr101290][fast]") {
  Plan p;
  // PCR every 20 packets (20 ms); drop 8 of them: a 180 ms interval, above 40 ms and above 100 ms.
  for (size_t k = 304; k < 304 + 20 * 8; k += 20) {
    p.skip_pcr.insert(k);
  }
  const Report rep = report_of(build(p));
  CHECK(rep.count("2.3") == 1);
  CHECK(rep.count("2.3a") == 1);
  CHECK(rep.count("2.3b") == 1);
  CHECK(rep.check("2.3")->first_packet_index() == 464);
}

TEST_CASE("tr101290: PAT and PMT timers (500 ms)", "[container][mpegts][tr101290][fast]") {
  SECTION("an interval of exactly 500 ms is allowed") {
    Plan p;
    p.skip_pat = {100, 200, 300, 400};  // PAT at 0 and 500
    p.skip_pmt = {101, 201, 301, 401};
    require_all_zero(report_of(build(p)));
  }
  SECTION("600 ms without a PAT is one PAT_error_2") {
    Plan p;
    p.skip_pat = {100, 200, 300, 400, 500};
    const Report rep = report_of(build(p));
    CHECK(rep.count("1.3") == 1);
    CHECK(rep.check("1.3")->first_packet_index() == 0);
    require_all_zero(rep, "1.3");
    CHECK(rep.r->diagnostics()->size() == 1);
    CHECK(rep.r->diagnostics()->Get(0)->code()->string_view() == "CNT-TS-1003");
  }
  SECTION("600 ms without a PMT is one PMT_error_2") {
    Plan p;
    p.skip_pmt = {101, 201, 301, 401, 501};
    const Report rep = report_of(build(p));
    CHECK(rep.count("1.5") == 1);
    CHECK(rep.check("1.5")->first_packet_index() == 1);
    require_all_zero(rep, "1.5");
  }
  SECTION("no PAT in the last 600 ms of the file counts up to its end") {
    Plan p;
    p.packets = 800;
    p.skip_pat = {200, 300, 400, 500, 600, 700};
    p.skip_pmt = {201, 301, 401, 501, 601, 701};
    const Report rep = report_of(build(p));
    CHECK(rep.count("1.3") == 1);
    CHECK(rep.count("1.5") == 1);
  }
}

TEST_CASE("tr101290: PID_error timeout is configurable", "[container][mpegts][tr101290][fast]") {
  Plan p;
  // The audio PID is silent for 360 ms (packets 12 + 20 × 10 to 12 + 20 × 27).
  for (size_t i = 10; i < 28; ++i) {
    p.skip_audio.insert(12 + 20 * i);
  }
  const Bytes b = build(p);
  const Report strict = report_of(b, {.pid_timeout_ms = 300});
  CHECK(strict.count("1.6") == 1);
  CHECK(strict.check("1.6")->first_packet_index() == 12 + 20 * 9);
  CHECK(strict.count("1.4") == 1);  // the loss is also one counter error
  const Report lax = report_of(b);  // 5 s
  CHECK(lax.count("1.6") == 0);
}

TEST_CASE("tr101290: CAT_error timer runs while packets are scrambled", "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.scrambled_from = 212;
  p.scrambled_to = 972;
  p.skip_cat = {202, 302, 402, 502, 602, 702};  // CAT at 102 and 802: 700 ms
  const Report rep = report_of(build(p));
  CHECK(rep.count("2.6") == 1);
  CHECK(rep.check("2.6")->first_packet_index() == 102);

  Plan q = p;
  q.skip_cat.clear();
  require_all_zero(report_of(build(q)));

  Plan clear;  // no scrambled packet, no CAT: nothing to report
  clear.with_cat = false;
  require_all_zero(report_of(build(clear)));
}

TEST_CASE("tr101290: scrambled packets without any CAT stay one whole-file error",
          "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.with_cat = false;
  p.scrambled_from = 212;
  p.scrambled_to = 972;
  const Report rep = report_of(build(p));
  CHECK(rep.count("2.6") == 1);
}

TEST_CASE("tr101290: without a PCR the timer checks are not evaluated", "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.with_pcr = false;
  p.skip_pat = {100, 200, 300, 400, 500, 600, 700};
  const Report rep = report_of(build(p));
  CHECK(rep.count("1.3") == 0);
}

TEST_CASE("tr101290: the reader exposes the report", "[container][mpegts][tr101290][fast]") {
  Plan p;
  p.skip_audio = {12 + 20 * 5};
  const Bytes b = build(p);
  const ovt::io::MemoryFile file{std::span<const uint8_t>(b)};
  const auto reader = ts::make_reader(file);
  REQUIRE(reader != nullptr);
  const auto bytes = reader->ts_report();
  REQUIRE(!bytes.empty());
  const auto* r = flatbuffers::GetRoot<ovt::fb::TsReport>(bytes.data());
  REQUIRE(r->checks()->size() == 14);
  CHECK(r->checks()->Get(3)->count() == 1);
  // The list holds the CNT-TS diagnostic the walk raised.
  REQUIRE(r->diagnostics()->size() == 1);
  CHECK(r->diagnostics()->Get(0)->code()->string_view() == "CNT-TS-1006");
}

TEST_CASE("corpus: syn_ts_cc_errors reports its seven continuity errors",
          "[container][mpegts][tr101290][needs-corpus][fast]") {
  // Recipe 33 drops 7 packets (tools/ts_inject_errors.py --cc-errors 7), each exactly one error. TSDuck
  // `tsp -P continuity` gives the same count (oracle comparison in docs/acceptance/wi/WI-4.207.md).
  const auto path = ovt::test::corpus_or_skip("syn_ts_cc_errors");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  const auto bytes = ts::build_ts_report(*f);
  const auto* r = flatbuffers::GetRoot<ovt::fb::TsReport>(bytes.data());
  CHECK(r->checks()->Get(3)->id()->string_view() == "1.4");
  CHECK(r->checks()->Get(3)->count() == 7);
}

TEST_CASE("corpus: syn_ts_clean has no priority 1 error", "[container][mpegts][tr101290][needs-corpus][fast]") {
  // ffmpeg's muxer repeats the PCR every 100 ms, so priority 2 check 2.3a legitimately fires (recipe 33 note).
  const auto cc_path = ovt::test::corpus_or_skip("syn_ts_cc_errors");  // recipe 33 writes the clean file beside it
  const auto path = cc_path.parent_path() / "syn_ts_clean.ts";
  if (!std::filesystem::exists(path)) {
    SKIP("syn_ts_clean.ts missing from the corpus entry");
  }
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  const auto bytes = ts::build_ts_report(*f);
  const auto* r = flatbuffers::GetRoot<ovt::fb::TsReport>(bytes.data());
  for (flatbuffers::uoffset_t i = 0; i < 6; ++i) {
    INFO("check " << r->checks()->Get(i)->id()->str());
    CHECK(r->checks()->Get(i)->count() == 0);
  }
}
