// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Per-unit fuzz target of the H.265 header parsers (chapter 7 WI-1.129 step 1, chapter 14 §14.7.1, backbone
// §0.7.1). The input is ONE NAL unit (header included, emulation prevention bytes present, no start code). For each
// canned context of fuzz_unit_contexts.inc (the first VPS, SPS and PPS of one seed stream of tools/fuzz_seed.py,
// plus the start of its first slice segment) a fresh ActiveSets is seeded with those sets, and the unit goes through
// every parser that accepts its nal_unit_type, the way the index builder dispatches it
// (core/src/index/h265_units.cpp): a slice segment header against the context's PPS table (a dependent segment
// copies the context's first independent one), then activation, the short-term RPS in use and the entry-point
// substreams; VPS, SPS and PPS into the ActiveSets (the PPS table is rebuilt, so activate_pps runs); prefix and
// suffix SEI with the context's parameter sets. AUD, end of sequence / bitstream and filler data have no parser yet
// (the index keeps them opaque). Invariants, each an abort when violated: only ovt::ParseError escapes a parser, and
// the checks of ovt_fuzz::drive() (fuzz_units_common.h): one pass over all contexts in under one second, the same tree
// from a second run on a freshly built context, the same outcome (and end bit on success) with recording off.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fuzz_units_common.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/active_sets.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt_fuzz::require;

#include "fuzz_unit_contexts.inc"

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
namespace h265 = ovt::syntax::h265;

constexpr uint8_t kVpsNut = 32;
constexpr uint8_t kSpsNut = 33;
constexpr uint8_t kPpsNut = 34;
constexpr uint8_t kPrefixSeiNut = 39;
constexpr uint8_t kSuffixSeiNut = 40;

struct Context {
  h265::ActiveSets sets;
  std::optional<h265::SliceSegmentHeader> independent;  // the context's first slice segment header
  std::vector<h265::SetDiagnostic> diags;
};

// A fresh ActiveSets holding the context's VPS, SPS and PPS, and its first slice segment header; all must parse.
std::unique_ptr<Context> build_context(std::span<const unsigned char> bytes) {
  auto ctx = std::make_unique<Context>();
  uint64_t index = 0;
  for (const std::span<const uint8_t> nal : ovt_fuzz::split_context(bytes)) {
    const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
    BitReader br(rbsp.rbsp);
    SyntaxBuilder b(br);
    b.set_recording(false);
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    if (h.nal_unit_type == kVpsNut) {
      ctx->sets.put_vps(h265::parse_vps(br, b), rbsp.rbsp, index, ctx->diags);
    } else if (h.nal_unit_type == kSpsNut) {
      ctx->sets.put_sps(h265::parse_sps(br, b, h.nuh_layer_id), rbsp.rbsp, index, ctx->diags);
    } else if (h.nal_unit_type == kPpsNut) {
      ctx->sets.put_pps(h265::parse_pps(br, b), rbsp.rbsp, index, ctx->diags);
    } else {
      require(h.is_vcl);
      ctx->independent = h265::parse_slice_segment_header(br, b, h, ctx->sets.pps_table(), nullptr);
    }
    ++index;
  }
  require(ctx->independent.has_value());
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

void parse_slice(BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h, const ovt::Rbsp& rbsp, size_t nal_size,
                 Context& ctx) {
  const h265::SliceSegmentHeader sh =
      h265::parse_slice_segment_header(br, b, h, ctx.sets.pps_table(), &*ctx.independent);
  const std::optional<h265::Activation> act =
      ctx.sets.activate(sh.slice_pic_parameter_set_id, sh.first_slice_segment_in_pic_flag, ctx.diags);
  if (act) {
    (void)h265::current_st_ref_pic_set(sh, *act->sps);
  }
  (void)h265::entry_point_substreams(sh, rbsp.epb_positions, nal_size);
  ctx.sets.end_picture();
}

void parse_sei(BitReader& br, SyntaxBuilder& b, const h265::NalHeader& h, Context& ctx) {
  std::array<std::shared_ptr<const h265::Vps>, h265::ActiveSets::kMaxVps> vps_keep;
  std::array<std::shared_ptr<const h265::Sps>, h265::ActiveSets::kMaxSps> sps_keep;
  std::array<const h265::Vps*, h265::ActiveSets::kMaxVps> vps{};
  std::array<const h265::Sps*, h265::ActiveSets::kMaxSps> sps{};
  for (unsigned i = 0; i < h265::ActiveSets::kMaxVps; ++i) {
    vps_keep.at(i) = ctx.sets.vps(i);
    vps.at(i) = vps_keep.at(i).get();
  }
  for (unsigned i = 0; i < h265::ActiveSets::kMaxSps; ++i) {
    sps_keep.at(i) = ctx.sets.sps(i);
    sps.at(i) = sps_keep.at(i).get();
  }
  h265::SeiContext sei;
  sei.vps_by_id = vps;
  sei.sps_by_id = sps;
  const h265::PpsTable& ppss = ctx.sets.pps_table();
  const h265::ActivePps* active = ppss.entries.at(ctx.independent->slice_pic_parameter_set_id);
  sei.active_sps = active != nullptr ? active->sps : nullptr;
  (void)h265::parse_sei_rbsp(br, b, h, sei);
}

ovt_fuzz::Result run(const ovt::Rbsp& rbsp, size_t nal_size, std::span<const unsigned char> context, bool recording) {
  const std::unique_ptr<Context> ctx = make_context(context);
  ovt_fuzz::Result r;
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    switch (h.nal_unit_type) {
      case kVpsNut:
        ctx->sets.put_vps(h265::parse_vps(br, b), rbsp.rbsp, 9, ctx->diags);
        (void)ctx->sets.pps_table();
        break;
      case kSpsNut:
        ctx->sets.put_sps(h265::parse_sps(br, b, h.nuh_layer_id), rbsp.rbsp, 9, ctx->diags);
        (void)ctx->sets.pps_table();  // rebuilds the context PPS's tile tables against the new SPS
        break;
      case kPpsNut:
        ctx->sets.put_pps(h265::parse_pps(br, b), rbsp.rbsp, 9, ctx->diags);
        (void)ctx->sets.pps_table();
        break;
      case kPrefixSeiNut:
      case kSuffixSeiNut:
        parse_sei(br, b, h, *ctx);
        break;
      default:
        if (h.is_vcl) {
          parse_slice(br, b, h, rbsp, nal_size, *ctx);
        }
        break;
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
  ovt_fuzz::drive(k_h265_contexts, size, [&](std::span<const unsigned char> context, bool recording) {
    return run(rbsp, size, context, recording);
  });
  const auto context = k_h265_contexts[size % std::size(k_h265_contexts)];
  ovt_fuzz::check_parameter_set(ovt::fb::Codec::H265, ovt_fuzz::split_context(context), {data, size});
  return 0;
}
