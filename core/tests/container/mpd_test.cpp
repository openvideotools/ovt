// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the DASH MPD parser and segment URL expansion (chapter 08 §8.10, WI-4.208).

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
#include "container/virtual_file/mpd.hpp"
#include "ovt_container_generated.h"

namespace {

namespace mpd = ovt::container::mpd;
using ovt::container::DiagCode;

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string data_path(const std::string& name) {
  return std::string(OVT_TEST_SOURCE_DIR) + "/core/tests/data/dash/" + name;
}

bool has_diag(const mpd::Manifest& m, DiagCode code) {
  return std::any_of(m.diagnostics.begin(), m.diagnostics.end(), [code](const auto& d) { return d.code == code; });
}

// A one-period MPD around `body`; `period_attrs` and `mpd_attrs` extend the opening tags.
std::string wrap(const std::string& body, const std::string& period_attrs = R"(duration="PT20S")",
                 const std::string& mpd_attrs = "") {
  return R"(<?xml version="1.0"?><MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" )" + mpd_attrs +
         R"(><Period id="p" )" + period_attrs + R"(><AdaptationSet mimeType="video/mp4">)" + body +
         "</AdaptationSet></Period></MPD>";
}

const mpd::Representation& only_rep(const mpd::ParseResult& r) {
  REQUIRE(r.ok);
  REQUIRE(r.manifest.periods.size() == 1);
  REQUIRE(r.manifest.periods[0].adaptation_sets.size() == 1);
  REQUIRE(r.manifest.periods[0].adaptation_sets[0].representations.size() == 1);
  return r.manifest.periods[0].adaptation_sets[0].representations[0];
}

}  // namespace

TEST_CASE("mpd: segment URLs equal the reference expansion", "[container][dash][fast]") {
  const std::string url = "https://example.invalid/dash/bbb_30fps_like.mpd";
  const auto result = mpd::parse(read_file(data_path("bbb_30fps_like.mpd")), {url, {}});
  REQUIRE(result.ok);
  const auto expected =
      nlohmann::json::parse(read_file(data_path("bbb_30fps_like.expected.json")));  // tools/ref_mpd.py

  const auto& periods = result.manifest.periods;
  REQUIRE(periods.size() == expected["periods"].size());
  for (size_t pi = 0; pi < periods.size(); ++pi) {
    const auto& ep = expected["periods"][pi];
    CHECK(periods[pi].id == ep["id"].get<std::string>());
    CHECK(periods[pi].start_ms == ep["start_ms"].get<int64_t>());
    CHECK(periods[pi].duration_ms == ep["duration_ms"].get<int64_t>());
    REQUIRE(periods[pi].adaptation_sets.size() == ep["adaptation_sets"].size());
    for (size_t ai = 0; ai < periods[pi].adaptation_sets.size(); ++ai) {
      const auto& ea = ep["adaptation_sets"][ai];
      const auto& reps = periods[pi].adaptation_sets[ai].representations;
      REQUIRE(reps.size() == ea["representations"].size());
      for (size_t ri = 0; ri < reps.size(); ++ri) {
        const auto& er = ea["representations"][ri];
        CHECK(reps[ri].timescale == er["timescale"].get<uint32_t>());
        REQUIRE(reps[ri].segments.size() == er["segments"].size());
        for (size_t si = 0; si < reps[ri].segments.size(); ++si) {
          const auto& es = er["segments"][si];
          const auto& s = reps[ri].segments[si];
          INFO(reps[ri].id << " segment " << si);
          CHECK(s.url == es["url"].get<std::string>());
          CHECK(s.number == es["number"].get<int64_t>());
          CHECK(s.start_time == es["start_time"].get<int64_t>());
          CHECK(s.duration == es["duration"].get<int64_t>());
          CHECK(s.is_init == es["init"].get<bool>());
        }
      }
    }
  }
}

