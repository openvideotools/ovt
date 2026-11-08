// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the raw AV1 OBU stream reader (chapter 08 §8.6.4, WI-1.227).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "container/format.hpp"
#include "container/obu.hpp"
#include "test_support/corpus.h"

namespace {

using namespace ovt::container;
using Bytes = std::vector<uint8_t>;

// One OBU in Section 5 form (obu_has_size_field = 1 unless `sized` is false).
Bytes obu(uint8_t type, const Bytes& payload, bool sized = true, bool ext = false) {
  Bytes b;
  b.push_back(static_cast<uint8_t>((type << 3U) | (ext ? 4U : 0U) | (sized ? 2U : 0U)));
  if (ext) {
    b.push_back(0x28);  // temporal_id 1, spatial_id 1
  }
  if (sized) {
    append_leb128(b, payload.size());
  }
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

Bytes with_len(const Bytes& content) {
  Bytes b;
  append_leb128(b, content.size());
  b.insert(b.end(), content.begin(), content.end());
  return b;
}

const Bytes kPayload10(10, 0x5A);
const Bytes kTd = obu(kObuTemporalDelimiter, {});

// A Section 5 stream of `tus` temporal units, each TD + sequence header (first only) + frame.
Bytes section5_stream(int tus) {
  Bytes s;
  for (int i = 0; i < tus; ++i) {
    s = cat({s, kTd});
    if (i == 0) {
      s = cat({s, obu(kObuSequenceHeader, kPayload10)});
    }
    s = cat({s, obu(6, Bytes(20 + static_cast<size_t>(i), 0x33))});
  }
  return s;
}

// The same temporal units in Annex B: one frame unit per temporal unit, OBUs without size fields.
Bytes annexb_stream(int tus) {
  Bytes s;
  for (int i = 0; i < tus; ++i) {
    Bytes fu = with_len(obu(kObuTemporalDelimiter, {}, false));
    if (i == 0) {
      fu = cat({fu, with_len(obu(kObuSequenceHeader, kPayload10, false))});
    }
    fu = cat({fu, with_len(obu(6, Bytes(20 + static_cast<size_t>(i), 0x33), false))});
    s = cat({s, with_len(with_len(fu))});
  }
  return s;
}

struct Walk {
  std::vector<ObuNode> obus;
  std::vector<FrameUnitNode> fus;
  std::vector<TemporalUnitNode> tus;
  std::vector<Finding> findings;
  ObuWalkSummary summary;
};

Walk walk(const Bytes& d, ObuFraming f) {
  Walk w;
  ObuVisitor v;
  v.on_obu = [&](const ObuNode& n) { w.obus.push_back(n); };
  v.on_frame_unit = [&](const FrameUnitNode& n) { w.fus.push_back(n); };
  v.on_temporal_unit = [&](const TemporalUnitNode& n) { w.tus.push_back(n); };
  w.summary = obu_walk(d, 0, f, v, [&](const Finding& x) { w.findings.push_back(x); });
  return w;
}

bool has(const Walk& w, DiagCode c) {
  return std::any_of(w.findings.begin(), w.findings.end(), [c](const Finding& f) { return f.code == c; });
}

}  // namespace

TEST_CASE("leb128 decodes up to 8 bytes and reports truncation", "[container][obu][fast]") {
  CHECK(read_leb128(Bytes{0x05}).value == 5);
  const auto two = read_leb128(Bytes{0xE5, 0x8E, 0x26});
  CHECK(two.status == LebStatus::Ok);
  CHECK(two.value == 624485);
  CHECK(two.bytes == 3);
  CHECK(read_leb128(Bytes{0x80}).status == LebStatus::Truncated);
  CHECK(read_leb128(Bytes{}).status == LebStatus::Truncated);
  CHECK(read_leb128(Bytes(8, 0x80)).status == LebStatus::TooLong);
  CHECK(read_leb128(Bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}).value == uint64_t{1} << 49U);
  for (const uint64_t v : {uint64_t{0}, uint64_t{127}, uint64_t{128}, uint64_t{1} << 40U}) {
    Bytes b;
    append_leb128(b, v);
    CHECK(read_leb128(b).value == v);
  }
}

