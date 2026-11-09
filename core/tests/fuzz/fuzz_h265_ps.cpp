// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the H.265 header parsers (chapter 7 §7.3.5 rows 1–9, chapter 14 §14.7.1, WI-1.112). The low
// three bits of the first input byte select the structure parsed after nal_unit_header(): 0 profile_tier_level()
// (bits 3–5: max_num_sub_layers_minus1), 1 hrd_parameters() (bit 3: commonInfPresentFlag, bits 4–6: sub-layers);
// other values are reserved for the later parts of the work item and parse nothing.
// The remaining bytes are the NAL unit (header included) and go through rbsp_from_nal, so emulation prevention is
// exercised too. Invariants, each an abort when violated: only ovt::ParseError escapes a parser; the builder always
// finishes into a tree; a second parse of the same bytes yields the same JSON (determinism, §0.7.5).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/profile_tier_level.hpp"
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

struct Parsed {
  bool ok = false;
  std::string json;
};

void parse_structure(BitReader& br, SyntaxBuilder& b, uint8_t sel) {
  switch (sel & 7U) {
    case 0:
      (void)h265::parse_profile_tier_level(br, b, true, (sel >> 3) & 7U);
      break;
    case 1:
      (void)h265::parse_hrd_parameters(br, b, (sel & 8U) != 0, (sel >> 4) & 7U);
      break;
    default:
      break;
  }
}

// Parses one NAL unit as the selected structure. Any exception other than ParseError aborts.
Parsed parse_one(std::span<const uint8_t> nal, uint8_t sel) {
  Parsed out;
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  try {
    (void)h265::parse_nal_header(br, b);
    parse_structure(br, b, sel);
    out.ok = true;
  } catch (const ovt::ParseError&) {
    out.ok = false;
  }
  SyntaxNodeBuffer tree = b.finish();
  out.json = ovt::syntax::to_json(tree);
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const std::span<const uint8_t> all(data, size);
  const uint8_t sel = all[0];
  const std::span<const uint8_t> nal = all.subspan(1);
  const Parsed first = parse_one(nal, sel);
  const Parsed again = parse_one(nal, sel);
  require(first.ok == again.ok && first.json == again.json);
  return 0;
}
