// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the unit scanners (chapter 7 §7.3.3, WI-1.107): synthetic buffers for every rule, the windowed
// Annex B scan against the single-span scan for many window sizes, a 4 GiB sparse file (acceptance test 3,
// [slow]) and one AV1 IVF corpus stream.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/unit_scan_internal.h"
#include "test_support/corpus.h"

namespace {

using ovt::io::OpenMode;
using ovt::io::RandomAccessFile;
using ovt::syntax::ByteRange;
using ovt::syntax::kNoContainerSample;
using ovt::syntax::ObuSpanKind;
using ovt::syntax::ScanDiagnostic;
using ovt::syntax::ScannedUnit;
using Bytes = std::vector<uint8_t>;

// A copy of one ScannedUnit (its byte view dies with the callback).
struct Unit {
  Unit(uint64_t off, Bytes b, uint64_t prefix_offset, uint64_t prefix_length, int64_t sample_index)
      : file_offset(off),
        bytes(std::move(b)),
        prefix{.offset = prefix_offset, .length = prefix_length},
        sample(sample_index) {}

  uint64_t file_offset;
  Bytes bytes;
  ByteRange prefix;
  int64_t sample;

  bool operator==(const Unit& o) const {
    return file_offset == o.file_offset && bytes == o.bytes && prefix.offset == o.prefix.offset &&
           prefix.length == o.prefix.length && sample == o.sample;
  }
};

struct Collector {
  std::vector<Unit> units;
  std::vector<ScanDiagnostic> diags;

  [[nodiscard]] ovt::syntax::UnitCallback on_unit() {
    return [this](const ScannedUnit& u) {
      units.emplace_back(u.file_offset, Bytes(u.bytes.begin(), u.bytes.end()), u.prefix_span.offset,
                         u.prefix_span.length, u.container_sample_index);
    };
  }
  [[nodiscard]] ovt::syntax::DiagnosticCallback on_diag() {
    return [this](const ScanDiagnostic& d) { diags.push_back(d); };
  }
};

// A file under the test binary directory (never TMPDIR: tests read no environment, chapter 14 §14.1.3).
class TempPath {
 public:
  explicit TempPath(const std::string& stem)
      : path_(std::filesystem::path(OVT_TEST_BINARY_DIR) / (stem + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(path_);
  }
  ~TempPath() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  TempPath(TempPath&&) = delete;
  TempPath& operator=(TempPath&&) = delete;

  [[nodiscard]] std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

void write_at(const std::string& path, uint64_t offset, const Bytes& bytes) {
  std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
  REQUIRE(out.good());
  out.seekp(static_cast<std::streamoff>(offset));
  out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::streamsize>(bytes.size()));
  REQUIRE(out.good());
}

void write_file(const std::string& path, const Bytes& bytes) {
  {
    std::ofstream create(path, std::ios::binary | std::ios::trunc);
  }
  write_at(path, 0, bytes);
}

Collector scan_span(const Bytes& data, uint64_t base = 0, int64_t sample = kNoContainerSample) {
  Collector c;
  ovt::syntax::scan_annexb(std::span<const uint8_t>(data), base, sample, c.on_unit(), c.on_diag());
  return c;
}

Collector scan_file(const std::string& path, size_t window, OpenMode mode = OpenMode::Auto) {
  Collector c;
  const auto file = RandomAccessFile::open(path, mode);
  ovt::syntax::detail::scan_annexb_windowed(*file, window, c.on_unit(), c.on_diag());
  return c;
}

// ---- Annex B ------------------------------------------------------------------------------------------------

TEST_CASE("unit_scan_annexb_prefix_lengths", "[syntax][unit_scan][fast]") {
  // Acceptance test 1: 00 00 00 01 67 .. 00 00 01 68 .. 00 00 00 00 01 65 ..
  const Bytes data{0, 0, 0, 1, 0x67, 0xAA, 0xBB, 0, 0, 1, 0x68, 0xCC, 0, 0, 0, 0, 1, 0x65, 0xDD, 0xEE};
  const std::vector<Unit> expected{
      {4, {0x67, 0xAA, 0xBB}, 0, 4, kNoContainerSample},
      {10, {0x68, 0xCC}, 7, 3, kNoContainerSample},
      {17, {0x65, 0xDD, 0xEE}, 12, 5, kNoContainerSample},
  };
  const Collector span = scan_span(data);
  CHECK(span.units == expected);
  CHECK(span.diags.empty());

  TempPath path("unit_scan_at1");
  write_file(path.str(), data);
  const Collector file = scan_file(path.str(), ovt::syntax::detail::kAnnexBWindowBytes);
  CHECK(file.units == expected);
  CHECK(file.diags.empty());
  Collector pub;
  ovt::syntax::scan_annexb(*RandomAccessFile::open(path.str()), pub.on_unit(), pub.on_diag());
  CHECK(pub.units == expected);
}

TEST_CASE("unit_scan_annexb_trailing_zeros_and_edges", "[syntax][unit_scan][fast]") {
  SECTION("trailing zeros before EOF are stripped") {
    const Collector c = scan_span({0, 0, 1, 0x09, 0xF0, 0, 0, 0});
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].bytes == Bytes{0x09, 0xF0});
  }
  SECTION("bytes before the first start code belong to no unit") {
    const Collector c = scan_span({0x12, 0x34, 0, 0, 1, 0x09, 0xF0});
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].file_offset == 5);
    CHECK(c.units[0].prefix.offset == 2);
    CHECK(c.units[0].prefix.length == 3);
  }
  SECTION("empty NAL units and a start code at EOF are not reported") {
    const Collector c = scan_span({0, 0, 1, 0, 0, 1, 0x41, 0, 0, 1});
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].file_offset == 6);
    CHECK(c.units[0].bytes == Bytes{0x41});
    CHECK(c.units[0].prefix.offset == 3);
  }
  SECTION("no start code, all zeros, empty input") {
    CHECK(scan_span({0x41, 0x42, 0, 1}).units.empty());
    CHECK(scan_span(Bytes(100, 0)).units.empty());
    CHECK(scan_span({}).units.empty());
  }
  SECTION("00 00 03 is not a boundary and not a finding") {
    const Collector c = scan_span({0, 0, 1, 0x41, 0, 0, 3, 1, 0x80});
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].bytes.size() == 6);
    CHECK(c.diags.empty());
  }
  SECTION("file offset and sample index are passed through") {
    const Collector c = scan_span({0, 0, 1, 0x41}, 1000, 7);
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0] == Unit{1003, {0x41}, 1000, 3, 7});
  }
}

