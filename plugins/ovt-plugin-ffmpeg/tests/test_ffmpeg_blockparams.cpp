// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.101: the AV_FRAME_DATA_VIDEO_BLOCK_PARAMS export of the patched FFmpeg h264 decoder against the JM trace
// goldens (tests/golden/h264/<id>.jm-trace.json.gz, chapter 14 §14.4.3), through ovt_plugin_ffmpeg_blockparams.
//
// JM traces the syntax as signalled: the mb_type / sub_mb_type numbers of the slice type, intra4x4_pred_mode as
// -1 (prev_intra4x4_pred_mode_flag) or rem_intra4x4_pred_mode, mvd_l0 in read order. The export carries the
// decoded values (class, partition directions, Intra4x4PredMode, mvd per 4x4 block), so each is turned back into
// the signalled form here. The slice types, field parity, MBAFF and constrained_intra_pred_flag come from the
// trace_headers golden of the same stream.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "test_support/corpus.h"

namespace {

using nlohmann::json;

struct Mb {
  int x = 0;
  int y = 0;
  int cls = 255;
  int slice = 0;
  std::array<int, 4> sub{};
  std::array<int, 4> dir{};
  std::array<std::array<int, 4>, 2> ref{};
  std::array<int, 16> i4{};
  std::array<int, 4> i8{};
  int i16 = -1;
  int chroma = -1;
  int cbp = 0;
  int t8 = 0;
  int field = 0;
  int qpd = 0;
  std::array<std::array<std::array<int, 2>, 16>, 2> mvd{};
};

struct Frame {
  int64_t idx = 0;
  int w = 0;
  int h = 0;
  bool tff = false;
  std::vector<Mb> mbs;  // raster order

  [[nodiscard]] const Mb* at(int x, int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) {
      return nullptr;
    }
    return &mbs.at((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x));
  }
};

std::string read_pipe(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): gzip or the oracle executable of this build
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

json read_golden(const std::string& name) {
  const std::filesystem::path file = std::filesystem::path(OVT_JM_GOLDEN_DIR) / name;
  return json::parse(read_pipe("gzip -dc \"" + file.string() + "\""));
}

template <size_t N>
void read_into(std::istringstream& in, std::array<int, N>& out) {
  for (int& v : out) {
    in >> v;
  }
}

// The oracle's lines, frames sorted by the index of the packet that started them (decoding order).
std::vector<Frame> parse_dump(const std::string& text) {
  std::vector<Frame> frames;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind == "F") {
      Frame f;
      int tff = 0;
      in >> f.idx >> f.w >> f.h >> tff;
      f.tff = tff != 0;
      frames.push_back(f);
    } else if (kind == "B") {
      REQUIRE_FALSE(frames.empty());
      Mb m;
      in >> m.x >> m.y >> m.cls >> m.slice;
      read_into(in, m.sub);
      read_into(in, m.dir);
      read_into(in, m.ref[0]);
      read_into(in, m.ref[1]);
      read_into(in, m.i4);
      read_into(in, m.i8);
      in >> m.i16 >> m.chroma >> m.cbp >> m.t8 >> m.field >> m.qpd;
      for (auto& list : m.mvd) {
        for (auto& v : list) {
          read_into(in, v);
        }
      }
      REQUIRE_FALSE(in.fail());
      m.x /= 16;
      m.y /= 16;
      frames.back().mbs.push_back(m);
    }
  }
  std::sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.idx < b.idx; });
  return frames;
}

// ---- the stream structure, from the trace_headers golden -------------------------------------------------------

struct Slice {
  int first_mb = 0;  // in macroblocks (pairs already doubled for MBAFF)
  int type = 0;      // slice_type % 5: 0 P, 1 B, 2 I, 3 SP, 4 SI
};

struct Picture {
  bool field = false;
  bool bottom = false;
  bool mbaff = false;
  bool constrained_intra = false;
  bool cabac = false;
  std::vector<Slice> slices;

  [[nodiscard]] int slice_type(int addr) const {
    int type = -1;
    for (const Slice& s : slices) {
      if (s.first_mb <= addr) {
        type = s.type;
      }
    }
    return type;
  }
};

