// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Canonical builder (rule V-032) and the structural FlatBuffers diff behind `ovt dump --diff` (chapter 06
// WI-0.085, chapter 12 §12.2.6). Both walk the object tree through the embedded binary schema
// (flatbuffers::reflection), so they serve all three schemas and every root table without generated code.
//
// Canonical form = the bytes the flatc object API (`T::Pack`) writes for the same values with ForceDefaults
// off, which is how every golden fixture is built (WI-0.093). flatbuffers::CopyTable is not used: it adds
// fields in name order and does not know about nested buffers, so its output differs from a native producer.
// What Pack does, and what copy_table() below repeats:
//   1. every child (string, vector, sub-table) is created first, in field-id order, depth first;
//   2. a string or vector that is empty is not written at all (unless the field is `required`);
//   3. the table is then started and its fields added in passes of size 8, 4, 2, 1 bytes (offsets and
//      structs count as 4), each pass in reverse field-id order; scalars equal to their default are skipped.
// Vectors are copied in their existing order: §6.3.7 orders are checked by the validator, not imposed here.
//
// SPEC-ASSUMPTION(#271): an empty, non-required string or vector is canonically absent (object API
// behaviour), and struct padding bytes are canonically zero.

#include "ovt/cxx/model/canonical.hpp"

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <flatbuffers/reflection_generated.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace ovt::model {
namespace {

static_assert(FLATBUFFERS_LITTLEENDIAN, "canonical.cpp copies scalar vectors as raw little-endian bytes");

using reflection::BaseType;

constexpr std::string_view kTypeSuffix = "_type";
constexpr std::size_t kFileIdentifierOffset = sizeof(flatbuffers::uoffset_t);
/// Nested buffers inside nested buffers. The OVT schemas nest one level (Block.ext, FrameInfo.ref_state,
/// FrameAnalysis.filter_params); a `_type` string may name any table, including the root, so untrusted input
/// could chain levels without bound (each with its own verifier and copy). Deeper input is rejected.
constexpr int kMaxNesting = 4;

[[noreturn]] void too_deep(const char* caller, const std::string& path) {
  throw std::invalid_argument(std::string(caller) + ": nested buffers deeper than " + std::to_string(kMaxNesting) +
                              " levels at " + path);
}

// ---- schema helpers -------------------------------------------------------------------------------------

[[noreturn]] void malformed_schema() {
  throw std::invalid_argument("canonical: malformed embedded .bfbs");
}

/// Reflection accessors return nullptr for an absent field; the embedded schemas always carry these.
const reflection::Type& type_of(const reflection::Field& field) {
  const auto* type = field.type();
  if (type == nullptr) {
    malformed_schema();
  }
  return *type;
}

template <class T>
std::string name_of(const T& node) {
  const auto* name = node.name();
  return name == nullptr ? std::string{} : name->str();
}

const flatbuffers::Vector<flatbuffers::Offset<reflection::Field>>& fields_of(const reflection::Object& object) {
  const auto* fields = object.fields();
  if (fields == nullptr) {
    malformed_schema();
  }
  return *fields;
}

const flatbuffers::Vector<flatbuffers::Offset<reflection::Object>>& objects_of(const reflection::Schema& schema) {
  const auto* objects = schema.objects();
  if (objects == nullptr) {
    malformed_schema();
  }
  return *objects;
}

/// The fields of @p object in field-id order (= declaration order; the .bfbs stores them sorted by name).
std::vector<const reflection::Field*> fields_by_id(const reflection::Object& object) {
  const auto& all = fields_of(object);
  std::vector<const reflection::Field*> fields(all.begin(), all.end());
  std::sort(fields.begin(), fields.end(),
            [](const reflection::Field* lhs, const reflection::Field* rhs) { return lhs->id() < rhs->id(); });
  return fields;
}

bool is_scalar(BaseType type) {
  return type >= BaseType::UType && type <= BaseType::Double;
}

/// Size class of a field in the Pack add order: scalars by width, offsets and structs 4.
std::size_t add_size(const reflection::Field& field) {
  const BaseType type = type_of(field).base_type();
  return is_scalar(type) ? flatbuffers::GetTypeSize(type) : sizeof(flatbuffers::uoffset_t);
}

const reflection::Object& object_at(const reflection::Schema& schema, std::int32_t index) {
  const auto& objects = objects_of(schema);
  const auto* object = index >= 0 && static_cast<flatbuffers::uoffset_t>(index) < objects.size()
                           ? objects.Get(static_cast<flatbuffers::uoffset_t>(index))
                           : nullptr;
  if (object == nullptr) {
    throw std::invalid_argument("canonical: malformed .bfbs (object index)");
  }
  return *object;
}

/// Looks up @p name ("HevcCu" or "ovt.fb.HevcCu") as a table, trying the namespace of @p context first.
const reflection::Object* lookup_table(const reflection::Schema& schema, const reflection::Object& context,
                                       std::string_view name) {
  if (name.empty()) {
    return nullptr;
  }
  const std::string context_name = name_of(context);
  const auto dot = context_name.rfind('.');
  if (dot != std::string_view::npos && name.find('.') == std::string_view::npos) {
    std::string qualified(context_name.substr(0, dot + 1));
    qualified.append(name);
    if (const auto* object = objects_of(schema).LookupByKey(qualified.c_str());
        object != nullptr && !object->is_struct()) {
      return object;
    }
  }
  const std::string plain(name);
  const auto* object = objects_of(schema).LookupByKey(plain.c_str());
  return object != nullptr && !object->is_struct() ? object : nullptr;
}

/// For a `[ubyte]` field `X`, the sibling string field `X_type` that names its nested table, else nullptr.
const reflection::Field* nested_type_field(const reflection::Object& object, const reflection::Field& field) {
  const auto* type = &type_of(field);
  if (type->base_type() != BaseType::Vector || type->element() != BaseType::UByte) {
    return nullptr;
  }
  std::string name = name_of(field);
  name.append(kTypeSuffix);
  const auto* sibling = fields_of(object).LookupByKey(name.c_str());
  return sibling != nullptr && type_of(*sibling).base_type() == BaseType::String ? sibling : nullptr;
}

std::string_view string_field(const flatbuffers::Table& table, const reflection::Field& field) {
  const auto* str = table.GetPointer<const flatbuffers::String*>(field.offset());
  return str == nullptr ? std::string_view{} : str->string_view();
}

template <class T>
const flatbuffers::Vector<T>* vector_field(const flatbuffers::Table& table, const reflection::Field& field) {
  return table.GetPointer<const flatbuffers::Vector<T>*>(field.offset());
}

using ByteView = std::span<const std::uint8_t>;
using MutableBytes = std::span<std::uint8_t>;

/// The element bytes of vector @p field (elements of @p elem_size bytes); empty when absent.
ByteView vector_bytes(const flatbuffers::Table& table, const reflection::Field& field, std::size_t elem_size) {
  const auto* vector = vector_field<std::uint8_t>(table, field);
  return vector == nullptr ? ByteView{} : ByteView(vector->Data(), std::size_t{vector->size()} * elem_size);
}

flatbuffers::uoffset_t vector_size(const flatbuffers::Table& table, const reflection::Field& field) {
  const auto* vector = vector_field<std::uint8_t>(table, field);
  return vector == nullptr ? 0 : vector->size();
}

/// The bytes of struct field @p field (of struct type @p object); empty when absent.
ByteView struct_bytes(const flatbuffers::Table& table, const reflection::Field& field,
                      const reflection::Object& object) {
  const auto* data = table.GetStruct<const std::uint8_t*>(field.offset());
  return data == nullptr ? ByteView{} : ByteView(data, static_cast<std::size_t>(object.bytesize()));
}

/// Byte size of one element of an array or vector of type @p type.
std::size_t element_size(const reflection::Schema& schema, const reflection::Type& type);

/// A buffer copied into 8-byte-aligned storage, so that reflection reads of 8-byte scalars are aligned even
/// when the caller's span (or a nested buffer inside a `[ubyte]` vector) is only 4-byte aligned.
class AlignedCopy {
 public:
  explicit AlignedCopy(std::span<const std::uint8_t> bytes)
      : words_((bytes.size() + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t)), size_(bytes.size()) {
    if (!bytes.empty()) {
      std::memcpy(words_.data(), bytes.data(), bytes.size());
    }
  }
  [[nodiscard]] const std::uint8_t* data() const { return reinterpret_cast<const std::uint8_t*>(words_.data()); }
  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] ByteView bytes() const { return {data(), size_}; }

 private:
  std::vector<std::uint64_t> words_;
  std::size_t size_;
};

// Reflection verifier. flatbuffers::Verify (reflection.cpp, v25.12.19) reads a sub-table field's offset before
// it checks that the offset slot lies inside the buffer, an out-of-bounds read that fuzz_canonical found. This
// verifier walks the schema the same way but checks every slot first (Table::VerifyOffset), as generated
// verifiers do, and it enforces `required`. Unions and 64-bit vectors, which the OVT schemas do not use, fail.
// The depth is bounded by the Verifier's max_depth (VerifyTableStart / EndTable), hence the recursion is too.
// NOLINTBEGIN(misc-no-recursion)
class SafeVerifier {
 public:
  SafeVerifier(const reflection::Schema& schema, ByteView buffer)
      : schema_(schema), verifier_(buffer.data(), buffer.size()) {}

  bool table(const reflection::Object& object, const flatbuffers::Table* table) {
    if (table == nullptr || !table->VerifyTableStart(verifier_)) {
      return false;
    }
    for (const auto* field : fields_of(object)) {
      if (!this->field(*table, *field)) {
        return false;
      }
    }
    return verifier_.EndTable();
  }

 private:
  const reflection::Schema& schema_;
  flatbuffers::Verifier verifier_;

  bool scalar(const flatbuffers::Table& table, flatbuffers::voffset_t offset, std::size_t size) const {
    switch (size) {
      case 1:
        return table.VerifyField<std::uint8_t>(verifier_, offset, 1);
      case 2:
        return table.VerifyField<std::uint16_t>(verifier_, offset, 2);
      case 4:
        return table.VerifyField<std::uint32_t>(verifier_, offset, 4);
      default:
        return table.VerifyField<std::uint64_t>(verifier_, offset, 8);
    }
  }

  bool vector(const flatbuffers::Table& table, const reflection::Field& field, const reflection::Type& type) {
    const auto* raw = table.GetPointer<const std::uint8_t*>(field.offset());
    if (raw == nullptr) {
      return true;
    }
    if (type.element() == BaseType::String) {
      const auto* strings = vector_field<flatbuffers::Offset<flatbuffers::String>>(table, field);
      return verifier_.VerifyVector(strings) && verifier_.VerifyVectorOfStrings(strings);
    }
    if (type.element() == BaseType::Obj && !object_at(schema_, type.index()).is_struct()) {
      const auto* tables = vector_field<flatbuffers::Offset<flatbuffers::Table>>(table, field);
      if (!verifier_.VerifyVector(tables)) {
        return false;
      }
      for (flatbuffers::uoffset_t i = 0; i < tables->size(); ++i) {
        if (!this->table(object_at(schema_, type.index()), tables->Get(i))) {
          return false;
        }
      }
      return true;
    }
    if (type.element() != BaseType::Obj && !is_scalar(type.element())) {
      return false;
    }
    return verifier_.VerifyVectorOrString(raw, element_size(schema_, type));
  }

  bool field(const flatbuffers::Table& table, const reflection::Field& field) {
    const auto& type = type_of(field);
    const auto offset = field.offset();
    if (is_scalar(type.base_type())) {
      return scalar(table, offset, flatbuffers::GetTypeSize(type.base_type()));
    }
    if (field.required() && !table.CheckField(offset)) {
      return false;
    }
    if (type.base_type() == BaseType::Obj && object_at(schema_, type.index()).is_struct()) {
      const auto& object = object_at(schema_, type.index());
      const auto field_offset = table.GetOptionalFieldOffset(offset);
      return field_offset == 0 || verifier_.VerifyFieldStruct(reinterpret_cast<const std::uint8_t*>(&table),
                                                              field_offset, static_cast<std::size_t>(object.bytesize()),
                                                              static_cast<std::size_t>(object.minalign()));
    }
    if (!table.VerifyOffset(verifier_, offset)) {  // the slot and the target offset, before any read
      return false;
    }
    switch (type.base_type()) {
      case BaseType::String:
        return verifier_.VerifyString(table.GetPointer<const flatbuffers::String*>(offset));
      case BaseType::Vector:
        return vector(table, field, type);
      case BaseType::Obj: {
        const auto* child = table.GetPointer<const flatbuffers::Table*>(offset);
        return child == nullptr || this->table(object_at(schema_, type.index()), child);
      }
      default:
        return false;
    }
  }
};
// NOLINTEND(misc-no-recursion)

bool verifies(const reflection::Schema& schema, const reflection::Object& object, const AlignedCopy& copy) {
  const auto bytes = copy.bytes();
  if (bytes.size() < 2 * sizeof(flatbuffers::uoffset_t) ||
      flatbuffers::ReadScalar<flatbuffers::uoffset_t>(copy.data()) > bytes.size() - sizeof(flatbuffers::soffset_t)) {
    return false;
  }
  return SafeVerifier(schema, bytes).table(object, flatbuffers::GetAnyRoot(copy.data()));
}

struct Resolved {
  const reflection::Schema* schema;
  const reflection::Object* root;
};

Resolved resolve(SchemaId schema_id, std::string_view root_table, const char* caller) {
  const auto bfbs_bytes = bfbs(schema_id);
  if (bfbs_bytes.empty()) {
    throw std::invalid_argument(std::string(caller) + ": unknown schema id");
  }
  const auto* schema = reflection::GetSchema(bfbs_bytes.data());
  const std::string name(root_table);
  const auto* root = objects_of(*schema).LookupByKey(name.c_str());
  if (root == nullptr || root->is_struct()) {
    throw std::invalid_argument(std::string(caller) + ": unknown root table '" + name + "'");
  }
  return {schema, root};
}

std::size_t element_size(const reflection::Schema& schema, const reflection::Type& type) {
  return type.element() == BaseType::Obj ? static_cast<std::size_t>(object_at(schema, type.index()).bytesize())
                                         : flatbuffers::GetTypeSize(type.element());
}

// ---- canonical builder ----------------------------------------------------------------------------------

// The walkers below recurse along the object tree. The depth is bounded: every buffer passes
// flatbuffers::Verify (max_depth 64) before it is walked, and nested buffers are verified the same way.
// NOLINTBEGIN(misc-no-recursion)

class Canonicalizer {
 public:
  explicit Canonicalizer(const reflection::Schema& schema) : schema_(schema) {}

  /// Canonical bytes of a finished buffer of @p object; @p identifier is written when non-null.
  std::vector<std::uint8_t> finish(const reflection::Object& object, const AlignedCopy& buffer, const char* identifier,
                                   const std::string& path) {
    if (!verifies(schema_, object, buffer)) {
      throw std::invalid_argument("canonicalize: " + (path.empty() ? std::string("buffer") : path) +
                                  " is not a valid " + name_of(object));
    }
    flatbuffers::FlatBufferBuilder builder;  // ForceDefaults off, vtable deduplication on (as Pack's caller)
    const auto root = copy_table(builder, object, *flatbuffers::GetAnyRoot(buffer.data()), path);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(root), identifier);
    const auto span = builder.GetBufferSpan();
    const ByteView out(span.data(), span.size());
    return {out.begin(), out.end()};
  }

 private:
  const reflection::Schema& schema_;
  int nesting_ = 0;  ///< nested-buffer levels above the table being copied

  struct RawVector {
    flatbuffers::uoffset_t offset;
    MutableBytes bytes;  ///< the element bytes inside the builder, to be filled before the next builder call
  };

  /// A vector of @p count elements of @p elem_size bytes, aligned as the generated code aligns it.
  static RawVector raw_vector(flatbuffers::FlatBufferBuilder& builder, std::size_t count, std::size_t elem_size,
                              std::size_t alignment) {
    std::uint8_t* out = nullptr;
#if FLATBUFFERS_VERSION_MAJOR >= 23
    const auto offset = builder.CreateUninitializedVector(count, elem_size, alignment, &out);
#else
    (void)alignment;  // flatbuffers 2.x aligns every vector to its element size, as its generated code does
    const auto offset = builder.CreateUninitializedVector(count, elem_size, &out);
#endif
    return {offset, MutableBytes(out, count * elem_size)};
  }

  static flatbuffers::uoffset_t copy_bytes(flatbuffers::FlatBufferBuilder& builder, ByteView bytes,
                                           std::size_t elem_size) {
    const auto vector = raw_vector(builder, bytes.size() / elem_size, elem_size, elem_size);
    std::ranges::copy(bytes, vector.bytes.begin());
    return vector.offset;
  }

  /// Copies one struct value field by field into zeroed storage (canonical padding).
  void copy_struct(const reflection::Object& object, ByteView src, MutableBytes dst) const {
    std::ranges::fill(dst, std::uint8_t{0});
    for (const auto* field : fields_of(object)) {
      const auto& type = type_of(*field);
      const std::size_t offset = field->offset();
      if (type.base_type() == BaseType::Obj) {
        const auto size = static_cast<std::size_t>(object_at(schema_, type.index()).bytesize());
        copy_struct(object_at(schema_, type.index()), src.subspan(offset, size), dst.subspan(offset, size));
        continue;
      }
      const std::size_t size = type.base_type() == BaseType::Array ? element_size(schema_, type) * type.fixed_length()
                                                                   : flatbuffers::GetTypeSize(type.base_type());
      std::ranges::copy(src.subspan(offset, size), dst.subspan(offset, size).begin());
    }
  }

  flatbuffers::uoffset_t copy_table_vector(flatbuffers::FlatBufferBuilder& builder, const reflection::Object& element,
                                           const reflection::Field& field, const flatbuffers::Table& table,
                                           const std::string& path) {
    const auto* src = vector_field<flatbuffers::Offset<flatbuffers::Table>>(table, field);
    std::vector<flatbuffers::Offset<flatbuffers::Table>> offsets;
    for (flatbuffers::uoffset_t i = 0; src != nullptr && i < src->size(); ++i) {
      offsets.emplace_back(copy_table(builder, element, *src->Get(i), path + '[' + std::to_string(i) + ']'));
    }
    return builder.CreateVector(offsets).o;
  }

  static flatbuffers::uoffset_t copy_string_vector(flatbuffers::FlatBufferBuilder& builder,
                                                   const reflection::Field& field, const flatbuffers::Table& table) {
    const auto* src = vector_field<flatbuffers::Offset<flatbuffers::String>>(table, field);
    std::vector<flatbuffers::Offset<flatbuffers::String>> offsets;
    for (flatbuffers::uoffset_t i = 0; src != nullptr && i < src->size(); ++i) {
      offsets.push_back(builder.CreateString(src->Get(i)->c_str(), src->Get(i)->size()));
    }
    return builder.CreateVector(offsets).o;
  }

  flatbuffers::uoffset_t copy_vector(flatbuffers::FlatBufferBuilder& builder, const reflection::Object& object,
                                     const reflection::Field& field, const flatbuffers::Table& table,
                                     const std::string& path) {
    const auto& type = type_of(field);
    const flatbuffers::uoffset_t count = vector_size(table, field);
    if (count == 0 && !field.required()) {
      return 0;  // Pack writes no vector for an empty one
    }
    if (type.element() == BaseType::String) {
      return copy_string_vector(builder, field, table);
    }
    if (type.element() == BaseType::Obj) {
      const auto& element = object_at(schema_, type.index());
      if (!element.is_struct()) {
        return copy_table_vector(builder, element, field, table, path);
      }
      const auto size = static_cast<std::size_t>(element.bytesize());
      const auto src = vector_bytes(table, field, size);
      const auto vector = raw_vector(builder, count, size, static_cast<std::size_t>(element.minalign()));
      for (std::size_t i = 0; i < count; ++i) {
        copy_struct(element, src.subspan(i * size, size), vector.bytes.subspan(i * size, size));
      }
      return vector.offset;
    }
    if (!is_scalar(type.element())) {  // vectors of unions or vectors: not used by the OVT schemas
      throw std::invalid_argument("canonicalize: unsupported vector element type at " + path);
    }
    const std::size_t elem = flatbuffers::GetTypeSize(type.element());
    const auto src = vector_bytes(table, field, elem);
    if (const auto* type_field = nested_type_field(object, field); type_field != nullptr && count != 0) {
      if (const auto* nested = lookup_table(schema_, object, string_field(table, *type_field)); nested != nullptr) {
        if (nesting_ == kMaxNesting) {
          too_deep("canonicalize", path);
        }
        ++nesting_;
        const auto bytes = finish(*nested, AlignedCopy(src), nullptr, path);  // §6.3.3: no identifier
        --nesting_;
        return copy_bytes(builder, bytes, 1);
      }
    }
    return copy_bytes(builder, src, elem);
  }

  /// Pass 1 of copy_table: the string, vector or sub-table of @p field (0 when none is written).
  flatbuffers::uoffset_t copy_child(flatbuffers::FlatBufferBuilder& builder, const reflection::Object& object,
                                    const reflection::Field& field, const flatbuffers::Table& table,
                                    const std::string& path) {
    switch (type_of(field).base_type()) {
      case BaseType::String: {
        const auto text = string_field(table, field);
        return text.empty() && !field.required() ? 0 : builder.CreateString(text.data(), text.size()).o;
      }
      case BaseType::Vector:
        return copy_vector(builder, object, field, table, path);
      case BaseType::Obj: {
        const auto& sub = object_at(schema_, type_of(field).index());
        const auto* child = sub.is_struct() ? nullptr : table.GetPointer<const flatbuffers::Table*>(field.offset());
        return child == nullptr ? 0 : copy_table(builder, sub, *child, path);
      }
      default:
        if (!is_scalar(type_of(field).base_type())) {  // unions, 64-bit vectors: not used by the OVT schemas
          throw std::invalid_argument("canonicalize: unsupported field type at " + path);
        }
        return 0;
    }
  }

  flatbuffers::uoffset_t copy_table(flatbuffers::FlatBufferBuilder& builder, const reflection::Object& object,
                                    const flatbuffers::Table& table, const std::string& path) {
    const auto fields = fields_by_id(object);
    const auto prefix = path.empty() ? std::string{} : path + '.';
    // 1. children, in field-id order
    std::vector<flatbuffers::uoffset_t> children(fields.size(), 0);
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (!fields[i]->deprecated()) {
        children[i] = copy_child(builder, object, *fields[i], table, prefix + name_of(*fields[i]));
      }
    }
    // 2. the table: passes of 8, 4, 2, 1 bytes, each in reverse field-id order
    const auto start = builder.StartTable();
    for (std::size_t size = sizeof(std::uint64_t); size != 0; size /= 2) {
      for (std::size_t i = fields.size(); i-- != 0;) {
        const auto& field = *fields[i];
        if (!field.deprecated() && add_size(field) == size) {
          add_field(builder, field, table, children[i]);
        }
      }
    }
    return builder.EndTable(start);
  }

  template <class T>
  static void add_scalar(flatbuffers::FlatBufferBuilder& builder, const reflection::Field& field,
                         const flatbuffers::Table& table) {
    T def{};
    if constexpr (std::is_floating_point_v<T>) {
      def = static_cast<T>(field.default_real());
    } else {
      def = static_cast<T>(field.default_integer());
    }
    if (field.optional()) {
      if (table.CheckField(field.offset())) {
        builder.AddElement<T>(field.offset(), table.GetField<T>(field.offset(), def));
      }
      return;
    }
    builder.AddElement<T>(field.offset(), table.GetField<T>(field.offset(), def), def);
  }

  void add_struct(flatbuffers::FlatBufferBuilder& builder, const reflection::Object& object,
                  const reflection::Field& field, const flatbuffers::Table& table) const {
    const auto src = struct_bytes(table, field, object);
    if (src.empty()) {
      return;
    }
    std::vector<std::uint8_t> value(src.size());
    copy_struct(object, src, value);
    builder.Align(static_cast<std::size_t>(object.minalign()));
    builder.PushBytes(value.data(), value.size());
    builder.AddStructOffset(field.offset(), builder.GetSize());
  }

  void add_field(flatbuffers::FlatBufferBuilder& builder, const reflection::Field& field,
                 const flatbuffers::Table& table, flatbuffers::uoffset_t child) const {
    switch (type_of(field).base_type()) {
      case BaseType::Bool:
      case BaseType::UType:
      case BaseType::UByte:
        add_scalar<std::uint8_t>(builder, field, table);
        break;
      case BaseType::Byte:
        add_scalar<std::int8_t>(builder, field, table);
        break;
      case BaseType::Short:
        add_scalar<std::int16_t>(builder, field, table);
        break;
      case BaseType::UShort:
        add_scalar<std::uint16_t>(builder, field, table);
        break;
      case BaseType::Int:
        add_scalar<std::int32_t>(builder, field, table);
        break;
      case BaseType::UInt:
        add_scalar<std::uint32_t>(builder, field, table);
        break;
      case BaseType::Long:
        add_scalar<std::int64_t>(builder, field, table);
        break;
      case BaseType::ULong:
        add_scalar<std::uint64_t>(builder, field, table);
        break;
      case BaseType::Float:
        add_scalar<float>(builder, field, table);
        break;
      case BaseType::Double:
        add_scalar<double>(builder, field, table);
        break;
      default:
        if (const auto& type = type_of(field);
            type.base_type() == BaseType::Obj && object_at(schema_, type.index()).is_struct()) {
          add_struct(builder, object_at(schema_, type.index()), field, table);
        } else if (child != 0) {  // string, vector, sub-table: created in pass 1
          builder.AddOffset(field.offset(), flatbuffers::Offset<void>(child));
        }
        break;
    }
  }
};

