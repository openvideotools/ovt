// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group A hand decoders (chapter 08 §8.3.1, WI-1.205): field values, spans, derived text, context
// side effects, diagnostics, and the field comparison against the MP4Box goldens of the container-smoke corpus.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"
#include "test_support/golden_compare.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;
using ovt::fb::FieldT;
using ovt::fb::SyntaxValueKind;

// ---- byte builders ----

void put(Bytes& b, uint64_t v, unsigned nbytes) {
  for (unsigned i = nbytes; i > 0; --i) {
    b.push_back(static_cast<uint8_t>(v >> (8U * (i - 1U))));
  }
}
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes be(uint64_t v, unsigned nbytes) {
  Bytes b;
  put(b, v, nbytes);
  return b;
}
Bytes str(std::string_view s) {
  Bytes b;
  put_str(b, s);
  return b;
}
Bytes matrix_bytes(int32_t a, int32_t b, int32_t c, int32_t d) {
  Bytes m;
  for (const int32_t v : {a, b, 0, c, d, 0, 0, 0, 0x40000000}) {
    put(m, static_cast<uint32_t>(v), 4);
  }
  return m;
}
constexpr int32_t kOne = 0x10000;

// ---- running one decoder the way the walker does (§8.2.3 steps 2.11-2.12) ----

constexpr auto kFullBoxes =
    std::to_array<std::string_view>({"mvhd", "tkhd", "elst", "mdhd", "hdlr", "elng", "vmhd", "smhd", "hmhd", "nmhd",
                                     "sthd", "dref", "url ", "urn ", "pdin", "msrc", "ster"});
bool is_fullbox(std::string_view cc) {
  return std::ranges::find(kFullBoxes, cc) != kFullBoxes.end();
}

iso::FourCC cc_of(std::string_view s) {
  iso::FourCC cc{};
  std::copy_n(s.begin(), 4, cc.begin());
  return cc;
}

iso::BoxContext ctx_of(std::initializer_list<std::string_view> chain) {
  iso::BoxContext c;
  for (const std::string_view s : chain) {
    c.ancestors.push_back(cc_of(s));
  }
  return c;
}

void add_uint(BoxNodeT& n, std::string_view name, uint64_t v, uint64_t bit, uint32_t len) {
  auto f = std::make_unique<FieldT>();
  f->name = std::string(name);
  f->kind = SyntaxValueKind::UInt;
  f->uvalue = v;
  f->span = std::make_unique<ovt::fb::BitSpan>(bit, len);
  n.fields.push_back(std::move(f));
}

struct Decoded {
  BoxNodeT node;
  iso::DecodeOutcome outcome;
  bool found = false;
};

// Decodes the payload of a box of type `type` in `ctx`. A FullBox's version/flags are read first, as the walker
// does, into node.version/flags and two fields.
Decoded run(std::string_view type, const Bytes& payload, iso::BoxContext& ctx) {
  Decoded d;
  d.node.fourcc = std::string(type);
  d.node.header_length = 8;
  d.node.file_span = std::make_unique<ovt::fb::ByteSpan>(0, 8 + payload.size());
  const iso::RegistryEntry* e = iso::lookup(type, ctx);
  if (e == nullptr || e->decoder == nullptr) {
    return d;
  }
  d.found = true;
  size_t skip = 0;
  if (is_fullbox(type) && payload.size() >= 4) {
    d.node.version = payload[0];
    d.node.flags = static_cast<int32_t>((uint32_t{payload[1]} << 16U) | (uint32_t{payload[2]} << 8U) | payload[3]);
    add_uint(d.node, "version", payload[0], 64, 8);
    add_uint(d.node, "flags", static_cast<uint32_t>(d.node.flags), 72, 24);
    skip = 4;
  }
  ovt::BitReader br(std::span<const uint8_t>(payload).subspan(skip));
  iso::FieldBuilder fb(d.node, 0, br, 64U + 8U * skip);
  d.outcome = iso::run_decoder(*e->decoder, ctx, br, fb);
  return d;
}

Decoded run(std::string_view type, const Bytes& payload) {
  iso::BoxContext ctx;
  return run(type, payload, ctx);
}

