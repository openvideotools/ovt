// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the generated box registry and its use by the walk (chapter 08 §8.2.10 and §8.2.3 steps 2.10-2.12,
// WI-1.203).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/box_walk.h"
#include "io/memory_file.hpp"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;

iso::BoxContext ctx_of(std::initializer_list<std::string_view> chain) {
  iso::BoxContext c;
  for (const std::string_view s : chain) {
    iso::FourCC cc{};
    std::copy_n(s.begin(), 4, cc.begin());
    c.ancestors.push_back(cc);
  }
  return c;
}

void put32(Bytes& b, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(shift)));
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
iso::WalkResult walk_bytes(const Bytes& data) {
  const ovt::io::MemoryFile f(data);
  return iso::walk(f);
}
const BoxNodeT* child(const BoxNodeT& n, std::string_view fourcc) {
  const auto it = std::ranges::find_if(n.children, [&](const auto& c) { return c->fourcc == fourcc; });
  return it == n.children.end() ? nullptr : it->get();
}
bool has_code(const BoxNodeT& n, std::string_view code) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code; });
}
const ovt::fb::FieldT* field(const BoxNodeT& n, std::string_view name) {
  const auto it = std::ranges::find_if(n.fields, [&](const auto& f) { return f->name == name; });
  return it == n.fields.end() ? nullptr : it->get();
}

// Test decoders, registered into the process-wide registry under fourccs / contexts no real decoder uses.
uint64_t dec_set_track(iso::BoxContext& ctx, ovt::BitReader& /*br*/, iso::FieldBuilder& fb) {
  ctx.track_id = fb.u32("track");
  return 0;
}
uint64_t dec_read_track(iso::BoxContext& ctx, ovt::BitReader& /*br*/, iso::FieldBuilder& fb) {
  fb.u32("value");
  fb.derived(std::to_string(ctx.track_id));
  return 0;
}
uint64_t dec_prefix(iso::BoxContext& /*ctx*/, ovt::BitReader& /*br*/, iso::FieldBuilder& fb) {
  fb.u32("entry_count");
  return fb.bytes_consumed();
}
OVT_BOX_DECODER("zzg1", "*", &dec_set_track)
OVT_BOX_DECODER("zzg2", "*", &dec_read_track)
OVT_BOX_DECODER("stsd", "*/moov/udta", &dec_prefix)  // a context no real stsd decoder uses

}  // namespace

TEST_CASE("acceptance 2: stsd under stbl is a FullBox ContainerWithPrefix", "[container][registry][fast]") {
  const auto* e = iso::lookup("stsd", ctx_of({"moov", "trak", "mdia", "minf", "stbl"}));
  REQUIRE(e != nullptr);
  CHECK(e->kind == iso::BoxKind::ContainerWithPrefix);
  CHECK(e->is_fullbox);
  CHECK(std::string_view(e->allowed_parents) == "stbl");
}

TEST_CASE("mvhd is a FullBox with versions 0 and 1", "[container][registry][fast]") {
  const auto* e = iso::lookup("mvhd", ctx_of({"moov"}));
  REQUIRE(e != nullptr);
  CHECK(e->kind == iso::BoxKind::Leaf);
  CHECK(e->is_fullbox);
  CHECK(e->known_versions_mask == 0x03);
  CHECK(std::string_view(e->spec_ref).starts_with("ISO/IEC 14496-12"));
  CHECK_FALSE(std::string_view(e->description).empty());
}

TEST_CASE("every box of the WI-1.202 hand table is still registered", "[container][registry][fast]") {
  CHECK(iso::lookup("moov", ctx_of({}))->kind == iso::BoxKind::Container);
  CHECK(iso::lookup("tkhd", ctx_of({"moov", "trak"}))->known_versions_mask == 0x03);
  CHECK(iso::lookup("hdlr", ctx_of({"moov", "trak", "mdia"}))->is_fullbox);
  CHECK(iso::lookup("hdlr", ctx_of({"moov", "meta"}))->is_fullbox);
  CHECK(iso::lookup("meta", ctx_of({"moov", "udta"}))->kind == iso::BoxKind::ContainerWithPrefix);
  const auto* sample_entry = iso::lookup("colr", ctx_of({"moov", "trak", "mdia", "minf", "stbl", "stsd", "hvc1"}));
  const auto* property = iso::lookup("colr", ctx_of({"meta", "iprp", "ipco"}));
  REQUIRE(sample_entry != nullptr);
  REQUIRE(property != nullptr);
  CHECK(std::string_view(sample_entry->spec_ref).starts_with("ISO/IEC 14496-12"));
  CHECK(std::string_view(property->spec_ref).starts_with("ISO/IEC 23008-12"));
  CHECK(std::string_view(property->context_pattern) == "*/ipco");
}

