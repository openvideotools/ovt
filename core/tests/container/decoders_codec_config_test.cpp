// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the group C part 1 hand decoders (chapter 08 §8.3.3, WI-1.207): `avcC`, `hvcC` and `av1C` with the
// parameter sets handed to the syntax module (parse_parameter_set) and attached under their fields, and the
// CodecPrivate list of a sample entry.
//
// The record payloads below were written by the same tools and options as the corpus recipes 29 (x264 Baseline,
// CAVLC, ffmpeg -c copy into MP4), 67 (x265 Main 10, ffmpeg -tag:v hev1) and 15 (aomenc 10-bit, ffmpeg into MP4),
// at 128x64 and four frames so they stay small. The corpus files themselves are checked by the [needs-corpus] case.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/box_walk.h"
#include "container/isobmff/decoders/codec_config.h"
#include "container/isobmff/field_builder.h"
#include "io/memory_file.hpp"
#include "ovt/cxx/io/random_access_file.hpp"
#include "syntax/common/parameter_set.h"
#include "test_support/corpus.h"

namespace {

namespace iso = ovt::container::isobmff;
using Bytes = std::vector<uint8_t>;
using ovt::fb::BoxNodeT;
using ovt::fb::FieldT;

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
Bytes hex(std::string_view h) {
  Bytes out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(std::string(h.substr(i, 2)), nullptr, 16)));
  }
  return out;
}
// A length-prefixed NAL unit as avcC / hvcC store it.
Bytes nal(std::string_view h) {
  const Bytes n = hex(h);
  return cat({be(n.size(), 2), n});
}

// x264 --profile baseline --no-cabac --slices 4, muxed by ffmpeg (recipe 29 tools).
constexpr std::string_view kAvccBaseline =
    "0142c00affe100176742c00adb0826c044000003000400000300f23c489b8001000568ca83cb20";
constexpr std::string_view kSpsBaseline = "6742c00adb0826c044000003000400000300f23c489b80";
// x264 --profile high, muxed by ffmpeg: the SPS holds an emulation prevention byte (raw byte 12).
constexpr std::string_view kSpsHigh = "6764000aacd94209b0110000030001000003003c8f122596";
constexpr std::string_view kPpsHigh = "68ebe3cb22c0";
// x265 --profile main10, ffmpeg -tag:v hev1 (recipe 67 tools): VPS, SPS and PPS arrays.
constexpr std::string_view kHvccMain10 =
    "0102200000009000000000001ef000fcfdfafa00000f03200001001840010c01ffff02200000030090000003000003001e959809210001"
    "002842010102200000030090000003000003001ea0102041365959a4932bc0404000000300400000078222000100074401c172b42240";
// aomenc --bit-depth=10, ffmpeg into MP4 (recipe 15 tools): one sequence header OBU in configOBUs.
constexpr std::string_view kAv1c10bit = "81004c000a0d000000032fffcdaf9d09100904";

// ---- running one decoder the way the walker does (§8.2.3 steps 2.11-2.12) ----

iso::FourCC cc_of(std::string_view s) {
  iso::FourCC cc{};
  std::copy_n(s.begin(), 4, cc.begin());
  return cc;
}

iso::BoxContext entry_ctx(std::string_view entry) {
  iso::BoxContext c;
  for (const std::string_view s : {"moov", "trak", "mdia", "minf", "stbl", "stsd"}) {
    c.ancestors.push_back(cc_of(s));
  }
  c.ancestors.push_back(cc_of(entry));
  c.sample_entry = cc_of(entry);
  c.handler_type = cc_of("vide");
  return c;
}

struct Decoded {
  BoxNodeT node;
  iso::DecodeOutcome outcome;
  bool found = false;
  size_t payload_bytes = 0;
};

