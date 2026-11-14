// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 uncompressed_header() (AV1 §5.9.2) and its sub-structures: chapter 7 §7.3.6 rows 11–19, WI-1.117 (part 1:
// up to and including delta_lf_params()); rows 20–29 follow in WI-1.118. WI-1.117a delivers rows 12–19 (frame
// size, render size, frame_size_with_refs, tile_info, quantization, segmentation, delta q / lf and the lossless
// derivations) in two PRs: a = rows 12–15 (this file's first version), b = rows 16–19; WI-1.117c adds the
// FrameHeader struct and parse_frame_header() for row 11. C++ helper header
// (D22): semi-public, no ABI promise, never included from a C header, not installed.
//
// The typed structs hold every syntax element under its spec name and every derived variable (§7.3.7); the
// SyntaxNode tree is for display. Fields the syntax table infers instead of reading carry the inferred value.
// The parsers read bits only; the reference slot processes of AV1 §7.20 / §7.21 live in decoder_state.hpp and
// run in the index (§7.3.10.4). Members of each struct are grouped by size, largest first (clang-analyzer
// optin.performance.Padding); the comments give the syntax order.

#ifndef OVT_CXX_SYNTAX_AV1_FRAME_HEADER_HPP
#define OVT_CXX_SYNTAX_AV1_FRAME_HEADER_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/av1/decoder_state.hpp"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::av1 {

// AV1 §3 constants used by rows 12–15.
inline constexpr uint32_t kMiSize = 4;                   // MI_SIZE: luma samples per mode-info unit
inline constexpr uint32_t kMaxTileWidth = 4096;          // MAX_TILE_WIDTH
inline constexpr uint32_t kMaxTileArea = 4096U * 2304U;  // MAX_TILE_AREA
inline constexpr uint32_t kMaxTileCols = 64;             // MAX_TILE_COLS
inline constexpr uint32_t kMaxTileRows = 64;             // MAX_TILE_ROWS
inline constexpr uint8_t kSuperresNum = 8;               // SUPERRES_NUM
inline constexpr uint8_t kSuperresDenomMin = 9;          // SUPERRES_DENOM_MIN
inline constexpr uint8_t kSuperresDenomBits = 3;         // SUPERRES_DENOM_BITS
/// Diagnostic codes of rows 11–19 (§7.9). SYN-AV1-0015 is proposed for frame-header values that violate a
/// conformance requirement without being a reserved value (SPEC-ASSUMPTION(#513), see frame_size.cpp /
/// tile_info.cpp).
inline constexpr std::string_view kDiagRefOrderHintMismatch = "SYN-AV1-0011";  // Info
inline constexpr std::string_view kDiagFrameHeaderRange = "SYN-AV1-0015";      // Error

// ---- row 12: frame_size(), superres_params(), compute_image_size() ---------------------------------------------

struct FrameSize {
  BitSpan span{0, 0};
  // frame_width_minus_1 f(n) / frame_height_minus_1 f(n) when frame_size_override_flag, else the sequence maxima.
  uint32_t frame_width_minus_1 = 0;
  uint32_t frame_height_minus_1 = 0;
  // Derived (AV1 §5.9.5 / §5.9.8 / §5.9.9)
  uint32_t UpscaledWidth = 0;  // frame_width_minus_1 + 1 before superres downscaling
  uint32_t FrameWidth = 0;     // (UpscaledWidth * SUPERRES_NUM + SuperresDenom / 2) / SuperresDenom
  uint32_t FrameHeight = 0;
  uint32_t MiCols = 0;  // 2 * ((FrameWidth + 7) >> 3)
  uint32_t MiRows = 0;
  // superres_params()
  uint8_t coded_denom = 0;
  uint8_t SuperresDenom = kSuperresNum;  // coded_denom + SUPERRES_DENOM_MIN, or SUPERRES_NUM without superres
  bool use_superres = false;
  bool size_from_sequence_header = false;  // derived: frame_size_override_flag == 0 (nothing read for the size)
};

/// frame_size() with superres_params() and compute_image_size() at the current position (node "frame_size").
/// `frame_size_override_flag` selects the two f(n) reads (n = frame_width_bits_minus_1 + 1 …) or the sequence
/// maxima. A frame larger than max_frame_width/height is Diagnostic Error SYN-AV1-0015; the values stand.
[[nodiscard]] FrameSize parse_frame_size(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                         bool frame_size_override_flag);

/// superres_params() (AV1 §5.9.8) inside the current struct: use_superres (read only with enable_superres),
/// coded_denom, SuperresDenom; sets UpscaledWidth from FrameWidth and downscales FrameWidth.
void parse_superres_params(SyntaxBuilder& b, const SequenceHeader& seq, FrameSize& fs);

/// compute_image_size() (AV1 §5.9.9): MiCols / MiRows from FrameWidth / FrameHeight.
void compute_image_size(FrameSize& fs) noexcept;

// ---- row 13: render_size() ------------------------------------------------------------------------------------

