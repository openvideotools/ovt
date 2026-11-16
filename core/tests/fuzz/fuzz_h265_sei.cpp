// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target for the H.265 SEI parsers (WI-1.114, chapter 14 §14.7): the input is a SEI NAL unit (header included,
// with emulation prevention bytes) behind one selector byte. Every ParseError is a legal outcome; anything else
// (another exception, a crash, a sanitizer finding) is a bug.
//
//   selector bit 0     recording off (the values must still be produced without recording)
//   selector bits 1-2  SPS 0 known to the parsers: 0 none, 1 NAL + VCL HRD with sub-picture parameters (DU fields
//                      in pic_timing), 2 NAL + VCL HRD with 2 CPBs, frame_field_info and long fields, 3 monochrome
//                      without VUI
//   selector bit 3     a buffering_period of this access unit named SPS 0
//   selector bit 4     VPS 0 known, with three layers (layer_sps_idx loop of active_parameter_sets)

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h265/hrd_parameters.hpp"
#include "ovt/cxx/syntax/h265/nal_header.hpp"
#include "ovt/cxx/syntax/h265/sei.hpp"
#include "ovt/cxx/syntax/h265/sps.hpp"
#include "ovt/cxx/syntax/h265/vps.hpp"
#include "ovt/cxx/syntax/h265/vui.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::h265::HrdParameters;
using ovt::syntax::h265::Sps;
using ovt::syntax::h265::Vps;
using ovt::syntax::h265::VuiParameters;

Sps make_sps(unsigned variant) {
  Sps sps;
  sps.PicSizeInCtbsY = 40;
  sps.MaxPicOrderCntLsb = 256;
  if (variant == 3) {
    sps.chroma_format_idc = 0;
    return sps;
  }
  HrdParameters hrd;
  hrd.nal_hrd_parameters_present_flag = true;
  hrd.vcl_hrd_parameters_present_flag = true;
  VuiParameters vui;
  if (variant == 1) {
    hrd.sub_pic_hrd_params_present_flag = true;
    hrd.sub_pic_cpb_params_in_pic_timing_sei_flag = true;
    hrd.initial_cpb_removal_delay_length_minus1 = 15;
    hrd.au_cpb_removal_delay_length_minus1 = 5;
    hrd.dpb_output_delay_length_minus1 = 5;
    hrd.dpb_output_delay_du_length_minus1 = 12;
    hrd.du_cpb_removal_delay_increment_length_minus1 = 7;
  } else {
    hrd.sub_layer[0].cpb_cnt_minus1 = 1;
    hrd.initial_cpb_removal_delay_length_minus1 = 31;
    vui.frame_field_info_present_flag = true;
  }
  vui.vui_hrd_parameters_present_flag = true;
  vui.hrd_parameters = hrd;
  sps.vui_parameters_present_flag = true;
  sps.vui = vui;
  return sps;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 3) {
    return 0;
  }
  const unsigned selector = data[0];
  const unsigned variant = (selector >> 1U) & 3U;
  const Sps sps = make_sps(variant);
  Vps vps;
  vps.vps_base_layer_internal_flag = true;
  vps.vps_max_layers_minus1 = 2;
  std::array<const Sps*, 16> sps_table{};
  std::array<const Vps*, 16> vps_table{};
  if (variant != 0) {
    sps_table[0] = &sps;
  }
  if ((selector & 16U) != 0) {
    vps_table[0] = &vps;
  }
  ovt::syntax::h265::SeiContext ctx;
  ctx.sps_by_id = sps_table;
  ctx.vps_by_id = vps_table;
  ctx.active_sps = sps_table[0];
  if ((selector & 8U) != 0) {
    ctx.buffering_period_sps_id = 0;
  }
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(std::span<const uint8_t>(data + 1, size - 1));
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  b.set_recording((selector & 1U) == 0);
  try {
    const auto nal = ovt::syntax::h265::parse_nal_header(br, b);
    (void)ovt::syntax::h265::parse_sei_rbsp(br, b, nal, ctx);
  } catch (const ovt::ParseError&) {
    // expected on malformed input
  }
  const auto tree = b.finish();
  if (!tree.empty()) {
    (void)tree.to_flatbuffer();
    (void)ovt::syntax::to_trace(tree, rbsp.rbsp);
  }
  return 0;
}