Bytes fullbox_payload(uint8_t version, uint32_t flags, const Bytes& body) {
  return cat({be(version, 1), be(flags, 3), body});
}

// ---- field access ----

const FieldT* child(const std::vector<std::unique_ptr<FieldT>>& fields, std::string_view name) {
  for (const auto& f : fields) {
    if (f->name == name) {
      return f.get();
    }
  }
  return nullptr;
}

// "entries/entry[0]/media_time"
const FieldT& field(const BoxNodeT& n, std::string_view path) {
  const std::vector<std::unique_ptr<FieldT>>* level = &n.fields;
  const FieldT* f = nullptr;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    f = child(*level, path.substr(0, slash));
    REQUIRE(f != nullptr);
    level = &f->children;
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
  }
  REQUIRE(f != nullptr);
  return *f;
}

bool has_field(const BoxNodeT& n, std::string_view name) {
  return child(n.fields, name) != nullptr;
}

bool has_code(const BoxNodeT& n, std::string_view code) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code; });
}

// Every top-level field starts where the previous one ended (zero-length derived fields excepted), and the last
// one ends at the payload end.
void check_contiguous(const BoxNodeT& n, uint64_t payload_bytes) {
  uint64_t at = 64;
  for (const auto& f : n.fields) {
    INFO(f->name);
    REQUIRE(f->span != nullptr);
    CHECK(f->span->bit_offset() == at);
    at = f->span->bit_offset() + f->span->bit_length();
  }
  CHECK(at == 64 + 8 * payload_bytes);
}

Bytes mvhd_v0_body(uint32_t timescale, uint32_t duration) {
  return cat({be(3231959510U, 4), be(3231959511U, 4), be(timescale, 4), be(duration, 4), be(0x00010000, 4),
              be(0x0100, 2), be(0, 2), be(0, 8), matrix_bytes(kOne, 0, 0, kOne), Bytes(24, 0), be(5, 4)});
}

}  // namespace

// ---- registration ----

TEST_CASE("every group A row with fields has a registered decoder", "[container][decoders][movie]") {
  for (const std::string_view cc : {"ftyp", "styp", "mvhd", "tkhd", "elst", "mdhd", "hdlr", "elng", "vmhd", "smhd",
                                    "hmhd", "nmhd", "sthd", "dref", "url ", "urn ", "pdin"}) {
    INFO(cc);
    const iso::RegistryEntry* e = iso::lookup(cc, ctx_of({"moov", "trak"}));
    REQUIRE(e != nullptr);
    CHECK(e->decoder != nullptr);
  }
  // Every child of tref / trgr, whatever its type.
  for (const std::string_view cc : {"hint", "cdsc", "chap", "zzzz"}) {
    const iso::RegistryEntry* e = iso::lookup(cc, ctx_of({"moov", "trak", "tref"}));
    REQUIRE(e != nullptr);
    CHECK(e->decoder != nullptr);
  }
  // hdlr: the one "*" registration reaches every parent the registry lists (mdia, meta, minf).
  for (const auto& chain : {ctx_of({"moov", "trak", "mdia"}), ctx_of({"moov", "udta", "meta"}), ctx_of({"meta"}),
                            ctx_of({"moov", "trak", "mdia", "minf"})}) {
    const iso::RegistryEntry* h = iso::lookup("hdlr", chain);
    REQUIRE(h != nullptr);
    CHECK(h->decoder != nullptr);
    CHECK(h->is_fullbox);
  }
  const iso::RegistryEntry* g = iso::lookup("msrc", ctx_of({"moov", "trak", "trgr"}));
  REQUIRE(g != nullptr);
  CHECK(g->decoder != nullptr);
  // Containers have no decoder.
  const iso::RegistryEntry* moov = iso::lookup("moov", ctx_of({}));
  CHECK((moov == nullptr || moov->decoder == nullptr));
}

// ---- ftyp / styp ----

