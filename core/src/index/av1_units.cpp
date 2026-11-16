// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 dispatch of the StreamIndex builder (chapter 7 §7.4.2 S3–S6 and S8, WI-1.121): one OBU at a time through
// the §7.3.6 parsers, the frame grouper of §7.3.9 and, when a frame ends, the slot step of §7.4.3.

#include <fmt/format.h>

#include <limits>
#include <optional>
#include <utility>

#include "index/av1_frame_boundary.h"
#include "index/av1_slots.h"
#include "index/builder_internal.h"
#include "index/frame_classify.h"
#include "ovt/cxx/syntax/av1/frame_header.hpp"
#include "ovt/cxx/syntax/av1/metadata.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/av1/tile_list.hpp"

namespace ovt::index {

namespace {

using namespace syntax::av1;  // NOLINT(google-build-using-namespace): the §7.3.6 parsers

constexpr std::string_view kDiagNoParameterSets = "SYN-GEN-0102";  // Warning, §7.4.7

// timing_info() as the elementary-stream time base of chapter 6 §6.2.6: pts = display_index *
// (num_ticks_per_picture_minus_1 + 1) with equal_picture_interval, else 1/1.
std::optional<StreamTiming> stream_timing(const SequenceHeader& seq) {
  if (!seq.timing_info_present_flag) {
    return std::nullopt;
  }
  const TimingInfo& t = seq.timing_info;
  constexpr auto kMax = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  if (!t.equal_picture_interval || t.num_units_in_display_tick == 0 || t.time_scale == 0 ||
      t.num_units_in_display_tick > kMax || t.time_scale > kMax) {
    return StreamTiming{};
  }
  return StreamTiming{static_cast<int32_t>(t.num_units_in_display_tick), static_cast<int32_t>(t.time_scale),
                      int64_t{t.num_ticks_per_picture_minus_1} + 1};
}

class Av1Dispatch final : public CodecDispatch {
 public:
  void on_unit(BuilderCore& core, uint32_t unit, std::span<const uint8_t> bytes) override {
    BitReader br(bytes);
    syntax::SyntaxBuilder sb(br);
    ObuEvent e;
    std::optional<FrameHeader> frame_header;
    bool parsed = false;
    try {
      parsed = parse(core, unit, br, sb, e, frame_header);
    } catch (const ParseError& err) {
      core.unit_diagnostic(unit, diag_from(err));
    }
    core.units[unit].header = sb.finish();
    core.units[unit].header.set_root_name("open_bitstream_unit");
    if (parsed) {
      place(core, unit, e, std::move(frame_header));  // S5
    }
    // A unit whose header did not parse belongs to no frame (S4 "continue with next unit").
  }

  void end_of_stream(BuilderCore& core) override {
    bool truncated = false;
    if (grouper_.end_of_stream(truncated, frame_diag(core))) {
      close_frame(core);
    }
  }

 private:
  // S3 and S4. Returns false when the OBU cannot be placed (a frame header before any sequence header).
  bool parse(BuilderCore& core, uint32_t unit, BitReader& br, syntax::SyntaxBuilder& sb, ObuEvent& e,
             std::optional<FrameHeader>& frame_header) {
    UnitRec& u = core.units[unit];
    e.header = parse_obu_header(br, sb);
    u.type = e.header.obu_type;
    u.type_name = e.header.type_name;
    u.layer_id = e.header.spatial_id;
    u.temporal_id = e.header.temporal_id;
    switch (e.header.type()) {
      case ObuType::SequenceHeader:
        u.parameter_set = true;
        seq_ = parse_sequence_header(br, sb);
        slots_.set_sequence(seq_->enable_order_hint, seq_->OrderHintBits);
        if (!core.timing) {
          core.timing = stream_timing(*seq_);
        }
        return true;
      case ObuType::TemporalDelimiter:
        parse_temporal_delimiter(br, sb);
        return true;
      case ObuType::Metadata:
        u.sei = true;
        (void)parse_metadata(br, sb);
        return true;
      case ObuType::Padding:
        (void)parse_padding(br, sb);
        return true;
      case ObuType::TileList:
        (void)parse_tile_list(br, sb);
        return true;
      case ObuType::FrameHeader:
      case ObuType::Frame:
        if (!have_sequence(core) || !seq_) {
          return false;
        }
        if (e.header.type() == ObuType::Frame) {
          FrameObu fo = parse_frame_obu(br, sb, e.header, *seq_, slots_.state());
          if (!fo.tile_group.tiles.empty()) {
            e.tg_end = fo.tile_group.tg_end;
          }
          frame_header = std::move(fo.frame_header);
        } else {
          frame_header = parse_frame_header(br, sb, e.header, *seq_, slots_.state(), false);
        }
        if (const FrameHeader* fh = frame_header ? &*frame_header : nullptr; fh != nullptr) {
          e.frame_header =
              FrameHeaderPeek{fh->show_existing_frame, fh->frame_to_show_map_idx, fh->frame_type, fh->show_frame};
          if (!fh->show_existing_frame) {
            e.num_tiles = fh->tile_info.NumTiles;
          }
        }
        return true;
      case ObuType::RedundantFrameHeader:
        if (seq_) {
          (void)parse_frame_header(br, sb, e.header, *seq_, slots_.state(), true);
        }
        return true;
      case ObuType::TileGroup:
        if (open_header_ && grouper_.frame_open()) {
          const TileGroup tg =
              parse_tile_group_obu(br, sb, open_header_->tile_info, static_cast<uint32_t>(br.bits_left() / 8));
          e.tg_end = tg.tg_end;
          e.num_tiles = open_header_->tile_info.NumTiles;
        }
        return true;
      default:
        return true;  // reserved types stay opaque
    }
  }

