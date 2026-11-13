// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.265 ref_pic_lists_modification() parser (H.265 §7.3.6.2, §7.4.7.2; chapter 7 §7.3.5 row 11; WI-1.113).

#include <cstdint>
#include <span>
#include <string_view>

#include "ovt/cxx/syntax/h265/slice_segment_header.hpp"
#include "syntax/h265/h265_internal.h"

namespace ovt::syntax::h265 {

namespace {

// list_entry_lX[i] indexes RefPicListTemp0/1, whose NumPicTotalCurr distinct entries repeat cyclically (8-8); a value
// beyond NumPicTotalCurr - 1 is a hard error so the later list construction never reads out of range.
void read_list(BitReader& br, SyntaxBuilder& b, std::string_view name, std::span<uint32_t> entries, uint32_t count,
               unsigned bits, uint32_t num_pic_total_curr) {
  for (uint32_t i = 0; i < count; ++i) {
    entries[i] = static_cast<uint32_t>(detail::read_u_at(b, name, static_cast<int>(i), bits, "u(v)"));
    detail::require_range(br, name, entries[i], 0, int64_t{num_pic_total_curr} - 1);
  }
}

}  // namespace

RefPicListsModification parse_ref_pic_lists_modification(BitReader& br, SyntaxBuilder& b, uint32_t slice_type,
                                                         uint32_t num_ref_idx_l0_active_minus1,
                                                         uint32_t num_ref_idx_l1_active_minus1,
                                                         uint32_t num_pic_total_curr) {
  RefPicListsModification m;
  const uint64_t start = br.bit_pos();
  auto s = b.scoped("ref_pic_lists_modification", "H.265 7.3.6.2");
  // The caller bounds the counts (num_ref_idx_lX_active_minus1 <= 14) and only calls with NumPicTotalCurr > 1.
  detail::require_range(br, "num_ref_idx_l0_active_minus1", num_ref_idx_l0_active_minus1, 0, kMaxRefIdx - 1);
  detail::require_range(br, "num_ref_idx_l1_active_minus1", num_ref_idx_l1_active_minus1, 0, kMaxRefIdx - 1);
  const unsigned bits = detail::ceil_log2(num_pic_total_curr);
  m.ref_pic_list_modification_flag_l0 = b.read_flag("ref_pic_list_modification_flag_l0");
  if (m.ref_pic_list_modification_flag_l0) {
    read_list(br, b, "list_entry_l0", m.list_entry_l0, num_ref_idx_l0_active_minus1 + 1, bits, num_pic_total_curr);
  }
  if (slice_type == kSliceB) {
    m.ref_pic_list_modification_flag_l1 = b.read_flag("ref_pic_list_modification_flag_l1");
    if (m.ref_pic_list_modification_flag_l1) {
      read_list(br, b, "list_entry_l1", m.list_entry_l1, num_ref_idx_l1_active_minus1 + 1, bits, num_pic_total_curr);
    }
  }
  m.span = br.span_since(start);
  return m;
}

}  // namespace ovt::syntax::h265