TEST_CASE("ftyp: brands, descriptions and spans", "[container][decoders][movie]") {
  const Bytes p = cat({str("mp42"), be(1, 4), str("mp42"), str("mp41"), str("zz9z")});
  iso::BoxContext ctx;
  const Decoded d = run("ftyp", p, ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "major_brand").svalue == "mp42");
  CHECK(field(d.node, "major_brand.derived").svalue == "MP4 version 2");
  CHECK(field(d.node, "minor_version").uvalue == 1);
  CHECK(field(d.node, "compatible_brands").uvalue == 3);
  CHECK(field(d.node, "compatible_brands/compatible_brands[1]").svalue == "mp41");
  CHECK(field(d.node, "compatible_brands/compatible_brands[2]").svalue == "zz9z");
  CHECK(field(d.node, "compatible_brands.derived").svalue == "mp42: MP4 version 2; mp41: MP4 version 1");
  CHECK(field(d.node, "minor_version").spec_ref == "ISO/IEC 14496-12:2022 §4.3");
  CHECK_FALSE(ctx.quicktime);
  check_contiguous(d.node, p.size());
  CHECK(d.node.diagnostics.empty());

  SECTION("a QuickTime major brand marks the context") {
    iso::BoxContext qt;
    run("ftyp", cat({str("qt  "), be(0x20050300, 4), str("qt  ")}), qt);
    CHECK(qt.quicktime);
  }
  SECTION("a partial brand at the end is CNT-1005") {
    CHECK(has_code(run("ftyp", cat({str("isom"), be(0, 4), str("is")})).node, "CNT-1005"));
  }
  SECTION("a payload shorter than major_brand + minor_version is CNT-1002") {
    const Decoded t = run("ftyp", str("isom"));
    CHECK(t.outcome.truncated);
    CHECK(has_code(t.node, "CNT-1002"));
    CHECK(field(t.node, "major_brand").svalue == "isom");
  }
}

TEST_CASE("styp decodes like ftyp with its own spec reference", "[container][decoders][movie]") {
  const Decoded d = run("styp", cat({str("msdh"), be(0, 4), str("msdh"), str("msix")}));
  CHECK(field(d.node, "major_brand").svalue == "msdh");
  CHECK(field(d.node, "compatible_brands").uvalue == 2);
  CHECK(field(d.node, "major_brand").spec_ref == "ISO/IEC 14496-12:2022 §8.16.2");
}

// ---- mvhd ----

TEST_CASE("mvhd v0 and v1: values, dates, duration and the movie timescale", "[container][decoders][movie]") {
  SECTION("version 0") {
    const Bytes p = fullbox_payload(0, 0, mvhd_v0_body(600, 6003));
    iso::BoxContext ctx;
    const Decoded d = run("mvhd", p, ctx);
    CHECK(field(d.node, "creation_time").uvalue == 3231959510U);
    CHECK(field(d.node, "creation_time").svalue == "2006-05-31T22:31:50Z");
    CHECK(field(d.node, "timescale").uvalue == 600);
    CHECK(field(d.node, "duration").uvalue == 6003);
    CHECK(field(d.node, "duration").svalue == "10.005 s");
    CHECK(field(d.node, "rate").svalue == "1.0");
    CHECK(field(d.node, "volume").svalue == "1.0");
    CHECK(field(d.node, "matrix").svalue == "identity");
    CHECK(field(d.node, "pre_defined").uvalue == 6);
    CHECK(field(d.node, "next_track_ID").uvalue == 5);
    CHECK(ctx.movie_timescale == 600);
    check_contiguous(d.node, p.size());
    CHECK(d.node.diagnostics.empty());
  }
  SECTION("version 1 has 64-bit times") {
    const Bytes body = cat({be(0x100000000ULL, 8), be(1, 8), be(1000, 4), be(UINT64_MAX, 8), be(0x00020000, 4),
                            be(0x0080, 2), be(0, 2), be(0, 8), matrix_bytes(kOne, 0, 0, kOne), Bytes(24, 0), be(2, 4)});
    const Bytes p = fullbox_payload(1, 0, body);
    const Decoded d = run("mvhd", p);
    CHECK(field(d.node, "creation_time").uvalue == 0x100000000ULL);
    CHECK(field(d.node, "creation_time").span->bit_length() == 64);
    CHECK(field(d.node, "duration").svalue == "unknown");
    CHECK(field(d.node, "rate").svalue == "2.0");
    CHECK(field(d.node, "volume").svalue == "0.5");
    check_contiguous(d.node, p.size());
  }
  SECTION("timescale 0 is CNT-1032") {
    CHECK(has_code(run("mvhd", fullbox_payload(0, 0, mvhd_v0_body(0, 10))).node, "CNT-1032"));
  }
  SECTION("an unknown version is CNT-1003 and decodes nothing") {
    const Decoded d = run("mvhd", fullbox_payload(2, 0, mvhd_v0_body(600, 1)));
    CHECK(has_code(d.node, "CNT-1003"));
    CHECK_FALSE(has_field(d.node, "timescale"));
  }
  SECTION("extra bytes are CNT-1015") {
    CHECK(has_code(run("mvhd", fullbox_payload(0, 0, cat({mvhd_v0_body(600, 1), Bytes(4, 0)}))).node, "CNT-1015"));
  }
  SECTION("a truncated mvhd keeps the fields read so far") {
    Bytes body = mvhd_v0_body(600, 1);
    body.resize(14);
    const Decoded d = run("mvhd", fullbox_payload(0, 0, body));
    CHECK(d.outcome.truncated);
    CHECK(has_code(d.node, "CNT-1002"));
    CHECK(field(d.node, "timescale").uvalue == 600);
  }
}

