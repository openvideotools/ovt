// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF Reader behind ovt_container_open and ovt_container_expand (chapter 08 §8.2.4, WI-1.204). The tree lives
// as mutable BoxNodeT objects; tree() serialises it on demand, so every expand yields a new immutable buffer.

#include "container/isobmff/lazy.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "container/isobmff/box_walk.h"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

bool IsobmffReader::open() {
  // §8.2.4 rule 4: the index location first, so it is known before the top level is walked.
  const std::optional<uint64_t> mfra = locate_mfra(file_);
  WalkResult w = walk(file_, &deferred_);
  if (std::ranges::any_of(w.diagnostics, [](const auto& d) { return d->code == "CNT-0006"; })) {
    return false;
  }
  tree_.format = "isobmff";
  tree_.root = std::move(w.top_level);
  tree_.diagnostics = std::move(w.diagnostics);
  index(tree_.root);
  // The walk confirms the located `mfra`; when the top level disagrees, the walk wins.
  if (mfra && std::ranges::any_of(
                  tree_.root, [&](const auto& b) { return b->file_span->offset() == *mfra && b->fourcc == "mfra"; })) {
    mfra_offset_ = mfra;
  }
  return true;
}

std::vector<uint8_t> IsobmffReader::tree() const {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::ContainerTree::Pack(fbb, &tree_));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

ovt_status IsobmffReader::expand(uint64_t box_offset) {
  if (!offsets_.contains(box_offset)) {
    return OVT_E_INVALID_ARG;
  }
  const auto it = std::ranges::find_if(deferred_, [&](const DeferredBox& d) { return d.offset == box_offset; });
  if (it == deferred_.end()) {
    return OVT_OK;
  }
  const auto node =
      std::ranges::find_if(tree_.root, [&](const auto& b) { return b->file_span->offset() == box_offset; });
  expand_node(file_, **node, it->ctx);
  index((*node)->children);
  deferred_.erase(it);
  return OVT_OK;
}

// Depth is bounded by kMaxBoxDepth.
// NOLINTNEXTLINE(misc-no-recursion)
void IsobmffReader::index(const BoxNodes& nodes) {
  for (const auto& n : nodes) {
    offsets_.insert(n->file_span->offset());
    index(n->children);
  }
}

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why) {
  auto r = std::make_unique<IsobmffReader>(file);
  if (!r->open()) {
    if (why != nullptr) {
      *why = OVT_E_CORRUPT;
    }
    return nullptr;
  }
  return r;
}

}  // namespace ovt::container::isobmff
