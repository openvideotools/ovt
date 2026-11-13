// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// FlatBuffers verification with the limits of chapter 06 WI-0.094, shared by the cache records (WI-0.091) and
// the invalidation rules (WI-0.092). Internal header.

#ifndef OVT_CORE_CACHE_FB_VERIFY_HPP
#define OVT_CORE_CACHE_FB_VERIFY_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <span>

namespace ovt::cache {

// A 4K HEVC block tree exceeds the FlatBuffers default max_tables (WI-0.094 notes).
inline flatbuffers::Verifier::Options verifier_options() {
  flatbuffers::Verifier::Options opts;
  opts.max_depth = 64;
  opts.max_tables = 10'000'000;
  return opts;
}

/// The root of a plain (not size-prefixed) buffer with @p identifier, or nullptr when it does not verify.
template <class T>
const T* verify_root(std::span<const std::uint8_t> buf, const char* identifier) {
  const auto opts = verifier_options();
  flatbuffers::Verifier verifier(buf.data(), buf.size(), opts);
  if (!verifier.VerifyBuffer<T>(identifier)) {
    return nullptr;
  }
  return flatbuffers::GetRoot<T>(buf.data());
}

/// The root of a size-prefixed buffer with @p identifier, or nullptr when it does not verify.
template <class T>
const T* verify_size_prefixed(std::span<const std::uint8_t> buf, const char* identifier) {
  const auto opts = verifier_options();
  flatbuffers::Verifier verifier(buf.data(), buf.size(), opts);
  if (!verifier.VerifySizePrefixedBuffer<T>(identifier)) {
    return nullptr;
  }
  return flatbuffers::GetSizePrefixedRoot<T>(buf.data());
}

}  // namespace ovt::cache

#endif  // OVT_CORE_CACHE_FB_VERIFY_HPP
