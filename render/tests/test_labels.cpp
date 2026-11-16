// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-2.433a (chapter 10 §10.11): the SDF glyph atlas (§10.5.1) and the block label strings (§10.5.5). The label pass
// that draws them, its goldens and the sample-value text of R-ZOOM-3 follow in WI-2.433b.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fonts/dejavu_sans_mono_sdf.h"
#include "overlay/label_text.h"
#include "overlay/overlay_set_impl.h"
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "ovt_model_generated.h"

namespace {

namespace fb = ovt::fb;
using ovt::render::LabelContent;
using ovt::render::OverlayBuilder;
using ovt::render::OverlayConfig;
using ovt::render::OverlayLayer;
using ovt::render::VideoSurface;
using ovt::render::detail::LabelCodec;

// A 32 x 32 HEVC-like picture: CTU 0 splits into three 16 x 16 CUs (ids 1-3) and a fourth (id 4) that splits into
// four 8 x 8 CUs (ids 5-8). Seven leaves; QP 20 + 4 x leaf rank.
constexpr int kPic = 32;
constexpr std::array<std::array<int, 4>, 9> kRects = {{{0, 0, 32, 32},
                                                       {0, 0, 16, 16},
                                                       {16, 0, 16, 16},
                                                       {0, 16, 16, 16},
                                                       {16, 16, 16, 16},
                                                       {16, 16, 8, 8},
                                                       {24, 16, 8, 8},
                                                       {16, 24, 8, 8},
                                                       {24, 24, 8, 8}}};
constexpr std::array<int, 9> kQp = {0, 20, 24, 28, 0, 32, 36, 40, 51};

uint32_t leaf_at(int x, int y) {
  for (uint32_t id = 8; id > 0; --id) {
    const auto& r = kRects.at(id);
    if (id != 4 && x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3]) {
      return id;
    }
  }
  return 0;
}

std::unique_ptr<fb::CellGridT> make_labels_grid() {
  auto grid = std::make_unique<fb::CellGridT>();
  grid->cells_w = kPic / 4;
  grid->cells_h = kPic / 4;
  for (int cy = 0; cy < kPic / 4; ++cy) {
    for (int cx = 0; cx < kPic / 4; ++cx) {
      const uint32_t id = leaf_at(cx * 4, cy * 4);
      const bool intra = id == 3;
      grid->block_id.push_back(id);
      grid->qp.push_back(static_cast<int16_t>(kQp.at(id)));
      grid->pred.push_back(static_cast<uint8_t>(intra ? fb::PredKind::Intra : fb::PredKind::Inter));
      grid->intra_mode_y.push_back(static_cast<int16_t>(intra ? 26 : -1));
      grid->tx_type.push_back(static_cast<uint8_t>(intra ? 1 : 0));
      grid->ref0.push_back(static_cast<int8_t>(intra ? -1 : 0));
      grid->ref1.push_back(static_cast<int8_t>(id == 2 ? 1 : -1));
    }
  }
  return grid;
}

std::vector<uint8_t> make_labels_fixture() {
  auto fa = std::make_unique<fb::FrameAnalysisT>();
  fa->schema_version = 1;
  fa->plugin_id = "ovt.plugin.hm";
  fa->grid = make_labels_grid();
  for (uint32_t id = 0; id < kRects.size(); ++id) {
    auto b = std::make_unique<fb::BlockT>();
    b->id = id;
    b->parent = id == 0 ? -1 : (id <= 4 ? 0 : 4);
    if (id == 0) {
      b->children = {1, 2, 3, 4};
    } else if (id == 4) {
      b->children = {5, 6, 7, 8};
    }
    b->kind = id == 0 ? fb::BlockKind::CTU : fb::BlockKind::CU;
    const auto& r = kRects.at(id);
    b->rect = std::make_unique<fb::Rect>(r[0], r[1], r[2], r[3]);
    b->pred = id == 3 ? fb::PredKind::Intra : fb::PredKind::Inter;
    b->qp = static_cast<int16_t>(kQp.at(id));
    b->bits_x8 = 8 * 312;
    b->ext_type = "HevcCu";
    fa->blocks.push_back(std::move(b));
  }
  const auto add_mv = [&](uint32_t block, uint8_t list, int16_t x, int16_t y) {
    auto mv = std::make_unique<fb::MotionVectorT>();
    mv->block_id = block;
    mv->list = list;
    mv->mv_x = x;
    mv->mv_y = y;
    fa->mvs.push_back(std::move(mv));
  };
  add_mv(1, 0, -48, 12);  // 1/16 units: (-12, +3) in quarter samples
  add_mv(2, 0, 16, 0);
  add_mv(2, 1, -4, -8);  // floor(-4 / 4) = -1, floor(-8 / 4) = -2
  flatbuffers::FlatBufferBuilder fbb;
  fb::FinishFrameAnalysisBuffer(fbb, fb::FrameAnalysis::Pack(fbb, fa.get()));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

constexpr uint32_t kLabels = static_cast<uint32_t>(OverlayLayer::Labels);

std::unique_ptr<ovt::render::OverlaySet> build_labels(VideoSurface* surface, LabelContent content) {
  const std::vector<uint8_t> fa = make_labels_fixture();
  OverlayConfig cfg;
  cfg.layers = kLabels;
  cfg.label = content;
  // OverlayBuilder only keeps the surface reference; the CPU build never touches it.
  std::unique_ptr<VideoSurface> dummy;
  if (surface == nullptr) {
    dummy = VideoSurface::create("labels-cpu");
    surface = dummy.get();
  }
  OverlayBuilder builder(*surface);
  auto set = builder.build(fa.data(), fa.size(), cfg);
  REQUIRE(set != nullptr);
  return set;
}

std::vector<std::string> texts(const ovt::render::OverlaySet& set) {
  std::vector<std::string> out;
  out.reserve(set.block_labels.size());
  for (const auto& l : set.block_labels) {
    out.push_back(l.text);
  }
  return out;
}

}  // namespace

