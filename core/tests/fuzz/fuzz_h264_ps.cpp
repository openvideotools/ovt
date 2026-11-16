// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target for the H.264 parameter-set parsers (WI-1.108, chapter 14 §14.7): the input is a NAL unit (with
// emulation prevention bytes) whose first byte selects the mode. Every ParseError is a legal outcome; anything
// else (another exception, a crash, a sanitizer finding) is a bug.
//
//   mode 0  parse_nal_header + parse_sps
//   mode 1  parse_nal_header + parse_pps without an SPS (provisional)
//   mode 2  parse_nal_header + parse_pps with a canned 4:4:4 SPS in the table (12 scaling lists)
//   mode 3  parse_nal_header only, recording off, then parse_sps

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/pps.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;

// The 4:4:4 SPS of core/tests/syntax/h264/test_syntax_h264_sps_pps.cpp (k444Sps), already EPB-free.
const std::vector<uint8_t> kSps444 = {0x67, 0xf4, 0x00, 0x15, 0x91, 0x9b, 0x28, 0x2c, 0x12, 0xd8, 0x0b, 0x50, 0x10,
                                      0x10, 0x14, 0x00, 0x00, 0x0f, 0xa4, 0x00, 0x03, 0xa9, 0x81, 0x81, 0x80, 0x00,
                                      0x4c, 0x4b, 0x40, 0x00, 0x26, 0x25, 0xa9, 0xa8, 0xc0, 0x1e, 0x2c, 0x5b, 0x2c};

const ovt::syntax::h264::Sps& canned_sps() {
  static const ovt::syntax::h264::Sps sps = [] {
    BitReader br(kSps444);
    SyntaxBuilder b(br);
    b.set_recording(false);
    (void)ovt::syntax::h264::parse_nal_header(br, b);
    auto s = ovt::syntax::h264::parse_sps(br, b);
    (void)b.finish();
    return s;
  }();
  return sps;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const unsigned mode = data[0] % 4U;
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(std::span<const uint8_t>(data + 1, size - 1));
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  if (mode == 3) {
    b.set_recording(false);
  }
  ovt::syntax::h264::SpsTable table;
  if (mode == 2) {
    table.entries[canned_sps().seq_parameter_set_id] = &canned_sps();
  }
  try {
    (void)ovt::syntax::h264::parse_nal_header(br, b);
    if (mode == 1 || mode == 2) {
      (void)ovt::syntax::h264::parse_pps(br, b, table);
    } else {
      (void)ovt::syntax::h264::parse_sps(br, b);
    }
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
