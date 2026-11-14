// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the HLS M3U8 parser (chapter 08 §8.10, WI-4.209).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "container/diagnostics.h"
#include "container/virtual_file/m3u8.hpp"
#include "ovt_container_generated.h"

namespace {

namespace m3u8 = ovt::container::m3u8;
using ovt::container::DiagCode;

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string data_path(const std::string& name) {
  return std::string(OVT_TEST_SOURCE_DIR) + "/core/tests/data/hls/" + name;
}

std::string url_of(const std::string& name) {
  return "https://example.invalid/hls/" + name + ".m3u8";
}

m3u8::ParseResult parse_fixture(const std::string& name) {
  return m3u8::parse(read_file(data_path(name + ".m3u8")), {url_of(name)});
}

nlohmann::json expected(const std::string& name) {  // tools/ref_m3u8.py
  return nlohmann::json::parse(read_file(data_path(name + ".expected.json")));
}

bool has_diag(const m3u8::Manifest& m, DiagCode code) {
  return std::any_of(m.diagnostics.begin(), m.diagnostics.end(), [code](const auto& d) { return d.code == code; });
}

m3u8::ParseResult parse_text(const std::string& text) {
  return m3u8::parse(text, {"https://example.invalid/d/p.m3u8"});
}

void check_segments_equal_reference(const m3u8::Manifest& m, const nlohmann::json& ref) {
  const auto& segs = m.media.segments;
  const auto& want = ref["segments"];
  REQUIRE(segs.size() == want.size());
  for (size_t i = 0; i < segs.size(); ++i) {
    CAPTURE(i);
    CHECK(segs[i].url == want[i]["url"].get<std::string>());
    CHECK(segs[i].byte_range.offset == want[i]["offset"].get<uint64_t>());
    CHECK(segs[i].byte_range.length == want[i]["length"].get<uint64_t>());
    CHECK(segs[i].start_time == want[i]["start_time"].get<int64_t>());
    CHECK(segs[i].duration == want[i]["duration"].get<int64_t>());
    CHECK(segs[i].number == want[i]["number"].get<int64_t>());
    CHECK(segs[i].is_init == want[i]["is_init"].get<bool>());
  }
}

}  // namespace

TEST_CASE("m3u8: master playlist variants equal the reference parse", "[container][hls][fast]") {
  const auto result = parse_fixture("bipbop_adv_example_hevc_master");
  REQUIRE(result.ok);
  const auto& m = result.manifest;
  CHECK(m.is_master);
  const auto ref = expected("bipbop_adv_example_hevc_master");

  REQUIRE(m.variants.size() == ref["variants"].size());
  for (size_t i = 0; i < m.variants.size(); ++i) {
    const auto& v = m.variants[i];
    const auto& r = ref["variants"][i];
    CAPTURE(i);
    CHECK(v.uri == r["uri"].get<std::string>());
    CHECK(v.is_iframe == r["is_iframe"].get<bool>());
    CHECK(v.bandwidth == r["bandwidth"].get<uint32_t>());
    CHECK(v.average_bandwidth == r["average_bandwidth"].get<uint32_t>());
    CHECK(v.codecs == r["codecs"].get<std::string>());
    CHECK(v.width == r["width"].get<uint32_t>());
    CHECK(v.height == r["height"].get<uint32_t>());
    CHECK(v.video_range == r["video_range"].get<std::string>());
    CHECK((v.frame_rate.empty() ? 0.0 : std::stod(v.frame_rate)) == Catch::Approx(r["frame_rate"].get<double>()));
  }

  REQUIRE(m.renditions.size() == ref["renditions"].size());
  for (size_t i = 0; i < m.renditions.size(); ++i) {
    const auto& v = m.renditions[i];
    const auto& r = ref["renditions"][i];
    CAPTURE(i);
    CHECK(v.type == r["type"].get<std::string>());
    CHECK(v.group_id == r["group_id"].get<std::string>());
    CHECK(v.name == r["name"].get<std::string>());
    CHECK(v.language == r["language"].get<std::string>());
    CHECK(v.uri == r["uri"].get<std::string>());
    CHECK(v.is_default == r["default"].get<bool>());
    CHECK(v.autoselect == r["autoselect"].get<bool>());
  }
  CHECK(m.diagnostics.empty());
}

