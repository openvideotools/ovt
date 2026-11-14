// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of raw YUV opening (chapter 08 §8.6.6, WI-1.229): the pix_fmt grammar, the format string, the sidecar,
// the filename and size heuristics and ranking, and ovt_container_open_format(…, "raw", …).

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <flatbuffers/flatbuffers.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/pix_fmt_grammar.hpp"
#include "container/rawyuv.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/color.h"
#include "ovt_container_generated.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace raw = ovt::container::raw;

// A per-process directory below the test binary directory; files keep the names the heuristics read.
class ScratchDir {
 public:
  ScratchDir() : dir_(std::filesystem::path(OVT_TEST_BINARY_DIR) / ("rawyuv_" + std::to_string(::getpid()))) {
    std::filesystem::create_directories(dir_);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  ScratchDir(ScratchDir&&) = delete;
  ScratchDir& operator=(ScratchDir&&) = delete;

  // A sparse file of `size` bytes.
  [[nodiscard]] std::string make(const std::string& name, uint64_t size) const {
    const auto p = dir_ / name;
    std::ofstream(p, std::ios::binary).put('\0');
    std::filesystem::resize_file(p, size);
    return p.string();
  }
  void write(const std::string& name, const std::string& text) const {
    std::ofstream(dir_ / name, std::ios::binary) << text;
  }

 private:
  std::filesystem::path dir_;
};

const ovt::fb::SampleTable* table_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::SampleTable>(b.data());
}
const ovt::fb::RawVideoFormat* raw_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::RawVideoFormat>(b.data());
}
const ovt::fb::ContainerTree* tree_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data());
}

bool has_code(const raw::Resolution& r, const char* code) {
  return std::ranges::any_of(r.diagnostics,
                             [&](const raw::Diagnostic& d) { return ovt::container::diag(d.code).code == code; });
}

}  // namespace

