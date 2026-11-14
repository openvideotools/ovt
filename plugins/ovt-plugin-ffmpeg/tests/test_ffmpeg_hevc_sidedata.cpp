// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.107 (chapter 07 §7.8): the optional ffmpeg_hevc plugin, which maps the motion vectors and per-CU QP that patch
// 0002 makes libavcodec's hevc decoder export. Acceptance test 1 compares its CellGrid (pred, qp, mv0, mv1) cell by
// cell with the patched libde265, through the oracle ovt_plugin_de265_grid_dump (plugins/ovt-plugin-de265/tests): the
// same internals the de265 plugin maps (WI-2.164), read directly because that plugin does not fill its grid yet.
// SPEC-ASSUMPTION(#371): the oracle is libde265's internals API, not the de265 plugin's report.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/analysis/decoder_plugin_registry.hpp"
#include "ovt_model_generated.h"
#include "test_support/corpus.h"

namespace {

constexpr uint8_t kCodecH265 = 2;  // ovt.fb.Codec.H265
#ifdef OVT_DE265_GRID_DUMP
constexpr const char* kDe265GridDump = OVT_DE265_GRID_DUMP;
#else
constexpr const char* kDe265GridDump = nullptr;  // no patched libde265 in this build (OVT_SYSTEM_DE265)
#endif

ovt::analysis::PluginRegistry& registry() {
  static ovt::analysis::PluginRegistry r(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_FFMPEG_PLUGIN_DIR), std::vector<std::string>{}});
  return r;
}

const ovt_AnalysisDecoderApi& api() {
  const ovt::analysis::LoadedPlugin* p = registry().find("ffmpeg_hevc");
  REQUIRE(p != nullptr);
  return *p->api;
}

struct Instance {
  explicit Instance(int threads, const uint8_t* cp = nullptr, size_t len = 0) {
    status = api().create(cp, len, threads, &dec);
  }
  ~Instance() {
    if (dec != nullptr) {
      api().destroy(dec);
    }
  }
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  ovt_AnalysisDecoder* dec = nullptr;
  ovt_status status = OVT_E_INTERNAL;
};

/// One cell as the oracle prints it: pred (0 none, 1 intra, 2 inter), QP_Y, then per list the flag and the vector in
/// quarter samples (0 when the list is unused).
struct Cell {
  int pred = 0;
  int qp = 0;
  std::array<int, 2> pf{};
  std::array<int, 2> mv_x{};
  std::array<int, 2> mv_y{};
  bool operator==(const Cell&) const = default;
};

struct Grid {
  int cells_w = 0;
  int cells_h = 0;
  std::vector<Cell> cells;
};

std::string describe(const Cell& c) {
  std::ostringstream s;
  s << "pred " << c.pred << " qp " << c.qp;
  for (int l = 0; l < 2; ++l) {
    s << " L" << l << (c.pf.at(static_cast<size_t>(l)) != 0 ? " (" : " none (") << c.mv_x.at(static_cast<size_t>(l))
      << "," << c.mv_y.at(static_cast<size_t>(l)) << ")";
  }
  return s.str();
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A quarter-sample vector component as the CellGrid can hold it: int16 in 1/16 samples, clamped to +-32767
/// (map_hevc_sidedata.cpp, MDL-GEN-0003), read back in quarter samples. MVCLIP_A has vectors past that range.
int in_cell_grid_range(int quarter) {
  constexpr int kMax = 32767;
  return std::clamp(quarter * 4, -kMax, kMax) / 4;
}

/// The pictures of the oracle's dump, in output order, with the vectors in the CellGrid's range.
std::vector<Grid> de265_grids(const std::filesystem::path& stream) {
  const std::string cmd = std::string("\"") + kDe265GridDump + "\" \"" + stream.string() + "\"";
  FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): the oracle executable of this build
  REQUIRE(p != nullptr);
  std::string text;
  std::array<char, 65536> buf{};
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    text.append(buf.data(), n);
  }
  REQUIRE(pclose(p) == 0);
  std::istringstream in(text);
  std::vector<Grid> out;
  std::string word;
  while (in >> word) {
    REQUIRE(word == "picture");
    int index = 0;
    int poc = 0;
    Grid g;
    in >> index >> poc >> g.cells_w >> g.cells_h;
    g.cells.resize(static_cast<size_t>(g.cells_w) * static_cast<size_t>(g.cells_h));
    for (Cell& c : g.cells) {
      in >> c.pred >> c.qp >> c.pf[0] >> c.mv_x[0] >> c.mv_y[0] >> c.pf[1] >> c.mv_x[1] >> c.mv_y[1];
      for (size_t l = 0; l < 2; ++l) {
        c.mv_x.at(l) = in_cell_grid_range(c.mv_x.at(l));
        c.mv_y.at(l) = in_cell_grid_range(c.mv_y.at(l));
      }
    }
    REQUIRE(in.good());
    out.push_back(std::move(g));
  }
  return out;
}

