// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of the side-data mapping of ovt-plugin-ffmpeg (WI-2.170, chapter 07 §7.6.3), through the plugin ABI.
// Hand-made streams have known vectors and QPs (computed from H.264 8.4.1.3 and 7.4.5 by hand). AT1, AT2 and AT4
// compare against the in-tree `ffmpeg_mvs` (ovt_plugin_ffmpeg_sidedata, chapter 14 §14.4.3a) and the committed JM
// trace goldens on the JVT streams.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "plugin_test_util.inc"
#include "test_support/corpus.h"

namespace {

using namespace ovt_ffmpeg_test;  // NOLINT(google-build-using-namespace)

struct Mv {
  int list = 0;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int mv_x = 0;
  int mv_y = 0;
  bool bi = false;
  auto operator<=>(const Mv&) const = default;
};

struct Frame {
  int decode_index = -1;
  uint32_t cells_w = 0;
  uint32_t cells_h = 0;
  std::vector<uint32_t> block_id;
  std::vector<uint8_t> pred;
  std::vector<int16_t> qp;
  std::vector<int8_t> ref0;
  std::vector<int8_t> ref1;
  std::vector<int16_t> mv0_x;
  std::vector<int16_t> mv0_y;
  std::vector<Mv> mvs;
  std::vector<uint32_t> usage;  // RefSlot.usage_cells
  std::vector<std::string> ref_names;
  std::vector<std::string> codes;  // Diagnostic codes
  std::vector<int> mb_qp;          // Block.qp of the MB blocks
  std::vector<uint8_t> mb_pred;
  bool has_hist = false;
};

template <typename T, typename V>
std::vector<T> vec(const V* v) {
  return v == nullptr ? std::vector<T>{} : std::vector<T>(v->begin(), v->end());
}

// Copies a FrameAnalysis out and checks the model rules the mapping must satisfy (chapter 06 §6.9: V-003..V-016,
// V-025, V-026, V-028) on the way.
Frame read_frame(  // NOLINT(readability-function-cognitive-complexity): one validator
    const ovt::fb::FrameAnalysis* fa) {
  Frame f;
  f.decode_index = fa->frame()->id()->decode_index();
  const ovt::fb::CellGrid* g = fa->grid();
  REQUIRE(g != nullptr);
  f.cells_w = g->cells_w();
  f.cells_h = g->cells_h();
  const size_t cells = static_cast<size_t>(f.cells_w) * f.cells_h;
  CHECK(f.cells_w == static_cast<uint32_t>((fa->frame()->coded_size()->w() + 3) / 4));  // V-003
  f.block_id = vec<uint32_t>(g->block_id());
  f.pred = vec<uint8_t>(g->pred());
  f.qp = vec<int16_t>(g->qp());
  f.ref0 = vec<int8_t>(g->ref0());
  f.ref1 = vec<int8_t>(g->ref1());
  f.mv0_x = vec<int16_t>(g->mv0_x());
  f.mv0_y = vec<int16_t>(g->mv0_y());
  const auto mv1_x = vec<int16_t>(g->mv1_x());
  const auto mv1_y = vec<int16_t>(g->mv1_y());
  for (const size_t n : {f.block_id.size(), f.pred.size(), f.qp.size(), f.ref0.size(), f.ref1.size(), f.mv0_x.size(),
                         f.mv0_y.size(), mv1_x.size(), mv1_y.size()}) {
    REQUIRE(n == cells);
  }
  CHECK(g->partition_id() == nullptr);  // n/a for this plugin

  const auto* blocks = fa->blocks();
  REQUIRE(blocks != nullptr);
  REQUIRE(blocks->Length() >= 1);
  const ovt::fb::Block* root = blocks->Get(0);  // V-004
  CHECK((root->parent() == -1 && root->kind() == ovt::fb::BlockKind::Other && root->level() == 0));
  CHECK((root->rect()->w() == fa->frame()->coded_size()->w() && root->rect()->h() == fa->frame()->coded_size()->h()));
  CHECK(root->children()->size() == blocks->size() - 1);
  for (uint32_t i = 1; i < blocks->size(); ++i) {  // V-005, V-006, V-008, V-009, V-021
    const ovt::fb::Block* b = blocks->Get(i);
    CHECK((b->id() == i && b->parent() == 0 && b->kind() == ovt::fb::BlockKind::MB && b->level() == 1));
    CHECK((b->ext_type() == nullptr || b->ext_type()->str().empty()));
    CHECK((b->rect()->w() == 16 && b->rect()->h() == 16 && b->rect()->x() % 16 == 0 && b->rect()->y() % 16 == 0));
    CHECK(root->children()->Get(i - 1) == i);
    f.mb_qp.push_back(b->qp());
    f.mb_pred.push_back(static_cast<uint8_t>(b->pred()));
  }
  size_t bad_cells = 0;                 // per-cell rules are counted, so a stream of 100k cells costs one assertion
  for (size_t c = 0; c < cells; ++c) {  // V-010, V-012, V-013
    if (f.block_id[c] < 1 || f.block_id[c] >= blocks->size()) {
      ++bad_cells;
      continue;
    }
    const ovt::fb::Rect* r = blocks->Get(f.block_id[c])->rect();
    const int x = static_cast<int>(c % f.cells_w) * 4;
    const int y = static_cast<int>(c / f.cells_w) * 4;
    const bool inside = r->x() <= x && x < r->x() + r->w() && r->y() <= y && y < r->y() + r->h();
    bad_cells += (!inside || f.pred[c] != static_cast<uint8_t>(blocks->Get(f.block_id[c])->pred())) ? 1U : 0U;
  }
  REQUIRE(bad_cells == 0);

  const auto* refs = fa->refs();
  const size_t n_refs = refs != nullptr ? refs->size() : 0;
  for (size_t k = 0; k < n_refs; ++k) {  // V-014
    CHECK(refs->Get(static_cast<uint32_t>(k))->index() == k);
    f.usage.push_back(refs->Get(static_cast<uint32_t>(k))->usage_cells());
    f.ref_names.push_back(refs->Get(static_cast<uint32_t>(k))->name()->str());
  }
  std::vector<uint32_t> counted(n_refs, 0);
  size_t bad_refs = 0;
  for (size_t c = 0; c < cells; ++c) {  // V-014, V-016, V-026
    for (const int8_t r : {f.ref0[c], f.ref1[c]}) {
      if (r >= static_cast<int>(n_refs)) {
        ++bad_refs;
      } else if (r >= 0) {
        ++counted[static_cast<size_t>(r)];
      }
    }
    const bool intra = f.pred[c] == static_cast<uint8_t>(ovt::fb::PredKind::Intra);
    const bool inter = f.pred[c] == static_cast<uint8_t>(ovt::fb::PredKind::Inter);
    bad_refs += (intra && (f.ref0[c] != -1 || f.ref1[c] != -1)) ? 1U : 0U;
    bad_refs += (inter && f.ref0[c] < 0 && f.ref1[c] < 0) ? 1U : 0U;
  }
  CHECK(bad_refs == 0);
  CHECK(counted == f.usage);

  if (fa->mvs() != nullptr) {
    std::tuple<uint32_t, int, int, int> last{0, 0, -1, -1};
    for (const ovt::fb::MotionVector* m : *fa->mvs()) {
      REQUIRE(m->block_id() < blocks->size());  // V-015
      const ovt::fb::Rect* br = blocks->Get(m->block_id())->rect();
      const ovt::fb::Rect* r = m->rect();
      const int8_t ref = m->ref();
      CHECK((m->list() <= 1 && ref == static_cast<int8_t>(m->list()) && static_cast<size_t>(ref) < n_refs));
      CHECK((r->x() >= br->x() && r->y() >= br->y() && r->x() + r->w() <= br->x() + br->w() &&
             r->y() + r->h() <= br->y() + br->h()));
      CHECK((m->mvd_x() == -32768 && m->mvd_y() == -32768));
      const std::tuple<uint32_t, int, int, int> key{m->block_id(), m->list(), r->y(), r->x()};
      CHECK(last < key);  // V-025: canonical order
      last = key;
      const auto& gx = m->list() == 0 ? f.mv0_x : mv1_x;
      const auto& gy = m->list() == 0 ? f.mv0_y : mv1_y;
      const auto& gr = m->list() == 0 ? f.ref0 : f.ref1;
      size_t differing = 0;
      for (int y = r->y(); y < r->y() + r->h(); y += 4) {
        for (int x = r->x(); x < r->x() + r->w(); x += 4) {
          const size_t c = (static_cast<size_t>(y / 4) * f.cells_w) + static_cast<size_t>(x / 4);
          differing += (gx[c] == m->mv_x() && gy[c] == m->mv_y() && gr[c] == m->ref()) ? 0U : 1U;
        }
      }
      CHECK(differing == 0);
      f.mvs.push_back(Mv{m->list(), r->x(), r->y(), r->w(), r->h(), m->mv_x(), m->mv_y(), (m->flags() & 1U) != 0});
    }
  }

  const auto hist = vec<uint32_t>(fa->qp_hist());  // V-028
  f.has_hist = !hist.empty();
  if (f.has_hist) {
    REQUIRE(hist.size() == 256);
    std::array<uint32_t, 256> want{};
    for (const int16_t q : f.qp) {
      ++want.at(static_cast<size_t>(std::clamp(q + 128, 0, 255)));
    }
    CHECK(std::equal(hist.begin(), hist.end(), want.begin()));
  }
  if (fa->diagnostics() != nullptr) {
    static const std::regex kCode(R"(^(CNT|SYN|DEC|VQ|MDL|IO|BR|VER|ANA|RND|GUI|CLI)(-[A-Z0-9]{2,8})?-[0-9]{4}$)");
    for (const ovt::fb::Diagnostic* d : *fa->diagnostics()) {
      const std::string code = d->code() != nullptr ? d->code()->str() : "";
      CHECK((code.empty() || std::regex_match(code, kCode)));  // V-029; code-less decode errors predate WI-2.170
      f.codes.push_back(code);
    }
  }
  return f;
}

std::vector<Frame> analyse(const std::vector<uint8_t>& bytes, int threads = 1) {
  std::vector<Frame> frames;
  for_each_frame(bytes, threads, [&](const ovt::fb::FrameAnalysis* fa) { frames.push_back(read_frame(fa)); });
  return frames;
}

bool has_code(const Frame& f, const std::string& code) {
  return std::find(f.codes.begin(), f.codes.end(), code) != f.codes.end();
}

void annexb(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal) {
  out->insert(out->end(), {0, 0, 0, 1});
  out->insert(out->end(), nal.begin(), nal.end());
}

// An IDR picture of two I_16x16 (DC prediction, no residual) macroblocks with mb_qp_delta d0 and d1.
std::vector<uint8_t> idr_with_qp_deltas(int d0, int d1) {
  BitWriter w;
  w.bits(0x65, 8).ue(0).ue(7).ue(0);  // IDR NAL, first_mb_in_slice, slice_type I (all), pps id
  w.bits(0, 4).ue(0);                 // frame_num, idr_pic_id
  w.bits(0, 2).se(0).ue(1);  // no_output_of_prior_pics, long_term_reference_flag, slice_qp_delta, no deblocking
  for (const int d : {d0, d1}) {
    w.ue(3).ue(0).se(d).bits(1,
                             1);  // I_16x16_2_0_0, chroma DC prediction, mb_qp_delta, Intra16x16DCLevel: 0 coefficients
  }
  w.trailing();
  return w.take();
}

struct MbSyntax {
  uint32_t mb_type;                      // P_L0_16x16 0, P_L0_L0_16x8 1, P_L0_L0_8x16 2
  std::vector<std::pair<int, int>> mvd;  // one per partition, quarter samples
};

// A P picture (nal_ref_idc 3) whose macroblocks have no residual: QP = 26 + slice_qp_delta everywhere.
std::vector<uint8_t> p_picture(int frame_num, int slice_qp_delta, const std::array<MbSyntax, 2>& mbs) {
  BitWriter w;
  w.bits(0x61, 8).ue(0).ue(5).ue(0);  // non-IDR NAL, first_mb_in_slice, slice_type P (all), pps id
  w.bits(static_cast<uint32_t>(frame_num), 4);
  w.bits(0, 2);  // num_ref_idx_active_override_flag, ref_pic_list_modification_flag_l0
  w.bits(0, 1);  // adaptive_ref_pic_marking_mode_flag
  w.se(slice_qp_delta).ue(1);
  for (const MbSyntax& mb : mbs) {
    w.ue(0).ue(mb.mb_type);  // mb_skip_run, mb_type
    for (const auto& [x, y] : mb.mvd) {
      w.se(x).se(y);
    }
    w.ue(0);  // coded_block_pattern 0 (me(v) code number 0 for inter, 4:2:0)
  }
  w.trailing();
  return w.take();
}

std::vector<uint8_t> synthetic_stream() {
  std::vector<uint8_t> s;
  annexb(&s, pcm_sps());
  annexb(&s, pcm_pps());
  annexb(&s, idr_with_qp_deltas(4, -6));
  annexb(&s, p_picture(1, 2, {MbSyntax{0, {{5, -7}}}, MbSyntax{0, {{-3, 2}}}}));
  annexb(&s, p_picture(2, -4, {MbSyntax{1, {{4, 4}, {1, -1}}}, MbSyntax{2, {{2, 0}, {0, 3}}}}));
  return s;
}

// The per-macroblock QP_Y of the first picture in a JM trace golden (tests/golden/h264/<id>.jm-trace.json.gz, the
// `normalize/jm_trace.py` block-grid): the "qp" of every {"mb_addr": …} entry up to the next mb_addr 0.
std::vector<int> jm_first_picture_qps(const std::string& golden_json) {
  static const std::regex kMb(R"re("mb_addr":\s*(\d+)[^}]*?"qp":\s*(-?\d+))re");
  std::vector<int> qps;
  for (std::sregex_iterator it(golden_json.begin(), golden_json.end(), kMb), end; it != end; ++it) {
    if (std::stoi((*it)[1]) == 0 && !qps.empty()) {
      break;
    }
    REQUIRE(static_cast<size_t>(std::stoi((*it)[1])) == qps.size());  // macroblocks in address order
    qps.push_back(std::stoi((*it)[2]));
  }
  return qps;
}

std::string gunzip(const std::filesystem::path& file) {
  FILE* p = popen((std::string("gzip -dc \"") + file.string() + "\"").c_str(), "r");  // NOLINT(cert-env33-c)
  REQUIRE(p != nullptr);
  std::string text;
  std::array<char, 65536> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    text.append(buf.data(), n);
  }
  REQUIRE(pclose(p) == 0);
  return text;
}

}  // namespace