TEST_CASE("raw: pix_fmt grammar accepts exactly the names of §8.6.6", "[container][raw][fast]") {
  struct Row {
    const char* name;
    ovt::container::raw::PixLayout layout;
    uint8_t chroma;
    uint8_t depth;
    bool le;
    bool uv;
    uint64_t bytes_6x5;  // odd height and width-rounded chroma
  };
  using L = raw::PixLayout;
  const std::vector<Row> rows = {
      {"yuv420p", L::Planar, 1, 8, true, false, 30 + 2 * 3 * 3},
      {"yuv420p10le", L::Planar, 1, 10, true, false, 2 * (30 + 2 * 3 * 3)},
      {"yuv420p10be", L::Planar, 1, 10, false, false, 2 * (30 + 2 * 3 * 3)},
      {"yuv420p9le", L::Planar, 1, 9, true, false, 2 * (30 + 2 * 3 * 3)},
      {"yuv422p", L::Planar, 2, 8, true, false, 30 + 2 * 3 * 5},
      {"yuv422p12le", L::Planar, 2, 12, true, false, 2 * (30 + 2 * 3 * 5)},
      {"yuv444p", L::Planar, 3, 8, true, false, 90},
      {"yuv444p16le", L::Planar, 3, 16, true, false, 180},
      {"yuv411p", L::Planar, 4, 8, true, false, 30 + 2 * 2 * 5},
      {"yuv440p", L::Planar, 2, 8, true, false, 30 + 2 * 6 * 3},
      {"yuv400p", L::Planar, 0, 8, true, false, 30},
      {"yuv400p10le", L::Planar, 0, 10, true, false, 60},
      {"nv12", L::SemiPlanar, 1, 8, true, true, 30 + 2 * 3 * 3},
      {"nv21", L::SemiPlanar, 1, 8, true, true, 30 + 2 * 3 * 3},
      {"nv16", L::SemiPlanar, 2, 8, true, true, 30 + 2 * 3 * 5},
      {"nv24", L::SemiPlanar, 3, 8, true, true, 90},
      {"p010", L::SemiPlanar, 1, 10, true, true, 2 * (30 + 2 * 3 * 3)},
      {"p010le", L::SemiPlanar, 1, 10, true, true, 2 * (30 + 2 * 3 * 3)},
      {"p010be", L::SemiPlanar, 1, 10, false, true, 2 * (30 + 2 * 3 * 3)},
      {"p012le", L::SemiPlanar, 1, 12, true, true, 2 * (30 + 2 * 3 * 3)},
      {"p016le", L::SemiPlanar, 1, 16, true, true, 2 * (30 + 2 * 3 * 3)},
      {"p210le", L::SemiPlanar, 2, 10, true, true, 2 * (30 + 2 * 3 * 5)},
      {"p410le", L::SemiPlanar, 3, 10, true, true, 2 * 90},
      {"gray", L::Gray, 0, 8, true, false, 30},
      {"gray10le", L::Gray, 0, 10, true, false, 60},
      {"gray12be", L::Gray, 0, 12, false, false, 60},
      {"gray16le", L::Gray, 0, 16, true, false, 60},
      {"rgb24", L::PackedRgb, 3, 8, true, false, 90},
      {"bgr24", L::PackedRgb, 3, 8, true, false, 90},
      {"rgb48le", L::PackedRgb, 3, 16, true, false, 180},
      {"rgb48be", L::PackedRgb, 3, 16, false, false, 180},
      {"gbrp", L::PlanarRgb, 3, 8, true, false, 90},
      {"gbrp10le", L::PlanarRgb, 3, 10, true, false, 180},
  };
  for (const auto& r : rows) {
    DYNAMIC_SECTION(r.name) {
      const auto p = raw::parse_pix_fmt(r.name);
      REQUIRE(p);
      CHECK(p->name == r.name);
      CHECK(p->layout == r.layout);
      CHECK(p->chroma_format == r.chroma);
      CHECK(p->bit_depth == r.depth);
      CHECK(p->little_endian == r.le);
      CHECK(p->interleaved_uv == r.uv);
      CHECK(raw::frame_bytes(*p, 6, 5) == r.bytes_6x5);
    }
  }
  for (const char* bad : {"",
                          "yuv",
                          "yuv420",
                          "yuv420p10",
                          "yuv420ple",
                          "yuv420p8le",
                          "yuv420p11le",
                          "yuv420p10lE",
                          "yuv410p",
                          "yuv420P",
                          " yuv420p",
                          "yuv420p ",
                          "nv12le",
                          "nv20",
                          "p010x",
                          "p011le",
                          "p110le",
                          "gray8",
                          "gray10",
                          "gray20le",
                          "rgb24le",
                          "rgb48",
                          "rgba",
                          "gbrp10",
                          "yuyv422",
                          "uyvy422",
                          "v210",
                          "yuva420p",
                          "yuv420p10le10le",
                          "0123456789abcdefg"}) {
    DYNAMIC_SECTION("rejects '" << bad << "'") {
      CHECK_FALSE(raw::parse_pix_fmt(bad));
    }
  }
}

TEST_CASE("raw: frame_bytes formula", "[container][raw][fast]") {
  const auto fb = [](const char* name, uint32_t w, uint32_t h) {
    return raw::frame_bytes(*raw::parse_pix_fmt(name), w, h);
  };
  CHECK(fb("nv12", 1280, 720) == 1382400);
  CHECK(fb("yuv420p", 1920, 1080) == 3110400);
  CHECK(fb("yuv420p10le", 1920, 1080) == 6220800);
  CHECK(fb("yuv420p", 1, 1) == 3);  // odd sizes round the chroma planes up
  CHECK(fb("yuv420p", 0, 4) == 0);
  CHECK(fb("yuv420p", raw::kMaxDimension + 1, 4) == 0);
  CHECK(fb("yuv444p16le", raw::kMaxDimension, raw::kMaxDimension) == 6 * (uint64_t{1} << 40));
}