TEST_CASE("obu header parsing", "[container][obu][fast]") {
  const auto h = parse_obu_header(Bytes{0x32});  // OBU_FRAME (6), has_size_field
  CHECK(h.valid);
  CHECK(h.obu_type == 6);
  CHECK(h.has_size_field);
  CHECK(h.header_bytes == 1);
  const auto e = parse_obu_header(Bytes{0x36, 0x68});  // + extension: temporal_id 3, spatial_id 1
  CHECK(e.valid);
  CHECK(e.header_bytes == 2);
  CHECK(e.temporal_id == 3);
  CHECK(e.spatial_id == 1);
  CHECK_FALSE(parse_obu_header(Bytes{0x36}).valid);  // extension byte missing
  CHECK_FALSE(parse_obu_header(Bytes{0x80}).valid);  // forbidden bit
  CHECK_FALSE(parse_obu_header(Bytes{0x01}).valid);  // reserved bit
  CHECK_FALSE(parse_obu_header(Bytes{0x48}).valid);  // obu_type 9 is reserved
}

TEST_CASE("obu_detect tells Section 5 from Annex B", "[container][obu][fast]") {
  const Bytes s5 = section5_stream(3);
  const Bytes ab = annexb_stream(3);
  CHECK(obu_detect(s5, s5.size()).framing == ObuFraming::Section5);
  CHECK(obu_detect(ab, ab.size()).framing == ObuFraming::AnnexB);
  CHECK(obu_detect(Bytes{}, 0).framing == ObuFraming::NotAv1);
  CHECK(obu_detect(Bytes(100, 0xFF), 100).framing == ObuFraming::NotAv1);
  // A stream that starts with a sequence header instead of a delimiter is fine.
  const Bytes seq_first = cat({obu(kObuSequenceHeader, kPayload10), obu(6, Bytes(5, 1))});
  CHECK(obu_detect(seq_first, seq_first.size()).framing == ObuFraming::Section5);
  // The first OBU must be a delimiter or a sequence header.
  const Bytes frame_first = cat({obu(6, Bytes(5, 1)), obu(6, Bytes(5, 1))});
  CHECK(obu_detect(frame_first, frame_first.size()).framing == ObuFraming::NotAv1);
  // A size that reaches past the end of the file fails; a window that ends inside an OBU passes.
  const Bytes cut(s5.begin(), s5.begin() + 20);
  CHECK(obu_detect(cut, s5.size()).framing == ObuFraming::Section5);
  CHECK(obu_detect(cut, cut.size()).framing == ObuFraming::NotAv1);
}

TEST_CASE("obu_detect rejects a Section 5 stream without obu_has_size_field", "[container][obu][fast]") {
  const Bytes d = cat({obu(kObuTemporalDelimiter, {}, false), obu(kObuSequenceHeader, kPayload10, false)});
  const auto r = obu_detect(d, d.size());
  CHECK(r.framing == ObuFraming::NotAv1);
  CHECK(r.missing_size_field);
  CHECK(has(walk(d, ObuFraming::Section5), DiagCode::CNT_OBU_0001));
}

TEST_CASE("obu_detect: both framings parse, the first-byte rule decides", "[container][obu][fast]") {
  // Section 5: sequence header (0A 04, 4 payload bytes), two temporal delimiters (12 00); Annex B: one temporal
  // unit of 10 bytes holding a frame unit of 4 bytes holding a 1-byte temporal delimiter (obu_length 1).
  const Bytes d = {0x0A, 0x04, 0x01, 0x10, 0x00, 0x00, 0x12, 0x00, 0x12, 0x00, 0x00};
  const auto r = obu_detect(d, d.size());
  CHECK(r.both_framings);
  CHECK(r.framing == ObuFraming::AnnexB);  // byte 0 is not 0x12
  const Bytes plain = section5_stream(3);
  CHECK_FALSE(obu_detect(plain, plain.size()).both_framings);
}

TEST_CASE("section 5 walk: one temporal unit per OBU_TEMPORAL_DELIMITER", "[container][obu][fast]") {
  const Bytes s = section5_stream(4);
  const auto w = walk(s, ObuFraming::Section5);
  CHECK(w.findings.empty());
  CHECK(w.summary.temporal_unit_count == 4);
  CHECK(w.tus.size() == 4);
  CHECK(static_cast<size_t>(std::count_if(w.obus.begin(), w.obus.end(), [](const ObuNode& n) {
          return n.header.obu_type == kObuTemporalDelimiter;
        })) == w.tus.size());
  CHECK(w.obus.size() == 4 + 1 + 4);
  // Units are contiguous, cover the file and start at their delimiter.
  uint64_t pos = 0;
  for (const auto& tu : w.tus) {
    CHECK(tu.file_span.offset == pos);
    CHECK(s[tu.file_span.offset] == 0x12);
    pos = tu.file_span.end();
  }
  CHECK(pos == s.size());
  CHECK(w.tus[0].obu_count == 3);
  CHECK(w.tus[1].obu_count == 2);
  CHECK(w.obus[1].obu_size == 10);
  CHECK(w.obus[1].file_span.length == 12);
  CHECK(w.obus[0].prefix_span.length == 0);
}