TEST_CASE("mpd: fixture attributes, content type and ContentProtection", "[container][dash][fast]") {
  const auto result = mpd::parse(read_file(data_path("bbb_30fps_like.mpd")), {"https://example.invalid/m.mpd", {}});
  REQUIRE(result.ok);
  const auto& sets = result.manifest.periods[0].adaptation_sets;
  CHECK(sets[0].content_type == "video");
  CHECK(sets[1].content_type == "audio");
  CHECK(sets[1].lang == "en");
  const auto& v = sets[0].representations[0];
  CHECK(v.codecs == "avc1.640028");
  CHECK(v.width == 1920);
  CHECK(v.frame_rate == "30");
  REQUIRE(v.encryption.size() == 1);
  CHECK(v.encryption[0].method == "cenc");
  CHECK(v.encryption[0].default_kid == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  CHECK(result.manifest.encryption.size() == 1);  // the same entry on both video representations
  CHECK(sets[1].representations[0].encryption.empty());
}

TEST_CASE("mpd: r = -1 fills the period exactly", "[container][dash][fast]") {
  // 20 s at timescale 1000, segments of 4 s after one 2 s segment and an explicit gap-free restart.
  const auto r = mpd::parse(
      wrap("<Representation id=\"v\" bandwidth=\"1\"><SegmentTemplate timescale=\"1000\" media=\"$Time$.m4s\">"
           "<SegmentTimeline><S t=\"0\" d=\"2000\"/><S d=\"3000\" r=\"-1\"/></SegmentTimeline></SegmentTemplate>"
           "</Representation>"),
      {"http://h/m.mpd", {}});
  const auto& rep = only_rep(r);
  // 2000 + 6 * 3000 = 20000: the last segment ends exactly at the period end.
  REQUIRE(rep.segments.size() == 7);
  CHECK(rep.segments.back().start_time + rep.segments.back().duration == 20000);
  CHECK(rep.segments.back().url == "http://h/17000.m4s");
  for (size_t i = 1; i < rep.segments.size(); ++i) {
    CHECK(rep.segments[i].start_time == rep.segments[i - 1].start_time + rep.segments[i - 1].duration);
  }
}

TEST_CASE("mpd: r = -1 stops at the next explicit S@t", "[container][dash][fast]") {
  const auto r = mpd::parse(
      wrap("<Representation id=\"v\"><SegmentTemplate timescale=\"10\" media=\"$Number$.m4s\" startNumber=\"5\">"
           "<SegmentTimeline><S t=\"0\" d=\"10\" r=\"-1\"/><S t=\"50\" d=\"10\" r=\"1\"/></SegmentTimeline>"
           "</SegmentTemplate></Representation>"),
      {"", {}});
  const auto& rep = only_rep(r);
  REQUIRE(rep.segments.size() == 7);  // 5 up to t=50, then 2
  CHECK(rep.segments[0].number == 5);
  CHECK(rep.segments[5].start_time == 50);
  CHECK(rep.segments[5].url == "10.m4s");
  CHECK(rep.segments[6].number == 11);
}

TEST_CASE("mpd: r = -1 without a period duration warns and keeps one segment", "[container][dash][fast]") {
  const auto r = mpd::parse(
      wrap("<Representation id=\"v\"><SegmentTemplate timescale=\"1\" media=\"$Number$.m4s\"><SegmentTimeline>"
           "<S t=\"0\" d=\"2\" r=\"-1\"/></SegmentTimeline></SegmentTemplate></Representation>",
           ""),
      {"", {}});
  const auto& rep = only_rep(r);
  CHECK(rep.segments.size() == 1);
  CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0004));
}

