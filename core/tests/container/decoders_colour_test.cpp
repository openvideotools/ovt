// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group C part 2 hand decoders (chapter 08 §8.3.3 rows 9-17, WI-1.208): `btrt`, `pasp`, `clap`, `colr`,
// `mdcv`, `clli`, `cclv`, `dvcC`/`dvvC`/`dvwC`, `fiel` and `srat`, run alone the way the walker runs them and inside
// walked files, plus the container-smoke corpus and its MP4Box goldens.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/field_builder.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;
using ovt::fb::FieldT;
using ovt::fb::SyntaxValueKind;

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes be(uint64_t v, unsigned nbytes) {
  Bytes b;
  for (unsigned i = nbytes; i > 0; --i) {
    b.push_back(static_cast<uint8_t>(v >> (8U * (i - 1U))));
  }
  return b;
}
Bytes str(std::string_view s) {
  return {s.begin(), s.end()};
}
Bytes zeros(size_t n) {
  return Bytes(n, 0);
}
Bytes box(std::string_view type, const Bytes& payload) {
  return cat({be(8 + payload.size(), 4), str(type), payload});
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

iso::BoxContext entry_child_ctx() {
  return ctx_of({"moov", "trak", "mdia", "minf", "stbl", "stsd", "hvc1"});
}

struct Decoded {
  BoxNodeT node;
  iso::DecodeOutcome outcome;
  bool found = false;
  size_t payload_bytes = 0;
};

// Runs the registered decoder of `type` in `ctx` the way the walker does (§8.2.3 steps 2.11-2.12).
Decoded run(std::string_view type, const Bytes& payload, iso::BoxContext ctx) {
  Decoded d;
  d.payload_bytes = payload.size();
  d.node.fourcc = std::string(type);
  d.node.header_length = 8;
  d.node.file_span = std::make_unique<ovt::fb::ByteSpan>(0, 8 + payload.size());
  const iso::RegistryEntry* e = iso::lookup(type, ctx);
  if (e == nullptr || e->decoder == nullptr) {
    return d;
  }
  d.found = true;
  size_t skip = 0;
  if (e->is_fullbox && payload.size() >= 4) {
    d.node.version = payload[0];
    skip = 4;
  }
  ovt::BitReader br(std::span<const uint8_t>(payload).subspan(skip));
  iso::FieldBuilder fb(d.node, 0, br, 64U + 8U * skip);
  d.outcome = iso::run_decoder(*e->decoder, ctx, br, fb);
  return d;
}

Decoded run(std::string_view type, const Bytes& payload) {
  return run(type, payload, entry_child_ctx());
}

const FieldT* child(const std::vector<std::unique_ptr<FieldT>>& fields, std::string_view name) {
  for (const auto& f : fields) {
    if (f->name == name) {
      return f.get();
    }
  }
  return nullptr;
}

// `path` is "a/b/c" through Field.children.
const FieldT& field(const std::vector<std::unique_ptr<FieldT>>& fields, std::string_view path) {
  const std::vector<std::unique_ptr<FieldT>>* level = &fields;
  const FieldT* f = nullptr;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    f = child(*level, path.substr(0, slash));
    INFO(path);
    REQUIRE(f != nullptr);
    level = &f->children;
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
  }
  REQUIRE(f != nullptr);
  return *f;
}
const FieldT& field(const BoxNodeT& n, std::string_view path) {
  return field(n.fields, path);
}

bool has_code(const BoxNodeT& n, std::string_view code) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code; });
}

// Every top-level field starts where the previous one ended (zero-length derived fields excepted), and the last one
// ends at the payload end.
void check_contiguous(const Decoded& d, uint64_t from_bit = 64) {
  uint64_t at = from_bit;
  for (const auto& f : d.node.fields) {
    INFO(f->name);
    REQUIRE(f->span != nullptr);
    CHECK(f->span->bit_offset() == at);
    at = f->span->bit_offset() + f->span->bit_length();
  }
  CHECK(at == 64 + 8 * d.payload_bytes);
}

const BoxNodeT* find_box(const std::vector<std::unique_ptr<BoxNodeT>>& boxes, std::string_view type) {
  for (const auto& b : boxes) {
    if (b->fourcc == type) {
      return b.get();
    }
    if (const BoxNodeT* c = find_box(b->children, type); c != nullptr) {
      return c;
    }
  }
  return nullptr;
}

// ---- payload builders ----