TEST_CASE("ffmpeg plugin: the JM trace golden reader finds the first picture's QPs", "[fast][third_party]") {
  const std::vector<int> qps =
      jm_first_picture_qps(R"({"frames":[{"frame":0,"mbs":[]},{"frame":1,"mbs":[{"mb_addr":0,"mb_qp_delta":4,"qp":30},)"
                           R"({"mb_addr":1,"mb_qp_delta":-6,"qp":24}]},{"frame":2,"mbs":[{"mb_addr":0,"qp":28}]}]})");
  CHECK(qps == std::vector<int>{30, 24});
  const std::vector<Frame> frames = analyse(synthetic_stream());
  REQUIRE_FALSE(frames.empty());
  CHECK(frames[0].mb_qp == qps);  // the comparison that ffmpeg_qp_vs_jm makes on Hi422FR1_SONY_A
  const std::vector<int> golden =
      jm_first_picture_qps(gunzip(std::filesystem::path(OVT_JM_GOLDEN_DIR) / "jvt_Hi422FR1_SONY_A.jm-trace.json.gz"));
  CHECK(golden.size() == 396);  // one CIF picture (the golden lists the next picture under the same frame)
  CHECK(std::all_of(golden.begin(), golden.end(), [](int q) { return q >= -12 && q <= 51; }));
}