TEST_CASE("render_test_labels: atlas layout (§10.5.1)", "[fast][render][overlay]") {
  namespace font = ovt::render::fonts;
  CHECK(font::kSdfFontPx == 48);
  CHECK(font::kSdfSpread == 8);
  CHECK(font::kSdfAtlasWidth == 1024);
  CHECK(font::kSdfGlyphCount == 95 + 6);
  CHECK(font::kSdfAtlas.size() == size_t{font::kSdfAtlasWidth} * font::kSdfAtlasHeight);
  CHECK(font::kSdfCellW * font::kSdfColumns <= font::kSdfAtlasWidth);
  CHECK(((font::kSdfGlyphCount + font::kSdfColumns - 1) / font::kSdfColumns) * font::kSdfCellH <=
        font::kSdfAtlasHeight);
  // The space glyph is empty: no texel of its cell is inside the outline (byte > 127).
  bool inked = false;
  for (int y = 0; y < font::kSdfCellH; ++y) {
    for (int x = 0; x < font::kSdfCellW; ++x) {
      inked =
          inked || font::kSdfAtlas.at((static_cast<size_t>(y) * font::kSdfAtlasWidth) + static_cast<size_t>(x)) > 127;
    }
  }
  CHECK_FALSE(inked);
}

TEST_CASE("render_test_labels: label strings (§10.5.5)", "[fast][render][overlay]") {
  CHECK(texts(*build_labels(nullptr, LabelContent::QP)) ==
        std::vector<std::string>{"20", "24", "28", "32", "36", "40", "51"});
  CHECK(texts(*build_labels(nullptr, LabelContent::BlockSize)) ==
        std::vector<std::string>{"16x16", "16x16", "16x16", "8x8", "8x8", "8x8", "8x8"});
  CHECK(texts(*build_labels(nullptr, LabelContent::PredMode)) ==
        std::vector<std::string>{"P", "B", "I", "P", "P", "P", "P"});
  CHECK(texts(*build_labels(nullptr, LabelContent::Bits)).front() == "312b");
  CHECK(texts(*build_labels(nullptr, LabelContent::BlockId)).back() == "8");
  const auto mvs = texts(*build_labels(nullptr, LabelContent::MvValues));
  REQUIRE(mvs.size() == 2);
  CHECK(mvs[0] == "L0 −12,+3");
  CHECK(mvs[1] == "L0 +4,0\nL1 −1,−2");
  CHECK(texts(*build_labels(nullptr, LabelContent::IntraMode)) == std::vector<std::string>{"ANG 26"});
  CHECK(texts(*build_labels(nullptr, LabelContent::TxType)).at(2) == "DST7");
  CHECK(texts(*build_labels(nullptr, LabelContent::RefIdx)).at(1) == "L0[0]\nL1[1]");

  using ovt::render::detail::intra_mode_label;
  using ovt::render::detail::tx_type_label;
  CHECK(intra_mode_label(LabelCodec::H265, 0) == "PLANAR");
  CHECK(intra_mode_label(LabelCodec::H265, 1) == "DC");
  CHECK(intra_mode_label(LabelCodec::Av1, 3) == "D45_PRED");
  CHECK(intra_mode_label(LabelCodec::H264, 18) == "I16 DC");
  CHECK(intra_mode_label(LabelCodec::Unknown, 7) == "7");
  CHECK(tx_type_label(LabelCodec::H265, 0) == "DCT2");
  CHECK(tx_type_label(LabelCodec::Av1, 7) == "ADST_FLIPADST");
  CHECK(tx_type_label(LabelCodec::Av1, 99) == "99");
}
