// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.583: ovt-plugin-ffmpeg (H.264, public-API FFmpeg) against the committed oracle goldens of chapter 14 §14.4.3 and
// §14.4.3a, through the plugin ABI:
//   ffmpeg_mv_vs_golden  every exported motion vector (>= 8x8) equals tests/golden/h264/<id>.ffmpeg-mvs.json.gz
//                        (rows of the `ffmpeg_mvs` oracle, already converted to 1/16 pel and a top-left rectangle);
//   ffmpeg_qp_vs_jm      the final QP_Y of every macroblock JM traced equals the plugin's Block.qp. QP_Y is rebuilt
//                        here as SliceQPY (trace_headers golden) plus the accumulated mb_qp_delta of the jm-trace
//                        golden (7.4.5, step 2 of the WI), not read from the golden's `qp`, which is wrong for 6 of the
//                        committed streams (SPEC-ASSUMPTION(#431)).
// A golden covers the first pictures of its stream (tests/golden/h26x-index.json, tools/goldens/gen_h26x.py cuts the
// stream before the access unit of picture N + 1), so the plugin analyses the same prefix, which also gives the
// oracle's output order. Inputs come from the corpus cache; tests skip without it, and the accumulation check needs no
// corpus.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "h264_golden.inc"
#include "plugin_test_util.inc"
#include "test_support/corpus.h"

namespace {

using namespace ovt_ffmpeg_test;  // NOLINT(google-build-using-namespace)

// ---- the stream prefix a golden covers (the same cut as tools/goldens/gen_h26x.py `truncate`) ---------------------

struct NalPos {
  size_t begin;  // first byte of the start code (the zero of a 4-byte start code included)
  size_t nal;    // first byte of the NAL unit
};

std::vector<NalPos> nal_positions(const std::vector<uint8_t>& s) {
  std::vector<NalPos> out;
  for (size_t i = 0; i + 3 <= s.size(); ++i) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      out.push_back(NalPos{i > 0 && s[i - 1] == 0 ? i - 1 : i, i + 3});
      i += 2;
    }
  }
  return out;
}

// The stream up to (not including) the access unit of picture `pictures + 1`; pictures start at a slice with
// first_mb_in_slice == 0, an access unit also owns the SEI/SPS/PPS/AUD (types 6-9, 14, 15) in front of its first slice.
std::vector<uint8_t> prefix_of_pictures(const std::vector<uint8_t>& s, int pictures) {
  const std::vector<NalPos> nals = nal_positions(s);
  int count = 0;
  for (size_t i = 0; i < nals.size(); ++i) {
    const size_t at = nals[i].nal;
    const int type = at < s.size() ? s[at] & 0x1F : 0;
    if (type < 1 || type > 5 || at + 1 >= s.size() || (s[at + 1] & 0x80) == 0) {
      continue;  // not the first slice of a picture
    }
    if (++count == pictures + 1) {
      size_t j = i;
      const auto prefix_type = [&](size_t k) { return s[nals[k].nal] & 0x1F; };
      while (j > 0 && ((prefix_type(j - 1) >= 6 && prefix_type(j - 1) <= 9) || prefix_type(j - 1) == 14 ||
                       prefix_type(j - 1) == 15)) {
        --j;
      }
      return {s.begin(), s.begin() + static_cast<std::ptrdiff_t>(nals[j].begin)};
    }
  }
  return s;
}

// The number of pictures golden `which` ("jm", "mvs") of `id` covers, from tests/golden/h26x-index.json.
int golden_pictures(const std::string& id, const std::string& which) {
  static const json index = json::parse(read_pipe(std::string("cat \"") + OVT_JM_GOLDEN_DIR + "/../h26x-index.json\""));
  return index.at("entries").at(id).at("goldens").at(which).at("pictures").get<int>();
}

// ---- what the plugin reports -------------------------------------------------------------------------------------