TEST_CASE("mpd: hostile repeat counts are capped", "[container][dash][fast]") {
  const auto r =
      mpd::parse(wrap("<Representation id=\"v\"><SegmentTemplate media=\"$Number$.m4s\"><SegmentTimeline>"
                      "<S t=\"0\" d=\"1\" r=\"4000000000\"/></SegmentTimeline></SegmentTemplate></Representation>"),
                 {"", {}});
  const auto& rep = only_rep(r);
  CHECK(rep.segments.size() == 1'000'000);
  CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0004));
}

TEST_CASE("mpd: $Number$ starts at startNumber (default 1) and honours %0Nd", "[container][dash][fast]") {
  const auto def = mpd::parse(
      wrap("<Representation id=\"v\"><SegmentTemplate timescale=\"1\" duration=\"10\" media=\"s$Number%04d$\"/>"
           "</Representation>"),
      {"", {}});
  const auto& rep = only_rep(def);
  REQUIRE(rep.segments.size() == 2);
  CHECK(rep.segments[0].url == "s0001");
  CHECK(rep.segments[1].url == "s0002");

  const auto zero =
      mpd::parse(wrap("<Representation id=\"v\"><SegmentTemplate startNumber=\"0\" timescale=\"1\" duration=\"10\" "
                      "media=\"s$Number$\"/></Representation>"),
                 {"", {}});
  CHECK(only_rep(zero).segments[0].url == "s0");
}

TEST_CASE("mpd: presentationTimeOffset shifts $Time$", "[container][dash][fast]") {
  const auto r = mpd::parse(wrap("<Representation id=\"v\"><SegmentTemplate timescale=\"100\" duration=\"1000\" "
                                 "presentationTimeOffset=\"500\" "
                                 "media=\"$Time$\"/></Representation>"),
                            {"", {}});
  const auto& rep = only_rep(r);
  REQUIRE(rep.segments.size() == 2);
  CHECK(rep.segments[0].url == "500");
  CHECK(rep.segments[1].url == "1500");
}

TEST_CASE("mpd: template identifiers", "[container][dash][fast]") {
  const mpd::TemplateValues v{"vid", 7, 250000, 123456789012};
  CHECK(mpd::expand_template("$RepresentationID$/$Bandwidth$/$Number%05d$/$Time$.m4s$$", v) ==
        "vid/250000/00007/123456789012.m4s$");
  CHECK_FALSE(mpd::expand_template("$Bogus$", v));
  CHECK_FALSE(mpd::expand_template("$Number%5d$", v));
  CHECK_FALSE(mpd::expand_template("$RepresentationID%05d$", v));
  CHECK_FALSE(mpd::expand_template("unterminated$Number", v));
}

TEST_CASE("mpd: an invalid template is reported and used as written", "[container][dash][fast]") {
  const auto r =
      mpd::parse(wrap("<Representation id=\"v\"><SegmentTemplate timescale=\"1\" duration=\"20\" media=\"a$Nope$\"/>"
                      "</Representation>"),
                 {"", {}});
  CHECK(only_rep(r).segments[0].url == "a$Nope$");
  CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0005));
}

TEST_CASE("mpd: RFC 3986 reference resolution", "[container][dash][fast]") {
  const std::string base = "http://a/b/c/d;p?q";
  CHECK(mpd::resolve_url(base, "g") == "http://a/b/c/g");
  CHECK(mpd::resolve_url(base, "./g") == "http://a/b/c/g");
  CHECK(mpd::resolve_url(base, "g/") == "http://a/b/c/g/");
  CHECK(mpd::resolve_url(base, "/g") == "http://a/g");
  CHECK(mpd::resolve_url(base, "//g") == "http://g");
  CHECK(mpd::resolve_url(base, "?y") == "http://a/b/c/d;p?y");
  CHECK(mpd::resolve_url(base, "g?y") == "http://a/b/c/g?y");
  CHECK(mpd::resolve_url(base, "") == "http://a/b/c/d;p?q");
  CHECK(mpd::resolve_url(base, "..") == "http://a/b/");
  CHECK(mpd::resolve_url(base, "../g") == "http://a/b/g");
  CHECK(mpd::resolve_url(base, "../../g") == "http://a/g");
  CHECK(mpd::resolve_url(base, "../../../g") == "http://a/g");
  CHECK(mpd::resolve_url(base, "g/./h") == "http://a/b/c/g/h");
  CHECK(mpd::resolve_url(base, "g/../h") == "http://a/b/c/h");
  CHECK(mpd::resolve_url(base, "https://x/y") == "https://x/y");
  CHECK(mpd::resolve_url("/local/dir/m.mpd", "seg/1.m4s") == "/local/dir/seg/1.m4s");
  CHECK(mpd::resolve_url("m.mpd", "seg.m4s") == "seg.m4s");
  CHECK(mpd::resolve_url("", "seg.m4s") == "seg.m4s");
}

