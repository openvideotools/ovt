// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// OverlayBuilder, CPU side (chapter 10 §10.2.5, §10.5.1, §10.5.5). WI-2.430 turns a FrameAnalysis into the arrays the
// overlay passes upload: the per-cell attribute texels, the heatmap texels and the slice/tile boundary segments.
// GPU upload happens in set_overlays() with the pass work items (WI-2.431 onward).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "overlay/heatmap_source.h"
#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"

namespace ovt::render {
namespace {

constexpr uint64_t kMaxCells = 1ULL << 26;  // 8K is 2.07 M cells; anything beyond 64 M is a corrupt grid
constexpr unsigned kChunks = 4;             // WI-2.430 step 5
constexpr uint32_t kAttrLayers = static_cast<uint32_t>(OverlayLayer::PredFill) |
                                 static_cast<uint32_t>(OverlayLayer::SkipHatch) |
                                 static_cast<uint32_t>(OverlayLayer::RefIdxFill);

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

bool is_av1_frame(const ovt::fb::FrameAnalysis& fa) {
  if (fa.filter_params_type() != nullptr && contains(fa.filter_params_type()->string_view(), "Av1")) {
    return true;
  }
  if (fa.plugin_id() != nullptr) {
    const std::string_view id = fa.plugin_id()->string_view();
    if (contains(id, "av1") || contains(id, "aom") || contains(id, "dav1d")) {
      return true;
    }
  }
  if (fa.blocks() != nullptr && fa.blocks()->size() > 0 && fa.blocks()->Get(0)->ext_type() != nullptr) {
    return contains(fa.blocks()->Get(0)->ext_type()->string_view(), "Av1");
  }
  return false;
}

template <typename V>
const V* sized(const V* v, size_t cells, const char* name, std::vector<std::string>& diag) {
  if (v == nullptr || v->size() == 0) {
    return nullptr;
  }
  if (v->size() != cells) {
    diag.push_back(std::string("CellGrid.") + name + " has " + std::to_string(v->size()) + " entries, expected " +
                   std::to_string(cells) + "; ignored");
    return nullptr;
  }
  return v;
}

void build_attributes(const ovt::fb::FrameAnalysis& fa, OverlaySet& set) {
  const ovt::fb::CellGrid& grid = *fa.grid();
  const size_t cells = static_cast<size_t>(set.cells_w) * set.cells_h;
  const auto* pred = sized(grid.pred(), cells, "pred", set.diagnostics);
  const auto* ref0 = sized(grid.ref0(), cells, "ref0", set.diagnostics);
  const auto* ref1 = sized(grid.ref1(), cells, "ref1", set.diagnostics);
  const auto* block_id = sized(grid.block_id(), cells, "block_id", set.diagnostics);
  const auto* blocks = fa.blocks();

  set.cell_attributes.assign(cells * 4, 0);
  uint8_t* out = set.cell_attributes.data();
  detail::parallel_ranges(cells, kChunks, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const auto idx = static_cast<flatbuffers::uoffset_t>(i);
      uint8_t* t = out + (i * 4);
      const uint8_t kind = pred != nullptr ? pred->Get(idx) : 0xFF;
      t[0] = kind;
      t[1] = ref0 != nullptr ? static_cast<uint8_t>(std::max<int>(ref0->Get(idx), -1) + 1) : 0xFF;
      t[2] = ref1 != nullptr ? static_cast<uint8_t>(std::max<int>(ref1->Get(idx), -1) + 1) : 0xFF;
      uint8_t flags = 0;
      bool skip = kind == static_cast<uint8_t>(ovt::fb::PredKind::Skip);
      bool merge = false;
      if (block_id != nullptr && blocks != nullptr) {
        const uint32_t id = block_id->Get(idx);
        if (id < blocks->size() && blocks->Get(id)->id() == id) {  // 0xFFFFFFFF = no owning block
          skip = skip || blocks->Get(id)->skip();
          merge = blocks->Get(id)->merge_or_direct();
        }
      }
      const bool bi = ref0 != nullptr && ref1 != nullptr && ref0->Get(idx) >= 0 && ref1->Get(idx) >= 0;
      flags = static_cast<uint8_t>((skip ? 1U : 0U) | (merge ? 2U : 0U) | (bi ? 4U : 0U));
      t[3] = flags;
    }
  });
}

