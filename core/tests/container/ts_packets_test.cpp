// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the MPEG-TS packet layer (chapter 08 §8.6.7, WI-4.204): packet size detection, sync and resync,
// header and adaptation-field decoding, per-PID statistics, diagnostics and the lazy packet tree.

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;
constexpr size_t kP188 = 188;
constexpr size_t kP192 = 192;
constexpr size_t kP204 = 204;

Bytes::iterator at_offset(Bytes& b, size_t off) {
  return b.begin() + static_cast<std::ptrdiff_t>(off);
}

namespace ts = ovt::container::mpegts;
using ovt::container::DiagCode;

// One transport packet. Payload bytes never equal 0x47 so that no accidental sync pattern appears.
struct Pkt {
  uint16_t pid = 0x100;
  uint8_t cc = 0;
  uint8_t afc = 1;
  bool tei = false;
  bool pusi = false;
  uint8_t tsc = 0;
  Bytes af_body;  // adaptation field bytes after adaptation_field_length (flags first)
};

Bytes make_packet(const Pkt& p, uint32_t seed = 0) {
  Bytes out{ts::kSyncByte, static_cast<uint8_t>((p.tei ? 0x80U : 0U) | (p.pusi ? 0x40U : 0U) | (p.pid >> 8U)),
            static_cast<uint8_t>(p.pid & 0xFFU), static_cast<uint8_t>((p.tsc << 6U) | (p.afc << 4U) | p.cc)};
  if ((p.afc & 2U) != 0) {
    Bytes body = p.af_body;
    if (p.afc == 2) {
      body.resize(183, 0xFF);  // adaptation-only: the field fills the packet
    }
    out.push_back(static_cast<uint8_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
  }
  for (uint32_t j = 0; out.size() < ts::kPacketBytes; ++j) {
    const auto v = static_cast<uint8_t>((seed * 31U + j * 13U + p.pid) % 251U);
    out.push_back(v == ts::kSyncByte ? 0x48 : v);
  }
  return out;
}

Bytes concat(const std::vector<Bytes>& v, size_t trailer = 0, size_t prefix = 0) {
  Bytes out;
  for (size_t i = 0; i < v.size(); ++i) {
    out.insert(out.end(), prefix, static_cast<uint8_t>(0x10 + i % 16));
    out.insert(out.end(), v[i].begin(), v[i].end());
    out.insert(out.end(), trailer, static_cast<uint8_t>(0xE0 + i % 16));
  }
  return out;
}

// n packets on three PIDs with correct continuity counters (PID 0x1FFF stays at 0).
std::vector<Bytes> clean_packets(size_t n) {
  std::array<uint8_t, 3> cc{};
  const std::array<uint16_t, 3> pids = {0x0000, 0x0100, 0x0101};
  std::vector<Bytes> v;
  for (size_t i = 0; i < n; ++i) {
    const size_t k = i % 3;
    Pkt p;
    p.pid = pids[k];
    p.cc = cc[k];
    cc[k] = static_cast<uint8_t>((cc[k] + 1U) & 15U);
    v.push_back(make_packet(p, static_cast<uint32_t>(i)));
  }
  return v;
}

size_t count_code(const ts::File& f, DiagCode code) {
  return static_cast<size_t>(std::count_if(f.diagnostics.begin(), f.diagnostics.end(),
                                           [code](const ts::Diagnostic& d) { return d.code == code; }));
}

std::optional<ts::File> parse_bytes(const Bytes& b) {
  const ovt::io::MemoryFile file{std::span<const uint8_t>(b)};
  return ts::parse(file);
}

const ts::PidStats* pid_stats(const ts::File& f, uint16_t pid) {
  const auto it = std::find_if(f.pids.begin(), f.pids.end(), [pid](const ts::PidStats& s) { return s.pid == pid; });
  return it == f.pids.end() ? nullptr : &*it;
}

Bytes pcr_field(uint64_t base, uint16_t ext, uint8_t extra_flags = 0) {
  return {static_cast<uint8_t>(0x10U | extra_flags), static_cast<uint8_t>(base >> 25U),
          static_cast<uint8_t>(base >> 17U),         static_cast<uint8_t>(base >> 9U),
          static_cast<uint8_t>(base >> 1U),          static_cast<uint8_t>(((base & 1U) << 7U) | 0x7EU | (ext >> 8U)),
          static_cast<uint8_t>(ext & 0xFFU)};
}

}  // namespace

