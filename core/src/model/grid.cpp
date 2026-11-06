// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Cell-grid and chroma coordinate helpers (chapter 06 §6.2.1, §6.2.2, WI-0.084).

#include "ovt/cxx/model/grid.hpp"

#include <cstdint>
#include <stdexcept>

namespace ovt::model {

namespace {

const ChromaSubsampling& subsampling(std::uint8_t chroma_format) {
  if (chroma_format >= kChromaSubsampling.size()) {
    throw std::invalid_argument("ovt::model: chroma_format must be 0..3");
  }
  return kChromaSubsampling.at(chroma_format);
}

// Floor division for a positive divisor (C++ division truncates towards zero).
constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept {
  const std::int32_t quotient = value / divisor;
  return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

}  // namespace

CellIndex cell_at(const fb::CellGrid& grid, int luma_x, int luma_y) {
  // Negative samples are invalid (WI-0.084 notes: floor division would give a negative cell).
  if (luma_x < 0 || luma_y < 0) {
    throw std::out_of_range("ovt::model::cell_at: negative luma coordinate");
  }
  const CellIndex cell{static_cast<std::uint32_t>(luma_x) / kCellSize, static_cast<std::uint32_t>(luma_y) / kCellSize};
  if (cell.x4 >= grid.cells_w() || cell.y4 >= grid.cells_h()) {
    throw std::out_of_range("ovt::model::cell_at: sample outside the cell grid");
  }
  return cell;
}

fb::Rect chroma_rect(fb::Rect luma, std::uint8_t chroma_format) {
  const ChromaSubsampling& sub = subsampling(chroma_format);
  if (sub.sub_width_c == 0) {
    return fb::Rect{0, 0, 0, 0};  // 4:0:0: no chroma planes
  }
  const std::int32_t sw = sub.sub_width_c;
  const std::int32_t sh = sub.sub_height_c;
  return fb::Rect{floor_div(luma.x(), sw), floor_div(luma.y(), sh), luma.w() / sw, luma.h() / sh};
}

PlaneSize chroma_plane_size(std::uint32_t width, std::uint32_t height, std::uint8_t chroma_format) {
  const ChromaSubsampling& sub = subsampling(chroma_format);
  if (sub.sub_width_c == 0) {
    return PlaneSize{0, 0};
  }
  const auto ceil_div = [](std::uint32_t value, std::uint32_t divisor) {
    return value / divisor + (value % divisor != 0 ? 1U : 0U);
  };
  return PlaneSize{ceil_div(width, sub.sub_width_c), ceil_div(height, sub.sub_height_c)};
}

}  // namespace ovt::model