// Pictures in decoding order; a picture starts at first_mb_in_slice = 0 (no ASO in the golden streams).
std::vector<Picture> header_pictures(const json& elements) {
  std::vector<std::map<std::string, int>> units;
  int64_t unit = -1;
  for (const json& e : elements) {
    if (e.at("unit").get<int64_t>() != unit) {
      unit = e.at("unit").get<int64_t>();
      units.emplace_back();
    }
    units.back().emplace(e.at("name").get<std::string>(), e.at("value").get<int>());  // first occurrence wins
  }
  const auto get = [](const std::map<std::string, int>& u, const std::string& k) {
    const auto it = u.find(k);
    return it != u.end() ? it->second : 0;
  };
  std::vector<Picture> pictures;
  int sps_mbaff = 0;
  std::map<int, bool> pps_constrained;
  std::map<int, bool> pps_cabac;
  for (const auto& u : units) {
    const int type = get(u, "nal_unit_type");
    if (type == 7) {
      sps_mbaff = get(u, "mb_adaptive_frame_field_flag");
    } else if (type == 8) {
      pps_constrained[get(u, "pic_parameter_set_id")] = get(u, "constrained_intra_pred_flag") != 0;
      pps_cabac[get(u, "pic_parameter_set_id")] = get(u, "entropy_coding_mode_flag") != 0;
    } else if (type == 1 || type == 5) {
      const bool field = get(u, "field_pic_flag") != 0;
      const bool mbaff = sps_mbaff != 0 && !field;
      const int first = get(u, "first_mb_in_slice") * (mbaff ? 2 : 1);
      if (first == 0 || pictures.empty()) {
        Picture p;
        p.field = field;
        p.bottom = get(u, "bottom_field_flag") != 0;
        p.mbaff = mbaff;
        p.constrained_intra = pps_constrained[get(u, "pic_parameter_set_id")];
        p.cabac = pps_cabac[get(u, "pic_parameter_set_id")];
        pictures.push_back(p);
      }
      pictures.back().slices.push_back(Slice{first, get(u, "slice_type") % 5});
    }
  }
  return pictures;
}

// ---- the export turned back into JM's signalled values ----------------------------------------------------------

constexpr int kSliceP = 0;
constexpr int kSliceB = 1;
constexpr int kSliceI = 2;
constexpr int kSliceSP = 3;

// mb_type of a B macroblock (Table 7-14) from its class and partition directions.
json b_mb_type(const Mb& m) {
  // B_{L0,L1,Bi}_{L0,L1,Bi}_16x8 by the directions of the two partitions; the 8x16 type is one more.
  static const std::map<std::pair<int, int>, int> kPair = {{{1, 1}, 4},  {{2, 2}, 6},  {{1, 2}, 8},
                                                           {{2, 1}, 10}, {{1, 3}, 12}, {{2, 3}, 14},
                                                           {{3, 1}, 16}, {{3, 2}, 18}, {{3, 3}, 20}};
  switch (m.cls) {
    case 8:
      return 0;
    case 9:
      return m.dir[0];  // B_L0_16x16 1, B_L1_16x16 2, B_Bi_16x16 3
    case 10:
    case 11: {
      const auto it = kPair.find({m.dir[0], m.cls == 10 ? m.dir[2] : m.dir[1]});
      return it != kPair.end() ? json(it->second + (m.cls == 11 ? 1 : 0)) : json("bad partition directions");
    }
    case 12:
      return 22;
    default:
      return "B_Skip";
  }
}

// mb_type as JM traces it: the syntax value of the slice type, or "P_Skip" / "B_Skip"; a string naming the class
// when the class cannot occur in that slice type.
json expected_mb_type(const Mb& m, int slice_type) {
  const bool p = slice_type == kSliceP || slice_type == kSliceSP;
  const bool b = slice_type == kSliceB;
  if (m.cls <= 2) {
    const int offset = p ? 5 : b ? 23 : 0;
    const int i16 = 1 + m.i16 + (4 * (m.cbp >> 4)) + ((m.cbp & 15) != 0 ? 12 : 0);
    return offset + (m.cls == 0 ? 0 : m.cls == 2 ? 25 : i16);
  }
  if (p && m.cls <= 7) {
    return m.cls == 7 ? json("P_Skip") : json(m.cls - 3);  // P_8x8 = 3; P_8x8ref0 (4) is checked by the caller
  }
  if (b && m.cls >= 8 && m.cls <= 13) {
    return b_mb_type(m);
  }
  return "class " + std::to_string(m.cls) + " in slice type " + std::to_string(slice_type);
}

