// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Internal header of the group E part 1 decoders (chapter 08 §8.3.5, WI-1.211): the post-children check the walker
// calls for `moof` and `traf`, because a decoder may not look at its siblings (§8.2.9).

#ifndef OVT_CONTAINER_ISOBMFF_DECODERS_FRAGMENT_BOXES_H
#define OVT_CONTAINER_ISOBMFF_DECODERS_FRAGMENT_BOXES_H

#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// SPEC-ASSUMPTION(#671): for a `moof`, raises CNT-1011 (Error) when its first child is not `mfhd` and puts the
// `mfhd.sequence_number` in the node description; for a `traf`, raises CNT-1011 when it has no `tfhd`. Other boxes are
// left alone. Idempotent: a diagnostic already on the node is not added twice.
void check_fragment_children(fb::BoxNodeT& node);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_DECODERS_FRAGMENT_BOXES_H
