// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.102: the block params of the OVT FFmpeg patch mapped into AvcMb and the phase-4 grid fields, through the plugin
// ABI, on hand-made CAVLC streams whose syntax is known: an I_NxN macroblock whose Intra4x4PredMode values follow from
// prev_intra4x4_pred_mode_flag / rem_intra4x4_pred_mode by H.264 8.3.1.1 (acceptance test 1 in miniature), and P
// pictures with two reference indices, P_8x8 sub-partitions and a P_Skip (acceptance test 2's ref_idx part). The JVT
// streams of both acceptance tests run against the JM goldens in test_ffmpeg_blockparams.cpp. Skipped when the plugin
// was built without the patch (OVT_SYSTEM_FFMPEG with an unpatched FFmpeg).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "plugin_test_util.inc"

namespace {

using namespace ovt_ffmpeg_test;  // NOLINT(google-build-using-namespace)

constexpr int16_t kNoMvd = -32768;

template <typename T, typename V>
std::vector<T> vec(const V* v) {
  std::vector<T> out;
  if (v != nullptr) {
    for (auto x : *v) {
      out.push_back(static_cast<T>(x));
    }
  }
  return out;
}

// The parts of one AvcMb (and its Block) the tests compare.
struct Mb {
  uint8_t pred = 0;
  uint8_t partition_type = 0;
  bool skip = false;
  uint32_t mb_addr = 0;
  int mb_type = 0;
  int mb_type_raw = 0;
  int mb_skip_flag = 0;
  int cbp = 0;
  int t8 = 0;
  int8_t qpd = 0;
  int8_t qp = 0;
  int i16 = 0;
  int chroma = 0;
  int slice = 0;
  std::vector<int> sub;
  std::vector<int> i4;
  std::vector<int> ref0;
  std::vector<int> ref1;
  std::vector<int> mvd0_x;
  std::vector<int> mvd0_y;
  std::vector<int> mvd1_x;
};

struct Mv {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int ref = 0;
  int mvd_x = 0;
  int mvd_y = 0;
  int flags = 0;
};

struct Frame {
  std::vector<Mb> mbs;
  std::vector<Mv> mvs;
  std::vector<std::string> ref_names;
  std::vector<uint32_t> usage;
  std::vector<int> intra_mode_y;
  std::vector<int> intra_mode_c;
  std::vector<int> tx_w;
  std::vector<int> tx_h;
  std::vector<int> cbf;
  std::vector<int> partition_id;
  std::vector<int> ref0;
  std::vector<int> pred;
  std::vector<std::string> codes;
  size_t partitions = 0;
  uint32_t slice_blocks = 0;
  int slice_type = 0;
};

Frame read_frame(const ovt::fb::FrameAnalysis* fa) {
  Frame f;
  const ovt::fb::CellGrid* g = fa->grid();
  REQUIRE(g != nullptr);
  f.intra_mode_y = vec<int>(g->intra_mode_y());
  f.intra_mode_c = vec<int>(g->intra_mode_c());
  f.tx_w = vec<int>(g->tx_log2_w());
  f.tx_h = vec<int>(g->tx_log2_h());
  f.cbf = vec<int>(g->cbf());
  f.partition_id = vec<int>(g->partition_id());
  f.ref0 = vec<int>(g->ref0());
  f.pred = vec<int>(g->pred());
  for (uint32_t i = 1; i < fa->blocks()->size(); ++i) {
    const ovt::fb::Block* b = fa->blocks()->Get(i);
    REQUIRE(b->ext() != nullptr);
    flatbuffers::Verifier verifier(b->ext()->data(), b->ext()->size());
    REQUIRE(verifier.VerifyBuffer<ovt::fb::AvcMb>(nullptr));
    const auto* e = flatbuffers::GetRoot<ovt::fb::AvcMb>(b->ext()->data());
    Mb m;
    m.pred = static_cast<uint8_t>(b->pred());
    m.partition_type = b->partition_type();
    m.skip = b->skip();
    m.mb_addr = e->mb_addr();
    m.mb_type = static_cast<int>(e->mb_type());
    m.mb_type_raw = e->mb_type_raw();
    m.mb_skip_flag = e->mb_skip_flag();
    m.cbp = e->coded_block_pattern();
    m.t8 = e->transform_size_8x8_flag();
    m.qpd = e->mb_qp_delta();
    m.qp = e->QPY();
    m.i16 = static_cast<int>(e->Intra16x16PredMode());
    m.chroma = static_cast<int>(e->intra_chroma_pred_mode());
    m.slice = e->slice_index();
    m.sub = vec<int>(e->sub_mb_type());
    m.i4 = vec<int>(e->intra4x4_pred_mode());
    m.ref0 = vec<int>(e->ref_idx_l0());
    m.ref1 = vec<int>(e->ref_idx_l1());
    m.mvd0_x = vec<int>(e->mvd_l0_x());
    m.mvd0_y = vec<int>(e->mvd_l0_y());
    m.mvd1_x = vec<int>(e->mvd_l1_x());
    f.mbs.push_back(m);
  }
  for (const ovt::fb::MotionVector* m : *fa->mvs()) {
    f.mvs.push_back(Mv{m->rect()->x(), m->rect()->y(), m->rect()->w(), m->rect()->h(), m->ref(), m->mvd_x(), m->mvd_y(),
                       m->flags()});
  }
  for (const ovt::fb::RefSlot* r : *fa->refs()) {
    f.ref_names.push_back(r->name()->str());
    f.usage.push_back(r->usage_cells());
  }
  if (fa->diagnostics() != nullptr) {
    for (const ovt::fb::Diagnostic* d : *fa->diagnostics()) {
      f.codes.push_back(d->code() != nullptr ? d->code()->str() : "");
    }
  }
  if (fa->partitions() != nullptr) {
    f.partitions = fa->partitions()->size();
    if (f.partitions > 0) {
      f.slice_blocks = fa->partitions()->Get(0)->block_count();
      f.slice_type = fa->partitions()->Get(0)->slice_type();
    }
  }
  return f;
}

std::vector<Frame> analyse(const std::vector<uint8_t>& bytes, int threads) {
  std::vector<Frame> frames;
  for_each_frame(bytes, threads, [&](const ovt::fb::FrameAnalysis* fa) { frames.push_back(read_frame(fa)); });
  return frames;
}

void annexb(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal) {
  out->insert(out->end(), {0, 0, 0, 1});
  out->insert(out->end(), nal.begin(), nal.end());
}

// The 32x16 Constrained Baseline SPS of h264_synth.hpp with two reference frames.
std::vector<uint8_t> sps_two_refs() {
  BitWriter w;
  w.bits(0x6742C00AU, 32);
  w.ue(0).ue(0).ue(2).ue(2);  // sps id, log2_max_frame_num_minus4, pic_order_cnt_type 2, max_num_ref_frames 2
  w.bits(0, 1).ue(1).ue(0);
  w.bits(0b1100, 4).trailing();
  return w.take();
}

// Intra4x4PredMode per luma4x4BlkIdx of macroblock 0 of intra_idr(), and how each is signalled given the left/top
// picture edge (8.3.1.1: predIntra4x4PredMode = 2 with a neighbour missing, else min(A, B)): -1 for
// prev_intra4x4_pred_mode_flag = 1, else rem_intra4x4_pred_mode. Blocks (raster of 4x4 blocks):
//    0  1  4  5          DC  H   HU  H
//    2  3  6  7    =     V   HU  DDR HD
//    8  9 12 13          VL  DDL H   H
//   10 11 14 15          V   VR  DC  DDR
constexpr std::array<int, 16> kModes = {2, 1, 0, 8, 8, 1, 4, 6, 7, 3, 0, 5, 1, 1, 2, 4};
constexpr std::array<int, 16> kSignalled = {-1, 1, 0, 7, 7, 1, 4, 5, 6, 3, 0, 4, 1, -1, 1, 3};

// IDR, one slice: MB 0 I_NxN with kModes, chroma DC, cbp 0; MB 1 I_16x16_2_0_0 (DC), chroma horizontal,
// mb_qp_delta 3.
std::vector<uint8_t> intra_idr() {
  BitWriter w;
  w.bits(0x65, 8).ue(0).ue(7).ue(0);  // IDR NAL, first_mb_in_slice, slice_type I (all), pps id
  w.bits(0, 4).ue(0);                 // frame_num, idr_pic_id
  w.bits(0, 2).se(0).ue(1);  // no_output_of_prior_pics, long_term_reference_flag, slice_qp_delta, no deblocking
  w.ue(0);                   // mb_type I_NxN
  for (const int s : kSignalled) {
    if (s < 0) {
      w.bits(1, 1);
    } else {
      w.bits(0, 1).bits(static_cast<uint32_t>(s), 3);
    }
  }
  w.ue(0).ue(3);                   // intra_chroma_pred_mode DC; coded_block_pattern 0 (Intra_4x4 code number 3)
  w.ue(3).ue(1).se(3).bits(1, 1);  // I_16x16_2_0_0, chroma horizontal, mb_qp_delta, Intra16x16DCLevel: no coefficient
  w.trailing();
  return w.take();
}

struct PMb {
  bool skip = false;
  uint32_t mb_type = 0;                  // P_L0_16x16 0, P_8x8 3
  std::vector<uint32_t> sub;             // P_8x8: sub_mb_type x4
  std::vector<int> ref_idx;              // when two references are active: one per partition
  std::vector<std::pair<int, int>> mvd;  // in syntax order
};

// A P picture (nal_ref_idc 3, CAVLC, no residual) with @p refs active references; skipped macroblocks only at the end.
std::vector<uint8_t> p_picture(int frame_num, int refs, const std::vector<PMb>& mbs) {
  BitWriter w;
  w.bits(0x61, 8).ue(0).ue(5).ue(0);  // non-IDR NAL, first_mb_in_slice, slice_type P (all), pps id
  w.bits(static_cast<uint32_t>(frame_num), 4);
  w.bits(refs > 1 ? 1 : 0, 1);  // num_ref_idx_active_override_flag
  if (refs > 1) {
    w.ue(static_cast<uint32_t>(refs - 1));
  }
  w.bits(0, 1).bits(0, 1).se(0).ue(1);  // no list modification, no adaptive marking, slice_qp_delta, no deblocking
  uint32_t skip_run = 0;
  for (const PMb& mb : mbs) {
    if (mb.skip) {
      ++skip_run;
      continue;
    }
    w.ue(skip_run).ue(mb.mb_type);
    skip_run = 0;
    for (const uint32_t s : mb.sub) {
      w.ue(s);
    }
    for (const int r : mb.ref_idx) {
      w.bits(r == 0 ? 1 : 0, 1);  // te(v) with range 1
    }
    for (const auto& [x, y] : mb.mvd) {
      w.se(x).se(y);
    }
    w.ue(0);  // coded_block_pattern 0 (inter code number 0)
  }
  if (skip_run != 0) {
    w.ue(skip_run);
  }
  w.trailing();
  return w.take();
}

std::vector<uint8_t> p_stream() {
  std::vector<uint8_t> s;
  annexb(&s, sps_two_refs());
  annexb(&s, pcm_pps());
  annexb(&s, intra_idr());
  // P1: MB 0 P_L0_16x16 mvd (1, 2); MB 1 P_Skip.
  annexb(&s, p_picture(1, 1, {PMb{false, 0, {}, {}, {{1, 2}}}, PMb{true, 0, {}, {}, {}}}));
  // P2 with RefPicList0 = [P1, IDR]: MB 0 P_L0_16x16 ref_idx 1 mvd (-2, 3); MB 1 P_8x8 with sub-macroblocks 8x8,
  // 8x4, 4x8, 8x8, ref_idx 0, 1, 0, 1.
  annexb(&s,
         p_picture(2, 2,
                   {PMb{false, 0, {}, {1}, {{-2, 3}}},
                    PMb{false, 3, {0, 1, 2, 0}, {0, 1, 0, 1}, {{4, 0}, {1, 1}, {-1, -1}, {0, 5}, {0, -5}, {6, 6}}}}));
  return s;
}

// The 4x4 cell (x, y) of the 8x4 grid of the 32x16 picture.
size_t cell(int x, int y) {
  return (static_cast<size_t>(y) * 8) + static_cast<size_t>(x);
}

int blk_at(int bx, int by) {
  return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2);
}

}  // namespace