TEST_CASE("raw: format string WxH:pix_fmt[:fps]", "[container][raw][fast]") {
  // Acceptance test 2: `1280x720:nv12:30000/1001` gives frame_bytes 1382400.
  const auto f = raw::parse_format_string("1280x720:nv12:30000/1001");
  REQUIRE(f);
  CHECK(f->width == 1280);
  CHECK(f->height == 720);
  CHECK(f->pix.name == "nv12");
  CHECK(f->fps_num == 30000);
  CHECK(f->fps_den == 1001);
  CHECK(f->frame_bytes == 1382400);
  CHECK(f->header_bytes == 0);

  const auto def = raw::parse_format_string("64X48:gray10le");
  REQUIRE(def);  // upper-case X, no rate: 25/1
  CHECK(def->fps_num == 25);
  CHECK(def->fps_den == 1);

  struct Fps {
    const char* text;
    uint32_t num;
    uint32_t den;
  };
  for (const Fps& r : {Fps{"30", 30, 1}, Fps{"25.0", 25, 1}, Fps{"29.97", 30000, 1001}, Fps{"23.976", 24000, 1001},
                       Fps{"59.94", 60000, 1001}, Fps{"119.88", 120000, 1001}, Fps{"12.5", 25, 2}, Fps{"60/2", 30, 1},
                       Fps{"0.5", 1, 2}, Fps{"29.970", 30000, 1001}, Fps{"30.03", 3003, 100}}) {
    DYNAMIC_SECTION("fps " << r.text) {
      const auto fps = raw::parse_fps(r.text);
      REQUIRE(fps);
      CHECK(fps->first == r.num);
      CHECK(fps->second == r.den);
    }
  }
  for (const char* bad : {"", "0", "0/1", "1/0", "-1", "1.", ".5", "1e3", "abc", "1/2/3", "30 ", "4294967296",
                          "0.0000000000000001", "1234567890.12345"}) {
    DYNAMIC_SECTION("fps rejects '" << bad << "'") {
      CHECK_FALSE(raw::parse_fps(bad));
    }
  }
  for (const char* bad :
       {"", "1280x720", "1280x720:", "1280x720:nv12:", "1280x720:nv13", "1280x720:nv12:0", "0x720:nv12", "1280x0:nv12",
        "1280:nv12", "x720:nv12", "1280x720x2:nv12", "1048577x16:nv12", "1280x720:nv12:30:1", "1280x720:yuv420p10",
        "1280*720:nv12", "-1x720:nv12", " 1280x720:nv12"}) {
    DYNAMIC_SECTION("format rejects '" << bad << "'") {
      CHECK_FALSE(raw::parse_format_string(bad));
    }
  }
}