Decoded run(std::string_view type, const Bytes& payload, std::string_view entry) {
  Decoded d;
  d.payload_bytes = payload.size();
  d.node.fourcc = std::string(type);
  d.node.header_length = 8;
  d.node.file_span = std::make_unique<ovt::fb::ByteSpan>(0, 8 + payload.size());
  iso::BoxContext ctx = entry_ctx(entry);
  const iso::RegistryEntry* e = iso::lookup(type, ctx);
  if (e == nullptr || e->decoder == nullptr) {
    return d;
  }
  d.found = true;
  ovt::BitReader br(payload);
  iso::FieldBuilder fb(d.node, 0, br, 64);
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

// "a/b/c": a path of field names from the box.
const FieldT& field(const BoxNodeT& n, std::string_view path) {
  const std::vector<std::unique_ptr<FieldT>>* level = &n.fields;
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

// Depth-first search for a field named `name` below `f`.
const FieldT* find(const FieldT& f, std::string_view name) {
  for (const auto& c : f.children) {
    if (c->name == name) {
      return c.get();
    }
    if (const FieldT* hit = find(*c, name); hit != nullptr) {
      return hit;
    }
  }
  return nullptr;
}

// The end of the last node of a field's subtree, in box bits.
uint64_t last_end(const FieldT& f) {
  uint64_t end = 0;
  for (const auto& c : f.children) {
    end = std::max({end, c->span->bit_offset() + c->span->bit_length(), last_end(*c)});
  }
  return end;
}

bool has_field(const BoxNodeT& n, std::string_view name) {
  return child(n.fields, name) != nullptr;
}

bool has_code(const BoxNodeT& n, std::string_view code) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code; });
}

bool has_severity_at_least(const BoxNodeT& n, ovt::fb::Severity s) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->severity >= s; });
}

// Every top-level field starts where the previous one ended (zero-length derived fields excepted) and the last one
// ends at the payload end.
void check_contiguous(const Decoded& d) {
  uint64_t at = 64;
  for (const auto& f : d.node.fields) {
    INFO(f->name);
    REQUIRE(f->span != nullptr);
    CHECK(f->span->bit_offset() == at);
    at = f->span->bit_offset() + f->span->bit_length();
  }
  CHECK(at == 64 + 8 * d.payload_bytes);
}

// Every node of an attached syntax tree lies inside the field that holds the unit's bytes.
void check_inside(const FieldT& unit, const FieldT& f) {
  for (const auto& c : f.children) {
    INFO(c->name);
    REQUIRE(c->span != nullptr);
    CHECK(c->span->bit_offset() >= unit.span->bit_offset());
    CHECK(c->span->bit_offset() + c->span->bit_length() <= unit.span->bit_offset() + unit.span->bit_length());
    check_inside(unit, *c);
  }
}

TEST_CASE("avcC, hvcC and av1C have decoders in sample entries and outside them",
          "[container][decoders][codec_config]") {
  for (const std::string_view cc : {"avcC", "hvcC", "av1C"}) {
    INFO(cc);
    const iso::RegistryEntry* in_entry = iso::lookup(cc, entry_ctx("avc1"));
    REQUIRE(in_entry != nullptr);
    CHECK(in_entry->decoder != nullptr);
    iso::BoxContext mkv;  // Matroska CodecPrivate reuse (§8.6.7): ancestors ["mkv","CodecPrivate"]
    mkv.ancestors = {cc_of("mkv "), cc_of("Code")};
    const iso::RegistryEntry* outside = iso::lookup(cc, mkv);
    REQUIRE(outside != nullptr);
    CHECK(outside->decoder != nullptr);
  }
}

