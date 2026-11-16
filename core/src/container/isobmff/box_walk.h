// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF box walk (chapter 08 §8.2.3, WI-1.201): a complete BoxNode tree with exact spans. Box kinds,
// descriptions and FullBox headers come from the generated registry (box_registry.h, WI-1.203); fields come from the
// hand decoders it joins to each box type.

#ifndef OVT_CONTAINER_ISOBMFF_BOX_WALK_H
#define OVT_CONTAINER_ISOBMFF_BOX_WALK_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "container/isobmff/box_context.h"
#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

inline constexpr unsigned kMaxBoxDepth = 32;         // §8.2.3 step 1
inline constexpr uint64_t kMaxBoxNodes = 1'000'000;  // §8.2.3 node bound per walk (CNT-0111)

using BoxNodes = std::vector<std::unique_ptr<fb::BoxNodeT>>;
using DiagList = std::vector<std::unique_ptr<fb::DiagnosticT>>;

// The state the walk shares between siblings and across levels (what the spec keeps in `ctx`).
struct WalkState {
  const io::RandomAccessFile* file = nullptr;
  bool saw_moov = false;
  bool saw_mdat = false;
  bool mdat_before_moov = false;
  bool saw_moof = false;
  uint64_t nodes = 0;           // BoxNodes created so far in this walk
  bool node_bound_hit = false;  // CNT-0111 already emitted
  uint64_t moof_count = 0;      // top-level `moof` boxes met
  // Lazy policy of §8.2.4 rule 3: when set, a top-level `moof` gets a header walk only (its `mfhd`) and is recorded
  // here with the context it was walked in, so expand_node() can finish it later.
  std::vector<struct DeferredBox>* deferred = nullptr;
};

// A `moof` whose `traf` children are not walked yet (CNT-0007 sits on its node).
struct DeferredBox {
  uint64_t offset = 0;  // file offset of the box
  BoxContext ctx;       // the sibling context the box was walked in (what a full walk would have used)
};

// §8.2.4 rule 3: a fragmented file with at most this many `moof` boxes is decoded eagerly.
inline constexpr uint64_t kEagerMoofLimit = 64;

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

// Top level: `walk_children(0, 0, file.size(), 0, root_ctx)` plus the end-of-walk checks of §8.2.3. With `deferred`
// null the walk is complete (every `moof` decoded). With a vector, the §8.2.4 rule 3 policy applies: more than
// kEagerMoofLimit top-level `moof`s keep only their `mfhd` (listed in `deferred`, in file order); at most that many are
// expanded before returning, so `deferred` is empty then.
[[nodiscard]] WalkResult walk(const io::RandomAccessFile& file, std::vector<DeferredBox>* deferred = nullptr);

// §8.2.4 rule 6: runs step 2.12 for a deferred top-level `moof`: replaces its children by the full walk of its payload
// and drops its CNT-0007. `ctx` is DeferredBox::ctx. At most kMaxBoxNodes nodes are created (CNT-0111 as usual).
void expand_node(const io::RandomAccessFile& file, fb::BoxNodeT& node, const BoxContext& ctx);

// §8.2.4 rule 4: the file offset of the `mfra` box found through the `mfro` in the last 16 bytes of the file, or
// nullopt when there is none, or when `mfro` and the `mfra` header disagree. Reads at most 16 + 8 bytes.
[[nodiscard]] std::optional<uint64_t> locate_mfra(const io::RandomAccessFile& file);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_BOX_WALK_H
