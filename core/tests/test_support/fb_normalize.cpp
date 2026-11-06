// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/fb_normalize.h"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "ovt_container_generated.h"
#include "ovt_metrics_generated.h"
#include "ovt_model_generated.h"

namespace ovt::test {
namespace {

void write_indent(std::string& out, int depth) {
  out.append(static_cast<std::size_t>(depth) * 2U, ' ');
}

void write_canonical(const nlohmann::json& value, std::string& out, int depth) {
  switch (value.type()) {
    case nlohmann::json::value_t::object: {
      if (value.empty()) {
        out += "{}";
        return;
      }
      // nlohmann::json stores objects in a std::map, so iteration is already in sorted key order.
      out += "{\n";
      bool first = true;
      for (const auto& [key, item] : value.items()) {
        if (!first) {
          out += ",\n";
        }
        first = false;
        write_indent(out, depth + 1);
        out += nlohmann::json(key).dump();
        out += ": ";
        write_canonical(item, out, depth + 1);
      }
      out += "\n";
      write_indent(out, depth);
      out += "}";
      return;
    }
    case nlohmann::json::value_t::array: {
      if (value.empty()) {
        out += "[]";
        return;
      }
      out += "[\n";
      bool first = true;
      for (const auto& item : value) {
        if (!first) {
          out += ",\n";
        }
        first = false;
        write_indent(out, depth + 1);
        write_canonical(item, out, depth + 1);
      }
      out += "\n";
      write_indent(out, depth);
      out += "]";
      return;
    }
    case nlohmann::json::value_t::number_float: {
      const double d = value.get<double>();
      if (!std::isfinite(d)) {
        throw std::invalid_argument("normalize_json: non-finite number");
      }
      std::string text = fmt::format("{:.6f}", d);
      if (text == "-0.000000") {
        text = "0.000000";  // one spelling for zero
      }
      out += text;
      return;
    }
    default:  // strings, integers, booleans, null
      out += value.dump();
      return;
  }
}

}  // namespace

std::string normalize_json(std::string_view json) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(json.begin(), json.end());
  } catch (const nlohmann::json::parse_error& e) {
    throw std::invalid_argument(std::string("normalize_json: ") + e.what());
  }
  std::string out;
  write_canonical(parsed, out, 0);
  out += "\n";
  return out;
}

std::optional<std::string> verify_flatbuffer(std::span<const uint8_t> buffer,
                                             std::optional<std::string_view> file_identifier) {
  if (buffer.size() < sizeof(flatbuffers::uoffset_t) || buffer.size() >= FLATBUFFERS_MAX_BUFFER_SIZE) {
    return "buffer size " + std::to_string(buffer.size()) + " is outside the FlatBuffer limits";
  }
  std::string identifier;
  if (file_identifier) {
    if (file_identifier->size() != flatbuffers::kFileIdentifierLength) {
      return "file identifier must have 4 characters";
    }
    identifier = std::string(*file_identifier);
  }
  flatbuffers::Verifier verifier(buffer.data(), buffer.size());
  // VerifyBufferFromStart checks the identifier (when given) and the root offset, then calls back
  // with a pointer to the root table; VerifyTableStart checks its vtable lies inside the buffer.
  struct AnyTable {
    bool Verify(flatbuffers::Verifier& v) const {
      return v.VerifyTableStart(reinterpret_cast<const uint8_t*>(this)) && v.EndTable();
    }
  };
  const bool ok = verifier.VerifyBuffer<AnyTable>(file_identifier ? identifier.c_str() : nullptr);
  if (!ok) {
    return file_identifier ? "not a valid FlatBuffer with identifier '" + identifier + "'"
                           : std::string("not a valid FlatBuffer");
  }
  return std::nullopt;
}

std::vector<uint8_t> fb_normalize(std::span<const uint8_t> buffer) {
  if (auto error = verify_flatbuffer(buffer)) {
    throw std::invalid_argument("fb_normalize: " + *error);
  }
  return {buffer.begin(), buffer.end()};
}