TEST_CASE("unit_scan_annexb_forbidden_pattern", "[syntax][unit_scan][fast]") {
  const auto pattern = GENERATE(uint8_t{0}, uint8_t{2});
  // 00 00 02 (or 00 00 00 05) inside the NAL unit: a Diagnostic, not a boundary; reported once per unit.
  const Bytes data{0, 0, 1, 0x41, 0x11, 0, 0, pattern, 5, 0, 0, pattern, 6, 0, 0, 1, 0x42};
  const Collector c = scan_span(data, 100);
  REQUIRE(c.units.size() == 2);
  CHECK(c.units[0].bytes.size() == 10);
  REQUIRE(c.diags.size() == 1);
  CHECK(c.diags[0].code == "SYN-GEN-0004");
  CHECK(c.diags[0].file_offset == 105);
  CHECK(c.diags[0].message == (pattern == 0 ? "forbidden byte pattern 00 00 00 inside NAL unit"
                                            : "forbidden byte pattern 00 00 02 inside NAL unit"));
}

// Random Annex B-like data with many zero runs and start codes, so that start codes and zero runs fall on every
// position relative to a window boundary.
Bytes zero_heavy_stream(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);  // NOLINT(cert-msc32-c,cert-msc51-cpp): fixed seed keeps the test deterministic
  Bytes out;
  out.reserve(n);
  while (out.size() < n) {
    switch (rng() % 6) {
      case 0:
        out.insert(out.end(), {0, 0, 1});
        break;
      case 1:
        out.insert(out.end(), rng() % 7, 0);
        break;
      case 2:
        out.push_back(1);
        break;
      default:
        out.push_back(static_cast<uint8_t>(rng() >> 56U));
        break;
    }
  }
  return out;
}