/// The plugin's CellGrid in the oracle's form: vectors from 1/16 to quarter samples.
Grid plugin_grid(const ovt::fb::FrameAnalysis* fa) {
  const ovt::fb::CellGrid* g = fa->grid();
  REQUIRE(g != nullptr);
  Grid out;
  out.cells_w = static_cast<int>(g->cells_w());
  out.cells_h = static_cast<int>(g->cells_h());
  const size_t cells = static_cast<size_t>(out.cells_w) * static_cast<size_t>(out.cells_h);
  REQUIRE(g->pred() != nullptr);
  REQUIRE(g->pred()->size() == cells);
  REQUIRE(g->qp()->size() == cells);
  out.cells.resize(cells);
  for (size_t i = 0; i < cells; ++i) {
    Cell& c = out.cells[i];
    c.pred = g->pred()->Get(static_cast<flatbuffers::uoffset_t>(i));
    c.qp = g->qp()->Get(static_cast<flatbuffers::uoffset_t>(i));
    const std::array<const flatbuffers::Vector<int16_t>*, 2> mx = {g->mv0_x(), g->mv1_x()};
    const std::array<const flatbuffers::Vector<int16_t>*, 2> my = {g->mv0_y(), g->mv1_y()};
    const std::array<const flatbuffers::Vector<int8_t>*, 2> ref = {g->ref0(), g->ref1()};
    for (size_t l = 0; l < 2; ++l) {
      const auto at = static_cast<flatbuffers::uoffset_t>(i);
      c.pf.at(l) = ref.at(l)->Get(at) >= 0 ? 1 : 0;
      c.mv_x.at(l) = c.pf.at(l) != 0 ? mx.at(l)->Get(at) / 4 : 0;
      c.mv_y.at(l) = c.pf.at(l) != 0 ? my.at(l)->Get(at) / 4 : 0;
    }
  }
  return out;
}

bool vcl(int type) {
  return type < 32;
}

/// The NAL units of an Annex B stream as (offset, size): start code to start code, trailing zero bytes dropped.
std::vector<std::pair<size_t, size_t>> split_nals(const std::vector<uint8_t>& bytes) {
  std::vector<std::pair<size_t, size_t>> nals;
  size_t start = SIZE_MAX;
  const auto close = [&](size_t end) {
    while (end > start && bytes[end - 1] == 0) {
      --end;
    }
    if (end > start) {
      nals.emplace_back(start, end - start);
    }
  };
  for (size_t i = 0; i + 3 <= bytes.size();) {
    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
      if (start != SIZE_MAX) {
        close(i);
      }
      i += 3;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX) {
    close(bytes.size());
  }
  return nals;
}

