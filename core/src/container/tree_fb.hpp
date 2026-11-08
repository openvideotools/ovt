// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Small helpers the elementary-stream readers (IVF, Y4M, ...) share to write ContainerTree nodes: spanned
// integer and string fields, diagnostics from the CNT-* registry, and a root node with children. Chapter 08 §8.4
// (BoxNode, Field), WI-1.225.

#ifndef OVT_CONTAINER_TREE_FB_HPP
#define OVT_CONTAINER_TREE_FB_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "ovt_container_generated.h"

namespace ovt::container::treefb {

using Fbb = flatbuffers::FlatBufferBuilder;

// A field of the box payload; `bit_offset` is relative to the owning BoxNode's file_span.offset (§8.4).
[[nodiscard]] inline flatbuffers::Offset<fb::Field> uint_field(Fbb& fbb, std::string_view name, uint64_t value,
                                                               uint64_t bit_offset, uint32_t bit_length) {
  const fb::BitSpan span(bit_offset, bit_length);
  return fb::CreateField(fbb, fbb.CreateString(name.data(), name.size()), fb::SyntaxValueKind::UInt, value, 0, 0, 0,
                         &span);
}

[[nodiscard]] inline flatbuffers::Offset<fb::Field> string_field(Fbb& fbb, std::string_view name,
                                                                 std::string_view value, uint64_t bit_offset,
                                                                 uint32_t bit_length) {
  const fb::BitSpan span(bit_offset, bit_length);
  return fb::CreateField(fbb, fbb.CreateString(name.data(), name.size()), fb::SyntaxValueKind::String, 0, 0,
                         fbb.CreateString(value.data(), value.size()), 0, &span);
}

// A registry diagnostic. `message` is the concrete text (the registry template with its parameters filled in);
// the span points at the byte offset in the file (length 0).
// SPEC-ASSUMPTION(#280): container-level diagnostics carry a file-relative BitSpan (byte offset * 8).
[[nodiscard]] inline flatbuffers::Offset<fb::Diagnostic> diagnostic(Fbb& fbb, DiagCode code, std::string_view message,
                                                                    uint64_t byte_offset) {
  const DiagInfo& info = diag(code);
  const fb::BitSpan span(byte_offset * 8U, 0);
  return fb::CreateDiagnostic(fbb, static_cast<fb::Severity>(info.severity),
                              fbb.CreateString(info.code.data(), info.code.size()),
                              fbb.CreateString(message.data(), message.size()), &span,
                              fbb.CreateString(info.spec_ref.data(), info.spec_ref.size()));
}

}  // namespace ovt::container::treefb

#endif  // OVT_CONTAINER_TREE_FB_HPP
