// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Bounds of the transport packet header and adaptation field decoder, and of the whole-file parser on damaged
// streams (chapter 08 §8.6.7, ISO/IEC 13818-1 §2.4.3): every adaptation field length and flag combination decodes
// inside the packet, and parse() accepts files whose packets were damaged at every byte of the first packets.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "container/mpegts/ts_packets.hpp"
#include "io/memory_file.hpp"

namespace {

namespace ts = ovt::container::mpegts;
using Bytes = std::vector<uint8_t>;

Bytes packet(uint16_t pid, uint8_t afc, uint8_t cc, const Bytes& af_body, int af_length = -1) {
  Bytes p(188, 0x55);
  p[0] = ts::kSyncByte;
  p[1] = static_cast<uint8_t>(0x40U | (pid >> 8U));
  p[2] = static_cast<uint8_t>(pid & 0xFFU);
  p[3] = static_cast<uint8_t>((static_cast<unsigned>(afc) << 4U) | (cc & 0x0FU));
  if ((afc & 2U) != 0) {
    p[4] = static_cast<uint8_t>(af_length < 0 ? static_cast<int>(af_body.size()) : af_length);
    for (size_t i = 0; i < af_body.size() && 5 + i < p.size(); ++i) {
      p[5 + i] = af_body[i];
    }
  }
  return p;
}

}  // namespace

TEST_CASE("ts packets: adaptation field flags and lengths decode inside the packet", "[container][mpegts][fast]") {
  for (unsigned afc = 0; afc < 4; ++afc) {
    for (unsigned flags = 0; flags < 256; ++flags) {
      for (const unsigned length : {0U, 1U, 2U, 7U, 8U, 13U, 40U, 182U, 183U, 184U, 255U}) {
        Bytes body(std::min<size_t>(length, 183), 0x21);
        if (!body.empty()) {
          body[0] = static_cast<uint8_t>(flags);
        }
        const Bytes p = packet(0x100, static_cast<uint8_t>(afc), 3, body, static_cast<int>(length));
        INFO("afc " << afc << " flags " << flags << " length " << length);
        const auto h = ts::decode_packet(p);
        REQUIRE(h.has_value());
        CHECK(h->pid == 0x100);
        CHECK(h->continuity_counter == 3);
        CHECK(h->payload_offset <= 188);
        CHECK(uint64_t{h->payload_offset} + h->payload_size <= 188);
        if (h->adaptation) {
          CHECK(h->adaptation->length == length);
        }
        if ((afc & 1U) == 0) {
          CHECK(h->payload_size == 0);
        }
      }
    }
  }
}

TEST_CASE("ts packets: a packet without the sync byte or too short is not decoded", "[container][mpegts][fast]") {
  Bytes p = packet(0x100, 1, 0, {});
  p[0] = 0x46;
  CHECK_FALSE(ts::decode_packet(p).has_value());
  CHECK_FALSE(ts::decode_packet(Bytes(3, ts::kSyncByte)).has_value());
  CHECK_FALSE(ts::decode_packet(Bytes{}).has_value());
}

TEST_CASE("ts packets: parse survives damage to every byte of the first packets", "[container][mpegts][fast]") {
  Bytes clean;
  for (unsigned i = 0; i < 12; ++i) {
    const Bytes pcr{0x10, 0x00, 0x00, 0x00, 0x01, 0x7E, 0x00};
    const Bytes p = packet(i % 3 == 0 ? 0x100 : 0x101, i % 3 == 0 ? 3 : 1, static_cast<uint8_t>(i), pcr);
    clean.insert(clean.end(), p.begin(), p.end());
  }
  {
    const ovt::io::MemoryFile f(std::span<const uint8_t>(clean.data(), clean.size()));
    const auto parsed = ts::parse(f);
    REQUIRE(parsed.has_value());
    CHECK(parsed->packet_count == 12);
    CHECK(parsed->pids.size() == 2);
  }
  for (size_t i = 0; i < size_t{4} * 188; ++i) {
    for (const uint8_t value : {uint8_t{0x00}, uint8_t{0xFF}, uint8_t{0x47}, uint8_t{0x80}}) {
      Bytes d = clean;
      d[i] = value;
      const ovt::io::MemoryFile f(std::span<const uint8_t>(d.data(), d.size()));
      const auto parsed = ts::parse(f);
      if (parsed) {
        CHECK(parsed->packet_count <= 13);
        for (const ts::Diagnostic& diag : parsed->diagnostics) {
          CHECK(diag.offset <= d.size());
        }
      }
    }
  }
  for (size_t len = 0; len <= clean.size(); len += 37) {
    const ovt::io::MemoryFile f(std::span<const uint8_t>(clean.data(), len));
    (void)ts::parse(f);
  }
}