  bool have_sequence(BuilderCore& core) {
    if (seq_) {
      return true;
    }
    if (!warned_no_sequence_) {
      // SPEC-ASSUMPTION(#641): §7.9 words SYN-GEN-0102 for H.26x; an AV1 frame before the first sequence header
      // cannot be parsed at all, so it is reported once with the same code and its OBUs stay outside any frame.
      warned_no_sequence_ = true;
      core.index_diagnostic(syntax::Severity::Warning, kDiagNoParameterSets, "no parameter sets before first VCL unit");
    }
    return false;
  }

  // S5 for AV1: the grouper decides, the core keeps the books.
  void place(BuilderCore& core, uint32_t unit, const ObuEvent& e, std::optional<FrameHeader> frame_header) {
    const ObuPlacement p = grouper_.on_obu(e, [&core, unit](const IndexDiagnostic& d) { report(core, unit, d); });
    if (p.closes_previous_frame) {
      close_frame(core);
    }
    if (p.starts_frame) {
      FrameRec& f = core.start_frame(unit);
      f.temporal_id = e.header.temporal_id;
      f.layer_id = e.header.spatial_id;
      f.slice_count = e.tg_end ? 1 : 0;
      open_header_ = std::move(frame_header);
      open_unit_ = unit;
    } else if (p.in_frame && core.frame_open()) {
      core.extend_frame(unit);
      if (e.header.type() == ObuType::TileGroup) {
        if (FrameRec* open = core.open_frame(); open != nullptr) {
          ++open->slice_count;
        }
      }
    } else if (p.pending_non_vcl || p.temporal_delimiter) {
      core.add_pending(unit);
    }
    if (p.completes_frame) {
      close_frame(core);
    }
  }

  // S6 and S7: the reference update of the frame that just ended, its classification, and the core's finish.
  void close_frame(BuilderCore& core) {
    FrameRec* const open = core.open_frame();
    if (open == nullptr || !open_header_ || !seq_) {
      core.finish_frame();
      open_header_.reset();
      return;
    }
    const FrameHeader& fh = *open_header_;
    Av1FrameHeaderValues h;
    h.show_existing_frame = fh.show_existing_frame;
    h.frame_to_show_map_idx = fh.frame_to_show_map_idx;
    h.show_frame = fh.show_frame;
    h.refresh_frame_flags = fh.refresh_frame_flags;
    h.ref_frame_idx = fh.ref_frame_idx;
    h.base_q_idx = fh.quantization_params.base_q_idx;
    h.state = fh.state;
    FrameRec& f = *open;
    const uint32_t unit = open_unit_;
    const Av1FrameRecord r = slots_.step(
        h, f.decode_index,
        [&core](int64_t decode_index) {
          return decode_index >= 0 && static_cast<size_t>(decode_index) < core.frames.size()
                     ? core.frames[static_cast<size_t>(decode_index)].qp_base
                     : int16_t{0};
        },
        [&core, unit](const IndexDiagnostic& d) { report(core, unit, d); });
    classify_av1(f, r, *seq_, fh.current_frame_id);
    core.finish_frame();
    open_header_.reset();
  }

  // A finding of the grouper or the slot model. SYN-AV1-0101 concerns the frame that was open, the others the
  // unit being processed (or, at a frame's end, the unit that started it).
  static void report(BuilderCore& core, uint32_t unit, const IndexDiagnostic& d) {
    if (d.code == kDiagTruncatedFrame) {
      const FrameRec* open = core.open_frame();
      const int32_t frame = open != nullptr ? open->decode_index : -1;
      core.index_diagnostic(static_cast<syntax::Severity>(d.severity), d.code,
                            fmt::format("frame {}: {}", frame, d.message));
      return;
    }
    core.unit_diagnostic(
        unit, DiagRec{static_cast<syntax::Severity>(d.severity), std::string(d.code), d.message, BitSpan{0, 0}});
  }

  IndexDiagnosticCallback frame_diag(BuilderCore& core) {
    return [&core, this](const IndexDiagnostic& d) { report(core, open_unit_, d); };
  }

  // The Annex B temporal_unit_size check (SYN-AV1-0032) needs the scanner to say where temporal units start; it
  // arrives with WI-1.124 ("Annex B temporal unit checks").
  Av1FrameGrouper grouper_{false};
  Av1SlotModel slots_;
  std::optional<SequenceHeader> seq_;
  std::optional<FrameHeader> open_header_;  // the open frame's header: tile_info for its tile groups, S6 at its end
  uint32_t open_unit_ = 0;                  // the unit that started the open frame
  bool warned_no_sequence_ = false;
};

}  // namespace

std::unique_ptr<CodecDispatch> make_av1_dispatch() {
  return std::make_unique<Av1Dispatch>();
}

}  // namespace ovt::index