struct Mv {
  int list = 0;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int mv_x = 0;
  int mv_y = 0;
  auto operator<=>(const Mv&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Mv& m) {  // for Catch2's failure output
  return os << "L" << m.list << " (" << m.x << "," << m.y << ") " << m.w << "x" << m.h << " mv " << m.mv_x << ","
            << m.mv_y;
}

struct PluginFrame {
  int decode_index = -1;
  int mb_w = 0;
  int mb_h = 0;
  std::vector<int> mb_qp;  // Block.qp of the macroblock at raster position (y * mb_w + x)
  std::vector<Mv> mvs;     // sorted by (y, x, list), >= 8x8 only
};

// Frames in pull() order, which is FFmpeg's output order.
std::vector<PluginFrame> analyse(const std::vector<uint8_t>& stream, int threads) {
  std::vector<PluginFrame> frames;
  for_each_frame(stream, threads, [&](const ovt::fb::FrameAnalysis* fa) {
    PluginFrame f;
    f.decode_index = fa->frame()->id()->decode_index();
    f.mb_w = (fa->frame()->coded_size()->w() + 15) / 16;
    f.mb_h = (fa->frame()->coded_size()->h() + 15) / 16;
    f.mb_qp.assign(static_cast<size_t>(f.mb_w) * static_cast<size_t>(f.mb_h), -1000);
    const auto* blocks = fa->blocks();
    REQUIRE(blocks != nullptr);
    for (uint32_t i = 1; i < blocks->size(); ++i) {
      const ovt::fb::Block* b = blocks->Get(i);
      const auto at = (static_cast<size_t>(b->rect()->y() / 16) * static_cast<size_t>(f.mb_w)) +
                      static_cast<size_t>(b->rect()->x() / 16);
      REQUIRE(at < f.mb_qp.size());
      f.mb_qp[at] = b->qp();
    }
    for (uint32_t i = 0; fa->mvs() != nullptr && i < fa->mvs()->size(); ++i) {
      const ovt::fb::MotionVector* m = fa->mvs()->Get(i);
      const ovt::fb::Rect* r = m->rect();
      if (r->w() >= 8 && r->h() >= 8) {
        f.mvs.push_back(Mv{m->list(), r->x(), r->y(), r->w(), r->h(), m->mv_x(), m->mv_y()});
      }
    }
    std::sort(f.mvs.begin(), f.mvs.end(),
              [](const Mv& a, const Mv& b) { return std::tie(a.y, a.x, a.list) < std::tie(b.y, b.x, b.list); });
    frames.push_back(std::move(f));
  });
  return frames;
}

// ---- AT1: motion vectors -----------------------------------------------------------------------------------------

// The golden's frames (output order, only frames with vectors are listed) as sorted >= 8x8 vectors.
std::map<size_t, std::vector<Mv>> golden_mvs(const json& golden) {
  std::map<size_t, std::vector<Mv>> out;
  for (const json& frame : golden.at("frames")) {
    std::vector<Mv>& v = out[frame.at("frame").get<size_t>()];
    for (const json& m : frame.at("mvs")) {
      if (m.at("w").get<int>() >= 8 && m.at("h").get<int>() >= 8) {
        v.push_back(Mv{m.at("list").get<int>(), m.at("x").get<int>(), m.at("y").get<int>(), m.at("w").get<int>(),
                       m.at("h").get<int>(), m.at("mv_x").get<int>(), m.at("mv_y").get<int>()});
      }
    }
    std::sort(v.begin(), v.end(),
              [](const Mv& a, const Mv& b) { return std::tie(a.y, a.x, a.list) < std::tie(b.y, b.x, b.list); });
  }
  return out;
}

// The vectors of `frames` against `want`; returns the number of vectors compared. A frame the golden does not list has
// none (the oracle writes no row for an intra frame).
size_t check_mvs(const std::vector<PluginFrame>& frames, const std::map<size_t, std::vector<Mv>>& want) {
  REQUIRE_FALSE(frames.empty());
  CHECK((want.empty() || want.rbegin()->first < frames.size()));
  size_t compared = 0;
  for (size_t g = 0; g < frames.size(); ++g) {
    CAPTURE(g);
    const auto it = want.find(g);
    const std::vector<Mv> empty;
    const std::vector<Mv>& expected = it != want.end() ? it->second : empty;
    CHECK(frames[g].mvs == expected);
    compared += expected.size();
  }
  return compared;
}

size_t mvs_vs_golden(const std::string& id) {
  CAPTURE(id);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::map<size_t, std::vector<Mv>> want = golden_mvs(read_golden(id + ".ffmpeg-mvs.json.gz"));
  const std::vector<uint8_t> stream = prefix_of_pictures(read_file(path), golden_pictures(id, "mvs"));
  size_t compared = 0;
  for (const int threads : {1, 4}) {
    compared = check_mvs(analyse(stream, threads), want);
  }
  return compared;
}

// ---- AT2: QP -----------------------------------------------------------------------------------------------------

// QP_Y of each macroblock JM traced, rebuilt from SliceQPY and mb_qp_delta (H.264 7.4.5): QP_Y = ((QP_Y,PRED +
// mb_qp_delta + 52 + 2 * QpBdOffsetY) % (52 + QpBdOffsetY)) - QpBdOffsetY, where QP_Y,PRED is the QP_Y of the previous
// macroblock of the slice (SliceQPY for the first). JM does not trace skipped macroblocks, which keep QP_Y,PRED, and a
// macroblock without mb_qp_delta (I_PCM) keeps it too.
std::vector<int> accumulated_qps(const Picture& pic, const std::vector<json>& mbs) {
  std::vector<int> out;
  const Slice* slice = nullptr;
  int pred = 0;
  for (const json& mb : mbs) {
    const Slice* s = pic.slice_of(mb.at("mb_addr").get<int>());
    REQUIRE(s != nullptr);
    if (s != slice) {
      slice = s;
      pred = s->qp;
    }
    if (mb.contains("mb_qp_delta")) {
      const int d = mb.at("mb_qp_delta").get<int>();
      pred = ((pred + d + 52 + (2 * s->bd_offset)) % (52 + s->bd_offset)) - s->bd_offset;
    }
    out.push_back(pred);
  }
  return out;
}

struct QpCounts {
  size_t compared = 0;
  size_t pcm = 0;
};

// The JM pictures of `id` against the plugin's frames in decoding order (a field pair is one frame).
QpCounts qp_vs_jm(const std::string& id, const std::vector<PluginFrame>& frames) {
  const std::vector<Picture> pictures = header_pictures(read_golden(id + ".trace_headers.json.gz"));
  const std::vector<std::vector<json>> jm = jm_pictures(read_golden(id + ".jm-trace.json.gz"));
  REQUIRE_FALSE(jm.empty());
  REQUIRE(pictures.size() >= jm.size());
  QpCounts c;
  std::vector<std::string> bad;
  size_t frame = 0;
  int open_field = -1;  // parity of a first field whose second field has not come yet
  for (size_t k = 0; k < jm.size(); ++k) {
    const Picture& pic = pictures[k];
    const int parity = pic.bottom ? 1 : 0;
    size_t this_frame = frame;
    if (pic.field && open_field >= 0 && open_field != parity) {
      this_frame = frame - 1;  // the second field of the frame the first field opened
      open_field = -1;
    } else {
      open_field = pic.field ? parity : -1;
      ++frame;
    }
    REQUIRE(this_frame < frames.size());
    const PluginFrame& f = frames[this_frame];
    const std::vector<int> want = accumulated_qps(pic, jm[k]);
    for (size_t i = 0; i < jm[k].size(); ++i) {
      const json& mb = jm[k][i];
      const int addr = mb.at("mb_addr").get<int>();
      const auto [x, y] = mb_position(pic, addr, f.mb_w);
      REQUIRE((x < f.mb_w && y < f.mb_h));
      const int got = f.mb_qp[(static_cast<size_t>(y) * static_cast<size_t>(f.mb_w)) + static_cast<size_t>(x)];
      ++c.compared;
      // SPEC-ASSUMPTION(#433): FFmpeg's QP table holds 0 for an I_PCM macroblock (the value deblocking uses, 8.7.2.2),
      // while QP_Y of the macroblock is the carried QP_Y,PRED (7.4.5); either is accepted for I_PCM.
      const bool pcm = mb.value("pcm", 0) != 0;
      c.pcm += pcm ? 1U : 0U;
      if (got != want[i] && (!pcm || got != 0) && bad.size() < 10) {
        bad.push_back("picture " + std::to_string(k) + " mb " + std::to_string(addr) + ": plugin " +
                      std::to_string(got) + ", SliceQPY + mb_qp_delta chain " + std::to_string(want[i]));
      }
    }
  }
  for (const std::string& b : bad) {
    UNSCOPED_INFO(b);
  }
  CHECK(bad.empty());
  return c;
}

void qp_vs_golden(const std::string& id) {
  CAPTURE(id);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::vector<uint8_t> stream = prefix_of_pictures(read_file(path), golden_pictures(id, "jm"));
  std::vector<PluginFrame> frames = analyse(stream, 2);
  std::stable_sort(frames.begin(), frames.end(),
                   [](const PluginFrame& a, const PluginFrame& b) { return a.decode_index < b.decode_index; });
  const QpCounts c = qp_vs_jm(id, frames);
  CHECK(c.compared > 0);
  if (id == "jvt_CVPCMNL1_SVA_C") {
    CHECK(c.pcm > 0);  // the stream has I_PCM macroblocks, so the exception above was exercised
  }
}

}  // namespace

