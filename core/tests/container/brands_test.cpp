// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the brand and profile checks (chapter 08 §8.4.5, WI-1.222a). Every file is built in memory from boxes, so
// each row of the check table has a minimal file that triggers it and one that does not.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/brands.h"
#include "container/isobmff/lazy.h"
#include "io/memory_file.hpp"
#include "test_support/corpus.h"

namespace {

using Bytes = std::vector<uint8_t>;
namespace iso = ovt::container::isobmff;
using ovt::fb::BoxNodeT;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}

Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}

Bytes full(uint8_t version, uint32_t flags, const Bytes& rest = {}) {
  Bytes b{version, static_cast<uint8_t>(flags >> 16U), static_cast<uint8_t>(flags >> 8U), static_cast<uint8_t>(flags)};
  b.insert(b.end(), rest.begin(), rest.end());
  return b;
}

Bytes u32s(std::initializer_list<uint32_t> v) {
  Bytes b;
  for (const uint32_t x : v) {
    put32(b, x);
  }
  return b;
}

Bytes types(std::string_view major, std::initializer_list<std::string_view> compat,
            std::string_view box_type = "ftyp") {
  Bytes p(major.begin(), major.end());
  put32(p, 0);
  for (const std::string_view c : compat) {
    p.insert(p.end(), c.begin(), c.end());
  }
  return box(box_type, p);
}

// ---- sample entries ------------------------------------------------------------------------------------------------

Bytes visual_entry(std::string_view type, const Bytes& children) {
  Bytes p(6, 0);
  p.insert(p.end(), {0, 1});  // data_reference_index
  p.insert(p.end(), 16, 0);
  p.insert(p.end(), {0x01, 0x40, 0x00, 0xF0});  // 320x240
  put32(p, 0x00480000);
  put32(p, 0x00480000);
  put32(p, 0);
  p.insert(p.end(), {0, 1});
  p.insert(p.end(), 32, 0);
  p.insert(p.end(), {0, 0x18, 0xFF, 0xFF});
  p.insert(p.end(), children.begin(), children.end());
  return box(type, p);
}

Bytes audio_entry(std::string_view type, const Bytes& children) {
  Bytes p(6, 0);
  p.insert(p.end(), {0, 1});
  p.insert(p.end(), 8, 0);
  p.insert(p.end(), {0, 2, 0, 16, 0, 0, 0, 0, 0xBB, 0x80, 0, 0});
  p.insert(p.end(), children.begin(), children.end());
  return box(type, p);
}

Bytes avcc(uint8_t num_sps) {
  Bytes p{1, 100, 0, 40, 0xFF, static_cast<uint8_t>(0xE0U | num_sps)};
  for (unsigned i = 0; i < num_sps; ++i) {
    p.insert(p.end(), {0, 4, 0x67, 0x64, 0, 0x28});
  }
  p.insert(p.end(), {1, 0, 2, 0x68, 0xEE});
  return box("avcC", p);
}

// hvcC with one array per type in `array_types`; `complete` sets array_completeness.
Bytes hvcc(std::initializer_list<unsigned> array_types, bool complete = true) {
  Bytes p(22, 0);
  p[0] = 1;
  p[21] = 3;  // lengthSizeMinusOne
  p.push_back(static_cast<uint8_t>(array_types.size()));
  for (const unsigned t : array_types) {
    p.push_back(static_cast<uint8_t>((complete ? 0x80U : 0U) | t));
    p.insert(p.end(), {0, 1, 0, 2, 0x40, 0x01});
  }
  return box("hvcC", p);
}

Bytes av1c(unsigned sequence_headers) {
  Bytes p{0x81, 0x08, 0x0C, 0x00};
  for (unsigned i = 0; i < sequence_headers; ++i) {
    p.insert(p.end(), {0x0A, 0x01, 0x00});  // OBU_SEQUENCE_HEADER, has_size, size 1
  }
  return box("av1C", p);
}

// ---- movie ---------------------------------------------------------------------------------------------------------