Bytes nclx(uint16_t cp, uint16_t tc, uint16_t mc, bool full) {
  return cat({str("nclx"), be(cp, 2), be(tc, 2), be(mc, 2), be(full ? 0x80 : 0, 1)});
}
Bytes nclc(uint16_t cp, uint16_t tc, uint16_t mc) {
  return cat({str("nclc"), be(cp, 2), be(tc, 2), be(mc, 2)});
}
// The mdcv of chapter 14 recipe 10: x265 --master-display G(13250,34500)B(7500,3000)R(34000,16000)
// WP(15635,16450)L(10000000,1), in the box order G, B, R.
Bytes mdcv_recipe10() {
  return cat({be(13250, 2), be(34500, 2), be(7500, 2), be(3000, 2), be(34000, 2), be(16000, 2), be(15635, 2),
              be(16450, 2), be(10000000, 4), be(1, 4)});
}
Bytes dovi(uint8_t profile, uint8_t level, uint8_t compat) {
  // dv_profile u(7), dv_level u(6), rpu/el/bl u(1) each, dv_bl_signal_compatibility_id u(4), reserved u(28).
  const uint64_t bits = (uint64_t{profile} << 57U) | (uint64_t{level} << 51U) | (uint64_t{1} << 50U) |
                        (uint64_t{1} << 48U) | (uint64_t{compat} << 44U);
  return cat({be(1, 1), be(0, 1), be(bits >> 16U, 6), zeros(16)});
}

Bytes visual_entry(uint16_t width, uint16_t height, const Bytes& children) {
  const Bytes compressor = zeros(32);
  return box("hvc1", cat({zeros(6), be(1, 2), zeros(16), be(width, 2), be(height, 2), be(0x00480000, 4),
                          be(0x00480000, 4), zeros(4), be(1, 2), compressor, be(24, 2), be(0xFFFF, 2), children}));
}

Bytes hdlr_box(std::string_view handler) {
  return box("hdlr", cat({zeros(4), zeros(4), str(handler), zeros(12), be(0, 1)}));
}

Bytes file_with_entry(const Bytes& entry, std::string_view brand = "isom") {
  const Bytes stsd = box("stsd", cat({be(0, 4), be(1, 4), entry}));
  const Bytes moov = box("moov", box("trak", box("mdia", cat({hdlr_box("vide"), box("minf", box("stbl", stsd))}))));
  return cat({box("ftyp", cat({str(brand), zeros(4), str(brand)})), moov});
}

iso::WalkResult walk_bytes(const Bytes& file) {
  const ovt::io::MemoryFile mem(file);
  return iso::walk(mem);
}

// ---- registration ----

TEST_CASE("every group C part 2 row has a decoder under a sample entry and as an item property",
          "[container][decoders][colour]") {
  for (const std::string_view cc :
       {"btrt", "pasp", "clap", "colr", "mdcv", "clli", "cclv", "dvcC", "dvvC", "dvwC", "fiel", "srat"}) {
    INFO(cc);
    const iso::RegistryEntry* e = iso::lookup(cc, entry_child_ctx());
    REQUIRE(e != nullptr);
    CHECK(e->decoder != nullptr);
    CHECK(e->is_fullbox == (cc == "srat"));
  }
  for (const std::string_view cc : {"pasp", "clap", "colr", "mdcv", "clli", "cclv"}) {
    INFO(cc);
    const iso::RegistryEntry* e = iso::lookup(cc, ctx_of({"meta", "iprp", "ipco"}));
    REQUIRE(e != nullptr);
    CHECK(e->decoder != nullptr);
    CHECK(std::string_view(e->spec_ref).starts_with("ISO/IEC 23008-12"));
  }
}

// ---- AT1: HDR10 colr / mdcv / clli ----