TEST_CASE("ffmpeg plugin: the stream prefix of a golden ends before the next picture's access unit",
          "[fast][third_party]") {
  std::vector<uint8_t> s;
  const auto add = [&s](const std::vector<uint8_t>& nal) {
    s.insert(s.end(), {0, 0, 0, 1});
    s.insert(s.end(), nal.begin(), nal.end());
  };
  add(pcm_sps());
  add(pcm_pps());
  add(pcm_idr(0));
  const size_t after_first = s.size();
  add({0x09, 0xF0});  // AUD: belongs to the next access unit
  add(pcm_idr(1));
  const size_t after_second = s.size();
  add(pcm_idr(2));
  CHECK(prefix_of_pictures(s, 1).size() == after_first);
  CHECK(prefix_of_pictures(s, 2).size() == after_second);
  CHECK(prefix_of_pictures(s, 3) == s);  // the whole stream when it has no picture 4
  CHECK(analyse(prefix_of_pictures(s, 2), 1).size() == 2);
}

TEST_CASE("ffmpeg plugin: JM goldens rebuild their QP_Y from SliceQPY and mb_qp_delta", "[fast][third_party]") {
  Picture pic;
  pic.slices = {Slice{0, 2, 30, 0}, Slice{4, 0, 20, 0}};
  const auto mb = [](int addr, int qp_delta) { return json{{"mb_addr", addr}, {"mb_qp_delta", qp_delta}}; };
  // Second slice restarts at its own SliceQPY; the macroblock without mb_qp_delta (PCM) and addresses JM skipped keep
  // it.
  const std::vector<json> mbs = {mb(0, 2), mb(1, -4), json{{"mb_addr", 2}, {"pcm", 1}}, mb(5, 3), mb(7, -26)};
  CHECK(accumulated_qps(pic, mbs) == std::vector<int>{32, 28, 28, 23, 49});
  pic.slices = {Slice{0, 2, 51, 12}};  // 10-bit: QP_Y spans -12..51 and wraps modulo 64
  CHECK(accumulated_qps(pic, {mb(0, 5), mb(1, -30)}) == std::vector<int>{-8, 26});
  CHECK(accumulated_qps(pic, {mb(0, -26), mb(1, -26), mb(2, -26)}) == std::vector<int>{25, -1, 37});
}

