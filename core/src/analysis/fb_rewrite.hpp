// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copy-with-overrides of FlatBuffers tables through the embedded model schema (reflection), used by the analysis
// Session to patch a plugin's FrameAnalysis (chapter 7 §7.5.3 A0/A6) without the object API: every field the
// schema knows, including fields appended later (additive schema rule), is copied unless it is overridden.
// Internal to libovt-core.

#ifndef OVT_SRC_ANALYSIS_FB_REWRITE_HPP
#define OVT_SRC_ANALYSIS_FB_REWRITE_HPP

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ovt::analysis::detail {

struct FieldOverrides {
  std::map<std::string, flatbuffers::uoffset_t, std::less<>> offsets;  // strings/vectors/tables built in the builder
  std::map<std::string, int64_t, std::less<>> scalars;                 // integer and bool fields
  std::map<std::string, std::vector<uint8_t>, std::less<>> structs;    // inline structs, wire layout

  template <class S>
  void set_struct(const std::string& name, const S& value) {
    std::vector<uint8_t> bytes(sizeof(S));
    std::memcpy(bytes.data(), &value, sizeof(S));
    structs[name] = std::move(bytes);
  }
};

// The model schema (ovt_model.fbs) from the embedded .bfbs. Throws std::runtime_error when it is missing.
[[nodiscard]] const reflection::Schema& model_schema();
// The object `qualified` ("ovt.fb.FrameInfo") of the model schema. Throws std::runtime_error when unknown.
[[nodiscard]] const reflection::Object& model_object(std::string_view qualified);

// Writes a copy of `table` (of type `object`; null = an empty table) into `fbb`, with `overrides` replacing the
// named fields, and returns its offset. Must not be called while another table is being built in `fbb`.
// Throws std::runtime_error for a union field (none exist in the model schema).
[[nodiscard]] flatbuffers::uoffset_t copy_table_with(flatbuffers::FlatBufferBuilder& fbb,
                                                     const reflection::Object& object, const flatbuffers::Table* table,
                                                     const FieldOverrides& overrides);

}  // namespace ovt::analysis::detail

#endif  // OVT_SRC_ANALYSIS_FB_REWRITE_HPP