TEST_CASE("section 5 walk: a stream that starts without a delimiter puts its first OBUs in unit 0",
          "[container][obu][fast]") {
  const Bytes s = cat({obu(kObuSequenceHeader, kPayload10), obu(6, Bytes(5, 1)), kTd, obu(6, Bytes(5, 2))});
  const auto w = walk(s, ObuFraming::Section5);
  REQUIRE(w.tus.size() == 2);
  CHECK(w.tus[0].obu_count == 2);
  CHECK(w.tus[1].obu_count == 2);
}

TEST_CASE("section 5 walk: extension header and clamped or malformed OBUs", "[container][obu][fast]") {
  const Bytes ext = cat({kTd, obu(6, Bytes(4, 7), true, true)});
  const auto w = walk(ext, ObuFraming::Section5);
  REQUIRE(w.obus.size() == 2);
  CHECK(w.obus[1].header.extension_flag);
  CHECK(w.obus[1].header.temporal_id == 1);
  CHECK(w.obus[1].file_span.length == 2 + 1 + 4);

  Bytes cut = cat({kTd, obu(6, Bytes(50, 7))});
  cut.resize(cut.size() - 10);
  const auto c = walk(cut, ObuFraming::Section5);
  CHECK(has(c, DiagCode::CNT_OBU_0003));
  REQUIRE(c.obus.size() == 2);
  CHECK(c.obus[1].file_span.end() == cut.size());
  CHECK(c.tus.size() == 1);

  const Bytes bad = cat({kTd, Bytes{0x80, 0x00}});  // forbidden bit
  const auto b = walk(bad, ObuFraming::Section5);
  CHECK(has(b, DiagCode::CNT_OBU_0004));
  CHECK(b.obus.size() == 1);
}

TEST_CASE("annex b walk: temporal units, frame units, prefix spans", "[container][obu][fast]") {
  const Bytes s = annexb_stream(3);
  const auto w = walk(s, ObuFraming::AnnexB);
  CHECK(w.findings.empty());
  REQUIRE(w.tus.size() == 3);
  REQUIRE(w.fus.size() == 3);
  CHECK(w.obus.size() == 3 + 1 + 3);
  CHECK(w.tus[0].prefix_span.offset == 0);
  CHECK(w.tus[0].prefix_span.length == 1);
  CHECK(w.tus[0].file_span.offset == 1);
  CHECK(w.tus[0].obu_count == 3);
  CHECK(w.tus[0].frame_unit_count == 1);
  CHECK(w.tus[2].file_span.end() == s.size());
  // The first OBU is the temporal delimiter: obu_length 1 at offset 3 (after temporal_unit_size, frame_unit_size).
  CHECK(w.obus[0].prefix_span.offset == 2);
  CHECK(w.obus[0].prefix_span.length == 1);
  CHECK(w.obus[0].obu_length == 1);
  CHECK(w.obus[0].file_span.offset == 3);
  CHECK(w.obus[0].header.obu_type == kObuTemporalDelimiter);
  CHECK_FALSE(w.obus[0].header.has_size_field);
  CHECK(w.obus[1].obu_size == 10);
  CHECK(w.obus[1].frame_unit_index == 0);
}

TEST_CASE("annex b walk: overruns are clamped, bad lengths skip the unit", "[container][obu][fast]") {
  Bytes s = annexb_stream(2);
  s.resize(s.size() - 5);
  const auto w = walk(s, ObuFraming::AnnexB);
  CHECK(has(w, DiagCode::CNT_OBU_0003));
  CHECK(w.tus.size() == 2);
  CHECK(w.tus[1].file_span.end() == s.size());

  // obu_length 0 and an invalid OBU header are reported; the next OBU is still found.
  const Bytes odd = with_len(with_len(cat({with_len(Bytes{}), with_len(Bytes{0x80}), with_len(obu(2, {}, false))})));
  const auto o = walk(odd, ObuFraming::AnnexB);
  CHECK(has(o, DiagCode::CNT_OBU_0004));
  REQUIRE(o.obus.size() == 1);
  CHECK(o.obus[0].header.obu_type == kObuTemporalDelimiter);

  const Bytes bad_leb = Bytes(9, 0xFF);
  const auto b = walk(bad_leb, ObuFraming::AnnexB);
  CHECK(has(b, DiagCode::CNT_OBU_0004));
  CHECK(b.tus.empty());
}

