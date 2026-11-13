// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.231: the unit splitter of chapter 08 §8.7.4. Acceptance test 1 runs on samples of the
// syn_h264_multislice_cavlc_mp4 corpus extracted by tools/extract_samples.py (CTest unit_split_corpus, which needs
// the extraction step unit_split_extract_samples); tests 2 and 3 and the rest use hand-built samples.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "container/obu.hpp"
#include "demux/unit_split.h"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

using ovt::demux::Framing;
using ovt::demux::SplitResult;
using ovt::demux::UnitSpan;
using Bytes = std::vector<uint8_t>;

struct Split {
  SplitResult result;
  Bytes scratch;
};

Split split(const Bytes& sample, Framing framing, uint8_t nal_length_size = 4, bool td_expected = false) {
  Split s;
  s.result = ovt::demux::split_units(sample, framing, nal_length_size, s.scratch, td_expected);
  return s;
}

Bytes unit(const Bytes& sample, const Split& s, std::size_t i) {
  const auto bytes = ovt::demux::unit_bytes(sample, s.result, s.scratch, i);
  return {bytes.begin(), bytes.end()};
}

int count_code(const SplitResult& r, const std::string& code, ovt::fb::Severity severity) {
  int n = 0;
  for (const auto& d : r.diagnostics) {
    n += d.code == code && d.severity == severity ? 1 : 0;
  }
  return n;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

}  // namespace

TEST_CASE("unit_split: length-prefixed samples with 1-, 2- and 4-byte lengths", "[demux][unit_split][fast]") {
  const Bytes nal_a{0x67, 0x42, 0x00, 0x1e};
  const Bytes nal_b{0x68, 0xce};
  const Bytes nal_c{0x65, 0x88, 0x84, 0x00, 0x33};
  for (const uint8_t length_size : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
    INFO("nal_length_size " << int{length_size});
    Bytes sample;
    for (const auto* nal : {&nal_a, &nal_b, &nal_c}) {
      for (int k = length_size - 1; k >= 0; --k) {
        sample.push_back(static_cast<uint8_t>(nal->size() >> (8U * static_cast<unsigned>(k))));
      }
      sample.insert(sample.end(), nal->begin(), nal->end());
    }
    const Split s = split(sample, Framing::LengthPrefixed, length_size);
    REQUIRE(s.result.units.size() == 3);
    CHECK(s.result.diagnostics.empty());
    CHECK_FALSE(s.result.rewritten);
    CHECK(s.result.units[0] == UnitSpan{length_size, 4, length_size});
    CHECK(unit(sample, s, 0) == nal_a);  // prefix stripped, no start code added
    CHECK(unit(sample, s, 1) == nal_b);
    CHECK(unit(sample, s, 2) == nal_c);
  }
}

TEST_CASE("unit_split: AT3 a NAL length exceeding the sample raises CNT-9001 and clamps", "[demux][unit_split][fast]") {
  const Bytes sample{0x00, 0x00, 0x00, 0x02, 0x09, 0xf0,         // AUD, 2 bytes
                     0x00, 0x00, 0x01, 0x00, 0x65, 0x88, 0x84};  // claims 256, has 3
  const Split s = split(sample, Framing::LengthPrefixed, 4);
  REQUIRE(s.result.units.size() == 2);
  CHECK(s.result.units[1] == UnitSpan{10, 3, 4});
  CHECK(unit(sample, s, 1) == Bytes{0x65, 0x88, 0x84});
  CHECK(count_code(s.result, "CNT-9001", ovt::fb::Severity::Error) == 1);
  CHECK(s.result.diagnostics.size() == 1);
}

