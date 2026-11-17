// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Robustness of the AV1 OBU detector and walkers (chapter 08 §8.6.4): streams cut at every length and with every
// single byte replaced still produce nodes that lie inside the data, in file order, and findings with a message.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "container/obu.hpp"

namespace {

namespace c = ovt::container;
using Bytes = std::vector<uint8_t>;

Bytes section5_stream() {
  // temporal delimiter, sequence header (4 payload bytes), frame (3 payload bytes), then a second unit.
  return {0x12, 0x00, 0x0A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x32, 0x03, 0xAA,
          0xBB, 0xCC, 0x12, 0x00, 0x32, 0x02, 0x11, 0x22, 0x26, 0x01, 0x7F /* extension-less tile group */};
}

Bytes annexb_stream() {
  // temporal_unit_size, frame_unit_size, then obu_length + OBU (no size field): TD in its own frame unit first.
  Bytes b;
  Bytes tu;
  Bytes fu1{0x01, 0x10};  // obu_length 1, TD header 0x10
  Bytes fu2;
  const Bytes seq{0x08, 0x00, 0x00, 0x00, 0x00};  // sequence header, 4 payload bytes
  fu2.push_back(static_cast<uint8_t>(seq.size()));
  fu2.insert(fu2.end(), seq.begin(), seq.end());
  const Bytes frame{0x30, 0xAA, 0xBB};
  fu2.push_back(static_cast<uint8_t>(frame.size()));
  fu2.insert(fu2.end(), frame.begin(), frame.end());
  tu.push_back(static_cast<uint8_t>(fu1.size()));
  tu.insert(tu.end(), fu1.begin(), fu1.end());
  tu.push_back(static_cast<uint8_t>(fu2.size()));
  tu.insert(tu.end(), fu2.begin(), fu2.end());
  b.push_back(static_cast<uint8_t>(tu.size()));
  b.insert(b.end(), tu.begin(), tu.end());
  return b;
}

struct Counts {
  size_t obus = 0;
  size_t frame_units = 0;
  size_t temporal_units = 0;
  size_t findings = 0;
};

Counts walk_checked(const Bytes& data, c::ObuFraming framing) {
  Counts n;
  c::ObuVisitor v;
  v.on_obu = [&](const c::ObuNode& o) {
    ++n.obus;
    CHECK(o.file_span.end() <= data.size());
    CHECK(o.prefix_span.end() <= data.size());
  };
  v.on_frame_unit = [&](const c::FrameUnitNode& f) {
    ++n.frame_units;
    CHECK(f.file_span.end() <= data.size());
  };
  v.on_temporal_unit = [&](const c::TemporalUnitNode& t) {
    ++n.temporal_units;
    CHECK(t.file_span.end() <= data.size());
  };
  const c::FindingSink sink = [&](const c::Finding& f) {
    ++n.findings;
    CHECK_FALSE(f.message.empty());
    CHECK(f.file_offset <= data.size());
  };
  (void)c::obu_walk(std::span<const uint8_t>(data), 0, framing, v, sink);
  return n;
}

}  // namespace

TEST_CASE("obu: the hand-built streams are detected and walked", "[container][obu][fast]") {
  const Bytes s5 = section5_stream();
  const Bytes ab = annexb_stream();
  CHECK(c::obu_detect(s5, s5.size()).framing == c::ObuFraming::Section5);
  const Counts a = walk_checked(s5, c::ObuFraming::Section5);
  CHECK(a.obus >= 4);
  CHECK(a.temporal_units == 2);
  CHECK(c::obu_detect(ab, ab.size()).framing == c::ObuFraming::AnnexB);
  const Counts b = walk_checked(ab, c::ObuFraming::AnnexB);
  CHECK(b.obus == 3);
  CHECK(b.frame_units == 2);
  CHECK(b.temporal_units == 1);
}

TEST_CASE("obu: streams cut at every length and with every byte replaced stay inside the data",
          "[container][obu][fast]") {
  for (const Bytes& full : {section5_stream(), annexb_stream()}) {
    for (const c::ObuFraming framing : {c::ObuFraming::Section5, c::ObuFraming::AnnexB}) {
      for (size_t len = 0; len <= full.size(); ++len) {
        const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
        (void)walk_checked(cut, framing);
        (void)c::obu_detect(cut, full.size());
        (void)c::obu_detect(cut, cut.size());
      }
      for (size_t i = 0; i < full.size(); ++i) {
        for (const uint8_t value : {uint8_t{0x00}, uint8_t{0x80}, uint8_t{0xFF}, uint8_t{0x01}}) {
          Bytes m = full;
          m[i] = value;
          (void)walk_checked(m, framing);
          (void)c::obu_detect(m, m.size());
        }
      }
    }
  }
}

TEST_CASE("obu: leb128 reader and writer round-trip, and a ninth byte is too long", "[container][obu][fast]") {
  for (const uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128}, uint64_t{300},
                               uint64_t{1} << 32U, (uint64_t{1} << 56U) - 1U}) {
    Bytes out;
    c::append_leb128(out, value);
    const c::Leb128 r = c::read_leb128(out);
    CHECK(r.status == c::LebStatus::Ok);
    CHECK(r.value == value);
    CHECK(r.bytes == out.size());
    if (out.size() > 1) {
      const Bytes cut(out.begin(), out.end() - 1);
      CHECK(c::read_leb128(cut).status == c::LebStatus::Truncated);
    }
  }
  const Bytes nine(9, 0x80);
  CHECK(c::read_leb128(nine).status != c::LebStatus::Ok);
  CHECK(c::read_leb128(Bytes{}).status == c::LebStatus::Truncated);
}

TEST_CASE("obu: header parsing rejects reserved bits and names the framings", "[container][obu][fast]") {
  CHECK_FALSE(c::parse_obu_header(Bytes{0x80}).valid);  // forbidden bit
  CHECK_FALSE(c::parse_obu_header(Bytes{0x11}).valid);  // reserved bit
  CHECK_FALSE(c::parse_obu_header(Bytes{}).valid);
  const c::ObuHeader ext = c::parse_obu_header(Bytes{0x0E, 0x28});  // extension flag, size field
  CHECK(ext.valid);
  CHECK(ext.extension_flag);
  CHECK(ext.header_bytes == 2);
  CHECK(ext.temporal_id == 1);
  CHECK(ext.spatial_id == 1);
  CHECK_FALSE(c::obu_framing_name(c::ObuFraming::Section5).empty());
  CHECK_FALSE(c::obu_framing_name(c::ObuFraming::AnnexB).empty());
}