TEST_CASE("ffmpeg plugin: block params give AvcMb, intra modes and transform grids", "[fast][third_party]") {
  if (!has_block_params()) {
    SKIP("the plugin's FFmpeg has no block params (OVT patch 0001)");
  }
  const int threads = GENERATE(1, 4);
  CAPTURE(threads);
  const std::vector<Frame> frames = analyse(p_stream(), threads);
  REQUIRE(frames.size() == 3);
  const Frame& f = frames[0];
  REQUIRE(f.mbs.size() == 2);
  const Mb& nxn = f.mbs[0];
  CHECK(nxn.mb_addr == 0);
  CHECK(nxn.mb_type == 0);  // I_NxN
  CHECK(nxn.mb_type_raw == 0);
  CHECK(nxn.partition_type == 0);
  CHECK(nxn.pred == 1);  // PredKind::Intra
  CHECK(nxn.mb_skip_flag == 0);
  CHECK(nxn.i4 == std::vector<int>(kModes.begin(), kModes.end()));  // AT1: Intra4x4PredMode in luma4x4BlkIdx order
  CHECK(nxn.chroma == 0);
  CHECK(nxn.cbp == 0);
  CHECK(nxn.t8 == 0);
  CHECK(nxn.qp == 26);
  CHECK(nxn.ref0.empty());
  const Mb& i16 = f.mbs[1];
  CHECK(i16.mb_addr == 1);
  CHECK(i16.mb_type == 3);  // I_16x16_2_0_0
  CHECK(i16.mb_type_raw == 3);
  CHECK(i16.i16 == 2);
  CHECK(i16.chroma == 1);
  CHECK(i16.qpd == 3);
  CHECK(i16.qp == 29);
  CHECK(i16.t8 == 255);  // not signalled for I_16x16
  CHECK(i16.i4.empty());
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 8; ++x) {
      CAPTURE(x, y);
      const size_t c = cell(x, y);
      CHECK(f.intra_mode_y[c] == (x < 4 ? kModes.at(static_cast<size_t>(blk_at(x, y))) : 16 + 2));
      CHECK(f.intra_mode_c[c] == (x < 4 ? 0 : 1));
      CHECK((f.tx_w[c] == 2 && f.tx_h[c] == 2));
      CHECK(f.cbf[c] == 0);
      CHECK(f.partition_id[c] == 0);
    }
  }
  CHECK(f.partitions == 1);
  CHECK(f.slice_blocks == 2);
  CHECK(f.slice_type == 2);  // I
  CHECK(f.ref_names.empty());
  CHECK(f.codes.empty());  // no ANA-H264-0002: the prediction modes are known
}

