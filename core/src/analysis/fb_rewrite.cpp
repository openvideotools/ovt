// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// See fb_rewrite.hpp. Mirrors flatbuffers::CopyTable (reflection.cpp) with an override hook.

#include "fb_rewrite.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

#include "ovt/cxx/model/schema_id.hpp"

namespace ovt::analysis::detail {
namespace {

bool is_offset_field(const reflection::Schema& schema, const reflection::Field& field) {
  switch (field.type()->base_type()) {
    case reflection::String:
    case reflection::Vector:
    case reflection::Union:
      return true;
    case reflection::Obj:
      return !schema.objects()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()))->is_struct();
    default:
      return false;
  }
}

void push_inline(flatbuffers::FlatBufferBuilder& fbb, const reflection::Field& field, const uint8_t* bytes,
                 std::size_t size, std::size_t align) {
  fbb.Align(align);
  fbb.PushBytes(bytes, size);
  fbb.TrackField(field.offset(), fbb.GetSize());
}

void write_struct_field(flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema& schema,
                        const reflection::Field& field, const flatbuffers::Table* table, const FieldOverrides& ov) {
  const auto* sub = schema.objects()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()));
  const auto align = static_cast<std::size_t>(sub->minalign());
  const auto size = static_cast<std::size_t>(sub->bytesize());
  if (auto it = ov.structs.find(field.name()->string_view()); it != ov.structs.end() && it->second.size() == size) {
    push_inline(fbb, field, it->second.data(), size, align);
  } else if (table != nullptr && table->GetStruct<const uint8_t*>(field.offset()) != nullptr) {
    push_inline(fbb, field, table->GetStruct<const uint8_t*>(field.offset()), size, align);
  }
}

void write_scalar_field(flatbuffers::FlatBufferBuilder& fbb, const reflection::Field& field,
                        const flatbuffers::Table* table, const FieldOverrides& ov) {
  const std::size_t size = flatbuffers::GetTypeSize(field.type()->base_type());
  if (auto it = ov.scalars.find(field.name()->string_view()); it != ov.scalars.end()) {
    if (it->second != field.default_integer()) {
      const auto value = static_cast<uint64_t>(it->second);
      std::vector<uint8_t> bytes(size);
      for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * i));  // little-endian wire format
      }
      push_inline(fbb, field, bytes.data(), size, size);
    }
  } else if (table != nullptr && table->GetAddressOf(field.offset()) != nullptr) {
    push_inline(fbb, field, table->GetAddressOf(field.offset()), size, size);
  }
}

// One vector of scalars or structs, copied byte for byte. FlatBuffers 23 added the alignment argument and
// deprecated the old overload; the local system package can still be 2.x.
flatbuffers::uoffset_t copy_raw_vector(flatbuffers::FlatBufferBuilder& fbb, const flatbuffers::VectorOfAny& v,
                                       std::size_t elem_size, std::size_t align) {
  uint8_t* dst = nullptr;
#if FLATBUFFERS_VERSION_MAJOR >= 23
  const auto off = fbb.CreateUninitializedVector(v.size(), elem_size, align, &dst);
#else
  fbb.ForceVectorAlignment(v.size(), elem_size, align);
  const auto off = fbb.CreateUninitializedVector(v.size(), elem_size, &dst);
#endif
  std::memcpy(dst, v.Data(), v.size() * elem_size);
  return off;
}

flatbuffers::uoffset_t copy_vector(flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema& schema,
                                   const reflection::Field& field, const flatbuffers::Table& table) {
  const auto elem = field.type()->element();
  if (elem == reflection::String) {
    const auto* v = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::String>>(table, field);
    std::vector<flatbuffers::Offset<flatbuffers::String>> out;
    out.reserve(v->size());
    for (const auto* s : *v) {
      out.push_back(fbb.CreateString(s));
    }
    return fbb.CreateVector(out).o;
  }
  if (elem == reflection::Obj) {
    const auto* sub = schema.objects()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()));
    if (!sub->is_struct()) {
      const auto* v = flatbuffers::GetFieldV<flatbuffers::Offset<flatbuffers::Table>>(table, field);
      std::vector<flatbuffers::Offset<const flatbuffers::Table*>> out;
      out.reserve(v->size());
      for (const auto* t : *v) {
        out.push_back(flatbuffers::CopyTable(fbb, schema, *sub, *t));
      }
      return fbb.CreateVector(out).o;
    }
    return copy_raw_vector(fbb, *flatbuffers::GetFieldAnyV(table, field), static_cast<std::size_t>(sub->bytesize()),
                           static_cast<std::size_t>(sub->minalign()));
  }
  const std::size_t size = flatbuffers::GetTypeSize(elem);
  return copy_raw_vector(fbb, *flatbuffers::GetFieldAnyV(table, field), size, size);
}

flatbuffers::uoffset_t copy_offset_field(flatbuffers::FlatBufferBuilder& fbb, const reflection::Schema& schema,
                                         const reflection::Field& field, const flatbuffers::Table& table) {
  switch (field.type()->base_type()) {
    case reflection::String:
      return fbb.CreateString(flatbuffers::GetFieldS(table, field)).o;
    case reflection::Obj:
      return flatbuffers::CopyTable(fbb, schema,
                                    *schema.objects()->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index())),
                                    *flatbuffers::GetFieldT(table, field))
          .o;
    case reflection::Vector:
      return copy_vector(fbb, schema, field, table);
    default:
      throw std::runtime_error("FrameAnalysis rewrite: union fields are not supported");
  }
}

}  // namespace

const reflection::Schema& model_schema() {
  const auto bytes = ovt::model::bfbs(ovt::model::SchemaId::Model);
  const reflection::Schema* schema = bytes.empty() ? nullptr : reflection::GetSchema(bytes.data());
  if (schema == nullptr || schema->objects() == nullptr) {
    throw std::runtime_error("embedded model schema missing");
  }
  return *schema;
}

const reflection::Object& model_object(std::string_view qualified) {
  const std::string key(qualified);
  const auto* object = model_schema().objects()->LookupByKey(key.c_str());
  if (object == nullptr) {
    throw std::runtime_error("model schema has no " + key);
  }
  return *object;
}

flatbuffers::uoffset_t copy_table_with(flatbuffers::FlatBufferBuilder& fbb, const reflection::Object& object,
                                       const flatbuffers::Table* table, const FieldOverrides& overrides) {
  const auto& schema = model_schema();
  // Offsets first: nested objects cannot be built while this table is open.
  std::vector<std::pair<const reflection::Field*, flatbuffers::uoffset_t>> offsets;
  for (const auto* field : *object.fields()) {
    if (field->deprecated() || !is_offset_field(schema, *field)) {
      continue;
    }
    if (auto it = overrides.offsets.find(field->name()->string_view()); it != overrides.offsets.end()) {
      offsets.emplace_back(field, it->second);
    } else if (table != nullptr && table->GetPointer<const uint8_t*>(field->offset()) != nullptr) {
      offsets.emplace_back(field, copy_offset_field(fbb, schema, *field, *table));
    }
  }
  const auto start = fbb.StartTable();
  for (const auto* field : *object.fields()) {
    if (field->deprecated() || is_offset_field(schema, *field)) {
      continue;
    }
    if (field->type()->base_type() == reflection::Obj) {
      write_struct_field(fbb, schema, *field, table, overrides);
    } else {
      write_scalar_field(fbb, *field, table, overrides);
    }
  }
  for (const auto& [field, off] : offsets) {
    fbb.AddOffset(field->offset(), flatbuffers::Offset<void>(off));
  }
  return fbb.EndTable(start);
}

}  // namespace ovt::analysis::detail
