// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Random access map of a StreamIndex (chapter 7 §7.4.2 S11, WI-1.121, WI-1.123).

#include "index/rap_map.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ovt::index {

namespace {

constexpr uint8_t kRaslN = 8;  // H.265 Table 7-1
constexpr uint8_t kRaslR = 9;
constexpr uint8_t kBlaWLp = 16;  // the only BLA type that may have RASL pictures
constexpr uint8_t kCraNut = 21;

// True when one of the frame's units has nal_unit_type `a` or `b`.
bool has_unit_type(const fb::StreamIndex& index, const fb::FrameInfo& f, uint8_t a, uint8_t b) {
  if (f.units() == nullptr || index.units() == nullptr) {
    return false;
  }
  const auto& units = *index.units();
  return std::ranges::any_of(*f.units(), [&units, a, b](uint32_t u) {
    return u < units.size() && (units.Get(u)->type() == a || units.Get(u)->type() == b);
  });
}

}  // namespace

RapMap::RapMap(const fb::StreamIndex& index) {
  if (index.frames() == nullptr) {
    return;
  }
  const bool h265 = index.codec() == fb::Codec::H265;
  rap_before_.reserve(index.frames()->size());
  skip_.reserve(index.frames()->size());
  int32_t last = -1;
  bool last_has_rasl = false;  // the last RAP is a CRA or BLA_W_LP (WI-1.123)
  for (uint32_t d = 0; d < index.frames()->size(); ++d) {
    const fb::FrameInfo& f = *index.frames()->Get(d);
    if (f.is_rap()) {
      last = static_cast<int32_t>(d);
      last_has_rasl = h265 && has_unit_type(index, f, kBlaWLp, kCraNut);
    }
    rap_before_.push_back(last);
    skip_.push_back(last_has_rasl && !f.is_rap() && has_unit_type(index, f, kRaslN, kRaslR));
  }
}

int32_t RapMap::rap_before(int64_t decode_index) const noexcept {
  return decode_index < 0 || static_cast<uint64_t>(decode_index) >= rap_before_.size()
             ? -1
             : rap_before_[static_cast<size_t>(decode_index)];
}

bool RapMap::skip_when_starting_at_rap(int64_t decode_index) const noexcept {
  return decode_index >= 0 && static_cast<uint64_t>(decode_index) < skip_.size() &&
         skip_[static_cast<size_t>(decode_index)];
}

}  // namespace ovt::index
