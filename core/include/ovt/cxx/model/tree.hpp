// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Block-tree traversal and nested extension access (chapter 06 §6.3.2, §6.3.3, WI-0.084).
// C++ helper header (D22): semi-public, no ABI promise, never included from a C header.
//
// The walker never trusts the tree: child and parent ids outside `blocks`, or that break the pre-order rule
// (a child id greater than its parent's, V-005), are skipped, and every block is visited at most once. A
// malformed FrameAnalysis therefore yields a partial walk, never a crash or an endless loop.

#ifndef OVT_CXX_MODEL_TREE_HPP
#define OVT_CXX_MODEL_TREE_HPP

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "ovt_model_generated.h"

namespace ovt::model {

/// Read-only view of the block tree of one FrameAnalysis. Holds a reference: the buffer must outlive it.
struct TreeWalker {
  const fb::FrameAnalysis& fa;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): WI-0.084 interface

  /// `blocks[id]`, or nullptr when @p id is outside `blocks`.
  [[nodiscard]] const fb::Block* block(std::uint32_t id) const noexcept;

  /// Visits the tree from `blocks[0]` in pre-order, children in their stored order (§6.3.7); the root has
  /// depth 0. Does nothing when `blocks` is empty.
  void pre_order(const std::function<void(const fb::Block&, int depth)>& visit) const;

  /// The block of kind @p preferred that contains luma sample (@p luma_x, @p luma_y): starts from the grid
  /// leaf `grid.block_id[cell]` (§6.3.2 rule 4), searches its subtree for the deepest block of that kind whose
  /// rect contains the sample (e.g. the PU of a CU, or the leaf TU of a split transform tree), then its
  /// ancestors, nearest first (e.g. the CTU). When no block
  /// of that kind is on the path, returns the grid leaf itself. Returns nullptr when the sample lies outside
  /// the grid, or the grid has no `block_id` vector, or the cell names a block outside `blocks`.
  // SPEC-ASSUMPTION(#98): the fallback to the grid leaf when @p preferred is not on the path.
  [[nodiscard]] const fb::Block* leaf_at(int luma_x, int luma_y, fb::BlockKind preferred) const;

  /// The parent chain of block @p id, nearest first and ending at the root: `ancestors(11) == {1, 0}` for
  /// the §6.10 example. Empty for the root and for an id outside `blocks`.
  [[nodiscard]] std::vector<const fb::Block*> ancestors(std::uint32_t id) const;
};

/// The root table of nested buffer @p buffer when @p type_name names table T and the buffer verifies, else
/// nullptr. @p type_name is the unqualified table name stored next to the buffer ("HevcCu", "Av1Block",
/// "HevcFilterParams", …; §6.3.3), compared with `T::GetFullyQualifiedName()` minus the "ovt.fb." prefix.
/// Verification uses flatbuffers::Verifier over the nested bytes with its default limits, the same check
/// FlatBuffers applies to a `nested_flatbuffer` field; alignment is checked relative to the nested buffer.
template <class T>
[[nodiscard]] const T* nested_as(const flatbuffers::Vector<std::uint8_t>* buffer,
                                 const flatbuffers::String* type_name) {
  if (buffer == nullptr || type_name == nullptr) {  // an empty buffer fails verification below
    return nullptr;
  }
  constexpr std::string_view kNamespacePrefix = "ovt.fb.";
  std::string_view expected = T::GetFullyQualifiedName();
  if (expected.starts_with(kNamespacePrefix)) {
    expected.remove_prefix(kNamespacePrefix.size());
  }
  if (std::string_view(type_name->c_str(), type_name->size()) != expected) {
    return nullptr;
  }
  flatbuffers::Verifier verifier(buffer->data(), buffer->size());
  if (!verifier.VerifyBuffer<T>(nullptr)) {
    return nullptr;
  }
  return flatbuffers::GetRoot<T>(buffer->data());
}

/// `Block.ext` as codec extension table T (§6.3.3): nullptr when `ext_type` does not name T, when `ext` is
/// absent or empty, or when the nested buffer fails verification (rule V-022).
template <class T>
[[nodiscard]] const T* ext_as(const fb::Block& block) {
  return nested_as<T>(block.ext(), block.ext_type());
}

}  // namespace ovt::model

#endif  // OVT_CXX_MODEL_TREE_HPP
