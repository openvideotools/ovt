// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group B hand decoders (chapter 08 §8.3.2, WI-1.206): `stsd`, the visual / audio / text / metadata /
// timecode sample entries, rule R-AUDIO, the children offset a ContainerWithPrefix decoder returns (§8.2.9), CNT-1010,
// and the comparison with the MP4Box goldens of the container-smoke corpus.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "container/isobmff/decoders/sample_entries.h"
#include "container/isobmff/field_builder.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "test_support/corpus.h"
#include "test_support/golden_compare.h"

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
Bytes fixed16(double v) {
  return be(static_cast<uint32_t>(v * 65536.0), 4);
}

// SampleEntry prefix: reserved u8[6], data_reference_index u16.
Bytes prefix(uint16_t dri = 1) {
  return cat({zeros(6), be(dri, 2)});
}

Bytes compressor(std::string_view name) {
  Bytes b = cat({be(name.size(), 1), str(name)});
  b.resize(32, 0);
  return b;
}

Bytes visual_fixed(uint16_t width, uint16_t height, uint16_t depth, std::string_view name = "") {
  return cat({prefix(), zeros(2 + 2 + 12), be(width, 2), be(height, 2), fixed16(72), fixed16(72), zeros(4), be(1, 2),
              compressor(name), be(depth, 2), be(0xFFFF, 2)});
}

// ---- running one decoder the way the walker does (§8.2.3 steps 2.11-2.12) ----

constexpr auto kFullBoxes = std::to_array<std::string_view>({"stsd", "txtC", "uri ", "uriI", "hdlr"});
bool is_fullbox(std::string_view cc) {
  return std::ranges::find(kFullBoxes, cc) != kFullBoxes.end();
}

iso::FourCC cc_of(std::string_view s) {
  iso::FourCC cc{};
  std::copy_n(s.begin(), 4, cc.begin());
  return cc;
}

iso::BoxContext ctx_of(std::initializer_list<std::string_view> chain, std::string_view handler = "") {
  iso::BoxContext c;
  for (const std::string_view s : chain) {
    c.ancestors.push_back(cc_of(s));
  }
  if (!handler.empty()) {
    c.handler_type = cc_of(handler);
  }
  return c;
}

iso::BoxContext entry_ctx(std::string_view handler) {
  return ctx_of({"moov", "trak", "mdia", "minf", "stbl", "stsd"}, handler);
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
  size_t payload_bytes = 0;
};