struct RenderSize {
  BitSpan span{0, 0};
  uint32_t render_width_minus_1 = 0;  // f(16), only when render_and_frame_size_different
  uint32_t render_height_minus_1 = 0;
  // Derived
  uint32_t RenderWidth = 0;  // render_width_minus_1 + 1, or UpscaledWidth
  uint32_t RenderHeight = 0;
  bool render_and_frame_size_different = false;
};

/// render_size() at the current position (node "render_size"); the defaults come from `fs`.
[[nodiscard]] RenderSize parse_render_size(BitReader& br, SyntaxBuilder& b, const FrameSize& fs);

// ---- row 14: frame_size_with_refs() ---------------------------------------------------------------------------

struct FrameSizeWithRefs {
  BitSpan span{0, 0};
  std::array<bool, kRefsPerFrame> found_ref{};  // found_ref f(1) for i = 0 .. found_ref_index (the rest unread)
  int8_t found_ref_index = -1;                  // the i whose found_ref was 1, or -1 (frame_size() was read)
};

/// frame_size_with_refs() (AV1 §5.9.7) at the current position (node "frame_size_with_refs"). With a found
/// reference the sizes are copied from slot ref_frame_idx[i] of `state` (RefUpscaledWidth, RefFrameHeight,
/// RefRenderWidth/Height), then superres_params() and compute_image_size() run; otherwise frame_size() and
/// render_size() are read. `fs` and `rs` receive the result. A found reference to an empty slot (RefValid == 0)
/// is Diagnostic Error SYN-AV1-0015 and the stale slot values are used, as a decoder would.
[[nodiscard]] FrameSizeWithRefs parse_frame_size_with_refs(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq,
                                                           const DecoderState& state, const RefFrameIdx& ref_frame_idx,
                                                           bool frame_size_override_flag, FrameSize& fs,
                                                           RenderSize& rs);

// ---- row 15: tile_info() --------------------------------------------------------------------------------------

/// A tile rectangle in luma samples (MiColStarts[i] * MI_SIZE …; the last column / row ends at MiCols * MI_SIZE,
/// which may exceed FrameWidth by up to 7 samples).
struct TileRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct TileInfo {
  std::vector<uint32_t> MiColStarts;  // TileCols + 1 entries, the last one MiCols
  std::vector<uint32_t> MiRowStarts;  // TileRows + 1 entries, the last one MiRows
  // width_in_sbs_minus_1 ns(maxWidth) / height_in_sbs_minus_1 ns(maxHeight): one per tile column / row when
  // uniform_tile_spacing_flag == 0; empty otherwise.
  std::vector<uint32_t> width_in_sbs_minus_1;
  std::vector<uint32_t> height_in_sbs_minus_1;
  BitSpan span{0, 0};
  // Derived superblock geometry (AV1 §5.9.15 locals, kept for the tests and the tile group parser)
  uint32_t sbCols = 0;
  uint32_t sbRows = 0;
  uint32_t minLog2TileCols = 0;
  uint32_t maxLog2TileCols = 0;
  uint32_t maxLog2TileRows = 0;
  uint32_t minLog2Tiles = 0;
  uint32_t widestTileSb = 0;  // non-uniform spacing only
  // Derived tile counts
  uint32_t TileCols = 0;
  uint32_t TileRows = 0;
  uint32_t NumTiles = 0;  // TileCols * TileRows
  uint32_t context_update_tile_id = 0;  // f(TileRowsLog2 + TileColsLog2), or 0
  uint8_t TileColsLog2 = 0;
  uint8_t TileRowsLog2 = 0;
  uint8_t tile_size_bytes_minus_1 = 3;  // f(2), only when TileColsLog2 > 0 || TileRowsLog2 > 0 (3 otherwise)
  uint8_t TileSizeBytes = 4;            // tile_size_minus_1 width in tile_group_obu(); 4 when not read (AV1 §5.9.15)
  uint8_t sbShift = 4;                  // 5 with 128x128 superblocks
  bool uniform_tile_spacing_flag = false;

  /// The rectangle of tile (tileRow, tileCol) in luma samples; {0,0,0,0} when out of range.
  [[nodiscard]] TileRect tile_rect(uint32_t tileRow, uint32_t tileCol) const noexcept;
};

/// tile_log2(blkSize, target) (AV1 §5.9.15): the smallest k with (blkSize << k) >= target.
[[nodiscard]] uint32_t tile_log2(uint32_t blkSize, uint32_t target) noexcept;

/// tile_info() (AV1 §5.9.15) at the current position (node "tile_info") for a frame of MiCols × MiRows mode-info
/// units with the superblock size of `seq`. More than MAX_TILE_COLS columns or MAX_TILE_ROWS rows from non-uniform
/// spacing is Diagnostic Error SYN-AV1-0015 (a conformance requirement of AV1 §5.9.15); the starts are kept.
[[nodiscard]] TileInfo parse_tile_info(BitReader& br, SyntaxBuilder& b, const SequenceHeader& seq, uint32_t MiCols,
                                       uint32_t MiRows);

}  // namespace ovt::syntax::av1

#endif  // OVT_CXX_SYNTAX_AV1_FRAME_HEADER_HPP