TEST_CASE("unit_scan_annexb_windows_match_single_span", "[syntax][unit_scan][fast]") {
  const Bytes data = zero_heavy_stream(5000, 42);
  const Collector reference = scan_span(data);
  REQUIRE(reference.units.size() > 100);

  TempPath path("unit_scan_windows");
  write_file(path.str(), data);
  for (const OpenMode mode : {OpenMode::Mmap, OpenMode::Pread}) {
    for (const size_t window :
         std::initializer_list<size_t>{1, 2, 3, 4, 5, 7, 8, 13, 64, 1000, 4096, size_t{1} << 20U}) {
      CAPTURE(window, static_cast<int>(mode));
      const Collector c = scan_file(path.str(), window, mode);
      CHECK(c.units == reference.units);
      CHECK(c.diags.size() == reference.diags.size());
    }
  }
}

TEST_CASE("unit_scan_annexb_start_code_straddles_window", "[syntax][unit_scan][fast]") {
  // Small-window version of acceptance test 3: the start code (and the zero run before it) crosses a boundary at
  // every possible split.
  constexpr size_t kWindow = 16;
  TempPath path("unit_scan_straddle");
  for (size_t split = 0; split <= 4; ++split) {
    CAPTURE(split);
    Bytes data(3 * kWindow, 0x55);
    data[0] = 0;
    data[1] = 0;
    data[2] = 1;
    const size_t sc = (kWindow * 2) - split;  // 00 00 00 01 starts `split` bytes before the boundary
    data[sc] = 0;
    data[sc + 1] = 0;
    data[sc + 2] = 0;
    data[sc + 3] = 1;
    write_file(path.str(), data);
    const Collector c = scan_file(path.str(), kWindow);
    REQUIRE(c.units.size() == 2);
    CHECK(c.units[1].file_offset == sc + 4);
    CHECK(c.units[1].prefix.offset == sc);
    CHECK(c.units[1].prefix.length == 4);
    CHECK(c.units[0].bytes.size() == sc - 3);
  }
}

TEST_CASE("unit_scan_annexb_4gib_sparse_file", "[syntax][unit_scan][slow]") {
  // Acceptance test 3: a 4 GiB sparse file with a start code straddling a 1 MiB window boundary (at 4 GiB, so
  // offsets above 32 bits are exercised too). The unit is found once.
  constexpr uint64_t kFourGiB = uint64_t{1} << 32U;
  constexpr uint64_t kSize = kFourGiB + (uint64_t{2} << 20U);
  TempPath path("unit_scan_sparse");
  {
    std::ofstream create(path.str(), std::ios::binary | std::ios::trunc);
  }
  std::filesystem::resize_file(path.str(), kSize);
  write_at(path.str(), 0, {0, 0, 0, 1, 0x65, 0x11});
  write_at(path.str(), kFourGiB - 2, {0, 0, 1, 0x41, 0x22});  // 00 00 | 01 across the boundary at 4 GiB

  Collector c;
  ovt::syntax::scan_annexb(*RandomAccessFile::open(path.str()), c.on_unit(), c.on_diag());
  REQUIRE(c.units.size() == 2);
  CHECK(c.units[0] == Unit{4, {0x65, 0x11}, 0, 4, kNoContainerSample});
  CHECK(c.units[1] == Unit{kFourGiB + 1, {0x41, 0x22}, 6, kFourGiB + 1 - 6, kNoContainerSample});
  CHECK(c.diags.empty());
}

// ---- length-prefixed ------------------------------------------------------------------------------------------

