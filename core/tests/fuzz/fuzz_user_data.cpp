// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the user-data parsers (chapter 7 §7.3.11, chapter 14 §14.7.1, WI-0.557): ITU-T T.35
// (parse_t35, HDR10+ and ATSC GA94 inside), user_data_unregistered (UUID + x264/x265 option text) and the
// AV1 trailing-bits helper. Input layout: byte 0 selects the entry point (modulo 5), the rest is the payload.
// A ParseError is the expected answer to malformed bytes; any other exception, a crash or a sanitizer report
// is a finding. Views returned by the parsers must lie inside the input.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "syntax/common/user_data_registry.h"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

bool inside(std::span<const uint8_t> outer, std::span<const uint8_t> inner) {
  return inner.empty() || (inner.data() >= outer.data() && inner.data() + inner.size() <= outer.data() + outer.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  const unsigned mode = data[0] % 5U;
  const std::span<const uint8_t> payload(data + 1, size - 1);
  try {
    switch (mode) {
      case 0:
      case 1: {
        const auto carrier = mode == 0 ? ovt::syntax::T35Carrier::Sei : ovt::syntax::T35Carrier::Av1Metadata;
        const ovt::syntax::T35Result r = ovt::syntax::parse_t35(payload, carrier);
        require(inside(payload, r.payload));
        break;
      }
      case 2: {
        const ovt::syntax::UserDataUnregistered r = ovt::syntax::parse_user_data_unregistered(payload);
        require(r.uuid_text.size() == 36);
        require(inside(payload, r.payload));
        break;
      }
      case 3: {
        const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
        (void)ovt::syntax::split_encoder_options(text);
        break;
      }
      default: {
        const std::span<const uint8_t> s = ovt::syntax::strip_av1_trailing_bits(payload);
        require(s.size() <= payload.size() && inside(payload, s));
        break;
      }
    }
  } catch (const ovt::ParseError&) {
    // expected for malformed input
  }
  return 0;
}
