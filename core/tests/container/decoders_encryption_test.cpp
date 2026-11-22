// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group H hand decoders (chapter 08 §8.3.8, WI-1.213): frma/schm/tenc fields and the IV-size context,
// pssh with the system table and the Widevine and PlayReady payload decoders, senc with a known, a track-wide and an
// inferred IV size, and the walk of a whole encrypted file.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/decoders/pb_lite.h"
#include "container/isobmff/decoders/pssh_systems.h"
#include "container/isobmff/field_builder.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;
using ovt::fb::FieldT;

Bytes be(uint64_t v, unsigned nbytes) {
  Bytes b;
  for (unsigned i = nbytes; i > 0; --i) {
    b.push_back(static_cast<uint8_t>(v >> (8U * (i - 1U))));
  }
  return b;
}
Bytes le(uint64_t v, unsigned nbytes) {
  Bytes b = be(v, nbytes);
  std::ranges::reverse(b);
  return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}
Bytes str(std::string_view s) {
  return {s.begin(), s.end()};
}
Bytes box(std::string_view type, const Bytes& payload = {}) {
  return cat({be(8 + payload.size(), 4), str(type), payload});
}
Bytes full(uint8_t version, uint32_t flags, const Bytes& body) {
  return cat({be(version, 1), be(flags, 3), body});
}
Bytes filled(size_t n, uint8_t v) {
  return Bytes(n, v);
}

Bytes utf16le(std::string_view s, bool bom = true) {
  Bytes b = bom ? Bytes{0xFF, 0xFE} : Bytes{};
  for (const char c : s) {
    b.push_back(static_cast<uint8_t>(c));
    b.push_back(0);
  }
  return b;
}

iso::FourCC cc_of(std::string_view s) {
  iso::FourCC cc{};
  std::copy_n(s.begin(), 4, cc.begin());
  return cc;
}

iso::BoxContext ctx_in(std::string_view parent) {
  iso::BoxContext c;
  c.ancestors.push_back(cc_of(parent));
  c.enc_for_track = std::make_shared<std::map<uint32_t, iso::TrackEncryption>>();
  return c;
}

void add_uint(BoxNodeT& n, std::string_view name, uint64_t v, uint64_t bit, uint32_t len) {
  auto f = std::make_unique<FieldT>();
  f->name = std::string(name);
  f->kind = ovt::fb::SyntaxValueKind::UInt;
  f->uvalue = v;
  f->span = std::make_unique<ovt::fb::BitSpan>(bit, len);
  n.fields.push_back(std::move(f));
}

struct Decoded {
  BoxNodeT node;
  iso::DecodeOutcome outcome;
};

// Runs the registered decoder of `type` the way the walker does. `payload` includes version/flags for a FullBox.
Decoded run(std::string_view type, const Bytes& payload, iso::BoxContext& ctx, bool fullbox = true) {
  Decoded d;
  d.node.fourcc = std::string(type);
  d.node.header_length = 8;
  d.node.file_span = std::make_unique<ovt::fb::ByteSpan>(0, 8 + payload.size());
  const iso::RegistryEntry* e = iso::lookup(type, ctx);
  REQUIRE(e != nullptr);
  REQUIRE(e->decoder != nullptr);
  size_t skip = 0;
  if (fullbox) {
    REQUIRE(payload.size() >= 4);
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

const FieldT* child(const std::vector<std::unique_ptr<FieldT>>& fields, std::string_view name) {
  for (const auto& f : fields) {
    if (f->name == name) {
      return f.get();
    }
  }
  return nullptr;
}

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

bool has_field(const BoxNodeT& n, std::string_view path) {
  const std::vector<std::unique_ptr<FieldT>>* level = &n.fields;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    const FieldT* f = child(*level, path.substr(0, slash));
    if (f == nullptr) {
      return false;
    }
    level = &f->children;
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
  }
  return true;
}

bool has_code(const BoxNodeT& n, std::string_view code) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code; });
}

const Bytes kKid = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
constexpr std::string_view kKidText = "01234567-89ab-cdef-0123-456789abcdef";

const Bytes kWidevineId = {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                           0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
const Bytes kPlayReadyId = {0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86,
                            0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95};

// tests/corpora/synthetic/widevine_pssh.bin: algorithm 1, one key_id, provider "ovt-test", content_id
// "syn_mp4_cenc_widevine".
Bytes widevine_data() {
  return cat({Bytes{0x08, 0x01, 0x12, 0x10}, kKid, Bytes{0x1a, 0x08}, str("ovt-test"), Bytes{0x22, 0x15},
              str("syn_mp4_cenc_widevine")});
}

Bytes tenc_v0(uint8_t is_protected, uint8_t iv_size, const Bytes& kid = kKid) {
  return full(0, 0, cat({be(0, 2), be(is_protected, 1), be(iv_size, 1), kid}));
}

Bytes tenc_v1_cbcs(uint8_t crypt, uint8_t skip, const Bytes& constant_iv) {
  return full(1, 0,
              cat({be(0, 1), be(static_cast<uint32_t>((crypt << 4U) | skip), 1), be(1, 1), be(0, 1), kKid,
                   be(constant_iv.size(), 1), constant_iv}));
}

}  // namespace

