// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 frame and temporal-unit grouping (chapter 7 §7.3.9, WI-1.119). The rules, from AV1 §7.5 "Ordering of OBUs":
// a frame is one OBU_FRAME, or one OBU_FRAME_HEADER with show_existing_frame, or an OBU_FRAME_HEADER followed by
// tile groups until tg_end == NumTiles - 1; a temporal unit starts at each OBU_TEMPORAL_DELIMITER and holds
// exactly one shown frame per spatial layer.

#include "index/av1_frame_boundary.h"

#include <fmt/format.h>

#include <cstdint>
#include <optional>

namespace ovt::index {

namespace {

using syntax::av1::FrameType;
using syntax::av1::ObuType;

void report(const IndexDiagnosticCallback& diag, Severity sev, std::string_view code, std::string message) {
  if (diag) {
    diag(IndexDiagnostic{sev, code, std::move(message)});
  }
}

}  // namespace

FrameHeaderPeek peek_frame_header(BitReader& br, const syntax::av1::SequenceHeader& seq) {
  FrameHeaderPeek p;
  if (seq.reduced_still_picture_header) {
    // AV1 §5.9.2: show_existing_frame = 0, frame_type = KEY_FRAME, FrameIsIntra = 1, show_frame = 1.
    return p;
  }
  p.show_existing_frame = br.u(1) != 0;
  if (p.show_existing_frame) {
    p.frame_to_show_map_idx = static_cast<uint8_t>(br.u(3));
    // frame_type / show_frame are those of the shown slot: the slot model resolves them (§7.4.3).
    p.show_frame = true;
    return p;
  }
  p.frame_type = static_cast<FrameType>(br.u(2));
  p.show_frame = br.u(1) != 0;
  return p;
}

// ---- Av1FrameGrouper -------------------------------------------------------------------------------------------

void Av1FrameGrouper::start_frame(const FrameHeaderPeek& h, uint8_t spatial_id, ObuPlacement& p) {
  p.starts_frame = true;
  p.in_frame = true;
  p.frame_shown = h.show_existing_frame || h.show_frame;
  ++frames_started_;
  tu_has_frames_ = true;
  const auto layer = static_cast<size_t>(spatial_id & 3U);
  tu_spatial_seen_[layer] = true;
  if (p.frame_shown) {
    ++shown_frames_;
    ++tu_shown_per_spatial_[layer];
  }
}

bool Av1FrameGrouper::close_open_frame(ObuPlacement& p, const IndexDiagnosticCallback& diag) {
  if (!seen_frame_header_) {
    return false;
  }
  // The frame is complete when its tile count is unknown (no header parser yet) or TileNum reached NumTiles;
  // otherwise it is truncated (§7.4.2 S8).
  const bool truncated = num_tiles_.has_value() && tile_num_ < *num_tiles_;
  if (truncated) {
    report(diag, Severity::Error, kDiagTruncatedFrame,
           fmt::format("truncated frame: {} of {} tiles", tile_num_, *num_tiles_));
  }
  seen_frame_header_ = false;
  tile_num_ = 0;
  num_tiles_.reset();
  ++frames_completed_;
  p.closes_previous_frame = true;
  p.previous_truncated = truncated;
  return true;
}

void Av1FrameGrouper::end_temporal_unit(const IndexDiagnosticCallback& diag) {
  if (!in_temporal_unit_) {
    return;
  }
  // AV1 §7.5: exactly one shown frame per spatial layer. A temporal unit without any frame (e.g. a trailing
  // temporal delimiter) is not reported here: the builder's SYN-GEN-0101 / unit accounting covers empty streams.
  if (tu_has_frames_) {
    for (size_t layer = 0; layer < tu_shown_per_spatial_.size(); ++layer) {
      if (!tu_spatial_seen_[layer]) {
        continue;
      }
      if (tu_shown_per_spatial_[layer] != 1) {
        report(diag, Severity::Warning, kDiagTemporalUnitShownFrames,
               fmt::format("temporal unit with {} shown frames (expected 1) in spatial layer {}",
                           tu_shown_per_spatial_[layer], layer));
      }
    }
  }
  in_temporal_unit_ = false;
  tu_has_frames_ = false;
  tu_shown_per_spatial_.fill(0);
  tu_spatial_seen_.fill(false);
}

void Av1FrameGrouper::on_tile_group(const ObuEvent& e, ObuPlacement& p, const IndexDiagnosticCallback& diag) {
  if (!seen_frame_header_) {
    report(diag, Severity::Error, kDiagTileGroupWithoutHeader, "tile group without frame header");
    return;  // belongs to no frame; the builder attaches the OBU to a synthetic frame or drops it
  }
  p.in_frame = true;
  if (e.num_tiles) {
    num_tiles_ = e.num_tiles;
  }
  if (e.tg_end) {
    tile_num_ = *e.tg_end + 1;  // TileNum after this group: tiles tg_start..tg_end were consumed
  }
  if (num_tiles_ && tile_num_ >= *num_tiles_) {
    // §7.3.9 (c): tg_end == NumTiles - 1 ends the frame.
    seen_frame_header_ = false;
    tile_num_ = 0;
    num_tiles_.reset();
    ++frames_completed_;
    p.completes_frame = true;
  }
}

ObuPlacement Av1FrameGrouper::on_obu(const ObuEvent& e, const IndexDiagnosticCallback& diag) {
  ObuPlacement p;
  const ObuType type = e.header.type();

  if (annexb_) {
    const bool is_td = type == ObuType::TemporalDelimiter;
    if (e.annexb_temporal_unit_start != is_td) {
      report(diag, Severity::Warning, kDiagAnnexBTemporalUnit,
             is_td ? "Annex B temporal_unit_size disagrees with temporal delimiters: temporal delimiter inside a "
                     "temporal unit"
                   : fmt::format("Annex B temporal_unit_size disagrees with temporal delimiters: temporal unit "
                                 "starts with {}",
                                 e.header.type_name));
    }
  }

  switch (type) {
    case ObuType::TemporalDelimiter:
      close_open_frame(p, diag);  // a frame never spans temporal units
      end_temporal_unit(diag);
      in_temporal_unit_ = true;
      ++temporal_units_;
      p.temporal_delimiter = true;
      return p;

    case ObuType::Frame:
    case ObuType::FrameHeader: {
      close_open_frame(p, diag);
      const FrameHeaderPeek h = e.frame_header.value_or(FrameHeaderPeek{});
      start_frame(h, e.header.spatial_id, p);
      if (h.show_existing_frame) {
        // §7.3.9 (b): complete by itself. No tile groups follow (AV1 §5.9.2 show_existing_frame).
        p.completes_frame = true;
        ++frames_completed_;
        return p;
      }
      if (type == ObuType::Frame) {
        // §7.3.9 (a): one OBU_FRAME carries the frame header and exactly one tile group (AV1 §5.10). With a known
        // tile count it may still be the first of several frames' worth only when tg_end < NumTiles - 1, which
        // AV1 §7.5 forbids for OBU_FRAME; treat it as complete.
        p.completes_frame = true;
        ++frames_completed_;
        return p;
      }
      seen_frame_header_ = true;
      tile_num_ = 0;
      num_tiles_ = e.num_tiles;
      return p;
    }

    case ObuType::TileGroup:
      on_tile_group(e, p, diag);
      return p;

    case ObuType::RedundantFrameHeader:
    case ObuType::Metadata:
    case ObuType::Padding:
      // Inside a frame they belong to it (§7.3.9 (c); AV1 §7.5 allows a redundant frame header only there);
      // before a frame they are pending non-VCL units.
      if (seen_frame_header_) {
        p.in_frame = true;
      } else {
        p.pending_non_vcl = true;
      }
      return p;

    case ObuType::SequenceHeader:
    case ObuType::TileList:
    case ObuType::Reserved0:
    default:
      // A sequence header or a tile list never sits inside a frame (AV1 §7.5): close an open frame as truncated
      // only if one is open with a known tile count; otherwise just mark the OBU pending.
      if (seen_frame_header_ && type == ObuType::SequenceHeader) {
        close_open_frame(p, diag);
      }
      p.pending_non_vcl = true;
      return p;
  }
}

bool Av1FrameGrouper::end_of_stream(bool& truncated, const IndexDiagnosticCallback& diag) {
  ObuPlacement p;
  const bool closed = close_open_frame(p, diag);
  truncated = p.previous_truncated;
  end_temporal_unit(diag);
  return closed;
}

}  // namespace ovt::index
