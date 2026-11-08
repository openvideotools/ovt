// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the IVF reader (chapter 08 §8.6.2, WI-1.225).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "container/diagnostics.h"
#include "container/ivf.hpp"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace ivf = ovt::container::ivf;

struct TestFrame {
  uint64_t timestamp;
  Bytes payload;
};

void put16(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8U));
}
void put32(Bytes& b, uint32_t v) {
  put16(b, v & 0xFFFFU);
  put16(b, v >> 16U);
}
void put64(Bytes& b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v));
  put32(b, static_cast<uint32_t>(v >> 32U));
}

struct Params {
  std::string fourcc = "AV01";
  uint16_t width = 64;
  uint16_t height = 48;
  uint32_t den = 30;                // timebase_denominator ("rate")
  uint32_t num = 1;                 // timebase_numerator ("scale")
  int64_t header_frame_count = -1;  // -1: the true count
  uint16_t header_size = 32;
};

Bytes make_ivf(const Params& p, const std::vector<TestFrame>& frames) {
  Bytes b = {'D', 'K', 'I', 'F'};
  put16(b, 0);
  put16(b, p.header_size);
  b.insert(b.end(), p.fourcc.begin(), p.fourcc.end());
  put16(b, p.width);
  put16(b, p.height);
  put32(b, p.den);
  put32(b, p.num);
  put32(b,
        p.header_frame_count < 0 ? static_cast<uint32_t>(frames.size()) : static_cast<uint32_t>(p.header_frame_count));
  put32(b, 0);
  b.resize(std::max<size_t>(p.header_size, 32), 0);
  for (const auto& f : frames) {
    put32(b, static_cast<uint32_t>(f.payload.size()));
    put64(b, f.timestamp);
    b.insert(b.end(), f.payload.begin(), f.payload.end());
  }
  return b;
}

// An AV1 temporal unit: temporal delimiter, then (key frames only) a sequence header, then one frame OBU whose
// first payload byte holds show_existing_frame / frame_type.
Bytes av1_tu(bool key, size_t pad = 4, bool show_existing = false) {
  Bytes tu = {0x12, 0x00};  // OBU_TEMPORAL_DELIMITER, has_size, size 0
  if (key) {
    tu.insert(tu.end(), {0x0A, 0x04, 0x00, 0x00, 0x00, 0x00});  // OBU_SEQUENCE_HEADER (profile 0, not reduced)
  }
  const uint8_t first = show_existing ? 0x80 : (key ? 0x10 : 0x30);  // frame_type KEY(0) or INTER(1)<<5
  tu.push_back(0x32);                                                // OBU_FRAME, has_size
  tu.push_back(static_cast<uint8_t>(1 + pad));
  tu.push_back(first);
  tu.insert(tu.end(), pad, 0x55);
  return tu;
}

std::vector<TestFrame> av1_frames(size_t n) {
  std::vector<TestFrame> v;
  v.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    v.push_back({i, av1_tu(i % 4 == 0, 3 + i)});
  }
  return v;
}

const ovt::fb::ContainerTree* tree_of(const Bytes& buf) {
  return flatbuffers::GetRoot<ovt::fb::ContainerTree>(buf.data());
}

const ovt::fb::SampleTable* table_of(const Bytes& buf) {
  return flatbuffers::GetRoot<ovt::fb::SampleTable>(buf.data());
}

size_t diag_count(const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::Diagnostic>>* v) {
  return v == nullptr ? 0 : v->size();
}

bool has_code(const ivf::File& f, const char* code) {
  return std::any_of(f.diagnostics.begin(), f.diagnostics.end(),
                     [&](const ivf::Diagnostic& d) { return ovt::container::diag(d.code).code == code; });
}

}  // namespace

