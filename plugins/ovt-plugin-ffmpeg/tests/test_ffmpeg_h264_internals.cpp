// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Unit tests of the side-data mapping (WI-2.170) on AVFrames built by hand. This executable compiles the mapping next
// to the static plugin flavour of FFmpeg, like ovt_plugin_ffmpeg itself; it does not use the host.

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>
}

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "map_h264_sidedata.hpp"

namespace {

using ovt::ffmpeg::Box;
using ovt::ffmpeg::Diag;
using ovt::ffmpeg::SideData;

struct FrameDeleter {
  void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

// A frame of w x h coded luma samples; no pixel buffers are needed by the mapping.
FramePtr make_frame(int w, int h) {
  FramePtr f(av_frame_alloc());
  f->width = w;
  f->height = h;
  f->format = AV_PIX_FMT_YUV420P;
  return f;
}

AVMotionVector motion(int source, int w, int h, int dst_x, int dst_y, int mx, int my, int scale = 4) {
  AVMotionVector m{};
  m.source = static_cast<int32_t>(source);
  m.w = static_cast<uint8_t>(w);
  m.h = static_cast<uint8_t>(h);
  m.dst_x = static_cast<int16_t>(dst_x);
  m.dst_y = static_cast<int16_t>(dst_y);
  m.motion_x = mx;
  m.motion_y = my;
  m.motion_scale = static_cast<uint16_t>(scale);
  return m;
}

void add_motion(AVFrame* f, const std::vector<AVMotionVector>& mvs) {
  AVFrameSideData* sd = av_frame_new_side_data(f, AV_FRAME_DATA_MOTION_VECTORS, mvs.size() * sizeof(AVMotionVector));
  REQUIRE(sd != nullptr);
  std::copy_n(reinterpret_cast<const uint8_t*>(mvs.data()), sd->size, sd->data);  // NOLINT(*-reinterpret-cast)
}

// AVVideoEncParams with one 16x16 block per macroblock (raster) and base qp @p qp.
void add_qp(AVFrame* f, int qp, const std::vector<int>& deltas) {
  size_t size = 0;
  AVVideoEncParams* par =
      av_video_enc_params_alloc(AV_VIDEO_ENC_PARAMS_H264, static_cast<unsigned>(deltas.size()), &size);
  REQUIRE(par != nullptr);
  par->qp = qp;
  const int mb_w = (f->width + 15) / 16;
  for (unsigned i = 0; i < deltas.size(); ++i) {
    AVVideoBlockParams* b = av_video_enc_params_block(par, i);
    b->src_x = static_cast<int>(i % static_cast<unsigned>(mb_w)) * 16;
    b->src_y = static_cast<int>(i / static_cast<unsigned>(mb_w)) * 16;
    b->w = 16;
    b->h = 16;
    b->delta_qp = deltas[i];
  }
  AVBufferRef* buf =
      av_buffer_create(reinterpret_cast<uint8_t*>(par), size, av_buffer_default_free, nullptr, 0);  // NOLINT
  REQUIRE(buf != nullptr);
  REQUIRE(av_frame_new_side_data_from_buf(f, AV_FRAME_DATA_VIDEO_ENC_PARAMS, buf) != nullptr);
}

bool has_code(const std::vector<Diag>& d, const std::string& code) {
  return std::any_of(d.begin(), d.end(), [&](const Diag& x) { return x.code == code; });
}

}  // namespace

TEST_CASE("map_h264_sidedata: no side data means no classification", "[fast][third_party]") {
  const FramePtr f = make_frame(32, 32);
  std::vector<Diag> diags;
  const SideData sd = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
  REQUIRE(sd.valid);
  CHECK((sd.cells_w == 8 && sd.cells_h == 8));
  CHECK(sd.mbs.size() == 4);
  CHECK(std::all_of(sd.pred.begin(), sd.pred.end(), [](uint8_t p) { return p == 0; }));
  CHECK(std::all_of(sd.qp.begin(), sd.qp.end(), [](int16_t q) { return q == -1; }));
  CHECK(sd.qp_hist.empty());
  CHECK(sd.mvs.empty());
  CHECK(diags.empty());
}

TEST_CASE("map_h264_sidedata: bi-prediction, units and canonical order", "[fast][third_party]") {
  const FramePtr f = make_frame(32, 16);  // MB 0 and MB 1
  add_motion(f.get(),
             {
                 motion(+1, 16, 16, 8, 8, 3, -5),    // MB 0, L1
                 motion(-1, 16, 16, 8, 8, 4, 6),     // MB 0, L0, same rect: bi-prediction
                 motion(-1, 16, 8, 24, 4, -2, 1),    // MB 1 upper half, L0 only
                 motion(-1, 16, 8, 24, 12, 7, 7, 2)  // MB 1 lower half, motion_scale 2: 16 / scale = 8, never assume 4
             });
  add_qp(f.get(), 26, {0, 3});
  std::vector<Diag> diags;
  const SideData sd = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
  REQUIRE(sd.mvs.size() == 4);
  // block_id, list, y, x order; the vector in 1/16 sample; the rect from the centre
  CHECK((sd.mvs[0].block_id == 1 && sd.mvs[0].list == 0 && sd.mvs[0].mv_x == 16 && sd.mvs[0].mv_y == 24));
  CHECK((sd.mvs[1].block_id == 1 && sd.mvs[1].list == 1 && sd.mvs[1].mv_x == 12 && sd.mvs[1].mv_y == -20));
  CHECK((sd.mvs[0].flags & 1U) == 1U);
  CHECK((sd.mvs[1].flags & 1U) == 1U);
  CHECK(sd.mvs[2].rect == Box{16, 0, 16, 8});
  CHECK((sd.mvs[2].mv_x == -8 && sd.mvs[2].mv_y == 4 && sd.mvs[2].flags == 0));
  CHECK(sd.mvs[3].rect == Box{16, 8, 16, 8});
  CHECK((sd.mvs[3].mv_x == 56 && sd.mvs[3].mv_y == 56));
  CHECK((sd.mbs[0].pred == 2 && sd.mbs[1].pred == 2));
  CHECK((sd.usage_cells[0] == 32 && sd.usage_cells[1] == 16));  // L0: 16x16 + two 16x8; L1: one 16x16 (16 cells each)
  CHECK(sd.ref[1][0] == 1);
  CHECK(sd.ref[1][4] == -1);  // MB 1 has no L1 vector
  CHECK(sd.qp[0] == 26);
  CHECK(sd.qp[4] == 29);
  CHECK_FALSE(has_code(diags, "ANA-H264-0002"));  // every macroblock has a vector: nothing is inferred intra
}

TEST_CASE("map_h264_sidedata: QpBdOffsetY is removed and intra is inferred from QP without vectors",
          "[fast][third_party]") {
  const FramePtr f = make_frame(32, 16);
  f->format = AV_PIX_FMT_YUV420P10LE;
  add_qp(f.get(), 26 + 12, {-12, 5});  // FFmpeg's qp + delta_qp is QP'Y = QPY + QpBdOffsetY = QPY + 12
  add_motion(f.get(), {motion(-1, 16, 16, 24, 8, 1, 1)});
  std::vector<Diag> diags;
  const SideData sd = ovt::ffmpeg::map_h264_sidedata(f.get(), 10, &diags);
  CHECK((sd.mbs[0].qp == 14 && sd.mbs[1].qp == 31));  // 38 - 12 - 12, 38 + 5 - 12
  CHECK(sd.mbs[0].pred == 1);                         // QP, no vector: Intra
  CHECK(sd.mbs[1].pred == 2);
  CHECK(has_code(diags, "ANA-H264-0002"));
  REQUIRE(sd.qp_hist.size() == 256);
  CHECK(sd.qp_hist[14 + 128] == 16);  // the 16 cells of MB 0
}

TEST_CASE("map_h264_sidedata: interlaced frames, misplaced and oversized vectors", "[fast][third_party]") {
  const FramePtr f = make_frame(32, 16);
#ifdef AV_FRAME_FLAG_INTERLACED
  f->flags |= AV_FRAME_FLAG_INTERLACED;
#else
  f->interlaced_frame = 1;
#endif
  add_motion(f.get(), {
                          motion(-1, 16, 16, 8, 8, 0x7FFF, 0),  // x4 -> 131068 does not fit a short
                          motion(-1, 16, 16, 400, 8, 1, 1),     // outside the picture
                          motion(-1, 16, 16, 8, 8, 1, 1, 0)     // motion_scale 0
                      });
  std::vector<Diag> diags;
  const SideData sd = ovt::ffmpeg::map_h264_sidedata(f.get(), 8, &diags);
  REQUIRE(sd.mvs.size() == 1);
  CHECK(sd.mvs[0].mv_x == 32767);
  CHECK((sd.mvs[0].flags & 16U) == 16U);  // field
  CHECK(has_code(diags, "ANA-H264-0001"));
  CHECK(has_code(diags, "MDL-GEN-0003"));
  CHECK(std::any_of(diags.begin(), diags.end(), [](const Diag& d) { return d.code.empty() && d.severity == 1; }));
}