/// Pushes an Annex B H.265 stream NAL by NAL (decode_index_hint = access unit index) and returns the grids of the
/// frames in pull() order.
std::vector<Grid> plugin_grids(const std::vector<uint8_t>& bytes, int threads) {
  Instance in(threads);
  REQUIRE(in.status == OVT_OK);
  std::vector<Grid> out;
  const auto drain = [&] {
    for (;;) {
      ovt_Buffer fb{};
      REQUIRE(api().pull(in.dec, 0, &fb) == OVT_OK);
      if (fb.data == nullptr) {
        return;
      }
      flatbuffers::Verifier verifier(fb.data, fb.size);
      REQUIRE(ovt::fb::VerifyFrameAnalysisBuffer(verifier));
      out.push_back(plugin_grid(ovt::fb::GetFrameAnalysis(fb.data)));
      api().release(in.dec, fb);
    }
  };
  int au = 0;
  bool seen_vcl = false;
  for (const auto& [pos, len] : split_nals(bytes)) {
    if (len < 3) {
      continue;
    }
    const uint8_t* nal = &bytes[pos];
    const int type = (nal[0] >> 1) & 0x3F;
    // H.265 7.4.2.4.4: an access unit starts at the first VCL unit with first_slice_segment_in_pic_flag, or at an
    // AUD, VPS, SPS, PPS, prefix SEI or reserved 41..44 / 48..55 unit after a VCL unit.
    const bool starts_au = type == 35 || type == 32 || type == 33 || type == 34 || type == 39 ||
                           (type >= 41 && type <= 44) || (type >= 48 && type <= 55);
    if (seen_vcl && ((vcl(type) && (nal[2] & 0x80) != 0) || starts_au)) {
      ++au;
      seen_vcl = false;
    }
    seen_vcl = seen_vcl || vcl(type);
    (void)api().push(in.dec, nal, len, au, 1000 + au);
    drain();
  }
  REQUIRE(api().flush(in.dec) == OVT_OK);
  drain();
  return out;
}

bool same_grid(const Grid& a, const Grid& b) {
  return a.cells_w == b.cells_w && a.cells_h == b.cells_h && a.cells == b.cells;
}

void compare_with_de265(const std::string& id) {
  if (kDe265GridDump == nullptr) {
    SKIP("this build has no patched libde265 for the oracle");
  }
  const std::filesystem::path path = ovt::test::corpus_or_skip(id);
  INFO("stream " << id);
  const std::vector<Grid> ref = de265_grids(path);
  const std::vector<uint8_t> bytes = read_file(path);
  const std::vector<Grid> got = plugin_grids(bytes, 1);
  REQUIRE(!got.empty());
  // libde265 1.0.15 never outputs the last picture it decodes in some streams (WI-2.162 note, #251). In a
  // hierarchical-B stream that picture is not the last in output order, so the pictures are matched in order: every
  // libde265 picture must equal the next unmatched ffmpeg_hevc picture or, while fewer pictures were withheld than
  // the difference in counts, one of the pictures after it.
  REQUIRE(ref.size() <= got.size());
  REQUIRE(ref.size() + 2 >= got.size());
  size_t skips = got.size() - ref.size();
  size_t g = 0;
  for (size_t pic = 0; pic < ref.size(); ++pic, ++g) {
    while (skips > 0 && g + 1 < got.size() && !same_grid(got[g], ref[pic])) {
      WARN(id << ": libde265 did not output the picture at ffmpeg_hevc output index " << g);
      --skips;
      ++g;
    }
    INFO("libde265 picture " << pic << ", ffmpeg_hevc picture " << g << " (output order)");
    REQUIRE(g < got.size());
    REQUIRE(got[g].cells_w == ref[pic].cells_w);
    REQUIRE(got[g].cells_h == ref[pic].cells_h);
    for (size_t i = 0; i < ref[pic].cells.size(); ++i) {
      const Cell& a = got[g].cells[i];
      const Cell& b = ref[pic].cells[i];
      if (!(a == b)) {
        const auto w = static_cast<size_t>(ref[pic].cells_w);
        FAIL("cell (" << (i % w) * 4 << "," << (i / w) * 4 << "): ffmpeg_hevc " << describe(a) << ", libde265 "
                      << describe(b));
      }
    }
  }
  if (skips > 0) {
    WARN(id << ": libde265 did not output the last " << skips << " ffmpeg_hevc picture(s)");
  }
  // The plugin's grid does not depend on libavcodec slice threads.
  const std::vector<Grid> threaded = plugin_grids(bytes, 4);
  REQUIRE(threaded.size() == got.size());
  for (size_t pic = 0; pic < got.size(); ++pic) {
    INFO("picture " << pic << " with 4 threads");
    REQUIRE(threaded[pic].cells == got[pic].cells);
  }
}

}  // namespace