// AT1, adapted (SPEC-ASSUMPTION(#675)): recipe 29 is an x264 Baseline encode, so the record has profile 66
// and no High-profile extension; the extension fields are covered by the High profile case below.
TEST_CASE("avcC of the recipe-29 encode: profile from the SPS, 4-byte lengths, one SPS with its syntax tree",
          "[container][decoders][codec_config]") {
  const Decoded d = run("avcC", hex(kAvccBaseline), "avc1");
  REQUIRE(d.found);
  CHECK_FALSE(d.outcome.truncated);
  CHECK(field(d.node, "configurationVersion").uvalue == 1);
  CHECK(field(d.node, "AVCProfileIndication").uvalue == 66);
  CHECK(field(d.node, "AVCProfileIndication").svalue == "Baseline");
  CHECK(field(d.node, "AVCLevelIndication").svalue == "1.0");
  CHECK(field(d.node, "lengthSizeMinusOne").uvalue == 3);
  CHECK(field(d.node, "lengthSizeMinusOne").svalue == "NAL length size = 4");
  CHECK(field(d.node, "numOfSequenceParameterSets").uvalue == 1);
  CHECK(field(d.node, "numOfPictureParameterSets").uvalue == 1);
  CHECK_FALSE(has_field(d.node, "chroma_format"));
  const FieldT& sps = field(d.node, "sequenceParameterSets/sequenceParameterSet[0]/sequenceParameterSetNALUnit");
  CHECK(sps.bytes == hex(kSpsBaseline));
  CHECK(sps.svalue == "→ syntax tree");
  const FieldT* profile_idc = find(sps, "profile_idc");
  REQUIRE(profile_idc != nullptr);
  CHECK(profile_idc->uvalue == field(d.node, "AVCProfileIndication").uvalue);
  check_inside(sps, sps);
  const FieldT& pps = field(d.node, "pictureParameterSets/pictureParameterSet[0]/pictureParameterSetNALUnit");
  REQUIRE(find(pps, "pic_parameter_set_id") != nullptr);
  check_inside(pps, pps);
  CHECK_FALSE(has_severity_at_least(d.node, ovt::fb::Severity::Warning));
  check_contiguous(d);
}

TEST_CASE("avcC High profile: the extension, and spans across an emulation prevention byte",
          "[container][decoders][codec_config]") {
  const Bytes ext = {0xFD, 0xF8, 0xF8, 0x00};  // chroma_format 1, 8-bit luma and chroma, no SPS extensions
  const Bytes rec = cat({hex("0164000affe1"), nal(kSpsHigh), be(1, 1), nal(kPpsHigh), ext});
  const Decoded d = run("avcC", rec, "avc1");
  REQUIRE(d.found);
  CHECK(field(d.node, "AVCProfileIndication").svalue == "High");
  CHECK(field(d.node, "chroma_format").uvalue == 1);
  CHECK(field(d.node, "chroma_format").svalue == "4:2:0");
  CHECK(field(d.node, "bit_depth_luma_minus8").uvalue == 0);
  CHECK(field(d.node, "bit_depth_chroma_minus8").uvalue == 0);
  CHECK(field(d.node, "numOfSequenceParameterSetExt").uvalue == 0);
  const FieldT& sps = field(d.node, "sequenceParameterSets/sequenceParameterSet[0]/sequenceParameterSetNALUnit");
  CHECK(find(sps, "profile_idc")->uvalue == 100);
  check_inside(sps, sps);
  // The PPS sees the SPS of the same record: it is not parsed provisionally (no SYN-H264-0020).
  CHECK_FALSE(has_code(d.node, "SYN-H264-0020"));
  // Two emulation prevention bytes sit inside the SPS: mapped back to raw bits, the last node of the tree ends where
  // the NAL unit ends, not 16 bits before.
  CHECK(last_end(sps) == sps.span->bit_offset() + sps.span->bit_length());
  check_contiguous(d);
}

TEST_CASE("avcC High profile without the extension bytes decodes without them", "[container][decoders][codec_config]") {
  const Bytes rec = cat({hex("0164000affe1"), nal(kSpsHigh), be(1, 1), nal(kPpsHigh)});
  const Decoded d = run("avcC", rec, "avc1");
  CHECK_FALSE(has_field(d.node, "chroma_format"));
  CHECK(d.node.diagnostics.empty());
  check_contiguous(d);
}

