// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the on-disk cache record reader (chapter 06 §6.7.1, chapter 14 §14.7.1 `fuzz_cache_blob`;
// WI-0.091 step 3): the zstd frame of a frames/*.fa.zst file followed by the size-prefixed FrameAnalysis
// verification the Entry applies. The input is the compressed file. A CacheError(OVT_E_CORRUPT) is the expected
// answer to malformed bytes, a failed verification is a normal outcome; any other exception, a crash, an
// allocation beyond the stated limit or a sanitizer report is a finding. The decompressed record also goes
// through the I5 and I6 checks of invalidating reads (WI-0.092), which must agree with the verifier.

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "cache/invalidate.hpp"
#include "cache/zstd_io.hpp"
#include "ovt/cxx/cache/error.hpp"
#include "ovt/status.h"
#include "ovt_model_generated.h"

namespace {

constexpr std::size_t kMaxRecord = 16U << 20U;  // the engine's -rss_limit_mb bounds the process, this the record

void require(bool ok) {
  if (!ok) {
    std::abort();
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::vector<std::uint8_t> record;
  try {
    record = ovt::cache::decompress(std::span<const std::uint8_t>(data, size), kMaxRecord);
  } catch (const ovt::cache::CacheError& e) {
    require(e.code == OVT_E_CORRUPT);
    return 0;
  }
  require(record.size() <= kMaxRecord);
  flatbuffers::Verifier::Options opts;
  opts.max_depth = 64;
  opts.max_tables = 10'000'000;
  flatbuffers::Verifier verifier(record.data(), record.size(), opts);
  const bool verified = verifier.VerifySizePrefixedBuffer<ovt::fb::FrameAnalysis>("OVTA");
  const std::uint32_t decode_index = size > 0 ? data[0] : 0U;
  const auto i5 = ovt::cache::check_frame(record, decode_index, ovt::cache::SelectedPlugin{"p", "1"});
  require(verified || i5 == ovt::cache::InvalidationReason::Frame);
  if (verified) {
    const auto* fa = flatbuffers::GetSizePrefixedRoot<ovt::fb::FrameAnalysis>(record.data());
    (void)ovt::cache::check_plane(*fa, ovt::fb::PlaneStage::Reconstruction, 0, record.size());
    // The fields the Entry reads after verification.
    if (fa->frame() != nullptr && fa->frame()->id() != nullptr) {
      (void)fa->frame()->id()->decode_index();
    }
    if (fa->plugin_id() != nullptr) {
      (void)fa->plugin_id()->string_view();
    }
    if (fa->planes() != nullptr) {
      for (const auto* ref : *fa->planes()) {
        (void)ref->width();
      }
    }
  }
  return 0;
}