struct TrackSpec {
  std::string handler = "vide";
  Bytes entry;  // the one sample entry of stsd
  uint32_t stsz_count = 0;
  uint32_t stts_entries = 0;
  Bytes extra_stbl;  // appended to stbl
  Bytes edts;        // whole edts box, or empty
};

Bytes trak(const TrackSpec& t) {
  Bytes hdlr_payload = full(0, 0, u32s({0}));
  hdlr_payload.insert(hdlr_payload.end(), t.handler.begin(), t.handler.end());
  hdlr_payload.insert(hdlr_payload.end(), 13, 0);
  const Bytes stsd = box("stsd", cat({full(0, 0, u32s({1})), t.entry}));
  Bytes stts_payload = full(0, 0, u32s({t.stts_entries}));
  for (uint32_t i = 0; i < t.stts_entries; ++i) {
    put32(stts_payload, 1);
    put32(stts_payload, 1);
  }
  const Bytes stbl = box("stbl", cat({stsd, box("stts", stts_payload), box("stsc", full(0, 0, u32s({0}))),
                                      box("stsz", full(0, 0, u32s({0, t.stsz_count}))),
                                      box("stco", full(0, 0, u32s({0}))), t.extra_stbl}));
  const Bytes minf = box("minf", cat({box("dinf"), stbl}));
  const Bytes mdia =
      box("mdia", cat({box("mdhd", full(0, 0, u32s({0, 0, 90000, 0, 0x55C40000}))), box("hdlr", hdlr_payload), minf}));
  return box("trak", cat({box("tkhd", Bytes(84, 0)), t.edts, mdia}));
}

// A track with one avc1 entry that carries an SPS.
TrackSpec video_track() {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(1));
  return t;
}

Bytes mvhd(uint32_t duration = 0) {
  return box("mvhd", full(0, 0, u32s({0, 0, 1000, duration, 0x00010000})));
}

Bytes mvex(bool with_trex = true) {
  return box("mvex", with_trex ? box("trex", full(0, 0, u32s({1, 1, 0, 0, 0}))) : Bytes{});
}

Bytes moof(bool tfdt = true, uint32_t tf_flags = 0x020000, unsigned trafs = 1) {
  Bytes traf = box(
      "traf", cat({box("tfhd", full(0, tf_flags, u32s({1}))), tfdt ? box("tfdt", full(0, 0, u32s({0}))) : Bytes{}}));
  Bytes kids = box("mfhd", full(0, 0, u32s({1})));
  for (unsigned i = 0; i < trafs; ++i) {
    kids.insert(kids.end(), traf.begin(), traf.end());
  }
  return box("moof", kids);
}

Bytes mdat() {
  return box("mdat", Bytes(16, 7));
}

Bytes sidx(uint32_t first_offset, std::initializer_list<uint32_t> sizes, bool hierarchical = false) {
  Bytes q = full(0, 0, u32s({1, 90000, 0, first_offset}));
  q.insert(q.end(), {0, 0, static_cast<uint8_t>(sizes.size() >> 8U), static_cast<uint8_t>(sizes.size())});
  for (const uint32_t s : sizes) {
    put32(q, s | (hierarchical ? 0x80000000U : 0U));
    put32(q, 90000);
    put32(q, 0x90000000U);
  }
  return box("sidx", q);
}

// ---- running the checks --------------------------------------------------------------------------------------------

void collect(const BoxNodeT& n, std::vector<std::pair<std::string, std::string>>& out) {
  for (const auto& d : n.diagnostics) {
    if (d->code.starts_with("CNT-70") || d->code.starts_with("CNT-80")) {
      out.emplace_back(d->code, d->message);
    }
  }
  for (const auto& c : n.children) {
    collect(*c, out);
  }
}

std::vector<std::pair<std::string, std::string>> run(const Bytes& data,
                                                     iso::ValidationLevel level = iso::ValidationLevel::Standard,
                                                     iso::BrandOptions options = {}) {
  const ovt::io::MemoryFile file(data);
  auto r = iso::walk(file);
  iso::check_brands(file, r.top_level, level, options);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& n : r.top_level) {
    collect(*n, out);
  }
  return out;
}

