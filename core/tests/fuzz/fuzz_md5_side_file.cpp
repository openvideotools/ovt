// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the picture-hash code (chapter 7 §7.6.4, WI-4.109): parse_md5_side_file on the whole input, and
// the D.3.19 plane hashes on a plane shaped by the first three bytes (width 1..32, height 1..32, bit depth
// 8..16) with the remaining bytes as sample data. Invariants, each an abort when violated: the parser yields at
// most one digest per 32 input bytes; a second run gives identical results (determinism, §0.7.5); the hashes
// never read outside the plane.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "hash_check.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace ovt::analysis;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto a = parse_md5_side_file(text);
  if (a.size() > (size / 32U) || a != parse_md5_side_file(text)) {
    std::abort();
  }
  if (size < 3U) {
    return 0;
  }
  ovt::model::PlaneView p;
  p.width = 1U + (data[0] % 32U);
  p.height = 1U + (data[1] % 32U);
  p.bit_depth = static_cast<uint8_t>(8U + (data[2] % 9U));
  p.sample_bytes = p.bit_depth > 8U ? 2U : 1U;
  p.format = p.sample_bytes == 2U ? ovt::model::SampleFormat::U16LE : ovt::model::SampleFormat::U8;
  p.stride_bytes = p.width * p.sample_bytes;
  std::vector<uint8_t> samples(static_cast<std::size_t>(p.stride_bytes) * p.height, 0);
  for (std::size_t i = 0; i < samples.size() && 3U + i < size; ++i) {
    samples[i] = data[3U + i];
  }
  p.data = samples.data();
  if (plane_md5(p) != plane_md5(p) || plane_crc(p) != plane_crc(p) || plane_checksum(p) != plane_checksum(p)) {
    std::abort();
  }
  return 0;
}
