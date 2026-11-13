// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF box header walk (chapter 08 §8.2.3, WI-1.201): a complete BoxNode tree with exact spans, no field
// decoding. Until the generated registry (WI-1.203) lands, box kinds come from a hard-coded table.

#ifndef OVT_CONTAINER_ISOBMFF_BOX_WALK_H
#define OVT_CONTAINER_ISOBMFF_BOX_WALK_H

#include <cstdint>
#include <memory>
#include <vector>

#include "container/isobmff/box_context.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

inline constexpr unsigned kMaxBoxDepth = 32;  // §8.2.3 step 1

using BoxNodes = std::vector<std::unique_ptr<fb::BoxNodeT>>;
using DiagList = std::vector<std::unique_ptr<fb::DiagnosticT>>;

// What the registry will say about a fourcc (WI-1.203 replaces this table).
enum class BoxKind : uint8_t { Unknown, Leaf, Container, Opaque };
[[nodiscard]] BoxKind box_kind(const FourCC& type) noexcept;

// The state the walk shares between siblings and across levels (what the spec keeps in `ctx`).
struct WalkState {
  const io::RandomAccessFile* file = nullptr;
  bool saw_moov = false;
  bool saw_mdat = false;
  bool mdat_before_moov = false;
  bool saw_moof = false;
};

// §8.2.3 `walk_children`. `parent_offset` is the file offset of the enclosing box (0 for the root); diagnostics that
// belong to the parent (CNT-0105) go to `parent_diags`; the rest are attached to the box they describe. Diagnostic
// spans are relative to `parent_offset` for parent-level findings and to the box start for box-level ones, in bits.
// Never throws except for an I/O failure of the file (io::IoError).
[[nodiscard]] BoxNodes walk_children(WalkState& st, uint64_t parent_offset, uint64_t parent_payload_begin,
                                     uint64_t parent_end, unsigned depth, const BoxContext& ctx,
                                     DiagList& parent_diags);

struct WalkResult {
  BoxNodes top_level;    // file order
  DiagList diagnostics;  // tree-level findings (CNT-0002, 0004, 0005, 0006); spans are file-relative bits
  BoxContext root_ctx;   // final flags: quicktime, fragment_only, saw_moov, mdat_before_moov
};

// Top level: `walk_children(0, 0, file.size(), 0, root_ctx)` plus the end-of-walk checks of §8.2.3.
[[nodiscard]] WalkResult walk(const io::RandomAccessFile& file);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_BOX_WALK_H