TEST_CASE("decode_packet: header bits", "[container][mpegts][fast]") {
  Pkt p;
  p.pid = 0x1ABC;
  p.cc = 9;
  p.afc = 1;
  p.tei = true;
  p.pusi = true;
  p.tsc = 2;
  const Bytes b = make_packet(p);
  const auto h = ts::decode_packet(b);
  REQUIRE(h);
  CHECK(h->transport_error_indicator);
  CHECK(h->payload_unit_start_indicator);
  CHECK_FALSE(h->transport_priority);
  CHECK(h->pid == 0x1ABC);
  CHECK(h->transport_scrambling_control == 2);
  CHECK(h->adaptation_field_control == 1);
  CHECK(h->continuity_counter == 9);
  CHECK_FALSE(h->adaptation);
  CHECK(h->payload_offset == 4);
  CHECK(h->payload_size == 184);

  CHECK_FALSE(ts::decode_packet(std::span<const uint8_t>(b).first(3)));  // shorter than the header
  Bytes bad = b;
  bad[0] = 0x46;
  CHECK_FALSE(ts::decode_packet(bad));
  const auto partial = ts::decode_packet(std::span<const uint8_t>(b).first(100));
  REQUIRE(partial);
  CHECK(partial->payload_size == 96);  // never reads past the span
}

TEST_CASE("decode_packet: adaptation field and PCR", "[container][mpegts][fast]") {
  // PCR = base * 300 + extension, 33-bit base and 9-bit extension at their maxima.
  const uint64_t base = 0x1FFFFFFFFULL;
  Pkt p;
  p.afc = 3;
  p.af_body = pcr_field(base, 299, 0x80U /* discontinuity_indicator */);
  const auto h = ts::decode_packet(make_packet(p));
  REQUIRE(h);
  REQUIRE(h->adaptation);
  const ts::AdaptationField& a = *h->adaptation;
  CHECK(a.length == 7);
  CHECK(a.discontinuity_indicator);
  CHECK(a.pcr_flag);
  CHECK_FALSE(a.opcr_flag);
  CHECK(a.pcr_base == base);
  CHECK(a.pcr_extension == 299);
  CHECK(a.pcr() == base * 300 + 299);
  CHECK_FALSE(a.truncated);
  CHECK(h->payload_offset == 12);
  CHECK(h->payload_size == 176);

  Pkt q;
  q.afc = 3;
  q.af_body = pcr_field(1, 0);
  const auto h1 = ts::decode_packet(make_packet(q));
  REQUIRE(h1);
  CHECK(h1->adaptation->pcr() == 300);

  // OPCR, splice countdown and private data after the PCR.
  Pkt r;
  r.afc = 3;
  r.af_body = pcr_field(7, 11, 0x08U | 0x04U | 0x02U);
  const Bytes opcr = pcr_field(9, 13);
  r.af_body.insert(r.af_body.end(), opcr.begin() + 1, opcr.end());
  r.af_body.push_back(0xFE);  // splice_countdown = -2
  r.af_body.push_back(2);     // transport_private_data_length
  r.af_body.push_back(0xAA);
  r.af_body.push_back(0xBB);
  const auto h2 = ts::decode_packet(make_packet(r));
  REQUIRE(h2);
  CHECK(h2->adaptation->opcr_base == 9);
  CHECK(h2->adaptation->opcr_extension == 13);
  CHECK(h2->adaptation->opcr() == 9 * 300 + 13);
  CHECK(h2->adaptation->splice_countdown == -2);
  CHECK(h2->adaptation->transport_private_data_length == 2);
  CHECK_FALSE(h2->adaptation->truncated);

  // Adaptation-only packet: no payload; a zero-length field (one stuffing byte) is legal.
  Pkt only;
  only.afc = 2;
  const auto h3 = ts::decode_packet(make_packet(only));
  REQUIRE(h3);
  CHECK(h3->payload_size == 0);
  CHECK_FALSE(h3->has_payload());
  CHECK(h3->adaptation->length == 183);
  Pkt zero;
  zero.afc = 3;
  const auto h4 = ts::decode_packet(make_packet(zero));
  REQUIRE(h4);
  CHECK(h4->adaptation->length == 0);
  CHECK(h4->payload_offset == 5);
  CHECK(h4->payload_size == 183);

  // A field whose length runs past the packet is flagged and never read out of bounds.
  Bytes bad = make_packet(q);
  bad[4] = 200;
  const auto h5 = ts::decode_packet(bad);
  REQUIRE(h5);
  CHECK(h5->adaptation->truncated);
  CHECK(h5->payload_size == 0);
  // PCR flag set but the field is too short for it.
  Bytes shortpcr = make_packet(zero);
  shortpcr[4] = 3;
  shortpcr[5] = 0x10;
  const auto h6 = ts::decode_packet(shortpcr);
  REQUIRE(h6);
  CHECK(h6->adaptation->truncated);
}

