// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// FlatBuffers verifier limits of chapter 06 WI-0.094, shared by the model validator (WI-0.086) and the
// canonical builder (WI-0.085). Internal header. The cache module keeps the same values in cache/fb_verify.hpp.

#ifndef OVT_CORE_MODEL_VERIFIER_OPTIONS_HPP
#define OVT_CORE_MODEL_VERIFIER_OPTIONS_HPP

#include <flatbuffers/flatbuffers.h>

namespace ovt::model::detail {

// A 4K HEVC block tree exceeds the FlatBuffers default max_tables (WI-0.094 notes).
inline flatbuffers::Verifier::Options verifier_options() {
  flatbuffers::Verifier::Options opts;
  opts.max_depth = 64;
  opts.max_tables = 10'000'000;
  return opts;
}

}  // namespace ovt::model::detail

#endif  // OVT_CORE_MODEL_VERIFIER_OPTIONS_HPP
