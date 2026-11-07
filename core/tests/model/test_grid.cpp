// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.084 acceptance test 1 (grid part): cell placement and chroma rectangles (chapter 06 §6.2.1, §6.2.2).

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include "ovt/cxx/model/grid.hpp"

namespace {

using ovt::model::CellIndex;
using ovt::model::PlaneSize;

// A finished CellGrid table of cells_w × cells_h cells (no vectors).
struct GridBuffer {
  flatbuffers::FlatBufferBuilder fbb;
  const ovt::fb::CellGrid* grid = nullptr;
  GridBuffer(std::uint32_t cells_w, std::uint32_t cells_h) {
    ovt::fb::CellGridBuilder b(fbb);
    b.add_cells_w(cells_w);
    b.add_cells_h(cells_h);
    fbb.Finish(b.Finish());
    grid = flatbuffers::GetRoot<ovt::fb::CellGrid>(fbb.GetBufferPointer());
  }
};

bool same_rect(const ovt::fb::Rect& a, const ovt::fb::Rect& b) {
  return a.x() == b.x() && a.y() == b.y() && a.w() == b.w() && a.h() == b.h();
}

}  // namespace

TEST_CASE("test_grid: cell_index is y4 * cells_w + x4 and bounds-checked", "[fast][model]") {
  const GridBuffer g(4, 3);
  CHECK(ovt::model::cell_index(*g.grid, CellIndex{0, 0}) == 0);
  CHECK(ovt::model::cell_index(*g.grid, CellIndex{1, 2}) == 9);
  CHECK(ovt::model::cell_index(*g.grid, CellIndex{3, 2}) == 11);
  CHECK_THROWS_AS(ovt::model::cell_index(*g.grid, CellIndex{4, 0}), std::out_of_range);
  CHECK_THROWS_AS(ovt::model::cell_index(*g.grid, CellIndex{0, 3}), std::out_of_range);

  const GridBuffer empty(0, 0);
  CHECK_THROWS_AS(ovt::model::cell_index(*empty.grid, CellIndex{0, 0}), std::out_of_range);

  // 2^17 × 2^16 cells: the last index does not fit 32 bits.
  const GridBuffer huge(1U << 17U, 1U << 16U);
  CHECK_THROWS_AS(ovt::model::cell_index(*huge.grid, CellIndex{(1U << 17U) - 1, (1U << 16U) - 1}), std::out_of_range);
}

TEST_CASE("test_grid: cell_at maps coded luma samples to cells", "[fast][model]") {
  const GridBuffer g(4, 4);  // the 16×16 picture of §6.10
  CHECK(ovt::model::cell_at(*g.grid, 0, 0) == CellIndex{0, 0});
  CHECK(ovt::model::cell_at(*g.grid, 3, 3) == CellIndex{0, 0});
  CHECK(ovt::model::cell_at(*g.grid, 4, 0) == CellIndex{1, 0});
  CHECK(ovt::model::cell_at(*g.grid, 9, 9) == CellIndex{2, 2});
  CHECK(ovt::model::cell_at(*g.grid, 15, 15) == CellIndex{3, 3});
  CHECK_THROWS_AS(ovt::model::cell_at(*g.grid, 16, 0), std::out_of_range);
  CHECK_THROWS_AS(ovt::model::cell_at(*g.grid, 0, 16), std::out_of_range);
  // Negative inputs are invalid, including -1..-3 that truncating division would put in cell 0.
  CHECK_THROWS_AS(ovt::model::cell_at(*g.grid, -1, 0), std::out_of_range);
  CHECK_THROWS_AS(ovt::model::cell_at(*g.grid, 0, -3), std::out_of_range);
}

TEST_CASE("test_grid: cells_for rounds up (AV1 sizes are arbitrary)", "[fast][model]") {
  CHECK(ovt::model::cells_for(0) == 0);
  CHECK(ovt::model::cells_for(1) == 1);
  CHECK(ovt::model::cells_for(16) == 4);
  CHECK(ovt::model::cells_for(17) == 5);
  CHECK(ovt::model::cells_for(1920) == 480);
  CHECK(ovt::model::cells_for(1080) == 270);
  static_assert(ovt::model::cells_for(4) == 1);
}

TEST_CASE("test_grid: chroma_rect for every chroma format", "[fast][model]") {
  const ovt::fb::Rect luma{8, 16, 16, 8};
  CHECK(same_rect(ovt::model::chroma_rect(luma, 0), ovt::fb::Rect{0, 0, 0, 0}));    // 4:0:0
  CHECK(same_rect(ovt::model::chroma_rect(luma, 1), ovt::fb::Rect{4, 8, 8, 4}));    // 4:2:0
  CHECK(same_rect(ovt::model::chroma_rect(luma, 2), ovt::fb::Rect{4, 16, 8, 8}));   // 4:2:2
  CHECK(same_rect(ovt::model::chroma_rect(luma, 3), ovt::fb::Rect{8, 16, 16, 8}));  // 4:4:4
  // A 4×4 luma cell in 4:2:0 covers a 2×2 chroma area (§6.2.2).
  CHECK(same_rect(ovt::model::chroma_rect(ovt::fb::Rect{4, 4, 4, 4}, 1), ovt::fb::Rect{2, 2, 2, 2}));
  // A CTU that starts left of the picture (never in practice) still rounds towards minus infinity.
  CHECK(same_rect(ovt::model::chroma_rect(ovt::fb::Rect{-3, -1, 8, 8}, 1), ovt::fb::Rect{-2, -1, 4, 4}));
  CHECK_THROWS_AS(ovt::model::chroma_rect(luma, 4), std::invalid_argument);
}

TEST_CASE("test_grid: chroma_plane_size uses ceil division", "[fast][model]") {
  CHECK(ovt::model::chroma_plane_size(17, 9, 0) == PlaneSize{0, 0});
  CHECK(ovt::model::chroma_plane_size(17, 9, 1) == PlaneSize{9, 5});
  CHECK(ovt::model::chroma_plane_size(17, 9, 2) == PlaneSize{9, 9});
  CHECK(ovt::model::chroma_plane_size(17, 9, 3) == PlaneSize{17, 9});
  CHECK(ovt::model::chroma_plane_size(16, 16, 1) == PlaneSize{8, 8});
  CHECK_THROWS_AS(ovt::model::chroma_plane_size(16, 16, 255), std::invalid_argument);
}