Decoded run(std::string_view type, const Bytes& payload, iso::BoxContext& ctx) {
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

bool has_field(const BoxNodeT& n, std::string_view name) {
  return child(n.fields, name) != nullptr;
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

TEST_CASE("every group B row with fields has a registered decoder", "[container][decoders][stsd]") {
  const std::array<std::pair<std::string_view, std::string_view>, 18> rows = {{
      {"stsd", "stbl"},
      {"avc1", "stsd"},
      {"mp4a", "stsd"},
      {"zzzz", "stsd"},
      {"tx3g", "stsd"},
      {"wvtt", "stsd"},
      {"stpp", "stsd"},
      {"mett", "stsd"},
      {"metx", "stsd"},
      {"urim", "stsd"},
      {"tmcd", "stsd"},
      {"ftab", "tx3g"},
      {"vttC", "wvtt"},
      {"vlab", "wvtt"},
      {"txtC", "mett"},
      {"uri ", "urim"},
      {"uriI", "urim"},
      {"name", "tmcd"},
  }};
  for (const auto& [cc, parent] : rows) {
    INFO(cc);
    iso::BoxContext c = entry_ctx("text");
    c.ancestors.resize(parent == "stbl" ? 5 : 6);
    if (parent != "stbl" && parent != "stsd") {
      c.ancestors.push_back(cc_of(parent));
    }
    const iso::RegistryEntry* e = iso::lookup(cc, c);
    REQUIRE(e != nullptr);
    CHECK(e->decoder != nullptr);
  }
}

TEST_CASE("stsd: entry_count, the child offset and the stsd version in the context", "[container][decoders][stsd]") {
  iso::BoxContext ctx = ctx_of({"moov", "trak", "mdia", "minf", "stbl"});
  Decoded d = run("stsd", cat({be(0, 4), be(2, 4), zeros(40)}), ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "entry_count").uvalue == 2);
  CHECK(d.outcome.children_offset == 4);  // counted from after version/flags (§8.2.9)
  CHECK(ctx.stsd_version == 0);
  CHECK(field(d.node, "entry_count").spec_ref == "ISO/IEC 14496-12:2022 §8.5.2");

  Decoded v1 = run("stsd", cat({be(0x01000000, 4), be(1, 4)}), ctx);
  CHECK(ctx.stsd_version == 1);
  CHECK(v1.outcome.children_offset == 4);
}

TEST_CASE("stsd of an unknown version is not decoded and has no children", "[container][decoders][stsd]") {
  iso::BoxContext ctx = ctx_of({"moov", "trak", "mdia", "minf", "stbl"});
  const Decoded d = run("stsd", cat({be(0x02000000, 4), be(1, 4), zeros(12)}), ctx);
  CHECK(has_code(d.node, "CNT-1003"));
  CHECK_FALSE(has_field(d.node, "entry_count"));
  CHECK(d.outcome.children_offset == 16);  // the whole payload after version/flags: nothing left to walk
}

TEST_CASE("check_stsd_entry_count: CNT-1010 when entry_count differs from the entries found",
          "[container][decoders][stsd]") {
  const auto stsd_with = [](uint64_t count, size_t entries) {
    BoxNodeT n;
    n.fourcc = "stsd";
    add_uint(n, "entry_count", count, 96, 32);
    for (size_t i = 0; i < entries; ++i) {
      auto c = std::make_unique<BoxNodeT>();
      c->fourcc = "avc1";
      n.children.push_back(std::move(c));
    }
    auto trailing = std::make_unique<BoxNodeT>();  // the walker's "trailing bytes" node is not an entry
    n.children.push_back(std::move(trailing));
    return n;
  };
  BoxNodeT ok = stsd_with(2, 2);
  iso::check_stsd_entry_count(ok);
  CHECK(ok.diagnostics.empty());
  BoxNodeT bad = stsd_with(3, 1);
  iso::check_stsd_entry_count(bad);
  REQUIRE(bad.diagnostics.size() == 1);
  CHECK(bad.diagnostics[0]->code == "CNT-1010");
  BoxNodeT undecoded;
  undecoded.fourcc = "stsd";
  iso::check_stsd_entry_count(undecoded);
  CHECK(undecoded.diagnostics.empty());
}

TEST_CASE("VisualSampleEntry: size, depth, resolution, compressorname and the children offset",
          "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("vide");
  const Bytes payload = visual_fixed(1920, 1080, 24, "HEVC Coding");
  Decoded d = run("hvc1", payload, ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "data_reference_index").uvalue == 1);
  CHECK(field(d.node, "width").uvalue == 1920);
  CHECK(field(d.node, "height").uvalue == 1080);
  CHECK(field(d.node, "depth").uvalue == 24);
  CHECK(field(d.node, "depth").svalue == "24 colour");
  CHECK(field(d.node, "horizresolution").ivalue == 0x480000);
  CHECK(field(d.node, "horizresolution").svalue == "72.0");
  CHECK(field(d.node, "compressorname").svalue == "HEVC Coding");
  CHECK(field(d.node, "codec.derived").svalue == "H.265");
  CHECK(d.outcome.children_offset == payload.size());
  CHECK(ctx.sample_entry == cc_of("hvc1"));
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);
}