TEST_CASE("unit_scan_length_prefixed_sizes", "[syntax][unit_scan][fast]") {
  SECTION("N = 1") {
    Collector c;
    const Bytes s{2, 0x41, 0x42, 1, 0x43};
    ovt::syntax::split_length_prefixed(s, 1, 500, 3, c.on_unit(), c.on_diag());
    CHECK(c.units == std::vector<Unit>{{501, {0x41, 0x42}, 500, 1, 3}, {504, {0x43}, 503, 1, 3}});
    CHECK(c.diags.empty());
  }
  SECTION("N = 2") {
    Collector c;
    const Bytes s{0, 1, 0x41, 0, 0, 0, 2, 0x42, 0x43};  // the zero length is skipped
    ovt::syntax::split_length_prefixed(s, 2, 0, 0, c.on_unit(), c.on_diag());
    CHECK(c.units == std::vector<Unit>{{2, {0x41}, 0, 2, 0}, {7, {0x42, 0x43}, 5, 2, 0}});
    CHECK(c.diags.empty());
  }
  SECTION("N = 4") {
    Collector c;
    Bytes s{0, 0, 1, 0};
    s.resize(4 + 256, 0x65);
    ovt::syntax::split_length_prefixed(s, 4, 0, 0, c.on_unit(), c.on_diag());
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].bytes.size() == 256);
    CHECK(c.units[0].prefix.length == 4);
  }
  SECTION("N = 3 is rejected") {
    Collector c;
    CHECK_THROWS_AS(ovt::syntax::split_length_prefixed(Bytes{0, 0, 0}, 3, 0, 0, c.on_unit(), c.on_diag()),
                    std::invalid_argument);
  }
}

TEST_CASE("unit_scan_length_prefixed_overrun", "[syntax][unit_scan][fast]") {
  SECTION("length overruns the sample: unit truncated, SYN-GEN-0105") {
    Collector c;
    ovt::syntax::split_length_prefixed(Bytes{1, 0x41, 9, 0x42, 0x43}, 1, 10, 0, c.on_unit(), c.on_diag());
    CHECK(c.units == std::vector<Unit>{{11, {0x41}, 10, 1, 0}, {13, {0x42, 0x43}, 12, 1, 0}});
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "SYN-GEN-0105");
    CHECK(c.diags[0].message == "length-prefixed unit overruns its sample");
    CHECK(c.diags[0].file_offset == 12);
  }
  SECTION("length field cut off by the sample end") {
    Collector c;
    ovt::syntax::split_length_prefixed(Bytes{0, 0, 0, 1, 0x41, 0, 0}, 4, 0, 0, c.on_unit(), c.on_diag());
    CHECK(c.units.size() == 1);
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "SYN-GEN-0105");
    CHECK(c.diags[0].file_offset == 5);
  }
}

// ---- AV1 low-overhead -----------------------------------------------------------------------------------------

// obu_header byte: forbidden 0, obu_type, extension flag, has_size flag, reserved 0.
constexpr uint8_t obu_header(unsigned type, bool extension, bool has_size) {
  return static_cast<uint8_t>((type << 3U) | (extension ? 4U : 0U) | (has_size ? 2U : 0U));
}

// An IVF frame payload with 4 OBUs: temporal delimiter, sequence header, metadata with an extension byte and a
// two-byte leb128 size, and a frame OBU without obu_size (legal: it is the last OBU of the frame).
Bytes four_obu_frame() {
  Bytes f{obu_header(2, false, true), 0x00};                         // OBU_TEMPORAL_DELIMITER, size 0
  f.insert(f.end(), {obu_header(1, false, true), 3, 1, 2, 3});       // OBU_SEQUENCE_HEADER, size 3
  f.insert(f.end(), {obu_header(5, true, true), 0x08, 0x81, 0x01});  // OBU_METADATA, ext, size 129 (leb128 81 01)
  f.insert(f.end(), 129, 0x5A);
  f.insert(f.end(), {obu_header(6, false, false), 9, 8, 7, 6});  // OBU_FRAME without obu_size
  return f;
}

TEST_CASE("unit_scan_obu_ivf_frame_tiles", "[syntax][unit_scan][fast]") {
  // Acceptance test 2: an IVF frame with 4 OBUs gives 4 units whose spans tile the frame exactly.
  const Bytes frame = four_obu_frame();
  Collector c;
  ovt::syntax::scan_obu_low_overhead(frame, 1000, 5, ObuSpanKind::LengthDelimited, c.on_unit(), c.on_diag());
  REQUIRE(c.units.size() == 4);
  CHECK(c.diags.empty());
  uint64_t next = 1000;
  for (const Unit& u : c.units) {
    CHECK(u.file_offset == next);
    CHECK(u.prefix.length == 0);
    CHECK(u.prefix.offset == u.file_offset);
    CHECK(u.sample == 5);
    next += u.bytes.size();
  }
  CHECK(next == 1000 + frame.size());
  CHECK(c.units[0].bytes.size() == 2);
  CHECK(c.units[1].bytes.size() == 5);
  CHECK(c.units[2].bytes.size() == 4 + 129);
  CHECK(c.units[3].bytes == Bytes{obu_header(6, false, false), 9, 8, 7, 6});
}

