// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output/tree_flatten.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "output/json_writer.h"

namespace ovt::cli {

namespace {

// Bytes shown in a text line before the value is shortened.
constexpr std::size_t kTextBytes = 32;

std::string_view sv(const flatbuffers::String* s) {
  return s == nullptr ? std::string_view() : s->string_view();
}

using Children = flatbuffers::Vector<flatbuffers::Offset<fb::BoxNode>>;

// Path segments of a sibling list: `fourcc`, or `fourcc[i]` when the fourcc repeats among the siblings.
template <typename Range>
std::vector<std::string> segments(const Range& boxes) {
  std::map<std::string_view, std::size_t> total;
  for (const fb::BoxNode* b : boxes) {
    ++total[sv(b->fourcc())];
  }
  std::map<std::string_view, std::size_t> seen;
  std::vector<std::string> out;
  out.reserve(total.size());
  for (const fb::BoxNode* b : boxes) {
    const std::string_view cc = sv(b->fourcc());
    const std::size_t i = seen[cc]++;
    out.push_back(total[cc] > 1 ? fmt::format("{}[{}]", cc, i) : std::string(cc));
  }
  return out;
}

BoxShow show_of(const BoxFilter& filter, const fb::BoxNode& b, int depth) {
  return filter ? filter(b, depth) : BoxShow{};
}

std::string flags_text(int flags) {
  return flags == 0 ? "0" : fmt::format("0x{:x}", static_cast<unsigned>(flags));
}

// ---- text ----------------------------------------------------------------------------------------------------

// Field nesting is bounded by the decoders (kMaxBoxDepth and their own limits).
// NOLINTNEXTLINE(misc-no-recursion)
void text_field(std::string& out, const fb::Field& f, int indent) {
  out.append(static_cast<std::size_t>(indent) * 2, ' ');
  out.append(sv(f.name()));
  if (f.kind() != fb::SyntaxValueKind::None) {
    out += " = " + field_value(f, true);
    if (f.kind() != fb::SyntaxValueKind::String && f.svalue() != nullptr && !f.svalue()->empty()) {
      out += fmt::format(" (\"{}\")", sv(f.svalue()));
    }
  }
  if (const fb::BitSpan* s = f.span(); s != nullptr && s->bit_length() > 0) {
    out += fmt::format(" (bits {}..{}", s->bit_offset(), s->bit_offset() + s->bit_length());
    out += f.spec_ref() != nullptr && !f.spec_ref()->empty() ? fmt::format(", {})", sv(f.spec_ref())) : ")";
  }
  out.push_back('\n');
  if (f.children() != nullptr) {
    for (const fb::Field* c : *f.children()) {
      text_field(out, *c, indent + 1);
    }
  }
}

// Box nesting is bounded by kMaxBoxDepth in the reader.
// NOLINTNEXTLINE(misc-no-recursion)
void text_box(std::string& out, const fb::BoxNode& b, int depth, const BoxFilter& filter) {
  const BoxShow show = show_of(filter, b, depth);
  out.append(static_cast<std::size_t>(depth - 1) * 2, ' ');
  const fb::ByteSpan* span = b.file_span();
  out += fmt::format("{:<6} @{} +{}", sv(b.fourcc()), span != nullptr ? span->offset() : 0,
                     span != nullptr ? span->length() : 0);
  if (b.version() >= 0) {
    out += fmt::format("  v{} f={}", b.version(), flags_text(b.flags()));
  }
  out.push_back('\n');
  if (b.diagnostics() != nullptr) {
    for (const fb::Diagnostic* d : *b.diagnostics()) {
      out.append(static_cast<std::size_t>(depth) * 2, ' ');
      out += fmt::format("! {} {}: {}\n", fb::EnumNameSeverity(d->severity()), sv(d->code()), sv(d->message()));
    }
  }
  if (show.fields && b.fields() != nullptr) {
    for (const fb::Field* f : *b.fields()) {
      text_field(out, *f, depth);
    }
  }
  if (show.children && b.children() != nullptr) {
    for (const fb::BoxNode* c : *b.children()) {
      text_box(out, *c, depth + 1, filter);
    }
  }
}

// ---- csv -----------------------------------------------------------------------------------------------------

// NOLINTNEXTLINE(misc-no-recursion)
void csv_field(CsvWriter& csv, const fb::Field& f, int depth, const std::string& parent) {
  const std::string path = parent + "/" + std::string(sv(f.name()));
  const fb::BitSpan* s = f.span();
  csv.row({std::to_string(depth), path, std::string(sv(f.name())), fb::EnumNameSyntaxValueKind(f.kind()),
           f.kind() == fb::SyntaxValueKind::None ? std::string() : field_value(f, false),
           s != nullptr ? std::to_string(s->bit_offset()) : std::string(),
           s != nullptr ? std::to_string(s->bit_length()) : std::string(), std::string(sv(f.spec_ref()))});
  if (f.children() != nullptr) {
    for (const fb::Field* c : *f.children()) {
      csv_field(csv, *c, depth + 1, path);
    }
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void csv_box(CsvWriter& csv, const fb::BoxNode& b, int depth, const std::string& path, const BoxFilter& filter) {
  const BoxShow show = show_of(filter, b, depth);
  const fb::ByteSpan* span = b.file_span();
  const std::uint64_t off = span != nullptr ? span->offset() : 0;
  const std::uint64_t len = span != nullptr ? span->length() : 0;
  csv.row({std::to_string(depth), path, std::string(sv(b.fourcc())), "box", std::to_string(len),
           std::to_string(off * 8), std::to_string(len * 8), std::string(sv(b.spec_ref()))});
  if (show.fields && b.fields() != nullptr) {
    for (const fb::Field* f : *b.fields()) {
      csv_field(csv, *f, depth + 1, path);
    }
  }
  if (show.children && b.children() != nullptr) {
    const std::vector<std::string> names = segments(*b.children());
    for (flatbuffers::uoffset_t i = 0; i < b.children()->size(); ++i) {
      csv_box(csv, *b.children()->Get(i), depth + 1, path + "/" + names[i], filter);
    }
  }
}

}  // namespace

std::string field_value(const fb::Field& f, bool for_text) {
  switch (f.kind()) {
    case fb::SyntaxValueKind::Int:
      return std::to_string(f.ivalue());
    case fb::SyntaxValueKind::Fixed:
      // ivalue is the raw fixed-point number; the decimal is the derived svalue.
      return for_text || f.svalue() == nullptr ? std::to_string(f.ivalue()) : f.svalue()->str();
    case fb::SyntaxValueKind::String:
      return for_text ? json_quote(sv(f.svalue())) : std::string(sv(f.svalue()));
    case fb::SyntaxValueKind::Bytes: {
      if (f.bytes() == nullptr) {
        return "";
      }
      const std::size_t n = for_text ? std::min<std::size_t>(f.bytes()->size(), kTextBytes) : f.bytes()->size();
      std::string hex = to_hex(
          std::vector<std::uint8_t>(f.bytes()->begin(), std::next(f.bytes()->begin(), static_cast<std::ptrdiff_t>(n))));
      return n < f.bytes()->size() ? fmt::format("{}… ({} bytes)", hex, f.bytes()->size()) : hex;
    }
    default:  // UInt, Bool, Enum
      return std::to_string(f.uvalue());
  }
}

std::string render_box_tree(const BoxList& boxes, const BoxFilter& filter) {
  std::string out;
  for (const fb::BoxNode* b : boxes) {
    text_box(out, *b, 1, filter);
  }
  return out;
}

void flatten_box_tree(CsvWriter& csv, const BoxList& boxes, const BoxFilter& filter) {
  csv.row({"depth", "path", "name", "descriptor", "value", "bit_offset", "bit_length", "spec_ref"});
  const std::vector<std::string> names = segments(boxes);
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    csv_box(csv, *boxes[i], 1, names[i], filter);
  }
}

}  // namespace ovt::cli