TEST_CASE("frma and schm decode the scheme and its version", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("sinf");
  const Decoded f = run("frma", str("avc1"), ctx, false);
  CHECK(field(f.node, "data_format").svalue == "avc1");
  CHECK(f.node.diagnostics.empty());

  const Decoded s = run("schm", full(0, 0, cat({str("cbcs"), be(0x00010000, 4)})), ctx);
  CHECK(field(s.node, "scheme_type").svalue == "cbcs");
  CHECK(field(s.node, "scheme_type.derived").svalue == "AES-CBC with pattern");
  CHECK(field(s.node, "scheme_version").uvalue == 0x00010000);
  CHECK(field(s.node, "scheme_version").svalue == "1.0");
  CHECK(!has_field(s.node, "scheme_uri"));

  const Decoded u =
      run("schm", full(0, 1, cat({str("piff"), be(0x00010001, 4), str("http://example.com/x"), Bytes{0}})), ctx);
  CHECK(field(u.node, "scheme_uri").svalue == "http://example.com/x");
  CHECK(field(u.node, "scheme_type.derived").svalue == "PIFF (Smooth Streaming)");
  CHECK(field(u.node, "scheme_version").svalue == "1.1");
  CHECK(u.node.diagnostics.empty());

  const Decoded other = run("schm", full(0, 0, cat({str("zzzz"), be(1, 4)})), ctx);
  CHECK(!has_field(other.node, "scheme_type.derived"));
  const Decoded v1 = run("schm", full(1, 0, cat({str("cenc"), be(1, 4)})), ctx);
  CHECK(has_code(v1.node, "CNT-1003"));
}

TEST_CASE("tenc v0 decodes the defaults and sets the IV size of the context and of the track",
          "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("schi");
  ctx.track_id = 7;
  const Decoded d = run("tenc", tenc_v0(1, 8), ctx);
  CHECK(field(d.node, "default_isProtected").uvalue == 1);
  CHECK(field(d.node, "default_Per_Sample_IV_Size").uvalue == 8);
  CHECK(field(d.node, "default_KID").svalue == kKidText);
  CHECK(field(d.node, "default_KID").bytes == kKid);
  CHECK(d.node.diagnostics.empty());
  CHECK(ctx.per_sample_iv_size == 8);
  REQUIRE(ctx.enc_for_track->contains(7));
  CHECK(ctx.enc_for_track->at(7).per_sample_iv_size == 8);
  CHECK(ctx.enc_for_track->at(7).is_protected);
  // The first tenc of a track wins.
  iso::BoxContext second = ctx;
  static_cast<void>(run("tenc", tenc_v0(1, 16), second));
  CHECK(second.enc_for_track->at(7).per_sample_iv_size == 8);
}

TEST_CASE("tenc v1 carries the pattern and a constant IV", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("schi");
  const Bytes iv = filled(16, 0xAB);
  const Decoded d = run("tenc", tenc_v1_cbcs(1, 9, iv), ctx);
  CHECK(field(d.node, "default_crypt_byte_block").uvalue == 1);
  CHECK(field(d.node, "default_skip_byte_block").uvalue == 9);
  CHECK(field(d.node, "pattern").svalue == "1:9");
  CHECK(field(d.node, "default_Per_Sample_IV_Size").uvalue == 0);
  CHECK(field(d.node, "default_constant_IV_size").uvalue == 16);
  CHECK(field(d.node, "default_constant_IV").bytes == iv);
  CHECK(d.node.diagnostics.empty());
  CHECK(ctx.per_sample_iv_size == 0);
}

TEST_CASE("tenc rejects an IV size other than 0, 8 and 16 (CNT-6003)", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("schi");
  ctx.track_id = 1;
  const Decoded d = run("tenc", tenc_v0(1, 4), ctx);
  CHECK(has_code(d.node, "CNT-6003"));
  CHECK(ctx.per_sample_iv_size == 0xFF);
  CHECK(ctx.enc_for_track->empty());
  const Decoded unknown = run("tenc", full(3, 0, filled(20, 0)), ctx);
  CHECK(has_code(unknown.node, "CNT-1003"));
}

