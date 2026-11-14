// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Oracle tests of the Annex B splitter and the EPB mapping (WI-1.570, chapter 08 §8.6.3): unit counts, types and
// access-unit sizes equal `ffprobe -show_packets` + `trace_headers` (goldens `tests/golden/<codec>/<id>.ovt-units.json`
// written by tools/goldens/gen_units.py), and raw.size - rbsp.size == epb_positions.size() for every NAL unit.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "container/annexb.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "test_support/corpus.h"
#include "units_oracle.hpp"

namespace {

using ovt::container::NalSpan;
using ovt::test::units::Golden;
using Bytes = std::vector<uint8_t>;

struct Unit {
  uint64_t start_code_pos = 0;  // offset of the 00 00 01 that starts the NAL unit
  uint64_t offset = 0;
  uint64_t length = 0;
  uint32_t type = 0;
};

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<Unit> split(const Bytes& data, bool h265) {
  std::vector<Unit> out;
  ovt::container::annexb_split(data, 0, [&](const NalSpan& n) {
    out.push_back({n.prefix_span.end() - 3, n.file_span.offset, n.file_span.length,
                   h265 ? (n.header_byte >> 1U) & 0x3FU : n.header_byte & 0x1FU});
  });
  return out;
}

// Every ffprobe packet (an access unit) holds exactly the NAL units that start inside its byte range, in the order and
// of the types trace_headers logged; no NAL unit is left over.
void check_against_golden(const Bytes& data, const Golden& g) {
  const bool h265 = g.codec == "h265";
  const std::vector<Unit> units = split(data, h265);
  size_t next = 0;
  uint64_t expected_pos = 0;
  for (size_t k = 0; k < g.packets.size(); ++k) {
    const auto& pkt = g.packets[k];
    REQUIRE(pkt.pos.has_value());
    if (k > 0) {
      REQUIRE(*pkt.pos == expected_pos);  // the access units tile the stream
    }
    expected_pos = *pkt.pos + pkt.size;
    std::vector<uint32_t> types;
    while (next < units.size() && units[next].start_code_pos < expected_pos) {
      REQUIRE(units[next].start_code_pos >= *pkt.pos);
      REQUIRE(units[next].offset + units[next].length <= expected_pos);
      types.push_back(units[next].type);
      ++next;
    }
    REQUIRE(pkt.types.has_value());
    INFO("access unit " << k << " at " << *pkt.pos << ", " << pkt.size << " bytes");
    REQUIRE(types == *pkt.types);
  }
  CHECK(next == units.size());
  CHECK(expected_pos <= data.size());
}

// Acceptance test 2: for every NAL unit, the raw bytes are the RBSP plus the emulation-prevention bytes, and every
// removed byte is a 03 behind two zeros.
size_t check_epb_invariant(const Bytes& data, bool h265) {
  size_t total_epbs = 0;
  for (const Unit& u : split(data, h265)) {
    const std::span<const uint8_t> raw(data.data() + u.offset, u.length);
    const ovt::Rbsp r = ovt::rbsp_from_nal(raw);
    REQUIRE(raw.size() - r.rbsp.size() == r.epb_positions.size());
    for (const uint32_t p : r.epb_positions) {
      REQUIRE(p >= 2);
      REQUIRE(raw[p] == 0x03);
      REQUIRE(raw[p - 1] == 0x00);
      REQUIRE(raw[p - 2] == 0x00);
    }
    total_epbs += r.epb_positions.size();
  }
  return total_epbs;
}

}  // namespace

TEST_CASE("units annexb: corpus access units equal the ffprobe goldens",
          "[container][annexb][golden][needs-corpus][fast]") {
  size_t compared = 0;
  for (const auto& g : ovt::test::units::committed_goldens("annexb")) {
    if (g.codec != "h264" && g.codec != "h265") {
      continue;
    }
    const auto path = ovt::test::corpus_path(g.id);
    if (!path) {
      continue;
    }
    DYNAMIC_SECTION(g.id) {
      check_against_golden(read_file(*path), g);
      ++compared;
    }
  }
  if (compared == 0) {
    if (ovt::test::require_corpora()) {
      FAIL("no Annex B corpus file has a units golden");
    }
    SKIP("no Annex B corpus file with a units golden is available");
  }
}

TEST_CASE("units annexb: raw minus RBSP equals the number of EPBs in every corpus unit",
          "[container][annexb][epb][needs-corpus][fast]") {
  size_t checked = 0;
  for (const auto& g : ovt::test::units::committed_goldens("annexb")) {
    const auto path = ovt::test::corpus_path(g.id);
    if (!path || (g.codec != "h264" && g.codec != "h265")) {
      continue;
    }
    DYNAMIC_SECTION(g.id) {
      check_epb_invariant(read_file(*path), g.codec == "h265");
      ++checked;
    }
  }
  if (checked == 0) {
    SKIP("no Annex B corpus file with a units golden is available");
  }
}

TEST_CASE("units annexb: EPB invariant on a hand-built stream", "[container][annexb][epb][fast]") {
  // NAL units whose payload is zeros and 00..03 bytes, so that the encoder-side rule inserts many EPBs.
  std::mt19937 rng(1570);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seed
  Bytes stream;
  size_t inserted = 0;
  for (int n = 0; n < 40; ++n) {
    Bytes rbsp = {0x65};
    for (int k = 0; k < 200; ++k) {
      rbsp.push_back(static_cast<uint8_t>(rng() % 5 < 3 ? 0 : rng() % 4));
    }
    rbsp.push_back(0x80);
    stream.insert(stream.end(), {0x00, 0x00, 0x00, 0x01});
    unsigned zeros = 0;
    for (const uint8_t b : rbsp) {
      if (zeros >= 2 && b <= 3) {
        stream.push_back(0x03);
        ++inserted;
        zeros = 0;
      }
      stream.push_back(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
  }
  REQUIRE(inserted > 100);
  CHECK(check_epb_invariant(stream, false) == inserted);
  CHECK(split(stream, false).size() == 40);
}

TEST_CASE("units annexb: access units of an FFmpeg-encoded stream equal ffprobe",
          "[container][annexb][ffprobe][fast]") {
  using ovt::test::units::MadeStream;
  struct Case {
    const char* codec;
    const char* file;
    const char* encode;
  };
  const std::array<Case, 2> cases = {{
      {"h264", "units.264", "-frames:v 24 -c:v libx264 -x264-params keyint=8:bframes=2:slices=2"},
      {"h265", "units.265", "-frames:v 24 -c:v libx265 -x265-params keyint=8:bframes=2:log-level=none"},
  }};
  size_t compared = 0;
  for (const Case& c : cases) {
    DYNAMIC_SECTION(c.codec) {
      MadeStream stream(c.file, ovt::test::units::kTestSource, c.encode);
      if (!stream.ok()) {
        continue;
      }
      const auto golden = ovt::test::units::generate(stream.path(), c.codec, "annexb");
      if (!golden) {
        continue;
      }
      const Bytes data = read_file(stream.path());
      check_against_golden(data, *golden);
      ++compared;
    }
  }
  if (compared == 0) {
    SKIP("ffmpeg, ffprobe or python3 is not available");
  }
}