TEST_CASE("mpd: BaseURL chain and namespace-prefixed documents", "[container][dash][fast]") {
  const std::string xml =
      "<d:MPD xmlns:d=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" mediaPresentationDuration=\"PT10S\">"
      "<d:BaseURL>cdn/</d:BaseURL><d:Period><d:BaseURL>../p/</d:BaseURL><d:AdaptationSet mimeType=\"audio/mp4\">"
      "<d:BaseURL>a/</d:BaseURL><d:Representation id=\"r\" bandwidth=\"9\"><d:BaseURL>r/</d:BaseURL>"
      "<d:SegmentTemplate timescale=\"1\" duration=\"10\" media=\"$RepresentationID$.m4s\"/>"
      "</d:Representation></d:AdaptationSet></d:Period></d:MPD>";
  const auto r = mpd::parse(xml, {"http://h/x/m.mpd", {}});
  const auto& rep = only_rep(r);
  REQUIRE(rep.segments.size() == 1);
  CHECK(rep.segments[0].url == "http://h/x/p/a/r/r.m4s");
  CHECK(r.manifest.periods[0].duration_ms == 10000);  // from mediaPresentationDuration
  CHECK(r.manifest.periods[0].adaptation_sets[0].content_type == "audio");
}

TEST_CASE("mpd: period start and duration derivation", "[container][dash][fast]") {
  const std::string xml =
      "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" mediaPresentationDuration=\"PT1M\">"
      "<Period id=\"a\" duration=\"PT10S\"/><Period id=\"b\"/><Period id=\"c\" start=\"PT30.5S\"/></MPD>";
  const auto r = mpd::parse(xml, {});
  REQUIRE(r.ok);
  const auto& p = r.manifest.periods;
  REQUIRE(p.size() == 3);
  CHECK(p[0].start_ms == 0);
  CHECK(p[0].duration_ms == 10000);
  CHECK(p[1].start_ms == 10000);
  CHECK(p[1].duration_ms == 20500);  // up to the next explicit start
  CHECK(p[2].start_ms == 30500);
  CHECK(p[2].duration_ms == 29500);  // up to mediaPresentationDuration
}

TEST_CASE("mpd: SegmentTemplate attributes inherit from AdaptationSet", "[container][dash][fast]") {
  const std::string xml =
      "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\"><Period duration=\"PT8S\">"
      "<AdaptationSet mimeType=\"video/mp4\" codecs=\"hvc1\"><SegmentTemplate timescale=\"1\" duration=\"4\" "
      "media=\"$RepresentationID$-$Number$.m4s\" initialization=\"$RepresentationID$-i.mp4\"/>"
      "<Representation id=\"x\" bandwidth=\"1\"/><Representation id=\"y\" bandwidth=\"2\">"
      "<SegmentTemplate duration=\"8\"/></Representation></AdaptationSet></Period></MPD>";
  const auto r = mpd::parse(xml, {});
  REQUIRE(r.ok);
  const auto& reps = r.manifest.periods[0].adaptation_sets[0].representations;
  CHECK(reps[0].segments.size() == 3);  // init + 2
  CHECK(reps[0].codecs == "hvc1");
  CHECK(reps[1].segments.size() == 2);  // init + 1: the Representation overrides @duration only
  CHECK(reps[1].segments[1].url == "y-1.m4s");
}