TEST_CASE("parse: clean 188-byte stream", "[container][mpegts][fast]") {
  const Bytes data = concat(clean_packets(30));
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->geometry.stride == 188);
  CHECK(f->geometry.prefix == 0);
  CHECK(f->geometry.first_sync == 0);
  CHECK(f->packet_count == 30);
  CHECK(f->diagnostics.empty());
  REQUIRE(f->pids.size() == 3);
  CHECK(f->pids[0].pid == 0);
  CHECK(f->pids[1].pid == 0x100);
  CHECK(f->pids[2].pid == 0x101);
  for (const auto& s : f->pids) {
    CHECK(s.packets == 10);
    CHECK(s.payload_packets == 10);
    CHECK(s.continuity_errors == 0);
  }
  CHECK(f->pids[1].first_packet == 1);
  CHECK(f->packet_offset(7) == 7 * kP188);
}

TEST_CASE("parse: not a transport stream", "[container][mpegts][fast]") {
  CHECK_FALSE(parse_bytes(Bytes(4096, 0)));
  CHECK_FALSE(parse_bytes(Bytes{}));
  CHECK_FALSE(parse_bytes(Bytes(100, ts::kSyncByte)));  // too short for two packets
  Bytes one = concat(clean_packets(1));
  CHECK_FALSE(parse_bytes(one));
}

TEST_CASE("parse: a 1-byte cut resyncs and reports CNT-TS-1001 once", "[container][mpegts][fast]") {
  Bytes data = concat(clean_packets(100));
  data.erase(at_offset(data, 50 * kP188 + 80));  // one byte inside packet 50
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->sync_loss_events == 1);
  CHECK(count_code(*f, DiagCode::CNT_TS_1001) == 1);
  CHECK(count_code(*f, DiagCode::CNT_TS_1002) == 0);
  CHECK(f->packet_count == 100);  // packet 50 (damaged) and every packet after the resync are still walked
  CHECK(f->runs.size() == 2);
  CHECK(f->runs[1].sync_offset == 50 * kP188 + 187);
  const auto& d = f->diagnostics.front();
  CHECK(d.message.find("TS_sync_loss") == 0);
  CHECK(d.message.find("`1` consecutive packets") != std::string::npos);
  CHECK(d.offset == 51 * kP188);
}

TEST_CASE("parse: an isolated bad sync byte is CNT-TS-1002, not a sync loss", "[container][mpegts][fast]") {
  Bytes data = concat(clean_packets(60));
  data[20 * kP188] = 0x00;
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->sync_byte_errors == 1);
  CHECK(f->sync_loss_events == 0);
  CHECK(count_code(*f, DiagCode::CNT_TS_1002) == 1);
  CHECK(count_code(*f, DiagCode::CNT_TS_1001) == 0);
  CHECK(f->packet_count == 59);
  CHECK(f->diagnostics.front().message == "Sync_byte_error at packet `20`");
  CHECK(f->packet_offset(20) == 21 * kP188);  // the bad slot is skipped, the index list stays contiguous
}

