// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// from_json(): the reverse of to_json() (chapter 06 §6.8.1, WI-0.088; `ovt cache import <file.json>`).
// The text is untrusted. It is parsed by flatbuffers::Parser, loaded with the embedded binary schema, after
// a streaming rewrite (no JSON DOM) that undoes the two §6.8.1 departures from the FlatBuffers text format:
// a hex string under a hash-like field becomes an array of numbers, and an object under a nested-buffer
// field (`X` next to `X_type`) is parsed as the table `X_type` names and replaced by its bytes. The rewrite
// follows the schema by reflection field names; it never searches the text. Everything else is copied
// through unchanged and judged by the Parser. The result is canonicalised (WI-0.085).
//
// SPEC-ASSUMPTION(#493): the Parser is built with Parser::Deserialize from the embedded .bfbs instead of from
// embedded .fbs text (`embed_bfbs.py --also-text`): the schemas include each other, which Parser::Parse can
// only resolve through the file system. from_json returns canonical bytes and writes the file identifier
// only for the schema's root type.

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/reflection_generated.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json_rules.hpp"
#include "ovt/cxx/model/canonical.hpp"
#include "ovt/cxx/model/json.hpp"
#include "ovt/cxx/model/schema_id.hpp"
#include "reflect_access.hpp"

namespace ovt::model {
namespace {

using reflection::BaseType;
using Bytes = std::vector<std::uint8_t>;

/// Objects and arrays the rewrite follows by schema (the Parser has its own limit of 64 for the rest).
constexpr int kMaxDepth = 64;
constexpr std::string_view kTypeSuffix = "_type";

[[noreturn]] void fail(const std::string& what) {
  throw std::invalid_argument("from_json: " + what);
}

template <class T>
const T& deref(const T* pointer) {
  if (pointer == nullptr) {
    fail("malformed embedded .bfbs");
  }
  return *pointer;
}

std::string name_of(const reflection::Object& object) {
  return deref(object.name()).str();
}

void append_bytes(std::string& out, std::span<const std::uint8_t> bytes) {
  out += '[';
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += std::to_string(bytes[i]);
  }
  out += ']';
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// The rewrite recurses along the tables of the schema, at most kMaxDepth deep, and along nested buffers, at
// most kMaxNestedBuffers deep.
// NOLINTBEGIN(misc-no-recursion)
class Importer {
 public:
  Importer(SchemaId id, const reflection::Schema& schema) : schema_(schema), parser_(parser_options()) {
    const auto bytes = bfbs(id);
    if (!parser_.Deserialize(bytes.data(), bytes.size())) {
      fail("malformed embedded .bfbs");
    }
  }

  /// The buffer of @p json parsed as @p object, as the Parser builds it (not canonical).
  Bytes parse(std::string_view json, const reflection::Object& object, std::string_view identifier) {
    const std::string text = Rewriter(*this, json).run(object);
    // Set after the rewrite: it parses nested buffers with this same Parser.
    if (!parser_.SetRootType(name_of(object).c_str())) {
      fail("unknown root table '" + name_of(object) + "'");
    }
    parser_.file_identifier_ = identifier;
    if (!parser_.ParseJson(text.c_str())) {
      fail(parser_.error_);
    }
    const auto built = parser_.builder_.GetBufferSpan();
    return {built.begin(), built.end()};
  }

 private:
  const reflection::Schema& schema_;
  flatbuffers::Parser parser_;
  int nesting_ = 0;  ///< nested-buffer levels above the text being rewritten

  static flatbuffers::IDLOptions parser_options() {
    flatbuffers::IDLOptions options;
    options.strict_json = true;
    options.skip_unexpected_fields_in_json = true;  // §12.1.4: a consumer ignores unknown fields
    return options;
  }

  /// What the schema expects under a key, as far as the rewrite cares.
  struct Shape {
    enum class Kind : std::uint8_t { Other, Table, TableVector, Hex, Nested, TypeName };
    Kind kind = Kind::Other;
    const reflection::Object* table = nullptr;  ///< Table, TableVector; Nested: a fixed table
  };

  struct Pending {           ///< an object under a nested-buffer field, resolved when its table's `}` is reached
    std::string_view key;    ///< the field name
    std::string_view value;  ///< the text of the object
  };

  class Rewriter {
   public:
    Rewriter(Importer& importer, std::string_view in) : importer_(importer), in_(in) {}

    std::string run(const reflection::Object& root) {
      skip_space();
      if (peek() != '{') {
        fail("the text is not a JSON object");
      }
      object(root);
      skip_space();
      if (pos_ != in_.size()) {
        fail("characters after the JSON object at offset " + std::to_string(pos_));
      }
      return std::move(out_);
    }

   private:
    Importer& importer_;
    std::string_view in_;
    std::size_t pos_ = 0;
    std::string out_;
    int depth_ = 0;

    [[nodiscard]] char peek() const { return pos_ < in_.size() ? in_[pos_] : '\0'; }

