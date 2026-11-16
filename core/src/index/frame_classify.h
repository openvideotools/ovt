// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Frame classification of the StreamIndex builder (chapter 7 §7.4.2 S6, WI-1.121): the FrameInfo fields that
// come from a frame's first header and the codec's order / slot model. Module-internal, not installed.

#ifndef OVT_INDEX_FRAME_CLASSIFY_H
#define OVT_INDEX_FRAME_CLASSIFY_H

#include <cstdint>

#include "index/av1_slots.h"
#include "index/builder_internal.h"
#include "index/h264_index.h"
#include "index/h265_index.h"
#include "ovt/cxx/syntax/av1/sequence_header.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vui.hpp"

namespace ovt::index {

/// S6 "AV1" with the §7.4.3 table: `r` is the slot step of the frame (for show_existing_frame it carries the
/// loaded slot), `current_frame_id` the header's value. declared_refs and dpb_after are left empty (WI-1.124).
void classify_av1(FrameRec& f, const Av1FrameRecord& r, const syntax::av1::SequenceHeader& seq,
                  uint32_t current_frame_id);

/// S6 "H.264" for the picture whose first slice is `sh`: `bd` is that slice's §7.3.9 decision, `poc` its
/// §7.3.10.1 result. A field picture has the field height (chapter 6 §6.2.4).
void classify_h264(FrameRec& f, const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh,
                   const syntax::h264::Sps& sps, const h264::BoundaryDecision& bd, const h264::PocResult& poc);

/// S6 "H.265" for the picture whose first slice segment is `sh`. A RASL picture stays shown: the full index
/// has no random-access start (§7.3.10.2).
void classify_h265(FrameRec& f, const syntax::h265::NalHeader& nal, const syntax::h265::SliceSegmentHeader& sh,
                   const syntax::h265::Sps& sps, const h265::PocResult& poc);

/// FrameType of one slice: SP counts as P and SI as I.
[[nodiscard]] fb::FrameType h264_slice_frame_type(const syntax::h264::SliceHeader& sh) noexcept;
[[nodiscard]] fb::FrameType h265_slice_frame_type(const syntax::h265::SliceSegmentHeader& sh) noexcept;

/// S5 "majority vote" over I, P and B: a frame with any B slice is B, else any P slice makes it P, else I.
[[nodiscard]] fb::FrameType merge_frame_type(fb::FrameType a, fb::FrameType b) noexcept;

/// Clock ticks per picture of chapter 6 §6.2.6: elemental_duration_in_tc_minus1 + 1 of the highest sub-layer
/// when the VUI HRD fixes the picture rate, else 1.
[[nodiscard]] int64_t h265_ticks_per_picture(const syntax::h265::VuiParameters& vui) noexcept;

}  // namespace ovt::index

#endif  // OVT_INDEX_FRAME_CLASSIFY_H