void build_partition_lines(const ovt::fb::FrameAnalysis& fa, OverlaySet& set) {
  const auto* parts = fa.partitions();
  if (parts == nullptr) {
    return;
  }
  // SPEC-ASSUMPTION(#317): slices and tiles only (§10.5.1 "slice / tile boundaries"); tile groups, slice segments and
  // WPP substreams are not spatial boundaries of their own. A shared edge is emitted once (§10.5.3), the first
  // partition keeps it.
  using Key = std::tuple<int32_t, int32_t, int32_t, int32_t>;
  std::set<Key> seen;
  for (flatbuffers::uoffset_t i = 0; i < parts->size(); ++i) {
    const auto* p = parts->Get(i);
    if (p->kind() != ovt::fb::PartitionKind::Slice && p->kind() != ovt::fb::PartitionKind::Tile) {
      continue;
    }
    if (p->rect() == nullptr) {
      continue;
    }
    const ovt::fb::Rect& r = *p->rect();
    if (r.w() <= 0 || r.h() <= 0) {
      continue;
    }
    const int32_t x0 = r.x();
    const int32_t y0 = r.y();
    const int32_t x1 = x0 + r.w();
    const int32_t y1 = y0 + r.h();
    const std::array<Key, 4> edges{Key{x0, y0, x1, y0}, Key{x0, y1, x1, y1}, Key{x0, y0, x0, y1}, Key{x1, y0, x1, y1}};
    for (const Key& e : edges) {
      if (seen.insert(e).second) {
        set.partition_lines.push_back({std::get<0>(e), std::get<1>(e), std::get<2>(e), std::get<3>(e),
                                       static_cast<uint8_t>(p->kind()), p->index()});
      }
    }
  }
}

}  // namespace

OverlayBuilder::OverlayBuilder(VideoSurface& owner) : owner_(&owner) {}

std::unique_ptr<OverlaySet> OverlayBuilder::build(const ovt::fb::FrameAnalysis* fa, const OverlayConfig& cfg,
                                                  const uint16_t* custom_cells) {
  auto set = std::make_unique<OverlaySet>();
  set->config = cfg;
  if (fa == nullptr || fa->grid() == nullptr) {
    set->diagnostics.emplace_back("FrameAnalysis has no CellGrid; overlay data is empty");
    return set;
  }
  const ovt::fb::CellGrid& grid = *fa->grid();
  const uint64_t cells64 = static_cast<uint64_t>(grid.cells_w()) * grid.cells_h();
  if (cells64 > kMaxCells) {
    set->diagnostics.emplace_back("CellGrid of " + std::to_string(grid.cells_w()) + "x" +
                                  std::to_string(grid.cells_h()) + " cells is too large; overlay data is empty");
  } else {
    set->cells_w = grid.cells_w();
    set->cells_h = grid.cells_h();
    if ((cfg.layers & kAttrLayers) != 0 && cells64 > 0) {
      build_attributes(*fa, *set);
    }
    if (cfg.heatmap != HeatmapSource::None && cells64 > 0) {
      detail::HeatmapResult h;
      if (detail::build_heatmap(grid, is_av1_frame(*fa), cfg, custom_cells, h, set->diagnostics)) {
        set->heatmap = std::move(h.values);
        set->heat_valid = std::move(h.valid);
        set->heat_lo = h.lo;
        set->heat_hi = h.hi;
      }
    }
  }
  if ((cfg.layers & static_cast<uint32_t>(OverlayLayer::GridPartition)) != 0) {
    build_partition_lines(*fa, *set);
  }
  return set;
}

std::unique_ptr<OverlaySet> OverlayBuilder::build(const uint8_t* buf, size_t size, const OverlayConfig& cfg,
                                                  const uint16_t* custom_cells) {
  if (buf == nullptr) {
    return nullptr;
  }
  flatbuffers::Verifier verifier(buf, size);
  if (!ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
    return nullptr;
  }
  return build(ovt::fb::GetFrameAnalysis(buf), cfg, custom_cells);
}

}  // namespace ovt::render