TEST_CASE("VisualSampleEntry compressorname is a counted string of 32 bytes", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("vide");
  Decoded d = run("avc1", visual_fixed(16, 16, 24, ""), ctx);
  CHECK(field(d.node, "compressorname").svalue.empty());
  CHECK(field(d.node, "compressorname").bytes.size() == 32);
  CHECK(field(d.node, "compressorname").span->bit_length() == 256);
  Bytes bad = visual_fixed(16, 16, 24, "x264");
  bad[8 + 2 + 2 + 12 + 2 + 2 + 4 + 4 + 4 + 2] = 200;  // a length above 31
  d = run("avc1", bad, ctx);
  CHECK(has_code(d.node, "CNT-1007"));
  CHECK(field(d.node, "compressorname").svalue.size() == 31);
}

TEST_CASE("garbage in pre_defined is CNT-1007 Info, never a failure; QuickTime files are not checked",
          "[container][decoders][stsd]") {
  Bytes garbage = visual_fixed(320, 240, 24);
  garbage[8] = 0x12;
  garbage[9] = 0x34;
  iso::BoxContext ctx = entry_ctx("vide");
  const Decoded d = run("avc1", garbage, ctx);
  CHECK(has_code(d.node, "CNT-1007"));
  CHECK_FALSE(d.outcome.truncated);
  CHECK(field(d.node, "width").uvalue == 320);
  iso::BoxContext qt = entry_ctx("vide");
  qt.quicktime = true;
  CHECK(run("avc1", garbage, qt).node.diagnostics.empty());
}

TEST_CASE("a visual entry cut short keeps the fields before the cut", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("vide");
  Bytes payload = visual_fixed(640, 480, 24);
  payload.resize(8 + 24 + 2);  // width, height and part of the resolution are there
  const Decoded d = run("avc1", payload, ctx);
  CHECK(d.outcome.truncated);
  CHECK(has_code(d.node, "CNT-1002"));
  CHECK(field(d.node, "width").uvalue == 640);
}

Bytes iso_audio(uint16_t channels, uint16_t samplesize, uint32_t rate) {
  return cat(
      {prefix(), zeros(8), be(channels, 2), be(samplesize, 2), zeros(4), be(static_cast<uint64_t>(rate) << 16U, 4)});
}

Bytes qt_audio_v1() {
  return cat({prefix(), be(1, 2), be(0, 2), str("appl"), be(2, 2), be(16, 2), be(0xFFFE, 2), be(0, 2), fixed16(44100),
              be(1024, 4), be(2, 4), be(4, 4), be(2, 4)});
}

Bytes wave_with_esds() {
  return box("wave", cat({box("frma", str("mp4a")), box("mp4a", zeros(12)), box("esds", cat({zeros(4), zeros(10)}))}));
}

TEST_CASE("AudioSampleEntry ISO v0: channels, sample size and rate", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("soun");
  const Bytes payload = iso_audio(2, 16, 48000);
  const Decoded d = run("mp4a", payload, ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "channelcount").uvalue == 2);
  CHECK(field(d.node, "channelcount").svalue == "stereo");
  CHECK(field(d.node, "samplesize").uvalue == 16);
  CHECK(field(d.node, "samplerate").ivalue == int64_t{48000} * 65536);
  CHECK(field(d.node, "samplerate").svalue == "48000 Hz");
  CHECK(field(d.node, "codec.derived").svalue == "MPEG-4 Audio");
  CHECK(d.outcome.children_offset == payload.size());
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);
}

TEST_CASE("AudioSampleEntry ISO V1 is chosen by the stsd version", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("soun");
  ctx.stsd_version = 1;
  const Bytes payload =
      cat({prefix(), be(1, 2), zeros(6), be(6, 2), be(24, 2), zeros(4), be(uint64_t{48000} << 16U, 4)});
  const Decoded d = run("Opus", payload, ctx);
  CHECK(field(d.node, "entry_version").uvalue == 1);
  CHECK(field(d.node, "channelcount").svalue == "6 channels");
  CHECK(field(d.node, "samplerate").svalue == "48000 Hz");
  CHECK(d.outcome.children_offset == payload.size());
  check_contiguous(d);
}

