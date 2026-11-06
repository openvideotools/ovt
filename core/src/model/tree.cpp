// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Block-tree traversal (chapter 06 §6.3.2, WI-0.084). See tree.hpp for the robustness rules.

#include "ovt/cxx/model/tree.hpp"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "ovt/cxx/model/grid.hpp"

namespace ovt::model {

namespace {

bool rect_contains(const fb::Rect& rect, int x, int y) noexcept {
  const std::int64_t x0 = rect.x();
  const std::int64_t y0 = rect.y();
  return x >= x0 && y >= y0 && x < x0 + rect.w() && y < y0 + rect.h();
}

// Depth-first search below blocks[root] over the children whose rect contains (x, y), for the DEEPEST block of
// kind @p kind (root itself excluded; the first in pre-order on a tie). Deepest, because HEVC transform trees
// nest TUs (§6.3.2 rule 3): the TU at a sample is the leaf TU, not the trafoDepth-0 root. Same robustness
// rules as pre_order.
const fb::Block* find_below(const flatbuffers::Vector<flatbuffers::Offset<fb::Block>>& blocks, std::uint32_t root,
                            int x, int y, fb::BlockKind kind) {
  std::vector<bool> seen(blocks.size(), false);
  std::vector<std::pair<std::uint32_t, int>> stack{{root, 0}};
  seen[root] = true;
  const fb::Block* best = nullptr;
  int best_depth = 0;
  while (!stack.empty()) {
    const auto [id, depth] = stack.back();
    stack.pop_back();
    const fb::Block& node = *blocks.Get(id);
    if (id != root && node.kind() == kind && depth > best_depth) {
      best = &node;
      best_depth = depth;
    }
    const auto* children = node.children();
    if (children == nullptr) {
      continue;
    }
    for (flatbuffers::uoffset_t i = children->size(); i-- > 0;) {
      const std::uint32_t child = children->Get(i);
      if (child <= id || child >= blocks.size() || seen[child]) {
        continue;
      }
      const fb::Rect* rect = blocks.Get(child)->rect();
      if (rect == nullptr || !rect_contains(*rect, x, y)) {
        continue;
      }
      seen[child] = true;
      stack.emplace_back(child, depth + 1);
    }
  }
  return best;
}

}  // namespace

const fb::Block* TreeWalker::block(std::uint32_t id) const noexcept {
  const auto* blocks = fa.blocks();
  if (blocks == nullptr || id >= blocks->size()) {
    return nullptr;
  }
  return blocks->Get(id);
}

void TreeWalker::pre_order(const std::function<void(const fb::Block&, int depth)>& visit) const {
  const auto* blocks = fa.blocks();
  if (blocks == nullptr) {
    return;
  }
  std::vector<bool> seen(blocks->size(), false);
  if (seen.empty()) {
    return;
  }
  std::vector<std::pair<std::uint32_t, int>> stack{{0U, 0}};
  seen[0] = true;
  while (!stack.empty()) {
    const auto [id, depth] = stack.back();
    stack.pop_back();
    const fb::Block& node = *blocks->Get(id);
    visit(node, depth);
    const auto* children = node.children();
    if (children == nullptr) {
      continue;
    }
    // Push in reverse so that the first child is visited first.
    for (flatbuffers::uoffset_t i = children->size(); i-- > 0;) {
      const std::uint32_t child = children->Get(i);
      if (child <= id || child >= blocks->size() || seen[child]) {
        continue;
      }
      seen[child] = true;
      stack.emplace_back(child, depth + 1);
    }
  }
}

const fb::Block* TreeWalker::leaf_at(int luma_x, int luma_y, fb::BlockKind preferred) const {
  const fb::CellGrid* grid = fa.grid();
  const auto* blocks = fa.blocks();
  if (grid == nullptr || grid->block_id() == nullptr || blocks == nullptr || luma_x < 0 || luma_y < 0) {
    return nullptr;
  }
  const CellIndex cell{static_cast<std::uint32_t>(luma_x) / kCellSize, static_cast<std::uint32_t>(luma_y) / kCellSize};
  if (cell.x4 >= grid->cells_w() || cell.y4 >= grid->cells_h()) {
    return nullptr;
  }
  const std::uint32_t index = cell_index(*grid, cell);
  if (index >= grid->block_id()->size()) {
    return nullptr;
  }
  const std::uint32_t leaf_id = grid->block_id()->Get(index);
  const fb::Block* leaf = block(leaf_id);
  if (leaf == nullptr) {
    return nullptr;
  }
  if (leaf->kind() == preferred) {
    return leaf;
  }

  // Below the leaf: the PU/TU of a CU, the TXB of an AV1 block, the SubMB of a MB.
  if (const fb::Block* below = find_below(*blocks, leaf_id, luma_x, luma_y, preferred); below != nullptr) {
    return below;
  }

  // Above the leaf (CTU, SB, the root).
  for (const fb::Block* ancestor : ancestors(leaf_id)) {
    if (ancestor->kind() == preferred) {
      return ancestor;
    }
  }
  return leaf;
}

std::vector<const fb::Block*> TreeWalker::ancestors(std::uint32_t id) const {
  std::vector<const fb::Block*> chain;
  const fb::Block* current = block(id);
  while (current != nullptr) {
    const std::int32_t parent = current->parent();
    // Pre-order numbering (V-005) puts every parent before its children; anything else would loop.
    if (parent < 0 || static_cast<std::uint32_t>(parent) >= id) {
      break;
    }
    id = static_cast<std::uint32_t>(parent);
    current = block(id);
    if (current != nullptr) {
      chain.push_back(current);
    }
  }
  return chain;
}

}  // namespace ovt::model