TEST_CASE("m3u8: Apple extensions CODECS, SUPPLEMENTAL-CODECS and VIDEO-RANGE", "[container][hls][fast]") {
  const auto m = parse_fixture("bipbop_adv_example_hevc_master").manifest;
  REQUIRE(m.variants.size() == 9);
  CHECK(m.variants[0].video_range == "SDR");
  CHECK(m.variants[3].video_range == "PQ");
  CHECK(m.variants[4].supplemental_codecs == "dvh1.08.06/db4h");
  CHECK(m.variants[5].video_range == "HLG");
  CHECK(m.variants[6].codecs == "mp4a.40.2");  // audio-only variant: no RESOLUTION
  CHECK(m.variants[7].is_iframe);
  CHECK(m.variants[7].uri == "https://example.invalid/hls/v5/iframe_index.m3u8");
}

TEST_CASE("m3u8: byterange playlist offsets continue from the previous entry", "[container][hls][fast]") {
  const auto result = parse_fixture("byterange");
  REQUIRE(result.ok);
  const auto& m = result.manifest;
  CHECK_FALSE(m.is_master);
  check_segments_equal_reference(m, expected("byterange"));

  // Hand-computed: map 1118@0, then 1000000@1118, 900000 (no @) at 1001118, 850000 at 1901118, 700000@5000000,
  // and 300000 continuing from 5700000.
  const auto& s = m.media.segments;
  REQUIRE(s.size() == 6);
  CHECK(s[0].is_init);
  CHECK(s[0].byte_range.offset == 0);
  CHECK(s[0].byte_range.length == 1118);
  CHECK(s[2].byte_range.offset == 1'001'118);
  CHECK(s[2].byte_range.length == 900'000);
  CHECK(s[3].byte_range.offset == 1'901'118);
  CHECK(s[5].byte_range.offset == 5'700'000);
  CHECK(s[5].byte_range.length == 300'000);
  CHECK_FALSE(has_diag(m, DiagCode::CNT_HLS_0004));
}

TEST_CASE("m3u8: fMP4 media playlist, init segments, keys, discontinuities", "[container][hls][fast]") {
  const auto result = parse_fixture("media_fmp4");
  REQUIRE(result.ok);
  const auto& m = result.manifest;
  check_segments_equal_reference(m, expected("media_fmp4"));

  const auto& p = m.media;
  CHECK(p.version == 7);
  CHECK(p.target_duration == 6);
  CHECK(p.media_sequence == 100);
  CHECK(p.discontinuity_sequence == 3);
  CHECK(p.playlist_type == "VOD");
  CHECK(p.end_list);
  CHECK(p.independent_segments);
  REQUIRE(p.segments.size() == 7);
  // #EXT-X-MAP applies to the following segments; it changes after the discontinuity.
  CHECK(p.segments[0].is_init);
  CHECK(p.segments[0].url == "https://example.invalid/hls/init_a.mp4");
  CHECK(p.segments[4].is_init);
  CHECK(p.segments[4].url == "https://example.invalid/hls/init_b.mp4");
  CHECK(p.segments[4].byte_range.length == 720);
  CHECK(p.segments[1].program_date_time == "2026-11-14T00:00:00.000Z");
  CHECK(p.segments[1].discontinuity_sequence == 3);
  CHECK(p.segments[5].discontinuity);
  CHECK(p.segments[5].discontinuity_sequence == 4);
  CHECK(p.segments[6].discontinuity_sequence == 4);
  // #EXT-X-KEY: segment 102 only; METHOD=NONE ends it.
  REQUIRE(p.keys.size() == 1);
  CHECK(p.keys[0].method == "AES-128");
  CHECK(p.keys[0].key_uri == "https://example.invalid/hls/keys/k1.bin");
  REQUIRE(p.keys[0].iv.size() == 16);
  CHECK(p.keys[0].iv[0] == 0x00);
  CHECK(p.keys[0].iv[1] == 0x01);
  CHECK(p.keys[0].iv[15] == 0x0F);
  CHECK(p.segments[2].key_index == -1);
  CHECK(p.segments[3].key_index == 0);
  CHECK(p.segments[5].key_index == -1);
  CHECK(has_diag(m, DiagCode::CNT_HLS_0008));
  CHECK_FALSE(has_diag(m, DiagCode::CNT_HLS_0007));
}

TEST_CASE("m3u8: short IV is left-padded and an odd digit count works", "[container][hls][fast]") {
  const auto r =
      parse_text("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\",IV=0xABC\n#EXTINF:1,\na.ts\n#EXT-X-ENDLIST\n");
  REQUIRE(r.ok);
  REQUIRE(r.manifest.media.keys.size() == 1);
  const auto& iv = r.manifest.media.keys[0].iv;
  REQUIRE(iv.size() == 16);
  CHECK(iv[14] == 0x0A);
  CHECK(iv[15] == 0xBC);
  CHECK(std::all_of(iv.begin(), iv.begin() + 14, [](uint8_t b) { return b == 0; }));

  const auto bad =
      parse_text("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0xZZ\n#EXTINF:1,\na.ts\n#EXT-X-ENDLIST\n");
  CHECK(bad.manifest.media.keys[0].iv.empty());
  CHECK(has_diag(bad.manifest, DiagCode::CNT_HLS_0003));
}

TEST_CASE("m3u8: not a playlist", "[container][hls][fast]") {
  const auto r = parse_text("<html>not a playlist</html>\n");
  CHECK_FALSE(r.ok);
  CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0001));
  CHECK_FALSE(parse_text("").ok);
}

