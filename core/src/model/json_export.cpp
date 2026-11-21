// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// to_json(): the reflection-based JSON exporter of chapter 06 §6.8.1 (WI-0.088), the JSON of `--format json`
// (chapter 12 §12.1.4). It walks the buffer through the embedded binary schema, so it serves all three
// schemas and every root table without generated code.
//
// The layout is the one flatbuffers::GenerateText writes with the §6.8.1 options (strict_json,
// output_default_scalars_in_json, indent_step 2 or -1, output_enum_identifiers, natural_utf8): `"key": value`,
// one field or vector element per line, fields in schema (field-id) order. The text is written here rather
// than by GenerateText because the rules of §6.8.1 and §12.1.4 that GenerateText lacks need the value, not its
// text: nested buffers and hash-like byte vectors (chosen by reflection field name), integers beyond ±2^53 as
// strings, shortest round-trip floats (GenerateText prints 6 / 12 fixed decimals, which loses values), NaN
// and infinities as strings, control characters as \u00XX (GenerateText writes them raw, which is not JSON).
// GenerateText also reads the buffer unverified and without a work bound.
//
// SPEC-ASSUMPTION(#493): own printer instead of GenerateText plus a rewrite of its output; a present empty
// vector is `[]`; an absent string, vector, struct or table is left out (the §6.8.1 example shows
// `"ext": null`); a nested-buffer field whose `X_type` names no table stays an array of numbers.

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/reflection_generated.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json_rules.hpp"
#include "ovt/cxx/model/json.hpp"
#include "ovt/cxx/model/schema_id.hpp"
#include "reflect_access.hpp"

namespace ovt::model {
namespace {

static_assert(FLATBUFFERS_LITTLEENDIAN, "json_export.cpp reads scalars as little-endian bytes");

using reflection::BaseType;
using ByteView = std::span<const std::uint8_t>;

[[noreturn]] void fail(const std::string& what) {
  throw std::invalid_argument("to_json: " + what);
}

template <class T>
const T& deref(const T* pointer) {
  if (pointer == nullptr) {
    fail("malformed embedded .bfbs");
  }
  return *pointer;
}

bool is_scalar(BaseType type) {
  return type >= BaseType::UType && type <= BaseType::Double;
}

template <class T>
T read(const std::uint8_t* at) {
  T value{};
  std::memcpy(&value, at, sizeof(T));
  return value;
}

/// Appends the ASCII character @p byte as it stands inside a JSON string.
void append_ascii(std::string& out, unsigned char byte) {
  constexpr std::string_view kHex = "0123456789abcdef";
  switch (byte) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      if (byte < 0x20) {
        out += "\\u00";
        out += kHex[byte >> 4U];
        out += kHex[byte & 0xfU];
      } else {
        out += static_cast<char>(byte);
      }
  }
}

/// The length of the multi-byte UTF-8 sequence at the start of @p text, or 0 when it is not one (a stray or
/// missing continuation byte, an overlong form, a surrogate, a code point above U+10FFFF).
std::size_t utf8_sequence(std::string_view text) {
  const auto lead = static_cast<unsigned char>(text.front());
  const std::size_t extra = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : 1;
  if (lead < 0xc2 || lead > 0xf4 || text.size() <= extra) {
    return 0;
  }
  std::uint32_t code = lead & (0x3fU >> extra);
  for (const char c : text.substr(1, extra)) {
    const auto next = static_cast<unsigned char>(c);
    if ((next & 0xc0U) != 0x80) {
      return 0;
    }
    code = (code << 6U) | (next & 0x3fU);
  }
  const std::uint32_t lowest = extra == 3 ? 0x10000 : extra == 2 ? 0x800 : 0x80;
  const bool valid = code >= lowest && code <= 0x10ffff && (code < 0xd800 || code > 0xdfff);
  return valid ? extra + 1 : 0;
}

/// Appends @p text as a JSON string. False when it is not UTF-8.
bool append_string(std::string& out, std::string_view text) {
  out += '"';
  for (std::size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x80) {
      append_ascii(out, byte);
      ++i;
      continue;
    }
    const std::size_t length = utf8_sequence(text.substr(i));
    if (length == 0) {
      return false;
    }
    out.append(text.substr(i, length));
    i += length;
  }
  out += '"';
  return true;
}