// sub_mb_type as signalled, for P_8x8 and B_8x8.
std::vector<int> expected_sub_mb_types(const Mb& m) {
  std::vector<int> out;
  for (size_t i = 0; i < 4; ++i) {
    const int shape = m.sub.at(i) & 15;
    if (m.cls == 6) {
      out.push_back(shape);
    } else if ((m.sub.at(i) & 0x10) != 0) {
      out.push_back(0);  // B_Direct_8x8
    } else {
      // Table 7-18: 8x8 1..3, then (8x4, 4x8) per direction L0 4/5, L1 6/7, Bi 8/9, then 4x4 L0 10, L1 11, Bi 12.
      const int d = m.dir.at(i);
      out.push_back(shape == 0 ? d : shape == 3 ? 9 + d : 2 + (2 * d) + (shape - 1));
    }
  }
  return out;
}

// The luma4x4BlkIdx of the top-left 4x4 block of each partition that has a motion vector difference for `list`,
// in the order the partitions are read.
std::vector<int> mvd_blocks(const Mb& m, int list) {
  const int bit = 1 << list;
  std::vector<int> out;
  const auto add = [&](int blk, int dir) {
    if ((dir & bit) != 0) {
      out.push_back(blk);
    }
  };
  switch (m.cls) {
    case 3:
    case 9:
      add(0, m.dir[0]);
      break;
    case 4:
    case 10:
      add(0, m.dir[0]);
      add(8, m.dir[2]);
      break;
    case 5:
    case 11:
      add(0, m.dir[0]);
      add(4, m.dir[1]);
      break;
    case 6:
    case 12:
      for (int i = 0; i < 4; ++i) {
        const auto ui = static_cast<size_t>(i);
        if ((m.sub.at(ui) & 0x10) != 0) {
          continue;
        }
        const int shape = m.sub.at(ui) & 15;
        const std::vector<int> subs = shape == 0   ? std::vector<int>{0}
                                      : shape == 1 ? std::vector<int>{0, 2}
                                      : shape == 2 ? std::vector<int>{0, 1}
                                                   : std::vector<int>{0, 1, 2, 3};
        for (const int s : subs) {
          add((4 * i) + s, m.dir.at(ui));
        }
      }
      break;
    default:
      break;
  }
  return out;
}

json expected_mvd(const Mb& m, int list) {
  json out = json::array();
  for (const int blk : mvd_blocks(m, list)) {
    const auto& v = m.mvd.at(static_cast<size_t>(list)).at(static_cast<size_t>(blk));
    out.push_back(json::array({v[0], v[1]}));
  }
  return out;
}

// ref_idx_lX as signalled: one per partition that uses `list` (B_Direct_8x8 sub-macroblocks have none), in read
// order. JM traces it only when the stream signals it (num_ref_idx_active > 1, not P_8x8ref0).
json expected_ref_idx(const Mb& m, int list) {
  std::vector<int> parts;  // the 8x8 index of each partition
  switch (m.cls) {
    case 3:
    case 9:
      parts = {0};
      break;
    case 4:
    case 10:
      parts = {0, 2};
      break;
    case 5:
    case 11:
      parts = {0, 1};
      break;
    case 6:
    case 12:
      for (int i = 0; i < 4; ++i) {
        if ((m.sub.at(static_cast<size_t>(i)) & 0x10) == 0) {
          parts.push_back(i);
        }
      }
      break;
    default:
      break;
  }
  json out = json::array();
  for (const int i : parts) {
    const auto ui = static_cast<size_t>(i);
    if ((m.dir.at(ui) & (1 << list)) != 0) {
      out.push_back(m.ref.at(static_cast<size_t>(list)).at(ui));
    }
  }
  return out;
}

