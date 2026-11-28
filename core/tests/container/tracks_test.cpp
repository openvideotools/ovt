// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of TrackInfo (chapter 08 §8.1.3, WI-1.217) over synthetic MP4 files: the three acceptance lines
// (dv_p81 shape, encv + frma, duplicate track_ID), codec strings, tkhd/mdhd/stbl values and the C API.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <flatbuffers/flatbuffers.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "io/memory_file.hpp"
#include "io/ovt_file.hpp"
#include "ovt/container.h"
#include "ovt_container_generated.h"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;

void put(Bytes& b, uint64_t v, int nbytes) {
  for (int s = (nbytes - 1) * 8; s >= 0; s -= 8) {
    b.push_back(s >= 64 ? uint8_t{0}
                        : static_cast<uint8_t>(v >> static_cast<unsigned>(s)));  // wider than 8 bytes: zero pad
  }
}
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
void put_all(Bytes& b, const Bytes& more) {
  b.insert(b.end(), more.begin(), more.end());
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    put_all(b, p);
  }
  return b;
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put(b, 8 + payload.size(), 4);
  put_str(b, type);
  put_all(b, payload);
  return b;
}
Bytes full_box(std::string_view type, uint32_t flags, const Bytes& payload) {
  Bytes p;
  put(p, flags, 4);  // version 0
  put_all(p, payload);
  return box(type, p);
}

Bytes mvhd(uint32_t timescale) {
  Bytes p;
  put(p, 0, 8);  // creation, modification
  put(p, timescale, 4);
  put(p, 0, 4);  // duration
  put(p, 0x10000, 4);
  put(p, 0x100, 2);
  put(p, 0, 10);
  for (const uint32_t m : {0x10000U, 0U, 0U, 0U, 0x10000U, 0U, 0U, 0U, 0x40000000U}) {
    put(p, m, 4);
  }
  put(p, 0, 24);
  put(p, 2, 4);
  return full_box("mvhd", 0, p);
}

Bytes tkhd(uint32_t track_id, uint32_t flags, const std::vector<uint32_t>& abcd = {0x10000, 0, 0, 0x10000},
           uint32_t w = 1920, uint32_t h = 1080) {
  Bytes p;
  put(p, 0, 8);
  put(p, track_id, 4);
  put(p, 0, 4);
  put(p, 3000, 4);  // duration, movie timescale
  put(p, 0, 8);
  put(p, 0, 8);  // layer, alternate_group, volume, reserved
  const std::vector<uint32_t> m = {abcd[0], abcd[1], 0, abcd[2], abcd[3], 0, 0, 0, 0x40000000};
  for (const uint32_t v : m) {
    put(p, v, 4);
  }
  put(p, uint64_t{w} << 16U, 4);
  put(p, uint64_t{h} << 16U, 4);
  return full_box("tkhd", flags, p);
}

Bytes mdhd(uint32_t timescale, uint32_t duration, std::string_view lang = "eng") {
  Bytes p;
  put(p, 0, 8);
  put(p, timescale, 4);
  put(p, duration, 4);
  put(p,
      (static_cast<uint32_t>(lang[0] - 0x60) << 10U) | (static_cast<uint32_t>(lang[1] - 0x60) << 5U) |
          static_cast<uint32_t>(lang[2] - 0x60),
      2);
  put(p, 0, 2);
  return full_box("mdhd", 0, p);
}

Bytes hdlr(std::string_view type) {
  Bytes p;
  put(p, 0, 4);
  put_str(p, type);
  put(p, 0, 12);
  p.push_back(0);
  return full_box("hdlr", 0, p);
}

Bytes stsz(uint32_t size, uint32_t count) {
  Bytes p;
  put(p, size, 4);
  put(p, count, 4);
  return full_box("stsz", 0, p);
}

Bytes visual_entry(std::string_view fourcc, uint16_t w, uint16_t h, const std::vector<Bytes>& children) {
  Bytes p;
  put(p, 0, 6);
  put(p, 1, 2);  // data_reference_index
  put(p, 0, 16);
  put(p, w, 2);
  put(p, h, 2);
  put(p, 0x480000, 4);
  put(p, 0x480000, 4);
  put(p, 0, 4);
  put(p, 1, 2);
  put(p, 0, 32);
  put(p, 0x18, 2);
  put(p, 0xFFFF, 2);
  for (const Bytes& c : children) {
    put_all(p, c);
  }
  return box(fourcc, p);
}