TEST_CASE("ffmpeg plugin: QP_Y rebuilt from trace_headers and JM's mb_qp_delta stays in range on every golden",
          "[fast][third_party]") {
  // The reference of ffmpeg_qp_vs_jm needs no corpus: SliceQPY of every slice header plus the signalled deltas must
  // give QP_Y within -QpBdOffsetY..51 for all macroblocks of all committed JM goldens, and the pictures' first slices
  // must be found (a golden read from the wrong stream, or a wrong SliceQPY, leaves the range quickly).
  size_t checked = 0;
  std::set<std::string> ids;
  const std::string suffix = ".jm-trace.json.gz";
  for (const auto& entry : std::filesystem::directory_iterator(OVT_JM_GOLDEN_DIR)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > suffix.size() && name.ends_with(suffix)) {
      ids.insert(name.substr(0, name.size() - suffix.size()));
    }
  }
  REQUIRE(ids.size() >= 20);
  for (const std::string& id : ids) {
    CAPTURE(id);
    const std::vector<Picture> pictures = header_pictures(read_golden(id + ".trace_headers.json.gz"));
    const std::vector<std::vector<json>> jm = jm_pictures(read_golden(id + ".jm-trace.json.gz"));
    REQUIRE(pictures.size() >= jm.size());
    for (size_t k = 0; k < jm.size(); ++k) {
      const std::vector<int> qps = accumulated_qps(pictures[k], jm[k]);
      REQUIRE(qps.size() == jm[k].size());
      const int offset = pictures[k].slices.front().bd_offset;
      CHECK(std::all_of(qps.begin(), qps.end(), [offset](int q) { return q >= -offset && q <= 51; }));
      checked += qps.size();
    }
  }
  CHECK(checked > 1000);
}