/// Work budget against shared offsets, as in canonical.cpp: a valid buffer may point many offsets at one
/// table or vector, so the text can grow far beyond the input. Every buffer byte visited is charged; past
/// 16 x input + 1 MiB, capped below the 2 GiB FlatBuffers limit, the call throws.
class Budget {
 public:
  explicit Budget(std::size_t input) {
    constexpr std::size_t kCap = (std::size_t{1} << 31U) - 1;
    constexpr std::size_t kSlack = std::size_t{1} << 20U;
    limit_ = input >= (kCap - kSlack) / 16 ? kCap : 16 * input + kSlack;
  }
  void charge(std::size_t bytes) {
    if (bytes > limit_ - spent_) {
      fail("work exceeds the budget of " + std::to_string(limit_) + " bytes (offsets shared by many parents?)");
    }
    spent_ += bytes;
  }

 private:
  std::size_t limit_ = 0;
  std::size_t spent_ = 0;
};

/// The significant digits of the decimal number @p text (an optional sign, digits, an optional fraction and
/// exponent, as fmt prints a finite value) without leading or trailing zeros, and in @p point the position of
/// the decimal point: the magnitude is 0.<digits> x 10^point.
std::string decimal_digits(std::string_view text, int& point) {
  std::string digits;
  bool fraction = false;
  std::size_t at = text.starts_with('-') ? 1 : 0;
  point = 0;
  for (; at < text.size() && text[at] != 'e'; ++at) {
    if (text[at] == '.') {
      fraction = true;
    } else if (!digits.empty() || text[at] != '0') {
      digits += text[at];
      point += fraction ? 0 : 1;
    } else if (fraction) {
      --point;  // a zero between the point and the first digit
    }
  }
  if (at + 1 < text.size()) {
    int exponent = 0;
    const std::string_view rest = text.substr(at + 1 + (text[at + 1] == '+' ? 1U : 0U));
    std::from_chars(std::to_address(rest.begin()), std::to_address(rest.end()), exponent);
    point += exponent;
  }
  while (!digits.empty() && digits.back() == '0') {
    digits.pop_back();
  }
  return digits;
}

/// Appends the decimal number @p text to @p out in one fixed layout, that of ECMAScript's Number::toString:
/// plain digits while the decimal point lies within 21 digits to the right of the first digit or 6 zeros to
/// its left, and `d.ddde±x` otherwise. The digits themselves are kept.
void append_decimal(std::string& out, std::string_view text) {
  int point = 0;
  const std::string digits = decimal_digits(text, point);
  if (text.starts_with('-')) {
    out += '-';
  }
  if (digits.empty()) {
    out += '0';
    return;
  }
  const int count = static_cast<int>(digits.size());
  constexpr int kMaxFixed = 21;
  constexpr int kMinFixed = -6;
  if (count <= point && point <= kMaxFixed) {
    out += digits;
    out.append(static_cast<std::size_t>(point - count), '0');
  } else if (0 < point && point <= kMaxFixed) {
    out.append(digits, 0, static_cast<std::size_t>(point));
    out += '.';
    out.append(digits, static_cast<std::size_t>(point));
  } else if (kMinFixed < point && point <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-point), '0');
    out += digits;
  } else {
    out += digits.front();
    if (count > 1) {
      out += '.';
      out.append(digits, 1);
    }
    out += 'e';
    out += point > 0 ? '+' : '-';
    out += std::to_string(point > 0 ? point - 1 : 1 - point);
  }
}

// The walk recurses along the object tree. The depth is bounded: every buffer passes the reflection verifier
// (max_depth 64) before it is walked, nested buffers are verified the same way and at most kMaxNestedBuffers
// deep. The width is bounded by the Budget.
class Printer {
 public:
  Printer(const reflection::Schema& schema, JsonOptions options, std::size_t input_size)
      : schema_(schema), options_(options), budget_(input_size) {}

