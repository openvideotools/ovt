// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the AV1 frame grouper, the frame-header peek and the slot model (chapter 7 §7.3.9, §7.3.10.4,
// §7.4.3/§7.4.4; chapter 14 §14.7.1; WI-1.119). The input is a Section 5 OBU stream: the unit scanner splits it,
// every OBU header is parsed, a sequence header is kept, frame headers / frame OBUs are peeked and fed to the
// grouper, and every frame drives one slot-model step whose refresh_frame_flags / ref_frame_idx / order hint come
// from the bytes after the peeked fields (the full header parser is WI-1.117). A ParseError is the expected answer
// to malformed bytes; any other exception, a crash or a sanitizer report is a finding. Invariants: dpb_after
// entries are -1 or an earlier decode index; declared_refs are earlier decode indices; counters are consistent.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>

#include "index/av1_frame_boundary.h"
#include "index/av1_slots.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using namespace ovt::syntax::av1;  // NOLINT(google-build-using-namespace)
using ovt::index::Av1FrameGrouper;
using ovt::index::Av1FrameHeaderValues;
using ovt::index::Av1FrameRecord;
using ovt::index::Av1SlotModel;
using ovt::index::FrameHeaderPeek;
using ovt::index::ObuEvent;
using ovt::index::ObuPlacement;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

struct Driver {
  Av1FrameGrouper grouper{false};
  Av1SlotModel slots;
  std::optional<SequenceHeader> seq;
  int64_t decode_index = 0;
  uint64_t diagnostics = 0;

  void step_slots(const FrameHeaderPeek& peek, ovt::BitReader& br) {
    Av1FrameHeaderValues h;
    h.show_existing_frame = peek.show_existing_frame;
    h.frame_to_show_map_idx = peek.frame_to_show_map_idx;
    h.show_frame = peek.show_frame;
    h.state.frame_type = peek.frame_type;
    h.state.showable_frame = !peek.show_frame;
    // Fabricated header values from the bytes that follow the peeked fields.
    auto next = [&br](unsigned n) -> uint64_t { return br.bits_left() >= n ? br.u(n) : 0; };
    h.refresh_frame_flags = static_cast<uint8_t>(next(8));
    h.state.OrderHint = static_cast<uint32_t>(next(slots.order_hint_bits()));
    const bool short_signaling = next(1) != 0;
    if (short_signaling && slots.enable_order_hint()) {
      const auto last = static_cast<uint8_t>(next(3));
      const auto gold = static_cast<uint8_t>(next(3));
      h.ref_frame_idx = set_frame_refs(slots.state(), h.state.OrderHint, last, gold, slots.enable_order_hint(),
                                       slots.order_hint_bits(), [this](const StateDiagnostic&) { ++diagnostics; });
    } else {
      for (auto& idx : h.ref_frame_idx) {
        idx = static_cast<int>(next(3));
      }
    }
    h.base_q_idx = static_cast<int16_t>(next(8));
    h.in_operating_point = next(1) != 0;
    const Av1FrameRecord r = slots.step(
        h, decode_index, [](int64_t) { return int16_t{0}; },
        [this](const ovt::index::IndexDiagnostic&) { ++diagnostics; });
    for (const int64_t d : r.dpb_after) {
      require(d == -1 || (d >= 0 && d <= decode_index));
    }
    for (const int64_t d : r.declared_refs) {
      require(d >= 0 && d < decode_index);
    }
    require(r.show_existing_of == -1 || (r.show_existing_of >= 0 && r.show_existing_of < decode_index));
    ++decode_index;
  }

  void on_unit(std::span<const uint8_t> unit) {
    ovt::BitReader br(unit);
    ovt::syntax::SyntaxBuilder b(br);
    b.set_recording(false);
    ObuEvent e;
    try {
      e.header = parse_obu_header(br, b);
      switch (e.header.type()) {
        case ObuType::SequenceHeader: {
          seq = parse_sequence_header(br, b);
          slots.set_sequence(seq->enable_order_hint, seq->OrderHintBits);
          break;
        }
        case ObuType::FrameHeader:
        case ObuType::Frame: {
          if (!seq) {
            return;  // no sequence header yet: the builder skips the frame (SYN-GEN-0102 analogue)
          }
          const FrameHeaderPeek peek = ovt::index::peek_frame_header(br, *seq);
          e.frame_header = peek;
          if (br.bits_left() >= 8) {
            const auto byte = static_cast<uint8_t>(br.peek_bits(8));
            if ((byte & 0x80U) != 0) {
              e.num_tiles = ((byte >> 4U) & 7U) + 1;
            }
          }
          const ObuPlacement p = grouper.on_obu(e, [this](const ovt::index::IndexDiagnostic&) { ++diagnostics; });
          require(p.starts_frame && p.in_frame);
          step_slots(peek, br);
          return;
        }
        case ObuType::TileGroup: {
          if (br.bits_left() >= 8) {
            const auto byte = static_cast<uint8_t>(br.u(8));
            e.tg_end = byte & 7U;
            if ((byte & 0x80U) != 0) {
              e.num_tiles = ((byte >> 4U) & 7U) + 1;
            }
          }
          break;
        }
        default:
          break;
      }
    } catch (const ovt::ParseError&) {  // NOLINT(bugprone-empty-catch): the expected answer to malformed input
      return;
    }
    (void)grouper.on_obu(e, [this](const ovt::index::IndexDiagnostic&) { ++diagnostics; });
    (void)b.finish();
  }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  Driver d;
  ovt::syntax::scan_obu_low_overhead(
      std::span<const uint8_t>(data, size), 0, ovt::syntax::kNoContainerSample,
      ovt::syntax::ObuSpanKind::Section5Stream, [&d](const ovt::syntax::ScannedUnit& u) { d.on_unit(u.bytes); },
      [](const ovt::syntax::ScanDiagnostic&) {});
  bool truncated = false;
  (void)d.grouper.end_of_stream(truncated);
  require(d.grouper.frames_completed() == d.grouper.frames_started());
  require(d.grouper.shown_frames() <= d.grouper.frames_started());
  require(static_cast<uint64_t>(d.decode_index) == d.grouper.frames_started());
  return 0;
}
