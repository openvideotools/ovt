// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// JSON and `--format trace` writers for SyntaxNodeBuffer (chapter 7 §7.3.2, §7.7.1; WI-1.106).
// Both are deterministic: the same tree always gives the same text (§0.7.5).

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "syntax/common/syntax_node_buffer.h"

namespace ovt::syntax {

namespace {

constexpr std::array<std::string_view, 8> kKindNames = {"None",  "UInt",   "Int",  "Bool",
                                                        "Bytes", "String", "Enum", "Fixed"};
constexpr std::array<std::string_view, 4> kSeverityNames = {"Info", "Warning", "Error", "Fatal"};

std::string_view kind_name(ValueKind k) {
  const auto i = static_cast<size_t>(k);
  return i < kKindNames.size() ? kKindNames.at(i) : std::string_view{"None"};
}

std::string_view severity_name(Severity s) {
  const auto i = static_cast<size_t>(s);
  return i < kSeverityNames.size() ? kSeverityNames.at(i) : std::string_view{"Info"};
}

// Length of the well-formed UTF-8 sequence that starts at s[i], or 0 when it is not well formed (RFC 3629:
// no overlongs, no surrogates, nothing above U+10FFFF).
size_t utf8_sequence_length(std::string_view s, size_t i) {
  const auto at = [&](size_t k) { return static_cast<unsigned char>(s.at(k)); };
  const unsigned c = at(i);
  size_t len = 0;
  unsigned lo = 0x80;
  unsigned hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) {
    len = 2;
  } else if (c >= 0xE0 && c <= 0xEF) {
    len = 3;
    if (c == 0xE0) {
      lo = 0xA0;
    }
    if (c == 0xED) {
      hi = 0x9F;
    }
  } else if (c >= 0xF0 && c <= 0xF4) {
    len = 4;
    if (c == 0xF0) {
      lo = 0x90;
    }
    if (c == 0xF4) {
      hi = 0x8F;
    }
  } else {
    return 0;
  }
  if (i + len > s.size()) {
    return 0;
  }
  if (at(i + 1) < lo || at(i + 1) > hi) {
    return 0;
  }
  for (size_t k = 2; k < len; ++k) {
    if (at(i + k) < 0x80 || at(i + k) > 0xBF) {
      return 0;
    }
  }
  return len;
}

// A JSON string literal. Strings can hold bytes from the stream (st(v)), so control characters are escaped and
// bytes that are not well-formed UTF-8 become U+FFFD.
void append_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s.at(i));
    if (c < 0x80) {
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
          if (c < 0x20 || c == 0x7F) {
            fmt::format_to(std::back_inserter(out), "\\u{:04x}", c);
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
      ++i;
      continue;
    }
    const size_t len = utf8_sequence_length(s, i);
    if (len == 0) {
      out += "\\ufffd";
      ++i;
    } else {
      out.append(s.substr(i, len));
      i += len;
    }
  }
  out.push_back('"');
}

class JsonWriter {
 public:
  explicit JsonWriter(std::string& out) : out_(out) {}

  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by SyntaxBuilder's fixed nesting stack
  void node(const SyntaxNodeData& n, int indent) {
    out_ += "{";
    first_ = true;
    const int in = indent + 1;
    key(in, "name");
    append_json_string(out_, n.name);
    string_field(in, "descriptor", n.descriptor);
    if (n.kind != ValueKind::None) {
      key(in, "kind");
      append_json_string(out_, kind_name(n.kind));
    }
    value(n, in);
    string_field(in, "enum_name", n.enum_name);
    string_field(in, "derived", n.derived);
    key(in, "span");
    span(n.span);
    string_field(in, "spec_ref", n.spec_ref);
    if (n.first_child != nullptr) {
      key(in, "children");
      out_ += "[";
      for (const SyntaxNodeData* c = n.first_child; c != nullptr; c = c->next_sibling) {
        if (c != n.first_child) {
          out_ += ",";
        }
        newline(in + 1);
        node(*c, in + 1);
      }
      newline(in);
      out_ += "]";
    }
    if (n.array_index != -1) {
      key(in, "array_index");
      fmt::format_to(std::back_inserter(out_), "{}", n.array_index);
    }
    if (n.first_diag != nullptr) {
      key(in, "diagnostics");
      out_ += "[";
      for (const DiagnosticData* d = n.first_diag; d != nullptr; d = d->next) {
        if (d != n.first_diag) {
          out_ += ",";
        }
        newline(in + 1);
        diagnostic(*d, in + 1);
      }
      newline(in);
      out_ += "]";
    }
    first_ = false;
    newline(indent);
    out_ += "}";
  }