std::set<std::string> codes(const Bytes& data, iso::ValidationLevel level = iso::ValidationLevel::Standard,
                            iso::BrandOptions options = {}) {
  std::set<std::string> out;
  for (const auto& [code, message] : run(data, level, options)) {
    out.insert(code);
  }
  return out;
}

bool has(const std::set<std::string>& s, const char* code) {
  return s.contains(code);
}

// A clean CMAF track file: ftyp, an init moov (one video track, empty tables, mvex/trex), then fragments.
Bytes cmaf_track(std::string_view major = "cmfc", const Bytes& entry = {}, unsigned fragments = 2) {
  TrackSpec t;
  t.entry = entry.empty()
                ? visual_entry("avc1", cat({avcc(1), box("colr", Bytes{'n', 'c', 'l', 'x', 0, 1, 0, 1, 0, 1, 0x80}),
                                            box("btrt", Bytes(12, 0))}))
                : entry;
  Bytes b = cat({types(major, {"iso6", "cmfc", "cmfc", "cmfc"}), box("moov", cat({mvhd(), trak(t), mvex()}))});
  for (unsigned i = 0; i < fragments; ++i) {
    b = cat({b, moof(), mdat()});
  }
  return b;
}

}  // namespace

TEST_CASE("a clean CMAF track file raises no CNT-70xx finding", "[container][isobmff][brands][fast]") {
  CHECK(run(cmaf_track()).empty());
}

TEST_CASE("brands are evaluated once however often the compatible list repeats them",
          "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(1));
  const Bytes two_tracks = cat({types("cmfc", {"cmfc", "cmfc", "cmfc", "cmfc", "cmfc", "cmfc"}),
                                box("moov", cat({mvhd(), trak(t), trak(t), mvex()}))});
  const auto found = run(two_tracks);
  CHECK(std::ranges::count_if(found, [](const auto& f) { return f.first == "CNT-7010"; }) == 1);
}

TEST_CASE("cmfc with two traks raises CNT-7010", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(1));
  CHECK(has(codes(cat({types("cmfc", {"iso6"}), box("moov", cat({mvhd(), trak(t), trak(t), mvex()}))})), "CNT-7010"));
  CHECK_FALSE(has(codes(cat({types("cmfc", {"iso6"}), box("moov", cat({mvhd(), trak(t), mvex()}))})), "CNT-7010"));
}

TEST_CASE("cmfc moov rows: mvex/trex, mehd, empty tables, sdtp and stss", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(1));
  const auto file = [&](const Bytes& moov_kids) { return cat({types("cmfc", {"iso6"}), box("moov", moov_kids)}); };
  CHECK(has(codes(file(cat({mvhd(), trak(t)}))), "CNT-7011"));
  CHECK(has(codes(file(cat({mvhd(), trak(t), mvex(false)}))), "CNT-7011"));
  CHECK(has(codes(file(cat({mvhd(5000), trak(t), mvex()}))), "CNT-7012"));
  CHECK_FALSE(has(codes(file(cat({mvhd(5000), trak(t),
                                  box("mvex", cat({box("mehd", full(0, 0, u32s({5000}))),
                                                   box("trex", full(0, 0, u32s({1, 1, 0, 0, 0})))}))}))),
                  "CNT-7012"));
  TrackSpec full_tables = t;
  full_tables.stsz_count = 3;
  CHECK(has(codes(file(cat({mvhd(), trak(full_tables), mvex()}))), "CNT-7013"));
  full_tables.stsz_count = 0;
  full_tables.stts_entries = 1;
  CHECK(has(codes(file(cat({mvhd(), trak(full_tables), mvex()}))), "CNT-7013"));
  TrackSpec with_stss = t;
  with_stss.extra_stbl = box("stss", full(0, 0, u32s({0})));
  CHECK(has(codes(file(cat({mvhd(), trak(with_stss), mvex()}))), "CNT-7017"));
}

