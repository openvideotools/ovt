// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Synthetic FrameAnalysis for the overlay tests (WI-2.430); cell formulas are documented in make_synthetic.h.

#include "fixtures/make_synthetic.h"

#include <cstddef>
#include <memory>
#include <utility>

#include "ovt_model_generated.h"

namespace ovt::render::test {

int expected_qp(uint32_t cx, uint32_t cy) {
  return (cx == 0 && cy == 0) ? -1 : static_cast<int>((7 * cx + 3 * cy) % 52);
}

std::vector<uint8_t> make_synthetic(const SyntheticOptions& opts) {
  namespace fb = ovt::fb;

  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = opts.cells_w;
  grid->cells_h = opts.cells_h;
  for (uint32_t cy = 0; cy < opts.cells_h; ++cy) {
    for (uint32_t cx = 0; cx < opts.cells_w; ++cx) {
      grid->block_id.push_back(cx < opts.cells_w / 2 ? 0U : 1U);
      if (!opts.drop_pred) {
        fb::PredKind k = fb::PredKind::Inter;
        if (cx == 7 && cy == 5) {
          k = fb::PredKind::Skip;
        } else if (cx == 1 && cy == 0) {
          k = fb::PredKind::Intra;
        }
        grid->pred.push_back(static_cast<uint8_t>(k));
      }
      grid->qp.push_back(static_cast<int16_t>(expected_qp(cx, cy)));
      grid->ref0.push_back(static_cast<int8_t>(static_cast<int>(cx % 3) - 1));
      grid->ref1.push_back(static_cast<int8_t>(cx % 5 == 0 ? 1 : -1));
      grid->mv0_x.push_back(static_cast<int16_t>((4 * static_cast<int>(cx)) - 100));
      grid->mv0_y.push_back(static_cast<int16_t>(cy));
      grid->bits_x8.push_back(8U * cx * cy);
      const bool na = cx == 0 && cy == 0;
      grid->tx_log2_w.push_back(na ? 0xFF : static_cast<uint8_t>(2 + ((cx % 4) / 2)));
      grid->tx_log2_h.push_back(na ? 0xFF : static_cast<uint8_t>(2 + ((cx % 4) / 2)));
    }
  }
  if (opts.short_qp) {
    grid->qp.pop_back();
  }

  auto fa = std::make_unique<fb::FrameAnalysisT>();
  fa->schema_version = 1;
  fa->plugin_id = opts.av1 ? "ovt.plugin.aom" : "ovt.plugin.ffmpeg";
  fa->filter_params_type = opts.av1 ? "Av1FilterParams" : "AvcFilterParams";
  fa->grid = std::move(grid);

  for (uint32_t id = 0; id < 2; ++id) {
    auto b = std::make_unique<fb::BlockT>();
    b->id = id;
    b->skip = id == 1;
    b->merge_or_direct = id == 1;
    fa->blocks.push_back(std::move(b));
  }

  const int w = static_cast<int>(opts.cells_w * 4);
  const int h = static_cast<int>(opts.cells_h * 4);
  auto add_partition = [&](fb::PartitionKind kind, uint16_t index, int x, int y, int pw, int ph) {
    auto p = std::make_unique<fb::PartitionInfoT>();
    p->kind = kind;
    p->index = index;
    p->rect = std::make_unique<fb::Rect>(x, y, pw, ph);
    fa->partitions.push_back(std::move(p));
  };
  add_partition(fb::PartitionKind::Tile, 0, 0, 0, w / 2, h);
  add_partition(fb::PartitionKind::Tile, 1, w / 2, 0, w - (w / 2), h);
  add_partition(fb::PartitionKind::SliceSegment, 2, 0, 0, w, h);
  add_partition(fb::PartitionKind::TileGroup, 3, 0, 0, w, h);

  flatbuffers::FlatBufferBuilder fbb;
  fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, fa.get()));
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

}  // namespace ovt::render::test