TEST_CASE("mpd: SegmentList", "[container][dash][fast]") {
  const auto r = mpd::parse(
      wrap("<BaseURL>media.mp4</BaseURL><Representation id=\"l\"><SegmentList timescale=\"1000\" duration=\"4000\">"
           "<Initialization range=\"0-799\"/><SegmentURL mediaRange=\"800-1799\"/>"
           "<SegmentURL media=\"seg2.m4s\"/></SegmentList></Representation>"),
      {"http://h/d/m.mpd", {}});
  const auto& rep = only_rep(r);
  REQUIRE(rep.segments.size() == 3);
  CHECK(rep.segments[0].is_init);
  CHECK(rep.segments[0].url == "http://h/d/media.mp4");
  CHECK(rep.segments[0].byte_range.offset == 0);
  CHECK(rep.segments[0].byte_range.length == 800);
  CHECK(rep.segments[1].byte_range.offset == 800);
  CHECK(rep.segments[1].byte_range.length == 1000);
  CHECK(rep.segments[1].start_time == 0);
  CHECK(rep.segments[2].url == "http://h/d/seg2.m4s");
  CHECK(rep.segments[2].start_time == 4000);
  CHECK(rep.segments[2].duration == 4000);
  CHECK(rep.segments[2].number == 2);
}

TEST_CASE("mpd: SegmentBase without and with a sidx resolver", "[container][dash][fast]") {
  const std::string body =
      "<BaseURL>v.mp4</BaseURL><Representation id=\"b\"><SegmentBase timescale=\"1000\" indexRange=\"1000-1099\">"
      "<Initialization range=\"0-999\"/></SegmentBase></Representation>";

  const auto plain = mpd::parse(wrap(body), {"http://h/m.mpd", {}});
  const auto& unresolved = only_rep(plain);
  REQUIRE(unresolved.segments.size() == 2);
  CHECK(unresolved.segments[0].is_init);
  CHECK(unresolved.segments[1].url == "http://h/v.mp4");
  CHECK(unresolved.segments[1].duration == 20000);
  CHECK(has_diag(plain.manifest, DiagCode::CNT_DASH_0003));

  mpd::Options options{"http://h/m.mpd", {}};
  mpd::ByteRange asked;
  std::string asked_url;
  options.sidx_resolver = [&](const std::string& url, mpd::ByteRange range) -> std::optional<mpd::SidxIndex> {
    asked = range;
    asked_url = url;
    mpd::SidxIndex idx;
    idx.timescale = 1000;
    idx.earliest_presentation_time = 0;
    idx.first_offset = 0;
    idx.references = {{5000, 4000}, {6000, 4000}, {7000, 2000}};
    return idx;
  };
  const auto resolved = mpd::parse(wrap(body), options);
  const auto& rep = only_rep(resolved);
  CHECK(asked_url == "http://h/v.mp4");
  CHECK(asked.offset == 1000);
  CHECK(asked.length == 100);
  REQUIRE(rep.segments.size() == 4);
  CHECK(rep.segments[1].byte_range.offset == 1100);  // right after the sidx box
  CHECK(rep.segments[1].byte_range.length == 5000);
  CHECK(rep.segments[2].byte_range.offset == 6100);
  CHECK(rep.segments[3].start_time == 8000);
  CHECK(rep.segments[3].duration == 2000);
  CHECK_FALSE(has_diag(resolved.manifest, DiagCode::CNT_DASH_0003));
}

