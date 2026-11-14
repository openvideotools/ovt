// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// H.264 nal_unit() header with the SVC / MVC / 3D-AVC extension headers (chapter 7 §7.3.4 row 1, WI-1.108).
// The syntax follows H.264 (08/2021) §7.3.1, G.7.3.1.1, H.7.3.1.1 and I.7.3.1.1 line by line.

#include "ovt/cxx/syntax/h264/nal_header.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string_view>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"

namespace ovt::syntax::h264 {

namespace {

constexpr std::string_view kSpec = "H.264 7.3.1";

void parse_svc_extension(SyntaxBuilder& b, NalHeaderSvcExtension& e) {
  auto s = b.scoped("nal_unit_header_svc_extension", "H.264 G.7.3.1.1");
  e.idr_flag = b.read_flag("idr_flag");
  e.priority_id = static_cast<uint32_t>(b.read_u("priority_id", 6));
  e.no_inter_layer_pred_flag = b.read_flag("no_inter_layer_pred_flag");
  e.dependency_id = static_cast<uint32_t>(b.read_u("dependency_id", 3));
  e.quality_id = static_cast<uint32_t>(b.read_u("quality_id", 4));
  e.temporal_id = static_cast<uint32_t>(b.read_u("temporal_id", 3));
  e.use_ref_base_pic_flag = b.read_flag("use_ref_base_pic_flag");
  e.discardable_flag = b.read_flag("discardable_flag");
  e.output_flag = b.read_flag("output_flag");
  e.reserved_three_2bits = static_cast<uint32_t>(b.read_f("reserved_three_2bits", 2, 3));
}

void parse_mvc_extension(SyntaxBuilder& b, NalHeaderMvcExtension& e) {
  auto s = b.scoped("nal_unit_header_mvc_extension", "H.264 H.7.3.1.1");
  e.non_idr_flag = b.read_flag("non_idr_flag");
  e.priority_id = static_cast<uint32_t>(b.read_u("priority_id", 6));
  e.view_id = static_cast<uint32_t>(b.read_u("view_id", 10));
  e.temporal_id = static_cast<uint32_t>(b.read_u("temporal_id", 3));
  e.anchor_pic_flag = b.read_flag("anchor_pic_flag");
  e.inter_view_flag = b.read_flag("inter_view_flag");
  e.reserved_one_bit = static_cast<uint32_t>(b.read_f("reserved_one_bit", 1, 1));
}

void parse_3davc_extension(SyntaxBuilder& b, NalHeader3dAvcExtension& e) {
  auto s = b.scoped("nal_unit_header_3davc_extension", "H.264 I.7.3.1.1");
  e.view_idx = static_cast<uint32_t>(b.read_u("view_idx", 8));
  e.depth_flag = b.read_flag("depth_flag");
  e.non_idr_flag = b.read_flag("non_idr_flag");
  e.temporal_id = static_cast<uint32_t>(b.read_u("temporal_id", 3));
  e.anchor_pic_flag = b.read_flag("anchor_pic_flag");
  e.inter_view_flag = b.read_flag("inter_view_flag");
}

}  // namespace

std::string_view nal_unit_type_name(uint32_t nal_unit_type) noexcept {
  switch (nal_unit_type) {  // Table 7-1
    case 0:
      return "unspecified";
    case 1:
      return "slice_layer_without_partitioning_rbsp (non-IDR)";
    case 2:
      return "slice_data_partition_a_layer_rbsp";
    case 3:
      return "slice_data_partition_b_layer_rbsp";
    case 4:
      return "slice_data_partition_c_layer_rbsp";
    case 5:
      return "slice_layer_without_partitioning_rbsp (IDR)";
    case 6:
      return "sei_rbsp";
    case 7:
      return "seq_parameter_set_rbsp";
    case 8:
      return "pic_parameter_set_rbsp";
    case 9:
      return "access_unit_delimiter_rbsp";
    case 10:
      return "end_of_seq_rbsp";
    case 11:
      return "end_of_stream_rbsp";
    case 12:
      return "filler_data_rbsp";
    case 13:
      return "seq_parameter_set_extension_rbsp";
    case 14:
      return "prefix_nal_unit_rbsp";
    case 15:
      return "subset_seq_parameter_set_rbsp";
    case 16:
      return "depth_parameter_set_rbsp";
    case 19:
      return "slice_layer_auxiliary_rbsp";
    case 20:
      return "slice_layer_extension_rbsp";
    case 21:
      return "slice_layer_extension_rbsp (3D-AVC)";
    case 17:
    case 18:
    case 22:
    case 23:
      return "reserved";
    default:
      return "unspecified";
  }
}

NalHeader parse_nal_header(BitReader& br, SyntaxBuilder& b) {
  NalHeader h;
  const auto start = br.mark();
  {
    auto s = b.scoped("nal_unit_header", kSpec);
    h.forbidden_zero_bit = static_cast<uint32_t>(b.read_u("forbidden_zero_bit", 1));
    h.nal_ref_idc = static_cast<uint32_t>(b.read_u("nal_ref_idc", 2));
    h.nal_unit_type = static_cast<uint32_t>(b.read_u("nal_unit_type", 5));
    h.type_name = nal_unit_type_name(h.nal_unit_type);
    b.set_enum(h.type_name);
    h.reserved_type = h.type_name == "reserved";
    if (h.forbidden_zero_bit != 0 || h.reserved_type) {
      b.diag(Severity::Error, "SYN-H264-0001",
             fmt::format("malformed nal_unit_header (forbidden_zero_bit = 1 or reserved nal_unit_type {})",
                         h.nal_unit_type),
             "H.264 7.4.1");
    }
    if (h.nal_unit_type == 14 || h.nal_unit_type == 20 || h.nal_unit_type == 21) {
      if (h.nal_unit_type != 21) {
        h.svc_extension_flag = b.read_flag("svc_extension_flag");
      } else {
        h.avc_3d_extension_flag = b.read_flag("avc_3d_extension_flag");
      }
      if (h.svc_extension_flag) {
        h.has_svc_extension = true;
        parse_svc_extension(b, h.svc);
        h.temporal_id = h.svc.temporal_id;
        h.layer_id = h.svc.dependency_id;
      } else if (h.avc_3d_extension_flag) {
        h.has_3davc_extension = true;
        parse_3davc_extension(b, h.avc_3d);
        h.temporal_id = h.avc_3d.temporal_id;
        h.layer_id = h.avc_3d.view_idx;  // SPEC-ASSUMPTION(#320): view index until the subset SPS is active
      } else {
        h.has_mvc_extension = true;
        parse_mvc_extension(b, h.mvc);
        h.temporal_id = h.mvc.temporal_id;
        h.layer_id = h.mvc.view_id;  // SPEC-ASSUMPTION(#320): view_id, mapped to VOIdx by the index module
      }
    }
    h.IdrPicFlag = h.nal_unit_type == 5;
    b.derived("IdrPicFlag", h.IdrPicFlag ? 1 : 0);
    if (h.has_svc_extension || h.has_mvc_extension || h.has_3davc_extension) {
      b.derived("temporal_id", h.temporal_id);
      b.derived("layer_id", h.layer_id);
    }
  }
  h.span = br.span_since(start);
  return h;
}

}  // namespace ovt::syntax::h264
