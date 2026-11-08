// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand-made H.264 units for the plugin tests (no host dependency): a bit writer and a 32x16 Baseline SPS / PPS.
#ifndef OVT_PLUGIN_FFMPEG_TESTS_H264_SYNTH_HPP
#define OVT_PLUGIN_FFMPEG_TESTS_H264_SYNTH_HPP

#include <cstdint>
#include <utility>
#include <vector>

namespace ovt_ffmpeg_test {

class BitWriter {
 public:
  BitWriter& bits(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      bit((value >> i) & 1U);
    }
    return *this;
  }
  BitWriter& ue(uint32_t v) {  // Exp-Golomb, H.264 9.1
    const uint32_t x = v + 1;
    int len = 0;
    while ((x >> len) > 1) {
      ++len;
    }
    return bits(0, len).bits(x, len + 1);
  }
  BitWriter& align_zero() {
    while (n_ != 0) {
      bit(0);
    }
    return *this;
  }
  BitWriter& trailing() { return bits(1, 1).align_zero(); }  // rbsp_trailing_bits
  BitWriter& se(int v) { return ue(v > 0 ? (2U * static_cast<uint32_t>(v)) - 1U : 2U * static_cast<uint32_t>(-v)); }
  void byte(uint8_t b) { bits(b, 8); }
  std::vector<uint8_t> take() { return std::move(out_); }

 private:
  void bit(uint32_t b) {
    cur_ = static_cast<uint8_t>((static_cast<uint32_t>(cur_) << 1U) | b);
    if (++n_ == 8) {
      out_.push_back(cur_);
      cur_ = 0;
      n_ = 0;
    }
  }
  std::vector<uint8_t> out_;
  uint8_t cur_ = 0;
  int n_ = 0;
};

// Constrained Baseline, level 1.0, 32x16 luma (two macroblocks), POC type 2, no VUI.
inline std::vector<uint8_t> pcm_sps() {
  BitWriter w;
  w.bits(0x6742C00AU, 32);       // SPS NAL (nal_ref_idc 3), profile_idc 66, constraint_set0/1, level_idc 10
  w.ue(0).ue(0).ue(2).ue(1);     // sps id, log2_max_frame_num_minus4, pic_order_cnt_type 2, max_num_ref_frames
  w.bits(0, 1).ue(1).ue(0);      // no frame_num gaps; 2x1 macroblocks
  w.bits(0b1100, 4).trailing();  // frame_mbs_only, direct_8x8_inference, no cropping, no VUI
  return w.take();
}

inline std::vector<uint8_t> pcm_pps() {
  BitWriter w;
  w.bits(0x68, 8).ue(0).ue(0);  // PPS NAL, pps id, sps id
  w.bits(0, 2).ue(0);           // CAVLC, no bottom_field_pic_order_in_frame_present, one slice group
  w.ue(0).ue(0).bits(0, 3);     // one reference per list, no weighted prediction
  w.ue(0).ue(0).ue(0);          // pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset: se(0)
  w.bits(0b100, 3).trailing();  // deblocking_filter_control_present; no constrained intra, no redundant_pic_cnt
  return w.take();
}

}  // namespace ovt_ffmpeg_test

#endif  // OVT_PLUGIN_FFMPEG_TESTS_H264_SYNTH_HPP