TEST_CASE("pssh v0: the system table names Widevine and the protobuf key_id equals the tenc KID",
          "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("moov");
  const Bytes data = widevine_data();
  const Decoded p = run("pssh", full(0, 0, cat({kWidevineId, be(data.size(), 4), data})), ctx);
  CHECK(field(p.node, "SystemID").svalue == "Widevine");
  CHECK(field(p.node, "DataSize").uvalue == data.size());
  CHECK(field(p.node, "Data").bytes == data);
  CHECK(field(p.node, "Data/algorithm").uvalue == 1);
  CHECK(field(p.node, "Data/algorithm").svalue == "AESCTR");
  CHECK(field(p.node, "Data/key_id[0]").bytes == kKid);
  CHECK(field(p.node, "Data/key_id[0]").svalue == kKidText);
  CHECK(field(p.node, "Data/provider").svalue == "ovt-test");
  CHECK(field(p.node, "Data/content_id").svalue == "syn_mp4_cenc_widevine");
  CHECK(p.node.diagnostics.empty());

  iso::BoxContext schi = ctx_in("schi");
  const Decoded t = run("tenc", tenc_v0(1, 8), schi);
  CHECK(field(p.node, "Data/key_id[0]").bytes == field(t.node, "default_KID").bytes);
  // The child spans lie inside the Data field.
  const FieldT& dat = field(p.node, "Data");
  const FieldT& kid = field(p.node, "Data/key_id[0]");
  CHECK(kid.span->bit_offset() >= dat.span->bit_offset());
  CHECK(kid.span->bit_offset() + kid.span->bit_length() <= dat.span->bit_offset() + dat.span->bit_length());
  CHECK(kid.span->bit_length() == 128);
}

TEST_CASE("pssh Widevine data decodes repeated key_ids, policy, period index and scheme",
          "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("moov");
  const Bytes kid2 = filled(16, 0x22);
  // protection_scheme is a varint of the fourcc 'cbcs' = 0x63626373.
  const Bytes scheme_varint = Bytes{0xF3, 0xC6, 0x89, 0x9B, 0x06};
  const Bytes real =
      cat({Bytes{0x12, 0x10}, kKid, Bytes{0x12, 0x10}, kid2, Bytes{0x32, 0x04}, str("pol1"), Bytes{0x38, 0x85, 0x01},
           Bytes{0x42, 0x02, 0xAA, 0xBB}, Bytes{0x48}, scheme_varint, Bytes{0xA8, 0x06, 0x05}});
  const Decoded p = run("pssh", full(0, 0, cat({kWidevineId, be(real.size(), 4), real})), ctx);
  CHECK(field(p.node, "Data/key_id[0]").bytes == kKid);
  CHECK(field(p.node, "Data/key_id[1]").bytes == kid2);
  CHECK(field(p.node, "Data/policy").svalue == "pol1");
  CHECK(field(p.node, "Data/crypto_period_index").uvalue == 133);
  CHECK(field(p.node, "Data/grouped_license").bytes == Bytes{0xAA, 0xBB});
  CHECK(field(p.node, "Data/protection_scheme").svalue == "cbcs");
  // field 101 (unknown) varint
  CHECK(field(p.node, "Data/field_101").uvalue == 5);
}

TEST_CASE("pssh v1 lists the KIDs; the box size is checked (CNT-6004); unknown systems warn (CNT-6009)",
          "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("moov");
  const Bytes common = {0x10, 0x77, 0xef, 0xec, 0xc0, 0xb2, 0x4d, 0x02, 0xac, 0xe3, 0x3c, 0x1e, 0x52, 0xe2, 0xfb, 0x4b};
  const Decoded ok = run("pssh", full(1, 0, cat({common, be(2, 4), kKid, filled(16, 0x33), be(0, 4)})), ctx);
  CHECK(field(ok.node, "SystemID").svalue == "W3C Common PSSH (ClearKey)");
  CHECK(field(ok.node, "KID_count").uvalue == 2);
  CHECK(field(ok.node, "KIDs/KID[0]").svalue == kKidText);
  CHECK(field(ok.node, "KIDs/KID[1]").bytes == filled(16, 0x33));
  CHECK(ok.node.diagnostics.empty());

  const Decoded big = run("pssh", full(0, 0, cat({kWidevineId, be(100, 4), filled(10, 0)})), ctx);
  CHECK(has_code(big.node, "CNT-6004"));
  CHECK(field(big.node, "Data").bytes.size() == 10);
  const Decoded small = run("pssh", full(0, 0, cat({kWidevineId, be(2, 4), filled(10, 0)})), ctx);
  CHECK(has_code(small.node, "CNT-6004"));
  const Decoded kids = run("pssh", full(1, 0, cat({common, be(1000000, 4), filled(32, 0)})), ctx);
  CHECK(has_code(kids.node, "CNT-6004"));

  const Decoded unknown = run("pssh", full(0, 0, cat({filled(16, 0x99), be(0, 4)})), ctx);
  CHECK(has_code(unknown.node, "CNT-6009"));
  CHECK(field(unknown.node, "SystemID").svalue == "99999999-9999-9999-9999-999999999999");
}