TEST_CASE("ivf: header and frame walk", "[container][ivf][fast]") {
  const auto frames = av1_frames(5);
  const Bytes bytes = make_ivf({}, frames);
  const ovt::io::MemoryFile file(bytes);
  const auto parsed = ivf::parse(file);
  REQUIRE(parsed);
  CHECK(parsed->header.width == 64);
  CHECK(parsed->header.height == 48);
  CHECK(parsed->header.timebase_denominator == 30);
  CHECK(parsed->header.timebase_numerator == 1);
  CHECK(parsed->first_frame_offset == 32);
  REQUIRE(parsed->frames.size() == 5);  // frame_count_actual
  CHECK(parsed->diagnostics.empty());
  uint64_t pos = 32;
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& fr = parsed->frames[i];
    CHECK(fr.header_offset == pos);
    CHECK(fr.payload_offset() == pos + 12);
    CHECK(fr.size == frames[i].payload.size());
    CHECK(fr.timestamp == i);
    pos += 12 + frames[i].payload.size();
  }
  CHECK(pos == bytes.size());
}

TEST_CASE("ivf: not an IVF file", "[container][ivf][fast]") {
  const Bytes tiny(31, 0);
  CHECK_FALSE(ivf::parse(ovt::io::MemoryFile(tiny)));
  Bytes b = make_ivf({}, av1_frames(1));
  b[0] = 'X';
  CHECK_FALSE(ivf::parse(ovt::io::MemoryFile(b)));
  CHECK(ivf::make_reader(ovt::io::MemoryFile(b)) == nullptr);
}

TEST_CASE("ivf: header frame_count is never trusted", "[container][ivf][fast]") {
  Params p;
  p.header_frame_count = 0;
  const Bytes bytes = make_ivf(p, av1_frames(3));
  const auto parsed = ivf::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(parsed);
  CHECK(parsed->frames.size() == 3);
  CHECK(has_code(*parsed, "CNT-IVF-0002"));
  CHECK_FALSE(has_code(*parsed, "CNT-IVF-0003"));
}

TEST_CASE("ivf: header_size other than 32 moves the first frame", "[container][ivf][fast]") {
  Params p;
  p.header_size = 48;
  const Bytes bytes = make_ivf(p, av1_frames(2));
  const auto parsed = ivf::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(parsed);
  CHECK(parsed->first_frame_offset == 48);
  REQUIRE(parsed->frames.size() == 2);
  CHECK(parsed->frames[0].header_offset == 48);
  CHECK(has_code(*parsed, "CNT-IVF-0001"));

  // A header_size below 32 (SPEC-ASSUMPTION): reported, first frame at 32.
  p.header_size = 8;
  const Bytes small = make_ivf(p, av1_frames(2));
  const auto parsed_small = ivf::parse(ovt::io::MemoryFile(small));
  REQUIRE(parsed_small);
  CHECK(parsed_small->first_frame_offset == 32);
  CHECK(parsed_small->frames.size() == 2);
  CHECK(has_code(*parsed_small, "CNT-IVF-0001"));
}

TEST_CASE("ivf: truncated mid-frame", "[container][ivf][fast]") {
  const auto frames = av1_frames(3);
  Bytes bytes = make_ivf({}, frames);
  const size_t cut = 5;
  bytes.resize(bytes.size() - cut);
  const auto parsed = ivf::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(parsed);
  REQUIRE(parsed->frames.size() == 3);
  CHECK(has_code(*parsed, "CNT-IVF-0003"));
  CHECK(parsed->frames[2].declared_size == frames[2].payload.size());
  CHECK(parsed->frames[2].size == frames[2].payload.size() - cut);  // clamped
  CHECK(parsed->frames[2].payload_offset() + parsed->frames[2].size == bytes.size());
}

TEST_CASE("ivf: truncated frame header", "[container][ivf][fast]") {
  Bytes bytes = make_ivf({}, av1_frames(2));
  bytes.insert(bytes.end(), {1, 2, 3, 4, 5});  // 5 stray bytes: not a whole 12-byte frame header
  const auto parsed = ivf::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(parsed);
  CHECK(parsed->frames.size() == 2);
  CHECK(has_code(*parsed, "CNT-IVF-0003"));
}

