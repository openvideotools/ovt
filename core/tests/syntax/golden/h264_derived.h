// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 derived and inferred values for the `gstparse` comparison of the header-parser golden suite (WI-1.572).

#pragma once

#include <array>
#include <cstdint>

#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "syntax_golden.h"

namespace ovt::test::syntax_golden {

/// The twelve scaling matrices (6 × 4x4 in entries 0..15, 6 × 8x8) in zig-zag order, after the fall-back rules of
/// §7.4.2.1.1 Table 7-2: Flat_16 without a scaling matrix, the default for UseDefaultScalingMatrixFlag.
using ScalingMatrices = std::array<std::array<uint8_t, 64>, 12>;

/// Fall-back rule A (sequence level).
ScalingMatrices h264_sps_scaling(const ovt::syntax::h264::Sps& sps);
/// Fall-back rule B (picture level), or the sequence-level matrices when the PPS has none.
ScalingMatrices h264_pps_scaling(const ovt::syntax::h264::Pps& pps, const ovt::syntax::h264::Sps& sps);

void add_h264_sps_derived(const ovt::syntax::h264::Sps& sps, ParsedUnit& u);
void add_h264_pps_derived(const ovt::syntax::h264::Pps& pps, const ovt::syntax::h264::Sps* sps, ParsedUnit& u);
void add_h264_slice_derived(const ovt::syntax::h264::SliceHeader& sh, ParsedUnit& u);

}  // namespace ovt::test::syntax_golden
