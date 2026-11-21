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
#include <map>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ovt::fb {
struct BoxNodeT;
}

namespace ovt::container::isobmff {

using FourCC = std::array<char, 4>;

class PagedSpan;

// Per-track state of the fragment decoders (WI-1.211): the `trex` defaults of `moov/mvex` and the `mdhd` timescale of
// the track, both keyed by track_ID. SPEC-ASSUMPTION(#670): one map shared by every copy of the context, because a
// `trex` is decoded in the child context of `mvex` and its values must reach the `moof`s that follow.
struct TrackFragmentInfo {
  bool has_trex = false;
  uint32_t default_sample_description_index = 0;
  uint32_t default_sample_duration = 0;
  uint32_t default_sample_size = 0;
  uint32_t default_sample_flags = 0;
  uint32_t timescale = 0;  // mdhd.timescale of the track, 0 = unknown
};

// What the `stbl` children tell each other (chapter 08 §8.3.4, WI-1.209): the counts the cross-box checks of
// finish_sample_tables() need. A fresh state starts at every `trak` because the children of a box see a copy of its
// context.
struct SampleTableState {
  uint64_t stsd_entries = 0;
  uint64_t stts_samples = 0;  // sum of the stts sample_count
  uint64_t ctts_samples = 0;
  uint64_t sample_count = 0;  // stsz / stz2
  uint64_t stss_entries = 0;
  uint64_t stss_max = 0;  // largest sample_number listed
  uint64_t stps_max = 0;
  uint64_t sdtp_bytes = 0;
  bool has_stsd = false;
  bool has_stts = false;
  bool has_ctts = false;
  bool has_sample_count = false;  // stsz / stz2 read
  bool has_stco = false;
  bool has_co64 = false;
  bool has_stss = false;
  bool has_stps = false;
  bool has_sdtp = false;
};

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
  uint8_t per_sample_iv_size = 0xFF;   // 0xFF = unknown
  uint64_t moof_offset = 0;            // enclosing moof start (trun data_offset base)
  uint64_t file_size = 0;              // total size of the file being walked (0 = unknown)
  SampleTableState stbl;               // filled by the stsd / stts / ... decoders for their later siblings
  PagedSpan* payload_pages = nullptr;  // the whole payload of a box a windowed decoder reads; null outside the walker
  // null until the walk creates it (a test context without one simply keeps no track state)
  std::shared_ptr<std::map<uint32_t, TrackFragmentInfo>> trex_for_track;
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
