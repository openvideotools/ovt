// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the unit scanners (chapter 7 §7.3.3, WI-1.107; fuzz item of issue #127, WI-1.105). Input
// layout: byte 0 selects the scanner (modulo 7: Annex B span, length-prefixed with N = 1, 2 and 4, AV1
// low-overhead length-delimited and Section 5, AV1 Annex B), the rest is the buffer scanned. Invariants, each
// an abort when violated: every unit lies inside the buffer, is non-empty, starts after the previous unit
// ended, has file_offset equal to the base offset plus its position in the buffer, and its prefix_span ends at
// or before the unit (and is empty at file_offset when it has length 0). Only the scanners' own diagnostics may
// be reported; no exception may escape.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "ovt/cxx/syntax/unit_scan.hpp"

namespace {

using ovt::syntax::ObuSpanKind;
using ovt::syntax::ScannedUnit;

constexpr uint64_t kBase = 4096;

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const unsigned mode = data[0] % 7U;
  const std::span<const uint8_t> buf(data + 1, size - 1);
  uint64_t next_free = kBase;  // end of the previous unit

  const auto cb = [&](const ScannedUnit& u) {
    require(!u.bytes.empty());
    require(u.bytes.data() >= buf.data() && u.bytes.data() + u.bytes.size() <= buf.data() + buf.size());
    const auto pos = static_cast<uint64_t>(u.bytes.data() - buf.data());
    require(u.file_offset == kBase + pos);
    require(u.file_offset >= next_free);
    require(u.prefix_span.offset >= kBase);
    require(u.prefix_span.offset + u.prefix_span.length <= u.file_offset);
    if (u.prefix_span.length == 0) {
      require(u.prefix_span.offset == u.file_offset);
    }
    next_free = u.file_offset + u.bytes.size();
  };
  const auto diag = [](const ovt::syntax::ScanDiagnostic& d) {
    require(!d.code.empty());
    require(d.file_offset >= kBase);
  };

  switch (mode) {
    case 0:
      ovt::syntax::scan_annexb(buf, kBase, ovt::syntax::kNoContainerSample, cb, diag);
      break;
    case 1:
      ovt::syntax::split_length_prefixed(buf, 1, kBase, 7, cb, diag);
      break;
    case 2:
      ovt::syntax::split_length_prefixed(buf, 2, kBase, 7, cb, diag);
      break;
    case 3:
      ovt::syntax::split_length_prefixed(buf, 4, kBase, 7, cb, diag);
      break;
    case 4:
      ovt::syntax::scan_obu_low_overhead(buf, kBase, 7, ObuSpanKind::LengthDelimited, cb, diag);
      break;
    case 5:
      ovt::syntax::scan_obu_low_overhead(buf, kBase, 7, ObuSpanKind::Section5Stream, cb, diag);
      break;
    default:
      ovt::syntax::scan_obu_annexb(buf, kBase, 7, cb, diag);
      break;
  }
  return 0;
}
