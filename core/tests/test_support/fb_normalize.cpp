// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_support/fb_normalize.h"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

namespace ovt::test {
namespace {

void write_indent(std::string& out, int depth) { out.append(static_cast<std::size_t>(depth) * 2U, ' '); }

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

}  // namespace ovt::test
