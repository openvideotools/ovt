// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the Annex B splitter (chapter 08 §8.6.3 and §8.9.4, WI-1.226). Byte 0 selects the codec hint,
// the rest is the byte stream. Invariants, each an abort when violated: no exception escapes; the units are in
// file order, non-empty, inside the input and never overlap; every prefix_span is `00 00 01` or `00 00 00 01` and
// ends where its unit starts; a unit never contains 00 00 00 or 00 00 01 and never ends in a zero byte; splitting
// the same bytes at a shifted base offset gives the same units shifted.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "container/annexb.hpp"

namespace {

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  using ovt::container::AnnexBCodec;
  static constexpr AnnexBCodec kCodecs[] = {AnnexBCodec::None, AnnexBCodec::H264, AnnexBCodec::H265,
                                            AnnexBCodec::Ambiguous};
  const AnnexBCodec codec = kCodecs[data[0] % 4];
  const std::span<const uint8_t> bytes(data + 1, size - 1);
  try {
    std::vector<ovt::container::NalSpan> a;
    ovt::container::annexb_split(bytes, 0, [&](const ovt::container::NalSpan& n) { a.push_back(n); }, {}, codec);
    uint64_t floor = 0;
    for (const auto& n : a) {
      require(n.file_span.length > 0 && n.file_span.end() <= bytes.size());
      require(n.prefix_span.offset >= floor && n.prefix_span.end() == n.file_span.offset);
      require(n.start_code_bytes == n.prefix_span.length && (n.start_code_bytes == 3 || n.start_code_bytes == 4));
      const auto p = bytes.subspan(n.prefix_span.offset, n.prefix_span.length);
      require(p[p.size() - 1] == 1 && p[p.size() - 2] == 0 && p[p.size() - 3] == 0);
      require(n.start_code_bytes == 3 || p[0] == 0);
      const auto nal = bytes.subspan(n.file_span.offset, n.file_span.length);
      require(nal[0] == n.header_byte && nal[nal.size() - 1] != 0);
      for (size_t i = 2; i < nal.size(); ++i) {
        require(nal[i - 2] != 0 || nal[i - 1] != 0 || nal[i] > 1);
      }
      floor = n.file_span.end();
    }
    std::vector<ovt::container::NalSpan> b;
    ovt::container::annexb_split(bytes, 777, [&](const ovt::container::NalSpan& n) { b.push_back(n); }, {}, codec);
    require(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      require(b[i].file_span.offset == a[i].file_span.offset + 777 && b[i].file_span.length == a[i].file_span.length);
    }
  } catch (...) {
    std::abort();
  }
  return 0;
}