TEST_CASE("pssh PlayReady: the object, its records and the WRMHEADER KID and LA_URL", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("moov");
  // KID bytes 00112233-4455-6677-8899-aabbccddeeff in CENC order; the GUID in the base64 has the first three
  // groups little-endian: 33 22 11 00 55 44 77 66 88 99 aa bb cc dd ee ff -> "MyIRAFVEd2aImaq7zN3u/w==".
  const std::string xml =
      "<WRMHEADER xmlns=\"http://schemas.microsoft.com/DRM/2007/03/PlayReadyHeader\" version=\"4.0.0.0\"><DATA>"
      "<PROTECTINFO><KEYLEN>16</KEYLEN><ALGID>AESCTR</ALGID></PROTECTINFO><KID>MyIRAFVEd2aImaq7zN3u/w==</KID>"
      "<LA_URL>https://license.example.com/rightsmanager.asmx</LA_URL></DATA></WRMHEADER>";
  const Bytes xml16 = utf16le(xml);
  const Bytes record = cat({le(1, 2), le(xml16.size(), 2), xml16});
  const Bytes object = cat({le(10 + record.size(), 4), le(1, 2), record});
  const Decoded p = run("pssh", full(0, 0, cat({kPlayReadyId, be(object.size(), 4), object})), ctx);
  CHECK(field(p.node, "SystemID").svalue == "PlayReady");
  CHECK(field(p.node, "Data/record_count").uvalue == 1);
  CHECK(field(p.node, "Data/record[0]/record_type").uvalue == 1);
  CHECK(field(p.node, "Data/record[0]/record_value").svalue == xml);
  CHECK(field(p.node, "Data/record[0]/KID[0]").svalue ==
        "00112233-4455-6677-8899-aabbccddeeff (base64 MyIRAFVEd2aImaq7zN3u/w==)");
  CHECK(field(p.node, "Data/record[0]/LA_URL").svalue == "https://license.example.com/rightsmanager.asmx");
  CHECK(p.node.diagnostics.empty());
}

TEST_CASE("pssh PlayReady: WRMHEADER 4.1 KID attributes, no BOM, a second record, truncation",
          "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("moov");
  const std::string xml =
      "<WRMHEADER version=\"4.2.0.0\"><DATA><KIDS><KID ALGID=\"AESCBC\" VALUE=\"MyIRAFVEd2aImaq7zN3u/w==\"></KID>"
      "<KID ALGID=\"AESCBC\" VALUE=\"AAAAAAAAAAAAAAAAAAAAAA==\"></KID></KIDS></DATA></WRMHEADER>";
  const Bytes xml16 = utf16le(xml, false);
  const Bytes rec1 = cat({le(1, 2), le(xml16.size(), 2), xml16});
  const Bytes rec3 = cat({le(3, 2), le(3, 2), Bytes{1, 2, 3}});
  const Bytes object = cat({le(10 + rec1.size() + rec3.size(), 4), le(2, 2), rec1, rec3});
  const Decoded p = run("pssh", full(0, 0, cat({kPlayReadyId, be(object.size(), 4), object})), ctx);
  CHECK(field(p.node, "Data/record[0]/KID[0]").svalue.starts_with("00112233-4455-6677-8899-aabbccddeeff"));
  CHECK(field(p.node, "Data/record[0]/KID[1]").svalue.starts_with("00000000-0000-0000-0000-000000000000"));
  CHECK(field(p.node, "Data/record[1]/record_type").svalue == "embedded license store");
  CHECK(field(p.node, "Data/record[1]/record_value").bytes == Bytes{1, 2, 3});
  CHECK(p.node.diagnostics.empty());

  // A record that is longer than the object, and an object shorter than its header: Info only, no throw.
  const Bytes cut = cat({le(30, 4), le(1, 2), le(1, 2), le(200, 2), Bytes{0, 0}});
  const Decoded t = run("pssh", full(0, 0, cat({kPlayReadyId, be(cut.size(), 4), cut})), ctx);
  CHECK(has_code(t.node, "CNT-1005"));
  const Decoded tiny = run("pssh", full(0, 0, cat({kPlayReadyId, be(3, 4), Bytes{1, 2, 3}})), ctx);
  CHECK(has_code(tiny.node, "CNT-1005"));
}

TEST_CASE("pssh Widevine data that is not protobuf keeps the fields before the error", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("moov");
  const Bytes data = cat({Bytes{0x08, 0x01}, Bytes{0x12, 0x40, 0x01}});  // key_id longer than the message
  const Decoded p = run("pssh", full(0, 0, cat({kWidevineId, be(data.size(), 4), data})), ctx);
  CHECK(field(p.node, "Data/algorithm").uvalue == 1);
  CHECK(has_code(p.node, "CNT-1005"));
  // A known field number with the wrong wire type is shown as an unknown field.
  const Bytes wrong = cat({Bytes{0x0A, 0x01, 0x07}});  // field 1 as length-delimited
  const Decoded w = run("pssh", full(0, 0, cat({kWidevineId, be(wrong.size(), 4), wrong})), ctx);
  CHECK(!has_field(w.node, "Data/algorithm"));
  CHECK(field(w.node, "Data/field_1").kind == ovt::fb::SyntaxValueKind::Bytes);
}

