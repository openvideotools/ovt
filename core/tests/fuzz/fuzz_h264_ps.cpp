// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target for the H.264 parameter-set parsers (WI-1.108a, chapter 14 §14.7): the input is a NAL unit (with
// emulation prevention bytes) whose first byte selects the mode. Every ParseError is a legal outcome; anything
// else (another exception, a crash, a sanitizer finding) is a bug.
//
//   mode 0  parse_nal_header + parse_sps
//   mode 1  parse_nal_header with recording off, then parse_sps
//   (WI-1.108b adds the PPS modes.)

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/rbsp.hpp"
#include "ovt/cxx/syntax/h264/nal_header.hpp"
#include "ovt/cxx/syntax/h264/sps.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "syntax/common/syntax_node_buffer.h"

namespace {

using ovt::BitReader;
using ovt::syntax::SyntaxBuilder;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  const unsigned mode = data[0] % 2U;
  const ovt::Rbsp rbsp = ovt::rbsp_from_nal(std::span<const uint8_t>(data + 1, size - 1));
  BitReader br(rbsp.rbsp);
  SyntaxBuilder b(br);
  if (mode == 1) {
    b.set_recording(false);
  }
  try {
    (void)ovt::syntax::h264::parse_nal_header(br, b);
    (void)ovt::syntax::h264::parse_sps(br, b);
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