TEST_CASE("parse: garbage in the middle and at the start", "[container][mpegts][fast]") {
  Bytes data = concat(clean_packets(40));
  data.insert(at_offset(data, 10 * kP188), 500, 0x00);  // 500 junk bytes between packets 9 and 10
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->sync_loss_events == 1);
  CHECK(f->packet_count == 40);
  CHECK(f->runs.size() == 2);
  CHECK(f->runs[1].sync_offset == 10 * kP188 + 500);

  Bytes lead(7, 0x00);
  const Bytes body = concat(clean_packets(10));
  lead.insert(lead.end(), body.begin(), body.end());
  const auto g = parse_bytes(lead);
  REQUIRE(g);
  CHECK(g->geometry.first_sync == 7);
  CHECK(g->geometry.prefix == 0);
  CHECK(g->sync_loss_events == 1);
  CHECK(g->diagnostics.front().offset == 0);
  CHECK(g->packet_count == 10);

  Bytes tail = concat(clean_packets(10));
  tail.insert(tail.end(), 400, 0x11);  // trailing junk: sync loss with no resync
  const auto h = parse_bytes(tail);
  REQUIRE(h);
  CHECK(h->packet_count == 10);
  CHECK(h->sync_loss_events == 1);
  CHECK(h->diagnostics.back().code == DiagCode::CNT_TS_1001);

  Bytes partial = concat(clean_packets(10));
  partial.insert(partial.end(), 100, 0x11);  // under one packet left: ignored without a diagnostic
  const auto i = parse_bytes(partial);
  REQUIRE(i);
  CHECK(i->diagnostics.empty());
}

TEST_CASE("parse: continuity counter rules", "[container][mpegts][fast]") {
  const auto run = [](const std::vector<Pkt>& pkts) {
    std::vector<Bytes> v;
    v.reserve(pkts.size());
    uint32_t seed = 0;
    for (const Pkt& p : pkts) {
      v.push_back(make_packet(p, seed++));
    }
    return parse_bytes(concat(v));
  };
  const auto mk = [](uint8_t cc, uint8_t afc = 1, bool disc = false) {
    Pkt p;
    p.cc = cc;
    p.afc = afc;
    if (afc == 2) {
      p.af_body = {0x00};  // flags without discontinuity_indicator (the stuffing default is 0xFF)
    }
    if (disc) {
      p.afc = 3;
      p.af_body = {0x80};
    }
    return p;
  };

  SECTION("a gap is reported with expected and got") {
    const auto f = run({mk(0), mk(1), mk(2), mk(4), mk(5), mk(6)});
    REQUIRE(f);
    CHECK(count_code(*f, DiagCode::CNT_TS_1006) == 1);
    CHECK(f->pids[0].continuity_errors == 1);
    CHECK(f->diagnostics.front().message == "Continuity_count_error on PID `0x0100` at packet `3` (`3`/`4`)");
  }
  SECTION("wraparound 15 to 0 is fine") {
    std::vector<Pkt> v;
    v.reserve(40);
    for (unsigned i = 0; i < 40; ++i) {
      v.push_back(mk(static_cast<uint8_t>(i & 15U)));
    }
    const auto f = run(v);
    REQUIRE(f);
    CHECK(f->diagnostics.empty());
  }
  SECTION("one duplicate is accepted, two are not") {
    const auto ok = run({mk(0), mk(1), mk(1), mk(2), mk(3)});
    REQUIRE(ok);
    CHECK(ok->diagnostics.empty());
    const auto bad = run({mk(0), mk(1), mk(1), mk(1), mk(2)});
    REQUIRE(bad);
    CHECK(count_code(*bad, DiagCode::CNT_TS_1006) == 1);
  }
  SECTION("packets without payload do not increment the counter") {
    const auto ok = run({mk(0), mk(1), mk(1, 2), mk(1, 2), mk(2), mk(3)});
    REQUIRE(ok);
    CHECK(ok->diagnostics.empty());
    const auto bad = run({mk(0), mk(1), mk(2, 2), mk(3)});  // adaptation-only packet must repeat the counter
    REQUIRE(bad);
    CHECK(count_code(*bad, DiagCode::CNT_TS_1006) >= 1);
  }
  SECTION("discontinuity_indicator resets the expectation") {
    const auto f = run({mk(0), mk(1), mk(9, 1, true), mk(10), mk(11)});
    REQUIRE(f);
    CHECK(f->diagnostics.empty());
  }
  SECTION("null packets and reserved afc are not checked") {
    Pkt a;
    a.pid = ts::kNullPid;
    a.cc = 0;
    Pkt b = a;
    b.cc = 7;
    const auto f = run({a, b, a});
    REQUIRE(f);
    CHECK(f->diagnostics.empty());
    CHECK(f->pids[0].packets == 3);
  }
  SECTION("counters are per PID") {
    Pkt a;
    a.pid = 0x100;
    Pkt b;
    b.pid = 0x200;
    b.cc = 5;
    Pkt a1 = a;
    a1.cc = 1;
    Pkt b1 = b;
    b1.cc = 6;
    const auto f = run({a, b, a1, b1});
    REQUIRE(f);
    CHECK(f->diagnostics.empty());
  }
}

