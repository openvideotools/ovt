// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of raw YUV opening (chapter 08 §8.6.6, WI-1.229 part a): the pix_fmt grammar, the format string and
// ovt_container_open_format(…, "raw", …).

#include <catch2/catch_test_macros.hpp>

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
const ovt::fb::SampleTable* table_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::SampleTable>(b.data());
}
const ovt::fb::RawVideoFormat* raw_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::RawVideoFormat>(b.data());
}
const ovt::fb::ContainerTree* tree_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data());
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
  const Bytes exact(768 * 2, 0);
  const ovt::io::MemoryFile exact_file(exact);
  const Bytes exact_tree = raw::make_reader(exact_file, fmt)->tree();
  const auto* exact_diags = tree_of(exact_tree)->diagnostics();
  CHECK((exact_diags == nullptr || exact_diags->size() == 0));
}

TEST_CASE("raw: ovt_container_open_format(\"raw\")", "[container][raw][fast]") {
  const std::string path = std::string(OVT_TEST_BINARY_DIR) + "/rawyuv_" + std::to_string(::getpid()) + ".yuv";
  {
    std::ofstream(path, std::ios::binary).put('\0');
    std::filesystem::resize_file(path, 1382400ULL * 30);  // sparse
  }
  const struct Cleanup {
    const std::string& p;
    ~Cleanup() { std::filesystem::remove(p); }
  } cleanup{path};
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
  CHECK(ovt_container_open_format(&handle, "raw", nullptr, nullptr, &c) == OVT_E_INVALID_ARG);
  // Headerless: the plain open of a raw file still wants a format.
  CHECK(ovt_container_open(&handle, &c) != OVT_OK);
}
