// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Oracle tests of the AV1 OBU stream walker (WI-1.570, chapter 08 §8.6.4): the temporal units of a raw OBU file equal
// the packets of `ffprobe -show_packets` (goldens `tests/golden/av1/<id>.ovt-units.json`, framing "obu"). A Section 5
// temporal unit is one FFmpeg packet (position and size); FFmpeg converts Annex B, so only the count is compared.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "container/obu.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"
#include "units_oracle.hpp"

namespace {

using ovt::container::ObuFraming;
using ovt::container::ObuNode;
using ovt::container::TemporalUnitNode;
using ovt::test::units::Golden;
using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void check_against_golden(const Bytes& data, const Golden& g) {
  const auto detect = ovt::container::obu_detect(
      std::span<const uint8_t>(data).first(std::min<size_t>(data.size(), size_t{64} * 1024)), data.size());
  REQUIRE(detect.framing != ObuFraming::NotAv1);
  std::vector<TemporalUnitNode> tus;
  std::vector<std::vector<uint8_t>> first_types;  // obu_type of every OBU of every temporal unit
  ovt::container::ObuVisitor v;
  v.on_obu = [&](const ObuNode& o) {
    if (first_types.size() <= o.temporal_unit_index) {
      first_types.resize(o.temporal_unit_index + 1);
    }
    first_types[o.temporal_unit_index].push_back(o.header.obu_type);
  };
  v.on_temporal_unit = [&](const TemporalUnitNode& t) { tus.push_back(t); };
  ovt::container::obu_walk(data, 0, detect.framing, v);
  REQUIRE(tus.size() == g.packets.size());
  uint64_t offset = 0;  // the temporal units tile the stream; ffprobe on some builds reports no position
  for (size_t k = 0; k < tus.size(); ++k) {
    INFO("temporal unit " << k);
    REQUIRE(first_types.at(k).front() == ovt::container::kObuTemporalDelimiter);
    if (detect.framing == ObuFraming::Section5) {
      CHECK(tus[k].file_span.offset == g.packets[k].pos.value_or(offset));
      CHECK(tus[k].file_span.offset == offset);
      CHECK(tus[k].file_span.length == g.packets[k].size);
      offset += g.packets[k].size;
    }
  }
}

}  // namespace

TEST_CASE("units obu: corpus temporal units equal the ffprobe goldens",
          "[container][obu][golden][needs-corpus][fast]") {
  size_t compared = 0;
  for (const auto& g : ovt::test::units::committed_goldens("obu")) {
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
      FAIL("no OBU corpus file has a units golden");
    }
    SKIP("no OBU corpus file with a units golden is available");
  }
}

TEST_CASE("units obu: a Section 5 stream written by FFmpeg equals ffprobe", "[container][obu][ffprobe][fast]") {
  ovt::test::units::MadeStream stream("units.obu", ovt::test::units::kTestSource,
                                      "-frames:v 24 -c:v libaom-av1 -cpu-used 8 -g 6 -f obu");
  if (!stream.ok()) {
    SKIP("ffmpeg with libaom-av1 is not available");
  }
  const auto golden = ovt::test::units::generate(stream.path(), "av1", "obu");
  if (!golden) {
    SKIP("ffprobe or python3 is not available");
  }
  REQUIRE(golden->packets.size() == 24);
  check_against_golden(read_file(stream.path()), *golden);
}
