// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Random access map of a StreamIndex (chapter 7 §7.4.2 S11, WI-1.121). Private to the index module; the C API
// exports it through ovt_stream_rap_before(). Module-internal, not installed.

#ifndef OVT_INDEX_RAP_MAP_H
#define OVT_INDEX_RAP_MAP_H

#include <cstdint>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::index {

class RapMap {
 public:
  RapMap() = default;
  /// `index` must be a verified StreamIndex.
  explicit RapMap(const fb::StreamIndex& index);

  /// max{ r <= decode_index : frames[r].is_rap }; -1 when there is none or decode_index is out of range.
  [[nodiscard]] int32_t rap_before(int64_t decode_index) const noexcept;
  /// H.265: frame `decode_index` is a RASL picture and rap_before() of it is a CRA, so a decoder that starts at
  /// that CRA cannot decode it ("skip when starting at r"). False for the other codecs.
  [[nodiscard]] bool skip_when_starting_at_rap(int64_t decode_index) const noexcept;
  [[nodiscard]] size_t size() const noexcept { return rap_before_.size(); }

 private:
  std::vector<int32_t> rap_before_;
  std::vector<bool> skip_;
};

}  // namespace ovt::index

#endif  // OVT_INDEX_RAP_MAP_H
