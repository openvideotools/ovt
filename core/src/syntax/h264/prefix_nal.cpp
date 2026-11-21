// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 prefix_nal_unit_rbsp() (chapter 7 §7.3.4 row 23, WI-1.109): §7.3.2.12, with prefix_nal_unit_svc()
// (G.7.3.2.12.1) and dec_ref_base_pic_marking() (G.7.3.3.5) line by line. Clause letters follow H.264 (08/2021),
// which the 06/2026 edition renumbers F.

#include "ovt/cxx/syntax/h264/prefix_nal.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

// Table G-3 names of memory_management_base_control_operation 0..2.
constexpr std::array<std::string_view, 3> kMmcoBaseNames{"end", "short-term base unused", "long-term base unused"};

// dec_ref_base_pic_marking() (G.7.3.3.5). As in dec_ref_pic_marking(), every iteration reads at least one bit, so the
// loop ends with the RBSP; operations beyond kMaxMmcoOps are reported once and not stored.
DecRefBasePicMarking parse_dec_ref_base_pic_marking(BitReader& br, SyntaxBuilder& b) {
  DecRefBasePicMarking m;
  auto scope = b.scoped("dec_ref_base_pic_marking", "H.264 G.7.3.3.5");
  m.adaptive_ref_base_pic_marking_mode_flag = b.read_flag("adaptive_ref_base_pic_marking_mode_flag");
  if (!m.adaptive_ref_base_pic_marking_mode_flag) {
    return m;
  }
  bool reported = false;
  for (;;) {
    MmcoBaseOp op;
    op.memory_management_base_control_operation = b.read_ue("memory_management_base_control_operation");
    detail::require_range(br, "memory_management_base_control_operation", op.memory_management_base_control_operation,
                          0, 2);
    b.set_enum(kMmcoBaseNames.at(op.memory_management_base_control_operation));
    if (op.memory_management_base_control_operation == 0) {
      return m;
    }
    if (op.memory_management_base_control_operation == 1) {
      op.difference_of_base_pic_nums_minus1 = b.read_ue("difference_of_base_pic_nums_minus1");
    } else {
      op.long_term_base_pic_num = b.read_ue("long_term_base_pic_num");
    }
    if (m.ops.size() < kMaxMmcoOps) {
      m.ops.push_back(op);
    } else if (!reported) {
      reported = true;
      b.diag(Severity::Error, detail::kRangeCode,
             fmt::format("dec_ref_base_pic_marking: more than {} memory_management_base_control_operation entries",
                         kMaxMmcoOps),
             "H.264 G.7.4.3.5");
    }
  }
}

// while (more_rbsp_data()) additional_prefix_nal_unit_extension_data_flag; rbsp_trailing_bits().
void read_extension_data_and_trailing_bits(BitReader& br, SyntaxBuilder& b, PrefixNalUnitSvc& p) {
  while (br.more_rbsp_data()) {
    (void)b.read_flag("additional_prefix_nal_unit_extension_data_flag");
    ++p.additional_prefix_nal_unit_extension_data_bits;
  }
  detail::read_rbsp_trailing_bits(br, b);
}

void parse_prefix_nal_unit_svc(BitReader& br, SyntaxBuilder& b, const NalHeader& nal, PrefixNalUnitSvc& p) {
  auto scope = b.scoped("prefix_nal_unit_svc", "H.264 G.7.3.2.12.1");
  if (nal.nal_ref_idc != 0) {
    p.store_ref_base_pic_flag = b.read_flag("store_ref_base_pic_flag");
    if ((nal.svc.use_ref_base_pic_flag || p.store_ref_base_pic_flag) && !nal.svc.idr_flag) {
      p.has_dec_ref_base_pic_marking = true;
      p.dec_ref_base_pic_marking = parse_dec_ref_base_pic_marking(br, b);
    }
    p.additional_prefix_nal_unit_extension_flag = b.read_flag("additional_prefix_nal_unit_extension_flag");
    if (p.additional_prefix_nal_unit_extension_flag) {
      read_extension_data_and_trailing_bits(br, b, p);
    } else {
      detail::read_rbsp_trailing_bits(br, b);
    }
  } else if (br.bits_left() > 0) {
    // else if (more_rbsp_data()): an RBSP holding only rbsp_trailing_bits() has no data flags, but its stop bit is
    // still recorded so that the tree covers the payload.
    read_extension_data_and_trailing_bits(br, b, p);
  }
}

}  // namespace

PrefixNalUnit parse_prefix_nal_unit(BitReader& br, SyntaxBuilder& b, const NalHeader& nal) {
  PrefixNalUnit p;
  p.svc = nal.svc_extension_flag;
  const auto start = br.mark();
  auto scope = b.scoped("prefix_nal_unit_rbsp", "H.264 7.3.2.12");
  if (p.svc) {
    parse_prefix_nal_unit_svc(br, b, nal, p.prefix_svc);
  }
  p.span = br.span_since(start);
  return p;
}

}  // namespace ovt::syntax::h264