Bytes audio_entry(std::string_view fourcc, uint16_t channels, uint32_t rate, const std::vector<Bytes>& children) {
  Bytes p;
  put(p, 0, 6);
  put(p, 1, 2);
  put(p, 0, 8);
  put(p, channels, 2);
  put(p, 16, 2);
  put(p, 0, 4);
  put(p, uint64_t{rate} << 16U, 4);
  for (const Bytes& c : children) {
    put_all(p, c);
  }
  return box(fourcc, p);
}

Bytes avcc() {
  return box("avcC", Bytes{1, 0x64, 0x00, 0x1F, 0xFF, 0xE0, 0});
}
Bytes hvcc() {
  Bytes p{1, 0x02};
  put(p, 0x20000000, 4);  // general_profile_compatibility_flags: Main 10
  put_all(p, Bytes{0xB0, 0, 0, 0, 0, 0});
  p.push_back(153);
  put_all(p, Bytes{0xF0, 0, 0xFC, 0xFC, 0xF8, 0xF8, 0, 0, 0x03, 0});
  return box("hvcC", p);
}
Bytes dvvc() {
  Bytes p{1, 0, 0x10, 0x30};  // profile 8, level 6
  put(p, 0, 20);
  return box("dvvC", p);
}
Bytes av1c() {
  return box("av1C", Bytes{0x81, 0x08, 0x4C, 0x00});
}
Bytes colr_nclx(unsigned cp, unsigned tc, unsigned mc, unsigned full) {
  Bytes p;
  put_str(p, "nclx");
  put(p, cp, 2);
  put(p, tc, 2);
  put(p, mc, 2);
  p.push_back(static_cast<uint8_t>(full << 7U));
  return box("colr", p);
}
Bytes btrt(uint32_t max, uint32_t avg) {
  Bytes p;
  put(p, 0, 4);
  put(p, max, 4);
  put(p, avg, 4);
  return box("btrt", p);
}
Bytes esds_aac() {
  Bytes body = {0x40, 0x15};                     // objectTypeIndication (Audio ISO/IEC 14496-3), streamType
  put(body, 0, 11);                              // bufferSizeDB, maxBitrate, avgBitrate
  put_all(body, Bytes{0x05, 0x02, 0x12, 0x10});  // DecoderSpecificInfo: AAC-LC, 44.1 kHz, 2 channels
  Bytes dcd = {0x04, static_cast<uint8_t>(body.size())};
  put_all(dcd, body);
  Bytes es = {0x03, static_cast<uint8_t>(3 + dcd.size()), 0, 1, 0};
  put_all(es, dcd);
  return full_box("esds", 0, es);
}

Bytes stsd(const Bytes& entry) {
  Bytes p;
  put(p, 1, 4);
  put_all(p, entry);
  return full_box("stsd", 0, p);
}

Bytes trak(uint32_t id, std::string_view handler, const Bytes& entry, uint32_t samples, uint32_t timescale = 30000,
           uint32_t duration = 90000, const Bytes& tkhd_box = {}, const std::vector<Bytes>& extra = {}) {
  const Bytes stbl = box("stbl", cat({stsd(entry), stsz(1000, samples)}));
  const Bytes mdia = box("mdia", cat({mdhd(timescale, duration), hdlr(handler), box("minf", stbl)}));
  Bytes children = tkhd_box.empty() ? tkhd(id, 3) : tkhd_box;
  for (const Bytes& e : extra) {
    put_all(children, e);
  }
  put_all(children, mdia);
  return box("trak", children);
}

Bytes mp4(const std::vector<Bytes>& moov_children) {
  Bytes ftyp;
  put_str(ftyp, "isom");
  put(ftyp, 0, 4);
  Bytes moov = mvhd(1000);
  for (const Bytes& c : moov_children) {
    put_all(moov, c);
  }
  return cat({box("ftyp", ftyp), box("moov", moov), box("mdat", Bytes(16, 0))});
}

struct Opened {
  ovt_File handle;
  ovt_Container* container = nullptr;
  explicit Opened(const Bytes& data) : handle{std::make_unique<ovt::io::MemoryFile>(data)} {
    REQUIRE(ovt_container_open(&handle, &container) == OVT_OK);
  }
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  ~Opened() { ovt_container_close(container); }
};