TEST_CASE("unit_scan_obu_low_overhead_findings", "[syntax][unit_scan][fast]") {
  SECTION("size-less OBU in a Section 5 stream: SYN-AV1-0001, extends to the end") {
    Collector c;
    ovt::syntax::scan_obu_low_overhead(four_obu_frame(), 0, kNoContainerSample, ObuSpanKind::Section5Stream,
                                       c.on_unit(), c.on_diag());
    CHECK(c.units.size() == 4);
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "SYN-AV1-0001");
    CHECK(c.diags[0].message == "obu_has_size_field = 0 in a position that requires a size");
    CHECK(c.diags[0].file_offset == four_obu_frame().size() - 5);
  }
  SECTION("obu_size overruns the span: SYN-GEN-0105, truncated") {
    Collector c;
    const Bytes s{obu_header(2, false, true), 0, obu_header(6, false, true), 10, 1, 2};
    ovt::syntax::scan_obu_low_overhead(s, 0, 0, ObuSpanKind::LengthDelimited, c.on_unit(), c.on_diag());
    REQUIRE(c.units.size() == 2);
    CHECK(c.units[1].bytes.size() == 4);
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "SYN-GEN-0105");
    CHECK(c.diags[0].file_offset == 2);
  }
  SECTION("header cut off: the BitReader code, scan ends") {
    Collector c;
    const Bytes s{obu_header(2, false, true), 0, obu_header(6, false, true), 0x80};  // leb128 continues past end
    ovt::syntax::scan_obu_low_overhead(s, 0, 0, ObuSpanKind::LengthDelimited, c.on_unit(), c.on_diag());
    CHECK(c.units.size() == 1);
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "BR-0001");
    CHECK(c.diags[0].file_offset >= 2);
  }
  SECTION("extension byte cut off") {
    Collector c;
    ovt::syntax::scan_obu_low_overhead(Bytes{obu_header(6, true, true)}, 0, 0, ObuSpanKind::LengthDelimited,
                                       c.on_unit(), c.on_diag());
    CHECK(c.units.empty());
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "BR-0001");
  }
  SECTION("over-long leb128 is a BR-0009 warning, scanning continues") {
    Collector c;
    const Bytes s{obu_header(2, false, true), 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    ovt::syntax::scan_obu_low_overhead(s, 0, 0, ObuSpanKind::LengthDelimited, c.on_unit(), c.on_diag());
    CHECK(c.units.size() == 1);
    REQUIRE_FALSE(c.diags.empty());
    CHECK(c.diags[0].code == "BR-0009");
  }
}

// ---- AV1 Annex B ----------------------------------------------------------------------------------------------

TEST_CASE("unit_scan_obu_annexb_nested", "[syntax][unit_scan][fast]") {
  // temporal_unit(12) { frame_unit(6) { obu(1) TD, obu(3) }, frame_unit(4) { obu(3) } }; then a second TU.
  const Bytes data{
      12,                               // temporal_unit_size
      6,                                // frame_unit_size
      1,  obu_header(2, false, false),  // obu_length 1, temporal delimiter
      3,  obu_header(1, false, false),
      1,  2,  // obu_length 3, sequence header
      4,      // frame_unit_size
      3,  obu_header(6, false, false),
      7,  8,  // obu_length 3, frame
      3,  2,
      1,  obu_header(2, false, false),  // second TU: tu 3, fu 2, obu_length 1, temporal delimiter
  };
  Collector c;
  ovt::syntax::scan_obu_annexb(data, 100, kNoContainerSample, c.on_unit(), c.on_diag());
  CHECK(c.diags.empty());
  const std::vector<Unit> expected{
      {103, {obu_header(2, false, false)}, 100, 3, kNoContainerSample},  // tu + fu + obu_length
      {105, {obu_header(1, false, false), 1, 2}, 104, 1, kNoContainerSample},
      {110, {obu_header(6, false, false), 7, 8}, 108, 2, kNoContainerSample},  // fu + obu_length
      {116, {obu_header(2, false, false)}, 113, 3, kNoContainerSample},
  };
  CHECK(c.units == expected);
}