TEST_CASE("unit_split: zero-length NAL, trailing bytes and a bad length size", "[demux][unit_split][fast]") {
  SECTION("a zero-length NAL is skipped with a CNT-9001 Warning") {
    const Bytes sample{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x09};
    const Split s = split(sample, Framing::LengthPrefixed, 4);
    REQUIRE(s.result.units.size() == 1);
    CHECK(s.result.units[0] == UnitSpan{8, 1, 4});
    CHECK(count_code(s.result, "CNT-9001", ovt::fb::Severity::Warning) == 1);
  }
  SECTION("bytes shorter than a length field after the last NAL raise CNT-9001") {
    const Bytes sample{0x00, 0x01, 0x09, 0x00};
    const Split s = split(sample, Framing::LengthPrefixed, 2);
    REQUIRE(s.result.units.size() == 1);
    CHECK(count_code(s.result, "CNT-9001", ovt::fb::Severity::Error) == 1);
  }
  SECTION("nal_length_size 3 is rejected: the sample is one unit") {
    const Bytes sample{0x00, 0x00, 0x01, 0x09};
    const Split s = split(sample, Framing::LengthPrefixed, 3);
    REQUIRE(s.result.units.size() == 1);
    CHECK(s.result.units[0] == UnitSpan{0, 4, 0});
    CHECK(count_code(s.result, "CNT-9001", ovt::fb::Severity::Error) == 1);
  }
}

TEST_CASE("unit_split: Annex B inside avc1 falls back to Annex B splitting with CNT-9002",
          "[demux][unit_split][fast]") {
  const Bytes sample{0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x01, 0x68, 0xce, 0x00, 0x00, 0x01, 0x65, 0x88};
  const Split s = split(sample, Framing::LengthPrefixed, 4);
  REQUIRE(s.result.units.size() == 3);
  CHECK(s.result.units[0] == UnitSpan{4, 2, 4});
  CHECK(s.result.units[1] == UnitSpan{9, 2, 3});
  CHECK(unit(sample, s, 2) == Bytes{0x65, 0x88});
  CHECK(count_code(s.result, "CNT-9002", ovt::fb::Severity::Warning) == 1);
}

TEST_CASE("unit_split: Annex B samples use the §8.6.3 splitter", "[demux][unit_split][fast]") {
  const Bytes sample{0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaf, 0x00, 0x00};
  const Split s = split(sample, Framing::AnnexB);
  REQUIRE(s.result.units.size() == 2);
  CHECK(s.result.units[0] == UnitSpan{4, 3, 4});
  CHECK(unit(sample, s, 1) == Bytes{0x26, 0x01, 0xaf});  // trailing zeros are not part of the NAL
  CHECK(unit(sample, s, 0)[0] != 0x00);
}

TEST_CASE("unit_split: AT2 an AV1 last OBU without a size field is rewritten", "[demux][unit_split][fast]") {
  const Bytes seq_header{0x0a, 0x03, 0x00, 0x00, 0x00};  // OBU_SEQUENCE_HEADER, size field, 3 payload bytes
  const Bytes frame_no_size{0x30, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};  // OBU_FRAME, no size field, 6 bytes
  const Bytes sample = cat({seq_header, frame_no_size});
  const Split s = split(sample, Framing::Av1Obu);
  REQUIRE(s.result.units.size() == 2);
  CHECK(s.result.rewritten);
  CHECK(s.result.diagnostics.empty());
  CHECK(s.result.units[0] == UnitSpan{0, 5, 0});
  CHECK(s.result.units[1] == UnitSpan{5, 7, 0});  // still the original bytes
  CHECK(unit(sample, s, 0) == seq_header);

  const Bytes last = unit(sample, s, 1);
  const auto header = ovt::container::parse_obu_header(std::span(last).first(1));
  REQUIRE(header.valid);
  CHECK(header.has_size_field);
  CHECK(header.obu_type == 6);
  const auto size = ovt::container::read_leb128(std::span(last).subspan(1));
  REQUIRE(size.status == ovt::container::LebStatus::Ok);
  CHECK(size.value == 6);
  CHECK(last.size() == std::size_t{1} + size.bytes + 6U);
  CHECK(Bytes(last.begin() + 1 + size.bytes, last.end()) == Bytes(frame_no_size.begin() + 1, frame_no_size.end()));
}