TEST_CASE("ffmpeg plugin: intra macroblocks map to Intra with their QP", "[fast][third_party]") {
  // AT3 and the QP part of AT2 on a hand-made IDR: QP_Y 26 + 4 = 30 for MB 0, 30 - 6 = 24 for MB 1.
  const int threads = GENERATE(1, 4);
  const std::vector<Frame> frames = analyse(synthetic_stream(), threads);
  REQUIRE(frames.size() == 3);
  const Frame& f = frames[0];
  CHECK(f.decode_index == 0);
  REQUIRE(f.cells_w * f.cells_h == 32);
  CHECK(f.mb_pred == std::vector<uint8_t>{1, 1});
  CHECK(f.mb_qp == std::vector<int>{30, 24});
  for (size_t c = 0; c < 32; ++c) {
    const bool left = (c % 8) < 4;
    CHECK(f.pred[c] == 1);  // PredKind::Intra
    CHECK(f.qp[c] == (left ? 30 : 24));
    CHECK(f.block_id[c] == (left ? 1U : 2U));
    CHECK((f.ref0[c] == -1 && f.ref1[c] == -1));
  }
  CHECK(f.mvs.empty());
  CHECK(f.ref_names.empty());
  CHECK(has_code(f, "ANA-H264-0002"));
  CHECK_FALSE(has_code(f, "ANA-H264-0001"));
  REQUIRE(f.has_hist);
}

