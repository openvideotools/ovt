// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the H.264 slice header parsers (chapter 7 §7.3.4 rows 24–27, chapter 14 §14.7.1, WI-1.109a).
// Input: byte 0 = SPS length, byte 1 = PPS length, then the SPS NAL unit, the PPS NAL unit and one more NAL unit
// (headers included, emulation prevention bytes present). When the SPS and PPS parse they seed a fresh
// ParameterSets (the SPS in both the SPS and the subset SPS table, so NAL 20 / 21 reach the slice header too); the
// third unit is parsed as a slice layer whatever its nal_unit_type (WI-1.109b adds the dispatch to the AUD, filler data
// and data partition parsers). Invariants, each an abort when violated: only ovt::ParseError
// escapes; two parses of the same bytes give the same JSON (determinism, §0.7.5); a parse with recording off leaves
// the reader at the same position and returns the same header length.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
namespace h264 = ovt::syntax::h264;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

struct Result {
  bool ok = false;
  uint64_t end_bit = 0;
  uint64_t header_bits = 0;
  std::string json;
};

Result parse_unit(const ovt::Rbsp& rbsp, const h264::ParameterSets& sets, bool recording) {
  Result r;
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  b.set_recording(recording);
  try {
    const h264::NalHeader h = h264::parse_nal_header(br, b);
    r.header_bits = h264::parse_slice_layer(br, b, h, sets).header_bits;
    r.ok = true;
  } catch (const ovt::ParseError&) {
    r.ok = false;
  }
  r.end_bit = br.bit_pos();
  const auto tree = b.finish();
  if (recording) {
    r.json = ovt::syntax::to_json(tree);
    (void)ovt::syntax::to_trace(tree, rbsp.rbsp);
  }
  return r;
}

template <class T, class Parser>
std::optional<T> parse_ps(std::span<const uint8_t> nal, Parser parser) {
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  std::optional<T> out;
  try {
    (void)h264::parse_nal_header(br, b);
    out = parser(br, b);
  } catch (const ovt::ParseError&) {
    out.reset();
  }
  (void)b.finish();
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 4) {
    return 0;
  }
  const std::span<const uint8_t> all(data, size);
  const size_t sps_len = std::min<size_t>(all[0], size - 2);
  const size_t pps_len = std::min<size_t>(all[1], size - 2 - sps_len);
  const auto sps = parse_ps<h264::Sps>(all.subspan(2, sps_len),
                                       [](BitReader& br, SyntaxBuilder& b) { return h264::parse_sps(br, b); });
  if (!sps) {
    return 0;
  }
  h264::SpsTable sps_table;
  sps_table.entries[sps->seq_parameter_set_id] = &*sps;
  const auto pps = parse_ps<h264::Pps>(all.subspan(2 + sps_len, pps_len), [&](BitReader& br, SyntaxBuilder& b) {
    return h264::parse_pps(br, b, sps_table);
  });
  if (!pps) {
    return 0;
  }
  h264::ParameterSets sets;
  sets.sps = sps_table;
  sets.subset_sps = sps_table;
  sets.pps.entries[pps->pic_parameter_set_id] = &*pps;
  const std::span<const uint8_t> nal = all.subspan(2 + sps_len + pps_len);
  if (nal.empty()) {
    return 0;
  }
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  const Result first = parse_unit(rbsp, sets, true);
  const Result again = parse_unit(rbsp, sets, true);
  require(first.ok == again.ok && first.json == again.json && first.end_bit == again.end_bit);
  const Result quiet = parse_unit(rbsp, sets, false);
  require(quiet.ok == first.ok && quiet.end_bit == first.end_bit && quiet.header_bits == first.header_bits);
  return 0;
}
