// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the AV1 OBU header, metadata, tile list, padding and temporal delimiter parsers (chapter 7
// §7.3.6 rows 1, 7, 8, 9 and 30, chapter 14 §14.7.1, WI-1.116a; WI-1.116b adds the sequence header). The input is one
// whole OBU: the header is parsed first and the payload parser is chosen by obu_type (an unknown type parses every
// payload parser in turn on a fresh reader, so a header fuzzed into a reserved type still exercises them). Both
// recording modes run and must return the same typed values. A ParseError is the expected answer to malformed
// bytes; any other exception, a crash or a sanitizer report is a finding. Views returned by the parsers must
// lie inside the input.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/metadata.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/tile_list.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

bool inside(std::span<const uint8_t> outer, std::span<const uint8_t> inner) {
  return inner.empty() || (inner.data() >= outer.data() && inner.data() + inner.size() <= outer.data() + outer.size());
}

// Runs the payload parser `type` after the header; returns a value that summarises the typed result so the two
// recording modes can be compared.
uint64_t run(std::span<const uint8_t> input, ObuType type, bool recording) {
  ovt::BitReader br(input);
  ovt::syntax::SyntaxBuilder b(br);
  b.set_recording(recording);
  uint64_t summary = 0;
  try {
    const ObuHeader h = parse_obu_header(br, b);
    summary = h.obu_size ^ (static_cast<uint64_t>(h.obu_type) << 56U);
    (void)check_operating_point(b, h, 0x0101);
    switch (type) {
      case ObuType::Metadata: {
        const Metadata m = parse_metadata(br, b);
        summary ^= m.metadata_type;
        require(inside(input, m.unknown_payload));
        if (m.itut_t35) {
          require(inside(input, m.itut_t35->t35_message));
        }
        ovt_ColorDescription cd{};
        apply_metadata(m, cd);
        break;
      }
      case ObuType::TileList: {
        const TileList t = parse_tile_list(br, b);
        summary ^= t.entries.size();
        break;
      }
      case ObuType::Padding:
        summary ^= parse_padding(br, b).byte_count;
        break;
      case ObuType::TemporalDelimiter:
        parse_temporal_delimiter(br, b);
        break;
      default:
        break;
    }
  } catch (const ovt::ParseError&) {  // NOLINT(bugprone-empty-catch): the expected answer to malformed input
    summary = ~uint64_t{0};
  }
  const ovt::syntax::SyntaxNodeBuffer buf = b.finish();
  if (recording) {
    (void)buf.to_flatbuffer();
    (void)ovt::syntax::to_json(buf);
  }
  return summary;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::span<const uint8_t> input(data, size);
  const auto type = static_cast<ObuType>((data[0] >> 3U) & 0x0FU);
  switch (type) {
    case ObuType::Metadata:
    case ObuType::TileList:
    case ObuType::Padding:
    case ObuType::TemporalDelimiter:
      require(run(input, type, true) == run(input, type, false));
      break;
    default:
      for (const ObuType t : {ObuType::Metadata, ObuType::TileList}) {
        require(run(input, t, true) == run(input, t, false));
      }
      break;
  }
  return 0;
}