int blk_of(int bx, int by) {
  return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2);
}

// Intra4x4PredMode / Intra8x8PredMode of the 4x4 cell (bx, by) of `m`, as a neighbour sees it (H.264 8.3.1.1 and
// 8.3.2.1: an 8x8 block covers its four cells, so A and B of an 8x8 block land on blocks 1 and 2 of an I4x4 MB).
int cell_mode(const Mb& m, int bx, int by) {
  return m.t8 != 0 ? m.i8.at(static_cast<size_t>((bx >> 1) | ((by >> 1) << 1)))
                   : m.i4.at(static_cast<size_t>(blk_of(bx, by)));
}

// The mode a neighbouring 4x4 cell (bx, by) of `m` contributes to the prediction (H.264 8.3.1.1): its mode when
// it lies in an I_NxN macroblock, 2 for other macroblocks; `dc` is set when it is unavailable (other slice, outside the
// picture, not decoded) or inter under constrained_intra_pred_flag, which makes the prediction 2.
int neighbour_mode(const Frame& f, const Mb& m, int bx, int by, int step, bool constrained_intra, bool& dc) {
  int mx = m.x;
  int my = m.y;
  if (bx < 0) {
    mx -= 1;
    bx += 4;
  }
  if (by < 0) {
    my -= step;
    by += 4;
  }
  const Mb* n = (mx == m.x && my == m.y) ? &m : f.at(mx, my);
  if (n == nullptr || n->cls == 255 || n->slice != m.slice || (constrained_intra && n->cls >= 3)) {
    dc = true;
    return 2;
  }
  return n->cls == 0 ? cell_mode(*n, bx, by) : 2;
}

// The signalled form of each Intra4x4/8x8 mode of the I_NxN macroblock `m` in a non-MBAFF picture: -1 when it equals
// the predicted mode, otherwise rem_intra_pred_mode. `step` is 2 in field pictures (rows of one field interleaved).
std::vector<int> expected_intra_modes(const Frame& f, const Mb& m, int step, bool constrained_intra) {
  std::vector<int> out;
  const bool t8 = m.t8 != 0;
  for (int i = 0; i < (t8 ? 4 : 16); ++i) {
    const int bx = t8 ? (i & 1) * 2 : (i & 1) | ((i >> 1) & 2);
    const int by = t8 ? (i >> 1) * 2 : ((i >> 1) & 1) | ((i >> 2) & 2);
    bool dc = false;
    const int a = neighbour_mode(f, m, bx - 1, by, step, constrained_intra, dc);
    const int b = neighbour_mode(f, m, bx, by - 1, step, constrained_intra, dc);
    const int pred = dc ? 2 : std::min(a, b);
    const int mode = cell_mode(m, bx, by);
    out.push_back(mode == pred ? -1 : mode < pred ? mode : mode - 1);
  }
  return out;
}

// ---- the comparison ---------------------------------------------------------------------------------------------

struct Counts {
  int mbs = 0;
  int intra_modes = 0;
  int mvds = 0;
  int sub_mb_types = 0;
  int ref_idx = 0;
  std::vector<std::string> bad;
};

void expect_eq(Counts& c, const std::string& where, const char* what, const json& got, const json& want) {
  if (got != want && c.bad.size() < 20) {
    c.bad.push_back(where + " " + what + ": exported " + got.dump() + ", JM " + want.dump());
  }
}

// The mb_type JM traces for `m`, given JM's value `jm` (needed to tell P_8x8ref0 from P_8x8).
json expected_jm_mb_type(const Mb& m, const Picture& pic, int slice_type, const json& jm) {
  json want_type = expected_mb_type(m, slice_type);
  const bool p_slice = slice_type == kSliceP || slice_type == kSliceSP;
  if (pic.cabac && p_slice && want_type.is_number_integer()) {
    // JM 19.0 traces the mb_type of CABAC P/SP slices as the syntax value + 1 (its reader counts P_Skip as 0):
    // every macroblock of the CABAC P pictures of jvt_HCAFF1_HHI_B and jvt_MR9_BT_B. CAVLC and I slices trace
    // the syntax value.
    want_type = want_type.get<int>() + 1;
  } else if (m.cls == 6 && jm == 4) {  // P_8x8ref0: every partition uses reference index 0
    want_type = std::all_of(m.ref[0].begin(), m.ref[0].end(), [](int r) { return r == 0; }) ? json(4) : json(-4);
  }
  return want_type;
}