TEST_CASE("avcC: parameter sets in an avc3 entry are CNT-1020 Info; avc1 is quiet",
          "[container][decoders][codec_config]") {
  CHECK(has_code(run("avcC", hex(kAvccBaseline), "avc3").node, "CNT-1020"));
  CHECK_FALSE(has_code(run("avcC", hex(kAvccBaseline), "avc1").node, "CNT-1020"));
  const Bytes empty = hex("0142c00affe000");  // no SPS, no PPS
  CHECK_FALSE(has_code(run("avcC", empty, "avc3").node, "CNT-1020"));
}

TEST_CASE("avcC: an unknown configurationVersion stops the decoder; a cut SPS is CNT-1002",
          "[container][decoders][codec_config]") {
  const Decoded v2 = run("avcC", hex("0242c00affe1"), "avc1");
  CHECK(has_code(v2.node, "CNT-1003"));
  CHECK(v2.node.fields.size() == 1);
  Bytes cut = hex(kAvccBaseline);
  cut.resize(20);
  const Decoded t = run("avcC", cut, "avc1");
  CHECK(t.outcome.truncated);
  CHECK(has_code(t.node, "CNT-1002"));
  CHECK(field(t.node, "numOfSequenceParameterSets").uvalue == 1);
}

TEST_CASE("avcC: a malformed SPS keeps its partial tree and its diagnostic lands on the box",
          "[container][decoders][codec_config]") {
  const Bytes rec = cat({hex("0164001fffe1"), nal("6764"), be(0, 1)});
  const Decoded d = run("avcC", rec, "avc1");
  CHECK_FALSE(d.outcome.truncated);
  const FieldT& sps = field(d.node, "sequenceParameterSets/sequenceParameterSet[0]/sequenceParameterSetNALUnit");
  CHECK_FALSE(sps.children.empty());
  CHECK(has_severity_at_least(d.node, ovt::fb::Severity::Error));
}

// AT2.
TEST_CASE("hvcC of the recipe-67 encode: Main 10, 10-bit luma, VPS / SPS / PPS arrays with syntax trees",
          "[container][decoders][codec_config]") {
  const Decoded d = run("hvcC", hex(kHvccMain10), "hev1");
  REQUIRE(d.found);
  CHECK_FALSE(d.outcome.truncated);
  CHECK(field(d.node, "general_profile_idc").uvalue == 2);
  CHECK(field(d.node, "general_profile_idc").svalue == "Main 10");
  CHECK(field(d.node, "general_tier_flag").svalue == "Main tier");
  CHECK(field(d.node, "general_constraint_indicator_flags").svalue == "progressive_source|frame_only_constraint");
  CHECK(field(d.node, "general_level_idc").svalue == "1");
  CHECK(field(d.node, "chromaFormat").svalue == "4:2:0");
  CHECK(field(d.node, "bitDepthLumaMinus8").uvalue == 2);
  CHECK(field(d.node, "bitDepthLumaMinus8").svalue == "10-bit");
  CHECK(field(d.node, "lengthSizeMinusOne").uvalue == 3);
  CHECK(field(d.node, "numOfArrays").uvalue == 3);
  std::vector<uint64_t> types;
  for (unsigned i = 0; i < 3; ++i) {
    const std::string a = "arrays/array[" + std::to_string(i) + "]";
    types.push_back(field(d.node, a + "/NAL_unit_type").uvalue);
    const FieldT& unit = field(d.node, a + "/nalus/nalu[0]/nalUnit");
    REQUIRE_FALSE(unit.children.empty());
    check_inside(unit, unit);
  }
  CHECK(types == std::vector<uint64_t>{32, 33, 34});
  const FieldT& sps = field(d.node, "arrays/array[1]/nalus/nalu[0]/nalUnit");
  const FieldT* luma = find(sps, "bit_depth_luma_minus8");
  REQUIRE(luma != nullptr);
  CHECK(luma->uvalue == 2);
  CHECK_FALSE(has_field(d.node, "parameter_sets.derived"));
  CHECK_FALSE(has_severity_at_least(d.node, ovt::fb::Severity::Warning));
  check_contiguous(d);
}