// ---- tkhd ----

namespace {
Bytes tkhd_v0_body(uint32_t track_id, uint32_t duration, const Bytes& matrix) {
  return cat({be(1, 4), be(2, 4), be(track_id, 4), be(0, 4), be(duration, 4), be(0, 8), be(0, 2), be(0, 2), be(0, 2),
              be(0, 2), matrix, be(1920U << 16U, 4), be(1080U << 16U, 4)});
}
}  // namespace

TEST_CASE("tkhd: flags, track_ID, duration in the movie timescale, size", "[container][decoders][movie]") {
  iso::BoxContext ctx;
  ctx.movie_timescale = 1000;
  const Bytes p = fullbox_payload(0, 0x3, tkhd_v0_body(7, 2500, matrix_bytes(kOne, 0, 0, kOne)));
  const Decoded d = run("tkhd", p, ctx);
  CHECK(field(d.node, "flags").svalue == "track_enabled|track_in_movie");
  CHECK(field(d.node, "track_ID").uvalue == 7);
  CHECK(field(d.node, "duration").svalue == "2.500 s");
  CHECK(field(d.node, "matrix").svalue == "identity");
  CHECK(field(d.node, "width").ivalue == (1920 << 16));
  CHECK(field(d.node, "width").svalue == "1920.0");
  CHECK(field(d.node, "height").svalue == "1080.0");
  CHECK(ctx.track_id == 7);
  check_contiguous(d.node, p.size());
  CHECK(d.node.diagnostics.empty());

  SECTION("all-ones duration is unknown in both versions") {
    CHECK(
        field(
            run("tkhd", fullbox_payload(0, 1, tkhd_v0_body(1, 0xFFFFFFFFU, matrix_bytes(kOne, 0, 0, kOne))), ctx).node,
            "duration")
            .svalue == "unknown");
    const Bytes v1 = cat({be(1, 8), be(2, 8), be(1, 4), be(0, 4), be(UINT64_MAX, 8), be(0, 8), be(0, 8),
                          matrix_bytes(kOne, 0, 0, kOne), be(0, 8)});
    const Decoded d1 = run("tkhd", fullbox_payload(1, 1, v1), ctx);
    CHECK(field(d1.node, "duration").svalue == "unknown");
    check_contiguous(d1.node, 4 + v1.size());
  }
}