TEST_CASE("pb_lite reads the four wire types and stops on malformed input", "[container][decoders][cenc]") {
  namespace pb = iso::pb;
  const Bytes msg = cat({Bytes{0x08, 0x96, 0x01}, Bytes{0x11}, le(0x0102030405060708ULL, 8),
                         Bytes{0x1A, 0x02, 'h', 'i'}, Bytes{0x25}, le(0xAABBCCDD, 4)});
  pb::Reader r(msg);
  pb::Field f;
  REQUIRE(r.next(f));
  CHECK((f.number == 1 && f.wire == pb::WireType::Varint && f.value == 150 && f.size == 2 && f.offset == 1));
  REQUIRE(r.next(f));
  CHECK((f.number == 2 && f.wire == pb::WireType::Fixed64 && f.value == 0x0102030405060708ULL));
  REQUIRE(r.next(f));
  CHECK((f.number == 3 && f.payload.size() == 2 && f.payload[0] == 'h'));
  REQUIRE(r.next(f));
  CHECK((f.number == 4 && f.wire == pb::WireType::Fixed32 && f.value == 0xAABBCCDD));
  CHECK(!r.next(f));
  CHECK(r.ok());

  for (const Bytes& bad : {Bytes{0x08}, Bytes{0x08, 0x80}, Bytes{0x12, 0x05, 1}, Bytes{0x00, 0x01}, Bytes{0x0B},
                           Bytes{0x09, 1, 2}, Bytes{0x0D, 1}, filled(11, 0x80)}) {
    pb::Reader br(bad);
    while (br.next(f)) {
    }
    CHECK(!br.ok());
  }
}

TEST_CASE("the pssh system table names every system of §8.3.8", "[container][decoders][cenc]") {
  CHECK(iso::pssh_systems().size() == 12);
  for (const iso::PsshSystem& s : iso::pssh_systems()) {
    CHECK(iso::pssh_system_name(s.id) == s.name);
  }
  CHECK(iso::uuid_text(iso::pssh_systems()[0].id) == "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed");
  const std::array<uint8_t, 16> none{};
  CHECK(iso::pssh_system_name(none).empty());
}

namespace {

// senc: sample_count, then per sample an IV and (with subsamples) a count and (clear, protected) pairs.
Bytes senc_sample(const Bytes& iv, std::initializer_list<std::pair<uint16_t, uint32_t>> subs, bool with_subs) {
  Bytes b = iv;
  if (with_subs) {
    b = cat({b, be(subs.size(), 2)});
    for (const auto& [c, p] : subs) {
      b = cat({b, be(c, 2), be(p, 4)});
    }
  }
  return b;
}

}  // namespace

TEST_CASE("senc with a known IV size: IVs and subsample pairs", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.per_sample_iv_size = 8;
  const Bytes s0 = senc_sample(filled(8, 0x11), {{4, 100}, {0, 50}}, true);
  const Bytes s1 = senc_sample(filled(8, 0x22), {{16, 200}}, true);
  const Decoded d = run("senc", full(0, 2, cat({be(2, 4), s0, s1})), ctx);
  CHECK(field(d.node, "flags").svalue == "use-subsample-encryption");
  CHECK(field(d.node, "sample_count").uvalue == 2);
  CHECK(field(d.node, "Per_Sample_IV_Size").svalue == "8 (tenc (context))");
  CHECK(field(d.node, "samples/sample[0]/InitializationVector").bytes == filled(8, 0x11));
  CHECK(field(d.node, "samples/sample[0]/subsample_count").uvalue == 2);
  CHECK(field(d.node, "samples/sample[0]/subsamples/subsample[0]/BytesOfClearData").uvalue == 4);
  CHECK(field(d.node, "samples/sample[0]/subsamples/subsample[1]/BytesOfProtectedData").uvalue == 50);
  CHECK(field(d.node, "samples/sample[0]").svalue == "clear 4 + protected 150 = 154 bytes");
  CHECK(field(d.node, "samples/sample[1]").svalue == "clear 16 + protected 200 = 216 bytes");
  CHECK(field(d.node, "subsample_totals").svalue == "3 subsamples, clear 20 bytes, protected 350 bytes");
  CHECK(d.node.diagnostics.empty());
}

TEST_CASE("senc without subsample flag lists only IVs; 16-byte IVs", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.per_sample_iv_size = 16;
  const Decoded d = run("senc", full(0, 0, cat({be(3, 4), filled(16, 1), filled(16, 2), filled(16, 3)})), ctx);
  CHECK(field(d.node, "samples/sample[2]/InitializationVector").bytes == filled(16, 3));
  CHECK(!has_field(d.node, "subsample_totals"));
  CHECK(d.node.diagnostics.empty());
}