namespace {

using nlohmann::json;

std::string hex_string(const flatbuffers::Vector<uint8_t>* bytes) {
  std::string out;
  if (bytes == nullptr) {
    return out;
  }
  for (const uint8_t b : *bytes) {
    out += fmt::format("{:02x}", b);
  }
  return out;
}

std::string str_of(const flatbuffers::String* s) {
  return s != nullptr ? s->str() : std::string();
}

// ---- syntax-elements / header-struct ------------------------------------------------------------

bool has_value(const fb::SyntaxNode& node) {
  return node.kind() != fb::SyntaxValueKind::None;
}

json node_value(const fb::SyntaxNode& node) {
  switch (node.kind()) {
    case fb::SyntaxValueKind::UInt:
      return node.uvalue();
    case fb::SyntaxValueKind::Int:
    case fb::SyntaxValueKind::Fixed:
      return node.ivalue();
    case fb::SyntaxValueKind::Bool:
      return node.bvalue() ? 1 : 0;
    case fb::SyntaxValueKind::Bytes:
      return hex_string(node.bytes());
    case fb::SyntaxValueKind::String:
      return str_of(node.svalue());
    case fb::SyntaxValueKind::Enum:
      if (node.enum_name() != nullptr && node.enum_name()->size() > 0) {
        return node.enum_name()->str();
      }
      return node.uvalue();
    default:
      return nullptr;
  }
}

void collect_elements(const fb::SyntaxNode& node, uint32_t unit, json& out) {
  if (has_value(node)) {
    const fb::BitSpan* span = node.span();
    out.push_back({{"unit", unit},
                   {"name", str_of(node.name())},
                   {"bit_offset", span != nullptr ? span->bit_offset() : 0U},
                   {"bit_length", span != nullptr ? span->bit_length() : 0U},
                   {"value", node_value(node)}});
  }
  if (node.children() != nullptr) {
    for (const fb::SyntaxNode* child : *node.children()) {
      collect_elements(*child, unit, out);
    }
  }
}

void sort_elements(json& elements) {
  auto& items = elements.get_ref<json::array_t&>();
  std::stable_sort(items.begin(), items.end(), [](const json& a, const json& b) {
    const auto ua = a["unit"].get<uint64_t>();
    const auto ub = b["unit"].get<uint64_t>();
    if (ua != ub) {
      return ua < ub;
    }
    return a["bit_offset"].get<uint64_t>() < b["bit_offset"].get<uint64_t>();
  });
}

// Named members of one JSON object. A key that occurs more than once, or was added with force_array, becomes an array.
class Members {
 public:
  void add(const std::string& key, json value, bool force_array = false) {
    values_[key].push_back(std::move(value));
    if (force_array) {
      forced_.insert(key);
    }
  }
  json build() {
    json object = json::object();
    for (auto& [key, list] : values_) {
      if (list.size() == 1 && forced_.count(key) == 0) {
        object[key] = std::move(list.front());
      } else {
        object[key] = json::array_t(std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()));
      }
    }
    return object;
  }

 private:
  std::map<std::string, std::vector<json>> values_;
  std::set<std::string> forced_;
};

json header_struct_of(const fb::SyntaxNode& node) {
  if (node.children() == nullptr || node.children()->size() == 0) {
    return has_value(node) ? node_value(node) : json::object();
  }
  Members members;
  if (has_value(node)) {
    members.add("value", node_value(node));
  }
  for (const fb::SyntaxNode* child : *node.children()) {
    members.add(str_of(child->name()), header_struct_of(*child), child->array_index() >= 0);
  }
  return members.build();
}

// ---- box-tree -----------------------------------------------------------------------------------

json field_value(const fb::Field& field) {
  if (field.children() != nullptr && field.children()->size() > 0) {
    const bool named = std::any_of(field.children()->begin(), field.children()->end(),
                                   [](const fb::Field* c) { return c->name() != nullptr && c->name()->size() > 0; });
    if (named) {
      Members members;
      for (const fb::Field* child : *field.children()) {
        members.add(str_of(child->name()), field_value(*child));
      }
      return members.build();
    }
    json array = json::array();
    for (const fb::Field* child : *field.children()) {
      array.push_back(field_value(*child));
    }
    return array;
  }
  switch (field.kind()) {
    case fb::SyntaxValueKind::UInt:
    case fb::SyntaxValueKind::Enum:
      return field.uvalue();
    case fb::SyntaxValueKind::Int:
    case fb::SyntaxValueKind::Fixed:
      return field.ivalue();
    case fb::SyntaxValueKind::Bool:
      return field.uvalue() != 0 ? 1 : 0;
    case fb::SyntaxValueKind::Bytes:
      return hex_string(field.bytes());
    case fb::SyntaxValueKind::String:
      return str_of(field.svalue());
    default:
      return nullptr;
  }
}

void collect_boxes(const flatbuffers::Vector<flatbuffers::Offset<fb::BoxNode>>* boxes, const std::string& parent,
                   json& out) {
  if (boxes == nullptr) {
    return;
  }
  std::map<std::string, uint32_t> total;
  for (const fb::BoxNode* box : *boxes) {
    ++total[str_of(box->fourcc())];
  }
  std::map<std::string, uint32_t> seen;
  for (const fb::BoxNode* box : *boxes) {
    const std::string fourcc = str_of(box->fourcc());
    std::string path = parent.empty() ? fourcc : parent + "/" + fourcc;
    if (total[fourcc] > 1) {
      path += "[" + std::to_string(++seen[fourcc]) + "]";
    }
    Members members;
    if (box->fields() != nullptr) {
      for (const fb::Field* field : *box->fields()) {
        members.add(str_of(field->name()), field_value(*field));
      }
    }
    json fields = members.build();
    const fb::ByteSpan* span = box->file_span();
    out.push_back({{"path", path},
                   {"offset", span != nullptr ? span->offset() : 0U},
                   {"size", span != nullptr ? span->length() : 0U},
                   {"fields", std::move(fields)}});
    collect_boxes(box->children(), path, out);
  }
}