TEST_CASE("AudioSampleEntry QuickTime v1: packet fields are present and the wave child follows",
          "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("soun");
  ctx.quicktime = true;
  const Bytes payload = qt_audio_v1();
  const Decoded d = run("mp4a", payload, ctx);
  CHECK(field(d.node, "version").uvalue == 1);
  CHECK(field(d.node, "vendor").svalue == "appl");
  CHECK(field(d.node, "channels").uvalue == 2);
  CHECK(field(d.node, "compression_id").ivalue == -2);
  CHECK(field(d.node, "sample_rate").svalue == "44100 Hz");
  CHECK(field(d.node, "samples_per_packet").uvalue == 1024);
  CHECK(field(d.node, "bytes_per_packet").uvalue == 2);
  CHECK(field(d.node, "bytes_per_frame").uvalue == 4);
  CHECK(field(d.node, "bytes_per_sample").uvalue == 2);
  CHECK(d.outcome.children_offset == payload.size());
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);
}

TEST_CASE("AudioSampleEntry QuickTime v2 reads the 64-bit float sample rate", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("soun");
  ctx.quicktime = true;
  const Bytes payload = cat({prefix(), be(2, 2), be(0, 2), str("appl"), be(3, 2), be(16, 2), be(0xFFFE, 2), be(0, 2),
                             be(65536, 4), be(72, 4), be(std::bit_cast<uint64_t>(48000.0), 8), be(2, 4),
                             be(0x7F000000, 4), be(16, 4), be(0, 4), be(4, 4), be(1, 4)});
  const Decoded d = run("lpcm", payload, ctx);
  CHECK(field(d.node, "version").uvalue == 2);
  CHECK(field(d.node, "numAudioChannels").uvalue == 2);
  CHECK(field(d.node, "always7F000000").uvalue == 0x7F000000);
  CHECK(field(d.node, "audioSampleRate").svalue == "48000 Hz");
  CHECK(field(d.node, "constLPCMFramesPerAudioPacket").uvalue == 1);
  CHECK(d.outcome.children_offset == payload.size());
  check_contiguous(d);
}

TEST_CASE("R-AUDIO: a version 1 layout without QuickTime context is QuickTime only when a wave child follows",
          "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("soun");  // no ftyp qt, not ctx.quicktime
  const Decoded with_wave = run("mp4a", cat({qt_audio_v1(), wave_with_esds()}), ctx);
  CHECK(has_field(with_wave.node, "samples_per_packet"));
  CHECK(with_wave.outcome.children_offset == qt_audio_v1().size());
  const Decoded without = run("mp4a", cat({qt_audio_v1(), box("esds", zeros(14))}), ctx);
  CHECK_FALSE(has_field(without.node, "samples_per_packet"));
  CHECK(has_field(without.node, "channelcount"));
  CHECK(has_code(without.node, "CNT-1007"));  // the QuickTime bytes read as reserved in the ISO v0 layout
  iso::BoxContext qt = entry_ctx("soun");     // QuickTime context, version 0: the ISO v0 layout, no reserved noise
  qt.quicktime = true;
  const Decoded v0 =
      run("twos", cat({prefix(), zeros(2), be(0, 2), str("appl"), be(2, 2), be(16, 2), zeros(4), fixed16(44100)}), qt);
  CHECK(field(v0.node, "channelcount").uvalue == 2);
  CHECK(v0.node.diagnostics.empty());
}