TEST_CASE("cmfc fragment rows: one traf, default-base-is-moof, tfdt", "[container][isobmff][brands][fast]") {
  const Bytes head = cmaf_track("cmfc", {}, 0);
  CHECK(has(codes(cat({head, moof(true, 0x020000, 2), mdat()})), "CNT-7014"));
  CHECK(has(codes(cat({head, moof(true, 0), mdat()})), "CNT-7015"));
  CHECK(has(codes(cat({head, moof(false), mdat()})), "CNT-7016"));
  const auto found = run(cat({head, moof(), mdat(), moof(false), mdat()}));
  const auto it = std::ranges::find_if(found, [](const auto& f) { return f.first == "CNT-7016"; });
  REQUIRE(it != found.end());
  CHECK(it->second.find("fragment 2") != std::string::npos);
}

TEST_CASE("cmfc video sample entries need colr and btrt", "[container][isobmff][brands][fast]") {
  const auto with = [](const Bytes& kids) {
    return codes(cmaf_track("cmfc", visual_entry("avc1", cat({avcc(1), kids}))));
  };
  CHECK(has(with({}), "CNT-7018"));
  CHECK(has(with({}), "CNT-7019"));
  CHECK_FALSE(has(with(box("colr", Bytes{'n', 'c', 'l', 'x', 0, 1, 0, 1, 0, 1, 0x80})), "CNT-7018"));
  CHECK_FALSE(has(with(box("btrt", Bytes(12, 0))), "CNT-7019"));
}

TEST_CASE("cmf2 rows: elst, cslg and in-band parameter sets", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", cat({avcc(1), box("colr", Bytes{'n', 'c', 'l', 'x', 0, 1, 0, 1, 0, 1, 0x80}),
                                      box("btrt", Bytes(12, 0))}));
  const auto cmf2 = [&](const TrackSpec& spec) {
    return cat({types("cmf2", {"iso6", "cmfc"}), box("moov", cat({mvhd(), trak(spec), mvex()}))});
  };
  CHECK(codes(cmf2(t)).empty());
  TrackSpec edit = t;
  edit.edts = box("edts", box("elst", full(0, 0, u32s({1, 1000, 512, 0x00010000}))));
  CHECK(has(codes(cmf2(edit)), "CNT-7020"));
  edit.edts = box("edts", box("elst", full(0, 0, u32s({1, 1000, 0xFFFFFFFFU, 0x00010000}))));
  CHECK_FALSE(has(codes(cmf2(edit)), "CNT-7020"));
  TrackSpec cslg = t;
  cslg.extra_stbl = box("cslg", full(0, 0, u32s({0, 0, 0, 0, 0})));
  CHECK(has(codes(cmf2(cslg)), "CNT-7021"));
  TrackSpec in_band = t;
  in_band.entry = visual_entry("avc3", cat({avcc(0), box("colr", Bytes{'n', 'c', 'l', 'x', 0, 1, 0, 1, 0, 1, 0x80}),
                                            box("btrt", Bytes(12, 0))}));
  CHECK(has(codes(cmf2(in_band)), "CNT-7022"));
}

TEST_CASE("cmfs, cmff and cmfl structure rows", "[container][isobmff][brands][fast]") {
  const Bytes init = cat({types("iso6", {"iso6"}), box("moov", cat({mvhd(), trak(video_track()), mvex()}))});
  const Bytes styp = types("msdh", {"cmfs", "cmff"}, "styp");
  // cmfs declared, moof present, no styp.
  CHECK(has(codes(cat({types("cmfs", {"iso6"}), moof(), mdat()})), "CNT-7023"));
  CHECK_FALSE(has(codes(cat({styp, moof(), mdat()})), "CNT-7023"));
  // sidx after the moof of the same segment.
  CHECK(has(codes(cat({styp, moof(), mdat(), sidx(0, {16})})), "CNT-7024"));
  CHECK_FALSE(has(codes(cat({styp, sidx(0, {16}), moof(), mdat()})), "CNT-7024"));
  // a cmfl chunk with two moofs.
  const Bytes chunk = types("msdh", {"cmfl"}, "styp");
  CHECK(has(codes(cat({chunk, moof(), mdat(), moof(), mdat()})), "CNT-7024"));
  CHECK_FALSE(has(codes(cat({chunk, moof(), mdat()})), "CNT-7024"));
  // emsg and prft only before the moof.
  CHECK(has(codes(cat({styp, moof(), box("emsg", full(0, 0, Bytes(20, 0))), mdat()})), "CNT-7025"));
  CHECK_FALSE(has(codes(cat({styp, box("emsg", full(0, 0, Bytes(20, 0))), moof(), mdat()})), "CNT-7025"));
  static_cast<void>(init);
}

