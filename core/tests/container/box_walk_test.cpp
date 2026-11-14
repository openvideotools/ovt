// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the ISOBMFF box header walk (chapter 08 §8.2.3, WI-1.201).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/box_walk.h"
#include "io/memory_file.hpp"

namespace {

using Bytes = std::vector<uint8_t>;
namespace iso = ovt::container::isobmff;
using ovt::fb::BoxNodeT;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
void put64(Bytes& b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v >> 32U));
  put32(b, static_cast<uint32_t>(v));
}
void put_str(Bytes& b, std::string_view s) {
  b.insert(b.end(), s.begin(), s.end());
}
void put_all(Bytes& b, const Bytes& more) {
  b.insert(b.end(), more.begin(), more.end());
}

Bytes box(std::string_view type, const Bytes& payload = {}) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  put_str(b, type);
  put_all(b, payload);
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    put_all(b, p);
  }
  return b;
}

iso::WalkResult walk_bytes(const Bytes& data) {
  const ovt::io::MemoryFile f(data);
  return iso::walk(f);
}

bool has_code(const std::vector<std::unique_ptr<ovt::fb::DiagnosticT>>& ds, std::string_view code) {
  return std::ranges::any_of(ds, [&](const auto& d) { return d->code == code; });
}

// Every code on the node or any descendant.
bool tree_has_code(const BoxNodeT& n, std::string_view code) {
  return has_code(n.diagnostics, code) ||
         std::ranges::any_of(n.children, [&](const auto& c) { return tree_has_code(*c, code); });
}
bool any_code(const iso::WalkResult& r, std::string_view code) {
  return has_code(r.diagnostics, code) ||
         std::ranges::any_of(r.top_level, [&](const auto& n) { return tree_has_code(*n, code); });
}

const BoxNodeT* child(const BoxNodeT& n, std::string_view fourcc) {
  for (const auto& c : n.children) {
    if (c->fourcc == fourcc) {
      return c.get();
    }
  }
  return nullptr;
}

uint64_t span_sum(const iso::BoxNodes& nodes) {
  uint64_t s = 0;
  for (const auto& n : nodes) {
    s += n->file_span->length();
  }
  return s;
}

// Children of every node tile their parent's payload without gaps or overlap.
void check_tiling(const iso::BoxNodes& nodes, uint64_t begin, uint64_t end) {
  uint64_t pos = begin;
  for (const auto& n : nodes) {
    REQUIRE(n->file_span->offset() == pos);
    REQUIRE(n->file_span->length() >= 1);
    pos += n->file_span->length();
    if (!n->children.empty()) {
      check_tiling(n->children, n->file_span->offset() + n->header_length, pos);
    }
  }
  REQUIRE(pos == end);
}

Bytes base_file() {
  const Bytes mvhd = box("mvhd", Bytes(100, 0));
  const Bytes tkhd = box("tkhd", Bytes(84, 0));
  const Bytes mdhd = box("mdhd", Bytes(24, 0));
  const Bytes stbl = box("stbl", cat({box("stts", Bytes(8, 0)), box("stsz", Bytes(12, 0))}));
  const Bytes trak = box("trak", cat({tkhd, box("mdia", cat({mdhd, box("minf", stbl)}))}));
  return cat({box("ftyp", Bytes(16, 'a')), box("moov", cat({mvhd, trak})), box("mdat", Bytes(300, 7))});
}