TEST_CASE("ffmpeg plugin: block params give true reference indices, mvds and skipped macroblocks",
          "[fast][third_party]") {
  if (!has_block_params()) {
    SKIP("the plugin's FFmpeg has no block params (OVT patch 0001)");
  }
  const int threads = GENERATE(1, 4);
  CAPTURE(threads);
  const std::vector<Frame> frames = analyse(p_stream(), threads);
  REQUIRE(frames.size() == 3);

  const Frame& a = frames[1];
  REQUIRE(a.mbs.size() == 2);
  CHECK(a.mbs[0].mb_type == 26);  // P_L0_16x16
  CHECK(a.mbs[0].mb_type_raw == 0);
  CHECK(a.mbs[0].ref0 == std::vector<int>{0, 0, 0, 0});
  CHECK(a.mbs[0].ref1 == std::vector<int>{-1, -1, -1, -1});
  CHECK(a.mbs[0].mvd0_x == std::vector<int>(16, 1));  // quarter samples, replicated over the partition
  CHECK(a.mbs[0].mvd0_y == std::vector<int>(16, 2));
  CHECK(a.mbs[0].mvd1_x == std::vector<int>(16, kNoMvd));
  const Mb& skip = a.mbs[1];
  CHECK(skip.mb_type == 54);  // P_Skip
  CHECK(skip.mb_type_raw == 255);
  CHECK(skip.mb_skip_flag == 1);
  CHECK(skip.skip);
  CHECK(skip.pred == 3);  // PredKind::Skip
  CHECK(skip.cbp == 255);
  CHECK(skip.ref0 == std::vector<int>{0, 0, 0, 0});
  CHECK(skip.mvd0_x == std::vector<int>(16, kNoMvd));  // derived, not signalled
  CHECK(a.ref_names == std::vector<std::string>{"L0[0]"});
  CHECK(a.usage == std::vector<uint32_t>{32});
  for (int x = 4; x < 8; ++x) {
    CHECK((a.tx_w[cell(x, 0)] == 255 && a.cbf[cell(x, 0)] == 0 && a.pred[cell(x, 0)] == 3));  // no transform
  }
  REQUIRE(a.mvs.size() == 2);
  CHECK((a.mvs[0].ref == 0 && a.mvs[0].mvd_x == 4 && a.mvs[0].mvd_y == 8));  // 1/16 samples
  CHECK((a.mvs[1].ref == 0 && a.mvs[1].mvd_x == kNoMvd && a.mvs[1].mvd_y == kNoMvd));
  CHECK((a.mvs[0].flags & 0x10) == 0);  // progressive: no field flag

  const Frame& b = frames[2];
  REQUIRE(b.mbs.size() == 2);
  CHECK(b.ref_names == std::vector<std::string>{"L0[0]", "L0[1]"});
  CHECK(b.mbs[0].ref0 == std::vector<int>{1, 1, 1, 1});  // AT2 in miniature: ref_idx_l0 as signalled
  CHECK(b.mbs[0].mvd0_x == std::vector<int>(16, -2));
  const Mb& p8 = b.mbs[1];
  CHECK(p8.mb_type == 29);  // P_8x8
  CHECK(p8.mb_type_raw == 3);
  CHECK(p8.sub == std::vector<int>{0, 1, 2, 0});  // P_L0_8x8, P_L0_8x4, P_L0_4x8, P_L0_8x8
  CHECK(p8.ref0 == std::vector<int>{0, 1, 0, 1});
  // luma4x4BlkIdx order: 8x8 0 one mvd; 8x8 1 (8x4) top 4, 5 / bottom 6, 7; 8x8 2 (4x8) left 8, 10 / right 9, 11.
  CHECK(p8.mvd0_x == std::vector<int>{4, 4, 4, 4, 1, 1, -1, -1, 0, 0, 0, 0, 6, 6, 6, 6});
  CHECK(p8.mvd0_y == std::vector<int>{0, 0, 0, 0, 1, 1, -1, -1, 5, -5, 5, -5, 6, 6, 6, 6});
  // The grid's ref0 names the slot of each 8x8: MB 0 L0[1]; MB 1 L0[0], L0[1], L0[0], L0[1].
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 8; ++x) {
      CAPTURE(x, y);
      const int part8 = ((x - 4) / 2) + (2 * (y / 2));
      const int want = x < 4 ? 1 : part8 % 2;
      CHECK(b.ref0[cell(x, y)] == want);
    }
  }
  CHECK(b.usage == std::vector<uint32_t>{8, 24});
  // FFmpeg's vectors: MB 0 one 16x16, MB 1 one per 8x8 (the 8x4 and 4x8 pairs collapsed, so their mvd stays n/a).
  REQUIRE(b.mvs.size() == 5);
  CHECK((b.mvs[0].ref == 1 && b.mvs[0].mvd_x == -8 && b.mvs[0].mvd_y == 12));
  CHECK((b.mvs[1].w == 8 && b.mvs[1].ref == 0 && b.mvs[1].mvd_x == 16 && b.mvs[1].mvd_y == 0));
  CHECK((b.mvs[2].ref == 1 && b.mvs[2].mvd_x == kNoMvd));
  CHECK((b.mvs[3].ref == 0 && b.mvs[3].mvd_x == kNoMvd));
  CHECK((b.mvs[4].ref == 1 && b.mvs[4].mvd_x == 24 && b.mvs[4].mvd_y == 24));
}