TEST_CASE("colr nclx: H.273 names, PQ transfer and the range flag", "[container][decoders][colour]") {
  const Decoded d = run("colr", nclx(9, 16, 9, false));
  REQUIRE(d.found);
  CHECK(field(d.node, "colour_type").svalue == "nclx");
  CHECK(field(d.node, "colour_primaries").uvalue == 9);
  CHECK(field(d.node, "colour_primaries").svalue == "BT.2020");
  CHECK(field(d.node, "transfer_characteristics").uvalue == 16);
  CHECK(field(d.node, "transfer_characteristics").kind == SyntaxValueKind::Enum);
  CHECK(field(d.node, "transfer_characteristics").svalue == "PQ (SMPTE ST 2084)");
  CHECK(field(d.node, "matrix_coefficients").svalue == "BT.2020 non-constant luminance");
  CHECK(field(d.node, "full_range_flag").uvalue == 0);
  CHECK(field(d.node, "full_range_flag").svalue == "limited range");
  CHECK(field(d.node, "colour_type").spec_ref == "ISO/IEC 14496-12:2022 §12.1.5");
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);

  const Decoded hlg = run("colr", nclx(9, 18, 9, true));
  CHECK(field(hlg.node, "transfer_characteristics").svalue == "HLG (ARIB STD-B67)");
  CHECK(field(hlg.node, "full_range_flag").svalue == "full range");
  CHECK(field(run("colr", nclx(1, 99, 1, false)).node, "transfer_characteristics").svalue == "reserved");
}

TEST_CASE("mdcv: chromaticities, luminance in cd/m² and the mastering display named by chromaticity",
          "[container][decoders][colour]") {
  const Decoded d = run("mdcv", mdcv_recipe10());
  REQUIRE(d.found);
  CHECK(field(d.node, "display_primaries").svalue == "P3-D65");
  CHECK(field(d.node, "display_primaries/display_primaries[0]").svalue == "green");
  CHECK(field(d.node, "display_primaries/display_primaries[1]").svalue == "blue");
  CHECK(field(d.node, "display_primaries/display_primaries[2]").svalue == "red");
  CHECK(field(d.node, "display_primaries/display_primaries[0]/display_primaries_x").uvalue == 13250);
  CHECK(field(d.node, "display_primaries/display_primaries[0]/display_primaries_x").svalue == "0.265");
  CHECK(field(d.node, "white_point_x").svalue == "0.3127");
  CHECK(field(d.node, "white_point_y").svalue == "0.329");
  CHECK(field(d.node, "max_display_mastering_luminance").uvalue == 10000000);
  CHECK(field(d.node, "max_display_mastering_luminance").svalue == "1000.0 cd/m²");
  CHECK(field(d.node, "min_display_mastering_luminance").svalue == "0.0001 cd/m²");
  CHECK(field(d.node, "mastering_display.derived").svalue == "P3-D65 1000/0.0001");
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);

  // Written R, G, B (some muxers) with BT.2020 primaries, 4000 / 0.005 cd/m²: names follow the chromaticities.
  const Bytes rgb = cat({be(35400, 2), be(14600, 2), be(8500, 2), be(39850, 2), be(6550, 2), be(2300, 2), be(15635, 2),
                         be(16450, 2), be(40000000, 4), be(50, 4)});
  const Decoded r = run("mdcv", rgb);
  CHECK(field(r.node, "display_primaries/display_primaries[0]").svalue == "red");
  CHECK(field(r.node, "display_primaries/display_primaries[1]").svalue == "green");
  CHECK(field(r.node, "display_primaries/display_primaries[2]").svalue == "blue");
  CHECK(field(r.node, "mastering_display.derived").svalue == "BT.2020 4000/0.005");

  // Unknown primaries are still named by geometry.
  const Bytes odd = cat({be(5000, 2), be(5000, 2), be(30000, 2), be(15000, 2), be(12000, 2), be(36000, 2), be(15635, 2),
                         be(16450, 2), be(5000000, 4), be(10, 4)});
  const Decoded o = run("mdcv", odd);
  CHECK(field(o.node, "display_primaries").svalue == "custom");
  CHECK(field(o.node, "display_primaries/display_primaries[0]").svalue == "blue");
  CHECK(field(o.node, "display_primaries/display_primaries[1]").svalue == "red");
  CHECK(field(o.node, "display_primaries/display_primaries[2]").svalue == "green");
}

TEST_CASE("clli: content light levels in cd/m²", "[container][decoders][colour]") {
  const Decoded d = run("clli", cat({be(1000, 2), be(400, 2)}));
  CHECK(field(d.node, "max_content_light_level").uvalue == 1000);
  CHECK(field(d.node, "max_content_light_level").svalue == "1000 cd/m²");
  CHECK(field(d.node, "max_pic_average_light_level").svalue == "400 cd/m²");
  check_contiguous(d);
}