TEST_CASE("tkhd.matrix of a 90°-rotated phone video derives the rotation", "[container][decoders][movie]") {
  // Acceptance test 3: the matrix a phone writes for portrait recording, {0, 1, 0, -1, 0, 0, tx, 0, 1}.
  Bytes m = matrix_bytes(0, kOne, -kOne, 0);
  const Bytes tx = be(1080U << 16U, 4);
  std::copy(tx.begin(), tx.end(), m.begin() + 24);  // x translation: the rotation keeps the picture on screen
  const Decoded d = run("tkhd", fullbox_payload(0, 3, tkhd_v0_body(1, 10, m)));
  CHECK(field(d.node, "matrix").svalue == "rotation 90°");
  CHECK_FALSE(has_code(d.node, "CNT-1024"));
  CHECK(field(run("tkhd", fullbox_payload(0, 3, tkhd_v0_body(1, 10, matrix_bytes(-kOne, 0, 0, -kOne)))).node, "matrix")
            .svalue == "rotation 180°");
  CHECK(field(run("tkhd", fullbox_payload(0, 3, tkhd_v0_body(1, 10, matrix_bytes(0, -kOne, kOne, 0)))).node, "matrix")
            .svalue == "rotation 270°");
  const Decoded scaled = run("tkhd", fullbox_payload(0, 3, tkhd_v0_body(1, 10, matrix_bytes(2 * kOne, 0, 0, kOne))));
  CHECK(field(scaled.node, "matrix").svalue == "custom");
  CHECK(has_code(scaled.node, "CNT-1024"));
}

// ---- tref / trgr ----

TEST_CASE("tref children list track_IDs with the reference type described", "[container][decoders][movie]") {
  iso::BoxContext ctx = ctx_of({"moov", "trak", "tref"});
  const Bytes p = cat({be(1, 4), be(3, 4)});
  const Decoded d = run("hint", p, ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "track_IDs").uvalue == 2);
  CHECK(field(d.node, "track_IDs").svalue == "hint track for the referenced media");
  CHECK(field(d.node, "track_IDs/track_IDs[1]").uvalue == 3);
  check_contiguous(d.node, p.size());
  CHECK(field(run("chap", be(2, 4), ctx).node, "track_IDs").svalue == "chapter list (QuickTime)");
}

TEST_CASE("trgr children carry a track_group_id", "[container][decoders][movie]") {
  iso::BoxContext ctx = ctx_of({"moov", "trak", "trgr"});
  const Decoded d = run("msrc", fullbox_payload(0, 0, be(42, 4)), ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "track_group_id").uvalue == 42);
}

// ---- elst ----

TEST_CASE("elst: segment durations in the movie timescale, media_time in the track timescale",
          "[container][decoders][movie]") {
  iso::BoxContext ctx;
  ctx.movie_timescale = 1000;
  ctx.track_timescale = 90000;
  SECTION("version 0 with an empty edit, a normal edit and a dwell") {
    const Bytes body = cat({be(3, 4),                                            // entry_count
                            be(500, 4), be(0xFFFFFFFFU, 4), be(1, 2), be(0, 2),  // empty edit
                            be(2000, 4), be(9000, 4), be(1, 2), be(0, 2),        // normal
                            be(100, 4), be(180000, 4), be(0, 2), be(0, 2)});     // dwell
    const Bytes p = fullbox_payload(0, 0, body);
    const Decoded d = run("elst", p, ctx);
    CHECK(field(d.node, "entry_count").uvalue == 3);
    CHECK(field(d.node, "entries").uvalue == 3);
    CHECK(field(d.node, "entries/entry[0]/segment_duration").svalue == "0.500 s");
    CHECK(field(d.node, "entries/entry[0]/media_time").ivalue == -1);
    CHECK(field(d.node, "entries/entry[0]/media_time").svalue == "empty edit");
    CHECK(field(d.node, "entries/entry[1]/media_time").ivalue == 9000);
    CHECK(field(d.node, "entries/entry[1]/media_time").svalue == "0.100 s");
    CHECK(field(d.node, "entries/entry[1]/media_rate_integer").svalue.empty());
    CHECK(field(d.node, "entries/entry[2]/media_rate_integer").svalue == "dwell");
    CHECK(field(d.node, "entries/entry[2]/media_time").svalue == "2.000 s");
    check_contiguous(d.node, p.size());
    CHECK(d.node.diagnostics.empty());
  }
  SECTION("version 1 has 64-bit fields") {
    const Bytes body = cat({be(1, 4), be(3000, 8), be(UINT64_MAX, 8), be(1, 2), be(0, 2)});
    const Decoded d = run("elst", fullbox_payload(1, 0, body), ctx);
    CHECK(field(d.node, "entries/entry[0]/segment_duration").svalue == "3.000 s");
    CHECK(field(d.node, "entries/entry[0]/media_time").ivalue == -1);
    CHECK(field(d.node, "entries/entry[0]/media_time").span->bit_length() == 64);
  }
  SECTION("an unknown track timescale is said so") {
    iso::BoxContext early;  // elst usually precedes mdhd
    early.movie_timescale = 1000;
    const Decoded d =
        run("elst", fullbox_payload(0, 0, cat({be(1, 4), be(1, 4), be(10, 4), be(1, 2), be(0, 2)})), early);
    CHECK(field(d.node, "entries/entry[0]/media_time").svalue == "track timescale unknown");
  }
  SECTION("entry_count beyond the payload is CNT-1014; the entries that fit are decoded") {
    const Decoded d = run("elst", fullbox_payload(0, 0, cat({be(5, 4), be(1, 4), be(0, 4), be(1, 2), be(0, 2)})), ctx);
    CHECK(has_code(d.node, "CNT-1014"));
    CHECK_FALSE(d.outcome.truncated);
    CHECK(field(d.node, "entries").children.size() == 1);
    CHECK(field(d.node, "entries").uvalue == 1);  // the readable count, not the declared one
    CHECK(field(d.node, "entry_count").uvalue == 5);
  }
}

