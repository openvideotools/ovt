// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Cell-grid and chroma coordinate helpers (chapter 06 §6.2.1, §6.2.2, WI-0.084). Every consumer of a
// FrameAnalysis places cells and derives chroma rectangles through these functions, so the coordinate rules
// are implemented once.
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.

#ifndef OVT_CXX_MODEL_GRID_HPP
#define OVT_CXX_MODEL_GRID_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "ovt_model_generated.h"

namespace ovt::model {

/// Side of one grid cell in luma samples (§6.2.1 rule 4).
inline constexpr std::uint32_t kCellSize = 4;

/// Position of a 4×4 luma cell in CellGrid units (cell column x4, cell row y4), coded-picture coordinates.
struct CellIndex {
  std::uint32_t x4;
  std::uint32_t y4;
  friend constexpr bool operator==(CellIndex, CellIndex) noexcept = default;
};

/// Chroma subsampling factors of one `chroma_format` (§6.2.2 table). For 4:0:0 there are no chroma planes and
/// both factors are 0.
struct ChromaSubsampling {
  std::uint8_t sub_width_c;
  std::uint8_t sub_height_c;
};

/// §6.2.2, indexed by `FrameInfo.chroma_format` 0..3 (4:0:0, 4:2:0, 4:2:2, 4:4:4).
inline constexpr std::array<ChromaSubsampling, 4> kChromaSubsampling{{{0, 0}, {2, 2}, {2, 1}, {1, 1}}};

/// Number of cells that cover @p luma_samples samples: ceil(luma_samples / 4) (§6.2.1 rule 4).
[[nodiscard]] constexpr std::uint32_t cells_for(std::uint32_t luma_samples) noexcept {
  return luma_samples / kCellSize + (luma_samples % kCellSize != 0 ? 1U : 0U);
}

/// Row-major index of @p c into the CellGrid vectors: `y4 * cells_w + x4`.
/// Throws std::out_of_range when the cell lies outside `cells_w × cells_h` or the index does not fit 32 bits.
[[nodiscard]] inline std::uint32_t cell_index(const fb::CellGrid& grid, CellIndex c) {
  if (c.x4 >= grid.cells_w() || c.y4 >= grid.cells_h()) {
    throw std::out_of_range("ovt::model::cell_index: cell outside the grid");
  }
  const std::uint64_t index = (std::uint64_t{c.y4} * grid.cells_w()) + c.x4;
  if (index > std::numeric_limits<std::uint32_t>::max()) {
    throw std::out_of_range("ovt::model::cell_index: index does not fit 32 bits");
  }
  return static_cast<std::uint32_t>(index);
}

/// The cell that contains the coded-picture luma sample (@p luma_x, @p luma_y).
/// Throws std::out_of_range for a negative coordinate or a sample outside `cells_w × cells_h` cells.
[[nodiscard]] CellIndex cell_at(const fb::CellGrid& grid, int luma_x, int luma_y);

/// The chroma rectangle (in chroma samples of one chroma plane) that luma rectangle @p luma covers (§6.2.2):
/// `(x / SubWidthC, y / SubHeightC, w / SubWidthC, h / SubHeightC)`, positions rounded towards minus infinity.
/// 4:0:0 has no chroma and yields the empty rectangle (0, 0, 0, 0). Throws std::invalid_argument when
/// @p chroma_format is not 0..3.
[[nodiscard]] fb::Rect chroma_rect(fb::Rect luma, std::uint8_t chroma_format);

/// Size of one chroma plane of a coded `width × height` picture (§6.2.2 last column): ceil division by the
/// subsampling factors; (0, 0) for 4:0:0. Throws std::invalid_argument when @p chroma_format is not 0..3.
struct PlaneSize {
  std::uint32_t width;
  std::uint32_t height;
  friend constexpr bool operator==(PlaneSize, PlaneSize) noexcept = default;
};
[[nodiscard]] PlaneSize chroma_plane_size(std::uint32_t width, std::uint32_t height, std::uint8_t chroma_format);

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_GRID_HPP
