// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Module-internal entry points of the unit scanners (WI-1.107). Not part of ovt/cxx/syntax/unit_scan.hpp: they
// exist so that unit tests can drive the windowed Annex B scan with small windows and check every start code
// position relative to a window boundary without building a multi-megabyte file.

#ifndef OVT_SYNTAX_COMMON_UNIT_SCAN_INTERNAL_H
#define OVT_SYNTAX_COMMON_UNIT_SCAN_INTERNAL_H

#include <cstddef>

#include "ovt/cxx/syntax/unit_scan.hpp"

namespace ovt::syntax::detail {

inline constexpr size_t kAnnexBWindowBytes = size_t{1} << 20U;  // §7.3.3: 1 MiB view() windows

// scan_annexb(file, cb, diag) with a window of `window_bytes` (> 0) instead of kAnnexBWindowBytes.
void scan_annexb_windowed(const io::RandomAccessFile& file, size_t window_bytes, const UnitCallback& cb,
                          const DiagnosticCallback& diag);

}  // namespace ovt::syntax::detail

#endif  // OVT_SYNTAX_COMMON_UNIT_SCAN_INTERNAL_H
