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
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "overlay/arrow_instances.h"
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
constexpr uint32_t kGridLayers =
    static_cast<uint32_t>(OverlayLayer::GridTU) | static_cast<uint32_t>(OverlayLayer::GridPU) |
    static_cast<uint32_t>(OverlayLayer::GridCU) | static_cast<uint32_t>(OverlayLayer::GridCTU);

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
  if (fa.blocks() != nullptr && fa.blocks()->begin() != fa.blocks()->end() &&
      fa.blocks()->Get(0)->ext_type() != nullptr) {
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

// Columns of the CellGrid the attribute texels read; null = not provided.
struct AttrColumns {
  const flatbuffers::Vector<uint8_t>* pred = nullptr;
  const flatbuffers::Vector<int8_t>* ref0 = nullptr;
  const flatbuffers::Vector<int8_t>* ref1 = nullptr;
  const flatbuffers::Vector<uint32_t>* block_id = nullptr;
  const flatbuffers::Vector<flatbuffers::Offset<ovt::fb::Block>>* blocks = nullptr;
};

// Attribute A: bit0 skip (pred == Skip or the owning block is skip), bit1 merge/direct, bit2 bi-predicted.
uint8_t attribute_flags(const AttrColumns& c, flatbuffers::uoffset_t idx) {
  bool skip = c.pred != nullptr && c.pred->Get(idx) == static_cast<uint8_t>(ovt::fb::PredKind::Skip);
  bool merge = false;
  if (c.block_id != nullptr && c.blocks != nullptr) {
    const uint32_t id = c.block_id->Get(idx);  // 0xFFFFFFFF = no owning block
    if (id < c.blocks->size() && c.blocks->Get(id)->id() == id) {
      skip = skip || c.blocks->Get(id)->skip();
      merge = c.blocks->Get(id)->merge_or_direct();
    }
  }
  const bool bi = c.ref0 != nullptr && c.ref1 != nullptr && c.ref0->Get(idx) >= 0 && c.ref1->Get(idx) >= 0;
  return static_cast<uint8_t>((skip ? 1U : 0U) | (merge ? 2U : 0U) | (bi ? 4U : 0U));
}

// ref + 1 (0 = no reference), or 0xFF when the plugin gave no vector.
uint8_t ref_texel(const flatbuffers::Vector<int8_t>* v, flatbuffers::uoffset_t idx) {
  return v != nullptr ? static_cast<uint8_t>(std::max<int>(v->Get(idx), -1) + 1) : 0xFF;
}

void build_attributes(const ovt::fb::FrameAnalysis& fa, OverlaySet& set) {
  const ovt::fb::CellGrid& grid = *fa.grid();
  const size_t cells = static_cast<size_t>(set.cells_w) * set.cells_h;
  AttrColumns c;
  c.pred = sized(grid.pred(), cells, "pred", set.diagnostics);
  c.ref0 = sized(grid.ref0(), cells, "ref0", set.diagnostics);
  c.ref1 = sized(grid.ref1(), cells, "ref1", set.diagnostics);
  c.block_id = sized(grid.block_id(), cells, "block_id", set.diagnostics);
  c.blocks = fa.blocks();

  set.cell_attributes.assign(cells * 4, 0);
  const std::span<uint8_t> out(set.cell_attributes);
  detail::parallel_ranges(cells, kChunks, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const auto idx = static_cast<flatbuffers::uoffset_t>(i);
      const size_t t = i * 4;
      out[t] = c.pred != nullptr ? c.pred->Get(idx) : 0xFF;
      out[t + 1] = ref_texel(c.ref0, idx);
      out[t + 2] = ref_texel(c.ref1, idx);
      out[t + 3] = attribute_flags(c, idx);
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

// §10.5.3 codec mapping, indexed by BlockKind {MB, SubMB, CTU, CU, PU, TU, SB, Block, TXB, Other}: H.264 MB -> CTU
// slot, SubMB -> PU slot, transform blocks -> TU slot, no CU; AV1 SB -> CTU, Block -> CU, TXB -> TU, no PU.
constexpr std::array<GridLevel, 10> kGridLevelOfKind = {GridLevel::CTU, GridLevel::PU,  GridLevel::CTU, GridLevel::CU,
                                                        GridLevel::PU,  GridLevel::TU,  GridLevel::CTU, GridLevel::CU,
                                                        GridLevel::TU,  GridLevel::None};

GridLevel grid_level_of(ovt::fb::BlockKind kind) {
  const auto k = static_cast<size_t>(kind);
  return k < kGridLevelOfKind.size() ? kGridLevelOfKind.at(k) : GridLevel::None;
}

// Flattens the block tree breadth-first so that every node's children are contiguous (WI-2.432 step 1). Children are
// followed only by id, each block at most once (a malformed tree with cycles or shared children cannot loop), and a
// node whose rect misses the coded picture is dropped with its subtree (FR-BLK-003: children lie inside the parent).
void build_grid_tree(const ovt::fb::FrameAnalysis& fa, OverlaySet& set) {
  const auto* blocks = fa.blocks();
  if (blocks == nullptr || blocks->empty() || blocks->Get(0)->rect() == nullptr) {
    set.diagnostics.emplace_back("FrameAnalysis has no block tree; grid lines are empty");
    return;
  }
  const ovt::fb::Rect& root = *blocks->Get(0)->rect();
  set.picture_w = std::max(root.w(), 0);
  set.picture_h = std::max(root.h(), 0);
  if (set.picture_w == 0 || set.picture_h == 0) {
    set.diagnostics.emplace_back("blocks[0] has an empty rect; grid lines are empty");
    return;
  }
  std::vector<bool> taken(blocks->size(), false);
  std::vector<uint32_t> source;  // FrameAnalysis block id of grid_nodes[i]
  taken[0] = true;
  source.push_back(0);
  set.grid_nodes.push_back({0, 0, set.picture_w, set.picture_h, 0, 0, GridLevel::None});
  for (size_t n = 0; n < set.grid_nodes.size(); ++n) {
    const auto* children = blocks->Get(source[n])->children();
    set.grid_nodes[n].first_child = static_cast<uint32_t>(set.grid_nodes.size());
    if (children == nullptr) {
      continue;
    }
    for (const uint32_t id : *children) {
      if (id >= blocks->size() || taken[id]) {
        continue;
      }
      const auto* b = blocks->Get(id);
      if (b->rect() == nullptr || b->rect()->w() <= 0 || b->rect()->h() <= 0) {
        continue;
      }
      const ovt::fb::Rect& r = *b->rect();
      const int64_t rx1 = int64_t{r.x()} + r.w();
      const int64_t ry1 = int64_t{r.y()} + r.h();
      GridNode node;
      node.x0 = std::max(r.x(), 0);
      node.y0 = std::max(r.y(), 0);
      node.x1 = static_cast<int32_t>(std::min<int64_t>(rx1, set.picture_w));
      node.y1 = static_cast<int32_t>(std::min<int64_t>(ry1, set.picture_h));
      if (node.x1 <= node.x0 || node.y1 <= node.y0) {
        continue;
      }
      node.level = grid_level_of(b->kind());
      if (node.level != GridLevel::None) {
        int32_t& side = set.grid_min_side.at(static_cast<size_t>(node.level));
        side = std::min({side, r.w(), r.h()});
      }
      taken[id] = true;
      source.push_back(id);
      set.grid_nodes.push_back(node);
      ++set.grid_nodes[n].child_count;
    }
  }
}

}  // namespace

OverlayBuilder::OverlayBuilder(VideoSurface& owner) : owner_(&owner) {}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): owner_ feeds the GPU upload of WI-2.431
std::unique_ptr<OverlaySet> OverlayBuilder::build(const ovt::fb::FrameAnalysis* fa, const OverlayConfig& cfg,
                                                  const uint16_t* custom_cells) {
  (void)owner_;  // kept for the GPU upload of WI-2.431; the CPU side does not touch the surface
  auto set = std::make_unique<OverlaySet>();
  set->config = cfg;
  if (fa != nullptr && (cfg.layers & kGridLayers) != 0) {
    build_grid_tree(*fa, *set);  // the grid lines need the block tree only, not the CellGrid
  }
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
  // WI-2.434: MV arrow and intra stroke instances (MotionVector list and CellGrid.intra_mode_y).
  detail::build_arrow_instances(*fa, cfg, set->arrows, set->diagnostics);
  return set;
}

std::unique_ptr<OverlaySet> OverlayBuilder::build(const uint8_t* buf, size_t size, const OverlayConfig& cfg,
                                                  const uint16_t* custom_cells) {
  if (buf == nullptr) {
    return nullptr;
  }
  // A dense 4K frame has a Block and a MotionVector per 4x4 cell (over a million tables, synthetic_4k_mv.fb); the
  // default verifier limit of 1 M tables would reject it. kMaxCells bounds what the build uses anyway.
  flatbuffers::Verifier::Options opts;
  opts.max_tables = 4 * kMaxCells;
  flatbuffers::Verifier verifier(buf, size, opts);
  if (!ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
    return nullptr;
  }
  return build(ovt::fb::GetFrameAnalysis(buf), cfg, custom_cells);
}

}  // namespace ovt::render