// ---- structural diff ------------------------------------------------------------------------------------

std::string quote(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (byte < 0x20 || byte == 0x7f) {
      constexpr std::string_view kHex = "0123456789abcdef";
      out += "\\x";
      out += kHex[byte >> 4U];
      out += kHex[byte & 0xfU];
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

template <class T>
std::string format_number(T value) {
  if constexpr (std::is_floating_point_v<T>) {
    if (std::isnan(value)) {
      return "nan";
    }
    if (std::isinf(value)) {
      return value < 0 ? "-inf" : "inf";
    }
  }
  std::array<char, 64> text{};
  const auto result = std::to_chars(text.begin(), text.end(), value);
  return {text.begin(), result.ptr};
}

/// One scalar read from a table field or a struct/vector element, kept wide enough to compare exactly.
struct Scalar {
  bool is_real = false;
  bool is_unsigned = false;
  std::int64_t i = 0;
  std::uint64_t u = 0;
  double d = 0.0;

  [[nodiscard]] bool operator==(const Scalar& other) const {
    if (is_real) {
      return (std::isnan(d) && std::isnan(other.d)) || d == other.d;
    }
    return is_unsigned ? u == other.u : i == other.i;
  }
};

bool is_unsigned_type(BaseType type) {
  return type == BaseType::Bool || type == BaseType::UType || type == BaseType::UByte || type == BaseType::UShort ||
         type == BaseType::UInt || type == BaseType::ULong;
}

/// Reads one little-endian scalar of @p type from the start of @p bytes.
Scalar read_scalar(BaseType type, ByteView bytes) {
  const std::uint8_t* p = bytes.data();
  Scalar s;
  s.is_unsigned = is_unsigned_type(type);
  switch (type) {
    case BaseType::Bool:
    case BaseType::UType:
    case BaseType::UByte:
      s.u = flatbuffers::ReadScalar<std::uint8_t>(p);
      break;
    case BaseType::Byte:
      // NOLINTNEXTLINE(bugprone-signed-char-misuse,cert-str34-c): a signed 8-bit field, sign extension intended
      s.i = flatbuffers::ReadScalar<std::int8_t>(p);
      break;
    case BaseType::Short:
      s.i = flatbuffers::ReadScalar<std::int16_t>(p);
      break;
    case BaseType::UShort:
      s.u = flatbuffers::ReadScalar<std::uint16_t>(p);
      break;
    case BaseType::Int:
      s.i = flatbuffers::ReadScalar<std::int32_t>(p);
      break;
    case BaseType::UInt:
      s.u = flatbuffers::ReadScalar<std::uint32_t>(p);
      break;
    case BaseType::Long:
      s.i = flatbuffers::ReadScalar<std::int64_t>(p);
      break;
    case BaseType::ULong:
      s.u = flatbuffers::ReadScalar<std::uint64_t>(p);
      break;
    case BaseType::Float:
      s.is_real = true;
      s.d = static_cast<double>(flatbuffers::ReadScalar<float>(p));
      break;
    case BaseType::Double:
      s.is_real = true;
      s.d = flatbuffers::ReadScalar<double>(p);
      break;
    default:
      break;
  }
  return s;
}

/// The value of scalar @p field of @p table, or its default when absent.
Scalar table_scalar(const flatbuffers::Table& table, const reflection::Field& field) {
  const BaseType type = type_of(field).base_type();
  if (const auto* p = table.GetAddressOf(field.offset()); p != nullptr) {
    return read_scalar(type, ByteView(p, flatbuffers::GetTypeSize(type)));
  }
  Scalar s;
  s.is_unsigned = is_unsigned_type(type);
  if (type == BaseType::Float || type == BaseType::Double) {
    s.is_real = true;
    s.d = field.default_real();
  } else if (s.is_unsigned) {
    s.u = static_cast<std::uint64_t>(field.default_integer());
  } else {
    s.i = field.default_integer();
  }
  return s;
}

std::string indexed(const std::string& path, std::size_t index) {
  return path + '[' + std::to_string(index) + ']';
}

class Differ {
 public:
  Differ(const reflection::Schema& schema, std::vector<std::string>& out) : schema_(schema), out_(out) {}

  void tables(const reflection::Object& object, const flatbuffers::Table& a, const flatbuffers::Table& b,
              const std::string& path) {
    const auto prefix = path.empty() ? std::string{} : path + '.';
    for (const auto* field : fields_by_id(object)) {
      if (field->deprecated()) {
        continue;
      }
      const auto field_path = prefix + name_of(*field);
      const auto& type = type_of(*field);
      switch (type.base_type()) {
        case BaseType::String:
          if (string_field(a, *field) != string_field(b, *field)) {
            report(field_path, quote(string_field(a, *field)), quote(string_field(b, *field)));
          }
          break;
        case BaseType::Vector:
          vectors(object, *field, a, b, field_path);
          break;
        case BaseType::Obj:
          objects(object_at(schema_, type.index()), *field, a, b, field_path);
          break;
        default:
          if (!is_scalar(type.base_type())) {  // unions, 64-bit vectors: not used by the OVT schemas
            throw std::invalid_argument("structural_diff: unsupported field type at " + field_path);
          }
          compare(field_path, type, false, table_scalar(a, *field), table_scalar(b, *field));
          break;
      }
    }
  }

 private:
  const reflection::Schema& schema_;
  std::vector<std::string>& out_;
  int nesting_ = 0;  ///< nested-buffer levels above the tables being compared

  static std::string presence(bool present) { return present ? "<present>" : "<absent>"; }

  void report(const std::string& path, const std::string& a, const std::string& b) {
    out_.push_back(path + ": " + a + " != " + b);
  }

  void compare(const std::string& path, const reflection::Type& type, bool element, const Scalar& a, const Scalar& b) {
    if (!(a == b)) {
      report(path, format(type, a, element), format(type, b, element));
    }
  }

  /// The enum value name of @p key in enum @p index, or empty.
  [[nodiscard]] std::string enum_name(std::int32_t index, std::int64_t key) const {
    const auto* enums = schema_.enums();
    if (enums == nullptr || index < 0 || static_cast<flatbuffers::uoffset_t>(index) >= enums->size()) {
      return {};
    }
    const auto* values = enums->Get(static_cast<flatbuffers::uoffset_t>(index))->values();
    if (values == nullptr) {
      return {};
    }
    for (const auto* value : *values) {
      if (value->value() == key) {
        return name_of(*value);
      }
    }
    return {};
  }

  /// Formats @p value of scalar type @p type (a field type, or a vector's element type with @p element set).
  [[nodiscard]] std::string format(const reflection::Type& type, const Scalar& value, bool element) const {
    const BaseType base = element ? type.element() : type.base_type();
    if (base == BaseType::Bool) {
      return value.u != 0 ? "true" : "false";
    }
    if (value.is_real) {
      return base == BaseType::Float ? format_number(static_cast<float>(value.d)) : format_number(value.d);
    }
    const std::int64_t key = value.is_unsigned ? static_cast<std::int64_t>(value.u) : value.i;
    if (auto name = enum_name(type.index(), key); !name.empty()) {
      return name;
    }
    return value.is_unsigned ? format_number(value.u) : format_number(value.i);
  }

  /// A struct or sub-table field present in both, or reported as present in one only.
  void objects(const reflection::Object& sub, const reflection::Field& field, const flatbuffers::Table& a,
               const flatbuffers::Table& b, const std::string& path) {
    if (sub.is_struct()) {
      const auto sa = struct_bytes(a, field, sub);
      const auto sb = struct_bytes(b, field, sub);
      if (!sa.empty() && !sb.empty()) {
        structs(sub, sa, sb, path);
      } else if (sa.empty() != sb.empty()) {
        report(path, presence(!sa.empty()), presence(!sb.empty()));
      }
      return;
    }
    const auto* ta = a.GetPointer<const flatbuffers::Table*>(field.offset());
    const auto* tb = b.GetPointer<const flatbuffers::Table*>(field.offset());
    if (ta != nullptr && tb != nullptr) {
      tables(sub, *ta, *tb, path);
    } else if ((ta == nullptr) != (tb == nullptr)) {
      report(path, presence(ta != nullptr), presence(tb != nullptr));
    }
  }

  void structs(const reflection::Object& object, ByteView a, ByteView b, const std::string& path) {
    for (const auto* field : fields_by_id(object)) {
      const auto& type = type_of(*field);
      const auto field_path = path + '.' + name_of(*field);
      const std::size_t offset = field->offset();
      if (type.base_type() == BaseType::Obj) {
        const auto& sub = object_at(schema_, type.index());
        const auto size = static_cast<std::size_t>(sub.bytesize());
        structs(sub, a.subspan(offset, size), b.subspan(offset, size), field_path);
      } else if (type.base_type() == BaseType::Array) {
        elements(type, a.subspan(offset), b.subspan(offset), type.fixed_length(), field_path);
      } else {
        compare(field_path, type, false, read_scalar(type.base_type(), a.subspan(offset)),
                read_scalar(type.base_type(), b.subspan(offset)));
      }
    }
  }

  /// The first @p count inline elements (structs or scalars) of an array or vector of type @p type.
  void elements(const reflection::Type& type, ByteView a, ByteView b, std::size_t count, const std::string& path) {
    const std::size_t size = element_size(schema_, type);
    for (std::size_t i = 0; i < count; ++i) {
      const auto ea = a.subspan(i * size, size);
      const auto eb = b.subspan(i * size, size);
      if (type.element() == BaseType::Obj) {
        structs(object_at(schema_, type.index()), ea, eb, indexed(path, i));
      } else {
        compare(indexed(path, i), type, true, read_scalar(type.element(), ea), read_scalar(type.element(), eb));
      }
    }
  }

  /// Nested buffers of the same named table that both verify: diffed as tables. Returns false otherwise.
  bool nested(const reflection::Object& object, const reflection::Field& field, const flatbuffers::Table& a,
              const flatbuffers::Table& b, const std::string& path) {
    const auto* type_field = nested_type_field(object, field);
    if (type_field == nullptr || string_field(a, *type_field) != string_field(b, *type_field)) {
      return false;  // a differing `_type` is reported by its own field; the bytes are compared as they are
    }
    const auto* table_object = lookup_table(schema_, object, string_field(a, *type_field));
    if (table_object == nullptr) {
      return false;
    }
    if (nesting_ == kMaxNesting) {
      too_deep("structural_diff", path);
    }
    const AlignedCopy ca(vector_bytes(a, field, 1));
    const AlignedCopy cb(vector_bytes(b, field, 1));
    if (!verifies(schema_, *table_object, ca) || !verifies(schema_, *table_object, cb)) {
      return false;
    }
    ++nesting_;
    tables(*table_object, *flatbuffers::GetAnyRoot(ca.data()), *flatbuffers::GetAnyRoot(cb.data()), path);
    --nesting_;
    return true;
  }

  void vectors(const reflection::Object& object, const reflection::Field& field, const flatbuffers::Table& a,
               const flatbuffers::Table& b, const std::string& path) {
    if (nested(object, field, a, b, path)) {
      return;
    }
    const auto& type = type_of(field);
    const flatbuffers::uoffset_t na = vector_size(a, field);
    const flatbuffers::uoffset_t nb = vector_size(b, field);
    if (na != nb) {
      report(path + ".length", format_number(na), format_number(nb));
    }
    const flatbuffers::uoffset_t common = std::min(na, nb);
    if (type.element() == BaseType::String) {
      const auto* sa = vector_field<flatbuffers::Offset<flatbuffers::String>>(a, field);
      const auto* sb = vector_field<flatbuffers::Offset<flatbuffers::String>>(b, field);
      for (flatbuffers::uoffset_t i = 0; i < common; ++i) {
        if (sa->Get(i)->string_view() != sb->Get(i)->string_view()) {
          report(indexed(path, i), quote(sa->Get(i)->string_view()), quote(sb->Get(i)->string_view()));
        }
      }
    } else if (type.element() == BaseType::Obj && !object_at(schema_, type.index()).is_struct()) {
      const auto* ta = vector_field<flatbuffers::Offset<flatbuffers::Table>>(a, field);
      const auto* tb = vector_field<flatbuffers::Offset<flatbuffers::Table>>(b, field);
      for (flatbuffers::uoffset_t i = 0; i < common; ++i) {
        tables(object_at(schema_, type.index()), *ta->Get(i), *tb->Get(i), indexed(path, i));
      }
    } else if (type.element() == BaseType::Obj || is_scalar(type.element())) {
      const std::size_t size = element_size(schema_, type);
      elements(type, vector_bytes(a, field, size), vector_bytes(b, field, size), common, path);
    } else {
      throw std::invalid_argument("structural_diff: unsupported vector element type at " + path);
    }
  }
};

// NOLINTEND(misc-no-recursion)

}  // namespace

std::vector<std::uint8_t> canonicalize(std::span<const std::uint8_t> buffer, SchemaId schema,
                                       std::string_view root_table) {
  const auto resolved = resolve(schema, root_table, "canonicalize");
  const AlignedCopy copy(buffer);
  // The schema's identifier is kept when the buffer carries it (root tables, §6.1.1); never invented.
  const char* identifier = nullptr;
  const auto* ident = resolved.schema->file_ident();
  if (ident != nullptr && ident->size() == flatbuffers::kFileIdentifierLength &&
      copy.size() >= kFileIdentifierOffset + flatbuffers::kFileIdentifierLength &&
      ident->string_view() ==
          std::string_view(reinterpret_cast<const char*>(copy.bytes().subspan(kFileIdentifierOffset).data()),
                           flatbuffers::kFileIdentifierLength)) {
    identifier = ident->c_str();
  }
  return Canonicalizer(*resolved.schema).finish(*resolved.root, copy, identifier, {});
}

std::vector<std::string> structural_diff(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b,
                                         SchemaId schema, std::string_view root_table) {
  const auto resolved = resolve(schema, root_table, "structural_diff");
  const AlignedCopy ca(a);
  const AlignedCopy cb(b);
  if (!verifies(*resolved.schema, *resolved.root, ca)) {
    throw std::invalid_argument("structural_diff: buffer A is not a valid " + std::string(root_table));
  }
  if (!verifies(*resolved.schema, *resolved.root, cb)) {
    throw std::invalid_argument("structural_diff: buffer B is not a valid " + std::string(root_table));
  }
  std::vector<std::string> out;
  Differ(*resolved.schema, out)
      .tables(*resolved.root, *flatbuffers::GetAnyRoot(ca.data()), *flatbuffers::GetAnyRoot(cb.data()), {});
  return out;
}

}  // namespace ovt::model
