// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the H.265 access_unit_delimiter_rbsp(), end_of_seq_rbsp(), end_of_bitstream_rbsp() and
// filler_data_rbsp() parsers (chapter 7 §7.3.5 row 15, chapter 14 §14.7.1, WI-1.112 follow-up). The low two bits
// of the first input byte select the parser (0 AUD, 1 end of sequence, 2 end of bitstream, 3 filler data); the
// remaining bytes are the NAL unit (header included) and go through rbsp_from_nal, so emulation prevention is
// exercised too. Invariants, each an abort when violated: only ovt::ParseError escapes a parser; the builder always
// finishes into a tree; a second parse of the same bytes yields the same JSON (determinism, §0.7.5); a filler run
// never exceeds the unit.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/aud.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
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

Parsed parse_one(std::span<const uint8_t> nal, uint8_t sel) {
  Parsed out;
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(nal);
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  try {
    (void)h265::parse_nal_header(br, b);
    switch (sel & 3U) {
      case 0:
        (void)h265::parse_access_unit_delimiter(br, b);
        break;
      case 1:
        (void)h265::parse_end_of_seq(br, b);
        break;
      case 2:
        (void)h265::parse_end_of_bitstream(br, b);
        break;
      default: {
        const h265::FillerData f = h265::parse_filler_data(br, b);
        require(f.ff_byte_count <= rbsp.rbsp.size());
        break;
      }
    }
    out.ok = true;
  } catch (const ovt::ParseError&) {
    out.ok = false;
  }
  const SyntaxNodeBuffer tree = b.finish();
  out.json = ovt::syntax::to_json(tree);
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const std::span<const uint8_t> all(data, size);
  const std::span<const uint8_t> nal = all.subspan(1);
  const Parsed first = parse_one(nal, all[0]);
  const Parsed again = parse_one(nal, all[0]);
  require(first.ok == again.ok && first.json == again.json);
  return 0;
}
