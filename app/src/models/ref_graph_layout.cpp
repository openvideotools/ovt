// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/ref_graph_layout.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "models/timeline_geometry.h"

namespace ovt::app {

namespace {

constexpr double kRightMargin = 8.0;
constexpr double kMaxNodeSize = 18.0;

bool is_intra(const FrameTable& table, int decode) {
  const fb::FrameType t = table.type(decode);
  return table.is_rap(decode) || t == fb::FrameType::I || t == fb::FrameType::Key || t == fb::FrameType::IntraOnly;
}

int decode_at_column(const FrameTable& table, RefAxis axis, int column) {
  if (column < 0) {
    return -1;
  }
  if (axis == RefAxis::Decode) {
    return static_cast<std::size_t>(column) < table.size() ? column : -1;
  }
  return table.decode_index_of_display(column);
}

RefNode make_node(const FrameTable& table, int decode, int column, int row, bool mirror) {
  RefNode n;
  n.decode_index = decode;
  n.display_index = table.display_index_of(decode);
  n.column = column;
  n.row = row;
  n.poc = table.poc(decode);
  n.type = table.type(decode);
  n.temporal_id = table.temporal_id(decode);
  n.is_rap = table.is_rap(decode);
  n.shown = table.shown(decode);
  n.refs_unknown = !is_intra(table, decode) && table.declared_refs(decode).empty();
  n.mirror = mirror;
  return n;
}

std::string hex_color(Rgba c) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string out = "#";
  for (const std::uint8_t v : {c.r, c.g, c.b}) {
    out += digits[v >> 4U];
    out += digits[v & 0x0FU];
  }
  return out;
}

// A number with two decimals ("12.50"), without printf.
std::string num(double v) {
  const auto scaled = static_cast<long long>(std::llround(std::abs(v) * 100.0));
  std::string out = v < 0 && scaled != 0 ? "-" : "";
  out += std::to_string(scaled / 100) + ".";
  const long long frac = scaled % 100;
  out += frac < 10 ? "0" : "";
  out += std::to_string(frac);
  return out;
}

}  // namespace

const RefNode* RefGraphLayout::find(int decode_index, bool mirror) const noexcept {
  for (const RefNode& n : nodes) {
    if (n.decode_index == decode_index && n.mirror == mirror) {
      return &n;
    }
  }
  return nullptr;
}

int centered_first_column(const FrameTable& table, RefAxis axis, int decode_index, int columns) {
  const int total = static_cast<int>(table.size());
  int centre = axis == RefAxis::Display ? table.display_index_of(decode_index) : decode_index;
  centre = std::max(centre, 0);
  return std::clamp(centre - columns / 2, 0, std::max(0, total - columns));
}

RefGraphLayout layout_ref_graph(const FrameTable& table, const RefGraphParams& params) {
  RefGraphLayout out;
  out.params = params;
  const RefAxis axis = params.mode == RefRowMode::DecodeVsDisplay ? RefAxis::Decode : params.axis;
  out.params.axis = axis;
  out.row0_at_bottom = params.mode == RefRowMode::TemporalLayers;
  const int first = std::max(params.first_column, 0);
  const int last = first + std::max(params.columns, 0);  // exclusive

  int max_tid = 0;
  for (int c = first; c < last; ++c) {
    const int d = decode_at_column(table, axis, c);
    if (d < 0) {
      continue;
    }
    const int tid = table.temporal_id(d);
    const int row = params.mode == RefRowMode::TemporalLayers ? tid : 0;
    max_tid = std::max(max_tid, row);
    out.nodes.push_back(make_node(table, d, c, row, false));
    if (!table.dpb_after(d).empty()) {
      out.dpb_known = true;
    }
  }
  if (params.mode == RefRowMode::DecodeVsDisplay) {
    out.rows = 2;
    const std::size_t primaries = out.nodes.size();
    for (std::size_t i = 0; i < primaries; ++i) {
      const int d = out.nodes[i].decode_index;
      const int slot = table.display_index_of(d);
      if (slot >= first && slot < last) {
        out.nodes.push_back(make_node(table, d, slot, 1, true));
      }
    }
  } else {
    out.rows = max_tid + 1;
  }

  for (const RefNode& n : out.nodes) {
    if (n.mirror) {
      continue;
    }
    for (const int ref : table.declared_refs(n.decode_index)) {
      const int target_column = axis == RefAxis::Display ? table.display_index_of(ref) : ref;
      out.arrows.push_back(RefArrow{n.decode_index, ref, target_column >= first && target_column < last});
    }
  }
  return out;
}

double RefGraphGeometry::cell_width() const noexcept {
  return std::max(1.0, (width - margin_left - kRightMargin) / std::max(columns, 1));
}