TEST_CASE("an unknown entry fourcc is decoded as the layout of the track's handler", "[container][decoders][stsd]") {
  iso::BoxContext vide = entry_ctx("vide");
  CHECK(has_field(run("zzzz", visual_fixed(8, 8, 24), vide).node, "width"));
  iso::BoxContext soun = entry_ctx("soun");
  const Decoded a = run("zzzz", iso_audio(1, 8, 8000), soun);
  CHECK(field(a.node, "channelcount").svalue == "mono");
  CHECK_FALSE(has_field(a.node, "codec.derived"));
  iso::BoxContext none = entry_ctx("");  // no handler: the codec tables choose, anything else is the prefix alone
  CHECK(has_field(run("avc1", visual_fixed(8, 8, 24), none).node, "width"));
  CHECK(has_field(run("mp4a", iso_audio(2, 16, 44100), none).node, "channelcount"));
  CHECK(run("mp4s", cat({prefix(), zeros(4)}), none).outcome.children_offset == 8);
}

TEST_CASE("tx3g: display flags, records and the ftab font list", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("text");
  const Bytes payload =
      cat({prefix(), be(0x00020060, 4), be(1, 1), be(0xFF, 1), be(0x00000000, 4), be(0, 2), be(0, 2), be(40, 2),
           be(320, 2), be(0, 2), be(0, 2), be(1, 2), be(0, 1), be(18, 1), be(0xFFFFFFFF, 4)});
  const Decoded d = run("tx3g", payload, ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "displayFlags").uvalue == 0x00020060);
  CHECK(field(d.node, "displayFlags").svalue == "scroll_in|scroll_out|write_text_vertically");
  CHECK(field(d.node, "horizontal_justification").uvalue == 1);
  CHECK(field(d.node, "BoxRecord/bottom").ivalue == 40);
  CHECK(field(d.node, "BoxRecord/right").ivalue == 320);
  CHECK(field(d.node, "StyleRecord/font_ID").uvalue == 1);
  CHECK(field(d.node, "StyleRecord/font_size").uvalue == 18);
  CHECK(field(d.node, "StyleRecord/text_color_rgba/a").uvalue == 255);
  CHECK(d.outcome.children_offset == payload.size());
  check_contiguous(d);

  iso::BoxContext tctx = entry_ctx("text");
  tctx.ancestors.push_back(cc_of("tx3g"));
  const Decoded f =
      run("ftab", cat({be(2, 2), be(1, 2), be(5, 1), str("Serif"), be(2, 2), be(9, 1), str("Monospace")}), tctx);
  REQUIRE(f.found);
  CHECK(field(f.node, "entry_count").uvalue == 2);
  CHECK(field(f.node, "entries/entry[0]/font").svalue == "Serif");
  CHECK(field(f.node, "entries/entry[1]/font_ID").uvalue == 2);
  CHECK(field(f.node, "entries/entry[1]/font").svalue == "Monospace");
  CHECK(f.node.diagnostics.empty());
  check_contiguous(f);
}

TEST_CASE("wvtt has the prefix only; vttC and vlab are strings", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("text");
  const Decoded w = run("wvtt", prefix(), ctx);
  CHECK(w.outcome.children_offset == 8);
  iso::BoxContext c = entry_ctx("text");
  c.ancestors.push_back(cc_of("wvtt"));
  CHECK(field(run("vttC", str("WEBVTT"), c).node, "config").svalue == "WEBVTT");
  CHECK(field(run("vlab", str("English"), c).node, "source_label").svalue == "English");
}

TEST_CASE("stpp and mett strings, then the children offset", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("subt");
  const Bytes stpp = cat(
      {prefix(), str("http://www.w3.org/ns/ttml"), be(0, 1), str("schema.xsd"), be(0, 1), str("image/png"), be(0, 1)});
  const Decoded s = run("stpp", stpp, ctx);
  CHECK(field(s.node, "namespace").svalue == "http://www.w3.org/ns/ttml");
  CHECK(field(s.node, "schema_location").svalue == "schema.xsd");
  CHECK(field(s.node, "auxiliary_mime_types").svalue == "image/png");
  CHECK(s.outcome.children_offset == stpp.size());
  check_contiguous(s);

  iso::BoxContext mctx = entry_ctx("meta");
  const Bytes mett = cat({prefix(), be(0, 1), str("text/plain"), be(0, 1), box("btrt", zeros(12))});
  const Decoded m = run("mett", mett, mctx);
  CHECK(field(m.node, "content_encoding").svalue.empty());
  CHECK(field(m.node, "mime_format").svalue == "text/plain");
  CHECK(m.outcome.children_offset == 8 + 1 + 11);  // the btrt child starts after the strings
}

