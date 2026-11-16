// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 derived and inferred values for the `gstparse` comparison of the header-parser golden suite (WI-1.572).

#pragma once

#include "ovt/cxx/syntax/h265/pps.hpp"
#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "syntax_golden.h"

namespace ovt::test::syntax_golden {

void add_h265_vps_derived(const ovt::syntax::h265::Vps& vps, ParsedUnit& u);
void add_h265_sps_derived(const ovt::syntax::h265::Sps& sps, ParsedUnit& u);
/// `sps` is the SPS the PPS names (nullptr when it has not been received).
void add_h265_pps_derived(const ovt::syntax::h265::Pps& pps, const ovt::syntax::h265::Sps* sps, ParsedUnit& u);
/// `active` is the slice's activated PPS (nullptr when its PPS or SPS is missing).
void add_h265_slice_derived(const ovt::syntax::h265::SliceSegmentHeader& sh, const ovt::syntax::h265::ActivePps* active,
                            ParsedUnit& u);

}  // namespace ovt::test::syntax_golden