// ---- block-grid ---------------------------------------------------------------------------------

template <typename T>
json cell_array(const flatbuffers::Vector<T>* values) {
  json array = json::array();
  if (values != nullptr) {
    for (const T v : *values) {
      array.push_back(v);
    }
  }
  return array;
}

using GridGetter = json (*)(const fb::CellGrid&);

const std::map<std::string, GridGetter>& grid_getters() {
  static const std::map<std::string, GridGetter> getters = {
      {"block_id", [](const fb::CellGrid& g) { return cell_array(g.block_id()); }},
      {"partition_id", [](const fb::CellGrid& g) { return cell_array(g.partition_id()); }},
      {"pred", [](const fb::CellGrid& g) { return cell_array(g.pred()); }},
      {"qp", [](const fb::CellGrid& g) { return cell_array(g.qp()); }},
      {"intra_mode_y", [](const fb::CellGrid& g) { return cell_array(g.intra_mode_y()); }},
      {"intra_mode_c", [](const fb::CellGrid& g) { return cell_array(g.intra_mode_c()); }},
      {"mv0_x", [](const fb::CellGrid& g) { return cell_array(g.mv0_x()); }},
      {"mv0_y", [](const fb::CellGrid& g) { return cell_array(g.mv0_y()); }},
      {"mv1_x", [](const fb::CellGrid& g) { return cell_array(g.mv1_x()); }},
      {"mv1_y", [](const fb::CellGrid& g) { return cell_array(g.mv1_y()); }},
      {"ref0", [](const fb::CellGrid& g) { return cell_array(g.ref0()); }},
      {"ref1", [](const fb::CellGrid& g) { return cell_array(g.ref1()); }},
      {"tx_log2_w", [](const fb::CellGrid& g) { return cell_array(g.tx_log2_w()); }},
      {"tx_log2_h", [](const fb::CellGrid& g) { return cell_array(g.tx_log2_h()); }},
      {"tx_type", [](const fb::CellGrid& g) { return cell_array(g.tx_type()); }},
      {"cbf", [](const fb::CellGrid& g) { return cell_array(g.cbf()); }},
      {"bits_x8", [](const fb::CellGrid& g) { return cell_array(g.bits_x8()); }},
      {"deblock_bs_v", [](const fb::CellGrid& g) { return cell_array(g.deblock_bs_v()); }},
      {"deblock_bs_h", [](const fb::CellGrid& g) { return cell_array(g.deblock_bs_h()); }},
      {"filter_idx", [](const fb::CellGrid& g) { return cell_array(g.filter_idx()); }},
  };
  return getters;
}

// ---- scores -------------------------------------------------------------------------------------

std::string score_key(const std::string& metric, std::size_t plane, std::size_t count) {
  if (count <= 1) {
    return metric;
  }
  static constexpr const char* kPlanes[] = {"_y", "_u", "_v"};
  return plane < 3 ? metric + kPlanes[plane] : metric + "_" + std::to_string(plane);
}

void add_pooled(json& pooled, const std::string& metric, const char* stat, const flatbuffers::Vector<double>* values) {
  if (values == nullptr) {
    return;
  }
  for (std::size_t i = 0; i < values->size(); ++i) {
    pooled[score_key(metric, i, values->size())][stat] = (*values)[static_cast<flatbuffers::uoffset_t>(i)];
  }
}

void require_family(Family actual, Family expected, const char* what) {
  if (actual != expected) {
    throw std::invalid_argument(std::string("to_canonical: ") + what + " cannot produce family " +
                                std::string(family_name(actual)));
  }
}

}  // namespace

std::string_view family_name(Family family) {
  switch (family) {
    case Family::kSyntaxElements:
      return "syntax-elements";
    case Family::kHeaderStruct:
      return "header-struct";
    case Family::kBlockGrid:
      return "block-grid";
    case Family::kBoxTree:
      return "box-tree";
    case Family::kSampleTable:
      return "sample-table";
    case Family::kScores:
      return "scores";
  }
  return "unknown";
}

std::optional<Family> family_from_name(std::string_view name) {
  for (const Family f : {Family::kSyntaxElements, Family::kHeaderStruct, Family::kBlockGrid, Family::kBoxTree,
                         Family::kSampleTable, Family::kScores}) {
    if (family_name(f) == name) {
      return f;
    }
  }
  return std::nullopt;
}