TEST_CASE("parse: transport errors and scrambling statistics", "[container][mpegts][fast]") {
  std::vector<Bytes> v;
  for (uint8_t i = 0; i < 8; ++i) {
    Pkt p;
    p.cc = i;
    p.tei = i == 3;
    p.tsc = i >= 6 ? 2 : 0;
    v.push_back(make_packet(p, i));
  }
  const auto f = parse_bytes(concat(v));
  REQUIRE(f);
  const ts::PidStats* s = pid_stats(*f, 0x100);
  REQUIRE(s);
  CHECK(s->packets == 8);
  CHECK(s->transport_errors == 1);
  CHECK(s->scrambled_packets == 2);
  CHECK(s->continuity_errors == 0);  // the packet with the error indicator is skipped and the counter restarts after it
  CHECK(count_code(*f, DiagCode::CNT_TS_2001) == 1);
  CHECK(f->diagnostics.front().message == "Transport_error: `transport_error_indicator` set at packet `3`");
}

TEST_CASE("parse: 192-byte M2TS, 192-byte trailer and 204-byte packets", "[container][mpegts][fast]") {
  const auto pk = clean_packets(20);

  SECTION("M2TS: 4-byte timestamp in front of each packet") {
    const auto f = parse_bytes(concat(pk, 0, 4));
    REQUIRE(f);
    CHECK(f->geometry.stride == 192);
    CHECK(f->geometry.prefix == 4);
    CHECK(f->geometry.first_sync == 4);
    CHECK(f->packet_count == 20);
    CHECK(f->diagnostics.empty());
    CHECK(f->packet_offset(3) == 3 * kP192);
    CHECK(f->sync_offset(3) == 3 * kP192 + 4);
  }
  SECTION("192 bytes with the 4 extra bytes behind the packet") {
    const auto f = parse_bytes(concat(pk, 4, 0));
    REQUIRE(f);
    CHECK(f->geometry.stride == 192);
    CHECK(f->geometry.prefix == 0);
    CHECK(f->packet_count == 20);
    CHECK(f->diagnostics.empty());
  }
  SECTION("204 bytes (Reed-Solomon parity)") {
    const auto f = parse_bytes(concat(pk, 16, 0));
    REQUIRE(f);
    CHECK(f->geometry.stride == 204);
    CHECK(f->packet_count == 20);
    CHECK(f->diagnostics.empty());
    CHECK(f->packet_offset(5) == 5 * kP204);
  }
}

TEST_CASE("detect_geometry: needs at least two packets", "[container][mpegts][fast]") {
  const Bytes two = concat(clean_packets(2));
  const auto g = ts::detect_geometry(two, two.size());
  REQUIRE(g);
  CHECK(g->stride == 188);
  const Bytes one = concat(clean_packets(1));
  CHECK_FALSE(ts::detect_geometry(one, one.size()));
}

TEST_CASE("parse: determinism", "[container][mpegts][fast]") {
  Bytes data = concat(clean_packets(80));
  data.erase(at_offset(data, 30 * kP188 + 3));
  data[60 * kP188] = 0;
  const auto a = parse_bytes(data);
  const auto b = parse_bytes(data);
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(a->diagnostics.size() == b->diagnostics.size());
  for (size_t i = 0; i < a->diagnostics.size(); ++i) {
    CHECK(a->diagnostics[i].message == b->diagnostics[i].message);
    CHECK(a->diagnostics[i].offset == b->diagnostics[i].offset);
  }
}