TEST_CASE("senc with a constant IV (Per_Sample_IV_Size 0) holds only subsample pairs", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.per_sample_iv_size = 0;
  const Bytes s0 = senc_sample({}, {{32, 160}}, true);
  const Bytes s1 = senc_sample({}, {{0, 96}, {5, 0}}, true);
  const Decoded d = run("senc", full(0, 2, cat({be(2, 4), s0, s1})), ctx);
  CHECK(!has_field(d.node, "samples/sample[0]/InitializationVector"));
  CHECK(field(d.node, "samples/sample[0]").svalue == "clear 32 + protected 160 = 192 bytes");
  CHECK(field(d.node, "samples/sample[1]").svalue == "clear 5 + protected 96 = 101 bytes");
  CHECK(d.node.diagnostics.empty());
  // Without subsamples there is nothing per sample.
  const Decoded none = run("senc", full(0, 0, be(4, 4)), ctx);
  CHECK(none.node.diagnostics.empty());
  CHECK(!has_field(none.node, "samples"));
}

TEST_CASE("senc reads the IV size of its track from the shared table", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.track_id = 2;
  (*ctx.enc_for_track)[2] = iso::TrackEncryption{16, true};
  (*ctx.enc_for_track)[1] = iso::TrackEncryption{8, true};
  const Decoded d = run("senc", full(0, 0, cat({be(1, 4), filled(16, 9)})), ctx);
  CHECK(field(d.node, "Per_Sample_IV_Size").svalue == "16 (tenc (track 2))");
  CHECK(d.node.diagnostics.empty());
}

TEST_CASE("senc infers the IV size when nothing is known (CNT-6014) and fails when it cannot (CNT-6001)",
          "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  SECTION("8 bytes") {
    const Decoded d = run("senc", full(0, 0, cat({be(2, 4), filled(8, 1), filled(8, 2)})), ctx);
    CHECK(field(d.node, "Per_Sample_IV_Size").svalue == "8 (inferred)");
    CHECK(has_code(d.node, "CNT-6014"));
    CHECK(!has_code(d.node, "CNT-6001"));
  }
  SECTION("16 bytes with subsamples") {
    const Bytes s = senc_sample(filled(16, 5), {{4, 12}}, true);
    const Decoded d = run("senc", full(0, 2, cat({be(2, 4), s, s})), ctx);
    CHECK(field(d.node, "Per_Sample_IV_Size").svalue == "16 (inferred)");
    CHECK(field(d.node, "samples/sample[1]").svalue == "clear 4 + protected 12 = 16 bytes");
  }
  SECTION("0 bytes with subsamples (constant IV)") {
    const Bytes s = senc_sample({}, {{4, 12}, {0, 7}}, true);
    const Decoded d = run("senc", full(0, 2, cat({be(2, 4), s, s})), ctx);
    CHECK(field(d.node, "Per_Sample_IV_Size").svalue == "0 (inferred)");
  }
  SECTION("not inferable") {
    const Decoded d = run("senc", full(0, 0, cat({be(2, 4), filled(11, 1)})), ctx);
    CHECK(has_code(d.node, "CNT-6001"));
    CHECK(!has_field(d.node, "samples"));
  }
  SECTION("no samples need no IV size") {
    const Decoded d = run("senc", full(0, 0, be(0, 4)), ctx);
    CHECK(d.node.diagnostics.empty());
  }
}

TEST_CASE("senc override flag carries its own IV size and KID", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.per_sample_iv_size = 16;  // the override wins
  const Decoded d = run("senc", full(0, 1, cat({be(1, 3), be(8, 1), kKid, be(1, 4), filled(8, 4)})), ctx);
  CHECK(field(d.node, "AlgorithmID").uvalue == 1);
  CHECK(field(d.node, "KID").svalue == kKidText);
  CHECK(field(d.node, "Per_Sample_IV_Size").svalue == "8 (senc override)");
  CHECK(field(d.node, "flags").svalue == "override-track-encryption");
}

TEST_CASE("senc: a truncated payload and trailing bytes", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.per_sample_iv_size = 8;
  const Decoded cut = run("senc", full(0, 0, cat({be(3, 4), filled(8, 1), filled(5, 2)})), ctx);
  CHECK(has_code(cut.node, "CNT-1002"));
  CHECK(field(cut.node, "samples/sample[0]/InitializationVector").bytes == filled(8, 1));
  const Decoded extra = run("senc", full(0, 0, cat({be(1, 4), filled(8, 1), filled(3, 0)})), ctx);
  CHECK(has_code(extra.node, "CNT-1005"));
  const Decoded v1 = run("senc", full(1, 0, be(0, 4)), ctx);
  CHECK(has_code(v1.node, "CNT-1003"));
}

TEST_CASE("senc with many samples summarises the middle of the array", "[container][decoders][cenc]") {
  iso::BoxContext ctx = ctx_in("traf");
  ctx.per_sample_iv_size = 8;
  constexpr uint32_t kCount = 3000;
  Bytes payload = be(kCount, 4);
  for (uint32_t i = 0; i < kCount; ++i) {
    const Bytes iv = be(i, 8);
    payload.insert(payload.end(), iv.begin(), iv.end());
  }
  const Decoded d = run("senc", full(0, 0, payload), ctx);
  CHECK(field(d.node, "samples").uvalue == kCount);
  CHECK(has_field(d.node, "samples/sample[63]"));
  CHECK(!has_field(d.node, "samples/sample[64]"));
  CHECK(has_field(d.node, "samples/sample[2999]"));
  CHECK(d.node.diagnostics.empty());
}

