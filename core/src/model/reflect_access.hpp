// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The reflection helpers of the canonical builder (WI-0.085, canonical.cpp) that the JSON exporter and
// importer (WI-0.088) share: schema lookup, the safe reflection verifier and the nested-buffer rule.
// Internal header; the definitions are in canonical.cpp.

#ifndef OVT_CORE_MODEL_REFLECT_ACCESS_HPP
#define OVT_CORE_MODEL_REFLECT_ACCESS_HPP

#include <flatbuffers/reflection_generated.h>

#include <cstdint>
#include <span>
#include <string_view>

#include "ovt/cxx/model/schema_id.hpp"

namespace ovt::model::detail {

/// Nested buffers inside nested buffers accepted by canonicalize, structural_diff, to_json and from_json.
inline constexpr int kMaxNestedBuffers = 4;

struct ReflectedRoot {
  const reflection::Schema* schema;
  const reflection::Object* root;
};

/// The embedded schema of @p id and its table @p root_table ("ovt.fb.FrameAnalysis").
/// @throws std::invalid_argument ("<caller>: ...") for an unknown schema id or table name.
ReflectedRoot resolve_root(SchemaId id, std::string_view root_table, const char* caller);

/// True when @p aligned (8-byte aligned storage) is a finished, not size-prefixed buffer whose root is
/// @p object. Uses the verifier of canonical.cpp (every slot checked before it is read, `required` enforced,
/// the WI-0.094 limits), not flatbuffers::Verify.
bool verify_aligned(const reflection::Schema& schema, const reflection::Object& object,
                    std::span<const std::uint8_t> aligned);

/// Table @p name ("HevcCu" or "ovt.fb.HevcCu"), looked up in the namespace of @p context first; else nullptr.
const reflection::Object* find_table(const reflection::Schema& schema, const reflection::Object& context,
                                     std::string_view name);

/// For a `[ubyte]` field `X` of @p object, its sibling string field `X_type` (§6.3.3); else nullptr.
const reflection::Field* nested_type_sibling(const reflection::Object& object, const reflection::Field& field);

}  // namespace ovt::model::detail

#endif  // OVT_CORE_MODEL_REFLECT_ACCESS_HPP