TEST_CASE("raw: sidecar parse, write and round trip", "[container][raw][fast]") {
  const auto s =
      raw::parse_sidecar(R"({"width":64,"height":48,"pix_fmt":"yuv422p10le","fps":"30000/1001","header_bytes":64,)"
                         R"("color":{"primaries":9,"transfer":16,"matrix":9,"full_range":true},"future_key":[1,2]})");
  REQUIRE(s);
  CHECK(s->width == 64);
  CHECK(s->height == 48);
  CHECK(s->pix.name == "yuv422p10le");
  CHECK(s->fps_num == 30000);
  CHECK(s->fps_den == 1001);
  CHECK(s->header_bytes == 64);
  CHECK(s->color.present);
  CHECK(s->color.primaries == 9);
  CHECK(s->color.transfer == 16);
  CHECK(s->color.matrix == 9);
  CHECK(s->color.full_range);
  CHECK(s->frame_bytes == 2ULL * (64 * 48 + 2 * 32 * 48));

  const auto min = raw::parse_sidecar(R"({"width":16,"height":16,"pix_fmt":"gray"})");
  REQUIRE(min);
  CHECK(min->fps_num == 25);
  CHECK(min->header_bytes == 0);
  CHECK_FALSE(min->color.present);

  const auto num = raw::parse_sidecar(R"({"width":16,"height":16,"pix_fmt":"gray","fps":29.97})");
  REQUIRE(num);
  CHECK(num->fps_num == 30000);
  CHECK(num->fps_den == 1001);
  const auto integer = raw::parse_sidecar(R"({"width":16,"height":16,"pix_fmt":"gray","fps":24})");
  REQUIRE(integer);
  CHECK(integer->fps_num == 24);

  for (const char* bad :
       {"", "[]", "null", "{", R"({"height":16,"pix_fmt":"gray"})", R"({"width":16,"pix_fmt":"gray"})",
        R"({"width":16,"height":16})", R"({"width":0,"height":16,"pix_fmt":"gray"})",
        R"({"width":-4,"height":16,"pix_fmt":"gray"})", R"({"width":16.5,"height":16,"pix_fmt":"gray"})",
        R"({"width":16,"height":16,"pix_fmt":"yuv420p10"})", R"({"width":16,"height":16,"pix_fmt":7})",
        R"({"width":16,"height":16,"pix_fmt":"gray","fps":"0/1"})",
        R"({"width":16,"height":16,"pix_fmt":"gray","fps":-1})",
        R"({"width":16,"height":16,"pix_fmt":"gray","header_bytes":-1})",
        R"({"width":16,"height":16,"pix_fmt":"gray","header_bytes":"4"})",
        R"({"width":16,"height":16,"pix_fmt":"gray","color":3})",
        R"({"width":16,"height":16,"pix_fmt":"gray","color":{"primaries":256}})",
        R"({"width":16,"height":16,"pix_fmt":"gray","color":{"full_range":1}})"}) {
    DYNAMIC_SECTION("rejects " << bad) {
      CHECK_FALSE(raw::parse_sidecar(bad));
    }
  }

  // Write then parse again: every field survives.
  const auto back = raw::parse_sidecar(raw::sidecar_json(*s));
  REQUIRE(back);
  CHECK(back->width == s->width);
  CHECK(back->pix.name == s->pix.name);
  CHECK(back->fps_num == s->fps_num);
  CHECK(back->fps_den == s->fps_den);
  CHECK(back->header_bytes == s->header_bytes);
  CHECK(back->color.primaries == 9);
  CHECK(back->color.full_range);
  // The written JSON carries exactly the documented keys.
  const auto doc = nlohmann::json::parse(raw::sidecar_json(*min));
  CHECK(doc.contains("width"));
  CHECK(doc.contains("header_bytes"));
  CHECK_FALSE(doc.contains("color"));

  CHECK(raw::sidecar_path("/a/b/clip.yuv") == "/a/b/clip.yuv.ovt-raw.json");
  CHECK(raw::sidecar_path("fd:7").empty());
  CHECK(raw::sidecar_path("").empty());
  CHECK(raw::sidecar_path("content://x/y").empty());
}

TEST_CASE("raw: acceptance 1, file name and size heuristics", "[container][raw][fast]") {
  // Given foo_1920x1080_yuv420p10le.yuv of exactly 30 frames, when opened without a sidecar,
  // then candidates[0] is 1920x1080 yuv420p10le with frame_count 30.
  const ScratchDir dir;
  const std::string path = dir.make("foo_1920x1080_yuv420p10le.yuv", 30ULL * 1920 * 1080 * 3);
  const auto file = ovt::io::RandomAccessFile::open(path);
  const auto cands = raw::candidates(file->info().display_name, file->size());
  REQUIRE(!cands.empty());
  CHECK(cands[0].format.width == 1920);
  CHECK(cands[0].format.height == 1080);
  CHECK(cands[0].format.pix.name == "yuv420p10le");
  CHECK(cands[0].frame_count == 30);
  CHECK(cands[0].filename_geometry);
  CHECK(cands.size() == 1);

  const auto r = raw::resolve(*file);
  CHECK(r.source == raw::Source::Heuristic);
  REQUIRE(r.format);
  CHECK(r.format->frame_bytes == 1920ULL * 1080 * 3);
  CHECK(r.status == OVT_OK);
}

