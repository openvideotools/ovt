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
  // §7.4.4 (WI-1.124): declared_refs in LAST..ALTREF order; dpb_after the 8 slots, -1 for an empty one.
  f.declared_refs.clear();
  for (const int64_t d : r.declared_refs) {
    f.declared_refs.push_back(to_i32(static_cast<uint64_t>(d)));
  }
  f.dpb_after.clear();
  for (const int64_t d : r.dpb_after) {
    f.dpb_after.push_back(d >= 0 ? to_i32(static_cast<uint64_t>(d)) : -1);
  }
}

fb::FrameType h264_slice_frame_type(const syntax::h264::SliceHeader& sh) noexcept {
  switch (sh.slice_type_mod5) {
    case syntax::h264::kSliceB:
      return fb::FrameType::B;
    case syntax::h264::kSliceP:
    case syntax::h264::kSliceSP:
      return fb::FrameType::P;
    default:
      return fb::FrameType::I;
  }
}

fb::FrameType h265_slice_frame_type(const syntax::h265::SliceSegmentHeader& sh) noexcept {
  switch (sh.slice_type) {
    case syntax::h265::kSliceB:
      return fb::FrameType::B;
    case syntax::h265::kSliceP:
      return fb::FrameType::P;
    default:
      return fb::FrameType::I;
  }
}

fb::FrameType merge_frame_type(fb::FrameType a, fb::FrameType b) noexcept {
  if (a == fb::FrameType::B || b == fb::FrameType::B) {
    return fb::FrameType::B;
  }
  return a == fb::FrameType::P || b == fb::FrameType::P ? fb::FrameType::P : fb::FrameType::I;
}

int64_t h265_ticks_per_picture(const syntax::h265::VuiParameters& vui) {
  if (!vui.hrd_parameters) {
    return 1;
  }
  const syntax::h265::HrdParameters& hrd = *vui.hrd_parameters;
  const syntax::h265::SubLayerHrdInfo& top = hrd.sub_layer.at(std::min<size_t>(hrd.max_num_sub_layers_minus1, 6));
  return top.fixed_pic_rate_within_cvs_flag ? int64_t{top.elemental_duration_in_tc_minus1} + 1 : 1;
}

void classify_h264(FrameRec& f, const syntax::h264::NalHeader& nal, const syntax::h264::SliceHeader& sh,
                   const syntax::h264::Sps& sps, const h264::BoundaryDecision& bd, const h264::PocResult& poc) {
  using syntax::h264::PictureStructure;
  f.type = h264_slice_frame_type(sh);
  f.shown = true;  // every H.264 picture is output (S9)
  switch (bd.structure) {
    case PictureStructure::TopField:
      f.structure = fb::PictureStructure::TopField;
      break;
    case PictureStructure::BottomField:
      f.structure = fb::PictureStructure::BottomField;
      break;
    case PictureStructure::Mbaff:
      f.structure = fb::PictureStructure::Mbaff;
      break;
    case PictureStructure::Frame:
      f.structure = fb::PictureStructure::Frame;
      break;
  }
  f.temporal_id = static_cast<uint8_t>(nal.temporal_id);
  f.layer_id = static_cast<uint8_t>(nal.layer_id);
  f.is_rap = sh.IdrPicFlag;
  f.poc = poc.poc;
  f.qp_base = static_cast<int16_t>(sh.SliceQPY);
  // PicHeightInMbs * 16: the frame height, or half of it for a field picture (chapter 6 §6.2.4).
  const uint64_t height = uint64_t{sps.FrameHeightInMbs} * 16 / (sh.field_pic_flag ? 2 : 1);
  f.coded_size = fb::Rect(0, 0, to_i32(sps.PicWidthInSamplesL), to_i32(height));
  f.crop = fb::Rect(sps.crop_x, sps.crop_y, sps.crop_width, sps.crop_height);
  f.chroma_format = static_cast<uint8_t>(sps.chroma_format_idc);
  f.bit_depth_luma = static_cast<uint8_t>(sps.BitDepthY);
  f.bit_depth_chroma = static_cast<uint8_t>(sps.BitDepthC);
  f.frame_num = to_i32(sh.frame_num);
}

void classify_h265(FrameRec& f, const syntax::h265::NalHeader& nal, const syntax::h265::SliceSegmentHeader& sh,
                   const syntax::h265::Sps& sps, const h265::PocResult& poc) {
  f.type = h265_slice_frame_type(sh);
  // SPEC-ASSUMPTION(#695): a RASL picture whose CRA has NoRaslOutputFlag = 1 in the stream itself (first picture
  // or first after an end of sequence) is output by no decoder, so it is not shown; the RASL pictures of any
  // other CRA stay shown in the full index (§7.3.10.2).
  f.shown = sh.pic_output_flag && !poc.rasl_skipped;
  f.structure = fb::PictureStructure::Frame;
  f.temporal_id = nal.TemporalId;
  f.layer_id = nal.nuh_layer_id;
  f.is_rap = nal.is_irap;
  f.poc = poc.poc;
  f.qp_base = static_cast<int16_t>(sh.SliceQpY);
  f.coded_size = fb::Rect(0, 0, to_i32(sps.pic_width_in_luma_samples), to_i32(sps.pic_height_in_luma_samples));
  f.crop = fb::Rect(to_i32(sps.conformance_crop.x), to_i32(sps.conformance_crop.y), to_i32(sps.conformance_crop.w),
                    to_i32(sps.conformance_crop.h));
  f.chroma_format = sps.chroma_format_idc;
  f.bit_depth_luma = static_cast<uint8_t>(sps.BitDepthY);
  f.bit_depth_chroma = static_cast<uint8_t>(sps.BitDepthC);
}

}  // namespace ovt::index