TEST_CASE("reader: packet tree, fields and spans", "[container][mpegts][fast]") {
  std::vector<Bytes> v = clean_packets(5);
  Pkt p;
  p.pid = 0x0200;
  p.afc = 3;
  p.pusi = true;
  p.cc = 0;
  p.af_body = pcr_field(1000, 5);
  v[2] = make_packet(p);
  const Bytes data = concat(v);
  const ovt::io::MemoryFile file{std::span<const uint8_t>(data)};
  auto reader = ts::make_reader(file);
  REQUIRE(reader != nullptr);
  CHECK(reader->track_count() == 0);
  const auto buf = reader->tree();
  const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data());
  CHECK(std::string(tree->format()->c_str()) == "mpegts");
  REQUIRE(tree->root()->size() == 1);
  const auto* root = tree->root()->Get(0);
  CHECK(std::string(root->fourcc()->c_str()) == "MPTS");
  CHECK(root->file_span()->length() == data.size());
  REQUIRE(root->children()->size() == 5);
  const auto* pk = root->children()->Get(2);
  CHECK(std::string(pk->fourcc()->c_str()) == "TSPK");
  CHECK(pk->file_span()->offset() == 2 * kP188);
  CHECK(pk->file_span()->length() == 188);
  CHECK(pk->header_length() == 4);
  const auto field = [](const ovt::fb::BoxNode* n, const char* name) -> const ovt::fb::Field* {
    for (const auto* f : *n->fields()) {
      if (std::string(f->name()->c_str()) == name) {
        return f;
      }
    }
    return nullptr;
  };
  CHECK(field(pk, "PID")->uvalue() == 0x200);
  CHECK(field(pk, "PID")->span()->bit_offset() == 11);
  CHECK(field(pk, "PID")->span()->bit_length() == 13);
  CHECK(field(pk, "payload_unit_start_indicator")->uvalue() == 1);
  CHECK(field(pk, "adaptation_field_control")->uvalue() == 3);
  CHECK(field(pk, "continuity_counter")->span()->bit_offset() == 28);
  REQUIRE(pk->children()->size() == 1);
  const auto* adpt = pk->children()->Get(0);
  CHECK(std::string(adpt->fourcc()->c_str()) == "ADPT");
  CHECK(adpt->file_span()->offset() == 2 * kP188 + 4);
  CHECK(adpt->file_span()->length() == 8);
  CHECK(field(adpt, "program_clock_reference_base")->uvalue() == 1000);
  CHECK(field(adpt, "program_clock_reference_base")->span()->bit_offset() == 16);
  CHECK(field(adpt, "program_clock_reference_base")->span()->bit_length() == 33);
  CHECK(field(adpt, "program_clock_reference_extension")->uvalue() == 5);
  CHECK(field(adpt, "program_clock_reference_extension")->span()->bit_offset() == 55);
  CHECK(field(adpt, "PCR_27MHz")->uvalue() == 1000 * 300 + 5);
  REQUIRE(pk->payload_span() != nullptr);
  CHECK(pk->payload_span()->offset() == 2 * kP188 + 12);
  CHECK(pk->payload_span()->length() == 176);

  // The root carries the derived statistics.
  CHECK(field(root, "packet_size")->uvalue() == 188);
  CHECK(field(root, "packet_count")->uvalue() == 5);
  CHECK(field(root, "pid_count")->uvalue() == 3);
}

TEST_CASE("reader: M2TS fields carry the timestamp prefix", "[container][mpegts][fast]") {
  Bytes data;
  const auto pk = clean_packets(4);
  for (uint32_t i = 0; i < pk.size(); ++i) {
    const uint32_t ts_word = (1U << 30U) | (1000U + i);  // copy_permission_indicator 1, arrival_time_stamp 1000+i
    for (int s = 24; s >= 0; s -= 8) {
      data.push_back(static_cast<uint8_t>(ts_word >> s));
    }
    data.insert(data.end(), pk[i].begin(), pk[i].end());
  }
  const ovt::io::MemoryFile file{std::span<const uint8_t>(data)};
  auto reader = ts::make_reader(file);
  REQUIRE(reader != nullptr);
  const auto buf = reader->tree();
  const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data());
  const auto* pkt = tree->root()->Get(0)->children()->Get(1);
  CHECK(pkt->file_span()->offset() == 192);
  CHECK(pkt->file_span()->length() == 192);
  CHECK(pkt->header_length() == 8);
  CHECK(pkt->fields()->Get(0)->uvalue() == 1);
  CHECK(std::string(pkt->fields()->Get(1)->name()->c_str()) == "arrival_time_stamp");
  CHECK(pkt->fields()->Get(1)->uvalue() == 1001);
  CHECK(pkt->fields()->Get(2)->span()->bit_offset() == 32);  // sync_byte follows the 4-byte prefix
}

