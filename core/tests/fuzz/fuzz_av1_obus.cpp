// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Per-OBU fuzz target of the AV1 header parsers (chapter 7 WI-1.129 step 1, chapter 14 §14.7.1, backbone §0.7.1).
// The input is ONE OBU (obu_header, optional extension and obu_size, then the payload; without obu_size the payload
// runs to the end of the input, with it the bytes after the payload are ignored). For each canned context of
// fuzz_unit_contexts.inc (the first sequence header and the header of the first frame of one seed stream of
// tools/fuzz_seed.py) a fresh DecoderState is built: the sequence header parsed and the first frame header parsed
// and saved into the reference slots by the reference frame update process (§7.20). The OBU then goes through every
// parser that accepts its obu_type: sequence header (and its colour description), temporal delimiter, frame header
// and redundant frame header against the context's state (followed by the reference frame update, or the loading
// process for show_existing_frame), frame_obu(), a tile group over the context frame's tile_info(), metadata, tile
// list and padding. Invariants, each an abort when violated: only ovt::ParseError escapes a parser, and the
// checks of ovt_fuzz::drive() (fuzz_units_common.h): one pass over all contexts in under one second, the same tree
// from a second run on a freshly built context, the same outcome (and end bit on success) with recording off.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fuzz_units_common.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/frame_header.hpp"
#include "ovt/cxx/syntax/av1/metadata.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/av1/tile_list.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt_fuzz::require;

#include "fuzz_unit_contexts.inc"

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)

constexpr uint8_t kAllFrames = 0xFF;

// The bytes of the first OBU of `bytes`: its header plus obu_size payload bytes, cut at the end of the input.
std::span<const uint8_t> first_obu(std::span<const uint8_t> bytes) {
  BitReader br(bytes);
  SyntaxBuilder b(br);
  b.set_recording(false);
  const ObuHeader h = parse_obu_header(br, b);
  const uint64_t end = h.header_bits / 8 + h.obu_size;
  return bytes.first(std::min<uint64_t>(end, bytes.size()));
}

struct Context {
  SequenceHeader seq;
  DecoderState state;
  TileInfo tile_info;  // of the context's first frame
};

// The context's sequence header and first frame header (cut short: only its header is read); both must parse.
std::unique_ptr<Context> build_context(std::span<const unsigned char> bytes) {
  auto ctx = std::make_unique<Context>();
  const std::span<const uint8_t> seq_obu = first_obu(bytes);
  {
    BitReader br(seq_obu);
    SyntaxBuilder b(br);
    b.set_recording(false);
    require(parse_obu_header(br, b).type() == ObuType::SequenceHeader);
    ctx->seq = parse_sequence_header(br, b);
  }
  BitReader br(bytes.subspan(seq_obu.size()));
  SyntaxBuilder b(br);
  b.set_recording(false);
  const ObuHeader h = parse_obu_header(br, b);
  require(h.type() == ObuType::Frame || h.type() == ObuType::FrameHeader);
  const FrameHeader fh = parse_frame_header(br, b, h, ctx->seq, ctx->state, false);
  reference_frame_update(ctx->state, fh.state, fh.refresh_frame_flags, 0);
  ctx->tile_info = fh.tile_info;
  return ctx;
}

// A fresh copy of the context, built once per process: every run starts from the same state (reset per input),
// and the 60 s smoke run does not spend its time re-parsing the canned parameter sets.
std::unique_ptr<Context> make_context(std::span<const unsigned char> bytes) {
  static std::map<const unsigned char*, std::unique_ptr<const Context>> pristine;
  std::unique_ptr<const Context>& slot = pristine[bytes.data()];
  if (!slot) {
    slot = build_context(bytes);
  }
  return std::make_unique<Context>(*slot);
}

// The reference frame update (or, for show_existing_frame, loading) process after a frame header (§7.20, §7.21).
void update_slots(Context& ctx, const FrameHeader& fh) {
  if (fh.show_existing_frame) {
    FrameState shown;
    if (reference_frame_loading(ctx.state, fh.frame_to_show_map_idx, shown) && shown.frame_type == FrameType::Key) {
      reference_frame_update(ctx.state, shown, kAllFrames, 1);
    }
    return;
  }
  reference_frame_update(ctx.state, fh.state, fh.refresh_frame_flags, 1);
}

void parse_payload(BitReader& br, SyntaxBuilder& b, const ObuHeader& h, Context& ctx) {
  switch (h.type()) {
    case ObuType::SequenceHeader:
      (void)color_description(parse_sequence_header(br, b));
      break;
    case ObuType::TemporalDelimiter:
      parse_temporal_delimiter(br, b);
      break;
    case ObuType::FrameHeader:
    case ObuType::RedundantFrameHeader:
      update_slots(ctx, parse_frame_header(br, b, h, ctx.seq, ctx.state, h.type() == ObuType::RedundantFrameHeader));
      break;
    case ObuType::Frame:
      update_slots(ctx, parse_frame_obu(br, b, h, ctx.seq, ctx.state).frame_header);
      break;
    case ObuType::TileGroup:
      (void)parse_tile_group_obu(br, b, ctx.tile_info, static_cast<uint32_t>(br.bits_left() / 8));
      break;
    case ObuType::Metadata: {
      const Metadata m = parse_metadata(br, b);
      ovt_ColorDescription cd{};
      apply_metadata(m, cd);
      break;
    }
    case ObuType::TileList:
      (void)parse_tile_list(br, b);
      break;
    case ObuType::Padding:
      (void)parse_padding(br, b);
      break;
    default:
      break;  // reserved: no parser accepts them
  }
}

ovt_fuzz::Result run(std::span<const uint8_t> obu, std::span<const unsigned char> context, bool recording) {
  const std::unique_ptr<Context> ctx = make_context(context);
  ovt_fuzz::Result r;
  BitReader br(obu);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    const ObuHeader h = parse_obu_header(br, b);
    parse_payload(br, b, h, *ctx);
    r.ok = true;
  } catch (const ovt::ParseError& e) {  // the expected answer to malformed input
    r.ok = false;
    r.error = e.code;
  }
  r.end_bit = br.bit_pos();
  const ovt::syntax::SyntaxNodeBuffer tree = b.finish();
  if (recording) {
    r.tree = ovt_fuzz::fingerprint(tree);
  }
  return r;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  std::span<const uint8_t> obu(data, size);
  try {
    obu = first_obu(obu);
  } catch (const ovt::ParseError&) {  // NOLINT(bugprone-empty-catch): a truncated header; parsed whole below
  }
  ovt_fuzz::drive(k_av1_contexts, size,
                  [&](std::span<const unsigned char> context, bool recording) { return run(obu, context, recording); });
  const std::span<const uint8_t> sequence_header = first_obu(k_av1_contexts[size % std::size(k_av1_contexts)]);
  ovt_fuzz::check_parameter_set(ovt::fb::Codec::AV1, {&sequence_header, 1}, obu);
  return 0;
}
