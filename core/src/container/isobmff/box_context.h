// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// BoxContext: the ancestor chain and per-track state a box sees while it is walked (chapter 08 §8.2.7, WI-1.201).
// The registry lookup (WI-1.202/1.203) keys on it. Only the members the header walk maintains are filled in
// here; the decoders of group A (WI-1.205) set the timescale / track_id / handler members.

#ifndef OVT_CONTAINER_ISOBMFF_BOX_CONTEXT_H
#define OVT_CONTAINER_ISOBMFF_BOX_CONTEXT_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace ovt::fb {
struct BoxNodeT;
}

namespace ovt::container::isobmff {

using FourCC = std::array<char, 4>;

struct BoxContext {
  std::vector<FourCC> ancestors;  // ["moov","trak","mdia"] innermost last
  FourCC handler_type{};          // hdlr.handler_type of the enclosing trak, or all zero
  FourCC sample_entry{};          // innermost sample entry fourcc, or all zero
  uint32_t movie_timescale = 0;
  uint32_t track_timescale = 0;
  uint32_t track_id = 0;
  bool quicktime = false;  // no ftyp, or major_brand 'qt  '
  bool fragment_only = false;
  bool saw_moov = false;
  bool mdat_before_moov = false;
  uint8_t stsd_version = 0;
  uint8_t per_sample_iv_size = 0xFF;  // 0xFF = unknown
  uint64_t moof_offset = 0;           // enclosing moof start (trun data_offset base)
  const fb::BoxNodeT* trex_for_track = nullptr;
  const fb::BoxNodeT* parent = nullptr;

  [[nodiscard]] bool in(std::string_view fourcc) const noexcept {
    return std::ranges::any_of(ancestors, [&](const FourCC& a) { return std::string_view(a.data(), 4) == fourcc; });
  }
  [[nodiscard]] bool parent_is(std::string_view fourcc) const noexcept {
    return !ancestors.empty() && std::string_view(ancestors.back().data(), 4) == fourcc;
  }
  // The context of the children of a box of type `type` that lives at `node`.
  [[nodiscard]] BoxContext push(const FourCC& type, const fb::BoxNodeT* node) const {
    BoxContext c = *this;
    c.ancestors.push_back(type);
    c.parent = node;
    return c;
  }
};

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_BOX_CONTEXT_H