TEST_CASE("hvcC of a hev1 entry without arrays says the parameter sets are in-band",
          "[container][decoders][codec_config]") {
  Bytes rec = hex(kHvccMain10);
  rec.resize(23);
  rec[22] = 0;  // numOfArrays
  const Decoded hev1 = run("hvcC", rec, "hev1");
  CHECK(field(hev1.node, "parameter_sets.derived").svalue == "parameter sets in-band");
  CHECK_FALSE(has_field(run("hvcC", rec, "hvc1").node, "parameter_sets.derived"));
}

// AT3.
TEST_CASE("av1C of the recipe-15 encode: 10-bit, 4:2:0 and exactly one sequence header in configOBUs",
          "[container][decoders][codec_config]") {
  const Decoded d = run("av1C", hex(kAv1c10bit), "av01");
  REQUIRE(d.found);
  CHECK_FALSE(d.outcome.truncated);
  CHECK(field(d.node, "marker").uvalue == 1);
  CHECK(field(d.node, "version").uvalue == 1);
  CHECK(field(d.node, "seq_profile").svalue == "Main");
  CHECK(field(d.node, "high_bitdepth").uvalue == 1);
  CHECK(field(d.node, "bit_depth.derived").svalue == "10");
  CHECK(field(d.node, "chroma_format.derived").svalue == "4:2:0");
  const FieldT& obus = field(d.node, "configOBUs");
  REQUIRE(obus.children.size() == 1);
  const FieldT& seq = *obus.children[0];
  CHECK(find(seq, "obu_type")->uvalue == 1);
  CHECK(find(seq, "high_bitdepth")->uvalue == 1);
  check_inside(seq, seq);
  CHECK_FALSE(has_code(d.node, "CNT-1021"));
  CHECK_FALSE(has_severity_at_least(d.node, ovt::fb::Severity::Warning));
  check_contiguous(d);
}

TEST_CASE("av1C: seq_level_idx_0 13 is level 5.1, 31 is unconstrained", "[container][decoders][codec_config]") {
  Bytes rec = hex(kAv1c10bit);
  rec[1] = 13;  // seq_profile 0, seq_level_idx_0 13
  CHECK(field(run("av1C", rec, "av01").node, "seq_level_idx_0").svalue == "5.1");
  rec[1] = 31;
  CHECK(field(run("av1C", rec, "av01").node, "seq_level_idx_0").svalue == "unconstrained");
}

TEST_CASE("av1C: an OBU other than sequence header or metadata in configOBUs is CNT-1021",
          "[container][decoders][codec_config]") {
  const Bytes rec = cat({hex(kAv1c10bit), hex("1200")});  // a temporal delimiter OBU after the sequence header
  const Decoded d = run("av1C", rec, "av01");
  CHECK(field(d.node, "configOBUs").children.size() == 2);
  CHECK(has_code(d.node, "CNT-1021"));
  const Decoded none = run("av1C", hex("81004c00"), "av01");
  CHECK(field(none.node, "configOBUs").children.empty());
  check_contiguous(none);
}

TEST_CASE("parse_parameter_set: one dispatcher for the three codecs", "[syntax][parameter_set]") {
  using ovt::fb::Codec;
  ovt::syntax::ParameterSetContext ctx;
  const auto sps = ovt::syntax::parse_parameter_set(Codec::H264, hex(kSpsHigh), &ctx);
  CHECK(sps.has_parser);
  CHECK(sps.complete);
  CHECK(sps.unit_type == 7);
  CHECK(sps.epb_positions == std::vector<uint32_t>{12, 17});
  const auto sei = ovt::syntax::parse_parameter_set(Codec::H264, hex("0605"), &ctx);
  CHECK(sei.unit_type == 6);
  CHECK_FALSE(sei.has_parser);
  const auto vps = ovt::syntax::parse_parameter_set(Codec::H265, hex("40010c01ffff022000000300900000030000030"
                                                                     "01e959809"));
  CHECK(vps.unit_type == 32);
  CHECK(vps.complete);
  const auto obu = ovt::syntax::parse_parameter_set(Codec::AV1, hex("0a0d000000032fffcdaf9d09100904"));
  CHECK(obu.unit_type == 1);
  CHECK(obu.complete);
  CHECK(ovt::syntax::parse_parameter_set(Codec::VP9, hex("00")).tree.empty());
}

