// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.436 fixture: deblocking edges, reference slots and selectable blocks/MVs (layout in make_synthetic.h).

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

constexpr int kW = 128;
constexpr int kH = 96;

std::unique_ptr<fb::BlockT> block(uint32_t id, fb::BlockKind kind, const fb::Rect& r, std::vector<uint32_t> children) {
  auto b = std::make_unique<fb::BlockT>();
  b->id = id;
  b->kind = kind;
  b->rect = std::make_unique<fb::Rect>(r);
  b->children = std::move(children);
  b->pred = fb::PredKind::Inter;
  return b;
}

uint32_t owner(uint32_t cx, uint32_t cy) {
  const uint32_t ctu = 1 + (cx / 16) + (2 * (cy / 16));
  if (ctu != 1) {
    return ctu;
  }
  return 5 + (cx / 8) + (2 * (cy / 8));  // the CUs of CTU 1
}

std::unique_ptr<fb::MotionVectorT> mv(uint32_t block_id, uint8_t list, const fb::Rect& r) {
  auto m = std::make_unique<fb::MotionVectorT>();
  m->block_id = block_id;
  m->list = list;
  m->rect = std::make_unique<fb::Rect>(r);
  m->mv_x = 32;
  m->mv_y = -16;
  return m;
}

// HEVC bS on the 8x8 grid: 2 every 32 samples, else 1; the picture edge (index 0) is not filtered.
uint8_t bs_at(uint32_t i) {
  if (i == 0 || i % 2 != 0) {
    return 0;
  }
  return i % 8 == 0 ? 2 : 1;
}

std::unique_ptr<fb::CellGridT> make_misc_grid() {
  auto g = std::make_unique<fb::CellGridT>();
  g->cells_w = kW / 4;
  g->cells_h = kH / 4;
  for (uint32_t cy = 0; cy < g->cells_h; ++cy) {
    const bool gap = cy >= 8 && cy < 12;  // rows without vertical edges
    for (uint32_t cx = 0; cx < g->cells_w; ++cx) {
      g->block_id.push_back(owner(cx, cy));
      g->pred.push_back(static_cast<uint8_t>(fb::PredKind::Inter));
      g->deblock_bs_v.push_back(gap ? 0 : bs_at(cx));
      g->deblock_bs_h.push_back(bs_at(cy));
      const auto slot0 = static_cast<int>(((cx / 4) + (2 * (cy / 4))) % 8);
      const auto slot1 = static_cast<int>((cx / 4) % 8);
      g->ref0.push_back(static_cast<int8_t>(cy < 20 ? slot0 : -1));
      g->ref1.push_back(static_cast<int8_t>(cy >= 20 ? slot1 : -1));
    }
  }
  return g;
}

}  // namespace

std::vector<uint8_t> make_misc_fixture() {
  auto fa = std::make_unique<fb::FrameAnalysisT>();
  fa->schema_version = 1;
  fa->plugin_id = "ovt.plugin.de265";
  fa->filter_params_type = "HevcFilterParams";
  fa->blocks.push_back(block(0, fb::BlockKind::Other, fb::Rect(0, 0, kW, kH), {1, 2, 3, 4}));
  fa->blocks.push_back(block(1, fb::BlockKind::CTU, fb::Rect(0, 0, 64, 64), {5, 6, 7, 8}));
  fa->blocks.push_back(block(2, fb::BlockKind::CTU, fb::Rect(64, 0, 64, 64), {}));
  fa->blocks.push_back(block(3, fb::BlockKind::CTU, fb::Rect(0, 64, 64, 32), {}));
  fa->blocks.push_back(block(4, fb::BlockKind::CTU, fb::Rect(64, 64, 64, 32), {}));
  for (uint32_t i = 0; i < 4; ++i) {
    const auto x = static_cast<int32_t>(32 * (i % 2));
    const auto y = static_cast<int32_t>(32 * (i / 2));
    fa->blocks.push_back(block(5 + i, fb::BlockKind::CU, fb::Rect(x, y, 32, 32), {}));
  }

  fa->grid = make_misc_grid();
  fa->mvs.push_back(mv(5, 0, fb::Rect(0, 0, 16, 16)));
  fa->mvs.push_back(mv(2, 0, fb::Rect(64, 32, 16, 16)));
  fa->mvs.push_back(mv(4, 1, fb::Rect(96, 64, 32, 32)));

  flatbuffers::FlatBufferBuilder fbb;
  fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, fa.get()));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

}  // namespace ovt::render::test
