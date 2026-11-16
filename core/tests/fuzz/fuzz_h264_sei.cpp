// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target for the H.264 SEI parsers (WI-1.110, chapter 14 §14.7): the input is a SEI NAL unit (with emulation
// prevention bytes) behind one selector byte. Every ParseError is a legal outcome; anything else (another
// exception, a crash, a sanitizer finding) is a bug.
//
//   selector bit 0     recording off (the values must still be produced without recording)
//   selector bits 1-2  parameter sets known to the parsers: 0 none, 1 NAL HRD, 2 NAL + VCL HRD with 2 CPBs and
//                      a time_offset_length of 7, 3 VCL HRD only (short delay fields)
//   selector bit 3     a buffering_period of an earlier access unit named SPS 0

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/sei.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;
using ovt::syntax::h264::Sps;

Sps make_sps(unsigned variant) {
  Sps sps;
  sps.vui_parameters_present_flag = variant != 0;
  if (variant == 0) {
    return sps;
  }
  sps.vui.pic_struct_present_flag = true;
  if (variant == 1 || variant == 2) {
    sps.vui.nal_hrd_parameters_present_flag = true;
    sps.vui.nal_hrd.initial_cpb_removal_delay_length_minus1 = 19;
    sps.vui.nal_hrd.cpb_removal_delay_length_minus1 = 6;
    sps.vui.nal_hrd.dpb_output_delay_length_minus1 = 6;
    sps.vui.nal_hrd.time_offset_length = variant == 2 ? 7 : 0;
  }
  if (variant == 2) {
    sps.vui.nal_hrd.cpb_cnt_minus1 = 1;
    sps.vui.vcl_hrd_parameters_present_flag = true;
    sps.vui.vcl_hrd.initial_cpb_removal_delay_length_minus1 = 31;
  }
  if (variant == 3) {
    sps.vui.vcl_hrd_parameters_present_flag = true;
    sps.vui.vcl_hrd.initial_cpb_removal_delay_length_minus1 = 3;
    sps.vui.vcl_hrd.cpb_removal_delay_length_minus1 = 0;
    sps.vui.vcl_hrd.dpb_output_delay_length_minus1 = 0;
    sps.vui.vcl_hrd.time_offset_length = 24;
  }
  return sps;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const unsigned selector = data[0];
  const Sps sps = make_sps((selector >> 1U) & 3U);
  std::array<const Sps*, 32> table{};
  if ((selector >> 1U) % 4U != 0) {
    table[0] = &sps;
  }
  ovt::syntax::h264::SeiContext ctx;
  ctx.sps_by_id = table;
  ctx.active_sps = table[0];
  if ((selector & 8U) != 0) {
    ctx.buffering_period_sps_id = 0;
  }
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(std::span<const uint8_t>(data + 1, size - 1));
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  b.set_recording((selector & 1U) == 0);
  try {
    (void)ovt::syntax::h264::parse_nal_header(br, b);
    (void)ovt::syntax::h264::parse_sei_rbsp(br, b, ctx);
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