// ---- mdhd ----

TEST_CASE("mdhd: language, duration and the track timescale", "[container][decoders][movie]") {
  const auto body = [](uint16_t lang) {
    return cat({be(1, 4), be(2, 4), be(48000, 4), be(480256, 4), be(lang, 2), be(0, 2)});
  };
  iso::BoxContext ctx;
  const Bytes p = fullbox_payload(0, 0, body(0x15C7));  // "eng"
  const Decoded d = run("mdhd", p, ctx);
  CHECK(field(d.node, "language").svalue == "eng");
  CHECK(field(d.node, "language").uvalue == 0x15C7);
  CHECK(field(d.node, "timescale").uvalue == 48000);
  CHECK(field(d.node, "duration").svalue == "10.005 s");
  CHECK(ctx.track_timescale == 48000);
  check_contiguous(d.node, p.size());
  CHECK(field(run("mdhd", fullbox_payload(0, 0, body(0x7FFF))).node, "language").svalue == "und");
  CHECK(has_code(
      run("mdhd", fullbox_payload(0, 0, cat({be(1, 4), be(2, 4), be(0, 4), be(0, 4), be(0x55C4, 2), be(0, 2)}))).node,
      "CNT-1032"));
}

// ---- hdlr ----

TEST_CASE("hdlr: handler type, description, name and the track context", "[container][decoders][movie]") {
  iso::BoxContext ctx = ctx_of({"moov", "trak", "mdia"});
  const Bytes p = fullbox_payload(0, 0, cat({be(0, 4), str("vide"), Bytes(12, 0), str("VideoHandler"), be(0, 1)}));
  const Decoded d = run("hdlr", p, ctx);
  CHECK(field(d.node, "handler_type").svalue == "vide");
  CHECK(field(d.node, "handler_type.derived").svalue == "Video track");
  CHECK(field(d.node, "name").svalue == "VideoHandler");
  CHECK(std::string(ctx.handler_type.data(), 4) == "vide");
  check_contiguous(d.node, p.size());
  CHECK(d.node.diagnostics.empty());

  SECTION("a meta handler does not change the track's handler") {
    iso::BoxContext meta = ctx_of({"moov", "udta", "meta"});
    run("hdlr", fullbox_payload(0, 0, cat({be(0, 4), str("mdir"), Bytes(12, 0), be(0, 1)})), meta);
    CHECK(meta.handler_type == iso::FourCC{});
  }
}

