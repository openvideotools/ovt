// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the Y4M reader (chapter 08 §8.6.5, WI-1.228).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <flatbuffers/flatbuffers.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/y4m.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/color.h"
#include "ovt_container_generated.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace y4m = ovt::container::y4m;

// A Y4M stream: `header` after "YUV4MPEG2 ", then `n` frames of `plane_bytes` filler bytes; `frame_tags` follows
// "FRAME" in every frame line.
Bytes make_y4m(const std::string& header, size_t n, size_t plane_bytes, const std::string& frame_tags = {}) {
  const std::string head = "YUV4MPEG2 " + header + "\n";
  Bytes b(head.begin(), head.end());
  for (size_t i = 0; i < n; ++i) {
    const std::string line = "FRAME" + frame_tags + "\n";
    b.insert(b.end(), line.begin(), line.end());
    b.insert(b.end(), plane_bytes, static_cast<uint8_t>(i));
  }
  return b;
}

const ovt::fb::ContainerTree* tree_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data());
}
const ovt::fb::SampleTable* table_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::SampleTable>(b.data());
}
const ovt::fb::RawVideoFormat* raw_of(const Bytes& b) {
  return flatbuffers::GetRoot<ovt::fb::RawVideoFormat>(b.data());
}

bool has_code(const y4m::File& f, const char* code) {
  return std::ranges::any_of(f.diagnostics,
                             [&](const y4m::Diagnostic& d) { return ovt::container::diag(d.code).code == code; });
}

ovt_ColorDescription color_of(const ovt::fb::RawVideoFormat* r) {
  ovt_ColorDescription c{};
  REQUIRE(r->color() != nullptr);
  REQUIRE(r->color()->size() == sizeof(c));
  std::memcpy(&c, r->color()->data(), sizeof(c));
  return c;
}

}  // namespace

TEST_CASE("y4m: C table", "[container][y4m][fast]") {
  struct Row {
    const char* c;
    uint8_t chroma;
    uint8_t depth;
    uint8_t siting;
    const char* pix_fmt;
    unsigned bytes_6x5;  // frame_bytes for 6x5 (odd height rounds chroma up)
  };
  const std::vector<Row> rows = {
      {"420jpeg", 1, 8, 2, "yuv420p", 30 + 2 * 3 * 3},
      {"420", 1, 8, 2, "yuv420p", 30 + 2 * 3 * 3},
      {"420mpeg2", 1, 8, 1, "yuv420p", 30 + 2 * 3 * 3},
      {"420paldv", 1, 8, 3, "yuv420p", 30 + 2 * 3 * 3},
      {"422", 2, 8, 1, "yuv422p", 30 + 2 * 3 * 5},
      {"444", 3, 8, 0, "yuv444p", 90},
      {"444alpha", 3, 8, 0, "yuva444p", 120},
      {"411", 4, 8, 0, "yuv411p", 30 + 2 * 2 * 5},
      {"mono", 0, 8, 0, "gray", 30},
      {"420p10", 1, 10, 0, "yuv420p10le", 2 * (30 + 2 * 3 * 3)},
      {"422p12", 2, 12, 0, "yuv422p12le", 2 * (30 + 2 * 3 * 5)},
      {"444p14", 3, 14, 0, "yuv444p14le", 2 * 90},
      {"444p16", 3, 16, 0, "yuv444p16le", 2 * 90},
      {"mono10", 0, 10, 0, "gray10le", 60},
      {"mono12", 0, 12, 0, "gray12le", 60},
      {"mono14", 0, 14, 0, "gray14le", 60},
      {"mono16", 0, 16, 0, "gray16le", 60},
  };
  for (const auto& r : rows) {
    DYNAMIC_SECTION(r.c) {
      const auto cs = y4m::colour_space(r.c);
      REQUIRE(cs);
      CHECK(cs->chroma_format == r.chroma);
      CHECK(cs->bit_depth == r.depth);
      CHECK(cs->siting == r.siting);
      CHECK(cs->pix_fmt == r.pix_fmt);
      CHECK(y4m::frame_bytes(*cs, 6, 5) == r.bytes_6x5);
      // A stream with that C value opens and exposes the RawVideoFormat.
      const Bytes bytes = make_y4m(std::string("W6 H5 F25:1 C") + r.c, 2, r.bytes_6x5);
      const ovt::io::MemoryFile file(bytes);
      const auto reader = y4m::make_reader(file);
      REQUIRE(reader);
      const Bytes raw = reader->raw_format();
      const auto* rf = raw_of(raw);
      CHECK(rf->chroma_format() == r.chroma);
      CHECK(rf->bit_depth() == r.depth);
      CHECK(rf->frame_bytes() == r.bytes_6x5);
      CHECK(rf->frame_count() == 2);
      CHECK(std::string(rf->pix_fmt()->c_str()) == r.pix_fmt);
      CHECK(color_of(rf).chroma_siting == r.siting);
    }
  }
  for (const char* bad : {"", "420p9", "410", "444alpha10", "mono9", "420p", "xyz", "420P10", "422p10le"}) {
    CHECK_FALSE(y4m::colour_space(bad));
  }
}

