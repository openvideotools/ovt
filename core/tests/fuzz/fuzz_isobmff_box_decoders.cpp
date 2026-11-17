// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the ISOBMFF hand box decoders (chapter 08 §8.3, chapter 14 §14.7; WI-1.205 group A). It calls
// one registered decoder directly on a fuzzed payload, the way the walker does after reading version/flags, so the
// decoders are reached without a valid box tree around them (the whole-file walk is fuzz_isobmff_tree, WI-1.204).
// Input layout: byte 0 selects the box type and ancestor chain from kCases, byte 1 is the FullBox version, bytes
// 2-4 the flags, byte 5 the context bits (bit 0 quicktime, bit 1 movie/track timescales known, bits 2-3 the track
// handler: 0 none, 1 vide, 2 soun, 3 text; bit 4 stsd version 1), the rest is the payload. Invariants, each an abort
// when violated: no exception escapes run_decoder(); every top-level field span lies inside the box; decoding the same
// input twice gives byte-identical FlatBuffers. Later decoder WIs add their boxes to kCases (append: the corpus seeds
// are indexed by position).

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt_container_generated.h"

namespace {

namespace iso = ovt::container::isobmff;

struct Case {
  std::string_view fourcc;
  std::string_view parent;  // innermost ancestor ("" = top level)
  bool fullbox;
  std::string_view outer{};  // NOLINT(readability-redundant-member-init): keeps -Wmissing-field-initializers quiet;
                             // ancestor of `parent`, "" = none (sample entry children: stsd)
};

constexpr auto kCases = std::to_array<Case>({
    {"ftyp", "", false},
    {"styp", "", false},
    {"mvhd", "moov", true},
    {"tkhd", "trak", true},
    {"hint", "tref", false},
    {"msrc", "trgr", true},
    {"elst", "edts", true},
    {"mdhd", "mdia", true},
    {"hdlr", "mdia", true},
    {"hdlr", "meta", true},
    {"elng", "mdia", true},
    {"vmhd", "minf", true},
    {"smhd", "minf", true},
    {"hmhd", "minf", true},
    {"nmhd", "minf", true},
    {"sthd", "minf", true},
    {"dref", "dinf", true},
    {"url ", "dref", true},
    {"urn ", "dref", true},
    {"pdin", "", true},
    // WI-1.206 group B: stsd and the sample entries (the layout follows the handler bits of the context byte).
    {"stsd", "stbl", true},
    {"avc1", "stsd", false},
    {"mp4a", "stsd", false},
    {"tx3g", "stsd", false},
    {"ftab", "tx3g", false, "stsd"},
    {"wvtt", "stsd", false},
    {"vttC", "wvtt", false, "stsd"},
    {"vlab", "wvtt", false, "stsd"},
    {"stpp", "stsd", false},
    {"mett", "stsd", false},
    {"txtC", "mett", true, "stsd"},
    {"metx", "stsd", false},
    {"urim", "stsd", false},
    {"uri ", "urim", true, "stsd"},
    {"uriI", "urim", true, "stsd"},
    {"tmcd", "stsd", false},
    {"name", "tmcd", false, "stsd"},
    // WI-1.208 group C part 2: colour, HDR, Dolby Vision and other sample-entry children.
    {"btrt", "hvc1", false, "stsd"},
    {"pasp", "hvc1", false, "stsd"},
    {"clap", "hvc1", false, "stsd"},
    {"colr", "hvc1", false, "stsd"},
    {"mdcv", "hvc1", false, "stsd"},
    {"clli", "hvc1", false, "stsd"},
    {"cclv", "hvc1", false, "stsd"},
    {"dvcC", "hvc1", false, "stsd"},
    {"dvvC", "hvc1", false, "stsd"},
    {"fiel", "hvc1", false, "stsd"},
    {"srat", "mp4a", true, "stsd"},
    {"colr", "ipco", false},
    // WI-1.207 group C part 1: the codec configuration records (their parameter sets go through the syntax module).
    {"avcC", "avc1", false, "stsd"},
    {"hvcC", "hvc1", false, "stsd"},
    {"av1C", "av01", false, "stsd"},
    {"vpcC", "vp09", true, "stsd"},
    {"esds", "mp4a", true, "stsd"},
    {"dOps", "Opus", false, "stsd"},
});

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

iso::FourCC cc_of(std::string_view s) {
  iso::FourCC cc{};
  std::memcpy(cc.data(), s.data(), cc.size());
  return cc;
}

std::vector<uint8_t> decode_once(const Case& c, uint8_t version, uint32_t flags, uint8_t ctx_bits,
                                 std::span<const uint8_t> payload) {
  iso::BoxContext ctx;
  if (!c.outer.empty()) {
    ctx.ancestors.push_back(cc_of(c.outer));
  }
  if (!c.parent.empty()) {
    ctx.ancestors.push_back(cc_of(c.parent));
  }
  ctx.quicktime = (ctx_bits & 1U) != 0;
  if ((ctx_bits & 2U) != 0) {
    ctx.movie_timescale = 600;
    ctx.track_timescale = 90000;
  }
  constexpr std::array<std::string_view, 4> kHandlers = {"", "vide", "soun", "text"};
  if (const std::string_view h = kHandlers.at((ctx_bits >> 2U) & 3U); !h.empty()) {
    ctx.handler_type = cc_of(h);
  }
  ctx.stsd_version = (ctx_bits & 16U) != 0 ? 1 : 0;
  const iso::RegistryEntry* e = iso::lookup(c.fourcc, ctx);
  require(e != nullptr && e->decoder != nullptr);

  ovt::fb::BoxNodeT node;
  node.fourcc = std::string(c.fourcc);
  node.header_length = 8;
  const uint64_t extra = c.fullbox ? 4U : 0U;
  node.file_span = std::make_unique<ovt::fb::ByteSpan>(0, 8 + extra + payload.size());
  if (c.fullbox) {
    node.version = version;
    node.flags = static_cast<int32_t>(flags);
  }
  ovt::BitReader br(payload);
  iso::FieldBuilder fb(node, 0, br, 64U + 8U * extra);
  try {
    (void)iso::run_decoder(*e->decoder, ctx, br, fb);
  } catch (...) {
    std::abort();  // only ParseError is expected, and run_decoder turns it into CNT-1002
  }
  const uint64_t box_bits = 8U * node.file_span->length();
  for (const auto& f : node.fields) {
    require(f->span != nullptr && f->span->bit_offset() + f->span->bit_length() <= box_bits);
  }
  flatbuffers::FlatBufferBuilder b;
  b.Finish(ovt::fb::BoxNode::Pack(b, &node));
  return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 6) {
    return 0;
  }
  const Case& c = kCases.at(data[0] % kCases.size());
  const uint32_t flags = (uint32_t{data[2]} << 16U) | (uint32_t{data[3]} << 8U) | data[4];
  const std::span<const uint8_t> payload(data + 6, size - 6);
  const auto a = decode_once(c, data[1], flags, data[5], payload);
  const auto b = decode_once(c, data[1], flags, data[5], payload);
  require(a == b);
  return 0;
}