std::vector<uint8_t> read_corpus(const std::string& name) {
  const auto path = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/corpora/synthetic/isobmff-broken" / name;
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("a well-formed file yields a tiled tree with exact spans", "[container][isobmff][walk][fast]") {
  const Bytes data = base_file();
  const auto r = walk_bytes(data);
  REQUIRE(r.top_level.size() == 3);
  CHECK(r.top_level[0]->fourcc == "ftyp");
  CHECK(r.top_level[0]->file_span->offset() == 0);
  CHECK(r.top_level[0]->header_length == 8);
  CHECK(r.top_level[0]->version == -1);
  CHECK(span_sum(r.top_level) == data.size());
  check_tiling(r.top_level, 0, data.size());
  const BoxNodeT& moov = *r.top_level[1];
  REQUIRE(child(moov, "trak") != nullptr);
  CHECK(child(*child(moov, "trak"), "mdia") != nullptr);
  // Opaque and leaf payloads are described, not read.
  const BoxNodeT& mdat = *r.top_level[2];
  REQUIRE(mdat.payload_span != nullptr);
  CHECK(mdat.payload_span->offset() == mdat.file_span->offset() + 8);
  CHECK(mdat.payload_span->length() == 300);
  CHECK(r.diagnostics.empty());
  CHECK_FALSE(r.root_ctx.quicktime);
  CHECK(r.root_ctx.saw_moov);
  CHECK_FALSE(r.root_ctx.mdat_before_moov);
}

TEST_CASE("size 1 reads largesize and size 0 extends to the end of the file", "[container][isobmff][walk][fast]") {
  Bytes big;
  put32(big, 1);
  put_str(big, "free");
  put64(big, 16 + 5);
  big.insert(big.end(), 5, 0);
  Bytes zero;
  put32(zero, 0);
  put_str(zero, "mdat");
  zero.insert(zero.end(), 40, 9);
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), big, zero});
  const auto r = walk_bytes(data);
  REQUIRE(r.top_level.size() == 3);
  CHECK(r.top_level[1]->header_length == 16);
  CHECK(r.top_level[1]->file_span->length() == 21);
  CHECK(r.top_level[2]->file_span->length() == 48);  // 8 header + 40 payload
  CHECK(r.top_level[2]->header_length == 8);
  CHECK(span_sum(r.top_level) == data.size());
  CHECK_FALSE(any_code(r, "CNT-0104"));  // allowed at the top level
}

TEST_CASE("size 0 inside a container warns and uses the parent end", "[container][isobmff][walk][fast]") {
  Bytes inner;
  put32(inner, 0);
  put_str(inner, "free");
  inner.insert(inner.end(), 12, 0);
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), box("moov", inner)});
  const auto r = walk_bytes(data);
  const BoxNodeT& moov = *r.top_level[1];
  REQUIRE(moov.children.size() == 1);
  CHECK(moov.children[0]->file_span->length() == 20);
  CHECK(has_code(moov.children[0]->diagnostics, "CNT-0104"));
}

TEST_CASE("uuid boxes carry their usertype and a header of 24 bytes", "[container][isobmff][walk][fast]") {
  Bytes payload;
  for (uint8_t i = 1; i <= 16; ++i) {
    payload.push_back(i);
  }
  payload.insert(payload.end(), 4, 0xEE);
  const auto r = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("uuid", payload)}));
  const BoxNodeT& u = *r.top_level[1];
  CHECK(u.fourcc == "uuid");
  CHECK(u.header_length == 24);
  REQUIRE(u.uuid.size() == 16);
  CHECK(u.uuid[0] == 1);
  CHECK(u.uuid[15] == 16);
  CHECK(u.payload_span->length() == 4);
}

TEST_CASE("a uuid box too short for its usertype is CNT-0107", "[container][isobmff][walk][fast]") {
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), box("uuid", Bytes(4, 0))});
  const auto r = walk_bytes(data);
  CHECK(any_code(r, "CNT-0107"));
  CHECK(span_sum(r.top_level) == data.size());
}

TEST_CASE("a uuid box with size < 8 is CNT-0106 and is not recovered to 8", "[container][isobmff][walk][fast]") {
  Bytes bad;
  put32(bad, 7);
  put_str(bad, "uuid");
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), bad, Bytes(40, 0)});
  const auto r = walk_bytes(data);
  CHECK(any_code(r, "CNT-0106"));
  CHECK_FALSE(any_code(r, "CNT-0107"));
  REQUIRE(r.top_level.size() == 2);  // ftyp, then everything from the uuid on as one trailing-bytes node
  CHECK(r.top_level[1]->description == "trailing bytes");
  CHECK(span_sum(r.top_level) == data.size());
}

