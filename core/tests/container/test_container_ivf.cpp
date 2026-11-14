// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Oracle tests of the IVF frame walk (WI-1.570, chapter 08 §8.6.2): frame count, frame positions and payload sizes
// equal `ffprobe -show_packets` (goldens `tests/golden/av1/<id>.ovt-units.json`, framing "ivf"). FFmpeg reports the
// position of the 12-byte frame header (ivfdec.c: `pkt->pos -= 12`).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "container/ivf.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"
#include "units_oracle.hpp"

namespace {

using ovt::test::units::Golden;

void check_against_golden(const std::string& path, const Golden& g) {
  const auto file = ovt::io::RandomAccessFile::open(path);
  const auto parsed = ovt::container::ivf::parse(*file);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->frames.size() == g.packets.size());
  for (size_t k = 0; k < g.packets.size(); ++k) {
    INFO("frame " << k);
    REQUIRE(g.packets[k].pos.has_value());
    CHECK(parsed->frames[k].header_offset == *g.packets[k].pos);
    CHECK(parsed->frames[k].size == g.packets[k].size);
    CHECK(parsed->frames[k].declared_size == g.packets[k].size);
  }
}

}  // namespace

TEST_CASE("units ivf: corpus frames equal the ffprobe goldens", "[container][ivf][golden][needs-corpus][fast]") {
  size_t compared = 0;
  for (const auto& g : ovt::test::units::committed_goldens("ivf")) {
    const auto path = ovt::test::corpus_path(g.id);
    if (!path) {
      continue;
    }
    DYNAMIC_SECTION(g.id) {
      check_against_golden(path->string(), g);
      ++compared;
    }
  }
  if (compared == 0) {
    if (ovt::test::require_corpora()) {
      FAIL("no IVF corpus file has a units golden");
    }
    SKIP("no IVF corpus file with a units golden is available");
  }
}

TEST_CASE("units ivf: an IVF file written by FFmpeg equals ffprobe", "[container][ivf][ffprobe][fast]") {
  ovt::test::units::MadeStream stream("units.ivf", ovt::test::units::kTestSource,
                                      "-frames:v 24 -c:v libaom-av1 -cpu-used 8 -g 6");
  if (!stream.ok()) {
    SKIP("ffmpeg with libaom-av1 is not available");
  }
  const auto golden = ovt::test::units::generate(stream.path(), "av1", "ivf");
  if (!golden) {
    SKIP("ffprobe or python3 is not available");
  }
  REQUIRE(golden->packets.size() == 24);
  check_against_golden(stream.path().string(), *golden);
}