// The macroblock of `f` that JM's address `addr` in picture `pic` names, or nullptr.
const Mb* mb_at(const Frame& f, const Picture& pic, int addr) {
  if (pic.mbaff) {
    return f.at((addr / 2) % f.w, (2 * ((addr / 2) / f.w)) + (addr % 2));
  }
  return f.at(addr % f.w, pic.field ? (2 * (addr / f.w)) + (pic.bottom ? 1 : 0) : addr / f.w);
}

void compare_mb(Counts& c, const Frame& f, const Picture& pic, const json& g, const std::string& where) {
  const int addr = g.at("mb_addr").get<int>();
  const Mb* m = mb_at(f, pic, addr);
  if (m == nullptr) {
    expect_eq(c, where, "position", addr, "inside the picture");
    return;
  }
  ++c.mbs;
  const int slice_type = pic.slice_type(addr);
  if (g.contains("mb_type") && slice_type != 4) {
    expect_eq(c, where, "mb_type", expected_jm_mb_type(*m, pic, slice_type, g.at("mb_type")), g.at("mb_type"));
  }
  if (g.contains("sub_mb_type") && (m->cls == 6 || m->cls == 12)) {
    ++c.sub_mb_types;
    expect_eq(c, where, "sub_mb_type", expected_sub_mb_types(*m), g.at("sub_mb_type"));
  }
  const std::array<std::pair<const char*, int>, 5> scalars = {{{"cbp", m->cbp},
                                                               {"mb_qp_delta", m->qpd},
                                                               {"intra_chroma_pred_mode", m->chroma},
                                                               {"transform_size_8x8_flag", m->t8},
                                                               {"mb_field_decoding_flag", m->field}}};
  for (const auto& [key, value] : scalars) {
    if (g.contains(key)) {
      expect_eq(c, where, key, value, g.at(key));
    }
  }
  for (int list = 0; list < 2; ++list) {
    const std::string key = "mvd_l" + std::to_string(list);
    if (g.contains(key)) {
      c.mvds += static_cast<int>(g.at(key).size());
      expect_eq(c, where, key.c_str(), expected_mvd(*m, list), g.at(key));
    }
    const std::string ref_key = "ref_idx_l" + std::to_string(list);
    if (g.contains(ref_key)) {
      c.ref_idx += static_cast<int>(g.at(ref_key).size());
      expect_eq(c, where, ref_key.c_str(), expected_ref_idx(*m, list), g.at(ref_key));
    }
  }
  // The neighbour derivation of MBAFF pairs (6.4.12.2) is not modelled here, so MBAFF intra modes are not compared.
  if (g.contains("intra4x4_pred_mode") && m->cls == 0 && !pic.mbaff) {
    c.intra_modes += static_cast<int>(g.at("intra4x4_pred_mode").size());
    expect_eq(c, where, "intra4x4_pred_mode", expected_intra_modes(f, *m, pic.field ? 2 : 1, pic.constrained_intra),
              g.at("intra4x4_pred_mode"));
  }
}