TEST_CASE("dash brand: self-initialising segments", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(1));
  CHECK_FALSE(has(codes(cat({types("dash", {"iso6"}), box("moov", cat({mvhd(), trak(t), mvex()}))})), "CNT-7030"));
  CHECK(has(codes(cat({types("dash", {"iso6"}), box("moov", cat({mvhd(), trak(t)}))})), "CNT-7030"));
  t.stsz_count = 10;
  CHECK(has(codes(cat({types("dash", {"iso6"}), box("moov", cat({mvhd(), trak(t), mvex()}))})), "CNT-7030"));
}

TEST_CASE("msix without sidx raises CNT-7031", "[container][isobmff][brands][fast]") {
  const Bytes head = cat({types("msix", {"iso6"}), box("moov", cat({mvhd(), trak(video_track()), mvex()}))});
  CHECK(has(codes(cat({head, moof(), mdat()})), "CNT-7031"));
  const Bytes moof_and_mdat = cat({moof(), mdat()});
  const auto indexed = codes(cat({head, sidx(0, {static_cast<uint32_t>(moof_and_mdat.size())}), moof_and_mdat}));
  CHECK_FALSE(has(indexed, "CNT-7031"));
  CHECK_FALSE(has(indexed, "CNT-7032"));
}

TEST_CASE("msix: a moof no sidx reference covers raises CNT-7032", "[container][isobmff][brands][fast]") {
  const Bytes head = cat({types("msix", {"iso6"}), box("moov", cat({mvhd(), trak(video_track()), mvex()}))});
  const Bytes one = cat({moof(), mdat()});
  const auto found = run(cat({head, sidx(0, {static_cast<uint32_t>(one.size())}), one, one}));
  const auto n = std::ranges::count_if(found, [](const auto& f) { return f.first == "CNT-7032"; });
  CHECK(n == 1);
  // A reference to another sidx (hierarchical index) cannot be followed here, so nothing is reported.
  CHECK_FALSE(has(codes(cat({head, sidx(0, {1000}, true), one})), "CNT-7032"));
}

TEST_CASE("lmsg on a segment that is not the last raises CNT-7033", "[container][isobmff][brands][fast]") {
  const Bytes seg_a = types("msdh", {"lmsg"}, "styp");
  const Bytes seg_b = types("msdh", {"msix"}, "styp");
  CHECK(has(codes(cat({seg_a, moof(), mdat(), seg_b, moof(), mdat()})), "CNT-7033"));
  CHECK_FALSE(has(codes(cat({seg_b, moof(), mdat(), seg_a, moof(), mdat()})), "CNT-7033"));
}

TEST_CASE("avc1 and avc2 need a sequence parameter set in avcC", "[container][isobmff][brands][fast]") {
  const auto file = [](std::string_view entry, uint8_t sps) {
    TrackSpec t;
    t.entry = visual_entry(entry, avcc(sps));
    return cat({types("isom", {"iso2"}), box("moov", cat({mvhd(), trak(t)}))});
  };
  CHECK(has(codes(file("avc1", 0)), "CNT-7003"));
  CHECK(has(codes(file("avc2", 0)), "CNT-7003"));
  CHECK_FALSE(has(codes(file("avc3", 0)), "CNT-7003"));
  CHECK_FALSE(has(codes(file("avc1", 1)), "CNT-7003"));
}