TEST_CASE("ffmpeg plugin: P macroblocks map to Inter with the vectors of the stream", "[fast][third_party]") {
  // Quarter-sample vectors from H.264 8.4.1.3 (see synthetic_stream()), scaled to 1/16: multiply by 4.
  const int threads = GENERATE(1, 4);
  const std::vector<Frame> frames = analyse(synthetic_stream(), threads);
  REQUIRE(frames.size() == 3);

  const Frame& a = frames[1];  // 16x16 partitions: mv(MB 0) = (5,-7), mv(MB 1) = (5,-7) + (-3,2) = (2,-5)
  CHECK(a.decode_index == 1);
  CHECK(a.mvs == std::vector<Mv>{{0, 0, 0, 16, 16, 20, -28, false}, {0, 16, 0, 16, 16, 8, -20, false}});
  CHECK(a.mb_pred == std::vector<uint8_t>{2, 2});
  CHECK(a.mb_qp == std::vector<int>{28, 28});  // slice QP 26 + 2; macroblocks without residual keep it
  CHECK(a.ref_names == std::vector<std::string>{"L0", "L1"});
  CHECK(a.usage == std::vector<uint32_t>{32, 0});
  CHECK_FALSE(has_code(a, "ANA-H264-0002"));  // no intra macroblock, nothing inferred
  for (size_t c = 0; c < 32; ++c) {
    CHECK((a.ref0[c] == 0 && a.ref1[c] == -1 && a.pred[c] == 2));
  }

  const Frame& b = frames[2];                                         // 16x8 partitions of MB 0, 8x16 of MB 1
  CHECK(b.mvs == std::vector<Mv>{{0, 0, 0, 16, 8, 16, 16, false},     // (4,4)
                                 {0, 0, 8, 16, 8, 20, 12, false},     // (4,4) + (1,-1) = (5,3)
                                 {0, 16, 0, 8, 16, 24, 16, false},    // A = (4,4); + (2,0) = (6,4)
                                 {0, 24, 0, 8, 16, 24, 28, false}});  // median = A = (6,4); + (0,3) = (6,7)
  CHECK(b.mb_qp == std::vector<int>{22, 22});
}

