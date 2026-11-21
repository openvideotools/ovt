// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Per-unit fuzz target of the H.264 header parsers (chapter 7 WI-1.129 step 1, chapter 14 §14.7.1, backbone
// §0.7.1). The input is ONE NAL unit (header included, emulation prevention bytes present, no start code). For each
// canned context of fuzz_unit_contexts.inc (the first SPS and PPS of one seed stream of tools/fuzz_seed.py) a fresh
// ActiveSets is seeded with that SPS and PPS, and the unit goes through every parser that accepts its
// nal_unit_type, the way the index builder dispatches it (core/src/index/h264_units.cpp): the slice header and
// activation for 1 / 5 / 20 / 21 and partition A, partitions B / C, SEI with the context's SPS, SPS and PPS into the
// ActiveSets (re-parse of the stale PPS), AUD, end of sequence / stream, filler, SPS extension, prefix NAL and subset
// SPS. Invariants, each an abort when violated: only ovt::ParseError escapes a parser, and the
// checks of ovt_fuzz::drive() (fuzz_units_common.h): one pass over all contexts in under one second, the same tree
// from a second run on a freshly built context, the same outcome (and end bit on success) with recording off.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fuzz_units_common.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/active_sets.hpp"
#include "ovt/cxx/syntax/h264/aud.hpp"
#include "ovt/cxx/syntax/h264/data_partition.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/prefix_nal.hpp"
#include "ovt/cxx/syntax/h264/sei.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h264/sps_ext.hpp"
#include "ovt/cxx/syntax/h264/subset_sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt_fuzz::require;

#include "fuzz_unit_contexts.inc"

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
namespace h264 = ovt::syntax::h264;

// A fresh ActiveSets holding the context's SPS and PPS; the context must parse.
struct Context {
  h264::ActiveSets sets;
  std::shared_ptr<const h264::Sps> sps;
  std::shared_ptr<const h264::Pps> pps;
  std::vector<h264::SetDiagnostic> diags;
};

std::unique_ptr<Context> build_context(std::span<const unsigned char> bytes) {
  auto ctx = std::make_unique<Context>();
  uint64_t index = 0;
  for (const std::span<const uint8_t> nal : ovt_fuzz::split_context(bytes)) {
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
    BitReader br(rbsp.rbsp);
    SyntaxBuilder b(br);
    b.set_recording(false);
    const h264::NalHeader h = h264::parse_nal_header(br, b);
    if (h.nal_unit_type == 7) {
      h264::Sps sps = h264::parse_sps(br, b);
      ctx->sets.put_sps(std::move(sps), rbsp.rbsp, index, ctx->diags);
    } else {
      require(h.nal_unit_type == 8);
      h264::Pps pps = h264::parse_pps(br, b, ctx->sets.parameter_sets(ctx->diags).sps);
      ctx->pps = std::make_shared<const h264::Pps>(pps);
      ctx->sets.put_pps(std::move(pps), rbsp.rbsp, index, ctx->diags);
    }
    ++index;
  }
  require(ctx->pps != nullptr);
  ctx->sps = ctx->sets.sps(ctx->pps->seq_parameter_set_id);
  require(ctx->sps != nullptr);
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

void parse_slice(BitReader& br, SyntaxBuilder& b, const h264::NalHeader& h, Context& ctx) {
  const h264::ParameterSets& ps = ctx.sets.parameter_sets(ctx.diags);
  std::optional<h264::SliceHeader> sh;
  if (h.nal_unit_type == 2) {
    sh = h264::parse_slice_data_partition_a(br, b, h, ps).slice_header;
  } else {
    sh = h264::parse_slice_layer(br, b, h, ps);
  }
  if (sh) {
    (void)ctx.sets.activate(h, *sh, true, ctx.diags);
    ctx.sets.end_picture();
  }
}

void parse_non_vcl(BitReader& br, SyntaxBuilder& b, const h264::NalHeader& h, std::span<const uint8_t> rbsp,
                   Context& ctx) {
  switch (h.nal_unit_type) {
    case 3:
    case 4:
      (void)h264::parse_slice_data_partition_bc(br, b, h, *ctx.sps, *ctx.pps);
      break;
    case 6: {
      h264::SeiContext sei;
      sei.sps_by_id = ctx.sets.parameter_sets(ctx.diags).sps.entries;
      sei.active_sps = ctx.sps.get();
      (void)h264::parse_sei_rbsp(br, b, sei);
      break;
    }
    case 7:
      ctx.sets.put_sps(h264::parse_sps(br, b), rbsp, 9, ctx.diags);
      (void)ctx.sets.parameter_sets(ctx.diags);  // re-parses the context PPS when its SPS was replaced
      break;
    case 8:
      ctx.sets.put_pps(h264::parse_pps(br, b, ctx.sets.parameter_sets(ctx.diags).sps), rbsp, 9, ctx.diags);
      (void)ctx.sets.parameter_sets(ctx.diags);
      break;
    case 9:
      (void)h264::parse_access_unit_delimiter(br, b);
      break;
    case 10:
      (void)h264::parse_end_of_seq(br, b);
      break;
    case 11:
      (void)h264::parse_end_of_stream(br, b);
      break;
    case 12:
      (void)h264::parse_filler_data(br, b);
      break;
    case 13:
      (void)h264::parse_sps_extension(br, b);
      break;
    case 14:
      (void)h264::parse_prefix_nal_unit(br, b, h);
      break;
    case 15:
      ctx.sets.put_subset_sps(h264::parse_subset_sps(br, b).sps, rbsp, 9, ctx.diags);
      (void)ctx.sets.parameter_sets(ctx.diags);
      break;
    default:
      break;  // reserved / unspecified: no parser accepts them
  }
}

ovt_fuzz::Result run(const ovt::Rbsp& rbsp, std::span<const unsigned char> context, bool recording) {
  const std::unique_ptr<Context> ctx = make_context(context);
  ovt_fuzz::Result r;
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    const h264::NalHeader h = h264::parse_nal_header(br, b);
    const uint32_t t = h.nal_unit_type;
    if (t == 1 || t == 2 || t == 5 || t == 20 || t == 21) {
      parse_slice(br, b, h, *ctx);
    } else {
      parse_non_vcl(br, b, h, rbsp.rbsp, *ctx);
    }
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
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(std::span<const uint8_t>(data, size));
  ovt_fuzz::drive(k_h264_contexts, size, [&](std::span<const unsigned char> context, bool recording) {
    return run(rbsp, context, recording);
  });
  const auto context = k_h264_contexts[size % std::size(k_h264_contexts)];
  ovt_fuzz::check_parameter_set(ovt::fb::Codec::H264, ovt_fuzz::split_context(context), {data, size});
  return 0;
}