TEST_CASE("y4m: header tags", "[container][y4m][fast]") {
  const Bytes bytes =
      make_y4m("W16 H8 F30000:1001 Ip A1:1 C420p10 XYSCSS=420P10 XCOLORRANGE=FULL Zfoo", 3, 16 * 8 * 2 * 3 / 2, " Ia");
  const ovt::io::MemoryFile file(bytes);
  const auto p = y4m::parse(file);
  REQUIRE(p);
  CHECK(p->width == 16);
  CHECK(p->height == 8);
  CHECK(p->fps_num == 30000);
  CHECK(p->fps_den == 1001);
  CHECK(p->interlacing == 'p');
  CHECK(p->aspect_num == 1);
  CHECK(p->full_range);
  CHECK(p->diagnostics.empty());
  REQUIRE(p->header_tags.size() == 9);
  CHECK(p->header_tags[8].text == "Zfoo");  // unknown tags are kept
  CHECK(p->header_tags[0].offset == 10);
  REQUIRE(p->frames.size() == 3);
  REQUIRE(p->frames[0].tags.size() == 1);
  CHECK(p->frames[0].tags[0].text == "Ia");
  CHECK(p->frames[0].data_offset == p->header_bytes + 9);  // "FRAME Ia\n"

  const auto reader = y4m::make_reader(file);
  REQUIRE(reader);
  const Bytes raw = reader->raw_format();
  CHECK(color_of(raw_of(raw)).full_range == 1);
  CHECK(raw_of(raw)->header_bytes() == p->header_bytes);
}

TEST_CASE("y4m: missing F tag", "[container][y4m][fast]") {
  const Bytes bytes = make_y4m("W4 H2", 2, 4 * 2 * 3 / 2);
  const auto p = y4m::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(p);
  CHECK(p->fps_num == 25);
  CHECK(p->fps_den == 1);
  CHECK(has_code(*p, "CNT-Y4M-0001"));
  CHECK(p->frames.size() == 2);
}

TEST_CASE("y4m: unknown C fails with unsupported", "[container][y4m][fast]") {
  const Bytes bytes = make_y4m("W4 H2 F25:1 C420p9", 2, 100);
  const auto p = y4m::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(p);
  CHECK(p->unsupported_colour);
  CHECK(has_code(*p, "CNT-Y4M-0002"));
  ovt_status why = OVT_OK;
  CHECK(y4m::make_reader(ovt::io::MemoryFile(bytes), &why) == nullptr);
  CHECK(why == OVT_E_UNSUPPORTED);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes)};
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open(&handle, &c) == OVT_E_UNSUPPORTED);
  CHECK(c == nullptr);
}

TEST_CASE("y4m: corrupted FRAME magic", "[container][y4m][fast]") {
  const size_t plane = 8 * 4 * 3 / 2;
  Bytes bytes = make_y4m("W8 H4 F25:1", 8, plane);
  const auto good = y4m::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(good);
  REQUIRE(good->frames.size() == 8);
  bytes[good->frames[4].header_offset] = 'X';  // the 5th FRAME
  const auto p = y4m::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(p);
  CHECK(p->frames.size() == 4);
  CHECK(has_code(*p, "CNT-Y4M-0003"));
}

TEST_CASE("y4m: truncated last frame", "[container][y4m][fast]") {
  const size_t plane = 8 * 4 * 3 / 2;
  Bytes bytes = make_y4m("W8 H4 F25:1", 3, plane);
  bytes.resize(bytes.size() - 1);
  const auto p = y4m::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(p);
  CHECK(p->frames.size() == 2);
  CHECK(has_code(*p, "CNT-Y4M-0003"));
  // A FRAME line without a newline before EOF.
  Bytes cut = make_y4m("W8 H4 F25:1", 1, plane);
  cut.insert(cut.end(), {'F', 'R', 'A', 'M', 'E'});
  const auto q = y4m::parse(ovt::io::MemoryFile(cut));
  REQUIRE(q);
  CHECK(q->frames.size() == 1);
  CHECK(has_code(*q, "CNT-Y4M-0003"));
}

