// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Types shared by the elementary-stream splitters of the container module: Annex B (chapter 08 §8.6.3, WI-1.226)
// and raw AV1 OBU streams (§8.6.4, WI-1.227).

#ifndef OVT_CONTAINER_SPLIT_TYPES_HPP
#define OVT_CONTAINER_SPLIT_TYPES_HPP

#include <cstdint>
#include <functional>
#include <string>

#include "container/diagnostics.h"

namespace ovt::container {

// A file byte range; same layout as ovt.fb.ByteSpan.
struct FileSpan {
  uint64_t offset = 0;
  uint64_t length = 0;
  [[nodiscard]] uint64_t end() const noexcept { return offset + length; }
  friend bool operator==(const FileSpan&, const FileSpan&) = default;
};

// A CNT-* finding of a splitter. `message` is the registry template with the parameters filled in.
struct Finding {
  DiagCode code;
  uint64_t file_offset = 0;
  std::string message;
};

using FindingSink = std::function<void(const Finding&)>;  // may be empty: findings are dropped

}  // namespace ovt::container

#endif  // OVT_CONTAINER_SPLIT_TYPES_HPP