namespace {

// AT1 and AT2 for one stream against the in-tree ffmpeg_mvs: every vector and every macroblock QP after the conversion
// of tests/oracles/normalize/ffmpeg_mvs.py, at 1 and 4 threads.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void compare_with_oracle(const std::string& id) {
  CAPTURE(id);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::string cmd = std::string("\"") + OVT_FFMPEG_SIDEDATA + "\" \"" + path.string() + "\"";
  FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): the oracle executable of this build
  REQUIRE(p != nullptr);
  std::string text;
  std::array<char, 65536> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    text.append(buf.data(), n);
  }
  REQUIRE(pclose(p) == 0);

  // Rows -> per packet index: the vectors after the conversion of tests/oracles/normalize/ffmpeg_mvs.py (rect from the
  // centre, mv * 16 / motion_scale, list from the sign), and the QP of every 16x16 block.
  std::map<int, std::vector<Mv>> want_mvs;
  std::map<int, std::map<std::pair<int, int>, int>> want_qp;  // (x, y) -> qp + delta_qp
  std::istringstream lines(text);
  std::string kind;
  while (lines >> kind) {
    std::array<int, 12> v{};  // M: pts source w h src_x src_y dst_x dst_y flags motion_x motion_y scale
    const size_t n_fields = kind == "M" ? v.size() : 7;  // Q: pts x y w h qp delta_qp
    for (size_t k = 0; k < n_fields; ++k) {
      lines >> v.at(k);
    }
    if (kind == "M") {
      const int w = v[2];
      const int h = v[3];
      want_mvs[v[0]].push_back(
          Mv{v[1] < 0 ? 0 : 1, v[6] - (w / 2), v[7] - (h / 2), w, h, v[9] * 16 / v[11], v[10] * 16 / v[11], false});
    } else {
      want_qp[v[0]][{v[1], v[2]}] = v[5] + v[6];
    }
  }
  for (auto& [pts, v] : want_mvs) {
    // both lists for the same rect make bi-prediction, which the oracle rows only imply
    for (Mv& a : v) {
      a.bi = std::any_of(v.begin(), v.end(), [&](const Mv& b) {
        return b.list != a.list && b.x == a.x && b.y == a.y && b.w == a.w && b.h == a.h;
      });
    }
    std::sort(v.begin(), v.end(),
              [](const Mv& a, const Mv& b) { return std::tie(a.y, a.x, a.list) < std::tie(b.y, b.x, b.list); });
  }

  int frames_checked = 0;
  int total_mvs = 0;
  for (const int threads : {1, 4}) {
    for_each_frame(read_file(path), threads, [&](const ovt::fb::FrameAnalysis* fa) {
      const Frame f = read_frame(fa);
      ++frames_checked;
      CAPTURE(f.decode_index);
      // The plugin's order is (macroblock, list, y, x); the oracle's rows are compared as sets per frame.
      std::vector<Mv> got = f.mvs;
      const auto it = want_mvs.find(f.decode_index);
      std::vector<Mv> want = it != want_mvs.end() ? it->second : std::vector<Mv>{};
      const auto key = [](const Mv& a, const Mv& b) { return std::tie(a.y, a.x, a.list) < std::tie(b.y, b.x, b.list); };
      std::sort(got.begin(), got.end(), key);
      std::sort(want.begin(), want.end(), key);
      CHECK(got == want);
      total_mvs += static_cast<int>(got.size());
      if (const auto q = want_qp.find(f.decode_index); q != want_qp.end()) {
        const int depth = fa->frame()->bit_depth_luma();
        for (const auto& [xy, qp_plus_delta] : q->second) {
          const size_t cell = (static_cast<size_t>(xy.second / 4) * f.cells_w) + static_cast<size_t>(xy.first / 4);
          CHECK(f.qp[cell] == qp_plus_delta - (6 * (depth - 8)));
        }
      }
    });
  }
  CHECK(frames_checked > 0);
  if (id == "jvt_CVWP1_TOSHIBA_E") {
    CHECK(total_mvs > 0);  // a weighted-P stream has inter macroblocks
  }
}
}  // namespace

