// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Tests of the partition-tree reconstruction of ovt-plugin-aom (WI-2.156a, chapter 07 §7.6.1): hand-made MI grids,
// and the blockSize layers of libaom's own `inspect` (data/inspect_reference.json, written by
// data/make_inspect_reference.py).
// AT3: syn_av1_tiles_cdef_lr (128x128 superblocks), every leaf covers exactly its MIs and the leaf sizes equal
//      inspect's blockSize layout, HORZ_A and VERT_4 included.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "enums.h"
#include "partition_reconstruct.hpp"

using ovt::aom::PartNode;

namespace {

// ---- hand-made MI grids ----------------------------------------------------------------------------------------

struct Grid {
  int32_t rows;
  int32_t cols;
  std::vector<uint8_t> bsize;
  Grid(int32_t r, int32_t c) : rows(r), cols(c), bsize(static_cast<size_t>(r) * static_cast<size_t>(c), 255) {}
  // Stores block size @p b in every MI of the block at (r, c) that lies inside the grid, as libaom does.
  void paint(int32_t r, int32_t c, int b) {
    const auto s = ovt::aom::kBlockMiSize.at(static_cast<size_t>(b));
    for (int32_t y = r; y < std::min(r + s.h, rows); ++y) {
      for (int32_t x = c; x < std::min(c + s.w, cols); ++x) {
        bsize[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x)] = static_cast<uint8_t>(b);
      }
    }
  }
};

constexpr int kB8x8 = 3;
constexpr int kB16x8 = 5;
constexpr int kB8x16 = 4;
constexpr int kB16x16 = 6;
constexpr int kB32x16 = 8;
constexpr int kB16x32 = 7;
constexpr int kB32x32 = 9;
constexpr int kB64x32 = 11;
constexpr int kB64x64 = 12;
constexpr int kB128x64 = 14;
constexpr int kB128x128 = 15;
constexpr int kB16x4 = 17;
constexpr int kB4x16 = 16;

std::vector<PartNode> rebuild(const Grid& g, int32_t r, int32_t c, int32_t sb) {
  std::vector<PartNode> nodes;
  const ovt::aom::BsizeGrid bg{g.bsize, g.rows, g.cols};
  REQUIRE(ovt::aom::reconstruct_superblock(bg, r, c, sb, &nodes) == 0);
  return nodes;
}

// Every MI of the grid inside the superblock is covered by exactly one leaf, and the leaf's size is the size the
// grid stores there (inspect's blockSize layout).
void check_cover(const std::vector<PartNode>& nodes, const Grid& g, int32_t r0, int32_t c0, int32_t sb,
                 bool sizes = true) {
  std::map<std::pair<int32_t, int32_t>, int> cover;
  for (const PartNode& n : nodes) {
    if (!n.children.empty()) {
      continue;
    }
    for (int32_t y = n.mi_row; y < std::min(n.mi_row + n.mi_h, g.rows); ++y) {
      for (int32_t x = n.mi_col; x < std::min(n.mi_col + n.mi_w, g.cols); ++x) {
        ++cover[{y, x}];
        if (sizes) {
          CHECK(g.bsize[static_cast<size_t>(y) * static_cast<size_t>(g.cols) + static_cast<size_t>(x)] == n.bsize);
        }
      }
    }
  }
  for (int32_t y = r0; y < std::min(r0 + sb, g.rows); ++y) {
    for (int32_t x = c0; x < std::min(c0 + sb, g.cols); ++x) {
      CHECK(cover[{y, x}] == 1);
    }
  }
  // Pre-order: parents before children, children listed in coding order.
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (const uint32_t ch : nodes[i].children) {
      CHECK(nodes[ch].parent == static_cast<int32_t>(i));
      CHECK(ch > i);
    }
  }
}

const PartNode* node_at(const std::vector<PartNode>& nodes, int32_t r, int32_t c, int32_t size) {
  for (const PartNode& n : nodes) {
    if (n.mi_row == r && n.mi_col == c && n.mi_w == size && n.mi_h == size && !n.children.empty()) {
      return &n;
    }
  }
  return nullptr;
}

uint8_t part(ovt::aom::Partition p) {
  return static_cast<uint8_t>(p);
}

}  // namespace

