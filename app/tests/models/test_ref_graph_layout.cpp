// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the Reference / GOP graph layout (chapter 11 §11.3.11; WI-2.484). Acceptance test 1 names the fixture
// `syn_h265_bpyramid` (chapter 14 §14.3.3 recipe 43), which the oracle image produces; until its index is available
// to the GUI tests, SPEC-ASSUMPTION(#457) the tests build the same shape in memory: a closed GOP of 8 with a
// hierarchical B pyramid (temporal ids 0..3), decode order POC 0 8 4 2 1 3 6 5 7, and the references an HEVC encoder
// writes for it. The assertions compare the layout with the index columns, not with numbers copied from the layout.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "models/frame_table.h"
#include "models/ref_graph_layout.h"
#include "models/stream_index_adapter.h"
#include "ovt_model_generated.h"
#include "synthetic_index.h"

using namespace ovt::app;  // NOLINT(google-build-using-namespace): test file

namespace {

struct PyramidFrame {
  std::int64_t poc;
  int tid;
  ovt::fb::FrameType type;
  std::vector<int> refs;  // decode indices
  std::vector<int> dpb;
};

std::vector<PyramidFrame> bpyramid() {
  using T = ovt::fb::FrameType;
  return {
      {0, 0, T::I, {}, {0}},
      {8, 0, T::P, {0}, {0, 1}},
      {4, 1, T::B, {0, 1}, {0, 1, 2}},
      {2, 2, T::B, {0, 2}, {0, 1, 2, 3}},
      {1, 3, T::B, {0, 3}, {0, 1, 2, 3, 4}},
      {3, 3, T::B, {3, 2}, {1, 2, 3, 4, 5}},
      {6, 2, T::B, {2, 1}, {1, 2, 5, 6}},
      {5, 3, T::B, {2, 6}, {1, 2, 6, 7}},
      {7, 3, T::B, {6, 1}, {1, 6, 8}},
  };
}

std::shared_ptr<const StreamIndexHandle> make_index(const std::vector<PyramidFrame>& frames, bool with_dpb = true) {
  ovt::fb::StreamIndexT index;
  index.codec = ovt::fb::Codec::H265;
  index.schema_version = 1;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto fi = std::make_unique<ovt::fb::FrameInfoT>();
    fi->id = std::make_unique<ovt::fb::FrameId>(static_cast<int>(i), -1);
    fi->pts = frames[i].poc;
    fi->poc = frames[i].poc;
    fi->timebase_num = 1;
    fi->timebase_den = 30;
    fi->size_bytes = 1000;
    fi->is_rap = i == 0;
    fi->shown = true;
    fi->temporal_id = static_cast<std::uint8_t>(frames[i].tid);
    fi->type = frames[i].type;
    fi->declared_refs = frames[i].refs;
    if (with_dpb) {
      fi->dpb_after = frames[i].dpb;
    }
    index.frames.push_back(std::move(fi));
  }
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &index));
  const auto bytes = fbb.GetBufferSpan();
  return StreamIndexHandle::adopt({bytes.begin(), bytes.end()});
}

}  // namespace

TEST_CASE("Rows follow temporal_id and arrows equal declared_refs", "[fast][gui][ref_graph]") {
  const auto frames = bpyramid();
  const auto handle = make_index(frames);
  REQUIRE(handle != nullptr);
  const FrameTable table{handle->index()};

  RefGraphParams params;
  params.axis = RefAxis::Display;
  params.first_column = 0;
  const RefGraphLayout layout = layout_ref_graph(table, params);

  REQUIRE(layout.nodes.size() == frames.size());
  CHECK(layout.rows == 4);
  CHECK(layout.row0_at_bottom);
  for (const RefNode& n : layout.nodes) {
    const auto d = static_cast<std::size_t>(n.decode_index);
    CHECK(n.row == frames[d].tid);
    CHECK(n.temporal_id == frames[d].tid);
    // The display axis: the column is the display slot, which is the POC here (pts == POC, GOP of 8 from 0).
    CHECK(n.column == frames[d].poc);
    CHECK(n.display_index == frames[d].poc);
  }

  std::set<std::pair<int, int>> expected;
  std::size_t expected_count = 0;
  for (std::size_t d = 0; d < frames.size(); ++d) {
    for (const int ref : frames[d].refs) {
      expected.insert({static_cast<int>(d), ref});
      ++expected_count;
    }
  }
  std::set<std::pair<int, int>> actual;
  for (const RefArrow& a : layout.arrows) {
    actual.insert({a.from_decode, a.to_decode});
    CHECK(a.target_in_window);
  }
  CHECK(layout.arrows.size() == expected_count);
  CHECK(actual == expected);
  CHECK(layout.dpb_known);
}

TEST_CASE("The decode axis puts frames at their decode index", "[fast][gui][ref_graph]") {
  const auto frames = bpyramid();
  const auto handle = make_index(frames);
  const FrameTable table{handle->index()};
  RefGraphParams params;
  params.axis = RefAxis::Decode;
  const RefGraphLayout layout = layout_ref_graph(table, params);
  REQUIRE(layout.nodes.size() == frames.size());
  for (const RefNode& n : layout.nodes) {
    CHECK(n.column == n.decode_index);
  }
}