TEST_CASE("hdlr.name of a QuickTime file is a counted string without its length byte", "[container][decoders][movie]") {
  // Acceptance test 2.
  iso::BoxContext ctx = ctx_of({"moov", "trak", "mdia"});
  ctx.quicktime = true;
  const std::string_view name = "Apple Video Media Handler";
  const Bytes p =
      fullbox_payload(0, 0, cat({str("mhlr"), str("vide"), str("appl"), be(0, 8), be(name.size(), 1), str(name)}));
  const Decoded d = run("hdlr", p, ctx);
  CHECK(field(d.node, "name_length").uvalue == name.size());
  CHECK(field(d.node, "name").svalue == name);
  check_contiguous(d.node, p.size());

  SECTION("the same bytes in an ISO file are read as a C string") {
    iso::BoxContext iso_ctx = ctx_of({"moov", "trak", "mdia"});
    const Decoded i = run("hdlr", p, iso_ctx);
    CHECK_FALSE(has_field(i.node, "name_length"));
    CHECK(field(i.node, "name").svalue.size() == name.size() + 1);
  }
  SECTION("an empty QuickTime name") {
    const Decoded e = run("hdlr", fullbox_payload(0, 0, cat({be(0, 4), str("soun"), Bytes(12, 0), be(0, 1)})), ctx);
    CHECK(field(e.node, "name_length").uvalue == 0);
    CHECK(field(e.node, "name").svalue.empty());
  }
}

// ---- small boxes ----

TEST_CASE("media headers, elng, data references and pdin", "[container][decoders][movie]") {
  const Decoded vmhd = run("vmhd", fullbox_payload(0, 1, cat({be(0x40, 2), be(1, 2), be(2, 2), be(3, 2)})));
  CHECK(field(vmhd.node, "graphicsmode").svalue == "dither copy");
  CHECK(field(vmhd.node, "opcolor/opcolor[2]").uvalue == 3);
  CHECK(vmhd.node.diagnostics.empty());

  const Decoded smhd = run("smhd", fullbox_payload(0, 0, cat({be(0xFF80, 2), be(0, 2)})));
  CHECK(field(smhd.node, "balance").svalue == "-0.5");

  const Decoded hmhd =
      run("hmhd", fullbox_payload(0, 0, cat({be(1500, 2), be(1000, 2), be(8000, 4), be(4000, 4), be(0, 4)})));
  CHECK(field(hmhd.node, "maxbitrate").uvalue == 8000);

  CHECK(run("nmhd", fullbox_payload(0, 0, {})).node.diagnostics.empty());
  CHECK(has_code(run("sthd", fullbox_payload(0, 0, Bytes(2, 0))).node, "CNT-1005"));

  CHECK(field(run("elng", fullbox_payload(0, 0, cat({str("en-US"), be(0, 1)}))).node, "extended_language").svalue ==
        "en-US");

  const Decoded dref = run("dref", fullbox_payload(0, 0, be(1, 4)));
  CHECK(field(dref.node, "entry_count").uvalue == 1);
  CHECK(dref.outcome.children_offset == 4);

  const Decoded self = run("url ", fullbox_payload(0, 1, {}));
  CHECK(field(self.node, "flags").svalue == "self-contained");
  CHECK_FALSE(has_field(self.node, "location"));
  CHECK(self.node.diagnostics.empty());
  const Decoded ext = run("url ", fullbox_payload(0, 0, cat({str("http://x/a.mp4"), be(0, 1)})));
  CHECK(field(ext.node, "location").svalue == "http://x/a.mp4");

  const Decoded urn = run("urn ", fullbox_payload(0, 0, cat({str("urn:a"), be(0, 1), str("loc"), be(0, 1)})));
  CHECK(field(urn.node, "name").svalue == "urn:a");
  CHECK(field(urn.node, "location").svalue == "loc");

  const Decoded pdin = run("pdin", fullbox_payload(0, 0, cat({be(1000, 4), be(5, 4), be(2000, 4), be(3, 4)})));
  CHECK(field(pdin.node, "entries").uvalue == 2);
  CHECK(field(pdin.node, "entries/entry[1]/initial_delay").uvalue == 3);
}

// ---- golden comparison against MP4Box on the container-smoke corpus ----

namespace {

// Field tree -> JSON the way the MP4Box goldens print it: a group whose children are "<name>[<i>]" becomes an
// array; other groups become objects; leaves give their value.
nlohmann::json json_of(const FieldT& f);

nlohmann::json json_of_children(const std::vector<std::unique_ptr<FieldT>>& children) {
  const bool indexed = !children.empty() && std::ranges::all_of(children, [](const auto& c) {
    return !c->name.empty() && c->name.back() == ']';
  });
  if (indexed) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& c : children) {
      a.push_back(json_of(*c));
    }
    return a;
  }
  nlohmann::json o = nlohmann::json::object();
  for (const auto& c : children) {
    o[c->name] = json_of(*c);
  }
  return o;
}

