// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Synthetic FrameAnalysis for the overlay tests (WI-2.430); cell formulas are documented in make_synthetic.h.

#include "fixtures/make_synthetic.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "ovt_model_generated.h"

namespace ovt::render::test {

int expected_qp(uint32_t cx, uint32_t cy) {
  return (cx == 0 && cy == 0) ? -1 : static_cast<int>((7 * cx + 3 * cy) % 52);
}

namespace {

namespace fb = ovt::fb;

fb::PredKind pred_at(uint32_t cx, uint32_t cy) {
  if (cx == 7 && cy == 5) {
    return fb::PredKind::Skip;
  }
  return (cx == 1 && cy == 0) ? fb::PredKind::Intra : fb::PredKind::Inter;
}

void add_cell(fb::CellGridT& g, const SyntheticOptions& opts, uint32_t cx, uint32_t cy) {
  g.block_id.push_back(cx < opts.cells_w / 2 ? 0U : 1U);
  if (!opts.drop_pred) {
    g.pred.push_back(static_cast<uint8_t>(pred_at(cx, cy)));
  }
  g.qp.push_back(static_cast<int16_t>(expected_qp(cx, cy)));
  g.ref0.push_back(static_cast<int8_t>(static_cast<int>(cx % 3) - 1));
  g.ref1.push_back(static_cast<int8_t>(cx % 5 == 0 ? 1 : -1));
  g.mv0_x.push_back(static_cast<int16_t>((4 * static_cast<int>(cx)) - 100));
  g.mv0_y.push_back(static_cast<int16_t>(cy));
  g.bits_x8.push_back(8U * cx * cy);
  const auto tx = (cx == 0 && cy == 0) ? uint8_t{0xFF} : static_cast<uint8_t>(2 + ((cx % 4) / 2));
  g.tx_log2_w.push_back(tx);
  g.tx_log2_h.push_back(tx);
}

std::unique_ptr<fb::CellGridT> make_grid(const SyntheticOptions& opts) {
  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = opts.cells_w;
  grid->cells_h = opts.cells_h;
  for (uint32_t cy = 0; cy < opts.cells_h; ++cy) {
    for (uint32_t cx = 0; cx < opts.cells_w; ++cx) {
      add_cell(*grid, opts, cx, cy);
    }
  }
  if (opts.short_qp) {
    grid->qp.pop_back();
  }
  return grid;
}

void add_partition(fb::FrameAnalysisT& fa, fb::PartitionKind kind, uint16_t index, const fb::Rect& r) {
  auto p = std::make_unique<fb::PartitionInfoT>();
  p->kind = kind;
  p->index = index;
  p->rect = std::make_unique<fb::Rect>(r);
  fa.partitions.push_back(std::move(p));
}

}  // namespace

std::vector<uint8_t> make_synthetic(const SyntheticOptions& opts) {
  auto fa = std::make_unique<fb::FrameAnalysisT>();
  fa->schema_version = 1;
  fa->plugin_id = opts.av1 ? "ovt.plugin.aom" : "ovt.plugin.ffmpeg";
  fa->filter_params_type = opts.av1 ? "Av1FilterParams" : "AvcFilterParams";
  fa->grid = make_grid(opts);

  for (uint32_t id = 0; id < 2; ++id) {
    auto b = std::make_unique<fb::BlockT>();
    b->id = id;
    b->skip = id == 1;
    b->merge_or_direct = id == 1;
    fa->blocks.push_back(std::move(b));
  }

  const int w = static_cast<int>(opts.cells_w * 4);
  const int h = static_cast<int>(opts.cells_h * 4);
  add_partition(*fa, fb::PartitionKind::Tile, 0, fb::Rect(0, 0, w / 2, h));
  add_partition(*fa, fb::PartitionKind::Tile, 1, fb::Rect(w / 2, 0, w - (w / 2), h));
  add_partition(*fa, fb::PartitionKind::SliceSegment, 2, fb::Rect(0, 0, w, h));
  add_partition(*fa, fb::PartitionKind::TileGroup, 3, fb::Rect(0, 0, w, h));

  flatbuffers::FlatBufferBuilder fbb;
  fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, fa.get()));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

}  // namespace ovt::render::test