TEST_CASE("urim and its uri / uriI children, txtC", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("meta");
  CHECK(run("urim", prefix(), ctx).outcome.children_offset == 8);
  iso::BoxContext u = entry_ctx("meta");
  u.ancestors.push_back(cc_of("urim"));
  const Decoded uri = run("uri ", cat({zeros(4), str("urn:mpeg:dash:event:2012"), be(0, 1)}), u);
  CHECK(field(uri.node, "theURI").svalue == "urn:mpeg:dash:event:2012");
  const Decoded ini = run("uriI", cat({zeros(4), Bytes{1, 2, 3}}), u);
  CHECK(field(ini.node, "uri_initialization_data").bytes == Bytes{1, 2, 3});
  iso::BoxContext t = entry_ctx("meta");
  t.ancestors.push_back(cc_of("mett"));
  CHECK(field(run("txtC", cat({zeros(4), str("cfg"), be(0, 1)}), t).node, "text_config").svalue == "cfg");
}

TEST_CASE("tmcd: flags, frame rate and the name child", "[container][decoders][stsd]") {
  iso::BoxContext ctx = entry_ctx("tmcd");
  const Bytes payload = cat({prefix(), be(0, 4), be(0x3, 4), be(30000, 4), be(1001, 4), be(30, 1), be(0, 1)});
  const Decoded d = run("tmcd", payload, ctx);
  REQUIRE(d.found);
  CHECK(field(d.node, "flags").svalue == "drop_frame|24_hour_max");
  CHECK(field(d.node, "time_scale").uvalue == 30000);
  CHECK(field(d.node, "frame_duration").uvalue == 1001);
  CHECK(field(d.node, "frame_rate.derived").svalue == "29.97 fps");
  CHECK(field(d.node, "number_of_frames").uvalue == 30);
  CHECK(d.outcome.children_offset == payload.size());
  check_contiguous(d);

  iso::BoxContext n = entry_ctx("tmcd");
  n.ancestors.push_back(cc_of("tmcd"));
  const Decoded name = run("name", cat({be(7, 2), be(0, 2), str("Tape 01")}), n);
  CHECK(field(name.node, "text_size").uvalue == 7);
  CHECK(field(name.node, "text").svalue == "Tape 01");
  check_contiguous(name);
}

// The walker still treats `stsd` as a leaf until WI-1.203 wires the registry in: walk the children here.

struct Harness {
  const ovt::io::RandomAccessFile* file = nullptr;
  std::map<std::string, nlohmann::json> by_path;  // decoded boxes only

  void decode(BoxNodeT& n, iso::BoxContext& ctx, uint64_t payload_begin, uint64_t end) const {
    const iso::RegistryEntry* e = iso::lookup(n.fourcc, ctx);
    if (e == nullptr || e->decoder == nullptr || !n.fields.empty() || end <= payload_begin) {
      return;
    }
    Bytes payload(end - payload_begin);
    REQUIRE(file->read(payload_begin, payload.size(), payload.data()) == payload.size());
    size_t skip = 0;
    if (is_fullbox(n.fourcc) && payload.size() >= 4) {
      n.version = payload[0];
      n.flags = static_cast<int32_t>((uint32_t{payload[1]} << 16U) | (uint32_t{payload[2]} << 8U) | payload[3]);
      add_uint(n, "version", payload[0], 64, 8);
      add_uint(n, "flags", static_cast<uint32_t>(n.flags), 72, 24);
      skip = 4;
    }
    ovt::BitReader br(std::span<const uint8_t>(payload).subspan(skip));
    iso::FieldBuilder fb(n, n.file_span->offset(), br, static_cast<uint64_t>(n.header_length) * 8U + 8U * skip);
    const iso::DecodeOutcome out = iso::run_decoder(*e->decoder, ctx, br, fb);
    const bool prefixed = n.fourcc == "stsd" || ctx.parent_is("stsd");
    if (prefixed && n.children.empty() && out.children_offset > 0 && !out.truncated) {
      iso::WalkState st;
      st.file = file;
      n.children = iso::walk_children(st, n.file_span->offset(), payload_begin + skip + out.children_offset, end, 1,
                                      ctx.push(cc_of(n.fourcc), &n), n.diagnostics);
    }
    if (n.fourcc == "stsd") {
      iso::check_stsd_entry_count(n);
    }
  }