TEST_CASE("ffmpeg plugin: ffmpeg_mv_vs_oracle", "[fast][third_party][needs-corpus]") {
  // CVWP1_TOSHIBA_E: weighted P (AT1); CABA1_SVA_B: B pictures, so bi-prediction.
  compare_with_oracle(GENERATE(as<std::string>{}, "jvt_CVWP1_TOSHIBA_E", "jvt_CABA1_SVA_B"));
}

TEST_CASE("ffmpeg plugin: ffmpeg_mv_vs_oracle on the slow JVT streams", "[slow][third_party][needs-corpus]") {
  // CAQP1_Sony_B: QP changes (AT2); CAMA1_Sony_C: MBAFF.
  compare_with_oracle(GENERATE(as<std::string>{}, "jvt_CAQP1_Sony_B", "jvt_CAMA1_Sony_C"));
}

TEST_CASE("ffmpeg plugin: ffmpeg_qp_vs_jm", "[fast][third_party][needs-corpus]") {
  // AT4: 10-bit 4:2:2 intra stream. Every grid.qp lies in -12..51 and the first picture's QPs are those JM traced
  // (SliceQPY plus the mb_qp_delta accumulation, QpBdOffsetY already removed).
  const std::string id = "jvt_Hi422FR1_SONY_A";
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::vector<int> jm =
      jm_first_picture_qps(gunzip(std::filesystem::path(OVT_JM_GOLDEN_DIR) / (id + ".jm-trace.json.gz")));
  REQUIRE_FALSE(jm.empty());
  const std::vector<Frame> frames = analyse(read_file(path), 2);
  REQUIRE_FALSE(frames.empty());
  for (const Frame& f : frames) {
    CAPTURE(f.decode_index);
    CHECK(std::all_of(f.qp.begin(), f.qp.end(), [](int16_t q) { return q >= -12 && q <= 51; }));
  }
  const auto first = std::find_if(frames.begin(), frames.end(), [](const Frame& f) { return f.decode_index == 0; });
  REQUIRE(first != frames.end());
  CHECK(first->mb_qp == jm);
}