TEST_CASE("aom partition: every partition type inside a 64x64 superblock", "[fast]") {
  using ovt::aom::Partition;
  Grid g(32, 32);
  // Superblock (0,0): HORZ_A, HORZ_B, VERT_A, VERT_B in its four 32x32 quadrants.
  g.paint(0, 0, kB16x16), g.paint(0, 4, kB16x16), g.paint(4, 0, kB32x16);     // HORZ_A
  g.paint(0, 8, kB32x16), g.paint(4, 8, kB16x16), g.paint(4, 12, kB16x16);    // HORZ_B
  g.paint(8, 0, kB16x16), g.paint(12, 0, kB16x16), g.paint(8, 4, kB16x32);    // VERT_A
  g.paint(8, 8, kB16x32), g.paint(8, 12, kB16x16), g.paint(12, 12, kB16x16);  // VERT_B
  // Superblock (0,16): HORZ_4 and VERT_4 16x16 nodes, NONE, HORZ, VERT and an 8x8 SPLIT.
  for (int k = 0; k < 4; ++k) {
    g.paint(k, 16, kB16x4);      // HORZ_4 at (0,16)
    g.paint(0, 20 + k, kB4x16);  // VERT_4 at (0,20)
  }
  g.paint(4, 16, kB16x16);                            // NONE
  g.paint(4, 20, kB16x8), g.paint(6, 20, kB16x8);     // HORZ
  g.paint(0, 24, kB32x32);                            // NONE 32x32
  g.paint(8, 16, kB32x16), g.paint(12, 16, kB32x16);  // HORZ 32x32
  g.paint(8, 24, kB8x16), g.paint(8, 26, kB8x16);     // VERT 16x16 at (8,24)
  for (int32_t y = 8; y < 12; y += 2) {               // SPLIT 16x16 at (8,28) into 8x8s
    g.paint(y, 28, kB8x8), g.paint(y, 30, kB8x8);
  }
  g.paint(12, 24, kB16x16), g.paint(12, 28, kB16x16);
  g.paint(16, 0, kB64x64), g.paint(16, 16, kB64x64);

  const auto a = rebuild(g, 0, 0, 16);
  check_cover(a, g, 0, 0, 16);
  CHECK(a[0].partition == part(Partition::Split));
  REQUIRE(node_at(a, 0, 0, 8) != nullptr);
  CHECK(node_at(a, 0, 0, 8)->partition == part(Partition::HorzA));
  CHECK(node_at(a, 0, 0, 8)->children.size() == 3);
  CHECK(node_at(a, 0, 8, 8)->partition == part(Partition::HorzB));
  CHECK(node_at(a, 8, 0, 8)->partition == part(Partition::VertA));
  CHECK(node_at(a, 8, 8, 8)->partition == part(Partition::VertB));

  const auto b = rebuild(g, 0, 16, 16);
  check_cover(b, g, 0, 16, 16);
  REQUIRE(node_at(b, 0, 16, 4) != nullptr);
  CHECK(node_at(b, 0, 16, 4)->partition == part(Partition::Horz4));
  CHECK(node_at(b, 0, 16, 4)->children.size() == 4);
  CHECK(node_at(b, 0, 20, 4)->partition == part(Partition::Vert4));
  CHECK(node_at(b, 4, 20, 4)->partition == part(Partition::Horz));
  CHECK(node_at(b, 8, 16, 8)->partition == part(Partition::Horz));
  CHECK(node_at(b, 8, 24, 4)->partition == part(Partition::Vert));
  CHECK(node_at(b, 8, 28, 4)->partition == part(Partition::Split));
  for (const PartNode& n : b) {
    CHECK_FALSE(n.ambiguous);
  }

  const auto leaf_sb = rebuild(g, 16, 0, 16);
  REQUIRE(leaf_sb.size() == 1);  // a 64x64 leaf superblock
  CHECK(leaf_sb[0].partition == 0);
  CHECK(leaf_sb[0].bsize == kB64x64);
}

