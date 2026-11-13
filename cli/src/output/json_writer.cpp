// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output/json_writer.h"

#include <fmt/format.h>

#include <cmath>

#include "ovt/version.h"

namespace ovt::cli {

namespace {

constexpr std::int64_t kMaxExactInt = std::int64_t{1} << 53;

}  // namespace

std::string json_quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          out += fmt::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out += fmt::format("{:02x}", b);
  }
  return out;
}

void JsonWriter::put(std::string_view s) {
  if (!s.empty() && std::fwrite(s.data(), 1, s.size(), out_) != s.size()) {
    failed_ = true;
  }
}

void JsonWriter::newline_indent() {
  if (pretty_) {
    put("\n");
    put(std::string(stack_.size() * 2, ' '));
  }
}

void JsonWriter::before_value() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (stack_.empty()) {
    return;
  }
  if (!stack_.back().empty) {
    put(",");
  }
  stack_.back().empty = false;
  newline_indent();
}

void JsonWriter::begin_object() {
  before_value();
  put("{");
  stack_.push_back({false, true});
}

void JsonWriter::begin_array() {
  before_value();
  put("[");
  stack_.push_back({true, true});
}

void JsonWriter::end_object() {
  const bool was_empty = stack_.back().empty;
  stack_.pop_back();
  if (!was_empty) {
    newline_indent();
  }
  put("}");
}

void JsonWriter::end_array() {
  const bool was_empty = stack_.back().empty;
  stack_.pop_back();
  if (!was_empty) {
    newline_indent();
  }
  put("]");
}

void JsonWriter::key(std::string_view name) {
  before_value();
  put(json_quote(name));
  put(pretty_ ? ": " : ":");
  after_key_ = true;
}

void JsonWriter::string(std::string_view s) {
  before_value();
  put(json_quote(s));
}

void JsonWriter::boolean(bool b) {
  before_value();
  put(b ? "true" : "false");
}

void JsonWriter::null() {
  before_value();
  put("null");
}

void JsonWriter::integer(std::int64_t v) {
  before_value();
  const bool exact = v >= -kMaxExactInt && v <= kMaxExactInt;
  put(exact ? fmt::format("{}", v) : fmt::format("\"{}\"", v));
}

void JsonWriter::unsigned_integer(std::uint64_t v) {
  before_value();
  const bool exact = v <= static_cast<std::uint64_t>(kMaxExactInt);
  put(exact ? fmt::format("{}", v) : fmt::format("\"{}\"", v));
}

void JsonWriter::number(double v) {
  before_value();
  if (std::isnan(v)) {
    put("\"nan\"");
    return;
  }
  if (std::isinf(v)) {
    put(v > 0 ? "\"inf\"" : "\"-inf\"");
    return;
  }
  put(fmt::format("{}", v));  // shortest round-trip, locale independent
}

void JsonWriter::raw(std::string_view json) {
  before_value();
  put(json);
}

bool JsonWriter::finish() {
  put("\n");
  return !failed_ && std::fflush(out_) == 0;
}

void write_diagnostic(JsonWriter& w, const Diagnostic& d) {
  w.begin_object();
  w.key("severity");
  w.string(d.severity);
  w.key("code");
  w.string(d.code);
  w.key("message");
  w.string(d.message);
  if (d.span) {
    w.key("span");
    w.begin_object();
    w.key("bit_offset");
    w.unsigned_integer(d.span->first);
    w.key("bit_length");
    w.unsigned_integer(d.span->second);
    w.end_object();
  }
  if (!d.spec_ref.empty()) {
    w.key("spec_ref");
    w.string(d.spec_ref);
  }
  w.end_object();
}

bool write_json_envelope(std::FILE* out, bool pretty, const Envelope& env,
                         const std::function<void(JsonWriter&)>& result) {
  JsonWriter w(out, pretty);
  w.begin_object();
  w.key("$schema");
  w.string(kEnvelopeSchemaUrl);
  w.key("ovt_version");
  w.string(ovt_version_string());
  w.key("schema_version");
  w.unsigned_integer(kEnvelopeSchemaVersion);
  w.key("command");
  w.string(env.command);
  if (env.input) {
    w.key("input");
    w.begin_object();
    w.key("path");
    w.string(env.input->path);
    w.key("size_bytes");
    w.unsigned_integer(env.input->size_bytes);
    w.key("source_hash");
    w.string(to_hex(env.input->source_hash));
    w.end_object();
  }
  w.key("result");
  if (result) {
    result(w);
  } else {
    w.begin_object();
    w.end_object();
  }
  w.key("diagnostics");
  w.begin_array();
  for (const Diagnostic& d : env.diagnostics) {
    write_diagnostic(w, d);
  }
  w.end_array();
  w.key("status");
  w.string(exit_code_name(env.status));
  if (env.timing) {
    w.key("timing");
    w.begin_object();
    for (const auto& [name, value] : env.timing->values) {
      w.key(name);
      w.number(value);
    }
    if (env.timing->cache_hit) {
      w.key("cache_hit");
      w.boolean(*env.timing->cache_hit);
    }
    w.end_object();
  }
  w.end_object();
  return w.finish();
}

}  // namespace ovt::cli