// The i-th track of the tree, decoded to the object API (copy; the tree buffer is not kept).
ovt::fb::TrackInfoT track_of(ovt_Container* c, int i) {
  const ovt_Buffer b = ovt_container_tree(c);
  const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(b.data);
  REQUIRE(t->tracks() != nullptr);
  REQUIRE(static_cast<int>(t->tracks()->size()) > i);
  ovt::fb::TrackInfoT out;
  t->tracks()->Get(static_cast<flatbuffers::uoffset_t>(i))->UnPackTo(&out);
  return out;
}

bool has_code(const ovt::fb::TrackInfoT& t, std::string_view code) {
  return std::ranges::any_of(t.diagnostics, [&](const auto& d) { return d->code == code; });
}

const ovt::fb::DiagnosticT* find_diag(const ovt::fb::TrackInfoT& t, std::string_view code) {
  const auto it = std::ranges::find_if(t.diagnostics, [&](const auto& d) { return d->code == code; });
  return it == t.diagnostics.end() ? nullptr : it->get();
}

// Boxes named `fourcc` in the subtree of a tree node.
size_t count_boxes(const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::BoxNode>>* nodes, std::string_view fourcc) {
  size_t n = 0;
  if (nodes == nullptr) {
    return n;
  }
  for (const auto* b : *nodes) {
    // A "trailing bytes" node (fewer than eight bytes after the last box) has an empty fourcc, which the FlatBuffer
    // omits: fourcc() is then nullptr, as container.cpp's own tree walks already allow for.
    n += b->fourcc() != nullptr && b->fourcc()->string_view() == fourcc ? 1U : 0U;
    n += count_boxes(b->children(), fourcc);
  }
  return n;
}

}  // namespace

TEST_CASE("tracks: Dolby Vision HEVC entry keeps hvcC and dvvC in order", "[container][tracks][fast]") {
  const Bytes data = mp4({trak(1, "vide", visual_entry("hvc1", 3840, 2160, {hvcc(), dvvc()}), 3600)});
  Opened o(data);
  REQUIRE(ovt_container_track_count(o.container) == 1);
  const auto t = track_of(o.container, 0);
  CHECK(t.codec == ovt::fb::Codec::H265);
  CHECK(t.sample_entry_fourcc == "hvc1");
  REQUIRE(t.codec_private.size() == 2);
  CHECK(t.codec_private[0]->fourcc == "hvcC");
  CHECK(t.codec_private[1]->fourcc == "dvvC");
  CHECK(t.codec_string == "hvc1.2.4.L153.B0");
  CHECK(t.coded_width == 3840);
  CHECK(t.coded_height == 2160);
}

TEST_CASE("tracks: dvh1 gets the Dolby Vision codec string", "[container][tracks][fast]") {
  const Bytes data = mp4({trak(1, "vide", visual_entry("dvh1", 1920, 1080, {hvcc(), dvvc()}), 10)});
  Opened o(data);
  const auto t = track_of(o.container, 0);
  CHECK(t.codec == ovt::fb::Codec::H265);
  CHECK(t.codec_string == "dvh1.08.06");
}

TEST_CASE("tracks: encv entry resolves through sinf/frma", "[container][tracks][fast]") {
  Bytes frma;
  put_str(frma, "avc1");
  const Bytes sinf = box("sinf", box("frma", frma));
  const Bytes data = mp4({trak(1, "vide", visual_entry("encv", 1280, 720, {avcc(), sinf}), 10)});
  Opened o(data);
  const auto t = track_of(o.container, 0);
  CHECK(t.is_encrypted);
  CHECK(t.sample_entry_fourcc == "encv");
  CHECK(t.original_format == "avc1");
  CHECK(t.codec == ovt::fb::Codec::H264);
  CHECK(t.codec_string == "avc1.64001F");
}

TEST_CASE("tracks: a duplicate track_ID gets CNT-1012 on the second track", "[container][tracks][fast]") {
  const Bytes entry = visual_entry("avc1", 640, 360, {avcc()});
  const Bytes data = mp4({trak(7, "vide", entry, 1), trak(7, "vide", entry, 1)});
  Opened o(data);
  REQUIRE(ovt_container_track_count(o.container) == 2);
  CHECK_FALSE(has_code(track_of(o.container, 0), "CNT-1012"));
  const auto second = track_of(o.container, 1);
  const auto* d = find_diag(second, "CNT-1012");
  REQUIRE(d != nullptr);
  CHECK(d->severity == ovt::fb::Severity::Error);  // §8.5
  CHECK(d->spec_ref == ovt::container::diag("CNT-1012")->spec_ref);
}