TEST_CASE("ffmpeg plugin: ffmpeg_mv_vs_golden", "[fast][third_party][needs-corpus]") {
  // AT1 on the fast JVT streams that have an ffmpeg-mvs golden. Streams whose golden pictures are all intra have an
  // empty golden: the plugin must report no vector there either.
  const std::string id = GENERATE(as<std::string>{}, "jvt_BA1_Sony_D", "jvt_BA_MW_D", "jvt_CABA1_SVA_B", "jvt_CI1_FT_B",
                                  "jvt_CVPCMNL1_SVA_C", "jvt_CVSE2_Sony_B", "jvt_CVSE3_Sony_H", "jvt_CVWP1_TOSHIBA_E",
                                  "jvt_FRExt1_Panasonic_D", "jvt_HCAFF1_HHI_B", "jvt_Hi422FR1_SONY_A", "jvt_MIDR_MW_D",
                                  "jvt_MR1_BT_A", "jvt_MR6_BT_B", "jvt_MR9_BT_B", "jvt_NRF_MW_E", "jvt_aud_mw_e");
  const size_t compared = mvs_vs_golden(id);
  const json golden = read_golden(id + ".ffmpeg-mvs.json.gz");
  if (!golden.at("frames").empty()) {
    CHECK(compared > 0);  // the golden has vectors, so the comparison saw some
  }
}

TEST_CASE("ffmpeg plugin: ffmpeg_qp_vs_jm on the fast JVT streams", "[fast][third_party][needs-corpus]") {
  qp_vs_golden(GENERATE(as<std::string>{}, "jvt_BA1_Sony_D", "jvt_BA_MW_D", "jvt_CABA1_SVA_B", "jvt_CI1_FT_B",
                        "jvt_CVPCMNL1_SVA_C", "jvt_CVSE2_Sony_B", "jvt_CVSE3_Sony_H", "jvt_CVWP1_TOSHIBA_E",
                        "jvt_FRExt1_Panasonic_D", "jvt_HCAFF1_HHI_B", "jvt_Hi422FR1_SONY_A", "jvt_MIDR_MW_D",
                        "jvt_MR1_BT_A", "jvt_MR6_BT_B", "jvt_MR9_BT_B", "jvt_NRF_MW_E", "jvt_aud_mw_e"));
}

TEST_CASE("ffmpeg plugin: ffmpeg_qp_vs_jm on the MBAFF streams", "[slow][third_party][needs-corpus]") {
  qp_vs_golden(GENERATE(as<std::string>{}, "jvt_CAMA1_Sony_C", "jvt_CAPAMA3_Sand_F"));
}
