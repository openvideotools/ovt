// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the Annex B start-code splitter (chapter 08 §8.6.3, WI-1.226).

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "container/annexb.hpp"
#include "container/diagnostics.h"
#include "test_support/corpus.h"

namespace {

using ovt::container::AnnexBCodec;
using ovt::container::DiagCode;
using ovt::container::Finding;
using ovt::container::NalSpan;
using Bytes = std::vector<uint8_t>;

struct Result {
  std::vector<NalSpan> nals;
  std::vector<Finding> findings;
};

Result split(const Bytes& data, AnnexBCodec codec = AnnexBCodec::None, uint64_t base = 0) {
  Result r;
  ovt::container::annexb_split(
      data, base, [&](const NalSpan& n) { r.nals.push_back(n); }, [&](const Finding& f) { r.findings.push_back(f); },
      codec);
  return r;
}

size_t count(const Result& r, DiagCode code) {
  return static_cast<size_t>(
      std::count_if(r.findings.begin(), r.findings.end(), [code](const Finding& f) { return f.code == code; }));
}

class TempFile {
 public:
  TempFile(const std::string& name, const Bytes& content)
      : path_(std::filesystem::path(OVT_TEST_BINARY_DIR) / (std::to_string(::getpid()) + "_" + name)) {
    std::ofstream(path_, std::ios::binary)
        .write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
  }
  ~TempFile() { std::filesystem::remove(path_); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("annexb: 3- and 4-byte start codes give exact prefix and file spans", "[container][annexb][fast]") {
  const Bytes data = {0, 0, 0, 1, 0x67, 0xAA, 0xBB, 0, 0, 1, 0x68, 0xCC, 0, 0, 0, 1, 0x65, 0x80};
  const auto r = split(data);
  REQUIRE(r.nals.size() == 3);
  CHECK(r.nals[0].prefix_span.offset == 0);
  CHECK(r.nals[0].prefix_span.length == 4);
  CHECK(r.nals[0].start_code_bytes == 4);
  CHECK(r.nals[0].file_span.offset == 4);
  CHECK(r.nals[0].file_span.length == 3);
  CHECK(r.nals[0].header_byte == 0x67);
  CHECK(r.nals[1].prefix_span.offset == 7);
  CHECK(r.nals[1].prefix_span.length == 3);
  CHECK(r.nals[1].start_code_bytes == 3);
  CHECK(r.nals[1].file_span.offset == 10);
  CHECK(r.nals[1].file_span.length == 2);
  CHECK(r.nals[2].prefix_span.offset == 12);
  CHECK(r.nals[2].prefix_span.length == 4);
  CHECK(r.nals[2].file_span.offset == 16);
  CHECK(r.nals[2].file_span.length == 2);
  CHECK(r.nals[2].index == 2);
  CHECK(r.findings.empty());
}

TEST_CASE("annexb: 00 00 03 is an emulation prevention byte, 00 00 02 is not a boundary", "[container][annexb][fast]") {
  const Bytes data = {0, 0, 1, 0x41, 0, 0, 3, 1, 0, 0, 2, 0x55, 0, 0, 1, 0x41, 0x11};
  const auto r = split(data);
  REQUIRE(r.nals.size() == 2);
  CHECK(r.nals[0].file_span.offset == 3);
  CHECK(r.nals[0].file_span.length == 9);
  CHECK(r.nals[1].file_span.offset == 15);
}

TEST_CASE("annexb: 1000 trailing zero bytes are excluded and reported once", "[container][annexb][fast]") {
  Bytes data = {0, 0, 1, 0x41, 0x9A};
  data.resize(data.size() + 1000, 0);
  const auto at_eof = split(data);
  REQUIRE(at_eof.nals.size() == 1);
  CHECK(at_eof.nals[0].file_span.length == 2);
  CHECK(count(at_eof, DiagCode::CNT_ESB_0002) == 1);

  data.insert(data.end(), {0, 0, 0, 1, 0x41, 0x10});
  const auto before_next = split(data);
  REQUIRE(before_next.nals.size() == 2);
  CHECK(before_next.nals[0].file_span.length == 2);
  CHECK(before_next.nals[1].prefix_span.length == 4);
  CHECK(before_next.nals[1].prefix_span.offset == 1005);
  CHECK(count(before_next, DiagCode::CNT_ESB_0002) == 1);
  CHECK(before_next.findings[0].file_offset == 5);

  Bytes eight = {0, 0, 1, 0x41, 0x9A};
  eight.resize(eight.size() + 8, 0);
  eight.insert(eight.end(), {0, 0, 1, 0x41, 0x10});
  CHECK(count(split(eight), DiagCode::CNT_ESB_0002) == 0);
}

TEST_CASE("annexb: an empty NAL unit gives CNT-ESB-0003 and no unit", "[container][annexb][fast]") {
  const auto r = split({0, 0, 1, 0, 0, 1, 0x65, 0x80});
  REQUIRE(r.nals.size() == 1);
  CHECK(r.nals[0].file_span.offset == 6);
  CHECK(r.nals[0].index == 0);
  REQUIRE(count(r, DiagCode::CNT_ESB_0003) == 1);
  CHECK(r.findings[0].file_offset == 0);
}

TEST_CASE("annexb: garbage before the first and after the last start code", "[container][annexb][fast]") {
  const auto head = split({0x12, 0x34, 0, 0, 1, 0x65, 0x80});
  REQUIRE(head.nals.size() == 1);
  CHECK(count(head, DiagCode::CNT_ESB_0001) == 1);
  CHECK(head.nals[0].prefix_span.offset == 2);

  const auto none = split({0x12, 0x34, 0x56});
  CHECK(none.nals.empty());
  CHECK(count(none, DiagCode::CNT_ESB_0001) == 1);

  // Leading zero bytes are legal and silent.
  const auto leading = split({0, 0, 0, 0, 0, 0, 1, 0x65, 0x80});
  REQUIRE(leading.nals.size() == 1);
  CHECK(leading.nals[0].prefix_span.offset == 3);
  CHECK(leading.nals[0].prefix_span.length == 4);
  CHECK(leading.findings.empty());

  // 00 00 00 followed by a non-zero byte ends the NAL unit; the rest is garbage up to the next start code.
  const auto mid = split({0, 0, 1, 0x41, 0x11, 0, 0, 0, 0x77, 0, 0, 1, 0x41, 0x22});
  REQUIRE(mid.nals.size() == 2);
  CHECK(mid.nals[0].file_span.length == 2);
  CHECK(count(mid, DiagCode::CNT_ESB_0001) == 1);
}

TEST_CASE("annexb: forbidden_zero_bit and the zero_byte rule", "[container][annexb][fast]") {
  const auto bad = split({0, 0, 1, 0xE5, 0x80});
  REQUIRE(bad.nals.size() == 1);  // still reported
  CHECK(count(bad, DiagCode::CNT_ESB_0005) == 1);

  const Bytes h264 = {0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x65, 0xBB};  // SPS and slice, both with 3-byte codes
  CHECK(count(split(h264, AnnexBCodec::H264), DiagCode::CNT_ESB_0004) == 1);
  CHECK(count(split(h264, AnnexBCodec::None), DiagCode::CNT_ESB_0004) == 0);
  const Bytes h265 = {0, 0, 1, 0x40, 0x01, 0xAA, 0, 0, 1, 0x26, 0x01, 0xBB};  // VPS and IDR slice
  CHECK(count(split(h265, AnnexBCodec::H265), DiagCode::CNT_ESB_0004) == 1);
  const Bytes with_zero_byte = {0, 0, 0, 1, 0x67, 0xAA};
  CHECK(count(split(with_zero_byte, AnnexBCodec::H264), DiagCode::CNT_ESB_0004) == 0);
}

TEST_CASE("annexb: base offset shifts every span", "[container][annexb][fast]") {
  const auto r = split({0, 0, 1, 0x41, 0x80}, AnnexBCodec::None, 1000);
  REQUIRE(r.nals.size() == 1);
  CHECK(r.nals[0].prefix_span.offset == 1000);
  CHECK(r.nals[0].file_span.offset == 1003);
}

TEST_CASE("annexb: the file overload equals the span overload across window edges", "[container][annexb][fast]") {
  // About 2.5 windows of NAL units whose start codes hit every alignment around the 1 MiB window edges.
  Bytes data;
  uint32_t seed = 12345;
  const auto next = [&seed] {
    seed = seed * 1664525U + 1013904223U;
    return seed >> 16U;
  };
  while (data.size() < (size_t{5} << 19U)) {
    const bool four = (next() & 1U) != 0;
    data.insert(data.end(), {0, 0});
    if (four) {
      data.push_back(0);
    }
    data.push_back(1);
    data.push_back(static_cast<uint8_t>(0x41 | (next() & 0x1CU)));
    const size_t len = next() % 3000;
    for (size_t i = 0; i < len; ++i) {
      auto b = static_cast<uint8_t>(next());
      if (b == 0 && (next() & 3U) != 0) {
        b = 0x55;  // keep zero pairs rare, the NAL payload must not contain 00 00 0x
      }
      data.push_back(b);
      if (data.size() >= 2 && data[data.size() - 1] == 0 && data[data.size() - 2] == 0) {
        data.push_back(3);
      }
    }
    data.push_back(0x80);
  }
  // Put start codes exactly across the first window edge.
  const size_t edge = size_t{1} << 20U;
  for (const size_t shift : {size_t{0}, size_t{1}, size_t{2}, size_t{3}}) {
    Bytes d = data;
    d.insert(d.begin() + static_cast<std::ptrdiff_t>(edge - shift), {0x80, 0, 0, 1, 0x41, 0x11});
    const auto expected = split(d);
    const TempFile f("edge.264", d);
    Result got;
    const auto file = ovt::io::RandomAccessFile::open(f.path());
    ovt::container::annexb_split(
        *file, [&](const NalSpan& n) { got.nals.push_back(n); }, [&](const Finding& x) { got.findings.push_back(x); });
    REQUIRE(got.nals.size() == expected.nals.size());
    for (size_t i = 0; i < got.nals.size(); ++i) {
      REQUIRE(got.nals[i].file_span == expected.nals[i].file_span);
      REQUIRE(got.nals[i].prefix_span == expected.nals[i].prefix_span);
    }
    CHECK(got.findings.size() == expected.findings.size());
  }
}

TEST_CASE("annexb: every Annex B corpus file matches tools/ref_annexb.py", "[container][annexb][needs-corpus][fast]") {
  std::ifstream in(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/manifest.json");
  REQUIRE(in);
  const auto manifest = nlohmann::json::parse(in);
  const auto script = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tools/ref_annexb.py";
  for (const auto& entry : manifest.at("entries")) {
    const auto& containers = entry.value("containers", nlohmann::json::array());
    if (std::find(containers.begin(), containers.end(), "annexb") == containers.end()) {
      continue;
    }
    const std::string id = entry.at("id");
    const auto& codecs = entry.value("codecs", nlohmann::json::array());
    const bool h265 = std::find(codecs.begin(), codecs.end(), "h265") != codecs.end();
    DYNAMIC_SECTION(id) {
      const auto path = ovt::test::corpus_or_skip(id);
      const auto file = ovt::io::RandomAccessFile::open(path.string());
      std::vector<std::string> got;
      ovt::container::annexb_split(*file, [&](const NalSpan& n) {
        const unsigned type = h265 ? (n.header_byte >> 1U) & 0x3FU : n.header_byte & 0x1FU;
        got.push_back(std::to_string(n.file_span.offset) + "," + std::to_string(n.file_span.length) + "," +
                      std::to_string(type));
      });
      const std::string cmd =
          "python3 -I \"" + script.string() + "\" --codec " + (h265 ? "h265" : "h264") + " \"" + path.string() + "\"";
      std::vector<std::string> expected;
      // NOLINTNEXTLINE(cert-env33-c): test-only invocation of the reference splitter
      FILE* pipe = popen(cmd.c_str(), "r");
      REQUIRE(pipe != nullptr);
      std::string line;
      for (int c = fgetc(pipe); c != EOF; c = fgetc(pipe)) {
        if (c == '\n') {
          expected.push_back(line);
          line.clear();
        } else {
          line.push_back(static_cast<char>(c));
        }
      }
      REQUIRE(pclose(pipe) == 0);
      CHECK(got == expected);
    }
  }
}