// AT1 (mb_type class, sub_mb_type and the other signalled fields against JM) and AT3 (frame threads).
void compare_with_jm(const std::string& id) {
  CAPTURE(id);
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  const std::string cmd = std::string("\"") + OVT_FFMPEG_BLOCKPARAMS + "\" \"" + path.string() + "\" ";
  const std::string single = read_pipe(cmd + "1");
  CHECK(read_pipe(cmd + "4") == single);  // AT3: frame threading exports the same side data

  const std::vector<Frame> frames = parse_dump(single);
  REQUIRE_FALSE(frames.empty());
  for (const Frame& f : frames) {
    REQUIRE(f.w > 0);  // every frame carries the side data
    REQUIRE(f.mbs.size() == static_cast<size_t>(f.w) * static_cast<size_t>(f.h));
  }
  const std::vector<Picture> pictures = header_pictures(read_golden(id + ".trace_headers.json.gz"));

  // JM pictures: the macroblocks of the golden, split where the address stops increasing.
  std::vector<std::vector<json>> jm;
  int last = -1;
  const json golden = read_golden(id + ".jm-trace.json.gz");
  for (const json& frame : golden.at("frames")) {
    for (const json& mb : frame.value("mbs", json::array())) {
      const int addr = mb.at("mb_addr").get<int>();
      if (addr <= last || jm.empty()) {
        jm.emplace_back();
      }
      jm.back().push_back(mb);
      last = addr;
    }
  }
  REQUIRE_FALSE(jm.empty());
  REQUIRE(pictures.size() >= jm.size());

  Counts c;
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
    for (const json& mb : jm[k]) {
      compare_mb(c, frames[this_frame], pic, mb, "picture " + std::to_string(k) + " mb " + mb.at("mb_addr").dump());
    }
  }
  INFO("macroblocks " << c.mbs << ", intra modes " << c.intra_modes << ", mvds " << c.mvds << ", sub_mb_types "
                      << c.sub_mb_types << ", ref_idx " << c.ref_idx);
  for (const std::string& b : c.bad) {
    UNSCOPED_INFO(b);
  }
  CHECK(c.bad.empty());
  CHECK(c.mbs > 0);
}

}  // namespace

TEST_CASE("ffmpeg plugin: block params turn back into JM's mb_type and sub_mb_type", "[fast][third_party]") {
  Mb m;
  m.cls = 0;
  CHECK(expected_mb_type(m, kSliceI) == 0);
  CHECK(expected_mb_type(m, kSliceP) == 5);
  CHECK(expected_mb_type(m, kSliceB) == 23);
  m.cls = 1;
  m.i16 = 2;
  m.cbp = 15 + (2 << 4);
  CHECK(expected_mb_type(m, kSliceI) == 1 + 2 + 8 + 12);  // I_16x16_2_2_1 = 23
  m.cls = 2;
  CHECK(expected_mb_type(m, kSliceP) == 30);
  m.cls = 7;
  CHECK(expected_mb_type(m, kSliceP) == "P_Skip");
  CHECK(expected_mb_type(m, kSliceB).is_string());
  m.cls = 10;
  m.dir = {2, 2, 3, 3};
  CHECK(expected_mb_type(m, kSliceB) == 14);  // B_L1_Bi_16x8
  m.cls = 11;
  m.dir = {3, 1, 3, 1};
  CHECK(expected_mb_type(m, kSliceB) == 17);  // B_Bi_L0_8x16
  m.cls = 12;
  m.sub = {0x10, 0, 1, 3};
  m.dir = {3, 2, 1, 3};
  CHECK(expected_sub_mb_types(m) == std::vector<int>{0, 2, 4, 12});
  m.sub = {2, 1, 2, 3};
  m.dir = {2, 3, 3, 1};
  CHECK(expected_sub_mb_types(m) == std::vector<int>{7, 8, 9, 10});
}