// ---- whole-file walks ----

namespace {

Bytes encrypted_track(uint32_t track_id, const Bytes& tenc_box) {
  const Bytes tkhd = box("tkhd", full(0, 3,
                                      cat({be(0, 4), be(0, 4), be(track_id, 4), be(0, 4), be(0, 4), filled(8, 0),
                                           be(0, 4), be(0, 4), filled(36, 0), be(0, 4), be(0, 4)})));
  const Bytes sinf =
      box("sinf", cat({box("frma", str("avc1")), box("schm", full(0, 0, cat({str("cenc"), be(0x00010000, 4)}))),
                       box("schi", tenc_box)}));
  const Bytes encv = box("encv", cat({filled(78, 0), sinf}));
  const Bytes stsd = box("stsd", full(0, 0, cat({be(1, 4), encv})));
  return box("trak", cat({tkhd, box("mdia", box("minf", box("stbl", stsd)))}));
}

Bytes fragment(uint32_t track_id, const Bytes& senc_box) {
  const Bytes tfhd = box("tfhd", full(0, 0x20000, be(track_id, 4)));
  return box("moof", cat({box("mfhd", full(0, 0, be(1, 4))), box("traf", cat({tfhd, senc_box}))}));
}

const BoxNodeT* find_box(const std::vector<std::unique_ptr<BoxNodeT>>& boxes, std::string_view type) {
  for (const auto& b : boxes) {
    if (b->fourcc == type) {
      return b.get();
    }
    if (const BoxNodeT* in = find_box(b->children, type); in != nullptr) {
      return in;
    }
  }
  return nullptr;
}

const Bytes kFtyp = box("ftyp", cat({str("isom"), be(0, 4), str("isom"), str("cmfc")}));

}  // namespace

TEST_CASE("the walk carries the tenc IV size from the moov to the senc of a moof", "[container][decoders][cenc]") {
  const Bytes sample_a = senc_sample(filled(8, 1), {{16, 100}}, true);
  const Bytes senc_box = box("senc", full(0, 2, cat({be(2, 4), sample_a, sample_a})));
  const Bytes file = cat({kFtyp, box("moov", encrypted_track(3, box("tenc", tenc_v0(1, 8)))), fragment(3, senc_box)});
  const ovt::io::MemoryFile mem(file);
  const iso::WalkResult w = iso::walk(mem);
  const BoxNodeT* tenc = find_box(w.top_level, "tenc");
  REQUIRE(tenc != nullptr);
  CHECK(field(*tenc, "default_KID").svalue == kKidText);
  const BoxNodeT* senc = find_box(w.top_level, "senc");
  REQUIRE(senc != nullptr);
  CHECK(field(*senc, "Per_Sample_IV_Size").svalue == "8 (tenc (track 3))");
  CHECK(!has_code(*senc, "CNT-6001"));
  CHECK(!has_code(*senc, "CNT-6014"));
  CHECK(field(*senc, "subsample_totals").svalue == "2 subsamples, clear 32 bytes, protected 200 bytes");
  const BoxNodeT* frma = find_box(w.top_level, "frma");
  REQUIRE(frma != nullptr);
  CHECK(field(*frma, "data_format").svalue == "avc1");
}

TEST_CASE("senc of a cbcs fragment (constant IV) has only subsample pairs whose sums match the trun sizes",
          "[container][decoders][cenc]") {
  const Bytes s0 = senc_sample({}, {{32, 160}}, true);
  const Bytes s1 = senc_sample({}, {{40, 80}, {0, 80}}, true);
  const Bytes senc_box = box("senc", full(0, 2, cat({be(2, 4), s0, s1})));
  const Bytes trun = box("trun", full(0, 0x200, cat({be(2, 4), be(192, 4), be(200, 4)})));
  const Bytes tfhd = box("tfhd", full(0, 0x20000, be(1, 4)));
  const Bytes moof = box("moof", cat({box("mfhd", full(0, 0, be(1, 4))), box("traf", cat({tfhd, trun, senc_box}))}));
  const Bytes tenc = box("tenc", tenc_v1_cbcs(1, 9, filled(16, 0x5A)));
  const Bytes file = cat({kFtyp, box("moov", encrypted_track(1, tenc)), moof});
  const ovt::io::MemoryFile mem(file);
  const iso::WalkResult w = iso::walk(mem);
  const BoxNodeT* senc = find_box(w.top_level, "senc");
  REQUIRE(senc != nullptr);
  CHECK(field(*senc, "Per_Sample_IV_Size").svalue == "0 (tenc (track 1))");
  CHECK(!has_field(*senc, "samples/sample[0]/InitializationVector"));
  const BoxNodeT* trun_node = find_box(w.top_level, "trun");
  REQUIRE(trun_node != nullptr);
  const auto size_of = [&](int i) {
    return field(*trun_node, "entries/entry[" + std::to_string(i) + "]/sample_size").uvalue;
  };
  const auto sum_of = [&](int i) {
    const FieldT& s = field(*senc, "samples/sample[" + std::to_string(i) + "]");
    return s.svalue;
  };
  CHECK(size_of(0) == 192);
  CHECK(size_of(1) == 200);
  CHECK(sum_of(0) == "clear 32 + protected 160 = 192 bytes");
  CHECK(sum_of(1) == "clear 40 + protected 160 = 200 bytes");
}