TEST_CASE("hvc1 hvcC arrays must hold VPS, SPS and PPS", "[container][isobmff][brands][fast]") {
  const auto file = [](std::string_view entry, const Bytes& config) {
    TrackSpec t;
    t.entry = visual_entry(entry, config);
    return cat({types("isom", {"iso2"}), box("moov", cat({mvhd(), trak(t)}))});
  };
  CHECK(has(codes(file("hvc1", hvcc({}))), "CNT-7005"));
  CHECK(has(codes(file("hvc1", hvcc({33, 34}))), "CNT-7005"));
  CHECK_FALSE(has(codes(file("hvc1", hvcc({32, 33, 34}))), "CNT-7005"));
  CHECK_FALSE(has(codes(file("hev1", hvcc({}))), "CNT-7005"));
  CHECK_FALSE(has(codes(file("hvc1", hvcc({32, 33, 34}))), "CNT-7006"));
  CHECK(has(codes(file("hvc1", hvcc({32, 33, 34}, false))), "CNT-7006"));
  CHECK_FALSE(has(codes(file("hev1", hvcc({32, 33, 34}, false))), "CNT-7006"));
}

TEST_CASE("av1C.configOBUs holds exactly one sequence header", "[container][isobmff][brands][fast]") {
  const auto file = [](unsigned headers) {
    TrackSpec t;
    t.entry = visual_entry("av01", av1c(headers));
    return cat({types("isom", {"iso2"}), box("moov", cat({mvhd(), trak(t)}))});
  };
  CHECK(has(codes(file(0)), "CNT-7007"));
  CHECK(has(codes(file(2)), "CNT-7007"));
  CHECK_FALSE(has(codes(file(1)), "CNT-7007"));
}

TEST_CASE("mp42 files need esds in mp4a and mp4v entries", "[container][isobmff][brands][fast]") {
  const Bytes esds = box("esds", full(0, 0, Bytes{3, 3, 0, 0, 0}));
  const auto file = [&](std::string_view major, const Bytes& kids) {
    TrackSpec t;
    t.handler = "soun";
    t.entry = audio_entry("mp4a", kids);
    return cat({types(major, {"isom"}), box("moov", cat({mvhd(), trak(t)}))});
  };
  CHECK(has(codes(file("mp42", {})), "CNT-7002"));
  CHECK_FALSE(has(codes(file("mp42", esds)), "CNT-7002"));
  CHECK_FALSE(has(codes(file("isom", {})), "CNT-7002"));
}

TEST_CASE("tfdt and default-base-is-moof need an iso5+ style brand", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(1));
  const Bytes moov = box("moov", cat({mvhd(), trak(t), mvex()}));
  CHECK(has(codes(cat({types("isom", {"iso2", "mp41"}), moov, moof(), mdat()})), "CNT-7001"));
  CHECK(has(codes(cat({types("isom", {"iso4"}), moov, moof(false, 0x020000), mdat()})), "CNT-7001"));
  CHECK_FALSE(has(codes(cat({types("isom", {"iso6"}), moov, moof(), mdat()})), "CNT-7001"));
  CHECK_FALSE(has(codes(cat({types("isom", {"iso2", "dash"}), moov, moof(), mdat()})), "CNT-7001"));
  CHECK_FALSE(has(codes(cat({types("isom", {"iso2"}), moov, moof(false, 0), mdat()})), "CNT-7001"));
  // No ftyp (QuickTime): there are no brands to evaluate.
  CHECK_FALSE(has(codes(cat({moov, moof(), mdat()})), "CNT-7001"));
}

TEST_CASE("iTunes brands expect an ilst", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.handler = "soun";
  t.entry = audio_entry("mp4a", box("esds", full(0, 0, Bytes{3, 3, 0, 0, 0})));
  const Bytes ilst_meta =
      box("udta", box("meta", cat({full(0, 0), box("hdlr", full(0, 0, Bytes(25, 0))), box("ilst")})));
  CHECK(has(codes(cat({types("M4A ", {"mp42", "isom"}), box("moov", cat({mvhd(), trak(t)}))})), "CNT-7050"));
  CHECK_FALSE(
      has(codes(cat({types("M4A ", {"mp42", "isom"}), box("moov", cat({mvhd(), trak(t), ilst_meta}))})), "CNT-7050"));
}