TEST_CASE("AT1: an HDR10 hvc1 entry with colr, mdcv and clli is walked and decoded", "[container][decoders][colour]") {
  const Bytes entry = visual_entry(1920, 1080,
                                   cat({box("colr", nclx(9, 16, 9, false)), box("mdcv", mdcv_recipe10()),
                                        box("clli", cat({be(1000, 2), be(400, 2)}))}));
  const iso::WalkResult w = walk_bytes(file_with_entry(entry));
  const BoxNodeT* colr = find_box(w.top_level, "colr");
  const BoxNodeT* mdcv = find_box(w.top_level, "mdcv");
  const BoxNodeT* clli = find_box(w.top_level, "clli");
  REQUIRE(colr != nullptr);
  REQUIRE(mdcv != nullptr);
  REQUIRE(clli != nullptr);
  CHECK(field(*colr, "transfer_characteristics").uvalue == 16);
  CHECK(field(*colr, "transfer_characteristics").svalue == "PQ (SMPTE ST 2084)");
  CHECK(field(*mdcv, "max_display_mastering_luminance").uvalue == 10000000);
  CHECK(field(*mdcv, "max_display_mastering_luminance").svalue == "1000.0 cd/m²");
  CHECK(field(*clli, "max_content_light_level").uvalue == 1000);
  CHECK(colr->diagnostics.empty());
  CHECK(mdcv->diagnostics.empty());
}

// SPEC-ASSUMPTION(#676): recipe 10 is expected to mux mdcv/clli; the corpus is not fetchable yet (synthetic entry).
TEST_CASE("AT1 on the recipe-10 corpus file syn_h265_main10_pq_hdr.mp4",
          "[container][decoders][colour][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("syn_h265_main10_pq_hdr");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult w = iso::walk(*file);
  const BoxNodeT* colr = find_box(w.top_level, "colr");
  const BoxNodeT* mdcv = find_box(w.top_level, "mdcv");
  const BoxNodeT* clli = find_box(w.top_level, "clli");
  REQUIRE(colr != nullptr);
  CHECK(field(*colr, "transfer_characteristics").uvalue == 16);
  CHECK(field(*colr, "transfer_characteristics").svalue == "PQ (SMPTE ST 2084)");
  REQUIRE(mdcv != nullptr);
  CHECK(field(*mdcv, "max_display_mastering_luminance").uvalue == 10000000);
  CHECK(field(*mdcv, "max_display_mastering_luminance").svalue == "1000.0 cd/m²");
  REQUIRE(clli != nullptr);
  CHECK(field(*clli, "max_content_light_level").uvalue == 1000);
}

TEST_CASE("cclv: presence flags, primaries and luminance in 10^-7 cd/m²", "[container][decoders][colour]") {
  // reserved u(2)=0, cancel 0, persistence 1, primaries 1, min 1, max 1, avg 0.
  const Bytes payload = cat({be(0b00011110, 1), be(35400, 4), be(14600, 4), be(8500, 4), be(39850, 4), be(6550, 4),
                             be(2300, 4), be(500, 4), be(10000000000ULL & 0xFFFFFFFFU, 4)});
  const Decoded d = run("cclv", payload);
  REQUIRE(d.found);
  CHECK(field(d.node, "ccv_persistence_flag").uvalue == 1);
  CHECK(field(d.node, "ccv_avg_luminance_value_present_flag").uvalue == 0);
  CHECK(field(d.node, "ccv_primaries/ccv_primaries[0]/ccv_primaries_x").ivalue == 35400);
  CHECK(field(d.node, "ccv_primaries/ccv_primaries[0]/ccv_primaries_x").svalue == "0.708");
  CHECK(field(d.node, "ccv_min_luminance_value").svalue == "0.00005 cd/m²");
  CHECK(child(d.node.fields, "ccv_avg_luminance_value") == nullptr);
  check_contiguous(d);

  const Decoded only_max = run("cclv", cat({be(0b00000010, 1), be(10000000, 4)}));
  CHECK(field(only_max.node, "ccv_max_luminance_value").svalue == "1.0 cd/m²");
  CHECK(child(only_max.node.fields, "ccv_primaries") == nullptr);
}

TEST_CASE("mdcv cut short keeps the fields before the cut", "[container][decoders][colour]") {
  Bytes payload = mdcv_recipe10();
  payload.resize(16);
  const Decoded d = run("mdcv", payload);
  CHECK(d.outcome.truncated);
  CHECK(has_code(d.node, "CNT-1002"));
  CHECK(field(d.node, "white_point_x").uvalue == 15635);
}