TEST_CASE("aom partition: 128x128 superblocks and the frame edge", "[fast]") {
  using ovt::aom::Partition;
  Grid g(40, 40);                                     // the second superblock row and column are cut by the frame edge
  g.paint(0, 0, kB128x64), g.paint(16, 0, kB128x64);  // HORZ at the 128x128 root
  g.paint(0, 32, kB64x32), g.paint(8, 32, kB64x32);   // right edge: HORZ inside an 8-MI-wide superblock
  g.paint(16, 32, kB64x64);
  g.paint(32, 0, kB64x64), g.paint(32, 16, kB64x64);  // edge: forced SPLIT, bottom quadrants outside
  g.paint(32, 32, kB32x32);

  const auto a = rebuild(g, 0, 0, 32);
  check_cover(a, g, 0, 0, 32);
  CHECK(a[0].partition == part(Partition::Horz));
  CHECK(a[0].bsize == kB128x128);
  CHECK(a[0].children.size() == 2);

  const auto right = rebuild(g, 0, 32, 32);  // 8 MI columns inside the grid
  check_cover(right, g, 0, 32, 32);
  CHECK(right[0].partition == part(Partition::Split));
  REQUIRE(node_at(right, 0, 32, 16) != nullptr);
  CHECK(node_at(right, 0, 32, 16)->partition == part(Partition::Horz));

  const auto bottom = rebuild(g, 32, 0, 32);  // 8 MI rows inside the grid
  check_cover(bottom, g, 32, 0, 32);
  CHECK(bottom[0].partition == part(Partition::Split));
  CHECK(bottom[0].children.size() == 2);  // the bottom quadrants' origins are outside the grid

  const auto corner = rebuild(g, 32, 32, 32);
  check_cover(corner, g, 32, 32, 32);
  CHECK(corner[0].children.size() == 1);

  Grid strip(4, 16);  // bottom edge: HORZ whose second half lies outside the grid is not coded
  strip.paint(0, 0, kB64x32);
  const auto s = rebuild(strip, 0, 0, 16);
  check_cover(s, strip, 0, 0, 16);
  CHECK(s[0].partition == part(Partition::Horz));
  CHECK(s[0].children.size() == 1);
}

TEST_CASE("aom partition: an inconsistent grid is still covered exactly once", "[fast]") {
  Grid g(16, 16);
  g.paint(0, 0, kB64x64);
  g.paint(0, 0, kB128x128);  // larger than the superblock: fits no partition
  const auto n = rebuild(g, 0, 0, 16);
  check_cover(n, g, 0, 0, 16, /*sizes=*/false);
  CHECK(n[0].ambiguous);
  const Grid holes(16, 16);  // never painted: invalid sizes everywhere
  check_cover(rebuild(holes, 0, 0, 16), holes, 0, 0, 16, /*sizes=*/false);
}

// ---- inspect's layers ------------------------------------------------------------------------------------------

namespace {

const nlohmann::json& reference() {
  static const nlohmann::json j = [] {
    std::ifstream in(std::filesystem::path(OVT_AOM_TEST_DATA_DIR) / "inspect_reference.json");
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
  }();
  return j;
}

std::vector<int> unrle(const nlohmann::json& r) {
  std::vector<int> out;
  for (size_t i = 0; i + 1 < r.size(); i += 2) {
    out.insert(out.end(), r[i + 1].get<size_t>(), r[i].get<int>());
  }
  return out;
}
}  // namespace

TEST_CASE("aom partition AT3: inspect's blockSize layouts are rebuilt exactly", "[fast]") {
  // Superblock size per stream (sb_size of the sequence header): syn_av1_tiles_cdef_lr is encoded with
  // --sb-size=128 (chapter 14 recipe 13); the libaom test vectors use 64x64.
  const std::map<std::string, int32_t> sb_mi = {
      {"aom_av1_1_b8_02_allintra", 16}, {"aom_av1_1_b8_05_mv", 16}, {"syn_av1_tiles_cdef_lr", 32}};
  for (const auto& [stream, sb] : sb_mi) {
    std::set<uint8_t> partitions;
    const auto& frames = reference()["streams"][stream];
    REQUIRE(!frames.empty());
    for (size_t f = 0; f < frames.size(); ++f) {
      INFO(stream << " frame " << f);
      const auto layer = unrle(frames[f]["layers"]["blockSize"]);
      Grid g(frames[f]["mi_rows"].get<int32_t>(), frames[f]["mi_cols"].get<int32_t>());
      REQUIRE(layer.size() == g.bsize.size());
      std::transform(layer.begin(), layer.end(), g.bsize.begin(), [](int b) { return static_cast<uint8_t>(b); });
      for (int32_t r = 0; r < g.rows; r += sb) {
        for (int32_t c = 0; c < g.cols; c += sb) {
          const auto nodes = rebuild(g, r, c, sb);
          check_cover(nodes, g, r, c, sb);
          for (const PartNode& n : nodes) {
            CHECK_FALSE(n.ambiguous);
            partitions.insert(n.partition);
          }
        }
      }
    }
    if (stream == "syn_av1_tiles_cdef_lr") {
      CHECK(partitions.contains(part(ovt::aom::Partition::HorzA)));
      CHECK(partitions.contains(part(ovt::aom::Partition::Vert4)));
    }
  }
}
