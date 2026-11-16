// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Helpers shared by the block-mapping tests (test_de265_map.cpp, WI-2.164; test_de265_map_syntax.cpp, WI-2.165):
// hand-made libde265 metadata, map_blocks() wrapped in a FrameAnalysis, the chapter 06 validator, and the plugin run
// over a conformance stream.
#ifndef OVT_PLUGIN_DE265_TESTS_TEST_DE265_MAP_SUPPORT_HPP
#define OVT_PLUGIN_DE265_TESTS_TEST_DE265_MAP_SUPPORT_HPP

#include <catch2/catch_test_macros.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "map_blocks.hpp"
#include "ovt_model_generated.h"

namespace de265_test {

namespace fb = ovt::fb;
using ovt::de265::BlockData;
using ovt::de265::CbInfo;
using ovt::de265::PbInfo;
using ovt::de265::PictureFormat;

// ---- hand-made libde265 metadata -----------------------------------------------------------------------------------

struct Pic {
  BlockData d;
  PictureFormat fmt;

  Pic(int32_t w, int32_t h, int32_t ctb_log2, int32_t min_cb_log2) {
    fmt.coded_w = w;
    fmt.coded_h = h;
    d.valid = true;
    d.ctb_log2 = ctb_log2;
    d.min_cb_log2 = min_cb_log2;
    d.ctbs_w = (w + (1 << ctb_log2) - 1) >> ctb_log2;
    d.ctbs_h = (h + (1 << ctb_log2) - 1) >> ctb_log2;
    d.cbs_w = (w + (1 << min_cb_log2) - 1) >> min_cb_log2;
    d.cbs_h = (h + (1 << min_cb_log2) - 1) >> min_cb_log2;
    d.cells_w = static_cast<uint32_t>((w + 3) / 4);
    d.cells_h = static_cast<uint32_t>((h + 3) / 4);
    const std::size_t cells = std::size_t{d.cells_w} * d.cells_h;
    const auto ctbs = static_cast<std::size_t>(d.ctbs_w) * static_cast<std::size_t>(d.ctbs_h);
    d.cb.resize(static_cast<std::size_t>(d.cbs_w) * static_cast<std::size_t>(d.cbs_h));
    d.pb.resize(cells);
    d.intra_y.assign(cells, 255);
    d.intra_c.assign(cells, 255);
    d.tu.resize(cells);
    d.deblk.resize(cells);
    d.bs_v.resize(cells);
    d.ctb_slice.resize(ctbs);
    d.ctb_addr_ts.resize(ctbs);
    d.ctb_tile.resize(ctbs);
    d.sao.resize(ctbs);
    for (std::size_t i = 0; i < ctbs; ++i) {
      d.ctb_addr_ts[i] = static_cast<int32_t>(i);
    }
    d.slices.emplace_back();
  }

  [[nodiscard]] std::size_t cell(int32_t x, int32_t y) const {
    return static_cast<std::size_t>(y / 4) * d.cells_w + static_cast<std::size_t>(x / 4);
  }

  // Fills every 4x4 of the luma rect (x, y, w, h) through @p fn(cell index).
  template <typename Fn>
  void each(int32_t x, int32_t y, int32_t w, int32_t h, Fn fn) {
    for (int32_t yy = y; yy < y + h; yy += 4) {
      for (int32_t xx = x; xx < x + w; xx += 4) {
        fn(cell(xx, yy));
      }
    }
  }

  // A CU of 1 << log2 samples at (x, y); libde265 keeps its CB info at every MinCb unit it covers.
  void cu(int32_t x, int32_t y, int32_t log2, ovt::de265::De265PredMode mode, uint8_t part = 0, int16_t qp = 30) {
    const int32_t n = 1 << log2;
    const int32_t m = 1 << d.min_cb_log2;
    for (int32_t yy = y; yy < y + n; yy += m) {
      for (int32_t xx = x; xx < x + n; xx += m) {
        CbInfo& c = d.cb[cb_index(xx / m, yy / m)];
        c.log2CbSize = static_cast<uint8_t>(log2);
        c.PartMode = part;
        c.ctDepth = static_cast<uint8_t>(d.ctb_log2 - log2);
        c.PredMode = static_cast<uint8_t>(mode);
        c.QP_Y = qp;
      }
    }
  }
  [[nodiscard]] std::size_t cb_index(int32_t cx, int32_t cy) const {
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(d.cbs_w) + static_cast<std::size_t>(cx);
  }
  void cb_flag(int32_t x, int32_t y, int32_t log2, uint8_t CbInfo::* field) {
    const int32_t n = 1 << log2;
    const int32_t m = 1 << d.min_cb_log2;
    for (int32_t yy = y; yy < y + n; yy += m) {
      for (int32_t xx = x; xx < x + n; xx += m) {
        d.cb[cb_index(xx / m, yy / m)].*field = 1;
      }
    }
  }
  void intra(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t mode_y, uint8_t mode_c) {
    each(x, y, w, h, [&](std::size_t i) {
      d.intra_y[i] = mode_y;
      d.intra_c[i] = mode_c;
    });
  }
  void motion(int32_t x, int32_t y, int32_t w, int32_t h, int list, int8_t ref, int16_t mvx, int16_t mvy, int32_t poc) {
    each(x, y, w, h, [&](std::size_t i) {
      PbInfo& p = d.pb[i];
      const auto l = static_cast<std::size_t>(list);
      p.predFlag.at(l) = 1;
      p.refIdx.at(l) = ref;
      p.mv_x.at(l) = mvx;
      p.mv_y.at(l) = mvy;
      p.refPOC.at(l) = poc;
    });
  }
  void split_tu(int32_t x, int32_t y, int depth) { d.tu[cell(x, y)] |= static_cast<uint8_t>(1U << depth); }
  void nonzero(int32_t x, int32_t y, int32_t log2) {
    each(x, y, 1 << log2, 1 << log2, [&](std::size_t i) { d.tu[i] |= ovt::de265::kTuNonzeroCoeff; });
  }
};

// A FrameAnalysis around map_blocks(), the way plugin.cpp assembles it.
struct Analysis {
  std::unique_ptr<flatbuffers::FlatBufferBuilder> fbb;
  std::vector<std::string> warnings;
  [[nodiscard]] const fb::FrameAnalysis& fa() const { return *fb::GetFrameAnalysis(fbb->GetBufferPointer()); }
  [[nodiscard]] std::span<const uint8_t> bytes() const { return {fbb->GetBufferPointer(), fbb->GetSize()}; }
};

/// map_blocks() of @p p in a verified FrameAnalysis.
Analysis analyse(const Pic& p);

/// Runs the chapter 06 validator on @p buf; every finding fails except the ones the host fills later.
void check_valid(std::span<const uint8_t> buf);

template <typename T>
const T* ext(const fb::Block* b) {
  REQUIRE(b->ext() != nullptr);
  return flatbuffers::GetRoot<T>(b->ext()->data());
}

/// Every FrameAnalysis of the first @p max_frames pictures (decode order) of @p stream through the plugin, keyed by
/// decode index, each checked by the validator.
std::map<int32_t, std::vector<uint8_t>> analyse_stream(const std::filesystem::path& stream, int32_t max_frames);

/// The standard output of @p cmd (gzip -dc of a golden).
std::string read_pipe(const std::string& cmd);

}  // namespace de265_test

#endif  // OVT_PLUGIN_DE265_TESTS_TEST_DE265_MAP_SUPPORT_HPP