TEST_CASE("the generated table is sorted, unique and well formed", "[container][registry][fast]") {
  const auto rows = iso::generated_entries();
  REQUIRE(rows.size() > 300);
  std::set<std::pair<std::string, std::string>> seen;
  for (size_t i = 0; i < rows.size(); ++i) {
    const iso::RegistryEntry& e = rows[i];
    const std::string_view cc = e.fourcc;
    CAPTURE(cc, e.context_pattern);
    CHECK((cc.size() == 4 || cc.starts_with("uuid:")));
    CHECK(e.decoder == nullptr);
    CHECK(seen.emplace(e.fourcc, e.context_pattern).second);
    CHECK((e.is_fullbox || e.known_versions_mask == 0));
    CHECK(e.known_flag_mask <= 0xFFFFFFU);
    if (i > 0) {
      CHECK(std::tuple(std::string_view(rows[i - 1].fourcc), std::string_view(rows[i - 1].context_pattern)) <
            std::tuple(cc, std::string_view(e.context_pattern)));
    }
  }
}

TEST_CASE("kinds follow the §8.2.10 step 4 lists", "[container][registry][fast]") {
  for (const char* cc : {"mdat", "free", "skip", "idat", "wide"}) {
    CAPTURE(cc);
    CHECK(iso::lookup(cc, ctx_of({}))->kind == iso::BoxKind::Opaque);
  }
  for (const char* cc : {"trak", "mdia", "minf", "stbl", "edts", "dinf", "mvex", "moof", "traf", "udta", "sinf"}) {
    CAPTURE(cc);
    CHECK(iso::lookup(cc, ctx_of({"moov"}))->kind == iso::BoxKind::Container);
  }
  CHECK(iso::lookup("dref", ctx_of({"dinf"}))->kind == iso::BoxKind::ContainerWithPrefix);
  CHECK(iso::lookup("avcC", ctx_of({"stsd", "avc1"}))->kind == iso::BoxKind::Leaf);
  CHECK(iso::lookup("avcC", ctx_of({"stsd", "avc1"}))->has_trailing_data_ok);
  CHECK(iso::lookup("hvc1", ctx_of({"stbl", "stsd"}))->kind == iso::BoxKind::ContainerWithPrefix);
  CHECK(iso::lookup("tkhd", ctx_of({"trak"}))->known_flag_mask == 0x00000F);
}

TEST_CASE("extra boxes: CENC, codec configurations, QuickTime, iTunes, PIFF", "[container][registry][fast]") {
  CHECK(iso::lookup("pssh", ctx_of({"moov"}))->is_fullbox);
  CHECK(iso::lookup("av1C", ctx_of({"stsd", "av01"}))->kind == iso::BoxKind::Leaf);
  CHECK(iso::lookup("esds", ctx_of({"stsd", "mp4a"}))->is_fullbox);
  CHECK(iso::lookup("ilst", ctx_of({"moov", "udta", "meta"}))->kind == iso::BoxKind::Container);
  CHECK(iso::lookup("\xA9xyz", ctx_of({"moov", "udta"})) != nullptr);
  CHECK(iso::lookup("tmcd", ctx_of({"trak", "tref"}))->kind == iso::BoxKind::Leaf);
  CHECK(iso::lookup("tmcd", ctx_of({"stbl", "stsd"}))->kind == iso::BoxKind::ContainerWithPrefix);
  CHECK(iso::lookup("keys", ctx_of({"moov", "udta", "meta"}))->is_fullbox);
  const std::array<uint8_t, 16> piff_tenc{0x89, 0x74, 0xdb, 0xce, 0x7b, 0xe7, 0x4c, 0x51,
                                          0x84, 0xf9, 0x71, 0x48, 0xf9, 0x88, 0x25, 0x54};
  const auto* e = iso::lookup_uuid(piff_tenc);
  REQUIRE(e != nullptr);
  CHECK(e->is_fullbox);
  CHECK(std::string_view(e->description).find("PIFF") != std::string_view::npos);
}

TEST_CASE("MP4RA name tables", "[container][registry][fast]") {
  const auto has = [](std::span<const iso::RegistryName> table, std::string_view code) {
    return std::ranges::any_of(table, [&](const iso::RegistryName& n) { return std::string_view(n.code) == code; });
  };
  CHECK(has(iso::generated_brands(), "isom"));
  CHECK(has(iso::generated_brands(), "qt  "));
  CHECK_FALSE(has(iso::generated_brands(), "zzzz"));
  CHECK(has(iso::generated_handlers(), "vide"));
  CHECK(has(iso::generated_item_types(), "hvc1"));
  CHECK(has(iso::generated_schemes(), "cenc"));
  CHECK(iso::brand_description("zzzz").empty());
  for (const auto table :
       {iso::generated_brands(), iso::generated_handlers(), iso::generated_item_types(), iso::generated_schemes()}) {
    REQUIRE_FALSE(table.empty());
    CHECK(std::ranges::is_sorted(table, {}, [](const iso::RegistryName& n) { return std::string_view(n.code); }));
  }
}

