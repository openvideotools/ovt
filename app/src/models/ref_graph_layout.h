// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout of the Reference / GOP graph (chapter 11 §11.3.11; WI-2.484): which frames fall in the window, in which
// row and column each node sits, which arrows join them, and the SVG of the result. Everything comes from the
// StreamIndex columns of the FrameTable (`temporal_id`, `declared_refs`, `dpb_after`), never from decoded data. No Qt
// types, so the arithmetic is unit-testable without a scene.

#ifndef OVT_APP_MODELS_REF_GRAPH_LAYOUT_H
#define OVT_APP_MODELS_REF_GRAPH_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "models/frame_table.h"
#include "ovt_model_generated.h"

namespace ovt::app {

// The row modes of §11.3.11: temporal layers (default), decode-vs-display (two rows), flat (one row).
enum class RefRowMode : std::uint8_t { TemporalLayers, DecodeVsDisplay, Flat };
// What the x axis counts: decode index or display slot.
enum class RefAxis : std::uint8_t { Decode, Display };

// The window is `columns` columns of the x axis starting at `first_column` (§11.3.11: ±64 frames).
inline constexpr int kRefGraphHalfWindow = 64;

struct RefGraphParams {
  RefRowMode mode = RefRowMode::TemporalLayers;
  RefAxis axis = RefAxis::Display;
  int first_column = 0;
  int columns = 2 * kRefGraphHalfWindow + 1;
};

struct RefNode {
  int decode_index = -1;
  int display_index = -1;
  int column = 0;  // x slot on the chosen axis (row 1 of DecodeVsDisplay uses the display slot)
  int row = 0;     // see RefGraphLayout::rows; row 0 is the temporal layer 0 / the decode row / the only row
  std::int64_t poc = 0;
  fb::FrameType type = fb::FrameType::Unknown;
  int temporal_id = 0;
  bool is_rap = false;
  bool shown = true;
  // An inter frame whose headers list no references: the index does not know them (§11.3.11 pitfall), so the node is
  // drawn open instead of claiming an intra frame.
  bool refs_unknown = false;
  // DecodeVsDisplay only: the copy of the frame in the display row (the primary node is in the decode row).
  bool mirror = false;
};

struct RefArrow {
  int from_decode = -1;  // the referencing frame
  int to_decode = -1;    // the frame it refers to (`declared_refs`)
  bool target_in_window = false;
};

struct RefGraphLayout {
  RefGraphParams params;
  std::vector<RefNode> nodes;    // primary nodes first, in column order; then the mirror nodes
  std::vector<RefArrow> arrows;  // one per declared reference of each primary node, in node then list order
  int rows = 1;
  // TemporalLayers draws row 0 at the bottom (T0 lowest); the other modes draw row 0 at the top.
  bool row0_at_bottom = false;
  // Whether any frame in the window has a `dpb_after` list.
  bool dpb_known = false;
  [[nodiscard]] const RefNode* find(int decode_index, bool mirror = false) const noexcept;
};

// The window that keeps `decode_index` in the middle (clamped to the stream), as params.first_column.
[[nodiscard]] int centered_first_column(const FrameTable& table, RefAxis axis, int decode_index, int columns);

[[nodiscard]] RefGraphLayout layout_ref_graph(const FrameTable& table, const RefGraphParams& params);

// Pixel geometry of a layout inside a `width` × `height` box.
struct RefGraphGeometry {
  double width = 0;
  double height = 0;
  int columns = 1;
  int rows = 1;
  bool row0_at_bottom = false;
  double margin_left = 34;
  double margin_top = 10;
  double margin_bottom = 22;
  [[nodiscard]] double cell_width() const noexcept;
  [[nodiscard]] double row_height() const noexcept;
  [[nodiscard]] double x_of(int column, int first_column) const noexcept;
  [[nodiscard]] double y_of(int row) const noexcept;
};
[[nodiscard]] RefGraphGeometry geometry_of(const RefGraphLayout& layout, double width, double height);

// The node under a point, preferring the nearest centre within `radius`; nullptr for none.
[[nodiscard]] const RefNode* node_at(const RefGraphLayout& layout, const RefGraphGeometry& geometry, double x, double y,
                                     double radius);

// One line per frame in the window: "#decode<TAB>display<TAB>type<TAB>poc<TAB>tid<TAB>refs (decode indices, comma)".
[[nodiscard]] std::string frame_list_text(const RefGraphLayout& layout);

// "Export graph as SVG" (§11.3.11): the nodes as rects coloured by frame type, the arrows as quadratic curves.
[[nodiscard]] std::string ref_graph_svg(const RefGraphLayout& layout, double width, double height);

// The label of a frame type in the node ("I", "P", "B", ...).
[[nodiscard]] const char* frame_type_label(fb::FrameType type) noexcept;

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_REF_GRAPH_LAYOUT_H
