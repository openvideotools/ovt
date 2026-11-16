// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Oracle tests of the Y4M frame walk (WI-1.570, chapter 08 §8.6.5): frame count, plane-data offsets and frame sizes
// equal `ffprobe -show_packets` on a file FFmpeg writes. FFmpeg's position is the first plane byte, after `FRAME`.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "container/y4m.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "units_oracle.hpp"

namespace {

using ovt::test::units::Golden;

void check_against_golden(const std::string& path, const Golden& g) {
  const auto file = ovt::io::RandomAccessFile::open(path);
  const auto parsed = ovt::container::y4m::parse(*file);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->frames.size() == g.packets.size());
  for (size_t k = 0; k < g.packets.size(); ++k) {
    INFO("frame " << k);
    REQUIRE(g.packets[k].pos.has_value());
    CHECK(parsed->frames[k].data_offset == *g.packets[k].pos);
    CHECK(parsed->frame_bytes == g.packets[k].size);
  }
}

}  // namespace

TEST_CASE("units y4m: a Y4M file written by FFmpeg equals ffprobe", "[container][y4m][ffprobe][fast]") {
  ovt::test::units::MadeStream stream("units.y4m", ovt::test::units::kTestSource,
                                      "-frames:v 12 -pix_fmt yuv420p -f yuv4mpegpipe");
  if (!stream.ok()) {
    SKIP("ffmpeg is not available");
  }
  const auto golden = ovt::test::units::generate(stream.path(), "rawvideo", "y4m");
  if (!golden) {
    SKIP("ffprobe or python3 is not available");
  }
  REQUIRE(golden->packets.size() == 12);
  check_against_golden(stream.path().string(), *golden);
}