TEST_CASE("mif1 files need hdlr pict, pitm, iinf, iloc and iprp in meta", "[container][isobmff][brands][fast]") {
  Bytes pict = full(0, 0, u32s({0}));
  pict.insert(pict.end(), {'p', 'i', 'c', 't'});
  pict.insert(pict.end(), 13, 0);
  const Bytes hdlr = box("hdlr", pict);
  const auto meta = [&](const Bytes& kids) {
    return cat({types("mif1", {"heic"}), box("meta", cat({full(0, 0), kids}))});
  };
  const Bytes complete = cat({hdlr, box("pitm", full(0, 0, Bytes{0, 1})), box("iinf", full(0, 0, Bytes{0, 0})),
                              box("iloc", full(0, 0, Bytes{0x44, 0, 0, 0})), box("iprp")});
  CHECK(codes(meta(complete)).empty());
  CHECK(has(codes(meta(cat({hdlr, box("iinf", full(0, 0, Bytes{0, 0}))}))), "CNT-8010"));
  CHECK(has(codes(meta(box("pitm", full(0, 0, Bytes{0, 1})))), "CNT-8010"));
  CHECK(has(codes(types("mif1", {"heic"})), "CNT-8010"));
  const auto found = run(meta(cat({hdlr, box("pitm", full(0, 0, Bytes{0, 1}))})));
  CHECK(std::ranges::count_if(found, [](const auto& f) { return f.first == "CNT-8010"; }) == 3);  // iinf, iloc, iprp
}

TEST_CASE("HLS context adds the Apple rows", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("hev1", hvcc({}));
  const Bytes file = cat(
      {types("iso6", {"iso6", "cmfc"}), box("moov", cat({mvhd(), trak(t), mvex()})), moof(false, 0x020000), mdat()});
  CHECK_FALSE(has(codes(file), "CNT-7040"));
  CHECK_FALSE(has(codes(file), "CNT-7041"));
  iso::BrandOptions hls;
  hls.hls_context = true;
  const auto found = codes(file, iso::ValidationLevel::Standard, hls);
  CHECK(has(found, "CNT-7040"));
  CHECK(has(found, "CNT-7041"));
}

TEST_CASE("the Quick level runs no brand check, and a second run adds nothing", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(0));
  const Bytes file = cat({types("cmfc", {"iso6"}), box("moov", cat({mvhd(), trak(t), trak(t), mvex()}))});
  CHECK(run(file, iso::ValidationLevel::Quick).empty());
  const ovt::io::MemoryFile f(file);
  auto r = iso::walk(f);
  const size_t first = iso::check_brands(f, r.top_level, iso::ValidationLevel::Standard);
  CHECK(first > 0);
  CHECK(iso::check_brands(f, r.top_level, iso::ValidationLevel::Standard) == 0);
  CHECK(iso::check_brands(f, r.top_level, iso::ValidationLevel::Deep) == 0);
}

TEST_CASE("IsobmffReader::validate runs the brand checks", "[container][isobmff][brands][fast]") {
  TrackSpec t;
  t.entry = visual_entry("avc1", avcc(0));
  const Bytes file = cat({types("isom", {"iso2"}), box("moov", cat({mvhd(), trak(t)}))});
  const ovt::io::MemoryFile f(file);
  ovt_status why = OVT_OK;
  auto reader = iso::make_reader(f, &why);
  REQUIRE(reader != nullptr);
  auto& iso_reader = dynamic_cast<iso::IsobmffReader&>(*reader);
  CHECK(iso_reader.validate(iso::ValidationLevel::Standard) >= 1);  // CNT-7003 at least
}

