// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the block-params mapping (WI-4.102) on AVFrames built by hand, without the host: the mb_type and
// sub_mb_type tables, B macroblocks, field flags, and the phase-1 fallback when the side data is absent or does not
// fit the frame ("keep the phase-1 path working", chapter 07 §7.8).

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>
}

#if defined(AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS) && __has_include(<libavutil/video_block_params.h>)
#define OVT_TEST_BLOCK_PARAMS 1
extern "C" {
#include <libavutil/video_block_params.h>
}
#else
#define OVT_TEST_BLOCK_PARAMS 0
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "map_h264_blockparams.hpp"
#include "map_h264_sidedata.hpp"

namespace {

using ovt::ffmpeg::avc_mb_type;
using ovt::ffmpeg::avc_sub_mb_type;
using ovt::ffmpeg::BlockParams;
using ovt::ffmpeg::Diag;
using ovt::ffmpeg::SideData;

struct FrameDeleter {
  void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

// A 32x16 frame (two macroblocks) with one 16x16 L0 vector per macroblock and QP 30, as libavcodec exports them.
FramePtr make_frame() {
  FramePtr f(av_frame_alloc());
  f->width = 32;
  f->height = 16;
  f->format = AV_PIX_FMT_YUV420P;
  std::array<AVMotionVector, 3> mvs{};
  for (size_t i = 0; i < mvs.size(); ++i) {  // MB 0: L0 + L1 (bi), MB 1: L0
    AVMotionVector& m = mvs.at(i);
    m.source = i == 1 ? 1 : -1;
    m.w = 16;
    m.h = 16;
    m.dst_x = static_cast<int16_t>(i == 2 ? 24 : 8);
    m.dst_y = 8;
    m.motion_x = 4;
    m.motion_scale = 4;
  }
  AVFrameSideData* sd = av_frame_new_side_data(f.get(), AV_FRAME_DATA_MOTION_VECTORS, sizeof(mvs));
  REQUIRE(sd != nullptr);
  std::copy_n(reinterpret_cast<const uint8_t*>(mvs.data()), sd->size, sd->data);  // NOLINT(*-reinterpret-cast)
  AVVideoEncParams* par = av_video_enc_params_create_side_data(f.get(), AV_VIDEO_ENC_PARAMS_H264, 2);
  REQUIRE(par != nullptr);
  par->qp = 30;
  par->delta_qp[1][0] = 4;  // chroma_qp_index_offset
  par->delta_qp[2][0] = -2;
  for (unsigned i = 0; i < 2; ++i) {
    AVVideoBlockParams* b = av_video_enc_params_block(par, i);
    b->src_x = static_cast<int>(i) * 16;
    b->w = 16;
    b->h = 16;
  }
  return f;
}

}  // namespace

TEST_CASE("ffmpeg plugin: block-params mb_type classes map to the unified AvcMbType", "[fast][third_party]") {
  constexpr std::array<uint8_t, 4> kNone{};
  CHECK(avc_mb_type(0, kNone, -1, 0) == 0);                // I_NxN
  CHECK(avc_mb_type(1, kNone, 2, 15 + (2 << 4)) == 23);    // I_16x16_2_2_1
  CHECK(avc_mb_type(1, kNone, 3, 0) == 4);                 // I_16x16_3_0_0
  CHECK(avc_mb_type(1, kNone, 4, 0) == 255);               // no such mode
  CHECK(avc_mb_type(1, kNone, 0, 3 << 4) == 255);          // no such chroma pattern
  CHECK(avc_mb_type(2, kNone, -1, 0) == 25);               // I_PCM
  CHECK(avc_mb_type(3, {1, 1, 1, 1}, -1, 0) == 26);        // P_L0_16x16
  CHECK(avc_mb_type(5, {1, 1, 1, 1}, -1, 0) == 28);        // P_L0_L0_8x16
  CHECK(avc_mb_type(6, {1, 1, 1, 1}, -1, 0) == 29);        // P_8x8 (and P_8x8ref0)
  CHECK(avc_mb_type(7, {1, 1, 1, 1}, -1, 0) == 54);        // P_Skip
  CHECK(avc_mb_type(8, {3, 3, 3, 3}, -1, 0) == 31);        // B_Direct_16x16
  CHECK(avc_mb_type(9, {2, 2, 2, 2}, -1, 0) == 33);        // B_L1_16x16
  CHECK(avc_mb_type(9, {0, 0, 0, 0}, -1, 0) == 255);       // a B_16x16 with no direction
  CHECK(avc_mb_type(10, {2, 2, 3, 3}, -1, 0) == 31 + 14);  // B_L1_Bi_16x8
  CHECK(avc_mb_type(11, {3, 1, 3, 1}, -1, 0) == 31 + 17);  // B_Bi_L0_8x16
  CHECK(avc_mb_type(10, {1, 1, 1, 1}, -1, 0) == 31 + 4);   // B_L0_L0_16x8
  CHECK(avc_mb_type(11, {3, 3, 3, 3}, -1, 0) == 31 + 21);  // B_Bi_Bi_8x16
  CHECK(avc_mb_type(12, {1, 2, 3, 3}, -1, 0) == 53);       // B_8x8
  CHECK(avc_mb_type(13, {3, 3, 3, 3}, -1, 0) == 55);       // B_Skip
  CHECK(avc_mb_type(14, kNone, -1, 0) == 255);
  CHECK(avc_mb_type(255, kNone, -1, 0) == 255);  // not decoded

  CHECK(avc_sub_mb_type(false, 0, 1) == 0);    // P_L0_8x8
  CHECK(avc_sub_mb_type(false, 3, 1) == 3);    // P_L0_4x4
  CHECK(avc_sub_mb_type(true, 0x10, 3) == 4);  // B_Direct_8x8
  CHECK(avc_sub_mb_type(true, 0, 2) == 6);     // B_L1_8x8
  CHECK(avc_sub_mb_type(true, 1, 1) == 8);     // B_L0_8x4
  CHECK(avc_sub_mb_type(true, 2, 3) == 13);    // B_Bi_4x8
  CHECK(avc_sub_mb_type(true, 3, 2) == 15);    // B_L1_4x4
  CHECK(avc_sub_mb_type(true, 0, 0) == 255);   // no direction
  CHECK(avc_sub_mb_type(true, 4, 1) == 255);   // no such shape
}

TEST_CASE("ffmpeg plugin: without block params the phase-1 mapping stands", "[fast][third_party]") {
  const FramePtr f = make_frame();
  std::vector<Diag> diags{{0, "ANA-H264-0002", "inferred"}};
  SideData side = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
  REQUIRE(side.valid);
  const SideData before = side;
  const BlockParams bp = ovt::ffmpeg::map_h264_blockparams(f.get(), 8, &side, &diags);
  CHECK_FALSE(bp.valid);
  CHECK(bp.mbs.empty());
  CHECK(side.ref == before.ref);
  CHECK(side.pred == before.pred);
  REQUIRE(side.mvs.size() == 3);
  CHECK((side.mvs[0].ref == 0 && side.mvs[1].ref == 1 && side.mvs[0].mvd_x == -32768));  // pseudo slots, no mvd
  CHECK(std::any_of(diags.begin(), diags.end(), [](const Diag& d) { return d.code == "ANA-H264-0002"; }));
}

#if OVT_TEST_BLOCK_PARAMS

namespace {

AVH264MacroblockParams* add_block_params(AVFrame* f, unsigned n) {
  AVVideoBlockParamsHeader* par = av_video_block_params_create_side_data(f, AV_VIDEO_BLOCK_PARAMS_H264, n);
  REQUIRE(par != nullptr);
  for (unsigned i = 0; i < n; ++i) {
    auto* p = static_cast<AVH264MacroblockParams*>(av_video_block_params_block(par, i));
    p->src_x = static_cast<int32_t>(i % 2) * 16;
    p->src_y = static_cast<int32_t>(i / 2) * 16;
    std::fill(std::begin(p->intra4x4_pred_mode), std::end(p->intra4x4_pred_mode), int8_t{-1});
    std::fill(std::begin(p->intra8x8_pred_mode), std::end(p->intra8x8_pred_mode), int8_t{-1});
    p->intra16x16_pred_mode = -1;
    p->intra_chroma_pred_mode = -1;
  }
  return static_cast<AVH264MacroblockParams*>(av_video_block_params_block(par, 0));
}

}  // namespace

TEST_CASE("ffmpeg plugin: B macroblocks, field flags and chroma QPs from block params", "[fast][third_party]") {
  const FramePtr f = make_frame();
  f->flags |= AV_FRAME_FLAG_INTERLACED;
  AVH264MacroblockParams* p = add_block_params(f.get(), 2);
  // MB 0: B_Bi_16x16 with ref_idx L0 2 / L1 0, mvd L0 (3, -1); MB 1: B_8x8 direct / L1 8x4 / Bi 4x4 / L0 8x8.
  p[0].mb_type_class = 9;
  p[0].mb_field_decoding_flag = 1;
  std::fill(std::begin(p[0].pred_dir), std::end(p[0].pred_dir), uint8_t{3});
  std::fill(std::begin(p[0].ref_idx[0]), std::end(p[0].ref_idx[0]), int8_t{2});
  std::fill(std::begin(p[0].ref_idx[1]), std::end(p[0].ref_idx[1]), int8_t{0});
  for (auto& v : p[0].mvd[0]) {
    v[0] = 3;
    v[1] = -1;
  }
  p[1].mb_type_class = 12;
  p[1].qp_delta = -2;
  p[1].cbp = 0x21;  // luma 8x8 0 coded, chroma AC
  const std::array<uint8_t, 4> sub = {0x10, 1, 3, 0};
  const std::array<uint8_t, 4> dir = {3, 2, 3, 1};
  for (size_t k = 0; k < 4; ++k) {
    p[1].sub_mb_type[k] = sub.at(k);
    p[1].pred_dir[k] = dir.at(k);
    p[1].ref_idx[0][k] = (dir.at(k) & 1U) != 0 ? 0 : -1;
    p[1].ref_idx[1][k] = (dir.at(k) & 2U) != 0 ? 1 : -1;
  }
  std::vector<Diag> diags;
  SideData side = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
  const BlockParams bp = ovt::ffmpeg::map_h264_blockparams(f.get(), 8, &side, &diags);
  REQUIRE(bp.valid);
  REQUIRE(bp.mbs.size() == 2);
  const ovt::ffmpeg::AvcMbInfo& a = bp.mbs[0];
  CHECK(a.mb_type == 34);     // B_Bi_16x16
  CHECK(a.mb_type_raw == 3);  // a B slice: raw = unified - 31
  CHECK(a.mb_field_decoding_flag == 1);
  CHECK(a.ref_idx[0] == std::vector<int8_t>{2, 2, 2, 2});
  CHECK(a.direct_8x8 == std::vector<uint8_t>{0, 0, 0, 0});
  CHECK(a.qp_y == 30);
  CHECK((a.qpc_cb == 32 && a.qpc_cr == 28));  // Table 8-15: qPI 34 -> 32, qPI 28 -> 28
  const ovt::ffmpeg::AvcMbInfo& b = bp.mbs[1];
  CHECK(b.mb_type == 53);                                      // B_8x8
  CHECK(b.sub_mb_type == std::vector<uint8_t>{4, 10, 16, 5});  // B_Direct_8x8, B_L1_8x4, B_Bi_4x4, B_L0_8x8
  CHECK(b.direct_8x8 == std::vector<uint8_t>{1, 0, 0, 0});
  CHECK(b.direct);
  CHECK(b.mb_qp_delta == -2);
  CHECK(b.mvd_x[0][0] == -32768);  // direct 8x8: no mvd
  CHECK(b.mvd_x[1][4] == 0);       // L1 8x4: signalled (zero)
  CHECK(b.mvd_x[0][4] == -32768);  // L1 only: no list 0 mvd
  CHECK(b.pred_flag[8] == 3);
  CHECK(b.mb_field_decoding_flag == 0);
  // Slots: L0[0..2] then L1[0..1]; MB 0's vectors name L0[2] (index 2) and L1[0] (index 3).
  CHECK((bp.ref_slots[0] == 3 && bp.ref_slots[1] == 2));
  REQUIRE(side.mvs.size() == 3);
  CHECK((side.mvs[0].ref == 2 && side.mvs[0].mvd_x == 12 && side.mvs[0].mvd_y == -4));
  CHECK(side.mvs[1].ref == 3);
  CHECK((side.mvs[0].flags & 0x10U) != 0);  // field macroblock
  CHECK((side.mvs[2].flags & 0x10U) == 0);  // frame macroblock of the same interlaced frame
  CHECK(side.mvs[2].ref == 0);              // MB 1's 16x16 test vector lies in its direct 8x8: ref_idx 0
  CHECK(bp.cbf[0] == 0);                    // B_Bi_16x16 with cbp 0
  CHECK(bp.cbf[4] == 7);                    // MB 1, 8x8 0: luma bit + chroma
  CHECK(bp.cbf[6] == 6);                    // MB 1, 8x8 1: chroma only
  CHECK(bp.slices.size() == 1);
  CHECK(bp.slices[0].slice_type == 1);  // B
  CHECK(side.pred[0] == 2);             // Inter
}

TEST_CASE("ffmpeg plugin: block params that do not fit the frame are ignored", "[fast][third_party]") {
  SECTION("fewer records than macroblocks") {
    const FramePtr f = make_frame();
    add_block_params(f.get(), 1);
    std::vector<Diag> diags;
    SideData side = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
    CHECK_FALSE(ovt::ffmpeg::map_h264_blockparams(f.get(), 8, &side, &diags).valid);
  }
  SECTION("two records for one macroblock") {
    const FramePtr f = make_frame();
    AVH264MacroblockParams* p = add_block_params(f.get(), 2);
    p[1].src_x = 0;
    std::vector<Diag> diags;
    SideData side = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
    CHECK_FALSE(ovt::ffmpeg::map_h264_blockparams(f.get(), 8, &side, &diags).valid);
  }
  SECTION("no macroblock decoded") {
    const FramePtr f = make_frame();
    AVH264MacroblockParams* p = add_block_params(f.get(), 2);
    p[0].mb_type_class = 255;
    p[1].mb_type_class = 255;
    std::vector<Diag> diags;
    SideData side = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
    CHECK_FALSE(ovt::ffmpeg::map_h264_blockparams(f.get(), 8, &side, &diags).valid);
    CHECK(side.mvs[0].ref == 0);
  }
  SECTION("one macroblock not decoded keeps its phase-1 values") {
    const FramePtr f = make_frame();
    AVH264MacroblockParams* p = add_block_params(f.get(), 2);
    p[0].mb_type_class = 255;
    p[1].mb_type_class = 3;
    p[1].pred_dir[0] = p[1].pred_dir[1] = p[1].pred_dir[2] = p[1].pred_dir[3] = 1;
    std::vector<Diag> diags;
    SideData side = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
    const BlockParams bp = ovt::ffmpeg::map_h264_blockparams(f.get(), 8, &side, &diags);
    REQUIRE(bp.valid);
    CHECK_FALSE(bp.mbs[0].decoded);
    CHECK(bp.intra_mode_y[0] == -1);
    CHECK(bp.tx_log2[0] == 255);
    CHECK(side.pred[0] == 2);  // phase 1: it has a vector
    CHECK(side.mvs[0].ref == 0);
    CHECK(side.mvs[1].ref == 1);  // its list-1 vector keeps the first L1 slot (after the one L0 slot)
    CHECK((bp.ref_slots[0] == 1 && bp.ref_slots[1] == 1));
  }
}

#endif  // OVT_TEST_BLOCK_PARAMS