TEST_CASE("tracks: a non-rotation tkhd.matrix gives rotation 0 and CNT-1024", "[container][tracks][fast]") {
  const Bytes entry = visual_entry("avc1", 640, 360, {avcc()});
  const Bytes scaled = tkhd(1, 3, {0x20000, 0, 0, 0x10000});  // 2x horizontal scale
  const Bytes sheared = tkhd(2, 3, {0x10000, 0x8000, 0, 0x10000});
  const Bytes rotated = tkhd(3, 3, {0, 0x10000, 0xFFFF0000U, 0});
  const Bytes data =
      mp4({trak(1, "vide", entry, 1, 30000, 90000, scaled), trak(2, "vide", entry, 1, 30000, 90000, sheared),
           trak(3, "vide", entry, 1, 30000, 90000, rotated)});
  Opened o(data);
  for (const int i : {0, 1}) {
    const auto t = track_of(o.container, i);
    CHECK(t.rotation == 0);
    const auto* d = find_diag(t, "CNT-1024");
    REQUIRE(d != nullptr);
    CHECK(d->severity == ovt::fb::Severity::Info);
  }
  const auto r = track_of(o.container, 2);
  CHECK(r.rotation == 90);
  CHECK_FALSE(has_code(r, "CNT-1024"));
}

TEST_CASE("tracks: tkhd, mdhd and stbl values", "[container][tracks][fast]") {
  // A portrait phone recording: matrix (0,1,-1,0) is 90 degrees clockwise; the track is disabled in preview.
  const Bytes tk = tkhd(2, 3, {0, 0x10000, 0xFFFF0000U, 0}, 1080, 1920);
  const Bytes edts = box("edts", full_box("elst", 0, Bytes{0, 0, 0, 0}));
  const Bytes tref = box("tref", box("chap", Bytes{0, 0, 0, 5}));
  const Bytes entry = visual_entry("avc1", 1080, 1920, {avcc(), btrt(9'000'000, 8'000'000)});
  const Bytes data = mp4({trak(2, "vide", entry, 90, 30000, 90000, tk, {edts, tref})});
  Opened o(data);
  const auto t = track_of(o.container, 0);
  CHECK(t.index == 0);
  CHECK(t.track_id == 2);
  CHECK(t.handler == ovt::fb::HandlerType::Video);
  CHECK(t.handler_fourcc == "vide");
  CHECK(t.timescale == 30000);
  CHECK(t.duration == 90000);
  CHECK_THAT(t.duration_seconds, Catch::Matchers::WithinAbs(3.0, 1e-9));
  CHECK(t.movie_timescale == 1000);
  CHECK(t.tkhd_duration == 3000);
  CHECK(t.language == "eng");
  CHECK(t.enabled);
  CHECK(t.in_movie);
  CHECK_FALSE(t.in_preview);
  CHECK_THAT(t.width, Catch::Matchers::WithinAbs(1080.0, 1e-9));
  CHECK_THAT(t.height, Catch::Matchers::WithinAbs(1920.0, 1e-9));
  REQUIRE(t.matrix.size() == 9);
  CHECK(t.rotation == 90);
  CHECK(t.sample_count == 90);
  CHECK(t.sample_entry_count == 1);
  CHECK(t.bitrate_avg_bps == 8'000'000);
  CHECK(t.bitrate_max_bps == 9'000'000);
  CHECK(t.edit_list_present);
  CHECK_FALSE(t.is_fragmented);
  CHECK_FALSE(t.is_encrypted);
  CHECK(t.avstream_index == -1);
  REQUIRE(t.references.size() == 1);
  CHECK(t.references[0]->type == "chap");
  CHECK(t.references[0]->track_ids == std::vector<uint32_t>{5});
  CHECK(t.trak_offset > 0);
}

