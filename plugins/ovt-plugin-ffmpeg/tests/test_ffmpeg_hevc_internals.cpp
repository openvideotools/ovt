// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the H.265 side-data mapping of the ffmpeg_hevc plugin (WI-4.107, map_hevc_sidedata.cpp) on AVFrames
// built by hand, as patch 0002 lays the side data out: one AV_VIDEO_ENC_PARAMS_HEVC block per coding unit and one
// AVMotionVector per list and per 4x4 unit.

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>
}

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "map_hevc_sidedata.hpp"

#if OVT_FFMPEG_HEVC_ENC_PARAMS

namespace {

using ovt::ffmpeg::Box;
using ovt::ffmpeg::Diag;
using ovt::ffmpeg::SideData;

struct FrameDeleter {
  void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

FramePtr make_frame(int w, int h) {
  FramePtr f(av_frame_alloc());
  f->width = w;
  f->height = h;
  f->format = AV_PIX_FMT_YUV420P;
  return f;
}

struct Cu {
  int x, y, size, delta;
};

void add_cus(AVFrame* f, int qp, const std::vector<Cu>& cus, AVVideoEncParamsType type = AV_VIDEO_ENC_PARAMS_HEVC) {
  AVVideoEncParams* par = av_video_enc_params_create_side_data(f, type, static_cast<unsigned>(cus.size()));
  REQUIRE(par != nullptr);
  par->qp = qp;
  for (unsigned i = 0; i < cus.size(); ++i) {
    AVVideoBlockParams* b = av_video_enc_params_block(par, i);
    b->src_x = cus[i].x;
    b->src_y = cus[i].y;
    b->w = cus[i].size;
    b->h = cus[i].size;
    b->delta_qp = cus[i].delta;
  }
}

// One vector per 4x4 unit of the rect (x, y, w, h), quarter samples, as the patched decoder exports them.
void add_units(std::vector<AVMotionVector>* out, int list, int x, int y, int w, int h, int mx, int my) {
  for (int uy = y; uy < y + h; uy += 4) {
    for (int ux = x; ux < x + w; ux += 4) {
      AVMotionVector m{};
      m.source = list == 0 ? -1 : 1;
      m.w = 4;
      m.h = 4;
      m.dst_x = static_cast<int16_t>(ux + 2);
      m.dst_y = static_cast<int16_t>(uy + 2);
      m.motion_x = mx;
      m.motion_y = my;
      m.motion_scale = 4;
      out->push_back(m);
    }
  }
}

void attach(AVFrame* f, const std::vector<AVMotionVector>& mvs) {
  AVFrameSideData* sd = av_frame_new_side_data(f, AV_FRAME_DATA_MOTION_VECTORS, mvs.size() * sizeof(AVMotionVector));
  REQUIRE(sd != nullptr);
  std::copy_n(reinterpret_cast<const uint8_t*>(mvs.data()), sd->size, sd->data);  // NOLINT(*-reinterpret-cast)
}

size_t cell(const SideData& sd, int x, int y) {
  return (static_cast<size_t>(y / 4) * sd.cells_w) + static_cast<size_t>(x / 4);
}

}  // namespace

TEST_CASE("map_hevc_sidedata: CUs give blocks, QP and intra; vectors merge per PU", "[fast][unit]") {
  // 32x16 picture: a 16x16 intra CU at (0,0) and an inter CU at (16,0) split into two 16x8 PUs (2NxN): the top
  // one bi-predicted, the bottom one list 0 only. Base QP 30 at 10 bits: a CU QP of -3 stays negative.
  const FramePtr f = make_frame(32, 16);
  add_cus(f.get(), 30, {{0, 0, 16, -33}, {16, 0, 16, 2}});
  std::vector<AVMotionVector> mvs;
  add_units(&mvs, 0, 16, 0, 16, 8, 5, -6);
  add_units(&mvs, 1, 16, 0, 16, 8, -7, 8);
  add_units(&mvs, 0, 16, 8, 16, 8, 1, 2);
  attach(f.get(), mvs);
  std::vector<Diag> diags;
  const SideData sd = ovt::ffmpeg::map_hevc_sidedata(f.get(), &diags);
  REQUIRE(sd.valid);
  CHECK(diags.empty());
  REQUIRE(sd.cells_w == 8);
  REQUIRE(sd.cells_h == 4);
  REQUIRE(sd.mbs.size() == 2);
  CHECK(sd.mbs[0].rect == Box{0, 0, 16, 16});
  CHECK(sd.mbs[0].qp == -3);
  CHECK(sd.mbs[0].pred == ovt::ffmpeg::kPredIntra);
  CHECK(sd.mbs[1].qp == 32);
  CHECK(sd.mbs[1].pred == ovt::ffmpeg::kPredInter);
  // Three merged vectors: the top PU in both lists (bi-pred), the bottom PU in list 0, in canonical order.
  REQUIRE(sd.mvs.size() == 3);
  CHECK(sd.mvs[0].list == 0);
  CHECK(sd.mvs[0].rect == Box{16, 0, 16, 8});
  CHECK(sd.mvs[0].mv_x == 20);  // 1/16 sample
  CHECK(sd.mvs[0].mv_y == -24);
  CHECK(sd.mvs[0].flags == 1);
  CHECK(sd.mvs[1].list == 0);
  CHECK(sd.mvs[1].rect == Box{16, 8, 16, 8});
  CHECK(sd.mvs[1].flags == 0);
  CHECK(sd.mvs[2].list == 1);
  CHECK(sd.mvs[2].rect == Box{16, 0, 16, 8});
  CHECK(sd.mvs[2].flags == 1);
  for (const auto& m : sd.mvs) {
    CHECK(m.block_id == 2);
  }
  // Cells.
  CHECK(sd.block_id[cell(sd, 4, 4)] == 1);
  CHECK(sd.qp[cell(sd, 4, 4)] == -3);
  CHECK(sd.ref[0][cell(sd, 4, 4)] == -1);
  CHECK(sd.block_id[cell(sd, 28, 12)] == 2);
  CHECK(sd.mv_x[0][cell(sd, 28, 12)] == 4);
  CHECK(sd.ref[1][cell(sd, 28, 12)] == -1);
  CHECK(sd.mv_x[1][cell(sd, 20, 4)] == -28);
  CHECK(sd.usage_cells[0] == 16);
  CHECK(sd.usage_cells[1] == 8);
  REQUIRE(sd.qp_hist.size() == 256);
  CHECK(sd.qp_hist[128 - 3] == 16);
  CHECK(sd.qp_hist[128 + 32] == 16);
}

TEST_CASE("map_hevc_sidedata: no HEVC encoding parameters means no grid", "[fast][unit]") {
  std::vector<Diag> diags;
  const FramePtr none = make_frame(16, 16);
  CHECK_FALSE(ovt::ffmpeg::map_hevc_sidedata(none.get(), &diags).valid);
  const FramePtr h264 = make_frame(16, 16);
  add_cus(h264.get(), 26, {{0, 0, 16, 0}}, AV_VIDEO_ENC_PARAMS_H264);
  CHECK_FALSE(ovt::ffmpeg::map_hevc_sidedata(h264.get(), &diags).valid);
  CHECK(diags.empty());
}

TEST_CASE("map_hevc_sidedata: side data off the picture is dropped with a warning", "[fast][unit]") {
  const FramePtr f = make_frame(16, 16);
  add_cus(f.get(), 26, {{0, 0, 16, 0}, {16, 0, 16, 0}, {2, 0, 8, 0}});  // second outside, third off the 4x4 grid
  std::vector<AVMotionVector> mvs;
  add_units(&mvs, 0, 0, 0, 4, 4, 1 << 20, 0);  // clamped to 32767 in 1/16 units
  add_units(&mvs, 0, 16, 0, 4, 4, 0, 0);       // off the picture
  attach(f.get(), mvs);
  std::vector<Diag> diags;
  const SideData sd = ovt::ffmpeg::map_hevc_sidedata(f.get(), &diags);
  REQUIRE(sd.valid);
  CHECK(sd.mbs.size() == 1);
  REQUIRE(sd.mvs.size() == 1);
  CHECK(sd.mvs[0].mv_x == 32767);
  REQUIRE(diags.size() == 2);
  CHECK(diags[0].code == "MDL-GEN-0003");
  CHECK(diags[1].code.empty());
}

#endif  // OVT_FFMPEG_HEVC_ENC_PARAMS