TEST_CASE("raw: candidates from name tokens", "[container][raw][fast]") {
  const auto top = [](const std::string& name, uint64_t size) {
    const auto c = raw::candidates(name, size);
    return c.empty() ? std::string("none")
                     : std::to_string(c[0].format.width) + "x" + std::to_string(c[0].format.height) + " " +
                           c[0].format.pix.name + " " + std::to_string(c[0].frame_count);
  };
  const uint64_t f1080 = 1920ULL * 1080 * 3 / 2;
  CHECK(top("/x/y/clip_1920x1080_420p10.yuv", 5 * f1080 * 2) == "1920x1080 yuv420p10le 5");
  CHECK(top("clip_1920x1080_nv12.yuv", 7 * f1080) == "1920x1080 nv12 7");
  CHECK(top("clip_1920X1080_gray.YUV", 3 * 1920ULL * 1080) == "1920x1080 gray 3");
  CHECK(top("C:\\videos\\clip_352x288_p010.yuv", 2 * 352ULL * 288 * 3) == "352x288 p010 2");
  CHECK(top("clip_1920x1080_10bit.yuv", 4 * f1080 * 2) == "1920x1080 yuv420p10le 4");  // 8-bit does not divide the size
  CHECK(top("clip_1920x1080_444.yuv", 2 * 1920ULL * 1080 * 3) == "1920x1080 yuv444p 2");
  // A name without geometry: the common sizes decide; 1 frame of 1080p 4:2:0 is a multiple of several sizes.
  const auto many = raw::candidates("clip.yuv", f1080);
  REQUIRE(many.size() > 1);
  CHECK(std::ranges::any_of(many, [](const raw::Candidate& c) {
    return c.format.width == 1920 && c.format.height == 1080 && c.format.pix.name == "yuv420p" && c.frame_count == 1;
  }));
  for (const auto& c : many) {
    CHECK(c.frame_count >= 1);
    CHECK(f1080 % c.format.frame_bytes == 0);
    CHECK_FALSE(c.filename_geometry);
  }
  // Nothing fits.
  CHECK(raw::candidates("clip_1920x1080_yuv420p.yuv", f1080 + 1).empty());
  CHECK(raw::candidates("clip.yuv", 0).empty());
  CHECK(raw::candidates("clip.yuv", 7).empty());
  // Rank: the larger picture first, then 4:2:0 before 4:2:2 before 4:4:4, 8-bit before 10-bit.
  const auto eq = raw::candidates("clip_16x16.yuv", 16ULL * 16 * 6);  // several formats divide it
  REQUIRE(eq.size() >= 2);
  CHECK(eq.front().format.pix.chroma_format == 1);
  CHECK(eq.front().format.pix.bit_depth == 8);
}

TEST_CASE("raw: the larger frame size ranks first", "[container][raw][fast]") {
  // 4 frames of 1920x1080 yuv420p are also 16 frames of 960x540: the larger frame wins.
  const uint64_t size = 4ULL * 1920 * 1080 * 3 / 2;
  const auto c = raw::candidates("clip.yuv", size);
  const auto find = [&](uint32_t w, uint32_t h) {
    return std::ranges::find_if(c, [&](const raw::Candidate& x) {
      return x.format.width == w && x.format.height == h && x.format.pix.name == "yuv420p";
    });
  };
  const auto big = find(1920, 1080);
  const auto small = find(960, 540);
  REQUIRE(big != c.end());
  REQUIRE(small != c.end());
  CHECK(big->frame_count == 4);
  CHECK(small->frame_count == 16);
  CHECK(big < small);
  // A single frame ranks after the candidates with two or more frames.
  const auto one = raw::candidates("clip.yuv", 1920ULL * 1080 * 3 / 2 * 2);
  REQUIRE(one.size() >= 2);
  CHECK(one.front().frame_count >= 2);
  CHECK(one.back().frame_count == 1);
}

TEST_CASE("raw: a float sidecar frame rate does not depend on the locale", "[container][raw][fast]") {
  const auto s = raw::parse_sidecar(R"({"width":64,"height":48,"pix_fmt":"yuv420p","fps":29.97})");
  REQUIRE(s);
  CHECK(s->fps_num == 30000);
  CHECK(s->fps_den == 1001);
}