TEST_CASE("m3u8: BOM, CRLF line ends, comments and blank lines", "[container][hls][fast]") {
  const auto r = parse_text(
      "\xEF\xBB\xBF#EXTM3U\r\n\r\n# a "
      "comment\r\n#EXT-X-TARGETDURATION:4\r\n#EXTINF:3.5,\r\nseg.ts\r\n#EXT-X-ENDLIST\r\n");
  REQUIRE(r.ok);
  REQUIRE(r.manifest.media.segments.size() == 1);
  CHECK(r.manifest.media.segments[0].url == "https://example.invalid/d/seg.ts");
  CHECK(r.manifest.media.segments[0].duration == 3'500'000);
  CHECK(r.manifest.diagnostics.empty());
}

TEST_CASE("m3u8: a quoted attribute value may contain commas", "[container][hls][fast]") {
  const auto r = parse_text(
      "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.4d401f,mp4a.40.2\",RESOLUTION=640x360\nv.m3u8\n");
  REQUIRE(r.ok);
  REQUIRE(r.manifest.variants.size() == 1);
  CHECK(r.manifest.variants[0].codecs == "avc1.4d401f,mp4a.40.2");
  CHECK(r.manifest.variants[0].width == 640);
  CHECK(r.manifest.variants[0].height == 360);
  CHECK(r.manifest.variants[0].uri == "https://example.invalid/d/v.m3u8");
}

TEST_CASE("m3u8: diagnostics for malformed input", "[container][hls][fast]") {
  SECTION("BYTERANGE without @ that follows another resource") {
    const auto r = parse_text(
        "#EXTM3U\n#EXTINF:1,\n#EXT-X-BYTERANGE:100@5\na.ts\n#EXTINF:1,\n#EXT-X-BYTERANGE:100\nb.ts\n#EXT-X-ENDLIST\n");
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0004));
    CHECK(r.manifest.media.segments[1].byte_range.offset == 0);
    CHECK(r.manifest.media.segments[1].byte_range.length == 100);
  }
  SECTION("URI without tag, tag without URI") {
    const auto r = parse_text("#EXTM3U\nstray.ts\n#EXTINF:1,\n");
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0006));
    CHECK(r.manifest.media.segments.empty());
  }
  SECTION("malformed attribute list and bad values") {
    const auto r = parse_text("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=abc,CODECS=\"x\nv.m3u8\n");
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0002));
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0003));
    REQUIRE(r.manifest.variants.size() == 1);
    CHECK(r.manifest.variants[0].bandwidth == 0);
  }
  SECTION("EXTINF with a bad duration keeps the segment") {
    const auto r = parse_text("#EXTM3U\n#EXTINF:-3,\na.ts\n#EXTINF:abc,\nb.ts\n#EXT-X-ENDLIST\n");
    REQUIRE(r.manifest.media.segments.size() == 2);
    CHECK(r.manifest.media.segments[0].duration == -1);
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0003));
  }
  SECTION("live playlist and target duration") {
    const auto r = parse_text("#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:9.9,\na.ts\n");
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0007));
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0010));
  }
  SECTION("master and media tags mixed") {
    const auto r = parse_text("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nv.m3u8\n#EXTINF:1,\na.ts\n");
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0009));
    CHECK(r.manifest.is_master);
  }
  SECTION("zero-length byte range is invalid") {
    const auto r = parse_text("#EXTM3U\n#EXTINF:1,\n#EXT-X-BYTERANGE:0@10\na.ts\n#EXT-X-ENDLIST\n");
    CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0003));
    CHECK(r.manifest.media.segments[0].byte_range.length == 0);
  }
}

TEST_CASE("m3u8: the segment list is capped", "[container][hls][fast]") {
  std::string text = "#EXTM3U\n";
  text.reserve(14'000'000);
  for (int i = 0; i < 1'000'050; ++i) {
    text += "#EXTINF:1,\ns\n";
  }
  const auto r = parse_text(text);
  REQUIRE(r.ok);
  CHECK(r.manifest.media.segments.size() <= 1'000'000);
  CHECK(has_diag(r.manifest, DiagCode::CNT_HLS_0005));
}

