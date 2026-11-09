// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// NormalizationConfig validation, default expansion and canonical form (chapter 9 §9.2.4, WI-3.300 step 5).
// The schema is the single source: defaults are read from it, so a schema change needs no code change here.

#include "metrics/normalize/normalization_config.h"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "metrics/normalize/normalization_schema.inc"  // kNormalizationConfigSchemaText (configure time)

namespace ovt::metrics {

namespace {

using nlohmann::json;

// Dotted path of a JSON pointer ("/scale/kernel" -> "scale.kernel"; "" -> "<root>").
std::string dotted(const json::json_pointer& ptr) {
  const std::string s = ptr.to_string();
  if (s.empty()) {
    return "<root>";
  }
  std::string out;
  for (const char c : s.substr(1)) {
    out += (c == '/') ? '.' : c;
  }
  return out;
}

// Collects the first validation error with its dotted path.
class FirstError : public nlohmann::json_schema::error_handler {
 public:
  void error(const json::json_pointer& ptr, const json& /*instance*/, const std::string& message) override {
    if (message_.empty()) {
      message_ = dotted(ptr) + ": " + message;
    }
  }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

 private:
  std::string message_;
};

const json& schema_document() {
  static const json schema = json::parse(kNormalizationConfigSchemaText);
  return schema;
}

const nlohmann::json_schema::json_validator& validator() {
  static const nlohmann::json_schema::json_validator v(schema_document());
  return v;
}

// Resolves a local "$ref" ("#/$defs/rect") inside the schema document.
const json& deref(const json& node) {
  if (node.is_object() && node.contains("$ref")) {
    const std::string ref = node.at("$ref").get<std::string>();
    if (!ref.empty() && ref.front() == '#') {
      return schema_document().at(json::json_pointer(ref.substr(1)));
    }
  }
  return node;
}

bool declares_type(const json& node, std::string_view type) {
  if (!node.contains("type")) {
    return false;
  }
  const json& t = node.at("type");
  if (t.is_string()) {
    return t.get_ref<const std::string&>() == type;
  }
  if (t.is_array()) {
    for (const auto& e : t) {
      if (e.is_string() && e.get_ref<const std::string&>() == type) {
        return true;
      }
    }
  }
  return false;
}

// Fills `value` (an object validated against `node`) with the defaults of `node`'s properties, recursing into
// object-typed properties, which are created when absent. A `const` counts as the default. A property that is
// nullable (type includes "null") and has no default resolves to null.
// NOLINTNEXTLINE(misc-no-recursion): the schema nests objects a fixed two levels deep
void expand_defaults(const json& node_in, json& value) {
  const json& node = deref(node_in);
  if (!node.contains("properties") || !value.is_object()) {
    return;
  }
  for (const auto& [name, prop_in] : node.at("properties").items()) {
    const json& prop = deref(prop_in);
    if (!value.contains(name)) {
      if (prop.contains("default")) {
        value[name] = prop.at("default");
      } else if (prop.contains("const")) {
        value[name] = prop.at("const");
      } else if (declares_type(prop, "object") && !declares_type(prop, "null")) {
        value[name] = json::object();
      } else if (declares_type(prop, "null")) {
        value[name] = nullptr;
      } else {
        continue;  // no resolvable value: stays absent (SPEC-ASSUMPTION(#290))
      }
    }
    if (value[name].is_object()) {
      expand_defaults(prop, value[name]);
    }
  }
}

}  // namespace

std::string_view normalization_config_schema_text() noexcept {
  return kNormalizationConfigSchemaText;
}

Result<json> resolve_normalization_config(std::string_view text) {
  json value;
  if (text.empty()) {
    value = json::object();
  } else {
    try {
      value = json::parse(text);
    } catch (const json::exception& e) {
      return Result<json>::error(OVT_E_INVALID_ARG, std::string("NormalizationConfig is not valid JSON: ") + e.what());
    }
  }
  if (!value.is_object()) {
    return Result<json>::error(OVT_E_INVALID_ARG, "<root>: NormalizationConfig must be a JSON object");
  }
  FirstError err;
  try {
    (void)validator().validate(value, err);
  } catch (const std::exception& e) {
    return Result<json>::error(OVT_E_INVALID_ARG, std::string("NormalizationConfig: ") + e.what());
  }
  if (!err.message().empty()) {
    return Result<json>::error(OVT_E_INVALID_ARG, "NormalizationConfig " + err.message());
  }
  expand_defaults(schema_document(), value);
  return Result<json>::ok(std::move(value));
}

std::string canonical_normalization_json(const json& resolved) {
  // nlohmann::json (std::map object storage) keeps keys sorted; dump(-1) writes no whitespace and the shortest
  // round-trip form for doubles (Grisu2), integers as integers. Non-ASCII is kept as UTF-8 (no escaping).
  return resolved.dump(-1, ' ', false, json::error_handler_t::strict);
}

Result<std::string> resolve_normalization_config_json(std::string_view text) {
  Result<json> r = resolve_normalization_config(text);
  if (!r) {
    return Result<std::string>::error(r.status(), std::string(r.message()));
  }
  return Result<std::string>::ok(canonical_normalization_json(r.value()));
}

}  // namespace ovt::metrics