  void visit(std::vector<std::unique_ptr<BoxNodeT>>& boxes, iso::BoxContext ctx, const std::string& parent) {
    std::map<std::string, int> total;
    for (const auto& b : boxes) {
      ++total[b->fourcc];
    }
    std::map<std::string, int> seen;
    for (auto& b : boxes) {
      std::string path = parent.empty() ? b->fourcc : parent + "/" + b->fourcc;
      if (total[b->fourcc] > 1) {
        path += "[" + std::to_string(++seen[b->fourcc]) + "]";
      }
      const uint64_t begin = b->file_span->offset() + b->header_length;
      const uint64_t end = b->file_span->offset() + b->file_span->length();
      decode(*b, ctx, begin, end);
      if (!b->fields.empty()) {
        by_path[path] = json_of_children(b->fields);
      }
      visit(b->children, ctx.push(cc_of(b->fourcc), b.get()), path);
    }
  }

  // The scalar fields as the MP4Box goldens print them; groups (arrays, records) are not in these goldens.
  static nlohmann::json json_of_children(const std::vector<std::unique_ptr<FieldT>>& fields) {
    nlohmann::json o = nlohmann::json::object();
    for (const auto& f : fields) {
      if (!f->children.empty()) {
        continue;
      }
      const bool is_int = f->kind == SyntaxValueKind::Int || f->kind == SyntaxValueKind::Fixed;
      o[f->name] = f->kind == SyntaxValueKind::String ? nlohmann::json(f->svalue)
                   : is_int                           ? nlohmann::json(f->ivalue)
                                                      : nlohmann::json(f->uvalue);
    }
    return o;
  }
};

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

Bytes hdlr_box(std::string_view handler) {
  return box("hdlr", cat({zeros(4), zeros(4), str(handler), zeros(12), be(0, 1)}));
}

Bytes file_with_stsd(std::string_view handler, const Bytes& stsd_payload, std::string_view brand = "isom") {
  const Bytes stsd = box("stsd", stsd_payload);
  const Bytes moov = box("moov", box("trak", box("mdia", cat({hdlr_box(handler), box("minf", box("stbl", stsd))}))));
  return cat({box("ftyp", cat({str(brand), zeros(4), str(brand)})), moov});
}

std::vector<std::string> child_types(const BoxNodeT& n) {
  std::vector<std::string> v;
  v.reserve(n.children.size());
  for (const auto& c : n.children) {
    v.push_back(c->fourcc);
  }
  return v;
}