TEST_CASE("a walk creates at most 1000000 boxes and then reports CNT-0111", "[container][isobmff][walk][fast]") {
  Bytes data = box("ftyp", Bytes(8, 'a'));
  for (uint64_t i = 0; i < iso::kMaxBoxNodes;
       ++i) {  // ftyp + 999999 free boxes reach the bound; the last free box is not walked
    put32(data, 8);
    put_str(data, "free");
  }
  data.insert(data.end(), 100, 0);  // not walked
  const auto r = walk_bytes(data);
  CHECK(has_code(r.diagnostics, "CNT-0111"));
  REQUIRE(r.top_level.size() == iso::kMaxBoxNodes + 1);  // the bound, plus one trailing-bytes node
  CHECK(r.top_level.back()->description == "trailing bytes");
  CHECK(span_sum(r.top_level) == data.size());
}

TEST_CASE("wide is a bare 8-byte node", "[container][isobmff][walk][fast]") {
  Bytes wide;
  put32(wide, 8);
  put_str(wide, "wide");
  const Bytes data = cat({wide, box("mdat", Bytes(10, 1)), box("moov", box("mvhd", Bytes(4, 0)))});
  const auto r = walk_bytes(data);
  REQUIRE(r.top_level.size() == 3);
  CHECK(r.top_level[0]->fourcc == "wide");
  CHECK(r.top_level[0]->file_span->length() == 8);
  CHECK(r.root_ctx.quicktime);  // no ftyp, first box is wide
  CHECK(any_code(r, "CNT-0002"));
  CHECK(any_code(r, "CNT-0003"));  // moov after mdat
  CHECK(r.root_ctx.mdat_before_moov);
}

TEST_CASE("largesize problems are CNT-0102 and CNT-0103 and keep the tail as a node",
          "[container][isobmff][walk][fast]") {
  Bytes short_large;
  put32(short_large, 1);
  put_str(short_large, "free");
  short_large.insert(short_large.end(), 4, 0);  // 12 bytes: no room for the 64-bit size
  const Bytes d1 = cat({box("ftyp", Bytes(8, 'a')), short_large});
  const auto r1 = walk_bytes(d1);
  CHECK(any_code(r1, "CNT-0102"));
  CHECK(span_sum(r1.top_level) == d1.size());

  Bytes tiny_large;
  put32(tiny_large, 1);
  put_str(tiny_large, "free");
  put64(tiny_large, 12);
  const Bytes d2 = cat({box("ftyp", Bytes(8, 'a')), tiny_large, box("free")});
  const auto r2 = walk_bytes(d2);
  CHECK(any_code(r2, "CNT-0103"));
  CHECK(span_sum(r2.top_level) == d2.size());
}

TEST_CASE("fewer than eight trailing bytes become a trailing-bytes node", "[container][isobmff][walk][fast]") {
  const Bytes data = cat({base_file(), Bytes{1, 2, 3, 4, 5}});
  const auto r = walk_bytes(data);
  REQUIRE(r.top_level.size() == 4);
  const BoxNodeT& t = *r.top_level.back();
  CHECK(t.fourcc.empty());
  CHECK(t.description == "trailing bytes");
  CHECK(t.file_span->length() == 5);
  CHECK(has_code(t.diagnostics, "CNT-0101"));
  CHECK(span_sum(r.top_level) == data.size());
}