  /// Verifies @p buffer as a finished buffer of @p object and appends its JSON object at @p indent.
  // NOLINTNEXTLINE(misc-no-recursion): bounded by the schema, the verifier depth and the nested-buffer limit
  void buffer(const reflection::Object& object, ByteView buffer, int indent) {
    budget_.charge(buffer.size());
    // 8-byte aligned storage: a caller's span, or a nested buffer inside a [ubyte] vector, may be less aligned.
    std::vector<std::uint64_t> words((buffer.size() + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    if (!buffer.empty()) {
      std::memcpy(words.data(), buffer.data(), buffer.size());
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(words.data());
    if (!detail::verify_aligned(schema_, object, ByteView(data, buffer.size()))) {
      fail("buffer is not a valid " + deref(object.name()).str());
    }
    table(object, *flatbuffers::GetAnyRoot(data), indent);
  }

  std::string take() {
    newline();
    return std::move(out_);
  }

 private:
  const reflection::Schema& schema_;
  JsonOptions options_;
  Budget budget_;
  std::string out_;
  int nesting_ = 0;  ///< nested-buffer levels above the table being printed
  std::unordered_map<const reflection::Object*, std::vector<const reflection::Field*>> field_order_;

  static constexpr int kStep = 2;

  void newline() {
    if (!options_.compact) {
      out_ += '\n';
    }
  }

  void line(int indent) {
    if (!options_.compact) {
      out_ += '\n';
      out_.append(static_cast<std::size_t>(indent), ' ');
    }
  }

  /// The fields of @p object in field-id order (= schema order; the .bfbs stores them sorted by name).
  const std::vector<const reflection::Field*>& fields(const reflection::Object& object) {
    auto [it, inserted] = field_order_.try_emplace(&object);
    if (inserted) {
      const auto& all = deref(object.fields());
      it->second.assign(all.begin(), all.end());
      std::ranges::sort(it->second, {}, [](const reflection::Field* field) { return field->id(); });
    }
    return it->second;
  }

  [[nodiscard]] const reflection::Object& object_at(std::int32_t index) const {
    const auto& objects = deref(schema_.objects());
    if (index < 0 || static_cast<flatbuffers::uoffset_t>(index) >= objects.size()) {
      fail("malformed embedded .bfbs (object index)");
    }
    return deref(objects.Get(static_cast<flatbuffers::uoffset_t>(index)));
  }

  template <class T>
  void integer(T value) {
    std::array<char, 24> text{};
    const auto end = std::to_chars(std::to_address(text.begin()), std::to_address(text.end()), value).ptr;
    // Beyond ±2^53 a JSON number loses precision in double-based consumers: a decimal string (§12.1.4).
    constexpr std::uint64_t kExact = std::uint64_t{1} << 53U;
    const std::uint64_t magnitude = std::cmp_less(value, 0) ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                                                            : static_cast<std::uint64_t>(value);
    const bool quoted = magnitude > kExact;
    if (quoted) {
      out_ += '"';
    }
    out_.append(text.data(), end);
    if (quoted) {
      out_ += '"';
    }
  }

  template <class T>
  void real(T value) {
    if (std::isfinite(value)) {
      // fmt gives the shortest decimal digits that read back to the same value, locale-independent
      // (floating-point std::to_chars needs macOS 13.3; the deployment target is 13.0). Where fmt switches
      // between fixed and exponent notation differs between its versions, so the layout is done here.
      std::array<char, 64> text{};
      const auto end = fmt::format_to_n(text.begin(), text.size(), "{}", value).out;
      append_decimal(out_, std::string_view(text.data(), static_cast<std::size_t>(end - text.begin())));
      return;
    }
    const std::string_view text = std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
    if (options_.strict_numbers) {
      out_ += '"';
    }
    out_ += text;
    if (options_.strict_numbers) {
      out_ += '"';
    }
  }

  /// One scalar of @p type stored at @p at. @p enum_index is the schema's enum of an integer type, or -1.
  void scalar(BaseType type, std::int32_t enum_index, const std::uint8_t* at) {
    if (type == BaseType::Bool) {
      // A byte that is neither 0 nor 1 is written as its integer, like an unknown enum value: the importer
      // stores it back, so the round trip keeps what a producer wrote.
      if (const auto value = read<std::uint8_t>(at); value <= 1) {
        out_ += value != 0 ? "true" : "false";
      } else {
        integer(value);
      }
      return;
    }
    if (type == BaseType::Float) {
      real(read<float>(at));
      return;
    }
    if (type == BaseType::Double) {
      real(read<double>(at));
      return;
    }
    if (type == BaseType::ULong) {
      integer(read<std::uint64_t>(at));  // the OVT schemas have no ulong enum
      return;
    }
    std::int64_t value = 0;
    switch (type) {
      case BaseType::Byte:
        value = read<std::int8_t>(at);  // NOLINT(bugprone-signed-char-misuse,cert-str34-c): an 8-bit integer field
        break;
      case BaseType::UByte:
        value = read<std::uint8_t>(at);
        break;
      case BaseType::Short:
        value = read<std::int16_t>(at);
        break;
      case BaseType::UShort:
        value = read<std::uint16_t>(at);
        break;
      case BaseType::Int:
        value = read<std::int32_t>(at);
        break;
      case BaseType::UInt:
        value = read<std::uint32_t>(at);
        break;
      case BaseType::Long:
        value = read<std::int64_t>(at);
        break;
      default:  // union type tags: not used by the OVT schemas (the verifier rejects them first)
        fail("unsupported scalar type");
    }
    if (enum_index >= 0) {
      const auto& enums = deref(schema_.enums());
      const auto* def = static_cast<flatbuffers::uoffset_t>(enum_index) < enums.size()
                            ? enums.Get(static_cast<flatbuffers::uoffset_t>(enum_index))
                            : nullptr;
      const auto* entry = deref(deref(def).values()).LookupByKey(value);
      if (entry != nullptr) {
        out_ += '"';
        out_ += deref(entry->name()).string_view();
        out_ += '"';
        return;
      }
    }
    integer(value);
  }

  void string(const flatbuffers::String* text) {
    const auto view = deref(text).string_view();
    budget_.charge(view.size() + sizeof(flatbuffers::uoffset_t));
    if (!append_string(out_, view)) {
      fail("a string is not UTF-8");
    }
  }

  void key(const reflection::Field& field, bool first, int indent) {
    if (!first) {
      out_ += ',';
    }
    line(indent);
    out_ += '"';
    out_ += deref(field.name()).string_view();
    out_ += "\": ";
  }

  /// @p bytes starts at the struct (and reaches at least to its end: the verifier checked it).
  // NOLINTNEXTLINE(misc-no-recursion): bounded by the schema, the verifier depth and the nested-buffer limit
  void structure(const reflection::Object& object, ByteView bytes, int indent) {
    out_ += '{';
    bool first = true;
    for (const auto* field : fields(object)) {
      const auto& type = deref(field->type());
      key(*field, first, indent + kStep);
      first = false;
      const auto member = bytes.subspan(field->offset());
      if (type.base_type() == BaseType::Obj) {
        structure(object_at(type.index()), member, indent + kStep);
      } else if (is_scalar(type.base_type())) {
        scalar(type.base_type(), type.index(), member.data());
      } else {  // fixed-length arrays: not used by the OVT schemas
        fail("unsupported struct member type");
      }
    }
    line(indent);
    out_ += '}';
  }

  /// `[`, @p count elements written by @p element(i), `]`.
  template <class Element>
  // NOLINTNEXTLINE(misc-no-recursion): bounded by the schema, the verifier depth and the nested-buffer limit
  void array(std::size_t count, int indent, Element&& element) {
    if (count == 0) {
      out_ += "[]";
      return;
    }
    out_ += '[';
    for (std::size_t i = 0; i < count; ++i) {
      if (i != 0) {
        out_ += ',';
      }
      line(indent + kStep);
      element(i);
    }
    line(indent);
    out_ += ']';
  }

  void hex(ByteView bytes) {
    constexpr std::string_view kHex = "0123456789abcdef";
    out_ += '"';
    for (const auto byte : bytes) {
      out_ += kHex[byte >> 4U];
      out_ += kHex[byte & 0xfU];
    }
    out_ += '"';
  }

  // NOLINTNEXTLINE(misc-no-recursion): bounded by the schema, the verifier depth and the nested-buffer limit
  void vector(const reflection::Object& object, const reflection::Field& field, const flatbuffers::Table& table,
              int indent) {
    const auto& type = deref(field.type());
    const auto* raw = table.GetPointer<const flatbuffers::Vector<std::uint8_t>*>(field.offset());
    const std::size_t count = deref(raw).size();
    const std::uint8_t* data = raw->Data();
    if (type.element() == BaseType::String) {
      const auto* strings = reinterpret_cast<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(raw);
      array(count, indent, [&](std::size_t i) { string(strings->Get(static_cast<flatbuffers::uoffset_t>(i))); });
      return;
    }
    if (type.element() == BaseType::Obj) {
      const auto& element = object_at(type.index());
      if (element.is_struct()) {
        const auto size = static_cast<std::size_t>(element.bytesize());
        budget_.charge(count * size);
        const ByteView structs(data, count * size);
        array(count, indent, [&](std::size_t i) { structure(element, structs.subspan(i * size), indent + kStep); });
      } else {
        const auto* tables = reinterpret_cast<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(raw);
        budget_.charge(count * sizeof(flatbuffers::uoffset_t));
        // NOLINTNEXTLINE(misc-no-recursion): bounded by the schema, the verifier depth and the nested-buffer limit
        array(count, indent, [&](std::size_t i) {
          this->table(element, deref(tables->Get(static_cast<flatbuffers::uoffset_t>(i))), indent + kStep);
        });
      }
      return;
    }
    if (!is_scalar(type.element())) {  // vectors of unions or vectors: not used by the OVT schemas
      fail("unsupported vector element type");
    }
    const std::size_t size = flatbuffers::GetTypeSize(type.element());
    const ByteView bytes(data, count * size);
    if (type.element() == BaseType::UByte) {
      if (detail::is_hash_field(deref(field.name()).string_view())) {
        budget_.charge(count);
        hex(bytes);
        return;
      }
      if (const auto* nested = count == 0 ? nullptr : nested_table(object, field, table); nested != nullptr) {
        if (nesting_ == detail::kMaxNestedBuffers) {
          fail("nested buffers deeper than " + std::to_string(detail::kMaxNestedBuffers) + " levels");
        }
        ++nesting_;
        buffer(*nested, bytes, indent);
        --nesting_;
        return;
      }
    }
    budget_.charge(bytes.size());
    array(count, indent, [&](std::size_t i) { scalar(type.element(), type.index(), bytes.subspan(i * size).data()); });
  }

  /// The table that the nested buffer in `[ubyte]` field @p field of @p table holds: the one its `_type`
  /// sibling names, or the fixed one of a palette field (§6.8.1). Null when the field is plain bytes.
  [[nodiscard]] const reflection::Object* nested_table(const reflection::Object& object, const reflection::Field& field,
                                                       const flatbuffers::Table& table) const {
    if (const auto* type_field = detail::nested_type_sibling(object, field); type_field != nullptr) {
      const auto* name = table.GetPointer<const flatbuffers::String*>(type_field->offset());
      return detail::find_table(schema_, object, name == nullptr ? std::string_view{} : name->string_view());
    }
    const std::string_view fixed =
        detail::fixed_nested_table(deref(object.name()).string_view(), deref(field.name()).string_view());
    return fixed.empty() ? nullptr : detail::find_table(schema_, object, fixed);
  }

  // NOLINTNEXTLINE(misc-no-recursion): bounded by the schema, the verifier depth and the nested-buffer limit
  void table(const reflection::Object& object, const flatbuffers::Table& table, int indent) {
    budget_.charge(2 * sizeof(flatbuffers::uoffset_t));
    out_ += '{';
    bool first = true;
    for (const auto* field : fields(object)) {
      if (field->deprecated()) {
        continue;
      }
      const auto& type = deref(field->type());
      const auto base = type.base_type();
      const auto* at = table.GetAddressOf(field->offset());
      if (is_scalar(base)) {
        key(*field, first, indent + kStep);
        first = false;
        if (at != nullptr) {
          scalar(base, type.index(), at);
        } else if (field->optional()) {
          out_ += "null";
        } else {
          std::array<std::uint8_t, sizeof(std::uint64_t)> value{};
          default_scalar(*field, base, value);
          scalar(base, type.index(), value.data());
        }
        continue;
      }
      if (at == nullptr) {
        continue;  // an absent string, vector, struct or table
      }
      key(*field, first, indent + kStep);
      first = false;
      switch (base) {
        case BaseType::String:
          string(table.GetPointer<const flatbuffers::String*>(field->offset()));
          break;
        case BaseType::Vector:
          vector(object, *field, table, indent + kStep);
          break;
        case BaseType::Obj: {
          const auto& sub = object_at(type.index());
          if (sub.is_struct()) {
            budget_.charge(static_cast<std::size_t>(sub.bytesize()));
            structure(sub, ByteView(at, static_cast<std::size_t>(sub.bytesize())), indent + kStep);
          } else {
            this->table(sub, deref(table.GetPointer<const flatbuffers::Table*>(field->offset())), indent + kStep);
          }
          break;
        }
        default:  // unions, 64-bit vectors: not used by the OVT schemas (the verifier rejects them first)
          fail("unsupported field type");
      }
    }
    line(indent);
    out_ += '}';
  }

  /// The schema default of scalar @p field as the little-endian bytes of its type.
  static void default_scalar(const reflection::Field& field, BaseType base,
                             std::array<std::uint8_t, sizeof(std::uint64_t)>& out) {
    if (base == BaseType::Float) {
      const auto value = static_cast<float>(field.default_real());
      std::memcpy(out.data(), &value, sizeof(value));
    } else if (base == BaseType::Double) {
      const double value = field.default_real();
      std::memcpy(out.data(), &value, sizeof(value));
    } else {
      const std::int64_t value = field.default_integer();  // little-endian: the low bytes are the narrower types
      std::memcpy(out.data(), &value, sizeof(value));
    }
  }
};

}  // namespace

std::string to_json(std::span<const std::uint8_t> buf, SchemaId schema, std::string_view root_table,
                    JsonOptions options) {
  const auto resolved = detail::resolve_root(schema, root_table, "to_json");
  Printer printer(*resolved.schema, options, buf.size());
  printer.buffer(*resolved.root, buf, 0);
  return printer.take();
}

}  // namespace ovt::model
