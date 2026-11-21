// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Partition-grid fixture for the grid line pass (WI-2.432), the C++ twin of `tools/gen_fixture_fb.py --kind grid`.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "fixtures/make_synthetic.h"
#include "ovt_model_generated.h"

namespace ovt::render::test {
namespace {

namespace fb = ovt::fb;

struct Square {
  int32_t x, y, size;  // NOLINT(readability-isolate-declaration)
};

// gen_fixture_fb.py quadtree(): split while larger than the target, or while larger than 8 and crossing the edge.
// Iterative, depth-first in the generator's quadrant order (top-left, top-right, bottom-left, bottom-right).
void quadtree(Square root, int32_t target, int32_t w, int32_t h, std::vector<Square>& out) {
  std::vector<Square> stack = {root};
  while (!stack.empty()) {
    const Square s = stack.back();
    stack.pop_back();
    if (s.size > target || (s.size > 8 && (s.x + s.size > w || s.y + s.size > h))) {
      const int32_t half = s.size / 2;
      stack.push_back({s.x + half, s.y + half, half});
      stack.push_back({s.x, s.y + half, half});
      stack.push_back({s.x + half, s.y, half});
      stack.push_back({s.x, s.y, half});
      continue;
    }
    out.push_back(s);
  }
}

std::unique_ptr<fb::BlockT> block(uint32_t id, int32_t parent, fb::BlockKind kind, const fb::Rect& r, uint8_t level) {
  auto b = std::make_unique<fb::BlockT>();
  b->id = id;
  b->parent = parent;
  b->kind = kind;
  b->rect = std::make_unique<fb::Rect>(r);
  b->level = level;
  return b;
}

}  // namespace

std::vector<uint8_t> make_grid_fixture(const GridFixtureOptions& opts) {
  const auto w = static_cast<int32_t>(opts.width);
  const auto h = static_cast<int32_t>(opts.height);
  auto fa = std::make_unique<fb::FrameAnalysisT>();
  fa->schema_version = 1;
  fa->plugin_id = "ovt.plugin.de265";
  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = (opts.width + 3) / 4;
  grid->cells_h = (opts.height + 3) / 4;
  fa->grid = std::move(grid);

  fa->blocks.push_back(block(0, -1, fb::BlockKind::Other, fb::Rect(0, 0, w, h), 0));
  for (int32_t cy = 0; cy * 64 < h; ++cy) {
    for (int32_t cx = 0; cx * 64 < w; ++cx) {
      const auto ctu = static_cast<uint32_t>(fa->blocks.size());
      fa->blocks[0]->children.push_back(ctu);
      fa->blocks.push_back(block(ctu, 0, fb::BlockKind::CTU, fb::Rect(cx * 64, cy * 64, 64, 64), 1));
      std::vector<Square> cus;
      quadtree({cx * 64, cy * 64, 64}, 64 >> ((cx + cy) % 4), w, h, cus);
      for (const Square& s : cus) {
        const auto cu = static_cast<uint32_t>(fa->blocks.size());
        fa->blocks[ctu]->children.push_back(cu);
        auto b = block(cu, static_cast<int32_t>(ctu), fb::BlockKind::CU, fb::Rect(s.x, s.y, s.size, s.size), 2);
        b->pred = fb::PredKind::Intra;
        fa->blocks.push_back(std::move(b));
        if (opts.tus) {
          const int32_t half = s.size / 2;
          for (const int32_t dy : {0, half}) {
            for (const int32_t dx : {0, half}) {
              const auto tu = static_cast<uint32_t>(fa->blocks.size());
              fa->blocks[cu]->children.push_back(tu);
              fa->blocks.push_back(
                  block(tu, static_cast<int32_t>(cu), fb::BlockKind::TU, fb::Rect(s.x + dx, s.y + dy, half, half), 3));
            }
          }
        }
      }
    }
  }
  if (opts.tiles_2x2) {
    const int32_t mx = w / 2;
    const int32_t my = h / 2;
    const std::vector<fb::Rect> tiles = {fb::Rect(0, 0, mx, my), fb::Rect(mx, 0, w - mx, my),
                                         fb::Rect(0, my, mx, h - my), fb::Rect(mx, my, w - mx, h - my)};
    for (size_t i = 0; i < tiles.size(); ++i) {
      auto p = std::make_unique<fb::PartitionInfoT>();
      p->kind = fb::PartitionKind::Tile;
      p->index = static_cast<uint16_t>(i);
      p->rect = std::make_unique<fb::Rect>(tiles[i]);
      fa->partitions.push_back(std::move(p));
    }
  }
  flatbuffers::FlatBufferBuilder fbb;
  fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, fa.get()));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

}  // namespace ovt::render::test