TEST_CASE("size 7 in moov is CNT-0106 and the siblings after it are still listed", "[container][isobmff][walk][fast]") {
  Bytes bad;
  put32(bad, 7);
  put_str(bad, "free");
  const Bytes moov = box("moov", cat({box("mvhd", Bytes(8, 0)), bad, box("trak", box("tkhd", Bytes(8, 0)))}));
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), moov});
  const auto r = walk_bytes(data);
  const BoxNodeT& m = *r.top_level[1];
  REQUIRE(m.children.size() == 3);
  CHECK(m.children[1]->fourcc == "free");
  CHECK(m.children[1]->file_span->length() == 8);  // recovered as size 8
  CHECK(has_code(m.children[1]->diagnostics, "CNT-0106"));
  CHECK(m.children[2]->fourcc == "trak");
  check_tiling(r.top_level, 0, data.size());
}

TEST_CASE("an unknown type with size < 8 ends the container with a trailing node", "[container][isobmff][walk][fast]") {
  Bytes bad;
  put32(bad, 3);
  put_str(bad, "zzzz");
  bad.insert(bad.end(), 8, 0);
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), box("moov", cat({box("mvhd", Bytes(8, 0)), bad}))});
  const auto r = walk_bytes(data);
  CHECK(any_code(r, "CNT-0106"));
  CHECK(span_sum(r.top_level) == data.size());
  check_tiling(r.top_level, 0, data.size());
}

TEST_CASE("a moov that exceeds EOF by 1000 is truncated and keeps its trak", "[container][isobmff][walk][fast]") {
  Bytes file = cat({box("ftyp", Bytes(8, 'a')),
                    box("moov", cat({box("mvhd", Bytes(8, 0)), box("trak", box("tkhd", Bytes(8, 0)))}))});
  const size_t moov_at = 16;
  const auto declared = static_cast<uint32_t>(file.size() - moov_at + 1000);
  file[moov_at] = static_cast<uint8_t>(declared >> 24U);
  file[moov_at + 1] = static_cast<uint8_t>(declared >> 16U);
  file[moov_at + 2] = static_cast<uint8_t>(declared >> 8U);
  file[moov_at + 3] = static_cast<uint8_t>(declared);
  const auto r = walk_bytes(file);
  const BoxNodeT& moov = *r.top_level[1];
  CHECK(has_code(moov.diagnostics, "CNT-0108"));
  CHECK(moov.description.find("[truncated]") != std::string::npos);
  CHECK(moov.file_span->offset() + moov.file_span->length() == file.size());
  CHECK(child(moov, "trak") != nullptr);
  CHECK(span_sum(r.top_level) == file.size());
}

TEST_CASE("a size field of 0xFFFFFFFF is clamped, never overflows", "[container][isobmff][walk][fast]") {
  Bytes huge;
  put32(huge, 0xFFFFFFFFU);
  put_str(huge, "mdat");
  huge.insert(huge.end(), 20, 0);
  const Bytes d1 = cat({box("ftyp", Bytes(8, 'a')), huge});
  CHECK(any_code(walk_bytes(d1), "CNT-0108"));

  Bytes max64;
  put32(max64, 1);
  put_str(max64, "mdat");
  put64(max64, ~uint64_t{0});
  max64.insert(max64.end(), 20, 0);
  const Bytes d2 = cat({box("ftyp", Bytes(8, 'a')), max64});
  const auto r = walk_bytes(d2);
  CHECK(any_code(r, "CNT-0108"));
  CHECK(span_sum(r.top_level) == d2.size());
}

TEST_CASE("non-printable types are CNT-0109, copyright types are not", "[container][isobmff][walk][fast]") {
  Bytes bad;
  put32(bad, 12);
  bad.insert(bad.end(), {0, 0, 0, 1});
  bad.insert(bad.end(), 4, 0);
  Bytes nam;
  put32(nam, 12);
  nam.insert(nam.end(), {0xA9, 'n', 'a', 'm'});
  nam.insert(nam.end(), 4, 0);
  const auto r = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("moov", cat({bad, nam}))}));
  const BoxNodeT& moov = *r.top_level[1];
  REQUIRE(moov.children.size() == 2);
  CHECK(moov.children[0]->fourcc == "\\x00\\x00\\x00\\x01");
  CHECK(has_code(moov.children[0]->diagnostics, "CNT-0109"));
  CHECK(moov.children[1]->fourcc == "\xC2\xA9nam");  // ISO-8859-1 to UTF-8
  CHECK(moov.children[1]->diagnostics.empty());
}

