// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 slice data partitions A, B and C (§7.3.2.9): chapter 7 §7.3.4 row 28, WI-1.109. Element order follows
// H.264 (08/2021) §7.3.2.9.1–7.3.2.9.3; slice_data() stays opaque (§7.3.1 rule 2).

#include "ovt/cxx/syntax/h264/data_partition.hpp"

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/h264/slice_header.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/h264/h264_internal.h"

namespace ovt::syntax::h264 {

SliceDataPartitionA parse_slice_data_partition_a(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                                 const ParameterSets& sets) {
  SliceDataPartitionA p;
  const auto start = br.mark();
  auto scope = b.scoped("slice_data_partition_a_layer_rbsp", "H.264 7.3.2.9.1");
  p.slice_header = parse_slice_header(br, b, nal, sets);
  p.slice_id = b.read_ue("slice_id");
  b.opaque_rest("slice_data");
  p.span = br.span_since(start);
  return p;
}

SliceDataPartitionBC parse_slice_data_partition_bc(BitReader& br, SyntaxBuilder& b, const NalHeader& nal,
                                                   const Sps& sps, const Pps& pps) {
  SliceDataPartitionBC p;
  p.partition_c = nal.nal_unit_type == 4;
  const auto start = br.mark();
  auto scope = b.scoped(p.partition_c ? "slice_data_partition_c_layer_rbsp" : "slice_data_partition_b_layer_rbsp",
                        p.partition_c ? "H.264 7.3.2.9.3" : "H.264 7.3.2.9.2");
  p.slice_id = b.read_ue("slice_id");
  if (sps.separate_colour_plane_flag) {
    p.colour_plane_id = static_cast<uint32_t>(b.read_u("colour_plane_id", 2));
    detail::check_range(b, "colour_plane_id", p.colour_plane_id, 0, 2, "H.264 7.4.3");
  }
  if (pps.redundant_pic_cnt_present_flag) {
    p.redundant_pic_cnt = b.read_ue("redundant_pic_cnt");
    detail::require_range(br, "redundant_pic_cnt", p.redundant_pic_cnt, 0, 127);
  }
  b.opaque_rest("slice_data");
  p.span = br.span_since(start);
  return p;
}

}  // namespace ovt::syntax::h264