json to_canonical(const fb::SyntaxNode& header, Family family, uint32_t unit_index) {
  if (family == Family::kSyntaxElements) {
    json elements = json::array();
    collect_elements(header, unit_index, elements);
    sort_elements(elements);
    return elements;
  }
  require_family(family, Family::kHeaderStruct, "a SyntaxNode");
  Members members;
  members.add(str_of(header.name()), header_struct_of(header));
  return members.build();
}

json to_canonical(const fb::StreamIndex& index, Family family) {
  if (family == Family::kSyntaxElements) {
    json elements = json::array();
    if (index.units() != nullptr) {
      for (const fb::Unit* unit : *index.units()) {
        if (unit->header() != nullptr) {
          collect_elements(*unit->header(), unit->index(), elements);
        }
      }
    }
    sort_elements(elements);
    return elements;
  }
  require_family(family, Family::kHeaderStruct, "a StreamIndex");
  Members members;
  if (index.units() != nullptr) {
    for (const fb::Unit* unit : *index.units()) {
      if (unit->header() != nullptr) {
        members.add(str_of(unit->header()->name()), header_struct_of(*unit->header()));
      }
    }
  }
  return members.build();
}

json to_canonical(const fb::ContainerTree& tree, Family family) {
  require_family(family, Family::kBoxTree, "a ContainerTree");
  json boxes = json::array();
  collect_boxes(tree.root(), std::string(), boxes);
  return boxes;
}

json to_canonical(const fb::SampleTable& table, Family family) {
  require_family(family, Family::kSampleTable, "a SampleTable");
  json rows = json::array();
  const flatbuffers::uoffset_t count = table.dts() != nullptr ? table.dts()->size() : 0U;
  const auto at_u32 = [&](const flatbuffers::Vector<uint32_t>* v, flatbuffers::uoffset_t i) -> uint64_t {
    return v != nullptr && i < v->size() ? (*v)[i] : 0U;
  };
  for (flatbuffers::uoffset_t i = 0; i < count; ++i) {
    const int64_t dts = (*table.dts())[i];
    const int64_t cts_offset =
        table.cts_offset() != nullptr && i < table.cts_offset()->size() ? (*table.cts_offset())[i] : 0;
    const uint64_t offset = table.offset() != nullptr && i < table.offset()->size() ? (*table.offset())[i] : 0U;
    const bool sync = table.is_sync() != nullptr && i < table.is_sync()->size() && (*table.is_sync())[i] != 0;
    rows.push_back({{"i", i},
                    {"dts", dts},
                    {"cts", dts + cts_offset},
                    {"size", at_u32(table.size(), i)},
                    {"offset", offset},
                    {"sync", sync ? 1 : 0}});
  }
  return rows;
}

json to_canonical(const fb::FrameAnalysis& analysis, Family family, const std::vector<std::string>& fields) {
  require_family(family, Family::kBlockGrid, "a FrameAnalysis");
  const fb::CellGrid* grid = analysis.grid();
  json out = {{"w4", grid != nullptr ? grid->cells_w() : 0U},
              {"h4", grid != nullptr ? grid->cells_h() : 0U},
              {"fields", json::object()}};
  if (grid == nullptr) {
    return out;
  }
  if (fields.empty()) {
    for (const auto& [name, getter] : grid_getters()) {
      json values = getter(*grid);
      if (name != "block_id" && !values.empty()) {
        out["fields"][name] = std::move(values);
      }
    }
    return out;
  }
  for (const std::string& name : fields) {
    const auto it = grid_getters().find(name);
    if (it == grid_getters().end()) {
      throw std::invalid_argument("to_canonical: unknown CellGrid field '" + name + "'");
    }
    out["fields"][name] = it->second(*grid);
  }
  return out;
}

json to_canonical(const fb::MetricsResult& result, Family family) {
  require_family(family, Family::kScores, "a MetricsResult");
  const std::string metric = result.descriptor() != nullptr ? str_of(result.descriptor()->id()) : std::string();
  json frames = json::array();
  if (result.frames() != nullptr) {
    for (const fb::FrameScore* score : *result.frames()) {
      json row = {{"i", score->frame() != nullptr ? score->frame()->display_index() : 0}};
      if (score->values() != nullptr) {
        for (std::size_t p = 0; p < score->values()->size(); ++p) {
          row[score_key(metric, p, score->values()->size())] =
              (*score->values())[static_cast<flatbuffers::uoffset_t>(p)];
        }
      }
      frames.push_back(std::move(row));
    }
  }
  json pooled = json::object();
  add_pooled(pooled, metric, "mean", result.pooled_mean());
  add_pooled(pooled, metric, "harmonic_mean", result.pooled_harmonic());
  add_pooled(pooled, metric, "min", result.pooled_min());
  return {{"frames", std::move(frames)}, {"pooled", std::move(pooled)}};
}

}  // namespace ovt::test