TEST_CASE("nesting deeper than 32 levels stops with CNT-0105", "[container][isobmff][walk][fast]") {
  Bytes inner = box("free");
  for (int i = 0; i < 40; ++i) {
    inner = box("udta", inner);
  }
  const Bytes data = cat({box("ftyp", Bytes(8, 'a')), box("moov", inner)});
  const auto r = walk_bytes(data);
  CHECK(any_code(r, "CNT-0105"));
  // Children are walked down to depth 32: moov (depth 0) holds udta #1 at depth 1 ... udta #32; its child is refused.
  const BoxNodeT* n = r.top_level[1].get();
  int depth = 0;
  while (!n->children.empty()) {
    n = n->children[0].get();
    ++depth;
  }
  CHECK(depth == 32);
  check_tiling(r.top_level, 0, data.size());
}

TEST_CASE("meta is a FullBox in ISO files and a plain container in QuickTime files",
          "[container][isobmff][walk][fast]") {
  Bytes iso_meta;
  put32(iso_meta, 0);  // version 0, flags 0
  put_all(iso_meta, box("hdlr", Bytes(8, 0)));
  const auto r1 = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("moov", box("meta", iso_meta))}));
  const BoxNodeT& m1 = *child(*r1.top_level[1], "meta");
  CHECK(m1.version == 0);
  CHECK(m1.flags == 0);
  REQUIRE(m1.fields.size() == 2);
  CHECK(m1.fields[0]->name == "version");
  CHECK(m1.fields[0]->span->bit_offset() == 64);
  CHECK(m1.fields[1]->span->bit_length() == 24);
  REQUIRE(m1.children.size() == 1);
  CHECK(m1.children[0]->fourcc == "hdlr");
  CHECK(m1.children[0]->file_span->offset() == m1.file_span->offset() + 12);

  const auto r2 = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("moov", box("meta", box("hdlr", Bytes(8, 0))))}));
  const BoxNodeT& m2 = *child(*r2.top_level[1], "meta");
  CHECK(m2.version == -1);
  CHECK(has_code(m2.diagnostics, "CNT-1006"));
  REQUIRE(m2.children.size() == 1);
  CHECK(m2.children[0]->fourcc == "hdlr");
}

TEST_CASE("file-level findings: no ftyp, no moov, fragment-only", "[container][isobmff][walk][fast]") {
  const auto none = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("mdat", Bytes(8, 0))}));
  CHECK(any_code(none, "CNT-0004"));
  CHECK_FALSE(any_code(none, "CNT-0002"));
  const auto frag =
      walk_bytes(cat({box("styp", Bytes(8, 'a')), box("moof", box("mfhd", Bytes(8, 0))), box("mdat", Bytes(8, 0))}));
  CHECK(any_code(frag, "CNT-0005"));
  CHECK(frag.root_ctx.fragment_only);
  CHECK_FALSE(any_code(frag, "CNT-0004"));
  const auto tiny = walk_bytes(Bytes{1, 2, 3});
  CHECK(any_code(tiny, "CNT-0006"));
  CHECK(tiny.top_level.empty());
  const auto junk = walk_bytes(Bytes(32, 0));
  CHECK(any_code(junk, "CNT-0006"));
}

TEST_CASE("truncating the base file at any offset keeps the tiling invariant", "[container][isobmff][walk][fast]") {
  const Bytes data = base_file();
  for (size_t cut = 8; cut < data.size(); cut += (cut < 400 ? 1 : 37)) {
    const Bytes part(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(cut));
    const auto r = walk_bytes(part);
    REQUIRE(span_sum(r.top_level) == cut);
    check_tiling(r.top_level, 0, cut);
  }
}