TEST_CASE("ivf: empty and header-only files", "[container][ivf][fast]") {
  const Bytes bytes = make_ivf({}, {});
  const auto parsed = ivf::parse(ovt::io::MemoryFile(bytes));
  REQUIRE(parsed);
  CHECK(parsed->frames.empty());
  CHECK(parsed->diagnostics.empty());
  const auto reader = ivf::make_reader(ovt::io::MemoryFile(bytes));
  REQUIRE(reader);
  CHECK(reader->track_count() == 1);
}

TEST_CASE("ivf: timebase reduction", "[container][ivf][fast]") {
  ivf::Header h;
  h.timebase_denominator = 30000;
  h.timebase_numerator = 1001;
  CHECK(ivf::timing_of(h).timescale == 30000);
  CHECK(ivf::timing_of(h).scale == 1001);
  h.timebase_denominator = 60;
  h.timebase_numerator = 2;
  CHECK(ivf::timing_of(h).timescale == 30);
  CHECK(ivf::timing_of(h).scale == 1);
  h.timebase_numerator = 0;
  CHECK(ivf::timing_of(h).timescale == 0);
  h.timebase_numerator = 1;
  h.timebase_denominator = 0;
  CHECK(ivf::timing_of(h).timescale == 0);
}

TEST_CASE("ivf: sample table", "[container][ivf][fast]") {
  Params p;
  p.den = 30000;
  p.num = 1001;
  const auto frames = av1_frames(6);
  const Bytes bytes = make_ivf(p, frames);
  const ovt::io::MemoryFile file(bytes);
  const auto reader = ivf::make_reader(file);
  REQUIRE(reader);

  const Bytes all = reader->sample_table(0, 0, 0);
  const auto* t = table_of(all);
  REQUIRE(t != nullptr);
  CHECK(t->track_id() == 1);
  CHECK(t->timescale() == 30000);
  REQUIRE(t->count() == 6);
  REQUIRE(t->dts()->size() == 6);
  uint64_t pos = 32;
  for (uint32_t i = 0; i < 6; ++i) {
    CHECK(t->dts()->Get(i) == int64_t{i} * 1001);
    CHECK(t->duration()->Get(i) == 1001);
    CHECK(t->offset()->Get(i) == pos + 12);
    CHECK(t->size()->Get(i) == frames.at(i).payload.size());
    CHECK(t->cts_offset()->Get(i) == 0);
    const bool sync = t->is_sync()->Get(i) != 0;
    CHECK(sync == (i % 4 == 0));
    pos += 12 + frames.at(i).payload.size();
  }

  const Bytes window = reader->sample_table(0, 2, 3);
  const auto* w = table_of(window);
  REQUIRE(w->count() == 3);
  CHECK(w->offset()->Get(0) == t->offset()->Get(2));
  CHECK(w->dts()->Get(2) == t->dts()->Get(4));

  CHECK(reader->sample_table(1, 0, 0).empty());   // only track 0 exists
  CHECK(reader->sample_table(0, 99, 0).empty());  // first beyond the end
}