TEST_CASE("tracks: bitrate is derived from a constant stsz without btrt", "[container][tracks][fast]") {
  // 90 samples of 1000 bytes over 3 s = 240 kbit/s.
  const Bytes data = mp4({trak(1, "vide", visual_entry("avc1", 640, 360, {avcc()}), 90)});
  Opened o(data);
  const auto t = track_of(o.container, 0);
  CHECK(t.bitrate_avg_bps == 240'000);
  CHECK(t.bitrate_max_bps == 0);
}

TEST_CASE("tracks: AV1 codec strings", "[container][tracks][fast]") {
  {
    const Bytes data = mp4({trak(1, "vide", visual_entry("av01", 1920, 1080, {av1c()}), 1)});
    Opened o(data);
    const auto t = track_of(o.container, 0);
    CHECK(t.codec == ovt::fb::Codec::AV1);
    CHECK(t.codec_string == "av01.0.08M.10");
  }
  {
    const Bytes data = mp4({trak(1, "vide", visual_entry("av01", 1920, 1080, {av1c(), colr_nclx(9, 16, 9, 0)}), 1)});
    Opened o(data);
    CHECK(track_of(o.container, 0).codec_string == "av01.0.08M.10.0.110.09.16.09.0");
  }
}

TEST_CASE("tracks: AAC audio entry", "[container][tracks][fast]") {
  const Bytes data = mp4({trak(1, "soun", audio_entry("mp4a", 2, 48000, {esds_aac()}), 100, 48000, 96000)});
  Opened o(data);
  const auto t = track_of(o.container, 0);
  CHECK(t.handler == ovt::fb::HandlerType::Audio);
  CHECK(t.codec == ovt::fb::Codec::Unknown);
  CHECK(t.codec_string == "mp4a.40.2");
  CHECK(t.channel_count == 2);
  CHECK(t.sample_size == 16);
  CHECK_THAT(t.sample_rate, Catch::Matchers::WithinAbs(48000.0, 1e-9));
  REQUIRE(t.codec_private.size() == 1);
  CHECK(t.codec_private[0]->fourcc == "esds");
}

TEST_CASE("tracks: a file without moov has no tracks", "[container][tracks][fast]") {
  Bytes ftyp;
  put_str(ftyp, "isom");
  put(ftyp, 0, 4);
  Opened o(cat({box("ftyp", ftyp), box("mdat", Bytes(8, 0))}));
  CHECK(ovt_container_track_count(o.container) == 0);
  ovt_Buffer b{};
  CHECK(ovt_container_track_info(o.container, 0, &b) == OVT_E_INVALID_ARG);
}

TEST_CASE("tracks: a tree with a trailing-bytes node still counts its traks", "[container][tracks][fast]") {
  // Five bytes after the last box become a node with no fourcc; the corpus walk below met one in
  // mpegff_meta_2500000bps_0 and dereferenced the null fourcc() (SIGSEGV on the Linux nightly).
  const Bytes file = mp4({trak(1, "vide", visual_entry("avc1", 640, 360, {avcc()}), 90)});
  const Bytes data = cat({file, Bytes{1, 2, 3, 4, 5}});
  Opened o(data);
  const ovt_Buffer tree = ovt_container_tree(o.container);
  const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data);
  REQUIRE(t->root() != nullptr);
  bool has_unnamed = false;
  for (const auto* n : *t->root()) {
    has_unnamed = has_unnamed || n->fourcc() == nullptr;
  }
  CHECK(has_unnamed);
  CHECK(count_boxes(t->root(), "trak") == 1);
  CHECK(ovt_container_track_count(o.container) == 1);
}