TEST_CASE("mpd: a lone BaseURL is one segment; nothing at all is reported", "[container][dash][fast]") {
  const auto one =
      mpd::parse(wrap("<Representation id=\"s\"><BaseURL>sub.vtt</BaseURL></Representation>"), {"http://h/m.mpd", {}});
  const auto& rep = only_rep(one);
  REQUIRE(rep.segments.size() == 1);
  CHECK(rep.segments[0].url == "http://h/sub.vtt");

  const auto none = mpd::parse(wrap("<Representation id=\"s\"/>"), {});
  CHECK(only_rep(none).segments.empty());
  CHECK(has_diag(none.manifest, DiagCode::CNT_DASH_0006));
}

TEST_CASE("mpd: dynamic MPDs are snapshotted and xlink is ignored with Info", "[container][dash][fast]") {
  const std::string xml =
      "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" type=\"dynamic\" "
      "availabilityStartTime=\"2026-01-01T00:00:00Z\"><Period id=\"p\" xlink:href=\"http://h/remote.xml\" "
      "xlink:actuate=\"onLoad\" duration=\"PT4S\"><AdaptationSet><Representation id=\"r\">"
      "<SegmentTemplate timescale=\"1\" duration=\"2\" media=\"$Number$.m4s\"/></Representation></AdaptationSet>"
      "</Period></MPD>";
  const auto r = mpd::parse(xml, {});
  REQUIRE(r.ok);
  CHECK(r.manifest.dynamic);
  CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0001));
  CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0002));
  CHECK(r.manifest.periods[0].adaptation_sets[0].representations[0].segments.size() == 2);
}

TEST_CASE("mpd: not an MPD", "[container][dash][fast]") {
  for (const char* bad : {"", "not xml", "<a><b></a>", "<?xml version=\"1.0\"?><Other/>"}) {
    INFO(bad);
    const auto r = mpd::parse(bad, {});
    CHECK_FALSE(r.ok);
    CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0009));
  }
}

TEST_CASE("mpd: invalid attribute values are skipped with a warning", "[container][dash][fast]") {
  const auto r =
      mpd::parse(wrap("<Representation id=\"v\"><SegmentTemplate timescale=\"zero\" duration=\"10\" media=\"$Number$\">"
                      "</SegmentTemplate></Representation>",
                      "duration=\"PT10S\""),
                 {});
  CHECK(has_diag(r.manifest, DiagCode::CNT_DASH_0007));
  CHECK(only_rep(r).timescale == 1);
}

TEST_CASE("mpd: Manifest FlatBuffer round trip", "[container][dash][fast]") {
  const auto r = mpd::parse(read_file(data_path("bbb_30fps_like.mpd")), {"https://example.invalid/m.mpd", {}});
  REQUIRE(r.ok);
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(mpd::write_manifest(fbb, r.manifest));
  flatbuffers::Verifier verifier(fbb.GetBufferPointer(), fbb.GetSize());
  REQUIRE(verifier.VerifyBuffer<ovt::fb::Manifest>(nullptr));
  const auto* m = flatbuffers::GetRoot<ovt::fb::Manifest>(fbb.GetBufferPointer());
  CHECK(m->kind()->str() == "dash-mpd");
  CHECK(m->url()->str() == "https://example.invalid/m.mpd");
  REQUIRE(m->periods()->size() == 2);
  const auto* rep = m->periods()->Get(0)->adaptation_sets()->Get(0)->representations()->Get(0);
  CHECK(rep->id()->str() == "v_1080");
  CHECK(rep->bandwidth() == 4500000);
  CHECK(rep->timescale() == 30000);
  REQUIRE(rep->segments()->size() == 11);
  CHECK(rep->segments()->Get(0)->is_init());
  CHECK(rep->segments()->Get(1)->url()->str() == "https://example.invalid/dash/pkg/v_1080/seg-00001.m4s");
  CHECK(rep->segments()->Get(2)->start_time() == 120000);
  CHECK(rep->encryption()->Get(0)->default_kid()->size() == 16);
  CHECK(m->encryption()->size() == 1);
}