TEST_CASE("reader: children are materialised in windows of 65536 packets", "[container][mpegts][fast]") {
  const size_t n = ts::kChildWindow + 100;
  Pkt p;
  const Bytes one = make_packet(p);
  Bytes data;
  data.reserve(n * 188);
  for (size_t i = 0; i < n; ++i) {
    data.insert(data.end(), one.begin(), one.end());
  }
  // The same packet repeated: every counter value repeats, so continuity errors are expected; the diagnostics
  // list must stay capped while the exact count is kept in the statistics.
  const auto f = parse_bytes(data);
  REQUIRE(f);
  CHECK(f->packet_count == n);
  CHECK(f->diagnostics.size() == ts::kMaxDiagnostics);
  CHECK(f->pids[0].continuity_errors == (n - 1) / 2);  // duplicates alternate with errors: one duplicate is allowed

  const ovt::io::MemoryFile file{std::span<const uint8_t>(data)};
  auto reader = ts::make_reader(file);
  REQUIRE(reader != nullptr);
  auto buf = reader->tree();
  const auto* tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data());
  CHECK(tree->root()->Get(0)->children()->size() == ts::kChildWindow);
  REQUIRE(tree->root()->Get(0)->diagnostics()->size() == 1);
  CHECK(std::string(tree->root()->Get(0)->diagnostics()->Get(0)->code()->c_str()) == "CNT-0007");

  CHECK(reader->expand(1) == OVT_E_INVALID_ARG);  // not a packet start
  CHECK(reader->expand(5 * kP188) == OVT_OK);     // a packet start: nothing lazy below it
  CHECK(reader->expand(n * 188) == OVT_E_INVALID_ARG);
  REQUIRE(reader->expand(0) == OVT_OK);
  buf = reader->tree();
  tree = flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data());
  CHECK(tree->root()->Get(0)->children()->size() == n);
  const size_t lazy_diagnostics = tree->root()->Get(0)->diagnostics()->size();
  CHECK(lazy_diagnostics == 0U);
  CHECK(tree->diagnostics()->size() == ts::kMaxDiagnostics);
}

TEST_CASE("ovt_container_open: a transport stream gets the MPEG-TS reader", "[container][mpegts][open][fast]") {
  const Bytes data = concat(clean_packets(12));
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(std::span<const uint8_t>(data))};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  REQUIRE(c != nullptr);
  CHECK(std::string(ovt_container_format(c)) == "mpegts");
  const ovt_Buffer tree = ovt_container_tree(c);
  REQUIRE(tree.data != nullptr);
  const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data);
  REQUIRE(t->root()->size() == 1);
  CHECK(t->root()->Get(0)->children()->size() == 12);
  ovt_container_close(c);
}

TEST_CASE("corpus: tsduck-test input walks to the reference PID statistics",
          "[container][mpegts][needs-corpus][fast]") {
  // Reference: an independent brute-force count of the file (188-byte packets, header PID field), recorded here
  // because tsanalyze is not available on CI; see docs/acceptance/wi/WI-4.204.md.
  const auto path = ovt::test::corpus_or_skip("ts_h264_dvb");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto f = ts::parse(*file);
  REQUIRE(f);
  CHECK(f->geometry.stride == 188);
  CHECK(f->packet_count == 1987);
  // No sync, continuity or transport errors; the only diagnostics are the CNT-TS-1007 of WI-4.205 for the five
  // PMT streams that have no packets in this excerpt (0x424..0x427, 0x42B).
  CHECK(std::all_of(f->diagnostics.begin(), f->diagnostics.end(),
                    [](const ts::Diagnostic& d) { return d.code == DiagCode::CNT_TS_1007; }));
  CHECK(f->diagnostics.size() == 5);
  REQUIRE(f->pids.size() == 3);
  CHECK(f->pids[0].pid == 0);
  CHECK(f->pids[0].packets == 78);
  CHECK(f->pids[1].pid == 160);
  CHECK(f->pids[1].packets == 77);
  CHECK(f->pids[2].pid == 1068);
  CHECK(f->pids[2].packets == 1832);
}