TEST_CASE("a hvc1 entry is walked: size, depth and the hvcC, colr, pasp and btrt children",
          "[container][decoders][stsd]") {
  const Bytes entry =
      box("hvc1", cat({visual_fixed(1920, 1080, 24), box("hvcC", zeros(23)), box("colr", cat({str("nclx"), zeros(7)})),
                       box("pasp", zeros(8)), box("btrt", zeros(12))}));
  const Bytes file = file_with_stsd("vide", cat({be(0, 4), be(1, 4), entry}));
  const ovt::io::MemoryFile mem(file);
  iso::WalkResult walked = iso::walk(mem);
  Harness h;
  h.file = &mem;
  h.visit(walked.top_level, walked.root_ctx, "");
  const BoxNodeT* hvc1 = find_box(walked.top_level, "hvc1");
  REQUIRE(hvc1 != nullptr);
  CHECK(child_types(*hvc1) == std::vector<std::string>{"hvcC", "colr", "pasp", "btrt"});
  const auto& f = h.by_path.at("moov/trak/mdia/minf/stbl/stsd/hvc1");
  CHECK(f.at("width") == 1920);
  CHECK(f.at("height") == 1080);
  CHECK(f.at("depth") == 24);
  CHECK(find_box(walked.top_level, "stsd")->diagnostics.empty());
}

TEST_CASE("a QuickTime mp4a v1 entry in a .mov: samples_per_packet and esds under wave",
          "[container][decoders][stsd]") {
  const Bytes entry = box("mp4a", cat({qt_audio_v1(), wave_with_esds()}));
  const Bytes file = file_with_stsd("soun", cat({be(0, 4), be(1, 4), entry}), "qt  ");
  const ovt::io::MemoryFile mem(file);
  iso::WalkResult walked = iso::walk(mem);
  Harness h;
  h.file = &mem;
  h.visit(walked.top_level, walked.root_ctx, "");
  const auto& f = h.by_path.at("moov/trak/mdia/minf/stbl/stsd/mp4a");
  CHECK(f.at("samples_per_packet") == 1024);
  CHECK(f.at("bytes_per_frame") == 4);
  const BoxNodeT* wave = find_box(walked.top_level, "wave");
  REQUIRE(wave != nullptr);
  CHECK(find_box(wave->children, "esds") != nullptr);
}

// ---- golden comparison against MP4Box on the container-smoke corpus ----

const std::set<std::string, std::less<>> kGroupB = {"stsd", "avc1", "hvc1", "hev1", "mp4a",
                                                    "mp4v", "mp4s", "encv", "enca", "tx3g"};

std::string last_type(const std::string& path) {
  std::string last = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
  return last.substr(0, last.find('['));
}

size_t compare_with_golden(std::string_view id) {
  const auto path = ovt::test::corpus_or_skip(id);
  const auto file = ovt::io::RandomAccessFile::open(path.string());
  iso::WalkResult walked = iso::walk(*file);
  Harness h;
  h.file = file.get();
  h.visit(walked.top_level, walked.root_ctx, "");
  std::ifstream gf(std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests/golden/isobmff" /
                   (std::string(id) + ".mp4box-diso.json"));
  REQUIRE(gf.good());
  const auto golden = nlohmann::json::parse(gf);
  size_t compared = 0;
  for (const auto& b : golden) {
    const std::string p = b.at("path").get<std::string>();
    if (!kGroupB.contains(last_type(p)) || !b.contains("fields")) {
      continue;
    }
    INFO(std::string(id) + " " + p);
    REQUIRE(h.by_path.contains(p));
    const auto diffs = ovt::test::compare_canonical(b.at("fields"), h.by_path.at(p));
    for (const auto& d : diffs) {
      UNSCOPED_INFO(d.path + ": " + d.message);
    }
    CHECK(diffs.empty());
    ++compared;
  }
  return compared;
}

TEST_CASE("sample entries match the MP4Box goldens of the container-smoke corpus",
          "[container][decoders][stsd][golden][needs-corpus]") {
  for (const std::string_view id :
       {"mpegff_hvc1_only", "mpegff_hev1_clg1_header", "mpegff_a1_foreman_qcif", "mpegff_hevc_dual_track",
        "mpegff_video_cenc_128iv", "ytugc_clip01", "gpac_enc_cenc"}) {
    const size_t compared = compare_with_golden(id);  // outside CHECK: a missing corpus file SKIPs the test
    CHECK(compared > 0);
  }
}

}  // namespace