TEST_CASE("every CTA WAVE cmfc file is clean at the Standard level",
          "[container][isobmff][brands][needs-corpus][fast]") {
  const std::filesystem::path root(OVT_TEST_SOURCE_DIR);
  std::ifstream in(root / "tests/corpora/manifest.json");
  REQUIRE(in.good());
  const auto manifest = nlohmann::json::parse(in);
  size_t checked = 0;
  for (const auto& entry : manifest.at("entries")) {
    const auto& features = entry.value("features", nlohmann::json::array());
    if (entry.value("source", "") != "cta" || std::ranges::find(features, "cmaf") == features.end()) {
      continue;
    }
    const auto id = entry.at("id").get<std::string>();
    const auto path = ovt::test::corpus_path(id);
    if (!path) {
      INFO("corpus file missing: " << id);
      CHECK_FALSE(ovt::test::require_corpora());
      continue;
    }
    std::ifstream f(*path, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CAPTURE(id);
    for (const auto& [code, message] : run(bytes)) {
      CAPTURE(code, message);
      const auto* info = ovt::container::diag(code);
      REQUIRE(info != nullptr);
      CHECK(info->severity != ovt::container::Severity::Error);
      CHECK(info->severity != ovt::container::Severity::Warning);
    }
    ++checked;
  }
  if (checked == 0) {
    SKIP("no CTA WAVE cmfc file fetched: the real-asset run of WI-1.222 AT1 is handed to WI-1.237 (#802)");
  }
}

// ---- WI-1.222b: the Deep rows, on the files of tools/make_broken.py ------------------------------------------------

namespace {

Bytes broken_file(std::string_view name) {
  const std::filesystem::path path =
      std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/synthetic/isobmff-broken" / name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("Deep: parameter sets inside avc1 samples raise CNT-7004", "[container][isobmff][brands][deep][fast]") {
  const Bytes file = broken_file("syn_mp4_broken_avc1_inband_ps.mp4");
  const auto deep = run(file, iso::ValidationLevel::Deep);
  CHECK(has(codes(file, iso::ValidationLevel::Deep), "CNT-7004"));
  // The first 8 sync samples are probed, an SPS and a PPS in each: 16 distinct findings at most, 16 here.
  CHECK(std::ranges::count_if(deep, [](const auto& f) { return f.first == "CNT-7004"; }) == 16);
  CHECK_FALSE(has(codes(file, iso::ValidationLevel::Standard), "CNT-7004"));
  CHECK_FALSE(has(codes(file, iso::ValidationLevel::Quick), "CNT-7004"));
}

TEST_CASE("Deep: an OBU_TEMPORAL_DELIMITER inside an av01 sample raises CNT-7008",
          "[container][isobmff][brands][deep][fast]") {
  const Bytes file = broken_file("syn_mp4_broken_av01_td_in_sample.mp4");
  const auto found = codes(file, iso::ValidationLevel::Deep);
  CHECK(has(found, "CNT-7008"));
  CHECK_FALSE(has(found, "CNT-7009"));
  CHECK_FALSE(has(codes(file, iso::ValidationLevel::Standard), "CNT-7008"));
}

TEST_CASE("Deep: an unsized OBU that cannot end a sample raises CNT-7009", "[container][isobmff][brands][deep][fast]") {
  const Bytes file = broken_file("syn_mp4_broken_av01_unsized_obu.mp4");
  const auto found = codes(file, iso::ValidationLevel::Deep);
  CHECK(has(found, "CNT-7009"));
  CHECK_FALSE(has(found, "CNT-7008"));
  CHECK_FALSE(has(codes(file, iso::ValidationLevel::Standard), "CNT-7009"));
}

TEST_CASE("Deep: files whose samples are clean raise none of CNT-7004, 7008, 7009",
          "[container][isobmff][brands][deep][fast]") {
  for (const char* name : {"moov_after_mdat.mp4", "syn_mp4_broken_hvc1_no_ps.mp4", "dup_stts.mp4"}) {
    CAPTURE(name);
    const auto found = codes(broken_file(name), iso::ValidationLevel::Deep);
    CHECK_FALSE(has(found, "CNT-7004"));
    CHECK_FALSE(has(found, "CNT-7008"));
    CHECK_FALSE(has(found, "CNT-7009"));
  }
}

TEST_CASE("Deep: a second run adds nothing", "[container][isobmff][brands][deep][fast]") {
  const Bytes file = broken_file("syn_mp4_broken_avc1_inband_ps.mp4");
  const ovt::io::MemoryFile f(file);
  auto r = iso::walk(f);
  CHECK(iso::check_brands(f, r.top_level, iso::ValidationLevel::Deep) > 0);
  CHECK(iso::check_brands(f, r.top_level, iso::ValidationLevel::Deep) == 0);
}
