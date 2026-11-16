// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers for tests that compare ovt-plugin-ffmpeg with the committed H.264 oracle goldens
// (tests/golden/h264/<id>.{jm-trace,trace_headers,ffmpeg-mvs}.json.gz, chapter 14 §14.4.3 / §14.4.3a): reading a
// golden, the picture structure of a stream from its trace_headers golden, and JM's macroblock address -> macroblock
// position. Not a .hpp on purpose: it needs Catch2 (see plugin_test_util.inc).
#ifndef OVT_PLUGIN_FFMPEG_TESTS_H264_GOLDEN_INC
#define OVT_PLUGIN_FFMPEG_TESTS_H264_GOLDEN_INC

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ovt_ffmpeg_test {

using nlohmann::json;

inline std::string read_pipe(const std::string& cmd) {
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

// The golden `name` (e.g. "jvt_BA1_Sony_D.jm-trace.json.gz") of tests/golden/h264.
inline json read_golden(const std::string& name) {
  const std::filesystem::path file = std::filesystem::path(OVT_JM_GOLDEN_DIR) / name;
  return json::parse(read_pipe("gzip -dc \"" + file.string() + "\""));
}

// ---- the stream structure, from the trace_headers golden -------------------------------------------------------

struct Slice {
  int first_mb = 0;   // in macroblocks (pairs already doubled for MBAFF)
  int type = 0;       // slice_type % 5: 0 P, 1 B, 2 I, 3 SP, 4 SI
  int qp = 26;        // SliceQPY = 26 + pic_init_qp_minus26 + slice_qp_delta (7.4.3)
  int bd_offset = 0;  // QpBdOffsetY = 6 * bit_depth_luma_minus8
};

struct Picture {
  bool field = false;
  bool bottom = false;
  bool mbaff = false;
  bool constrained_intra = false;
  bool cabac = false;
  std::vector<Slice> slices;

  // The slice that holds macroblock `addr` (no ASO in the golden streams), or nullptr.
  [[nodiscard]] const Slice* slice_of(int addr) const {
    const Slice* found = nullptr;
    for (const Slice& s : slices) {
      if (s.first_mb <= addr) {
        found = &s;
      }
    }
    return found;
  }

  [[nodiscard]] int slice_type(int addr) const {
    const Slice* s = slice_of(addr);
    return s != nullptr ? s->type : -1;
  }
};

// Pictures in decoding order; a picture starts at first_mb_in_slice = 0 (no ASO in the golden streams).
inline std::vector<Picture> header_pictures(const json& elements) {
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
  std::map<int, int> sps_mbaff;
  std::map<int, int> sps_bd_offset;
  struct Pps {
    int sps = 0;
    int init_qp = 26;
    bool constrained = false;
    bool cabac = false;
  };
  std::map<int, Pps> pps;
  for (const auto& u : units) {
    const int type = get(u, "nal_unit_type");
    if (type == 7) {
      const int id = get(u, "seq_parameter_set_id");
      sps_mbaff[id] = get(u, "mb_adaptive_frame_field_flag");
      sps_bd_offset[id] = 6 * get(u, "bit_depth_luma_minus8");
    } else if (type == 8) {
      pps[get(u, "pic_parameter_set_id")] =
          Pps{get(u, "seq_parameter_set_id"), 26 + get(u, "pic_init_qp_minus26"),
              get(u, "constrained_intra_pred_flag") != 0, get(u, "entropy_coding_mode_flag") != 0};
    } else if (type == 1 || type == 5) {
      const Pps& p = pps[get(u, "pic_parameter_set_id")];
      const bool field = get(u, "field_pic_flag") != 0;
      const bool mbaff = sps_mbaff[p.sps] != 0 && !field;
      const int first = get(u, "first_mb_in_slice") * (mbaff ? 2 : 1);
      if (first == 0 || pictures.empty()) {
        Picture pic;
        pic.field = field;
        pic.bottom = get(u, "bottom_field_flag") != 0;
        pic.mbaff = mbaff;
        pic.constrained_intra = p.constrained;
        pic.cabac = p.cabac;
        pictures.push_back(pic);
      }
      pictures.back().slices.push_back(
          Slice{first, get(u, "slice_type") % 5, p.init_qp + get(u, "slice_qp_delta"), sps_bd_offset[p.sps]});
    }
  }
  return pictures;
}

// The position, in macroblocks of the frame, of the macroblock JM's address `addr` names in `pic`: pair order in MBAFF
// frames (top, bottom), interleaved rows in field pictures, raster order otherwise. `w` is the frame width in
// macroblocks.
inline std::pair<int, int> mb_position(const Picture& pic, int addr, int w) {
  if (pic.mbaff) {
    return {(addr / 2) % w, (2 * ((addr / 2) / w)) + (addr % 2)};
  }
  return {addr % w, pic.field ? (2 * (addr / w)) + (pic.bottom ? 1 : 0) : addr / w};
}

// JM's pictures of a jm-trace golden: the macroblocks in trace order, split where the address stops increasing.
inline std::vector<std::vector<json>> jm_pictures(const json& golden) {
  std::vector<std::vector<json>> jm;
  int last = -1;
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
  return jm;
}

}  // namespace ovt_ffmpeg_test

#endif  // OVT_PLUGIN_FFMPEG_TESTS_H264_GOLDEN_INC