// ---- CodecPrivate (§8.3.3 step 6) through the walker ----

Bytes visual_entry(std::string_view type, const Bytes& children) {
  Bytes compressor(32, 0);
  return box(type, cat({zeros(6), be(1, 2), zeros(16), be(128, 2), be(64, 2), be(0x00480000, 4), be(0x00480000, 4),
                        zeros(4), be(1, 2), compressor, be(24, 2), be(0xFFFF, 2), children}));
}

Bytes file_with_entry(const Bytes& entry) {
  const Bytes hdlr = box("hdlr", cat({zeros(4), zeros(4), str("vide"), zeros(12), be(0, 1)}));
  const Bytes stsd = box("stsd", cat({be(0, 4), be(1, 4), entry}));
  const Bytes moov = box("moov", box("trak", box("mdia", cat({hdlr, box("minf", box("stbl", stsd))}))));
  return cat({box("ftyp", cat({str("isom"), zeros(4), str("isom")})), moov});
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

TEST_CASE("codec_private_of lists the configuration children in order with payload bytes and box offsets",
          "[container][decoders][codec_config]") {
  const Bytes avcc = hex(kAvccBaseline);
  const Bytes btrt = box("btrt", zeros(12));
  const Bytes dvvc = box("dvvC", zeros(24));
  const Bytes file = file_with_entry(visual_entry("avc1", cat({box("avcC", avcc), btrt, dvvc})));
  const ovt::io::MemoryFile mem(file);
  const iso::WalkResult walked = iso::walk(mem);
  const BoxNodeT* avc1 = find_box(walked.top_level, "avc1");
  REQUIRE(avc1 != nullptr);
  const BoxNodeT* avcc_node = find_box(avc1->children, "avcC");
  REQUIRE(avcc_node != nullptr);
  CHECK(field(*avcc_node, "AVCProfileIndication").uvalue == 66);  // the walker ran the decoder
  const auto cps = iso::codec_private_of(mem, *avc1);
  REQUIRE(cps.size() == 2);
  CHECK(cps[0].fourcc == "avcC");
  CHECK(cps[0].bytes == avcc);
  CHECK(cps[0].box_offset == avcc_node->file_span->offset());
  CHECK(cps[1].fourcc == "dvvC");
  CHECK(cps[1].bytes == zeros(24));
  CHECK(iso::is_codec_config_fourcc("esds"));
  CHECK_FALSE(iso::is_codec_config_fourcc("btrt"));
}

// ---- the corpus files of the acceptance tests and the container-smoke corpus ----

void check_records(const std::vector<std::unique_ptr<BoxNodeT>>& boxes, size_t& seen) {
  for (const auto& b : boxes) {
    if (b->fourcc == "avcC" || b->fourcc == "hvcC" || b->fourcc == "av1C") {
      INFO(b->fourcc);
      ++seen;
      CHECK_FALSE(has_code(*b, "CNT-1002"));
      CHECK_FALSE(has_code(*b, "CNT-1003"));
      CHECK_FALSE(has_severity_at_least(*b, ovt::fb::Severity::Error));
    }
    check_records(b->children, seen);
  }
}

iso::WalkResult walk_corpus(std::string_view id, std::unique_ptr<ovt::io::RandomAccessFile>& file) {
  // corpus_or_skip() returns the entry's primary file. For the recipes with an "mp4" key (recipe 15), generate.py
  // wraps the elementary stream with ffmpeg and lists the .mp4 first, so the primary file is the MP4.
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  INFO(path.string());
  REQUIRE(path.extension() == ".mp4");
  file = ovt::io::RandomAccessFile::open(path.string());
  return iso::walk(*file);
}

void check_corpus_file(std::string_view id) {
  INFO(id);
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  const iso::WalkResult walked = walk_corpus(id, file);
  size_t seen = 0;
  check_records(walked.top_level, seen);
  CHECK(seen > 0);
}

const BoxNodeT& corpus_record(const iso::WalkResult& walked, std::string_view type) {
  const BoxNodeT* rec = find_box(walked.top_level, type);
  REQUIRE(rec != nullptr);
  size_t seen = 0;
  check_records(walked.top_level, seen);
  return *rec;
}

TEST_CASE("the configuration records of the container-smoke corpus decode without errors",
          "[container][decoders][codec_config][needs-corpus]") {
  for (const std::string_view id : {"mpegff_hvc1_only", "mpegff_hev1_clg1_header", "ytugc_clip01"}) {
    check_corpus_file(id);
  }
}

// AT1 on the corpus file, as adapted by #675 (SPEC-ASSUMPTION(#675)).
TEST_CASE("AT1 on syn_h264_multislice_cavlc_mp4: avcC profile, 4-byte lengths, one SPS, no extension",
          "[container][decoders][codec_config][needs-corpus]") {
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  const iso::WalkResult walked = walk_corpus("syn_h264_multislice_cavlc_mp4", file);
  const BoxNodeT& avcc = corpus_record(walked, "avcC");
  const FieldT& sps = field(avcc, "sequenceParameterSets/sequenceParameterSet[0]/sequenceParameterSetNALUnit");
  REQUIRE(find(sps, "profile_idc") != nullptr);
  CHECK(field(avcc, "AVCProfileIndication").uvalue == find(sps, "profile_idc")->uvalue);
  CHECK(field(avcc, "AVCProfileIndication").uvalue == 66);  // x264 --profile baseline (recipe 4)
  CHECK(field(avcc, "lengthSizeMinusOne").uvalue == 3);
  CHECK(field(avcc, "numOfSequenceParameterSets").uvalue == 1);
  CHECK_FALSE(has_field(avcc, "chroma_format"));
}

// AT2 on the corpus file.
TEST_CASE("AT2 on syn_h265_main10_hev1: hvcC Main 10, bitDepthLumaMinus8 2, arrays 32 / 33 / 34",
          "[container][decoders][codec_config][needs-corpus]") {
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  const iso::WalkResult walked = walk_corpus("syn_h265_main10_hev1", file);
  const BoxNodeT& hvcc = corpus_record(walked, "hvcC");
  CHECK(field(hvcc, "general_profile_idc").uvalue == 2);
  CHECK(field(hvcc, "bitDepthLumaMinus8").uvalue == 2);
  std::vector<uint64_t> types;
  const FieldT& arrays = field(hvcc, "arrays");
  for (const auto& a : arrays.children) {
    types.push_back(child(a->children, "NAL_unit_type")->uvalue);
  }
  for (const uint64_t t : {32U, 33U, 34U}) {
    INFO(t);
    CHECK(std::ranges::find(types, t) != types.end());
  }
}

// AT3 on the corpus file. SPEC-ASSUMPTION(#689): a 1080p30 aomenc encode carries seq_level_idx_0 8 ("4.0"), not
// 5.1; the expected value is the one mp4box.js reports for the file (recorded in the PR).
TEST_CASE("AT3 on syn_av1_filmgrain_10bit: av1C high_bitdepth, level and one sequence header",
          "[container][decoders][codec_config][needs-corpus]") {
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  const iso::WalkResult walked = walk_corpus("syn_av1_filmgrain_10bit", file);
  const BoxNodeT& av1c = corpus_record(walked, "av1C");
  CHECK(field(av1c, "high_bitdepth").uvalue == 1);
  CHECK(field(av1c, "seq_level_idx_0").uvalue == 8);
  CHECK(field(av1c, "seq_level_idx_0").svalue == "4.0");
  const FieldT& obus = field(av1c, "configOBUs");
  REQUIRE(obus.children.size() == 1);
  CHECK(find(*obus.children[0], "obu_type")->uvalue == 1);
}

}  // namespace