TEST_CASE("ivf: is_sync per codec", "[container][ivf][fast]") {
  const auto fourcc = [](const char* s) {
    return static_cast<uint32_t>(s[0]) | static_cast<uint32_t>(s[1]) << 8U | static_cast<uint32_t>(s[2]) << 16U |
           static_cast<uint32_t>(s[3]) << 24U;
  };
  SECTION("AV1") {
    const uint32_t av1 = fourcc("AV01");
    CHECK(ivf::frame_is_sync(av1, av1_tu(true)));
    CHECK_FALSE(ivf::frame_is_sync(av1, av1_tu(false)));
    CHECK_FALSE(ivf::frame_is_sync(av1, av1_tu(true, 4, true)));  // show_existing_frame
    // reduced_still_picture_header: the frame is a key frame without reading frame_type.
    const Bytes still = {0x12, 0x00, 0x0A, 0x01, 0x08, 0x32, 0x01, 0x60};
    CHECK(ivf::frame_is_sync(av1, still));
    // No size field on the last OBU, OBU extension byte, truncated and garbage input.
    const Bytes ext = {0x12, 0x00, 0x34, 0x00, 0x10};  // OBU_FRAME, extension, no size field
    CHECK(ivf::frame_is_sync(av1, ext));
    CHECK_FALSE(ivf::frame_is_sync(av1, Bytes{}));
    CHECK_FALSE(ivf::frame_is_sync(av1, Bytes{0x80}));
    CHECK_FALSE(ivf::frame_is_sync(av1, Bytes{0x12, 0x80}));
    CHECK_FALSE(ivf::frame_is_sync(av1, Bytes{0x12, 0x00, 0x32}));
  }
  SECTION("VP9") {
    const uint32_t vp9 = fourcc("VP90");
    CHECK(ivf::frame_is_sync(vp9, Bytes{0x82, 0x49, 0x83}));   // profile 0, key frame
    CHECK_FALSE(ivf::frame_is_sync(vp9, Bytes{0x86, 0x00}));   // profile 0, inter frame
    CHECK_FALSE(ivf::frame_is_sync(vp9, Bytes{0x88}));         // show_existing_frame
    CHECK(ivf::frame_is_sync(vp9, Bytes{0xB0 | 0x00, 0x00}));  // profile 3 (reserved bit 0), key frame
    CHECK_FALSE(ivf::frame_is_sync(vp9, Bytes{0x00}));         // bad frame_marker
    CHECK_FALSE(ivf::frame_is_sync(vp9, Bytes{}));
  }
  SECTION("VP8") {
    const uint32_t vp8 = fourcc("VP80");
    CHECK(ivf::frame_is_sync(vp8, Bytes{0x10, 0x02, 0x00}));
    CHECK_FALSE(ivf::frame_is_sync(vp8, Bytes{0x11, 0x02, 0x00}));
    CHECK_FALSE(ivf::frame_is_sync(vp8, Bytes{0x10}));
    CHECK_FALSE(ivf::frame_is_sync(fourcc("H264"), Bytes{0, 0, 0, 1, 0x65}));  // no detector for other fourccs
  }
}

TEST_CASE("ivf: tree and TrackInfo", "[container][ivf][fast]") {
  Params p;
  p.den = 30000;
  p.num = 1001;
  const auto frames = av1_frames(6);
  const Bytes bytes = make_ivf(p, frames);
  const ovt::io::MemoryFile file(bytes);
  const auto reader = ivf::make_reader(file);
  REQUIRE(reader);
  const Bytes buf = reader->tree();
  const auto* tree = tree_of(buf);
  CHECK(std::string(tree->format()->c_str()) == "ivf");
  REQUIRE(tree->root()->size() == 1);
  const auto* root = tree->root()->Get(0);
  CHECK(std::string(root->fourcc()->c_str()) == "IVF ");
  CHECK(root->file_span()->offset() == 0);
  CHECK(root->file_span()->length() == bytes.size());
  REQUIRE(root->fields()->size() == 10);
  CHECK(std::string(root->fields()->Get(3)->name()->c_str()) == "fourcc");
  CHECK(std::string(root->fields()->Get(3)->svalue()->c_str()) == "AV01");
  CHECK(root->fields()->Get(4)->uvalue() == 64);
  CHECK(root->fields()->Get(4)->span()->bit_offset() == 96);
  REQUIRE(root->children()->size() == 6);
  const auto* frm = root->children()->Get(1);
  CHECK(std::string(frm->fourcc()->c_str()) == "FRM ");
  CHECK(frm->header_length() == 12);
  CHECK(frm->file_span()->offset() == 32 + 12 + frames[0].payload.size());
  CHECK(frm->file_span()->length() == 12 + frames[1].payload.size());
  REQUIRE(frm->fields()->size() == 2);
  CHECK(frm->fields()->Get(0)->uvalue() == frames[1].payload.size());
  CHECK(frm->fields()->Get(1)->uvalue() == 1);
  CHECK(frm->payload_span()->offset() == frm->file_span()->offset() + 12);
  CHECK(diag_count(root->diagnostics()) == 0);

  REQUIRE(tree->tracks()->size() == 1);
  const auto* track = tree->tracks()->Get(0);
  CHECK(track->track_id() == 1);
  CHECK(track->codec() == ovt::fb::Codec::AV1);
  CHECK(track->handler() == ovt::fb::HandlerType::Video);
  CHECK(std::string(track->sample_entry_fourcc()->c_str()) == "AV01");
  CHECK(track->timescale() == 30000);
  CHECK(track->duration() == uint64_t{6} * 1001);  // (last timestamp + 1) * scale
  CHECK(track->duration_seconds() == Catch::Approx(6 * 1001 / 30000.0));
  CHECK(track->width() == 64);
  CHECK(track->coded_height() == 48);
  CHECK(track->sample_count() == 6);
}

