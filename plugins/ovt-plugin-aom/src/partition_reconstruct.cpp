// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Partition-tree reconstruction of chapter 07 §7.6.1 (WI-2.156 step 2 and notes).

#include "partition_reconstruct.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "enums.h"

namespace ovt::aom {

namespace {

struct Builder {
  const BsizeGrid& g;
  std::vector<PartNode>& out;

  [[nodiscard]] bool inside(int32_t r, int32_t c) const { return r >= 0 && c >= 0 && r < g.mi_rows && c < g.mi_cols; }

  // The MI shape of the block whose size is stored at (r, c); {0, 0} outside the grid or for an invalid size.
  [[nodiscard]] MiSize shape(int32_t r, int32_t c) const {
    if (!inside(r, c)) {
      return {0, 0};
    }
    const uint8_t b = g.bsize[static_cast<size_t>(r) * static_cast<size_t>(g.mi_cols) + static_cast<size_t>(c)];
    return b < kNumBlockSizes ? kBlockMiSize.at(b) : MiSize{0, 0};
  }

  [[nodiscard]] bool shape_is(int32_t r, int32_t c, int32_t w, int32_t h) const {
    const MiSize s = shape(r, c);
    return s.w == w && s.h == h;
  }

  uint32_t add(int32_t parent, int32_t r, int32_t c, int32_t w, int32_t h, uint8_t level) {
    PartNode n;
    n.parent = parent;
    n.mi_row = r;
    n.mi_col = c;
    n.mi_w = w;
    n.mi_h = h;
    n.bsize = static_cast<uint8_t>(block_size_of(w, h));
    n.level = level;
    const auto id = static_cast<uint32_t>(out.size());
    out.push_back(std::move(n));
    if (parent >= 0) {
      out[static_cast<size_t>(parent)].children.push_back(id);
    }
    return id;
  }

  // A leaf child of @p parent, skipped when its origin is outside the MI grid (not coded, AV1 §5.11.4).
  void leaf(uint32_t parent, int32_t r, int32_t c, int32_t w, int32_t h) {
    if (inside(r, c)) {
      add(static_cast<int32_t>(parent), r, c, w, h, static_cast<uint8_t>(out[parent].level + 1));
    }
  }

  void set(uint32_t id, Partition p) { out[id].partition = static_cast<uint8_t>(p); }

  // HORZ, HORZ_B, VERT or VERT_B for the n x n square @p id at (r, c) with origin shape @p s; false if none fits.
  bool halves(uint32_t id, int32_t r, int32_t c, int32_t n, MiSize s) {
    const int32_t half = n / 2;
    if (s.w == n && s.h == half) {
      if (shape_is(r + half, c, half, half)) {  // HORZ_B: n x n/2 on top, two squares below
        set(id, Partition::HorzB);
        leaf(id, r, c, n, half);
        leaf(id, r + half, c, half, half);
        leaf(id, r + half, c + half, half, half);
      } else {
        set(id, Partition::Horz);
        leaf(id, r, c, n, half);
        leaf(id, r + half, c, n, half);
      }
      return true;
    }
    if (s.w == half && s.h == n) {
      if (shape_is(r, c + half, half, half)) {  // VERT_B: n/2 x n on the left, two squares on the right
        set(id, Partition::VertB);
        leaf(id, r, c, half, n);
        leaf(id, r, c + half, half, half);
        leaf(id, r + half, c + half, half, half);
      } else {
        set(id, Partition::Vert);
        leaf(id, r, c, half, n);
        leaf(id, r, c + half, half, n);
      }
      return true;
    }
    return false;
  }

  // HORZ_A or VERT_A; false if neither fits.
  bool a_types(uint32_t id, int32_t r, int32_t c, int32_t n, MiSize s) {
    const int32_t half = n / 2;
    if (s.w != half || s.h != half) {
      return false;
    }
    if (shape_is(r + half, c, n, half)) {  // HORZ_A: two squares, n x n/2 below
      set(id, Partition::HorzA);
      leaf(id, r, c, half, half);
      leaf(id, r, c + half, half, half);
      leaf(id, r + half, c, n, half);
      return true;
    }
    if (shape_is(r, c + half, half, n)) {  // VERT_A: two squares, n/2 x n right
      set(id, Partition::VertA);
      leaf(id, r, c, half, half);
      leaf(id, r + half, c, half, half);
      leaf(id, r, c + half, half, n);
      return true;
    }
    return false;
  }

  // HORZ_4 or VERT_4; false if neither fits.
  bool quarters(uint32_t id, int32_t r, int32_t c, int32_t n, MiSize s) {
    const int32_t quarter = n / 4;
    if (quarter > 0 && s.w == n && s.h == quarter) {
      set(id, Partition::Horz4);
      for (int32_t k = 0; k < 4; ++k) {
        leaf(id, r + k * quarter, c, n, quarter);
      }
      return true;
    }
    if (quarter > 0 && s.w == quarter && s.h == n) {
      set(id, Partition::Vert4);
      for (int32_t k = 0; k < 4; ++k) {
        leaf(id, r, c + k * quarter, quarter, n);
      }
      return true;
    }
    return false;
  }

  // The square node of n x n MIs at (r, c), origin inside the grid. Returns its index.
  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by log2 of the superblock size (at most 5 levels)
  uint32_t square(int32_t parent, int32_t r, int32_t c, int32_t n, uint8_t level) {
    const uint32_t id = add(parent, r, c, n, n, level);
    if (n == 1) {
      return id;  // a 4x4 square is always a leaf
    }
    const int32_t half = n / 2;
    const MiSize s = shape(r, c);
    if ((s.w == n && s.h == n) || halves(id, r, c, n, s) || a_types(id, r, c, n, s) || quarters(id, r, c, n, s)) {
      return id;  // PARTITION_NONE (the square is the leaf) or one of the partitions above
    }
    // PARTITION_SPLIT: the origin block lies inside the top-left quadrant. Four equal squares and SPLIT give the
    // same leaves; SPLIT is the reading (§7.6.1 notes). Any other origin size fits no partition: flag it.
    set(id, Partition::Split);
    out[id].ambiguous = s.w == 0 || s.w > half || s.h > half;
    for (const auto& [dr, dc] : std::array{std::pair{0, 0}, std::pair{0, 1}, std::pair{1, 0}, std::pair{1, 1}}) {
      if (inside(r + dr * half, c + dc * half)) {
        square(static_cast<int32_t>(id), r + dr * half, c + dc * half, half, static_cast<uint8_t>(level + 1));
      }
    }
    return id;
  }
};

}  // namespace

uint32_t reconstruct_superblock(const BsizeGrid& grid, int32_t mi_row, int32_t mi_col, int32_t sb_mi,
                                std::vector<PartNode>* out) {
  Builder b{grid, *out};
  return b.square(-1, mi_row, mi_col, sb_mi, 0);
}

}  // namespace ovt::aom
