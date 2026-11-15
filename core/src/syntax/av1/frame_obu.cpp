// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 frame_obu(sz) (chapter 7 §7.3.6 row 28; WI-1.118 part c): frame_header_obu(), byte_alignment() and the one
// tile_group_obu() of an OBU_FRAME.

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecFrameObu = "AV1 5.10";

}  // namespace

FrameObu parse_frame_obu(BitReader& br, SyntaxBuilder& b, const ObuHeader& obu, const SequenceHeader& seq,
                         DecoderState& state) {
  // AV1 §5.10
  FrameObu f;
  const auto start = br.mark();
  auto s = b.scoped("frame_obu", kSpecFrameObu);
  {
    // frame_header_obu() (§7.3.6 row 10): an OBU_FRAME starts a frame, so this is never frame_header_copy().
    auto fh = b.scoped("frame_header_obu", kSpecFrameObu);
    f.frame_header = parse_frame_header(br, b, obu, seq, state, false);
  }
  br.byte_alignment();
  f.headerBytes = static_cast<uint32_t>((br.bit_pos() - start) / 8);
  if (f.frame_header.show_existing_frame) {
    // AV1 §7.5: not allowed in an OBU_FRAME. No tile group follows a show_existing_frame header (AV1 §5.9.2).
    b.derived("tile_group_obu", "not parsed: show_existing_frame = 1 (AV1 7.5 forbids it in OBU_FRAME)");
    f.span = br.span_since(start);
    return f;
  }
  // sz - headerBytes; a reader over more than the OBU cannot make it negative or wider than 32 bits in practice,
  // and tile_group_obu() also checks every tile against the bytes the reader holds.
  const uint64_t sz = obu.obu_size;
  const uint64_t rest = sz > f.headerBytes ? sz - f.headerBytes : 0;
  const auto left = static_cast<uint32_t>(std::min<uint64_t>(rest, UINT32_MAX));
  f.tile_group = parse_tile_group_obu(br, b, f.frame_header.tile_info, left);
  f.span = br.span_since(start);
  return f;
}

}  // namespace ovt::syntax::av1