TEST_CASE("tracks: ovt_container_track_info returns the TrackInfo FlatBuffer", "[container][tracks][c_api][fast]") {
  const Bytes data = mp4({trak(4, "vide", visual_entry("hvc1", 1920, 1080, {hvcc()}), 12),
                          trak(5, "soun", audio_entry("mp4a", 2, 44100, {esds_aac()}), 12)});
  Opened o(data);
  REQUIRE(ovt_container_track_count(o.container) == 2);
  ovt_Buffer b{};
  REQUIRE(ovt_container_track_info(o.container, 1, &b) == OVT_OK);
  flatbuffers::Verifier v(b.data, b.size);
  REQUIRE(v.VerifyBuffer<ovt::fb::TrackInfo>(nullptr));
  const auto* t = flatbuffers::GetRoot<ovt::fb::TrackInfo>(b.data);
  CHECK(t->index() == 1);
  CHECK(t->track_id() == 5);
  CHECK(t->codec_string()->string_view() == "mp4a.40.2");
  delete[] b.data;  // §12.3.1 rule 3a: caller-owned buffers are new[]; status.cpp is not linked into this test

  CHECK(ovt_container_track_info(o.container, 2, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_track_info(o.container, -1, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_track_info(nullptr, 0, &b) == OVT_E_INVALID_ARG);
  CHECK(ovt_container_track_info(o.container, 0, nullptr) == OVT_E_INVALID_ARG);
}

// ---- corpus (§8.1.3; WI-1.217 acceptance tests 1 and 2, step 5) ----

TEST_CASE("tracks on syn_mp4_cenc: encv with original_format avc1 (AT2)", "[container][tracks][needs-corpus][fast]") {
  const auto path = ovt::test::corpus_or_skip("syn_mp4_cenc");
  ovt_File handle{ovt::io::RandomAccessFile::open(path.string())};
  ovt_Container* c = nullptr;
  REQUIRE(ovt_container_open(&handle, &c) == OVT_OK);
  bool found = false;
  for (int i = 0; i < ovt_container_track_count(c); ++i) {
    const auto t = track_of(c, i);
    if (t.handler == ovt::fb::HandlerType::Video) {
      CHECK(t.is_encrypted);
      CHECK(t.original_format == "avc1");
      found = true;
    }
  }
  CHECK(found);
  ovt_container_close(c);
}

// Spec (§14.3, pass 26, #769): dv_p81.mp4 is the MP4Box `dv-profile=8.1` remux of hdr_hdr10plus_dv81 made by the corpus
// fetcher's `post` step, which does not exist yet; until then the entry is the Matroska file and this test skips.
TEST_CASE("tracks on dv_p81: hvc1 with hvcC then dvvC (AT1)", "[container][tracks][needs-corpus][fast]") {
  const auto path = ovt::test::corpus_or_skip("hdr_hdr10plus_dv81");
  ovt_File handle{ovt::io::RandomAccessFile::open(path.string())};
  ovt_Container* c = nullptr;
  if (ovt_container_open(&handle, &c) != OVT_OK || std::string(ovt_container_format(c)) != "isobmff") {
    ovt_container_close(c);  // the Matroska file is not readable in phase 1
    SKIP("hdr_hdr10plus_dv81 is still the Matroska file; waiting for the #769 remux");
  }
  REQUIRE(ovt_container_track_count(c) >= 1);
  const auto t = track_of(c, 0);
  CHECK(t.codec == ovt::fb::Codec::H265);
  CHECK(t.sample_entry_fourcc == "hvc1");
  REQUIRE(t.codec_private.size() >= 2);
  CHECK(t.codec_private[0]->fourcc == "hvcC");
  CHECK(t.codec_private[1]->fourcc == "dvvC");
  ovt_container_close(c);
}

TEST_CASE("tracks: every trak of every ISOBMFF corpus file yields a verified TrackInfo",
          "[container][tracks][needs-corpus][fast]") {
  std::ifstream in(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/manifest.json");
  REQUIRE(in);
  const auto manifest = nlohmann::json::parse(in);
  size_t checked = 0;
  for (const auto& entry : manifest.at("entries")) {
    const std::string id = entry.at("id");
    const auto& containers = entry.value("containers", nlohmann::json::array());
    bool iso = std::ranges::find(containers, "isobmff") != containers.end();
    const auto files = entry.value("files", nlohmann::json::object());
    for (const auto& [name, hash] : files.items()) {
      (void)hash;
      iso = iso || name.ends_with(".mp4") || name.ends_with(".m4a") || name.ends_with(".mov");
    }
    if (!iso) {
      continue;
    }
    const auto path = ovt::test::corpus_path(id);
    if (!path) {
      if (ovt::test::require_corpora()) {
        (void)ovt::test::corpus_or_skip(id);  // fails with "corpus file missing"
      }
      continue;
    }
    DYNAMIC_SECTION(id) {
      ovt_File handle{ovt::io::RandomAccessFile::open(path->string())};
      ovt_Container* c = nullptr;
      if (ovt_container_open(&handle, &c) != OVT_OK) {
        continue;  // not readable as a container (broken-file entries); the tree tests cover those
      }
      if (std::string(ovt_container_format(c)) == "isobmff") {
        const ovt_Buffer tree = ovt_container_tree(c);
        const auto* t = flatbuffers::GetRoot<ovt::fb::ContainerTree>(tree.data);
        const int count = ovt_container_track_count(c);
        CHECK(static_cast<size_t>(count) == count_boxes(t->root(), "trak"));
        for (int i = 0; i < count; ++i) {
          ovt_Buffer b{};
          REQUIRE(ovt_container_track_info(c, i, &b) == OVT_OK);
          flatbuffers::Verifier v(b.data, b.size);
          CHECK(v.VerifyBuffer<ovt::fb::TrackInfo>(nullptr));
          delete[] b.data;  // §12.3.1 rule 3a
        }
        ++checked;
      }
      ovt_container_close(c);
    }
  }
  if (checked == 0) {
    SKIP("no ISOBMFF corpus file present");
  }
}