TEST_CASE("a fragment-only file without init segment infers the senc IV size (CNT-6014) or raises CNT-6001",
          "[container][decoders][cenc]") {
  SECTION("inferred") {
    const Bytes senc_box = box("senc", full(0, 0, cat({be(2, 4), filled(8, 1), filled(8, 2)})));
    const Bytes file = cat({box("styp", cat({str("cmfs"), be(0, 4), str("cmfc")})), fragment(1, senc_box)});
    const ovt::io::MemoryFile mem(file);
    const iso::WalkResult w = iso::walk(mem);
    const BoxNodeT* senc = find_box(w.top_level, "senc");
    REQUIRE(senc != nullptr);
    CHECK(has_code(*senc, "CNT-6014"));
    CHECK(field(*senc, "Per_Sample_IV_Size").svalue == "8 (inferred)");
  }
  SECTION("not inferable") {
    const Bytes senc_box = box("senc", full(0, 0, cat({be(2, 4), filled(13, 1)})));
    const Bytes file = cat({box("styp", cat({str("cmfs"), be(0, 4), str("cmfc")})), fragment(1, senc_box)});
    const ovt::io::MemoryFile mem(file);
    const iso::WalkResult w = iso::walk(mem);
    const BoxNodeT* senc = find_box(w.top_level, "senc");
    REQUIRE(senc != nullptr);
    CHECK(has_code(*senc, "CNT-6001"));
  }
}

namespace {

void collect_boxes(const std::vector<std::unique_ptr<BoxNodeT>>& boxes, std::string_view type,
                   std::vector<const BoxNodeT*>& out) {
  for (const auto& b : boxes) {
    if (b->fourcc == type) {
      out.push_back(b.get());
    }
    collect_boxes(b->children, type, out);
  }
}

std::vector<const BoxNodeT*> all_boxes(const iso::WalkResult& w, std::string_view type) {
  std::vector<const BoxNodeT*> out;
  collect_boxes(w.top_level, type, out);
  return out;
}

void check_no_errors(const iso::WalkResult& w, std::initializer_list<std::string_view> types) {
  for (const std::string_view type : types) {
    for (const BoxNodeT* b : all_boxes(w, type)) {
      for (const auto& d : b->diagnostics) {
        INFO(std::string(type) + ": " + d->code + " " + d->message);
        CHECK(d->severity != ovt::fb::Severity::Error);
      }
    }
  }
}

}  // namespace

TEST_CASE("mpegff_video_cenc_128iv: cenc 1.0 scheme, tenc and a 16-byte-IV senc",
          "[container][decoders][cenc][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("mpegff_video_cenc_128iv");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult w = iso::walk(*file);
  const auto schm = all_boxes(w, "schm");
  REQUIRE(!schm.empty());
  CHECK(field(*schm[0], "scheme_type").svalue == "cenc");
  CHECK(field(*schm[0], "scheme_type.derived").svalue == "AES-CTR");
  CHECK(field(*schm[0], "scheme_version").svalue == "1.0");
  CHECK(!all_boxes(w, "tenc").empty());
  const auto senc = all_boxes(w, "senc");
  REQUIRE(!senc.empty());
  CHECK(field(*senc[0], "Per_Sample_IV_Size").svalue.starts_with("16 ("));
  CHECK(!has_code(*senc[0], "CNT-6001"));
  check_no_errors(w, {"schm", "tenc", "senc"});
}

TEST_CASE("gpac_enc_cenc: a piff 1.1 and a cenc 1.0 scheme, tenc under the cenc one",
          "[container][decoders][cenc][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("gpac_enc_cenc");
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  const iso::WalkResult w = iso::walk(*file);
  const auto schm = all_boxes(w, "schm");
  REQUIRE(schm.size() == 2);
  CHECK(field(*schm[0], "scheme_type").svalue == "piff");
  CHECK(field(*schm[0], "scheme_version").svalue == "1.1");
  CHECK(field(*schm[1], "scheme_type").svalue == "cenc");
  CHECK(field(*schm[1], "scheme_type.derived").svalue == "AES-CTR");
  CHECK(field(*schm[1], "scheme_version").svalue == "1.0");
  const auto tenc = all_boxes(w, "tenc");
  REQUIRE(tenc.size() == 1);
  CHECK(field(*tenc[0], "default_KID").bytes.size() == 16);
  check_no_errors(w, {"schm", "tenc", "senc"});
}
