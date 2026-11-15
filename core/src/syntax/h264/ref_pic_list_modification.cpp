// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 ref_pic_list_modification() (§7.3.3.1) and ref_pic_list_mvc_modification() (H.7.3.3.1.1): chapter 7
// §7.3.4 row 25, WI-1.109. Element order follows H.264 (08/2021) line by line.

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

// Table 7-7 and Table H-3 names of modification_of_pic_nums_idc 0..5.
constexpr std::array<std::string_view, 6> kIdcNames{"subtract from picture number prediction",
                                                    "add to picture number prediction",
                                                    "long-term picture",
                                                    "end of list",
                                                    "subtract from inter-view index prediction",
                                                    "add to inter-view index prediction"};

// One list: the flag, then the do-while loop until modification_of_pic_nums_idc == 3. Every iteration reads at
// least one bit, so the loop ends with the RBSP; ops beyond num_ref_idx_lX_active_minus1 + 1 are reported once and
// not stored (§7.4.3.1 bounds their number).
void read_list(BitReader& br, SyntaxBuilder& b, char list, uint32_t num_ref_idx_active_minus1, bool mvc, bool& flag,
               std::vector<RefPicListModificationOp>& ops) {
  flag = b.read_flag(list == '0' ? "ref_pic_list_modification_flag_l0" : "ref_pic_list_modification_flag_l1");
  if (!flag) {
    return;
  }
  const uint32_t max_idc = mvc ? 5 : 3;
  const uint32_t max_ops = num_ref_idx_active_minus1 + 1;
  bool reported = false;
  // The elements are recorded without array indices, as trace_headers prints them (§7.7.1).
  for (;;) {
    RefPicListModificationOp op;
    op.modification_of_pic_nums_idc = b.read_ue("modification_of_pic_nums_idc");
    detail::require_range(br, "modification_of_pic_nums_idc", op.modification_of_pic_nums_idc, 0, max_idc);
    b.set_enum(kIdcNames.at(op.modification_of_pic_nums_idc));
    const uint32_t idc = op.modification_of_pic_nums_idc;
    if (idc == 0 || idc == 1) {
      op.abs_diff_pic_num_minus1 = b.read_ue("abs_diff_pic_num_minus1");
    } else if (idc == 2) {
      op.long_term_pic_num = b.read_ue("long_term_pic_num");
    } else if (idc == 4 || idc == 5) {
      op.abs_diff_view_idx_minus1 = b.read_ue("abs_diff_view_idx_minus1");
    }
    if (idc == 3) {
      break;
    }
    if (ops.size() < max_ops) {
      ops.push_back(op);
    } else if (!reported) {
      reported = true;
      b.diag(Severity::Error, detail::kRangeCode,
             fmt::format("ref_pic_list_modification_l{}: more than num_ref_idx_l{}_active_minus1 + 1 = {} operations",
                         list, list, max_ops),
             "H.264 7.4.3.1");
    }
  }
}

}  // namespace

RefPicListModification parse_ref_pic_list_modification(BitReader& br, SyntaxBuilder& b, uint32_t slice_type,
                                                       uint32_t num_ref_idx_l0_active_minus1,
                                                       uint32_t num_ref_idx_l1_active_minus1, bool mvc) {
  RefPicListModification m;
  m.mvc = mvc;
  const auto start = br.mark();
  auto scope = b.scoped(mvc ? "ref_pic_list_mvc_modification" : "ref_pic_list_modification",
                        mvc ? "H.264 H.7.3.3.1.1" : "H.264 7.3.3.1");
  detail::require_range(br, "num_ref_idx_l0_active_minus1", num_ref_idx_l0_active_minus1, 0, kMaxRefIdx - 1);
  detail::require_range(br, "num_ref_idx_l1_active_minus1", num_ref_idx_l1_active_minus1, 0, kMaxRefIdx - 1);
  const uint32_t t = slice_type % 5;
  if (t != kSliceI && t != kSliceSI) {
    read_list(br, b, '0', num_ref_idx_l0_active_minus1, mvc, m.ref_pic_list_modification_flag_l0, m.ops_l0);
  }
  if (t == kSliceB) {
    read_list(br, b, '1', num_ref_idx_l1_active_minus1, mvc, m.ref_pic_list_modification_flag_l1, m.ops_l1);
  }
  m.span = br.span_since(start);
  return m;
}

}  // namespace ovt::syntax::h264
