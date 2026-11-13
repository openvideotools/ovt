// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the H.265 slice segment header parser (chapter 7 §7.3.5 rows 10–12, chapter 14 §14.7.1,
// WI-1.113). Input: byte 0 = SPS length, byte 1 = PPS length, then the SPS NAL unit, the PPS NAL unit and one slice
// segment NAL unit (headers included, emulation prevention bytes present). When SPS and PPS parse, the PPS is
// activated against the SPS and stored as PPS id `pps_pic_parameter_set_id`; the slice is then parsed twice without
// a preceding independent segment and, when the first parse succeeded with an independent segment, once more with
// that header as prev_independent (the dependent-segment copy path). Invariants, each an abort when violated: only
// ovt::ParseError escapes; the builder always finishes into a tree; two parses of the same bytes give the same JSON
// (determinism, §0.7.5).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::SyntaxNodeBuffer;
namespace h265 = ovt::syntax::h265;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

template <class T, class Parser>
std::optional<T> parse_ps(std::span<const uint8_t> nal, Parser parser) {
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  std::optional<T> out;
  try {
    (void)h265::parse_nal_header(br, b);
    out = parser(br, b);
  } catch (const ovt::ParseError&) {
    out.reset();
  }
  (void)b.finish();
  return out;
}

struct SliceResult {
  std::optional<h265::SliceSegmentHeader> sh;
  std::string json;
};

SliceResult parse_slice(const ovt::Rbsp& rbsp, const h265::PpsTable& table, const h265::SliceSegmentHeader* prev) {
  SliceResult out;
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  try {
    const h265::NalHeader h = h265::parse_nal_header(br, b);
    out.sh = h265::parse_slice_segment_header(br, b, h, table, prev);
  } catch (const ovt::ParseError&) {
    out.sh.reset();
  }
  SyntaxNodeBuffer tree = b.finish();
  out.json = ovt::syntax::to_json(tree);
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
  const auto sps = parse_ps<h265::Sps>(all.subspan(2, sps_len),
                                       [](BitReader& br, SyntaxBuilder& b) { return h265::parse_sps(br, b); });
  const auto pps = parse_ps<h265::Pps>(all.subspan(2 + sps_len, pps_len),
                                       [](BitReader& br, SyntaxBuilder& b) { return h265::parse_pps(br, b); });
  if (!sps || !pps) {
    return 0;
  }
  const h265::ActivePps active = h265::activate_pps(*pps, *sps);
  h265::PpsTable table;
  table.entries[pps->pps_pic_parameter_set_id] = &active;
  const std::span<const uint8_t> nal = all.subspan(2 + sps_len + pps_len);
  if (nal.size() < 2) {
    return 0;
  }
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  const SliceResult first = parse_slice(rbsp, table, nullptr);
  const SliceResult again = parse_slice(rbsp, table, nullptr);
  require(first.sh.has_value() == again.sh.has_value() && first.json == again.json);
  if (!first.sh) {
    return 0;
  }
  if (!first.sh->dependent_slice_segment_flag) {
    (void)parse_slice(rbsp, table, &*first.sh);
  }
  return 0;
}