TEST_CASE("ffmpeg plugin: block-params mvds and ref_idx come out in JM's read order", "[fast][third_party]") {
  // jvt_BA_MW_D picture 1 macroblock 0: P_8x8ref0, sub_mb_type [3, 0, 2, 0], 8 mvd_l0 (the JM golden's record).
  const json jm = json::parse(R"([[0,0],[12,10],[-1,13],[13,-1],[-7,5],[0,0],[0,12],[0,-2]])");
  Mb m;
  m.cls = 6;
  m.sub = {3, 0, 2, 0};
  m.dir = {1, 1, 1, 1};
  // The export replicates each partition's mvd over its 4x4 blocks (luma4x4BlkIdx order).
  const std::array<std::array<int, 2>, 16> blocks = {{{0, 0},
                                                      {12, 10},
                                                      {-1, 13},
                                                      {13, -1},  // 8x8 0: 4x4 x4
                                                      {-7, 5},
                                                      {-7, 5},
                                                      {-7, 5},
                                                      {-7, 5},  // 8x8 1: 8x8
                                                      {0, 0},
                                                      {0, 12},
                                                      {0, 0},
                                                      {0, 12},  // 8x8 2: 4x8 x2
                                                      {0, -2},
                                                      {0, -2},
                                                      {0, -2},
                                                      {0, -2}}};  // 8x8 3: 8x8
  m.mvd[0] = blocks;
  CHECK(expected_mvd(m, 0) == jm);
  CHECK(expected_mvd(m, 1).empty());
  m.ref[0] = {0, 1, 2, 0};
  m.sub = {0, 0, 0x10, 0};
  CHECK(expected_ref_idx(m, 0) == json::parse("[0, 1, 0]"));  // no ref_idx for a direct sub-macroblock
  m.cls = 11;                                                 // B_L1_Bi_8x16: partitions 0 (L1) and 1 (Bi)
  m.dir = {2, 3, 2, 3};
  m.ref = {{{-1, 4, -1, 4}, {1, 2, 1, 2}}};
  CHECK(expected_ref_idx(m, 0) == json::parse("[4]"));
  CHECK(expected_ref_idx(m, 1) == json::parse("[1, 2]"));
}

TEST_CASE("ffmpeg plugin: block-params intra modes turn back into prev flag / rem", "[fast][third_party]") {
  Frame f;
  f.w = 2;
  f.h = 1;
  f.mbs.resize(2);
  Mb& left = f.mbs[0];
  left.cls = 0;
  left.i4.fill(1);  // horizontal everywhere
  Mb& cur = f.mbs[1];
  cur.x = 1;
  cur.cls = 0;
  cur.i4.fill(0);
  // Top row: B unavailable, so the prediction is DC (2) and mode 0 is signalled as rem 0. Below it: A and B are
  // available; block 2's A is the left MB's block 7 (mode 1), B is block 0 (mode 0): min = 0 = mode -> -1.
  const std::vector<int> got = expected_intra_modes(f, cur, 1, false);
  CHECK(got[0] == 0);
  CHECK(got[1] == 0);
  CHECK(got[2] == -1);
  CHECK(got[3] == -1);
  // An inter left neighbour with constrained_intra_pred_flag forces DC: mode 0 -> rem 0 instead of -1.
  left.cls = 3;
  CHECK(expected_intra_modes(f, cur, 1, true)[2] == 0);
  CHECK(expected_intra_modes(f, cur, 1, false)[2] == -1);  // otherwise an inter neighbour counts as DC (2)
}

TEST_CASE("ffmpeg plugin: ffmpeg_mbparams_vs_jm", "[fast][third_party][needs-corpus]") {
  compare_with_jm(GENERATE(as<std::string>{}, "jvt_BA1_Sony_D", "jvt_BA_MW_D", "jvt_CABA1_SVA_B", "jvt_CI1_FT_B",
                           "jvt_CVPCMNL1_SVA_C", "jvt_CVSE2_Sony_B", "jvt_CVSE3_Sony_H", "jvt_CVWP1_TOSHIBA_E",
                           "jvt_FRExt1_Panasonic_D", "jvt_HCAFF1_HHI_B", "jvt_Hi422FR1_SONY_A", "jvt_MIDR_MW_D",
                           "jvt_MR1_BT_A", "jvt_MR6_BT_B", "jvt_MR9_BT_B", "jvt_NRF_MW_E", "jvt_aud_mw_e"));
}

TEST_CASE("ffmpeg plugin: ffmpeg_mbparams_vs_jm on CAMA1_Sony_C", "[slow][third_party][needs-corpus]") {
  // Acceptance test 1 names this MBAFF stream; its JM golden is not committed yet (docs/acceptance/wi/WI-4.101.md has
  // the oracle-image commands that make it).
  const std::string id = "jvt_CAMA1_Sony_C";
  if (!std::filesystem::exists(std::filesystem::path(OVT_JM_GOLDEN_DIR) / (id + ".jm-trace.json.gz"))) {
    SKIP("no JM golden for " << id);
  }
  compare_with_jm(id);
}