TEST_CASE("y4m: not a Y4M stream", "[container][y4m][fast]") {
  const auto parse = [](const std::string& s) { return y4m::parse(ovt::io::MemoryFile(Bytes(s.begin(), s.end()))); };
  CHECK_FALSE(parse(""));
  CHECK_FALSE(parse("YUV4MPEG W4 H2\n"));
  CHECK_FALSE(parse("YUV4MPEG2X W4 H2\n"));
  CHECK_FALSE(parse("YUV4MPEG2 W4 H2"));           // no newline
  CHECK_FALSE(parse("YUV4MPEG2 H2\n"));            // no W
  CHECK_FALSE(parse("YUV4MPEG2 W4\n"));            // no H
  CHECK_FALSE(parse("YUV4MPEG2 W0 H2\n"));         // zero width
  CHECK_FALSE(parse("YUV4MPEG2 Wabc H2\n"));       // not a number
  CHECK_FALSE(parse("YUV4MPEG2 W99999999 H2\n"));  // absurd size
  CHECK(parse("YUV4MPEG2 W4 H2\n"));               // header only: valid, no frames
  // The header line is capped at 64 KiB.
  std::string comment(y4m::kMaxHeaderLine, 'a');
  CHECK_FALSE(parse("YUV4MPEG2 W4 H2 X" + comment + "\n"));
  comment.resize(y4m::kMaxHeaderLine - 100);
  const auto big = parse("YUV4MPEG2 W4 H2 X" + comment + "\n");
  REQUIRE(big);
  CHECK(big->header_tags.size() == 3);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(Bytes(64, 'A'))};
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open_format(&handle, "y4m", nullptr, nullptr, &c) == OVT_E_CORRUPT);
}

TEST_CASE("y4m: tree, TrackInfo and sample table", "[container][y4m][fast]") {
  const size_t plane = 16 * 8 * 3 / 2;
  const Bytes bytes = make_y4m("W16 H8 F30000:1001 Ip", 5, plane, " Ia");
  const ovt::io::MemoryFile file(bytes);
  const auto reader = y4m::make_reader(file);
  REQUIRE(reader);
  CHECK(reader->track_count() == 1);

  const Bytes buf = reader->tree();
  const auto* tree = tree_of(buf);
  CHECK(std::string(tree->format()->c_str()) == "y4m");
  const auto* root = tree->root()->Get(0);
  CHECK(std::string(root->fourcc()->c_str()) == "Y4M ");
  CHECK(root->file_span()->length() == bytes.size());
  REQUIRE(root->fields()->size() == 5);
  CHECK(std::string(root->fields()->Get(0)->name()->c_str()) == "signature");
  CHECK(std::string(root->fields()->Get(1)->name()->c_str()) == "width");
  CHECK(std::string(root->fields()->Get(1)->svalue()->c_str()) == "16");
  CHECK(std::string(root->fields()->Get(3)->svalue()->c_str()) == "30000:1001");
  CHECK(root->fields()->Get(1)->span()->bit_offset() == 80);
  REQUIRE(root->children()->size() == 5);
  const auto* frame = root->children()->Get(2);
  CHECK(std::string(frame->fourcc()->c_str()) == "FRAME");
  CHECK(frame->payload_span()->length() == plane);
  CHECK(frame->file_span()->length() == 9 + plane);
  CHECK(frame->payload_span()->offset() == frame->file_span()->offset() + 9);
  REQUIRE(frame->fields()->size() == 1);
  CHECK(std::string(frame->fields()->Get(0)->svalue()->c_str()) == "Ia");

  const auto* track = tree->tracks()->Get(0);
  CHECK(track->codec() == ovt::fb::Codec::Unknown);
  CHECK(track->timescale() == 30000);
  CHECK(track->duration() == 5005);
  CHECK(track->width() == 16);
  CHECK(track->sample_count() == 5);

  const Bytes tbl = reader->sample_table(0, 0, 0);
  const auto* t = table_of(tbl);
  REQUIRE(t->count() == 5);
  CHECK(t->timescale() == 30000);
  for (uint32_t i = 0; i < 5; ++i) {
    CHECK(t->dts()->Get(i) == int64_t{i} * 1001);
    CHECK(t->duration()->Get(i) == 1001);
    CHECK(t->size()->Get(i) == plane);
    CHECK(t->offset()->Get(i) == root->children()->Get(i)->payload_span()->offset());
    CHECK(t->is_sync()->Get(i) != 0);
  }
  const Bytes window = reader->sample_table(0, 3, 10);
  CHECK(table_of(window)->count() == 2);
  CHECK(reader->sample_table(1, 0, 0).empty());
  CHECK(reader->sample_table(0, 6, 0).empty());
  CHECK(reader->tree() == reader->tree());
}