TEST_CASE("every fourcc a hand decoder registers has a registry row", "[container][registry][fast]") {
  const std::filesystem::path dir = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "core/src/container/isobmff";
  const std::regex reg(R"re(OVT_BOX_DECODER\(\s*"([^"]+)")re");
  for (const auto& f : std::filesystem::recursive_directory_iterator(dir)) {
    if (f.path().extension() != ".cpp") {
      continue;
    }
    std::ifstream in(f.path());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (auto it = std::sregex_iterator(text.begin(), text.end(), reg); it != std::sregex_iterator(); ++it) {
      const std::string cc = (*it)[1];
      CAPTURE(f.path().string(), cc);
      const std::string base = cc.ends_with("/*") ? cc.substr(0, cc.size() - 2) : cc;
      const bool listed = std::ranges::any_of(
          iso::generated_entries(), [&](const iso::RegistryEntry& e) { return std::string_view(e.fourcc) == base; });
      CHECK(listed);
    }
  }
}

TEST_CASE("acceptance 3: an unknown fourcc has no row and the walk reports CNT-1001", "[container][registry][fast]") {
  CHECK(iso::lookup("zzzz", ctx_of({"moov"})) == nullptr);
  const auto r = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("moov", box("zzzz", Bytes(4, 0)))}));
  REQUIRE(r.top_level.size() == 2);
  const BoxNodeT& moov = *r.top_level[1];
  CHECK_FALSE(has_code(moov, "CNT-1001"));
  CHECK(moov.description == iso::lookup("moov", ctx_of({}))->description);
  CHECK(moov.spec_ref.starts_with("ISO/IEC 14496-12"));
  const BoxNodeT* z = child(moov, "zzzz");
  REQUIRE(z != nullptr);
  CHECK(has_code(*z, "CNT-1001"));
  CHECK(z->description == "unknown");
  CHECK(z->payload_span->length() == 4);
}

TEST_CASE("the walk reads FullBox version/flags from the registry", "[container][registry][fast]") {
  Bytes mvhd_payload{1, 0, 0, 5};
  mvhd_payload.resize(112, 0);
  const auto r = walk_bytes(
      cat({box("ftyp", Bytes(8, 'a')), box("moov", cat({box("mvhd", mvhd_payload), box("mvhd", Bytes(2, 0))}))}));
  const BoxNodeT& moov = *r.top_level[1];
  REQUIRE(moov.children.size() == 2);
  const BoxNodeT& mvhd = *moov.children[0];
  CHECK(mvhd.version == 1);
  CHECK(mvhd.flags == 5);
  REQUIRE(mvhd.fields.size() == 2);
  CHECK(mvhd.fields[0]->name == "version");
  CHECK(mvhd.fields[1]->name == "flags");
  CHECK(mvhd.fields[1]->span->bit_offset() == 72);
  CHECK(mvhd.payload_span->offset() == mvhd.file_span->offset() + 12);
  CHECK(has_code(*moov.children[1], "CNT-0110"));  // a FullBox of 2 payload bytes
}

TEST_CASE("the walk runs hand decoders and carries their context to later siblings", "[container][registry][fast]") {
  Bytes track;
  put32(track, 7);
  Bytes value;
  put32(value, 99);
  const auto r =
      walk_bytes(cat({box("ftyp", Bytes(8, 'a')),
                      box("moov", cat({box("zzg2", value), box("zzg1", track), box("trak", box("zzg2", value))}))}));
  const BoxNodeT& moov = *r.top_level[1];
  REQUIRE(moov.children.size() == 3);
  const auto* before = field(*moov.children[0], "value");
  REQUIRE(before != nullptr);
  CHECK(before->svalue == "0");  // the context change does not reach earlier siblings
  CHECK(field(*moov.children[1], "track")->uvalue == 7);
  CHECK(moov.children[1]->payload_span == nullptr);
  const auto* after = field(*moov.children[2]->children.at(0), "value");
  REQUIRE(after != nullptr);
  CHECK(after->uvalue == 99);
  CHECK(after->svalue == "7");
}

TEST_CASE("a ContainerWithPrefix decoder sets where the children start", "[container][registry][fast]") {
  Bytes stsd_payload{0, 0, 0, 0};  // version/flags
  put32(stsd_payload, 1);          // entry_count
  const Bytes entry = box("abcd", Bytes(4, 0));
  stsd_payload.insert(stsd_payload.end(), entry.begin(), entry.end());
  const auto r = walk_bytes(cat({box("ftyp", Bytes(8, 'a')), box("moov", box("udta", box("stsd", stsd_payload))),
                                 box("udta", box("stsd", stsd_payload))}));
  const BoxNodeT* stsd = child(*child(*r.top_level[1], "udta"), "stsd");
  REQUIRE(stsd != nullptr);
  CHECK(field(*stsd, "entry_count")->uvalue == 1);
  CHECK(field(*stsd, "entry_count")->span->bit_offset() == 96);  // after the header and version/flags
  REQUIRE(stsd->children.size() == 1);
  CHECK(stsd->children[0]->fourcc == "abcd");
  CHECK(stsd->children[0]->file_span->offset() == stsd->file_span->offset() + 16);
  // Without a decoder (no moov ancestor) the prefix length is unknown, so stsd stays a leaf.
  const BoxNodeT* bare = child(*r.top_level[2], "stsd");
  REQUIRE(bare != nullptr);
  CHECK(bare->children.empty());
  CHECK(bare->payload_span->length() == 16);
}
