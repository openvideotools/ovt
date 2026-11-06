// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/fb_normalize.h"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ovt_container_generated.h"
#include "ovt_model_generated.h"

namespace ovt::test {
namespace {

// True for a null or zero-length FlatBuffers string/vector. Uses size() because the apt
// flatbuffers 2.0.8 used by local builds has no empty().
template <typename T>
bool fb_empty(const T* v) {
  return v == nullptr || v->size() == 0;  // NOLINT(readability-container-size-empty)
}

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
      if (!fb_empty(node.enum_name())) {
        return node.enum_name()->str();
      }
      return node.uvalue();
    default:
      return nullptr;
  }
}

// SPEC-ASSUMPTION(#111): bit_offset/bit_length are written as the schema's BitSpan holds them (counted from the unit
// start). §14.4.0 counts syntax-elements from the RBSP (H.26x) or OBU payload (AV1) start; converting between the two
// bases is deferred to the first real syntax golden, WI-1.570.
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
      if (list.size() == 1 && !forced_.contains(key)) {
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
  if (fb_empty(node.children())) {
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
  if (!fb_empty(field.children())) {
    const bool named = std::any_of(field.children()->begin(), field.children()->end(),
                                   [](const fb::Field* c) { return !fb_empty(c->name()); });
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
    std::string path = parent;
    if (!parent.empty()) {
      path += '/';
    }
    path += fourcc;
    if (total[fourcc] > 1) {
      path += '[';
      path += std::to_string(++seen[fourcc]);
      path += ']';
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

}  // namespace ovt::test