TEST_CASE("raw: frame rate from the file name", "[container][raw][fast]") {
  const uint64_t size = 16ULL * 16 * 3 / 2 * 4;
  const auto rate = [&](const std::string& name) {
    const auto c = raw::candidates(name, size);
    REQUIRE(!c.empty());
    return std::to_string(c[0].format.fps_num) + "/" + std::to_string(c[0].format.fps_den);
  };
  CHECK(rate("a_16x16_yuv420p_24fps.yuv") == "24/1");
  CHECK(rate("a_16x16_yuv420p_29.97fps.yuv") == "30000/1001");
  CHECK(rate("a_16x16_yuv420p_50p.yuv") == "50/1");
  CHECK(rate("a_16x16_yuv420p_60fps.yuv") == "60/1");
  CHECK(rate("a_16x16_yuv420p.yuv") == "25/1");
  CHECK(rate("a_16x16_yuv420p_1080p.yuv") == "25/1");   // four digits are a height, not a rate
  CHECK(rate("a_16x16_yuv420p_999fps.yuv") == "25/1");  // above 240 is not a rate
  CHECK(raw::candidates("a_16x16_yuv420p.yuv", size, 30000, 1001).front().format.fps_num == 30000);
}

TEST_CASE("raw: resolve priority list", "[container][raw][fast]") {
  const ScratchDir dir;
  const std::string path = dir.make("clip.yuv", 64ULL * 48 * 3 / 2 * 6);  // 6 frames of 64x48 4:2:0
  const auto file = ovt::io::RandomAccessFile::open(path);

  // Heuristics alone: ambiguous or not unique, so CNT-RAW-0002 with the candidates and OVT_E_INVALID_ARG.
  const auto amb = raw::resolve(*file);
  CHECK_FALSE(amb.format);
  CHECK(amb.status == OVT_E_INVALID_ARG);
  CHECK(has_code(amb, "CNT-RAW-0002"));
  CHECK(amb.diagnostics.front().message.find("candidates") != std::string::npos);

  // 2. Explicit.
  const auto ex = raw::resolve(*file, "64x48:yuv420p:30");
  CHECK(ex.source == raw::Source::Explicit);
  REQUIRE(ex.format);
  CHECK(ex.format->fps_num == 30);
  CHECK(raw::resolve(*file, "garbage").status == OVT_E_INVALID_ARG);
  CHECK_FALSE(raw::resolve(*file, "garbage").format);

  // --raw-offset and --raw-fps.
  raw::Overrides ov;
  ov.header_bytes = 16;
  ov.fps = std::pair<uint32_t, uint32_t>{50, 1};
  const auto over = raw::resolve(*file, "64x48:yuv420p", ov);
  REQUIRE(over.format);
  CHECK(over.format->header_bytes == 16);
  CHECK(over.format->fps_num == 50);

  // 1. A sidecar beats the explicit string. Its write path is the GUI's.
  raw::RawFormat side = *raw::parse_format_string("32x24:gray:10");
  REQUIRE(raw::write_sidecar(path, side));
  const auto sc = raw::resolve(*file, "64x48:yuv420p:30");
  CHECK(sc.source == raw::Source::Sidecar);
  REQUIRE(sc.format);
  CHECK(sc.format->width == 32);
  CHECK(sc.format->pix.name == "gray");
  const auto read_back = raw::read_sidecar(path);
  REQUIRE(read_back);
  CHECK(read_back->fps_num == 10);

  // An invalid sidecar is ignored.
  dir.write("clip.yuv.ovt-raw.json", "{ not json");
  CHECK(raw::resolve(*file, "64x48:yuv420p").source == raw::Source::Explicit);
  CHECK_FALSE(raw::read_sidecar(path));
  // Writing next to a descriptor-style name fails cleanly.
  CHECK_FALSE(raw::write_sidecar("fd:3", side));
}

TEST_CASE("raw: acceptance 3, sidecar header_bytes becomes the first sample offset", "[container][raw][fast]") {
  const ScratchDir dir;
  const std::string path = dir.make("hdr.yuv", 64 + 3 * (16 * 16 * 3 / 2) + 5);
  dir.write("hdr.yuv.ovt-raw.json", R"({"width":16,"height":16,"pix_fmt":"yuv420p","fps":"25/1","header_bytes":64})");
  const auto file = ovt::io::RandomAccessFile::open(path);
  const auto r = raw::resolve(*file);
  REQUIRE(r.format);
  CHECK(r.source == raw::Source::Sidecar);
  const auto reader = raw::make_reader(*file, *r.format);
  const Bytes table = reader->sample_table(0, 0, 0);
  REQUIRE(!table.empty());
  const auto* t = table_of(table);
  REQUIRE(t->count() == 3);
  CHECK(t->offset()->Get(0) == 64);
  CHECK(t->offset()->Get(1) == 64 + 384);
  CHECK(t->offset()->Get(2) == 64 + 2 * 384);
  CHECK(t->size()->Get(0) == 384);
}

