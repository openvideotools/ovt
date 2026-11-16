// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Frame classification of the StreamIndex builder (chapter 7 §7.4.2 S6, WI-1.121): the FrameInfo fields that
// come from a frame's first header and the codec's order / slot model. Module-internal, not installed.

#ifndef OVT_INDEX_FRAME_CLASSIFY_H
#define OVT_INDEX_FRAME_CLASSIFY_H

#include "index/av1_slots.h"
#include "index/builder_internal.h"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"

namespace ovt::index {

/// S6 "AV1" with the §7.4.3 table: `r` is the slot step of the frame (for show_existing_frame it carries the
/// loaded slot), `current_frame_id` the header's value. declared_refs and dpb_after are left empty (WI-1.124).
void classify_av1(FrameRec& f, const Av1FrameRecord& r, const syntax::av1::SequenceHeader& seq,
                  uint32_t current_frame_id);

}  // namespace ovt::index

#endif  // OVT_INDEX_FRAME_CLASSIFY_H