// ---- AT2: Dolby Vision ----

// SPEC-ASSUMPTION(#677): no ISOBMFF dv_p81.mp4 corpus entry exists; AT2 uses a synthetic dvvC record.
TEST_CASE("AT2: dvvC profile 8.1 is HDR10 compatible", "[container][decoders][colour]") {
  const Decoded d = run("dvvC", dovi(8, 6, 1));
  REQUIRE(d.found);
  CHECK(field(d.node, "dv_version_major").uvalue == 1);
  CHECK(field(d.node, "dv_profile").uvalue == 8);
  CHECK(field(d.node, "dv_level").uvalue == 6);
  CHECK(field(d.node, "rpu_present_flag").uvalue == 1);
  CHECK(field(d.node, "el_present_flag").uvalue == 0);
  CHECK(field(d.node, "bl_present_flag").uvalue == 1);
  CHECK(field(d.node, "dv_bl_signal_compatibility_id").uvalue == 1);
  CHECK(field(d.node, "dv_bl_signal_compatibility_id").svalue == "HDR10");
  CHECK(field(d.node, "dv_profile").svalue == "Profile 8.1 (HDR10 compatible)");
  CHECK(field(d.node, "dv_profile").svalue.find("HDR10 compatible") != std::string::npos);
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);
}

TEST_CASE("Dolby Vision: CNT-5020 when the box type does not fit the profile", "[container][decoders][colour]") {
  CHECK(has_code(run("dvcC", dovi(8, 6, 1)).node, "CNT-5020"));
  CHECK(has_code(run("dvvC", dovi(5, 6, 0)).node, "CNT-5020"));
  const Decoded p5 = run("dvcC", dovi(5, 6, 0));
  CHECK(p5.node.diagnostics.empty());
  CHECK(field(p5.node, "dv_profile").svalue == "Profile 5");
  CHECK(field(p5.node, "dv_bl_signal_compatibility_id").svalue == "none");
  CHECK(field(run("dvvC", dovi(8, 6, 4)).node, "dv_profile").svalue == "Profile 8.4 (HLG compatible)");
  CHECK(run("dvwC", dovi(20, 1, 0)).node.diagnostics.empty());
}

// ---- AT3: QuickTime nclc ----

TEST_CASE("AT3: a .mov with an nclc colr decodes three fields and no full_range_flag",
          "[container][decoders][colour]") {
  const Bytes entry = visual_entry(1920, 1080, box("colr", nclc(1, 1, 1)));
  const iso::WalkResult mov = walk_bytes(file_with_entry(entry, "qt  "));
  const BoxNodeT* colr = find_box(mov.top_level, "colr");
  REQUIRE(colr != nullptr);
  CHECK(field(*colr, "colour_type").svalue == "nclc");
  CHECK(field(*colr, "colour_primaries").svalue == "BT.709");
  CHECK(field(*colr, "transfer_characteristics").svalue == "BT.709");
  CHECK(field(*colr, "matrix_coefficients").svalue == "BT.709");
  CHECK(field(*colr, "colour_primaries").spec_ref == "QuickTime File Format, Color Parameter Atoms");
  CHECK(child(colr->fields, "full_range_flag") == nullptr);
  CHECK(colr->fields.size() == 4);
  CHECK(colr->diagnostics.empty());

  const iso::WalkResult mp4 = walk_bytes(file_with_entry(entry, "isom"));
  const BoxNodeT* iso_colr = find_box(mp4.top_level, "colr");
  REQUIRE(iso_colr != nullptr);
  CHECK(has_code(*iso_colr, "CNT-5002"));
}

// ---- colr ICC ----

// A minimal ICC header (128 bytes) and tag table with one `desc` tag.
Bytes icc_profile(const Bytes& desc_tag, uint8_t major) {
  Bytes header = zeros(128);
  const Bytes table = cat({be(1, 4), str("desc"), be(144, 4), be(desc_tag.size(), 4)});
  const size_t total = 128 + table.size() + desc_tag.size();
  std::ranges::copy(be(total, 4), header.begin());
  header[8] = major;
  header[9] = 0x30;
  std::ranges::copy(str("mntrRGB "), header.begin() + 12);
  return cat({header, table, desc_tag});
}