TEST_CASE("reframing an Annex B OBU gives a Section 5 OBU", "[container][obu][fast]") {
  Bytes out;
  const Bytes seq = obu(kObuSequenceHeader, Bytes(200, 9), false);  // obu_length 201: leb128 size needs 2 bytes
  REQUIRE(reframe_annexb_obu(seq, out));
  CHECK((out[0] & 0x02U) != 0);
  const auto leb = read_leb128(Bytes(out.begin() + 1, out.end()));
  CHECK(leb.value == seq.size() - 1);
  CHECK(leb.bytes == 2);
  CHECK(out.size() == 1 + 2 + 200);
  CHECK(out[0] == (seq[0] | 0x02U));

  const Bytes ext = obu(6, Bytes(5, 3), false, true);
  REQUIRE(reframe_annexb_obu(ext, out));
  CHECK(out[1] == ext[1]);
  CHECK(out[2] == 5);
  CHECK(out.size() == ext.size() + 1);

  const Bytes sized = obu(6, Bytes(5, 3));
  REQUIRE(reframe_annexb_obu(sized, out));
  CHECK(out == sized);

  Bytes wrong = sized;
  wrong.pop_back();  // obu_size now overruns
  CHECK_FALSE(reframe_annexb_obu(wrong, out));
  CHECK(out.empty());
  CHECK_FALSE(reframe_annexb_obu(Bytes{}, out));
  CHECK_FALSE(reframe_annexb_obu(Bytes{0x80}, out));

  // The reframed OBUs of a whole Annex B stream form a valid Section 5 stream with the same OBU sequence.
  const Bytes s = annexb_stream(3);
  const auto w = walk(s, ObuFraming::AnnexB);
  Bytes joined;
  for (const auto& n : w.obus) {
    REQUIRE(reframe_annexb_obu(Bytes(s.begin() + static_cast<std::ptrdiff_t>(n.file_span.offset),
                                     s.begin() + static_cast<std::ptrdiff_t>(n.file_span.end())),
                               out));
    joined.insert(joined.end(), out.begin(), out.end());
  }
  const auto back = walk(joined, ObuFraming::Section5);
  CHECK(back.findings.empty());
  CHECK(back.tus.size() == 3);
  REQUIRE(back.obus.size() == w.obus.size());
  for (size_t i = 0; i < back.obus.size(); ++i) {
    CHECK(back.obus[i].header.obu_type == w.obus[i].header.obu_type);
    CHECK(back.obus[i].obu_size == w.obus[i].obu_size);
  }
}

TEST_CASE("sniff and obu_detect agree on the raw OBU corpus files", "[container][obu][needs-corpus][fast]") {
  struct Case {
    const char* id;
    ObuFraming framing;
  };
  for (const Case c : {Case{"syn_av1_lowoverhead", ObuFraming::Section5}, Case{"syn_av1_annexb", ObuFraming::AnnexB}}) {
    DYNAMIC_SECTION(c.id) {
      const auto path = ovt::test::corpus_or_skip(c.id);
      const auto file = ovt::io::RandomAccessFile::open(path.string());
      CHECK(ovt::container::sniff(*file) == Format::Obu);
      Bytes head(std::min<uint64_t>(file->size(), 65536));
      REQUIRE(file->read(0, head.size(), head.data()) == head.size());
      CHECK(obu_detect(head, file->size()).framing == c.framing);

      std::vector<ObuNode> obus;
      std::vector<Finding> findings;
      ObuVisitor v;
      v.on_obu = [&](const ObuNode& n) { obus.push_back(n); };
      const auto summary = obu_walk(*file, c.framing, v, [&](const Finding& f) { findings.push_back(f); });
      CHECK(findings.empty());
      const auto delimiters = static_cast<uint64_t>(std::count_if(
          obus.begin(), obus.end(), [](const ObuNode& n) { return n.header.obu_type == kObuTemporalDelimiter; }));
      CHECK(summary.temporal_unit_count == delimiters);
      CHECK(summary.temporal_unit_count > 0);
    }
  }
}
