// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ATSC A/53 GA94 user data: cc_data() counts only (chapter 7 §7.3.11, WI-1.120). Caption bytes are skipped.

#include "syntax/common/user_data_registry.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "ovt/cxx/bitreader.hpp"

namespace ovt::syntax {

AtscCaptions parse_atsc_cc(BitReader& br) {
  AtscCaptions r;
  r.user_identifier = static_cast<uint32_t>(br.u(32));
  if (r.user_identifier != kGa94UserIdentifier) {
    throw ParseError("SYN-GEN-0003", "user_identifier is not 'GA94'", br.bit_pos() - 32);
  }
  r.user_data_type_code = static_cast<uint8_t>(br.u(8));
  if (r.user_data_type_code != 0x03) {  // 0x06 bar_data() and everything else stays opaque
    return r;
  }
  r.is_cc_data = true;
  r.process_em_data_flag = br.u(1) != 0;
  r.process_cc_data_flag = br.u(1) != 0;
  r.additional_data_flag = br.u(1) != 0;
  r.cc_count = static_cast<uint8_t>(br.u(5));
  r.em_data = static_cast<uint8_t>(br.u(8));
  for (unsigned i = 0; i < r.cc_count; ++i) {
    br.skip_bits(5);  // marker_bits
    const bool cc_valid = br.u(1) != 0;
    const auto cc_type = static_cast<unsigned>(br.u(2));
    br.skip_bits(16);  // cc_data_1, cc_data_2
    if (!cc_valid) {
      ++r.invalid_packets;
    } else if (cc_type < 2) {
      ++r.valid_608_packets;
    } else {
      ++r.valid_708_packets;
    }
  }
  return r;
}

}  // namespace ovt::syntax