TEST_CASE("raw: reader sample table, tree, RawVideoFormat and trailing bytes", "[container][raw][fast]") {
  const auto fmt = *raw::parse_format_string("16x16:yuv420p10le:30000/1001");  // 768 bytes per frame
  const Bytes bytes(10 * 768 + 100, 0);
  const ovt::io::MemoryFile file(bytes);
  const auto reader = raw::make_reader(file, fmt);
  REQUIRE(reader);
  CHECK(reader->track_count() == 1);

  const Bytes rf = reader->raw_format();
  const auto* r = raw_of(rf);
  CHECK(r->width() == 16);
  CHECK(r->height() == 16);
  CHECK(std::string(r->pix_fmt()->c_str()) == "yuv420p10le");
  CHECK(r->fps_num() == 30000);
  CHECK(r->fps_den() == 1001);
  CHECK(r->chroma_format() == 1);
  CHECK(r->bit_depth() == 10);
  CHECK(r->little_endian());
  CHECK(r->planar());
  CHECK_FALSE(r->interleaved_uv());
  CHECK(r->frame_bytes() == 768);
  CHECK(r->frame_count() == 10);
  CHECK(r->header_bytes() == 0);
  ovt_ColorDescription color{};
  REQUIRE(r->color() != nullptr);
  REQUIRE(r->color()->size() == sizeof(color));
  std::memcpy(&color, r->color()->data(), sizeof(color));
  CHECK(color.struct_size == sizeof(color));
  CHECK(color.chroma_format == 1);
  CHECK(color.bit_depth == 10);
  CHECK(color.primaries == 2);

  const Bytes table = reader->sample_table(0, 0, 0);
  const auto* t = table_of(table);
  REQUIRE(t->count() == 10);
  CHECK(t->timescale() == 30000);
  for (uint32_t i = 0; i < 10; ++i) {
    CHECK(t->offset()->Get(i) == i * 768ULL);
    CHECK(t->size()->Get(i) == 768);
    CHECK(t->dts()->Get(i) == static_cast<int64_t>(i) * 1001);
    CHECK(t->duration()->Get(i) == 1001);
    CHECK(t->is_sync()->Get(i) == 1);
  }
  const Bytes page = reader->sample_table(0, 4, 3);
  const auto* part = table_of(page);
  CHECK(part->count() == 3);
  CHECK(part->offset()->Get(0) == 4 * 768ULL);
  CHECK(reader->sample_table(0, 11, 0).empty());
  CHECK(reader->sample_table(1, 0, 0).empty());
  const Bytes none = reader->sample_table(0, 10, 0);
  CHECK(table_of(none)->count() == 0);

  const Bytes tree = reader->tree();
  const auto* ct = tree_of(tree);
  CHECK(std::string(ct->format()->c_str()) == "raw");
  REQUIRE(ct->tracks()->size() == 1);
  CHECK(ct->tracks()->Get(0)->sample_count() == 10);
  CHECK(ct->tracks()->Get(0)->timescale() == 30000);
  REQUIRE(ct->diagnostics() != nullptr);
  REQUIRE(ct->diagnostics()->size() == 1);  // CNT-RAW-0001: 100 trailing bytes
  CHECK(std::string(ct->diagnostics()->Get(0)->code()->c_str()) == "CNT-RAW-0001");
  CHECK(ct->diagnostics()->Get(0)->message()->str().find("100") != std::string::npos);
  CHECK(reader->expand(0) == OVT_OK);
  CHECK(reader->expand(5) == OVT_E_INVALID_ARG);

  // A file smaller than one frame, and a header that covers the whole file: zero frames, no crash.
  const Bytes tiny(100, 0);
  const ovt::io::MemoryFile tiny_file(tiny);
  CHECK(raw_of(raw::make_reader(tiny_file, fmt)->raw_format())->frame_count() == 0);
  auto huge_header = fmt;
  huge_header.header_bytes = 1'000'000;
  const auto empty = raw::make_reader(file, huge_header);
  CHECK(raw_of(empty->raw_format())->frame_count() == 0);
  const Bytes empty_table = empty->sample_table(0, 0, 0);
  CHECK(table_of(empty_table)->count() == 0);
  const Bytes exact(768ULL * 2, 0);
  const ovt::io::MemoryFile exact_file(exact);
  const Bytes exact_tree = raw::make_reader(exact_file, fmt)->tree();
  const auto* exact_diags = tree_of(exact_tree)->diagnostics();
  CHECK((exact_diags == nullptr || exact_diags->begin() == exact_diags->end()));
}