TEST_CASE("ivf: output is deterministic", "[container][ivf][fast]") {
  const Bytes bytes = make_ivf({}, av1_frames(7));
  const ovt::io::MemoryFile file(bytes);
  const auto a = ivf::make_reader(file);
  const auto b = ivf::make_reader(file);
  REQUIRE((a && b));
  CHECK(a->tree() == b->tree());
  CHECK(a->tree() == a->tree());
  CHECK(a->sample_table(0, 0, 0) == b->sample_table(0, 0, 0));
}

TEST_CASE("ivf: FRM children are lazy, expand adds windows", "[container][ivf][fast]") {
  std::vector<TestFrame> frames;
  frames.reserve(9000);
  for (uint64_t i = 0; i < 9000; ++i) {
    frames.push_back({i, Bytes{0x12, 0x00}});
  }
  const Bytes bytes = make_ivf({}, frames);
  const ovt::io::MemoryFile file(bytes);
  auto reader = ivf::make_reader(file);
  REQUIRE(reader);
  auto count = [&] {
    const Bytes buf = reader->tree();
    return tree_of(buf)->root()->Get(0)->children()->size();
  };
  CHECK(count() == 4096);
  {
    const Bytes buf = reader->tree();
    const auto* diags = tree_of(buf)->root()->Get(0)->diagnostics();
    REQUIRE(diags != nullptr);
    REQUIRE(diags->size() == 1);
    CHECK(std::string(diags->Get(0)->code()->c_str()) == "CNT-0007");
  }
  CHECK(reader->expand(0) == OVT_OK);
  CHECK(count() == 8192);
  CHECK(reader->expand(0) == OVT_OK);
  CHECK(count() == 9000);
  CHECK(reader->expand(32) == OVT_OK);             // an FRM node: nothing to expand
  CHECK(reader->expand(33) == OVT_E_INVALID_ARG);  // not a node
  const Bytes buf = reader->tree();
  const auto* diags = tree_of(buf)->root()->Get(0)->diagnostics();
  CHECK(diag_count(diags) == 0);
  // The sample table always lists every frame.
  CHECK(table_of(reader->sample_table(0, 0, 0))->count() == 9000);
}

TEST_CASE("ivf: through the C API", "[container][ivf][open][fast]") {
  const Bytes bytes = make_ivf({}, av1_frames(4));
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes)};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  CHECK(std::string(ovt_container_format(c)) == "ivf");
  CHECK(ovt_container_track_count(c) == 1);
  const ovt_Buffer tree = ovt_container_tree(c);
  REQUIRE(tree.data != nullptr);
  CHECK(flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data)->root()->Get(0)->children()->size() == 4);
  CHECK(ovt_container_expand(c, 0) == OVT_OK);
  CHECK(ovt_container_expand(c, 1) == OVT_E_INVALID_ARG);
  ovt_container_close(c);
}

TEST_CASE("ivf: forced IVF on other bytes is corrupt", "[container][ivf][open][fast]") {
  const Bytes bytes(64, 0x41);
  ovt_File handle{std::make_unique<ovt::io::MemoryFile>(bytes)};
  ovt_Container* c = nullptr;
  CHECK(ovt_container_open_format(&handle, "ivf", nullptr, nullptr, &c) == OVT_E_CORRUPT);
  CHECK(c == nullptr);
}

