// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output/fb_json.h"

#include <flatbuffers/reflection.h>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "ovt_container_generated.h"
#include "ovt_schemas_bfbs.inc"  // k_ovt_container_bfbs (build time, tools/embed_bfbs.py)

#ifndef OVT_BFBS_HAVE_OVT_CONTAINER
#error "ovt_container.bfbs is not embedded"
#endif

namespace ovt::cli {

namespace {

using reflection::BaseType;

// Chapter 06 §6.8.1: [ubyte] fields printed as lowercase hex.
constexpr std::array<std::string_view, 11> kHexFields = {"source_hash",
                                                         "reference_source_hash",
                                                         "distorted_source_hash",
                                                         "default_kid",
                                                         "system_id",
                                                         "uuid",
                                                         "iv",
                                                         "kid",
                                                         "kids",
                                                         "default_constant_iv",
                                                         "bytes"};

const reflection::Schema& schema() {
  static const reflection::Schema* s = reflection::GetSchema(static_cast<const void*>(k_ovt_container_bfbs));
  return *s;
}

// The fields of an object in schema (id) order; the reflection vector is sorted by name.
const std::vector<const reflection::Field*>& ordered_fields(const reflection::Object& obj) {
  // Built once per object type; the schema is immutable and the map only grows on first use of a type.
  static std::map<const reflection::Object*, std::vector<const reflection::Field*>> cache;
  auto [it, inserted] = cache.try_emplace(&obj);
  if (inserted) {
    it->second.assign(obj.fields()->begin(), obj.fields()->end());
    std::ranges::sort(it->second, {}, [](const reflection::Field* f) { return f->id(); });
  }
  return it->second;
}

bool is_integer(BaseType t) {
  return t >= BaseType::UType && t <= BaseType::ULong;
}

// One scalar value: enum name when the type is an enum and the value is named, else the number.
void scalar(JsonWriter& w, BaseType t, int index, std::int64_t i, double d) {
  if (t == BaseType::Float || t == BaseType::Double) {
    w.number(d);
    return;
  }
  if (t == BaseType::Bool) {
    if (i == 0 || i == 1) {
      w.boolean(i == 1);
    } else {
      w.integer(i);
    }
    return;
  }
  if (index >= 0 && is_integer(t)) {
    const reflection::Enum* e = schema().enums()->Get(static_cast<flatbuffers::uoffset_t>(index));
    if (const reflection::EnumVal* v = e->values()->LookupByKey(i); v != nullptr) {
      w.string(v->name()->string_view());
      return;
    }
  }
  if (t == BaseType::ULong) {
    w.unsigned_integer(static_cast<std::uint64_t>(i));
  } else {
    w.integer(i);
  }
}

const reflection::Object& object(int index) {
  return *schema().objects()->Get(static_cast<flatbuffers::uoffset_t>(index));
}

// A struct field or struct vector element: its fields are read from the inline bytes.
void write_struct(JsonWriter& w, const reflection::Object& so, const flatbuffers::Struct& st) {
  w.begin_object();
  for (const reflection::Field* f : ordered_fields(so)) {
    w.key(f->name()->string_view());
    scalar(w, f->type()->base_type(), f->type()->index(), flatbuffers::GetAnyFieldI(st, *f),
           flatbuffers::GetAnyFieldF(st, *f));
  }
  w.end_object();
}

}  // namespace

ContainerJson::ContainerJson(BoxFilter filter) : filter_(std::move(filter)) {}

void ContainerJson::write_root(JsonWriter& w, const std::uint8_t* buf, std::string_view type) const {
  write(w, flatbuffers::GetRoot<flatbuffers::Table>(buf), type);
}

// Nesting is bounded by the box tree depth (kMaxBoxDepth in the reader) and the Field children depth.
// NOLINTNEXTLINE(misc-no-recursion)
void ContainerJson::write(JsonWriter& w, const void* table, std::string_view type, int box_depth) const {
  const reflection::Object* obj = schema().objects()->LookupByKey(std::string(type).c_str());
  if (obj == nullptr) {
    throw std::invalid_argument("unknown table " + std::string(type));
  }
  const auto& t = *static_cast<const flatbuffers::Table*>(table);
  const bool box = filter_ && type == "ovt.fb.BoxNode";
  const BoxShow show = box ? filter_(*static_cast<const fb::BoxNode*>(table), box_depth) : BoxShow{};
  w.begin_object();
  for (const reflection::Field* f : ordered_fields(*obj)) {
    const std::string_view name = f->name()->string_view();
    if (box && ((name == "fields" && !show.fields) || (name == "children" && !show.children))) {
      continue;
    }
    const BaseType bt = f->type()->base_type();
    if (flatbuffers::IsScalar(bt)) {
      w.key(name);
      scalar(w, bt, f->type()->index(), flatbuffers::GetAnyFieldI(t, *f), flatbuffers::GetAnyFieldF(t, *f));
    } else if (t.CheckField(f->offset())) {  // an absent string, vector, struct or table is left out
      w.key(name);
      write_field(w, t, *f, box && name == "children" ? box_depth + 1 : box_depth);
    }
  }
  w.end_object();
}

// A present non-scalar field. Unions do not occur in ovt_container.fbs.
// NOLINTNEXTLINE(misc-no-recursion)
void ContainerJson::write_field(JsonWriter& w, const flatbuffers::Table& t, const reflection::Field& f,
                                int box_depth) const {
  const reflection::Type& ty = *f.type();
  if (ty.base_type() == BaseType::String) {
    w.string(flatbuffers::GetFieldS(t, f)->string_view());
  } else if (ty.base_type() == BaseType::Obj) {
    const reflection::Object& sub = object(ty.index());
    if (sub.is_struct()) {
      write_struct(w, sub, *flatbuffers::GetFieldStruct(t, f));
    } else {
      write(w, flatbuffers::GetFieldT(t, f), sub.name()->string_view(), box_depth);
    }
  } else if (ty.base_type() == BaseType::Vector) {
    write_vector(w, t, f, box_depth);
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void ContainerJson::write_vector(JsonWriter& w, const flatbuffers::Table& t, const reflection::Field& f,
                                 int box_depth) const {
  const reflection::Type& ty = *f.type();
  const BaseType et = ty.element();
  if (et == BaseType::UByte && std::ranges::find(kHexFields, f.name()->string_view()) != kHexFields.end()) {
    const auto* v = flatbuffers::GetFieldV<std::uint8_t>(t, f);
    w.string(to_hex(std::vector<std::uint8_t>(v->begin(), v->end())));
    return;
  }
  const auto* any = flatbuffers::GetFieldAnyV(t, f);
  w.begin_array();
  for (flatbuffers::uoffset_t i = 0; i < any->size(); ++i) {
    if (et == BaseType::String) {
      w.string(flatbuffers::GetAnyVectorElemPointer<const flatbuffers::String>(any, i)->string_view());
    } else if (et != BaseType::Obj) {
      scalar(w, et, ty.index(), flatbuffers::GetAnyVectorElemI(any, et, i), flatbuffers::GetAnyVectorElemF(any, et, i));
    } else if (const reflection::Object& sub = object(ty.index()); sub.is_struct()) {
      const auto* const elem = flatbuffers::GetAnyVectorElemAddressOf<const flatbuffers::Struct>(
          any, i, static_cast<std::size_t>(sub.bytesize()));
      write_struct(w, sub, *elem);
    } else {
      write(w, flatbuffers::GetAnyVectorElemPointer<const flatbuffers::Table>(any, i), sub.name()->string_view(),
            box_depth);
    }
  }
  w.end_array();
}

}  // namespace ovt::cli