    void skip_space() {
      while (pos_ < in_.size() && (in_[pos_] == ' ' || in_[pos_] == '\n' || in_[pos_] == '\t' || in_[pos_] == '\r')) {
        ++pos_;
      }
    }

    void expect(char c) {
      skip_space();
      if (peek() != c) {
        fail(std::string("expected '") + c + "' at offset " + std::to_string(pos_));
      }
      ++pos_;
    }

    /// Consumes a string and returns the text between its quotes, escapes untouched.
    std::string_view string_body() {
      const std::size_t start = ++pos_;  // the opening quote
      while (pos_ < in_.size() && in_[pos_] != '"') {
        pos_ += in_[pos_] == '\\' ? std::size_t{2} : std::size_t{1};
      }
      if (pos_ >= in_.size()) {
        fail("unterminated string at offset " + std::to_string(start - 1));
      }
      return in_.substr(start, pos_++ - start);
    }

    /// Consumes one value of any kind and returns its text. Brackets are only counted; the Parser checks them.
    std::string_view any_value() {
      skip_space();
      const std::size_t start = pos_;
      for (std::size_t open = 0;;) {
        if (pos_ >= in_.size()) {
          fail("the text ends inside a value");
        }
        const char c = in_[pos_];
        if (c == '"') {
          string_body();
        } else if (c == '{' || c == '[') {
          ++open;
          ++pos_;
          continue;
        } else if (c == '}' || c == ']') {
          if (open == 0) {
            break;  // the enclosing bracket: the end of a bare token, or an empty value the Parser reports
          }
          --open;
          ++pos_;
        } else if (open == 0 && (c == ',' || c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
          break;  // the end of a bare token (number, true, null)
        } else {
          ++pos_;
          continue;
        }
        if (open == 0) {
          break;  // a string or a bracketed value is complete
        }
      }
      return in_.substr(start, pos_ - start);
    }

    /// The shape of `[ubyte]` member @p key (@p field) of @p table: a hex string, a nested buffer or plain.
    [[nodiscard]] Shape bytes_shape(const reflection::Object& table, const reflection::Field& field,
                                    std::string_view key) const {
      if (detail::is_hash_field(key)) {
        return {Shape::Kind::Hex};
      }
      if (detail::nested_type_sibling(table, field) != nullptr) {
        return {Shape::Kind::Nested};
      }
      // A palette: a nested buffer of a table the schema fixes, without a `_type` sibling (§6.8.1).
      const auto fixed = detail::fixed_nested_table(deref(table.name()).string_view(), key);
      const auto* sub = fixed.empty() ? nullptr : detail::find_table(importer_.schema_, table, fixed);
      return sub == nullptr ? Shape{} : Shape{Shape::Kind::Nested, sub};
    }

    [[nodiscard]] Shape shape_of(const reflection::Object& table, std::string_view key) const {
      if (key.find('\\') != std::string_view::npos) {
        return {};  // an escaped key is no schema name the rewrite knows; the Parser decides
      }
      const std::string name(key);
      const auto* field = deref(table.fields()).LookupByKey(name.c_str());
      if (field == nullptr) {
        return {};
      }
      const auto& type = deref(field->type());
      const auto& objects = deref(importer_.schema_.objects());
      const auto table_at = [&](std::int32_t index) -> const reflection::Object* {
        const auto* object = index >= 0 && static_cast<flatbuffers::uoffset_t>(index) < objects.size()
                                 ? objects.Get(static_cast<flatbuffers::uoffset_t>(index))
                                 : nullptr;
        return object != nullptr && !object->is_struct() ? object : nullptr;
      };
      switch (type.base_type()) {
        case BaseType::String:
          return {key.ends_with(kTypeSuffix) ? Shape::Kind::TypeName : Shape::Kind::Other};
        case BaseType::Obj:
          if (const auto* sub = table_at(type.index()); sub != nullptr) {
            return {Shape::Kind::Table, sub};
          }
          return {};
        case BaseType::Vector:
          if (type.element() == BaseType::Obj) {
            if (const auto* sub = table_at(type.index()); sub != nullptr) {
              return {Shape::Kind::TableVector, sub};
            }
          } else if (type.element() == BaseType::UByte) {
            return bytes_shape(table, *field, key);
          }
          return {};
        default:
          return {};
      }
    }

    void hex(std::string_view digits) {
      if (digits.size() % 2 != 0) {
        fail("a hex string has an odd number of digits");
      }
      Bytes bytes(digits.size() / 2);
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        const int high = hex_digit(digits[2 * i]);
        const int low = hex_digit(digits[2 * i + 1]);
        if (high < 0 || low < 0) {
          fail("a hex string has a character that is not a hex digit");
        }
        bytes[i] = static_cast<std::uint8_t>(high * 16 + low);
      }
      append_bytes(out_, bytes);
    }