 private:
  // The value field that matches the node's kind.
  void value(const SyntaxNodeData& n, int in) {
    switch (n.kind) {
      case ValueKind::UInt:
      case ValueKind::Enum:
        key(in, "uvalue");
        fmt::format_to(std::back_inserter(out_), "{}", n.uvalue);
        break;
      case ValueKind::Int:
      case ValueKind::Fixed:
        key(in, "ivalue");
        fmt::format_to(std::back_inserter(out_), "{}", n.ivalue);
        break;
      case ValueKind::Bool:
        key(in, "bvalue");
        out_ += n.bvalue ? "true" : "false";
        break;
      case ValueKind::Bytes:
        if (!n.bytes.empty()) {
          key(in, "bytes");
          out_ += "[";
          for (size_t i = 0; i < n.bytes.size(); ++i) {
            if (i > 0) {
              out_ += ", ";
            }
            fmt::format_to(std::back_inserter(out_), "{}", n.bytes[i]);
          }
          out_ += "]";
        }
        break;
      case ValueKind::String:
        key(in, "svalue");
        append_json_string(out_, n.svalue);
        break;
      case ValueKind::None:
        break;
    }
  }

  void newline(int indent) {
    out_ += "\n";
    out_.append(static_cast<size_t>(indent) * 2, ' ');
  }

  void key(int indent, std::string_view k) {
    if (!first_) {
      out_ += ",";
    }
    first_ = false;
    newline(indent);
    append_json_string(out_, k);
    out_ += ": ";
  }

  void string_field(int indent, std::string_view k, std::string_view v) {
    if (v.empty()) {
      return;
    }
    key(indent, k);
    append_json_string(out_, v);
  }

  void span(BitSpan s) {
    fmt::format_to(std::back_inserter(out_), R"({{"bit_offset": {}, "bit_length": {}}})", s.bit_offset, s.bit_length);
  }

  void diagnostic(const DiagnosticData& d, int indent) {
    out_ += "{";
    first_ = true;
    const int in = indent + 1;
    key(in, "severity");
    append_json_string(out_, severity_name(d.severity));
    string_field(in, "code", d.code);
    string_field(in, "message", d.message);
    key(in, "span");
    span(d.span);
    string_field(in, "spec_ref", d.spec_ref);
    first_ = false;
    newline(indent);
    out_ += "}";
  }

  std::string& out_;
  bool first_ = true;
};

// Bits shown per trace line; longer elements (st(v)) end in "...".
constexpr uint32_t kMaxTraceBits = 64;

class TraceWriter {
 public:
  TraceWriter(std::string& out, std::span<const uint8_t> unit) : out_(out), unit_(unit) {}

  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by SyntaxBuilder's fixed nesting stack
  void walk(const SyntaxNodeData& n) {
    if (n.first_child == nullptr) {
      line(n);
      return;
    }
    const bool indexed = n.array_element;
    if (indexed) {
      indices_.push_back(n.array_index);
    }
    for (const SyntaxNodeData* c = n.first_child; c != nullptr; c = c->next_sibling) {
      walk(*c);
    }
    if (indexed) {
      indices_.pop_back();
    }
  }

 private:
  void line(const SyntaxNodeData& n) {
    if (n.kind == ValueKind::None || n.kind == ValueKind::Bytes || n.span.bit_length == 0) {
      return;
    }
    fmt::format_to(std::back_inserter(out_), "{} {}", n.span.bit_offset, n.name);
    for (const int i : indices_) {
      fmt::format_to(std::back_inserter(out_), "[{}]", i);
    }
    out_ += ' ';
    const uint32_t shown = n.span.bit_length < kMaxTraceBits ? n.span.bit_length : kMaxTraceBits;
    for (uint32_t k = 0; k < shown; ++k) {
      out_ += bit(n.span.bit_offset + k);
    }
    if (n.span.bit_length > kMaxTraceBits) {
      out_ += "...";
    }
    out_ += " = ";
    switch (n.kind) {
      case ValueKind::UInt:
      case ValueKind::Enum:
        fmt::format_to(std::back_inserter(out_), "{}", n.uvalue);
        break;
      case ValueKind::Int:
      case ValueKind::Fixed:
        fmt::format_to(std::back_inserter(out_), "{}", n.ivalue);
        break;
      case ValueKind::Bool:
        out_ += n.bvalue ? '1' : '0';
        break;
      case ValueKind::String:
        append_json_string(out_, n.svalue);
        break;
      case ValueKind::None:
      case ValueKind::Bytes:
        break;
    }
    out_ += '\n';
  }

  [[nodiscard]] char bit(uint64_t pos) const {
    const uint64_t byte = pos / 8;
    if (byte >= unit_.size()) {
      return '?';
    }
    return ((unit_[static_cast<size_t>(byte)] >> (7 - pos % 8)) & 1U) != 0 ? '1' : '0';
  }

  std::string& out_;
  std::span<const uint8_t> unit_;
  std::vector<int> indices_;
};

}  // namespace

std::string to_json(const SyntaxNodeBuffer& buffer) {
  const SyntaxNodeData* root = buffer.root();
  if (root == nullptr) {
    return "null\n";
  }
  std::string out;
  JsonWriter(out).node(*root, 0);
  out += "\n";
  return out;
}

std::string to_trace(const SyntaxNodeBuffer& buffer, std::span<const uint8_t> unit_bytes) {
  std::string out;
  const SyntaxNodeData* root = buffer.root();
  if (root == nullptr) {
    return out;
  }
  TraceWriter(out, unit_bytes).walk(*root);
  return out;
}

}  // namespace ovt::syntax
