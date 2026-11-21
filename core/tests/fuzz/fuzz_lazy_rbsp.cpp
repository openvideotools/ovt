// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the lazy emulation-prevention stripping (chapter 7 §7.4.6; chapter 14 §14.7.1; WI-1.125): the
// first byte picks the chunk size (1..256 bytes, doubled until the unit is covered) and whether the rest is only
// scanned for EPB positions; the rest is the NAL unit. Whatever the bytes are, LazyRbsp must give the RBSP bytes
// and the EPB positions of rbsp_from_nal(). A crash, a sanitizer report or a difference is a finding.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "index/lazy_syntax.h"
#include "ovt/cxx/rbsp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const std::span<const uint8_t> nal(data + 1, size - 1);
  const size_t chunk = size_t{data[0]} + 1;
  const bool scan_only = (data[0] & 0x80U) != 0;
  const ovt::Rbsp want = ovt::rbsp_from_nal(nal);
  ovt::index::LazyRbsp lazy;
  lazy.reset(nal);
  if (scan_only) {
    lazy.extend(chunk);
    lazy.scan_rest();
    if (!lazy.complete() || lazy.rbsp().size() > want.rbsp.size() ||
        !std::equal(lazy.rbsp().begin(), lazy.rbsp().end(), want.rbsp.begin())) {
      std::abort();
    }
  } else {
    for (size_t limit = chunk; !lazy.complete(); limit *= 2) {
      lazy.extend(limit);
      if (lazy.stripped_raw() != std::min(limit, nal.size())) {
        std::abort();
      }
    }
    if (lazy.rbsp().size() != want.rbsp.size() ||
        !std::equal(lazy.rbsp().begin(), lazy.rbsp().end(), want.rbsp.begin())) {
      std::abort();
    }
  }
  if (lazy.epb_positions() != want.epb_positions) {
    std::abort();
  }
  return 0;
}