TEST_CASE("unit_scan_obu_annexb_findings", "[syntax][unit_scan][fast]") {
  SECTION("obu_length overruns its frame unit: SYN-GEN-0105, truncated") {
    Collector c;
    ovt::syntax::scan_obu_annexb(Bytes{4, 3, 5, 0x10, 0x11}, 0, 0, c.on_unit(), c.on_diag());
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].bytes == Bytes{0x10, 0x11});
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "SYN-GEN-0105");
    CHECK(c.diags[0].file_offset == 2);
  }
  SECTION("zero-length OBU joins the next prefix") {
    Collector c;
    ovt::syntax::scan_obu_annexb(Bytes{4, 3, 0, 1, 0x10}, 0, 0, c.on_unit(), c.on_diag());
    REQUIRE(c.units.size() == 1);
    CHECK(c.units[0].prefix.offset == 0);
    CHECK(c.units[0].prefix.length == 4);
  }
  SECTION("length field cut off: BR-0001, scan ends") {
    Collector c;
    ovt::syntax::scan_obu_annexb(Bytes{2, 1, 0x80}, 0, 0, c.on_unit(), c.on_diag());
    CHECK(c.units.empty());
    REQUIRE(c.diags.size() == 1);
    CHECK(c.diags[0].code == "BR-0001");
  }
}

// ---- corpus ---------------------------------------------------------------------------------------------------

uint32_t le32(std::span<const uint8_t> b, size_t at) {
  return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8U) |
         (static_cast<uint32_t>(b[at + 2]) << 16U) | (static_cast<uint32_t>(b[at + 3]) << 24U);
}

TEST_CASE("unit_scan_obu_corpus_ivf", "[syntax][unit_scan][fast][needs-corpus]") {
  // aom av1-1-b8-01-size-16x16.ivf: 2 frames. OBUs per packet recorded from
  // `ffmpeg -i av1-1-b8-01-size-16x16.ivf -c copy -bsf:v trace_headers -f null -` (FFmpeg 6.1, 2026-11-06):
  // packet 0 (183 bytes) TD, sequence header, frame; packet 1 (79 bytes) TD, frame.
  const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_16x16");
  const auto file = RandomAccessFile::open(path.string());
  const auto all = file->view(0, file->size());
  const std::span<const uint8_t> bytes = all.bytes;
  REQUIRE(bytes.size() >= 32);
  const uint32_t header_size = static_cast<uint32_t>(bytes[6]) | (static_cast<uint32_t>(bytes[7]) << 8U);

  std::vector<size_t> obus_per_frame;
  Collector c;
  size_t pos = header_size;
  int64_t frame = 0;
  while (pos + 12 <= bytes.size()) {
    const uint32_t frame_size = le32(bytes, pos);
    pos += 12;
    REQUIRE(frame_size <= bytes.size() - pos);
    const size_t before = c.units.size();
    ovt::syntax::scan_obu_low_overhead(bytes.subspan(pos, frame_size), pos, frame++, ObuSpanKind::LengthDelimited,
                                       c.on_unit(), c.on_diag());
    obus_per_frame.push_back(c.units.size() - before);
    pos += frame_size;
  }
  std::vector<unsigned> types;
  types.reserve(c.units.size());
  for (const Unit& u : c.units) {
    types.push_back((u.bytes.at(0) >> 3U) & 0xFU);
  }
  CHECK(c.diags.empty());
  CHECK(obus_per_frame == std::vector<size_t>{3, 2});
  CHECK(types == std::vector<unsigned>{2, 1, 6, 2, 6});  // TD, sequence header, frame, TD, frame
}

}  // namespace