TEST_CASE("colr rICC / prof: the profile bytes with an ICC header preview", "[container][decoders][colour]") {
  const Bytes v2_desc = cat({str("desc"), zeros(4), be(11, 4), str("Display P3"), be(0, 1), zeros(3)});
  const Decoded v2 = run("colr", cat({str("prof"), icc_profile(v2_desc, 2)}));
  CHECK(field(v2.node, "ICC_profile").kind == SyntaxValueKind::Bytes);
  CHECK(field(v2.node, "ICC_profile").bytes.size() == 128 + 16 + v2_desc.size());
  CHECK(field(v2.node, "ICC_profile").svalue ==
        "ICC v2.3, mntr RGB, profile size " + std::to_string(128 + 16 + v2_desc.size()) + " bytes, \"Display P3\"");
  check_contiguous(v2);

  const Bytes mluc = cat({str("mluc"), zeros(4), be(1, 4), be(12, 4), str("enUS"), be(8, 4), be(28, 4), be('s', 2),
                          be('R', 2), be('G', 2), be('B', 2)});
  const Decoded v4 = run("colr", cat({str("rICC"), icc_profile(mluc, 4)}));
  CHECK(field(v4.node, "ICC_profile").svalue.starts_with("ICC v4.3, mntr RGB"));
  CHECK(field(v4.node, "ICC_profile").svalue.ends_with(", \"sRGB\""));

  const Decoded short_icc = run("colr", cat({str("prof"), zeros(20)}));
  CHECK(field(short_icc.node, "ICC_profile").svalue == "ICC profile too short (20 bytes)");
  const Decoded unknown = run("colr", cat({str("zzzz"), zeros(3)}));
  CHECK(field(unknown.node, "data").svalue == "unknown colour_type");
}

// ---- pasp, clap, btrt, fiel, srat ----

TEST_CASE("pasp: PAR, and DAR from the enclosing sample entry", "[container][decoders][colour]") {
  const Decoded alone = run("pasp", cat({be(16, 4), be(11, 4)}));
  CHECK(field(alone.node, "hSpacing").uvalue == 16);
  CHECK(field(alone.node, "pixel_aspect_ratio.derived").svalue == "16:11");
  CHECK(child(alone.node.fields, "display_aspect_ratio.derived") == nullptr);
  check_contiguous(alone);
  CHECK(field(run("pasp", zeros(8)).node, "pixel_aspect_ratio.derived").svalue == "invalid (zero spacing)");

  const iso::WalkResult w =
      walk_bytes(file_with_entry(visual_entry(720, 576, box("pasp", cat({be(64, 4), be(45, 4)})))));
  const BoxNodeT* pasp = find_box(w.top_level, "pasp");
  REQUIRE(pasp != nullptr);
  CHECK(field(*pasp, "pixel_aspect_ratio.derived").svalue == "64:45");
  CHECK(field(*pasp, "display_aspect_ratio.derived").svalue == "16:9 (1.778)");
}

Bytes clap(uint32_t w, uint32_t h, int32_t ho, int32_t vo, uint32_t d = 1) {
  return cat({be(w, 4), be(d, 4), be(h, 4), be(d, 4), be(static_cast<uint32_t>(ho), 4), be(d, 4),
              be(static_cast<uint32_t>(vo), 4), be(d, 4)});
}

TEST_CASE("clap: the clean aperture rectangle and CNT-1023", "[container][decoders][colour]") {
  const auto rect = [](const Bytes& payload) {
    const iso::WalkResult w = walk_bytes(file_with_entry(visual_entry(1920, 1088, box("clap", payload))));
    const BoxNodeT* c = find_box(w.top_level, "clap");
    REQUIRE(c != nullptr);
    return std::make_pair(field(*c, "clean_aperture.derived").svalue, has_code(*c, "CNT-1023"));
  };
  CHECK(rect(clap(1920, 1080, 0, -4)) == std::make_pair(std::string("1920x1080 at (0, 0)"), false));
  CHECK(rect(clap(1920, 1080, 0, 0)) == std::make_pair(std::string("1920x1080 at (0, 4)"), false));
  CHECK(rect(clap(1919, 1080, 0, 0)).second);  // left = 0.5
  CHECK(rect(clap(1920, 1080, 0, 8)).second);  // bottom edge outside the picture
  CHECK(rect(clap(3839, 1080, 0, 0, 2)).second);

  const Decoded alone = run("clap", clap(640, 480, 2, 0));
  CHECK(field(alone.node, "horizOffN").uvalue == 2);
  CHECK(field(alone.node, "clean_aperture.derived").svalue == "640x480, centre offset (2, 0)");
  CHECK(alone.node.diagnostics.empty());
  const Decoded zero = run("clap", clap(640, 480, 0, 0, 0));
  CHECK(has_code(zero.node, "CNT-1023"));
}