TEST_CASE("unit_split: AV1 samples with sized OBUs, delimiters and overruns", "[demux][unit_split][fast]") {
  const Bytes td{0x12, 0x00};
  const Bytes frame{0x32, 0x02, 0xaa, 0xbb};  // OBU_FRAME, size field, 2 bytes
  SECTION("a temporal delimiter inside an ISOBMFF sample raises CNT-9003 and is kept") {
    const Bytes sample = cat({td, frame});
    const Split s = split(sample, Framing::Av1Obu);
    REQUIRE(s.result.units.size() == 2);
    CHECK_FALSE(s.result.rewritten);
    CHECK(s.result.units[1] == UnitSpan{2, 4, 0});
    CHECK(count_code(s.result, "CNT-9003", ovt::fb::Severity::Warning) == 1);
  }
  SECTION("IVF temporal units start with a delimiter by definition: no warning") {
    const Split s = split(cat({td, frame}), Framing::Av1Obu, 0, true);
    CHECK(s.result.units.size() == 2);
    CHECK(s.result.diagnostics.empty());
  }
  SECTION("an obu_size past the end of the sample raises CNT-9001 and clamps") {
    const Bytes sample{0x32, 0x09, 0xaa, 0xbb};
    const Split s = split(sample, Framing::Av1Obu);
    REQUIRE(s.result.units.size() == 1);
    CHECK(s.result.units[0] == UnitSpan{0, 4, 0});
    CHECK(count_code(s.result, "CNT-9001", ovt::fb::Severity::Error) == 1);
  }
  SECTION("a truncated leb128 or a forbidden bit ends the split with CNT-9001") {
    const Split leb = split(Bytes{0x32, 0x80}, Framing::Av1Obu);
    CHECK(leb.result.units.size() == 1);
    CHECK(count_code(leb.result, "CNT-9001", ovt::fb::Severity::Error) == 1);
    const Split forbidden = split(cat({frame, Bytes{0xb2, 0x00}}), Framing::Av1Obu);
    REQUIRE(forbidden.result.units.size() == 2);
    CHECK(forbidden.result.units[1] == UnitSpan{4, 2, 0});
    CHECK(count_code(forbidden.result, "CNT-9001", ovt::fb::Severity::Error) == 1);
  }
}

TEST_CASE("unit_split: raw frames and empty samples", "[demux][unit_split][fast]") {
  const Bytes frame(384, 0x80);
  const Split s = split(frame, Framing::Raw);
  REQUIRE(s.result.units.size() == 1);
  CHECK(s.result.units[0] == UnitSpan{0, 384, 0});
  for (const Framing f : {Framing::LengthPrefixed, Framing::AnnexB, Framing::Av1Obu, Framing::Raw}) {
    const Split empty = split(Bytes{}, f);
    CHECK(empty.result.units.empty());
    CHECK(empty.result.diagnostics.empty());
  }
}

// Acceptance test 1. Hidden from discovery: CTest runs it as unit_split_corpus after unit_split_extract_samples has
// written OVT_UNIT_SPLIT_SAMPLES_DIR (skipped there when the corpus is absent).
TEST_CASE("unit_split: AT1 avc1 samples of syn_h264_multislice_cavlc_mp4",
          "[demux][unit_split][needs-corpus][.corpus]") {
  const std::filesystem::path dir = OVT_UNIT_SPLIT_SAMPLES_DIR;
  if (!std::filesystem::exists(dir / "samples.json")) {
    (void)ovt::test::corpus_or_skip("syn_h264_multislice_cavlc_mp4");  // skips, or fails under OVT_REQUIRE_CORPORA
    FAIL("corpus present but " << (dir / "samples.json").string() << " missing: run unit_split_extract_samples");
  }
  std::ifstream json_in(dir / "samples.json");
  const auto summary = nlohmann::json::parse(json_in);
  REQUIRE(summary["sample_entry"] == "avc1");
  REQUIRE(!summary["samples"].empty());
  for (const auto& rec : summary["samples"]) {
    const auto index = rec["index"].get<int>();
    INFO("sample " << index);
    std::ifstream in(dir / ("sample_" + std::to_string(index) + ".bin"), std::ios::binary);
    const Bytes sample{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(sample.size() == rec["size"].get<std::size_t>());
    const Split s = split(sample, Framing::LengthPrefixed, rec["nal_length_size"].get<uint8_t>());
    CHECK(s.result.units.size() == rec["nal_count"].get<std::size_t>());
    CHECK(s.result.diagnostics.empty());
    REQUIRE(!s.result.units.empty());
    const Bytes first = unit(sample, s, 0);
    REQUIRE(!first.empty());
    CHECK(first[0] != 0x00);         // no start code, no length prefix
    CHECK((first[0] & 0x80U) == 0);  // forbidden_zero_bit
    const unsigned type = first[0] & 0x1FU;
    CHECK(type >= 1);
    CHECK(type <= 23);  // a defined H.264 nal_unit_type (Table 7-1)
  }
}