TEST_CASE("ffmpeg_hevc plugin: caps and ABI table", "[fast][third_party]") {
  const ovt::analysis::LoadedPlugin* p = registry().find("ffmpeg_hevc");
  REQUIRE(p != nullptr);
  const ovt_AnalysisCaps& caps = *p->caps;
  CHECK(caps.codec == kCodecH265);
  CHECK(std::string(caps.id) == "ffmpeg_hevc");
  CHECK(caps.grid_fields == (OVT_GRID_BLOCK_ID | OVT_GRID_PRED | OVT_GRID_QP | OVT_GRID_MV0 | OVT_GRID_MV1 |
                             OVT_GRID_REF0 | OVT_GRID_REF1));
  CHECK(caps.bit_exact == 1);
}

TEST_CASE("ffmpeg_hevc plugin: never the default H.265 plugin", "[fast][third_party]") {
  // The plugin directory of this build has the ffmpeg plugins only (de265 and hm are not plugins yet, or live
  // elsewhere): the H.265 default stays empty instead of falling back to ffmpeg_hevc.
  ovt::analysis::PluginRegistry r(
      ovt::analysis::PluginRegistry::Options{std::string(OVT_FFMPEG_PLUGIN_DIR), std::vector<std::string>{}});
  REQUIRE(r.find("ffmpeg_hevc") != nullptr);
  const ovt::analysis::LoadedPlugin* def = r.default_for(kCodecH265);
  CHECK((def == nullptr || std::string(def->caps->id) != "ffmpeg_hevc"));
  // Chosen explicitly (analysis.default_plugin.h265), it is used.
  REQUIRE(r.set_default(kCodecH265, "ffmpeg_hevc") == OVT_OK);
  def = r.default_for(kCodecH265);
  REQUIRE(def != nullptr);
  CHECK(std::string(def->caps->id) == "ffmpeg_hevc");
}

TEST_CASE("ffmpeg_hevc plugin: malformed hvcC is rejected", "[fast][third_party]") {
  const std::array<uint8_t, 4> truncated = {1, 1, 0x60, 0};
  ovt_AnalysisDecoder* dec = nullptr;
  CHECK(api().create(truncated.data(), truncated.size(), 1, &dec) == OVT_E_CORRUPT);
  CHECK(dec == nullptr);
}

// Acceptance test 1: on the fast HEVC_v1 streams, the MV/QP grid of the patched FFmpeg equals libde265's.
// Left out: streams with pictures libde265 never outputs (OPFLAG_A, RAP_A/B, NoOutPrior_A; #251), SLIST_A (#254) and
// the RExt streams libde265 does not decode.
TEST_CASE("ffmpeg_hevc plugin: MV and QP grids equal libde265 on HEVC_v1", "[fast][third_party][needs-corpus]") {
  compare_with_de265(GENERATE(as<std::string>{}, "jctvc_AMP_A_Samsung_7", "jctvc_AMP_D_Hisilicon_3",
                              "jctvc_CAINIT_A_SHARP_4", "jctvc_CONFWIN_A_Sony_1", "jctvc_DSLICE_A_HHI_5",
                              "jctvc_MAXBINS_A_TI_5", "jctvc_MERGE_B_TI_3", "jctvc_PMERGE_A_TI_3",
                              "jctvc_POC_A_Bossen_3", "jctvc_PPS_A_qualcomm_7", "jctvc_RPS_A_docomo_5",
                              "jctvc_SAO_A_MediaTek_4", "jctvc_STRUCT_B_Samsung_7", "jctvc_TILES_B_Cisco_1",
                              "jctvc_TSCL_A_VIDYO_5", "jctvc_WP_A_Toshiba_3", "jctvc_MVCLIP_A_qualcomm_3",
                              "jctvc_MVEDGE_A_qualcomm_3", "jctvc_ipcm_A_NEC_3", "jctvc_WPP_A_ericsson_MAIN10_2"));
}