nlohmann::json json_of(const FieldT& f) {
  if (!f.children.empty()) {
    return json_of_children(f.children);
  }
  switch (f.kind) {
    case SyntaxValueKind::Int:
    case SyntaxValueKind::Fixed:
      return f.ivalue;
    case SyntaxValueKind::String:
      return f.svalue;
    default:
      return f.uvalue;
  }
}

// Collects the fields the walker decoded (§8.2.3 step 2.12), keyed by the MP4Box golden path ("moov/trak[2]/tkhd").
struct Harness {
  std::map<std::string, nlohmann::json> by_path;  // decoded boxes only

  void visit(const std::vector<std::unique_ptr<BoxNodeT>>& boxes, const std::string& parent) {
    std::map<std::string, int> total;
    for (const auto& b : boxes) {
      ++total[b->fourcc];
    }
    std::map<std::string, int> seen;
    for (const auto& b : boxes) {
      std::string path = parent.empty() ? b->fourcc : parent + "/" + b->fourcc;
      if (total[b->fourcc] > 1) {
        path += "[" + std::to_string(++seen[b->fourcc]) + "]";
      }
      if (!b->fields.empty()) {
        by_path[path] = json_of_children(b->fields);
      }
      visit(b->children, path);
    }
  }
};

const std::set<std::string, std::less<>> kGroupA = {"ftyp", "styp", "mvhd", "tkhd", "elst", "mdhd", "hdlr", "vmhd",
                                                    "smhd", "hmhd", "nmhd", "sthd", "dref", "url ", "urn ", "pdin"};

std::string last_type(const std::string& path) {
  std::string last = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
  return last.substr(0, last.find('['));
}

// Compares every group A box of the golden; returns the number of boxes compared.
size_t compare_with_golden(std::string_view id) {
  const auto path = ovt::test::corpus_or_skip(id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult walked = iso::walk(*file);
  Harness h;
  h.visit(walked.top_level, "");
  std::ifstream gf(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/isobmff" /
                   (std::string(id) + ".mp4box-diso.json"));
  REQUIRE(gf.good());
  const auto golden = nlohmann::json::parse(gf);
  size_t compared = 0;
  for (const auto& box : golden) {
    const std::string p = box.at("path").get<std::string>();
    if (!kGroupA.contains(last_type(p)) || !box.contains("fields")) {
      continue;
    }
    INFO(std::string(id) + " " + p);
    REQUIRE(h.by_path.contains(p));
    const auto diffs = ovt::test::compare_canonical(box.at("fields"), h.by_path.at(p));
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d.path + ": " + d.message);
    }
    CHECK(diffs.empty());
    ++compared;
  }
  return compared;
}

}  // namespace

TEST_CASE("isom_basic.mp4 matches the MP4Box golden for mvhd, tkhd, mdhd and elst",
          "[container][decoders][movie][golden][needs-corpus]") {
  // Acceptance test 1, SPEC-ASSUMPTION(#567). isom_basic.mp4 is mpegff_a1_foreman_qcif (§14.3.5); it has no edts, so
  // the elst part uses mpegff_hvc1_only, whose golden has elst.entries[0].media_time == 17000.
  const size_t foreman = compare_with_golden("mpegff_a1_foreman_qcif");
  CHECK(foreman >= 20);
  const size_t hvc1_only = compare_with_golden("mpegff_hvc1_only");
  CHECK(hvc1_only >= 9);
}

TEST_CASE("group A fields match the MP4Box goldens of the container-smoke corpus",
          "[container][decoders][movie][golden][needs-corpus]") {
  for (const std::string_view id :
       {"mpegff_10_fragments", "mpegff_a4_tone_fragmented", "mpegff_fragment_random_access_2",
        "mpegff_hev1_clg1_header", "mpegff_video_cenc_128iv", "mpegff_video_cenc_roll_frag1s", "ytugc_clip01"}) {
    // Outside CHECK: a missing corpus file SKIPs the test case, and a SKIP inside a CHECK expression is a failure.
    const size_t compared = compare_with_golden(id);
    CHECK(compared > 0);
  }
}