TEST_CASE("m3u8: parsing and serialising are deterministic", "[container][hls][fast]") {
  const std::string text = read_file(data_path("media_fmp4.m3u8"));
  const auto a = m3u8::parse(text, {url_of("media_fmp4")});
  const auto b = m3u8::parse(text, {url_of("media_fmp4")});
  flatbuffers::FlatBufferBuilder fa;
  flatbuffers::FlatBufferBuilder fb_;
  fa.Finish(m3u8::write_manifest(fa, a.manifest));
  fb_.Finish(m3u8::write_manifest(fb_, b.manifest));
  REQUIRE(fa.GetSize() == fb_.GetSize());
  CHECK(std::equal(fa.GetBufferPointer(), fa.GetBufferPointer() + fa.GetSize(), fb_.GetBufferPointer()));
}

TEST_CASE("m3u8: write_manifest of a media playlist", "[container][hls][fast]") {
  const auto r = parse_fixture("media_fmp4");
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(m3u8::write_manifest(fbb, r.manifest));
  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  REQUIRE(verifier.VerifyBuffer<ovt::fb::Manifest>(nullptr));
  const auto* mf = flatbuffers::GetRoot<ovt::fb::Manifest>(fbb.GetBufferPointer());
  CHECK(mf->kind()->str() == "hls-m3u8");
  CHECK(mf->url()->str() == url_of("media_fmp4"));
  REQUIRE(mf->periods()->size() == 1);
  const auto* period = mf->periods()->Get(0);
  CHECK(period->duration() == 26'515);  // 6.006 + 6.006 + 5.5 + 6 + 3.003 s
  REQUIRE(period->adaptation_sets()->size() == 1);
  const auto* rep = period->adaptation_sets()->Get(0)->representations()->Get(0);
  CHECK(rep->timescale() == m3u8::kTimescale);
  REQUIRE(rep->segments()->size() == 7);
  CHECK(rep->segments()->Get(0)->is_init());
  CHECK(rep->segments()->Get(1)->start_time() == 0);
  CHECK(rep->segments()->Get(1)->duration() == 6'006'000);
  CHECK(rep->segments()->Get(1)->number() == 100);
  CHECK(rep->segments()->Get(4)->byte_range()->length() == 720);
  REQUIRE(rep->encryption()->size() == 1);
  CHECK(rep->encryption()->Get(0)->method()->str() == "AES-128");
  CHECK(rep->encryption()->Get(0)->iv()->size() == 16);
  CHECK(mf->encryption()->size() == 1);
}

TEST_CASE("m3u8: write_manifest of a master playlist", "[container][hls][fast]") {
  const auto r = parse_fixture("bipbop_adv_example_hevc_master");
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(m3u8::write_manifest(fbb, r.manifest));
  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  REQUIRE(verifier.VerifyBuffer<ovt::fb::Manifest>(nullptr));
  const auto* mf = flatbuffers::GetRoot<ovt::fb::Manifest>(fbb.GetBufferPointer());
  CHECK(mf->kind()->str() == "hls-m3u8");
  const auto* sets = mf->periods()->Get(0)->adaptation_sets();
  std::vector<std::string> types;
  for (const auto* s : *sets) {
    types.push_back(s->content_type()->str());
  }
  CHECK(types == std::vector<std::string>{"video", "audio", "subtitles", "closed-captions"});
  const auto* video = sets->Get(0)->representations();
  REQUIRE(video->size() == 8);  // 7 video variants + 2 I-frame streams, minus the audio-only variant
  CHECK(video->Get(0)->id()->str() == "v0");
  CHECK(video->Get(0)->bandwidth() == 2'177'116);
  CHECK(video->Get(0)->width() == 960);
  CHECK(video->Get(0)->video_range()->str() == "SDR");
  CHECK(video->Get(0)->codecs()->str() == "hvc1.2.4.L93.B0,mp4a.40.2");
  CHECK(video->Get(3)->video_range()->str() == "PQ");
  CHECK(video->Get(0)->segments()->empty());
  const auto* audio = sets->Get(1)->representations();
  REQUIRE(audio->size() == 4);  // the audio-only variant + three EXT-X-MEDIA audio renditions
  CHECK(audio->Get(0)->id()->str() == "v6");
  CHECK(audio->Get(1)->id()->str() == "aud1/English");
  CHECK(sets->Get(1)->lang()->str().empty());  // en and fr: mixed
  CHECK(sets->Get(2)->lang()->str() == "en");
}