TEST_CASE("raw: ovt_container_open_format(\"raw\")", "[container][raw][fast]") {
  const ScratchDir dir;
  const std::string path = dir.make("clip.yuv", 1382400ULL * 30);
  ovt_File handle{ovt::io::RandomAccessFile::open(path)};
  ovt_Container* c = nullptr;

  // Acceptance 2 through the C API: frame_bytes == 1382400, 30 frames.
  REQUIRE(ovt_container_open_format(&handle, "raw", "1280x720:nv12:30000/1001", nullptr, &c) == OVT_OK);
  CHECK(std::string(ovt_container_format(c)) == "raw");
  CHECK(ovt_container_track_count(c) == 1);
  CHECK(ovt_container_size(c) == 1382400ULL * 30);
  const ovt_Buffer tree = ovt_container_tree(c);
  REQUIRE(tree.size > 0);
  CHECK(std::string(flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data)->format()->c_str()) == "raw");
  ovt_container_close(c);

  const ovt::container::raw::RawFormat parsed = *raw::parse_format_string("1280x720:nv12:30000/1001");
  CHECK(parsed.frame_bytes == 1382400);
  CHECK(raw::frame_count_of(parsed, 1382400ULL * 30) == 30);

  c = reinterpret_cast<ovt_Container*>(0x1);
  CHECK(ovt_container_open_format(&handle, "raw", "1280x720:nv13", nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(c == nullptr);
  CHECK(ovt_container_open_format(&handle, "raw", "", nullptr, &c) == OVT_E_INVALID_ARG);
  // NULL runs resolve(): without a sidecar and with several candidates that is CNT-RAW-0002 / INVALID_ARG.
  const std::string many = dir.make("many.yuv", 1920ULL * 1080 * 3 / 2 * 4);
  ovt_File many_handle{ovt::io::RandomAccessFile::open(many)};
  CHECK(ovt_container_open_format(&many_handle, "raw", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  CHECK(c == nullptr);
  // With a sidecar the NULL format reaches the C API, `header_bytes` included.
  const std::string with_side = dir.make("side.yuv", 16 + 64ULL * 48 * 3 / 2 * 6);
  dir.write("side.yuv.ovt-raw.json", R"({"width":64,"height":48,"pix_fmt":"yuv420p","fps":29.97,"header_bytes":16})");
  ovt_File side_handle{ovt::io::RandomAccessFile::open(with_side)};
  REQUIRE(ovt_container_open_format(&side_handle, "raw", nullptr, nullptr, &c) == OVT_OK);
  CHECK(ovt_container_size(c) == 16 + 64ULL * 48 * 3 / 2 * 6);
  const ovt_Buffer side_tree = ovt_container_tree(c);
  REQUIRE(side_tree.size > 0);
  const Bytes side_bytes(side_tree.data, side_tree.data + side_tree.size);
  const auto* side_diags = tree_of(side_bytes)->diagnostics();
  CHECK((side_diags == nullptr || side_diags->begin() == side_diags->end()));  // no trailing bytes
  ovt_container_close(c);
  // Headerless: the plain open of a raw file still wants a format.
  CHECK(ovt_container_open(&handle, &c) != OVT_OK);
}