TEST_CASE("Decode-vs-display mode has two rows and mirrors the display slot", "[fast][gui][ref_graph]") {
  const auto frames = bpyramid();
  const auto handle = make_index(frames);
  const FrameTable table{handle->index()};
  RefGraphParams params;
  params.mode = RefRowMode::DecodeVsDisplay;
  const RefGraphLayout layout = layout_ref_graph(table, params);
  CHECK(layout.rows == 2);
  CHECK_FALSE(layout.row0_at_bottom);
  REQUIRE(layout.nodes.size() == 2 * frames.size());
  for (std::size_t d = 0; d < frames.size(); ++d) {
    const RefNode* primary = layout.find(static_cast<int>(d));
    const RefNode* mirror = layout.find(static_cast<int>(d), true);
    REQUIRE(primary != nullptr);
    REQUIRE(mirror != nullptr);
    CHECK(primary->row == 0);
    CHECK(primary->column == static_cast<int>(d));
    CHECK(mirror->row == 1);
    CHECK(mirror->column == frames[d].poc);
  }
  // One arrow per declared reference, drawn between the decode-row nodes only.
  std::size_t refs = 0;
  for (const PyramidFrame& f : frames) {
    refs += f.refs.size();
  }
  CHECK(layout.arrows.size() == refs);
}

TEST_CASE("Flat mode puts every frame in one row", "[fast][gui][ref_graph]") {
  const auto handle = make_index(bpyramid());
  const FrameTable table{handle->index()};
  RefGraphParams params;
  params.mode = RefRowMode::Flat;
  const RefGraphLayout layout = layout_ref_graph(table, params);
  CHECK(layout.rows == 1);
  for (const RefNode& n : layout.nodes) {
    CHECK(n.row == 0);
  }
}

TEST_CASE("Frames whose references the index does not know are marked", "[fast][gui][ref_graph]") {
  auto frames = bpyramid();
  frames[4].refs.clear();  // an inter frame without declared_refs
  const auto handle = make_index(frames);
  const FrameTable table{handle->index()};
  const RefGraphLayout layout = layout_ref_graph(table, RefGraphParams{});
  for (const RefNode& n : layout.nodes) {
    // Only the inter frame #4 is "unknown"; the I frame legitimately has no references.
    CHECK(n.refs_unknown == (n.decode_index == 4));
  }
}

TEST_CASE("The window is clamped and bounded", "[fast][gui][ref_graph]") {
  using namespace ovt::app::testing;
  const auto handle = StreamIndexHandle::adopt(build_index(gop60(300, 1), 1, 30));
  const FrameTable table{handle->index()};
  const int columns = 2 * kRefGraphHalfWindow + 1;
  CHECK(centered_first_column(table, RefAxis::Decode, 0, columns) == 0);
  CHECK(centered_first_column(table, RefAxis::Decode, 150, columns) == 150 - columns / 2);
  CHECK(centered_first_column(table, RefAxis::Decode, 299, columns) == 300 - columns);

  RefGraphParams params;
  params.axis = RefAxis::Decode;
  params.first_column = centered_first_column(table, RefAxis::Decode, 150, columns);
  const RefGraphLayout layout = layout_ref_graph(table, params);
  CHECK(layout.nodes.size() == static_cast<std::size_t>(columns));
  // The frame before the window start references nothing in the window: its arrow is marked.
  const RefNode& first = layout.nodes.front();
  REQUIRE(first.decode_index == params.first_column);
  bool saw_outside = false;
  for (const RefArrow& a : layout.arrows) {
    if (a.to_decode < params.first_column) {
      CHECK_FALSE(a.target_in_window);
      saw_outside = true;
    }
  }
  CHECK(saw_outside);
}

TEST_CASE("Without dpb_after the DPB is reported as not derivable", "[fast][gui][ref_graph]") {
  const auto handle = make_index(bpyramid(), false);
  const FrameTable table{handle->index()};
  CHECK_FALSE(layout_ref_graph(table, RefGraphParams{}).dpb_known);
  CHECK(table.dpb_after(3).empty());
  const auto with = make_index(bpyramid(), true);
  const FrameTable table2{with->index()};
  REQUIRE(table2.dpb_after(2).size() == 3);
  CHECK(table2.dpb_after(2)[2] == 2);
  CHECK(table2.dpb_after(-1).empty());
}

TEST_CASE("Hit testing finds the node under a point", "[fast][gui][ref_graph]") {
  const auto handle = make_index(bpyramid());
  const FrameTable table{handle->index()};
  RefGraphParams params;
  params.axis = RefAxis::Display;
  const RefGraphLayout layout = layout_ref_graph(table, params);
  const RefGraphGeometry g = geometry_of(layout, 640, 240);
  for (const RefNode& n : layout.nodes) {
    const RefNode* hit = node_at(layout, g, g.x_of(n.column, 0), g.y_of(n.row), 4.0);
    REQUIRE(hit != nullptr);
    CHECK(hit->decode_index == n.decode_index);
  }
  CHECK(node_at(layout, g, 5000, 5000, 12.0) == nullptr);
}

TEST_CASE("The frame list and the SVG describe the window", "[fast][gui][ref_graph]") {
  const auto handle = make_index(bpyramid());
  const FrameTable table{handle->index()};
  const RefGraphLayout layout = layout_ref_graph(table, RefGraphParams{});
  const std::string text = frame_list_text(layout);
  CHECK(text.find("#2\t4\tB\t4\t1\t0,1\n") != std::string::npos);
  const std::string svg = ref_graph_svg(layout, 640, 240);
  CHECK(svg.rfind("<svg ", 0) == 0);
  std::size_t rects = 0;
  for (std::size_t at = svg.find("<rect "); at != std::string::npos; at = svg.find("<rect ", at + 1)) {
    ++rects;
  }
  CHECK(rects == layout.nodes.size());
}
