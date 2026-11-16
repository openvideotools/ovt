// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 dec_ref_pic_marking() (§7.3.3.3) with the MMCO op list and MaxLongTermFrameIdx: chapter 7 §7.3.4 row 27,
// WI-1.109. Element order follows H.264 (08/2021) §7.3.3.3 line by line; the loop elements carry no array index,
// as trace_headers prints them (§7.7.1).

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

namespace {

// Table 7-9 names of memory_management_control_operation 0..6.
constexpr std::array<std::string_view, 7> kMmcoNames{
    "end",        "short-term unused",   "long-term unused", "short-term to long-term", "set max long-term frame idx",
    "all unused", "current to long-term"};

std::string max_long_term_text(int32_t v) {
  return v == kNoLongTermFrameIndices ? std::string{"MaxLongTermFrameIdx = no long-term frame indices"}
                                      : fmt::format("MaxLongTermFrameIdx = {}", v);
}

// The adaptive_ref_pic_marking_mode_flag loop until memory_management_control_operation 0. Every iteration reads at
// least one bit, so the loop ends with the RBSP; entries beyond kMaxMmcoOps are reported once and not stored.
void read_mmco_ops(BitReader& br, SyntaxBuilder& b, uint32_t max_num_ref_frames, DecRefPicMarking& m) {
  bool reported = false;
  for (;;) {
    MmcoOp op;
    op.memory_management_control_operation = b.read_ue("memory_management_control_operation");
    detail::require_range(br, "memory_management_control_operation", op.memory_management_control_operation, 0, 6);
    b.set_enum(kMmcoNames.at(op.memory_management_control_operation));
    const uint32_t mmco = op.memory_management_control_operation;
    if (mmco == 0) {
      return;
    }
    if (mmco == 1 || mmco == 3) {
      op.difference_of_pic_nums_minus1 = b.read_ue("difference_of_pic_nums_minus1");
    }
    if (mmco == 2) {
      op.long_term_pic_num = b.read_ue("long_term_pic_num");
    }
    if (mmco == 3 || mmco == 6) {
      op.long_term_frame_idx = b.read_ue("long_term_frame_idx");
    }
    if (mmco == 4) {
      op.max_long_term_frame_idx_plus1 = b.read_ue("max_long_term_frame_idx_plus1");
      // §7.4.3.3: 0..max_num_ref_frames.
      detail::check_range(b, "max_long_term_frame_idx_plus1", op.max_long_term_frame_idx_plus1, 0, max_num_ref_frames,
                          "H.264 7.4.3.3");
      m.MaxLongTermFrameIdx = op.max_long_term_frame_idx_plus1 == 0
                                  ? kNoLongTermFrameIndices
                                  : static_cast<int32_t>(op.max_long_term_frame_idx_plus1 - 1);
    }
    if (mmco == 5) {
      m.has_mmco5 = true;
      m.MaxLongTermFrameIdx = kNoLongTermFrameIndices;  // §8.2.5.4.6
    }
    if (m.mmco.size() < kMaxMmcoOps) {
      m.mmco.push_back(op);
    } else if (!reported) {
      reported = true;
      b.diag(Severity::Error, detail::kRangeCode,
             fmt::format("dec_ref_pic_marking: more than {} memory_management_control_operation entries", kMaxMmcoOps),
             "H.264 7.4.3.3");
    }
  }
}

}  // namespace

DecRefPicMarking parse_dec_ref_pic_marking(BitReader& br, SyntaxBuilder& b, bool idr_pic_flag,
                                           uint32_t max_num_ref_frames) {
  DecRefPicMarking m;
  m.idr = idr_pic_flag;
  const auto start = br.mark();
  auto scope = b.scoped("dec_ref_pic_marking", "H.264 7.3.3.3");
  if (idr_pic_flag) {
    m.no_output_of_prior_pics_flag = b.read_flag("no_output_of_prior_pics_flag");
    m.long_term_reference_flag = b.read_flag("long_term_reference_flag");
    // §8.2.5.1: an IDR picture sets MaxLongTermFrameIdx to 0 or to "no long-term frame indices".
    m.MaxLongTermFrameIdx = m.long_term_reference_flag ? 0 : kNoLongTermFrameIndices;
  } else {
    m.adaptive_ref_pic_marking_mode_flag = b.read_flag("adaptive_ref_pic_marking_mode_flag");
    if (m.adaptive_ref_pic_marking_mode_flag) {
      read_mmco_ops(br, b, max_num_ref_frames, m);
    }
  }
  if (m.MaxLongTermFrameIdx) {
    b.derived("MaxLongTermFrameIdx", std::string_view{max_long_term_text(*m.MaxLongTermFrameIdx)});
  }
  if (m.has_mmco5) {
    b.derived("has_mmco5", std::string_view{"has_mmco5 = 1 (memory_management_control_operation 5)"});
  }
  m.span = br.span_since(start);
  return m;
}

}  // namespace ovt::syntax::h264
