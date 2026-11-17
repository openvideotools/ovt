// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Group B hand decoders (chapter 08 §8.3.2, WI-1.206): the one entry point other code needs. The decoders
// themselves register through OVT_BOX_DECODER in sample_entries.cpp.

#ifndef OVT_CONTAINER_ISOBMFF_DECODERS_SAMPLE_ENTRIES_H
#define OVT_CONTAINER_ISOBMFF_DECODERS_SAMPLE_ENTRIES_H

#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// CNT-1010 (Error) on a decoded `stsd` node whose `entry_count` differs from the number of sample-entry children
// the walk found. A decoder cannot count its own children, so the walker (or the §8.5 validation driver) calls this
// once `node.children` is filled (SPEC-ASSUMPTION(#592)). A node without an `entry_count` field (undecoded) is left
// alone.
void check_stsd_entry_count(fb::BoxNodeT& stsd);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_DECODERS_SAMPLE_ENTRIES_H