TEST_CASE("mutating every header byte of the base file never breaks the tiling invariant",
          "[container][isobmff][walk][fast]") {
  const Bytes data = base_file();
  for (size_t i = 0; i < 160; ++i) {
    for (const uint8_t v : std::array<uint8_t, 5>{0x00, 0x01, 0x07, 0x7F, 0xFF}) {
      Bytes m = data;
      m[i] = v;
      const auto r = walk_bytes(m);
      if (r.top_level.empty()) {  // first header implausible: CNT-0006, no tree
        REQUIRE(has_code(r.diagnostics, "CNT-0006"));
        continue;
      }
      REQUIRE(span_sum(r.top_level) == m.size());
      check_tiling(r.top_level, 0, m.size());
    }
  }
}

struct CorpusCase {
  const char* file;
  const char* code;
};

TEST_CASE("the committed broken corpus yields its expected codes and tiles the file",
          "[container][isobmff][walk][corpus][fast]") {
  constexpr std::array<CorpusCase, 14> kCases = {{
      {"size7_in_moov.mp4", "CNT-0106"},
      {"size0_in_moov.mp4", "CNT-0104"},
      {"largesize_lt16_in_moov.mp4", "CNT-0103"},
      {"sizeffff_in_moov.mp4", "CNT-0108"},
      {"size0_top_level.mp4", nullptr},
      {"moov_exceeds_eof_by_1000.mp4", "CNT-0108"},
      {"moov_after_mdat.mp4", "CNT-0003"},
      {"trak_overlaps_end_of_moov.mp4", "CNT-0108"},
      {"nesting_33_deep.mp4", "CNT-0105"},
      {"uuid_size7.mp4", "CNT-0106"},
      {"uuid_short.mp4", "CNT-0107"},
      {"nonprintable_type.mp4", "CNT-0109"},
      {"trailing_5_bytes.mp4", "CNT-0101"},
      {"truncated_at_1000.mp4", "CNT-0108"},
  }};
  for (const CorpusCase& c : kCases) {
    DYNAMIC_SECTION(c.file) {
      const Bytes data = read_corpus(c.file);
      const auto r = walk_bytes(data);
      CHECK(span_sum(r.top_level) == data.size());
      check_tiling(r.top_level, 0, data.size());
      if (c.code != nullptr) {
        CHECK(any_code(r, c.code));
      }
    }
  }
}

TEST_CASE("siblings after a size 7 box and trak children after a truncated moov survive in the corpus",
          "[container][isobmff][walk][corpus][fast]") {
  const auto r7 = walk_bytes(read_corpus("size7_in_moov.mp4"));
  const BoxNodeT* moov = r7.top_level[1].get();
  REQUIRE(moov->fourcc == "moov");
  CHECK(child(*moov, "mvhd") != nullptr);
  CHECK(child(*moov, "trak") != nullptr);  // the sibling after the 7-byte box

  const auto r1000 = walk_bytes(read_corpus("moov_exceeds_eof_by_1000.mp4"));
  const BoxNodeT* m2 = r1000.top_level[1].get();
  CHECK(has_code(m2->diagnostics, "CNT-0108"));
  CHECK(child(*m2, "trak") != nullptr);

  const auto after = walk_bytes(read_corpus("moov_after_mdat.mp4"));
  CHECK(after.root_ctx.mdat_before_moov);
}

TEST_CASE("the walk diagnostics are all in the registry", "[container][isobmff][walk][fast]") {
  for (const char* code : {"CNT-0101", "CNT-0102", "CNT-0103", "CNT-0104", "CNT-0105", "CNT-0106", "CNT-0107",
                           "CNT-0108", "CNT-0109", "CNT-0110", "CNT-1006"}) {
    CHECK(ovt::container::diag(code) != nullptr);
  }
}