double RefGraphGeometry::row_height() const noexcept {
  return std::max(1.0, (height - margin_top - margin_bottom) / std::max(rows, 1));
}

double RefGraphGeometry::x_of(int column, int first_column) const noexcept {
  return margin_left + (static_cast<double>(column - first_column) + 0.5) * cell_width();
}

double RefGraphGeometry::y_of(int row) const noexcept {
  const int slot = row0_at_bottom ? rows - 1 - row : row;
  return margin_top + (static_cast<double>(slot) + 0.5) * row_height();
}

RefGraphGeometry geometry_of(const RefGraphLayout& layout, double width, double height) {
  RefGraphGeometry g;
  g.width = width;
  g.height = height;
  g.columns = layout.params.columns;
  g.rows = layout.rows;
  g.row0_at_bottom = layout.row0_at_bottom;
  return g;
}

const RefNode* node_at(const RefGraphLayout& layout, const RefGraphGeometry& geometry, double x, double y,
                       double radius) {
  const RefNode* best = nullptr;
  double best_d2 = radius * radius;
  for (const RefNode& n : layout.nodes) {
    const double dx = geometry.x_of(n.column, layout.params.first_column) - x;
    const double dy = geometry.y_of(n.row) - y;
    const double d2 = dx * dx + dy * dy;
    if (d2 <= best_d2) {
      best_d2 = d2;
      best = &n;
    }
  }
  return best;
}

const char* frame_type_label(fb::FrameType type) noexcept {
  switch (type) {
    case fb::FrameType::I:
      return "I";
    case fb::FrameType::P:
      return "P";
    case fb::FrameType::B:
      return "B";
    case fb::FrameType::Key:
      return "K";
    case fb::FrameType::Inter:
      return "P";
    case fb::FrameType::IntraOnly:
      return "i";
    case fb::FrameType::Switch:
      return "S";
    case fb::FrameType::Unknown:
      break;
  }
  return "?";
}

std::string frame_list_text(const RefGraphLayout& layout) {
  std::string out;
  for (const RefNode& n : layout.nodes) {
    if (n.mirror) {
      continue;
    }
    out += "#" + std::to_string(n.decode_index) + "\t" + std::to_string(n.display_index) + "\t" +
           frame_type_label(n.type) + "\t" + std::to_string(n.poc) + "\t" + std::to_string(n.temporal_id) + "\t";
    bool first = true;
    for (const RefArrow& a : layout.arrows) {
      if (a.from_decode == n.decode_index) {
        out += (first ? "" : ",") + std::to_string(a.to_decode);
        first = false;
      }
    }
    out += "\n";
  }
  return out;
}

std::string ref_graph_svg(const RefGraphLayout& layout, double width, double height) {
  const RefGraphGeometry g = geometry_of(layout, width, height);
  const int first = layout.params.first_column;
  const double size = std::min({g.cell_width() * 0.8, g.row_height() * 0.8, kMaxNodeSize});
  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" + num(width) + R"(" height=")" + num(height) +
                    R"(" viewBox="0 0 )" + num(width) + " " + num(height) + "\">\n";
  for (const RefArrow& a : layout.arrows) {
    const RefNode* from = layout.find(a.from_decode);
    const RefNode* to = layout.find(a.to_decode);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    const double x0 = g.x_of(from->column, first);
    const double y0 = g.y_of(from->row);
    const double x1 = g.x_of(to->column, first);
    const double y1 = g.y_of(to->row);
    const double lift = std::min(std::abs(x1 - x0) * 0.35 + 6.0, g.row_height());
    svg += R"(<path d="M)" + num(x0) + " " + num(y0) + " Q" + num((x0 + x1) / 2) + " " + num(std::min(y0, y1) - lift) +
           " " + num(x1) + " " + num(y1) + R"(" fill="none" stroke="#FB8C00" stroke-width="1"/>)" + "\n";
  }
  for (const RefNode& n : layout.nodes) {
    const double cx = g.x_of(n.column, first);
    const double cy = g.y_of(n.row);
    const std::string type_hex = hex_color(frame_type_color(n.type));
    svg += R"(<rect x=")" + num(cx - size / 2) + R"(" y=")" + num(cy - size / 2) + R"(" width=")" + num(size) +
           R"(" height=")" + num(size) + R"(" rx="2" fill=")" + (n.refs_unknown ? std::string("none") : type_hex) +
           R"(" stroke=")" + type_hex + R"("><title>#)" + std::to_string(n.decode_index) + " " +
           frame_type_label(n.type) + " POC " + std::to_string(n.poc) + "</title></rect>\n";
  }
  svg += "</svg>\n";
  return svg;
}

}  // namespace ovt::app
