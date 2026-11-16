// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Display order of the StreamIndex (chapter 7 §7.4.2 S9, chapter 6 §6.2.5; WI-1.121).

#include <algorithm>
#include <cstddef>
#include <vector>

#include "index/builder_internal.h"

namespace ovt::index {

namespace {

// AV1: every frame with shown == true, in decode order. A hidden frame keeps display_index -1 even when a later
// show_existing_frame shows it; the show_existing entry gets the slot (§7.4.3).
std::vector<int32_t> av1_order(std::vector<FrameRec>& frames) {
  std::vector<int32_t> order;
  for (FrameRec& f : frames) {
    f.display_index = -1;
    if (f.shown) {
      f.display_index = static_cast<int32_t>(order.size());
      order.push_back(f.decode_index);
    }
  }
  return order;
}

// H.264 / H.265: within each coded video sequence, output pictures sorted by POC (stable); the sequences follow
// each other in decode order. The second field of a complementary pair shares the slot of its first field.
std::vector<int32_t> poc_order(std::vector<FrameRec>& frames) {
  std::vector<int32_t> order;
  size_t begin = 0;
  while (begin < frames.size()) {
    size_t end = begin;
    std::vector<size_t> slots;  // frames that own a display slot
    while (end < frames.size() && frames[end].sequence == frames[begin].sequence) {
      frames[end].display_index = -1;
      if (frames[end].shown && frames[end].second_field_of < 0) {
        slots.push_back(end);
      }
      ++end;
    }
    std::stable_sort(slots.begin(), slots.end(),
                     [&frames](size_t a, size_t b) { return frames[a].poc < frames[b].poc; });
    for (const size_t i : slots) {
      frames[i].display_index = static_cast<int32_t>(order.size());
      order.push_back(frames[i].decode_index);
    }
    for (size_t i = begin; i < end; ++i) {
      const int32_t first = frames[i].second_field_of;
      if (frames[i].shown && first >= 0 && static_cast<size_t>(first) < frames.size()) {
        frames[i].display_index = frames[static_cast<size_t>(first)].display_index;
      }
    }
    begin = end;
  }
  return order;
}

}  // namespace

std::vector<int32_t> assign_display_order(fb::Codec codec, std::vector<FrameRec>& frames) {
  return codec == fb::Codec::AV1 ? av1_order(frames) : poc_order(frames);
}

}  // namespace ovt::index
