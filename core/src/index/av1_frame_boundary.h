// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 frame and temporal-unit boundaries for the index (chapter 7 §7.3.9 "AV1 (§7.5 Ordering of OBUs)",
// WI-1.119): the SeenFrameHeader / TileNum state that groups OBUs into frames, the one-shown-frame-per-spatial-
// layer check of a temporal unit (SYN-AV1-0030), the tile-group-without-header (SYN-AV1-0031), Annex B
// temporal_unit_size (SYN-AV1-0032) and truncated-frame (SYN-AV1-0101) findings. The StreamIndex builder
// (WI-1.121 S5, WI-1.124) feeds it one OBU at a time. Module-internal, not installed.
//
// Until the uncompressed_header() parser of WI-1.117 lands, peek_frame_header() reads the first elements of the
// frame header (show_existing_frame, frame_to_show_map_idx, frame_type, show_frame) so that frames can be
// grouped and counted on real streams; WI-1.117 replaces it with the full header and supplies NumTiles / tg_end.

#ifndef OVT_INDEX_AV1_FRAME_BOUNDARY_H
#define OVT_INDEX_AV1_FRAME_BOUNDARY_H

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "index/av1_slots.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/obu_header.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"

namespace ovt::index {

inline constexpr std::string_view kDiagTemporalUnitShownFrames = "SYN-AV1-0030";  // Warning
inline constexpr std::string_view kDiagTileGroupWithoutHeader = "SYN-AV1-0031";   // Error
inline constexpr std::string_view kDiagAnnexBTemporalUnit = "SYN-AV1-0032";       // Warning
inline constexpr std::string_view kDiagTruncatedFrame = "SYN-AV1-0101";           // Error

/// The leading elements of uncompressed_header() (AV1 §5.9.2) that decide how an OBU_FRAME_HEADER /
/// OBU_FRAME groups: show_existing_frame, frame_to_show_map_idx, frame_type and show_frame. With
/// reduced_still_picture_header the values are inferred (KEY, shown) and nothing is read.
struct FrameHeaderPeek {
  bool show_existing_frame = false;
  uint8_t frame_to_show_map_idx = 0;
  syntax::av1::FrameType frame_type = syntax::av1::FrameType::Key;
  bool show_frame = true;
};

/// Reads the FrameHeaderPeek at the current position (just after obu_header / obu_size, i.e. the first bit of
/// frame_header_obu()). Throws ParseError BR-0001 on a truncated OBU. Stop-gap for WI-1.117 (see the file comment).
[[nodiscard]] FrameHeaderPeek peek_frame_header(BitReader& br, const syntax::av1::SequenceHeader& seq);

/// One OBU as the builder hands it to the grouper: the parsed header, and for frame headers / frame OBUs the
/// peek (or the equivalent fields of the full header). For a tile group `tg_end` and `num_tiles` close the frame
/// when tg_end == NumTiles - 1 (§7.3.9 (c)); when the tile count is unknown (no full header parser yet) pass
/// nullopt and the frame closes at the next frame start, temporal delimiter or end of stream.
struct ObuEvent {
  syntax::av1::ObuHeader header;
  std::optional<FrameHeaderPeek> frame_header;  // required for OBU_FRAME_HEADER / OBU_FRAME; ignored otherwise
  std::optional<uint32_t> tg_end;               // OBU_TILE_GROUP / OBU_FRAME: tg_end of tile_group_obu()
  std::optional<uint32_t> num_tiles;            // NumTiles of the frame's tile_info()
  bool annexb_temporal_unit_start = false;      // Annex B: this OBU carried a temporal_unit_size prefix
};

/// What the grouper decided for one OBU (§7.4.2 S5): the builder opens / extends / closes a FrameInfo from it.
struct ObuPlacement {
  bool closes_previous_frame = false;  // an unfinished frame was closed before this OBU (truncated when
                                       // `previous_truncated`; the OBU starting a new frame or a TD arrived)
  bool previous_truncated = false;     // the closed frame missed tile groups (SYN-AV1-0101 was reported)
  bool starts_frame = false;           // this OBU is the first unit of a new frame
  bool in_frame = false;               // this OBU belongs to the open (or just started) frame
  bool completes_frame = false;        // this OBU completes the frame (OBU_FRAME, show_existing header, last TG)
  bool pending_non_vcl = false;        // metadata / padding / sequence header before a frame: builder keeps it in
                                       // pending_nonvcl for size accounting (§7.3.9)
  bool temporal_delimiter = false;     // OBU_TEMPORAL_DELIMITER: the builder resets pending state (S4 "AUD / TD")
  bool frame_shown = false;            // the frame this OBU starts is shown (show_frame or show_existing_frame)
};

/// Groups OBUs into frames and checks temporal units (§7.3.9). One instance per stream; feed every OBU in stream
/// order, then end_of_stream().
class Av1FrameGrouper {
 public:
  /// `annexb`: the stream is Annex B length-delimited, so temporal_unit_size prefixes must coincide with temporal
  /// delimiters (SYN-AV1-0032).
  explicit Av1FrameGrouper(bool annexb = false) noexcept : annexb_(annexb) {}

  ObuPlacement on_obu(const ObuEvent& e, const IndexDiagnosticCallback& diag = {});
  /// Closes an open frame (SYN-AV1-0101 when its tile groups were incomplete) and checks the last temporal unit.
  /// Returns true when a frame was closed; `truncated` tells whether it was incomplete.
  bool end_of_stream(bool& truncated, const IndexDiagnosticCallback& diag = {});

  // §7.3.9 state (read by tests and the builder).
  [[nodiscard]] bool SeenFrameHeader() const noexcept { return seen_frame_header_; }
  [[nodiscard]] uint32_t TileNum() const noexcept { return tile_num_; }
  [[nodiscard]] bool frame_open() const noexcept { return seen_frame_header_; }
  [[nodiscard]] uint64_t frames_started() const noexcept { return frames_started_; }
  [[nodiscard]] uint64_t frames_completed() const noexcept { return frames_completed_; }
  [[nodiscard]] uint64_t shown_frames() const noexcept { return shown_frames_; }
  [[nodiscard]] uint64_t temporal_units() const noexcept { return temporal_units_; }

 private:
  // Closes the open frame because a new frame, a temporal delimiter or the end of the stream arrived.
  bool close_open_frame(ObuPlacement& p, const IndexDiagnosticCallback& diag);
  void end_temporal_unit(const IndexDiagnosticCallback& diag);
  void start_frame(const FrameHeaderPeek& h, uint8_t spatial_id, ObuPlacement& p);
  void on_tile_group(const ObuEvent& e, ObuPlacement& p, const IndexDiagnosticCallback& diag);

  bool annexb_;
  bool seen_frame_header_ = false;  // SeenFrameHeader: a frame header was seen and its tile groups are not complete
  uint32_t tile_num_ = 0;           // TileNum: tiles consumed so far by the open frame
  std::optional<uint32_t> num_tiles_;
  bool in_temporal_unit_ = false;
  bool tu_has_frames_ = false;
  std::array<uint32_t, 4> tu_shown_per_spatial_{};  // spatial_id is f(2)
  std::array<bool, 4> tu_spatial_seen_{};
  uint64_t frames_started_ = 0;
  uint64_t frames_completed_ = 0;
  uint64_t shown_frames_ = 0;
  uint64_t temporal_units_ = 0;
};

}  // namespace ovt::index

#endif  // OVT_INDEX_AV1_FRAME_BOUNDARY_H