TEST_CASE("y4m: FRAME children are lazy", "[container][y4m][fast]") {
  const Bytes bytes = make_y4m("W1 H1 Cmono F25:1", 9000, 1);
  const ovt::io::MemoryFile file(bytes);
  auto reader = y4m::make_reader(file);
  REQUIRE(reader);
  const auto count = [&] {
    const Bytes buf = reader->tree();
    return tree_of(buf)->root()->Get(0)->children()->size();
  };
  CHECK(count() == 4096);
  {
    const Bytes buf = reader->tree();
    const auto* d = tree_of(buf)->root()->Get(0)->diagnostics();
    REQUIRE(d != nullptr);
    CHECK(std::string(d->Get(0)->code()->c_str()) == "CNT-0007");
  }
  CHECK(reader->expand(0) == OVT_OK);
  CHECK(count() == 8192);
  CHECK(reader->expand(0) == OVT_OK);
  CHECK(count() == 9000);
  CHECK(reader->expand(1) == OVT_E_INVALID_ARG);
  CHECK(table_of(reader->sample_table(0, 0, 0))->count() == 9000);
}

TEST_CASE("y4m: through the C API", "[container][y4m][open][fast]") {
  const Bytes bytes = make_y4m("W8 H4 F25:1", 4, 8 * 4 * 3 / 2);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes)};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  CHECK(std::string(ovt_container_format(c)) == "y4m");
  CHECK(ovt_container_track_count(c) == 1);
  const ovt_Buffer tree = ovt_container_tree(c);
  REQUIRE(tree.data != nullptr);
  CHECK(flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data)->root()->Get(0)->children()->size() == 4);
  ovt_container_close(c);
}

TEST_CASE("y4m: 30 frames of 1080p 10-bit", "[container][y4m][fast]") {
  // Acceptance test 1 of WI-1.228 on a sparse file shaped like src_1080p_10bit.y4m (C420p10): only the header
  // and the FRAME lines are written, the planes are holes.
  constexpr uint64_t kFrame = 1920ULL * 1080 * 2 * 3 / 2;
  const std::string head = "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420p10\n";
  const auto path = std::filesystem::path(OVT_TEST_BINARY_DIR) / (std::to_string(::getpid()) + "_src_1080p_10bit.y4m");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(head.data(), static_cast<std::streamsize>(head.size()));
    for (int i = 0; i < 30; ++i) {
      out.write("FRAME\n", 6);
      out.seekp(static_cast<std::streamoff>(kFrame), std::ios::cur);
    }
    out.put('\0');  // extend to the end of the 30th frame
  }
  std::filesystem::resize_file(path, head.size() + 30 * (6 + kFrame));
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() { std::filesystem::remove(p); }
  } cleanup{path};

  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const auto reader = y4m::make_reader(*file);
  REQUIRE(reader);
  const Bytes raw = reader->raw_format();
  const auto* rf = raw_of(raw);
  CHECK(rf->frame_count() == 30);
  CHECK(rf->bit_depth() == 10);
  CHECK(rf->chroma_format() == 1);
  CHECK(rf->frame_bytes() == 1920ULL * 1080 * 2 * 3 / 2);
  CHECK(rf->width() == 1920);
  CHECK(rf->height() == 1080);
}

TEST_CASE("y4m: frame_count and format equal ffprobe", "[container][y4m][ffprobe][fast]") {
  // Compares with `ffprobe -count_frames` for the file in OVT_Y4M_FFPROBE_FILE; skipped when unset.
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at the start of the test
  const char* env = std::getenv("OVT_Y4M_FFPROBE_FILE");
  if (env == nullptr || *env == '\0') {
    SKIP("OVT_Y4M_FFPROBE_FILE is not set");
  }
  const std::string cmd = std::string(
                              "ffprobe -v error -count_frames -select_streams v:0 -show_entries "
                              "stream=width,height,pix_fmt,nb_read_frames,r_frame_rate -of json '") +
                          env + "' 2>/dev/null";
  // NOLINTNEXTLINE(cert-env33-c): test-only; the command is fixed and the path comes from the developer
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  std::string out;
  std::array<char, 4096> chunk{};
  while (pipe && fgets(chunk.data(), static_cast<int>(chunk.size()), pipe.get()) != nullptr) {
    out += chunk.data();
  }
  const auto json = nlohmann::json::parse(out, nullptr, false);
  if (json.is_discarded() || !json.contains("streams") || json.at("streams").empty()) {
    SKIP("ffprobe is not available");
  }
  const auto& s = json.at("streams").at(0);
  const auto file = ovt::io::RandomAccessFile::open(env);
  const auto reader = y4m::make_reader(*file);
  REQUIRE(reader);
  const Bytes raw = reader->raw_format();
  const auto* rf = raw_of(raw);
  CHECK(rf->frame_count() == std::stoull(s.at("nb_read_frames").get<std::string>()));
  CHECK(rf->width() == s.at("width").get<uint32_t>());
  CHECK(rf->height() == s.at("height").get<uint32_t>());
  CHECK(std::string(rf->pix_fmt()->c_str()) == s.at("pix_fmt").get<std::string>());
  const std::string rate = std::to_string(rf->fps_num()) + "/" + std::to_string(rf->fps_den());
  CHECK(rate == s.at("r_frame_rate").get<std::string>());
}