TEST_CASE("btrt, fiel and srat", "[container][decoders][colour]") {
  const Decoded b = run("btrt", cat({be(13941, 4), be(372144, 4), be(240856, 4)}));
  CHECK(field(b.node, "bufferSizeDB").svalue == "13941 bytes");
  CHECK(field(b.node, "maxBitrate").svalue == "372.144 kbit/s");
  CHECK(field(b.node, "avgBitrate").svalue == "240.856 kbit/s");
  check_contiguous(b);
  CHECK(has_code(run("btrt", zeros(13)).node, "CNT-1005"));

  const Decoded f = run("fiel", cat({be(2, 1), be(9, 1)}));
  CHECK(field(f.node, "fields").svalue == "interlaced");
  CHECK(field(f.node, "detail").svalue == "interleaved fields, top first (top displayed first)");
  CHECK(field(run("fiel", cat({be(1, 1), be(0, 1)})).node, "fields").svalue == "progressive");

  const Decoded s = run("srat", cat({be(0, 4), be(96000, 4)}), ctx_of({"moov", "trak", "stsd", "mp4a"}));
  REQUIRE(s.found);
  CHECK(field(s.node, "sampling_rate").uvalue == 96000);
  CHECK(field(s.node, "sampling_rate").svalue == "96000 Hz");
  check_contiguous(s, 96);
}

// ---- container-smoke corpus ----

TEST_CASE("colour boxes of the container-smoke corpus: Dolby Vision, HLG and the MP4Box pasp goldens",
          "[container][decoders][colour][golden][needs-corpus]") {
  const auto walked = [](std::string_view id) {
    const auto path = ovt::test::corpus_or_skip(id);
    const auto file = ovt::io::RandomAccessFile::open(path.string());
    return iso::walk(*file);
  };
  const iso::WalkResult dv = walked("gpac_dolby_vision_cenc");
  const BoxNodeT* dvcc = find_box(dv.top_level, "dvcC");
  REQUIRE(dvcc != nullptr);
  CHECK(field(*dvcc, "dv_profile").uvalue == 5);
  CHECK(field(*dvcc, "dv_profile").svalue == "Profile 5");
  CHECK(field(*dvcc, "bl_present_flag").uvalue == 1);
  CHECK(dvcc->diagnostics.empty());
  const BoxNodeT* colr = find_box(dv.top_level, "colr");
  REQUIRE(colr != nullptr);
  CHECK(field(*colr, "colour_primaries").svalue == "BT.709");

  // The pasp fields MP4Box printed for this file (tests/golden/isobmff/*.mp4box-diso.json).
  std::ifstream gf(std::filesystem::path(OVT_TEST_SOURCE_DIR) /
                   "tests/golden/isobmff/gpac_dolby_vision_cenc.mp4box-diso.json");
  REQUIRE(gf.good());
  size_t compared = 0;
  for (const auto& b : nlohmann::json::parse(gf)) {
    if (b.at("path").get<std::string>().ends_with("/pasp") && b.contains("fields")) {
      const BoxNodeT* pasp = find_box(dv.top_level, "pasp");
      REQUIRE(pasp != nullptr);
      CHECK(field(*pasp, "hSpacing").uvalue == b.at("fields").at("hSpacing").get<uint64_t>());
      CHECK(field(*pasp, "vSpacing").uvalue == b.at("fields").at("vSpacing").get<uint64_t>());
      ++compared;
    }
  }
  CHECK(compared == 1);

  const iso::WalkResult hlg = walked("mpegff_hev1_clg1_header");
  const BoxNodeT* hlg_colr = find_box(hlg.top_level, "colr");
  REQUIRE(hlg_colr != nullptr);
  CHECK(field(*hlg_colr, "transfer_characteristics").svalue == "HLG (ARIB STD-B67)");
  const iso::WalkResult hvc1 = walked("mpegff_hvc1_only");
  const BoxNodeT* btrt = find_box(hvc1.top_level, "btrt");
  REQUIRE(btrt != nullptr);
  CHECK(field(*btrt, "maxBitrate").uvalue == 372144);
}

}  // namespace
