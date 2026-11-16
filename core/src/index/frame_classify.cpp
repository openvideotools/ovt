// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Frame classification per codec (chapter 7 §7.4.2 S6, WI-1.121).

#include "index/frame_classify.h"

#include <algorithm>
#include <limits>

namespace ovt::index {

namespace {

int32_t to_i32(uint64_t v) noexcept {
  return static_cast<int32_t>(std::min<uint64_t>(v, static_cast<uint64_t>(std::numeric_limits<int32_t>::max())));
}

fb::FrameType av1_frame_type(syntax::av1::FrameType t) noexcept {
  switch (t) {
    case syntax::av1::FrameType::Key:
      return fb::FrameType::Key;
    case syntax::av1::FrameType::Inter:
      return fb::FrameType::Inter;
    case syntax::av1::FrameType::IntraOnly:
      return fb::FrameType::IntraOnly;
    case syntax::av1::FrameType::Switch:
      return fb::FrameType::Switch;
  }
  return fb::FrameType::Unknown;
}

}  // namespace

void classify_av1(FrameRec& f, const Av1FrameRecord& r, const syntax::av1::SequenceHeader& seq,
                  uint32_t current_frame_id) {
  f.type = av1_frame_type(r.type);
  f.shown = r.shown;
  f.show_existing_of = r.show_existing_of >= 0 ? to_i32(static_cast<uint64_t>(r.show_existing_of)) : -1;
  f.structure = fb::PictureStructure::Frame;
  f.is_rap = r.is_rap;
  f.poc = r.poc;
  f.qp_base = r.qp_base;
  // r.state is the frame's §7.20 state, or the loaded slot of a show_existing_frame (§7.21).
  f.coded_size = fb::Rect(0, 0, to_i32(r.state.FrameWidth), to_i32(r.state.FrameHeight));
  f.crop = fb::Rect(0, 0, to_i32(r.state.RenderWidth), to_i32(r.state.RenderHeight));
  f.chroma_format = seq.color_config.chroma_format;
  f.bit_depth_luma = r.state.BitDepth;
  f.bit_depth_chroma = r.state.BitDepth;
  f.frame_num = seq.frame_id_numbers_present_flag ? to_i32(current_frame_id) : -1;
}

}  // namespace ovt::index