    void table_vector(const reflection::Object& element) {
      out_ += in_[pos_++];  // [
      for (bool first = true;; first = false) {
        skip_space();
        if (peek() == ']') {
          out_ += in_[pos_++];
          return;
        }
        if (!first) {
          expect(',');
          out_ += ',';
          skip_space();
        }
        if (peek() == '{') {
          object(element);
        } else {
          out_ += any_value();
        }
      }
    }

    /// Writes `"key":`, after a comma when the object already has a member.
    void member(std::string_view key, bool& written) {
      if (written) {
        out_ += ',';
      }
      written = true;
      out_ += '"';
      out_ += key;
      out_ += "\":";
    }

    using TypeNames = std::vector<std::pair<std::string_view, std::string_view>>;  ///< `X_type` key, its value

    /// The table that `<key>_type` names among the string members @p type_names of an object of @p table.
    [[nodiscard]] const reflection::Object& nested_table(const reflection::Object& table, std::string_view key,
                                                         const TypeNames& type_names) const {
      std::string_view type_name;
      for (const auto& [type_key, value] : type_names) {
        if (type_key.size() == key.size() + kTypeSuffix.size() && type_key.starts_with(key)) {
          type_name = value;
        }
      }
      const auto* found = detail::find_table(importer_.schema_, table, type_name);
      if (found == nullptr) {
        fail("an object is given for '" + std::string(key) + "' but '" + std::string(key) + "_type' names no table");
      }
      return *found;
    }

    /// Rewrites the value at the cursor, of member @p key with @p shape; a string under an `X_type` key is
    /// noted in @p type_names.
    void value(const Shape& shape, std::string_view key, TypeNames& type_names) {
      const char c = peek();
      if (shape.kind == Shape::Kind::Nested && shape.table != nullptr && c == '{') {  // a fixed table: no sibling
        append_bytes(out_, importer_.nested(any_value(), *shape.table));
      } else if (shape.kind == Shape::Kind::Table && c == '{') {
        object(*shape.table);
      } else if (shape.kind == Shape::Kind::TableVector && c == '[') {
        table_vector(*shape.table);
      } else if (shape.kind == Shape::Kind::Hex && c == '"') {
        hex(string_body());
      } else {
        const auto text = any_value();
        if (shape.kind == Shape::Kind::TypeName && text.size() >= 2 && text.front() == '"') {
          type_names.emplace_back(key, text.substr(1, text.size() - 2));
        }
        out_ += text;
      }
    }

    /// Rewrites the object at the cursor as a table of type @p table.
    void object(const reflection::Object& table) {
      if (++depth_ > kMaxDepth) {
        fail("nesting deeper than " + std::to_string(kMaxDepth) + " levels");
      }
      std::vector<Pending> pending;
      TypeNames type_names;
      out_ += in_[pos_++];   // {
      bool written = false;  // a member has been written to out_
      for (bool first = true;; first = false) {
        skip_space();
        if (peek() == '}') {
          break;
        }
        if (!first) {
          expect(',');
          skip_space();
        }
        if (peek() != '"') {
          fail("expected a quoted key at offset " + std::to_string(pos_));
        }
        const auto key = string_body();
        expect(':');
        skip_space();
        const Shape shape = shape_of(table, key);
        const char c = peek();
        if (shape.kind == Shape::Kind::Nested && shape.table == nullptr && c == '{') {
          pending.push_back({key, any_value()});  // written at the closing brace, when `X_type` is known
          continue;
        }
        member(key, written);
        value(shape, key, type_names);
      }
      for (const auto& nested : pending) {
        member(nested.key, written);
        append_bytes(out_, importer_.nested(nested.value, nested_table(table, nested.key, type_names)));
      }
      out_ += in_[pos_++];  // }
      --depth_;
    }
  };

  /// The bytes of the nested buffer whose JSON object is @p json: no identifier (§6.3.3).
  Bytes nested(std::string_view json, const reflection::Object& object) {
    if (nesting_ == detail::kMaxNestedBuffers) {
      fail("nested buffers deeper than " + std::to_string(detail::kMaxNestedBuffers) + " levels");
    }
    ++nesting_;
    Bytes bytes = parse(json, object, {});
    --nesting_;
    return bytes;
  }
};
// NOLINTEND(misc-no-recursion)

}  // namespace

std::vector<std::uint8_t> from_json(std::string_view json, SchemaId schema, std::string_view root_table) {
  const auto resolved = detail::resolve_root(schema, root_table, "from_json");
  if (json.find('\0') != std::string_view::npos) {
    fail("the text contains a NUL character");
  }
  // §6.1.1: the schema's identifier belongs to its root type; other root tables get theirs from the writer
  // that stores them (the cache writes "OVTI" for a StreamIndex).
  std::string_view identifier;
  if (const auto* ident = resolved.schema->file_ident(); ident != nullptr && root_table == root_table_name(schema)) {
    identifier = ident->string_view();
  }
  Importer importer(schema, *resolved.schema);
  return canonicalize(importer.parse(json, *resolved.root, identifier), schema, root_table);
}

}  // namespace ovt::model
