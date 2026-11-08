// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the container format sniffer (chapter 8 §8.6.1 and §8.9.4, WI-1.200). Input layout: byte 0
// selects the file extension hint (0 none, 1 .yuv, 2 .raw, 3 .rgb, 4 .bin), byte 1 selects how the claimed file
// size relates to the prefix (0 = exactly the prefix, 1 = prefix + 1 MiB, 2 = 2^40, 3 = smaller than the
// prefix, which a real file never gives but direct callers could), the rest is the file prefix
// (the engine's -max_len bounds it; the sniffer itself reads at most 64 KiB). Invariants, each an abort when
// violated: no exception escapes; the result is deterministic; the codec guess is set only for Annex B; a
// result other than Unknown round-trips through format_name / format_from_name; a prefix that decides as raw
// does so only because of a raw extension hint.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

#include "container/format.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) {
    return 0;
  }
  static constexpr std::array<std::string_view, 5> kExt = {"", ".yuv", ".raw", ".rgb", ".bin"};
  static constexpr std::array<uint64_t, 3> kExtra = {0, uint64_t{1} << 20U, uint64_t{1} << 40U};
  const std::string_view ext = kExt.at(data[0] % kExt.size());
  const std::span<const uint8_t> prefix(data + 2, size - 2);
  const uint8_t size_mode = data[1] % 4;
  const uint64_t file_size = size_mode == 3 ? prefix.size() / 2 : prefix.size() + kExtra.at(size_mode);

  try {
    const auto a = ovt::container::sniff_bytes(prefix, file_size, ext);
    const auto b = ovt::container::sniff_bytes(prefix, file_size, ext);
    require(a.format == b.format && a.annexb_codec == b.annexb_codec);
    require(a.format == ovt::container::Format::AnnexB || a.annexb_codec == ovt::container::AnnexBCodec::None);
    if (a.format != ovt::container::Format::Unknown) {
      const auto back = ovt::container::format_from_name(ovt::container::format_name(a.format));
      require(back.has_value() && *back == a.format);
    }
    if (a.format == ovt::container::Format::Raw) {
      require(ext == ".yuv" || ext == ".raw" || ext == ".rgb");
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