TEST_CASE("ivf: every ivf corpus entry walks to the end", "[container][ivf][needs-corpus][fast]") {
  std::ifstream in(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/manifest.json");
  REQUIRE(in);
  const auto manifest = nlohmann::json::parse(in);
  for (const auto& entry : manifest.at("entries")) {
    const auto& expected = entry.value("expected", nlohmann::json::object());
    if (expected.value("format", "") != "ivf") {
      continue;
    }
    const std::string id = entry.at("id");
    DYNAMIC_SECTION(id) {
      const auto path = ovt::test::corpus_or_skip(id);
      const auto file = ovt::io::RandomAccessFile::open(path.string());
      const auto parsed = ivf::parse(*file);
      REQUIRE(parsed);
      CHECK_FALSE(has_code(*parsed, "CNT-IVF-0003"));
      uint64_t end = parsed->first_frame_offset;
      for (const auto& fr : parsed->frames) {
        CHECK(fr.header_offset == end);
        end = fr.payload_offset() + fr.size;
      }
      CHECK(end == file->size());  // the walk consumed the file exactly
      // The first temporal unit of an AV1 stream is a key frame.
      const auto reader = ivf::make_reader(*file);
      REQUIRE(reader);
      const Bytes tbl = reader->sample_table(0, 0, 1);
      REQUIRE_FALSE(tbl.empty());
      CHECK(table_of(tbl)->is_sync()->Get(0) != 0);
    }
  }
}

TEST_CASE("ivf: frame_count_actual, offsets and key frames equal ffprobe", "[container][ivf][ffprobe][fast]") {
  // Acceptance test 1 of WI-1.225. The reference is `ffprobe -show_packets`; OVT_IVF_FFPROBE_FILE selects the
  // file (the corpus recipe syn_av1_gop60_1080p.ivf in CI); the test is skipped when it is unset or ffprobe is
  // missing. ffprobe's `pos` of an IVF packet is the offset of the 12-byte frame header (FFmpeg ivfdec.c does
  // `pkt->pos -= 12`), our SampleTable offset is the payload offset: expected offset == pos + 12.
  // SPEC-ASSUMPTION(#280): §8.6.2 says `offset = frame_payload_offset`, the acceptance test says "equals pos".
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once at the start of the test
  const char* env = std::getenv("OVT_IVF_FFPROBE_FILE");
  if (env == nullptr || *env == '\0') {
    SKIP("OVT_IVF_FFPROBE_FILE is not set");
  }
  const std::string cmd =
      std::string("ffprobe -v error -select_streams v:0 -show_entries packet=pos,size,flags -of json '") + env +
      "' 2>/dev/null";
  // NOLINTNEXTLINE(cert-env33-c): test-only; the command is fixed and the path comes from the developer
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) {
    SKIP("ffprobe is not available");
  }
  std::string out;
  std::array<char, 4096> chunk{};
  while (fgets(chunk.data(), static_cast<int>(chunk.size()), pipe.get()) != nullptr) {
    out += chunk.data();
  }
  const auto json = nlohmann::json::parse(out, nullptr, false);
  if (json.is_discarded() || !json.contains("packets") || json.at("packets").empty()) {
    SKIP("ffprobe is not available");
  }
  const auto& packets = json.at("packets");
  const auto file = ovt::io::RandomAccessFile::open(env);
  const auto parsed = ivf::parse(*file);
  REQUIRE(parsed);
  REQUIRE(parsed->frames.size() == packets.size());
  const auto reader = ivf::make_reader(*file);
  REQUIRE(reader);
  const Bytes tbl = reader->sample_table(0, 0, 0);
  const auto* t = table_of(tbl);
  for (uint32_t i = 0; i < packets.size(); ++i) {
    const auto& pkt = packets.at(i);
    CHECK(t->offset()->Get(i) == std::stoull(pkt.at("pos").get<std::string>()) + 12);
    CHECK(t->size()->Get(i) == std::stoul(pkt.at("size").get<std::string>()));
    const bool key = pkt.at("flags").get<std::string>().find('K') != std::string::npos;
    CHECK((t->is_sync()->Get(i) != 0) == key);
  }
}
