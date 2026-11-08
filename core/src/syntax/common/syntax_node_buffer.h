// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// SyntaxNodeBuffer: the in-memory syntax tree that SyntaxBuilder::finish() returns (chapter 7 §7.3.2, WI-1.106),
// and its conversion to the ovt.fb.SyntaxNode FlatBuffer (backbone §0.5.2). Module header of `syntax`; callers
// of SyntaxBuilder::finish() include it.
//
// Nodes live in a std::pmr::monotonic_buffer_resource arena (§7.4.6): strings are copied into the arena and
// held as string_views, children and diagnostics are intrusive singly linked lists, and no destructor runs for
// a node. Nothing is allocated while SyntaxBuilder::set_recording(false) is in effect, unless a diagnostic is
// raised.
//
// SPEC-ASSUMPTION(#97): the root is a synthetic container node named "unit" (kind None) that holds the
// top-level structs and the diagnostics raised outside any struct; its span runs from the reader position at
// SyntaxBuilder construction to the position at finish(). The caller that knows the codec renames it with
// set_root_name() (e.g. "nal_unit", "open_bitstream_unit").

#ifndef OVT_SRC_SYNTAX_COMMON_SYNTAX_NODE_BUFFER_H
#define OVT_SRC_SYNTAX_COMMON_SYNTAX_NODE_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/bitreader.hpp"
#include "ovt/cxx/syntax/syntax_builder.hpp"
#include "ovt_model_generated.h"

namespace ovt::syntax {

/// Values equal ovt.fb.SyntaxValueKind (backbone §0.5.2).
enum class ValueKind : std::uint8_t {
  None = 0,
  UInt = 1,
  Int = 2,
  Bool = 3,
  Bytes = 4,
  String = 5,
  Enum = 6,
  Fixed = 7
};

/// One ovt.fb.Diagnostic of a node.
struct DiagnosticData {
  Severity severity = Severity::Info;
  std::string_view code;
  std::string_view message;
  std::string_view spec_ref;
  BitSpan span{0, 0};
  DiagnosticData* next = nullptr;
};

/// One ovt.fb.SyntaxNode. Field names follow the schema. String views point into the owning arena.
struct SyntaxNodeData {
  std::string_view name;
  std::string_view descriptor;
  ValueKind kind = ValueKind::None;
  bool bvalue = false;
  bool array_element = false;  // opened by begin_array_element: its direct reads inherit array_index
  int32_t array_index = -1;
  uint64_t uvalue = 0;
  int64_t ivalue = 0;
  std::span<const uint8_t> bytes;  // copy in the arena (read_bytes with n <= 256); empty otherwise
  std::string_view svalue;
  std::string_view enum_name;
  std::string_view derived;
  BitSpan span{0, 0};
  std::string_view spec_ref;
  SyntaxNodeData* first_child = nullptr;
  SyntaxNodeData* last_child = nullptr;
  SyntaxNodeData* next_sibling = nullptr;
  DiagnosticData* first_diag = nullptr;
  DiagnosticData* last_diag = nullptr;

  [[nodiscard]] size_t child_count() const noexcept;
  [[nodiscard]] size_t diagnostic_count() const noexcept;
  /// The idx-th child (document order) or nullptr.
  [[nodiscard]] const SyntaxNodeData* child(size_t idx) const noexcept;
};

/// Counts the bytes handed out by the arena; arena_bytes() == 0 proves a parse allocated no node (WI-1.106 AT2).
class CountingResource final : public std::pmr::memory_resource {
 public:
  explicit CountingResource(std::pmr::memory_resource* upstream) noexcept : upstream_(upstream) {}
  [[nodiscard]] size_t bytes() const noexcept { return bytes_; }

 private:
  void* do_allocate(size_t bytes, size_t alignment) override;
  void do_deallocate(void* p, size_t bytes, size_t alignment) override;
  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

  std::pmr::memory_resource* upstream_;
  size_t bytes_ = 0;
};

/// The arena and the root node of one tree. Not copyable or movable: nodes point into it.
struct SyntaxBufferState {
  SyntaxBufferState(std::string_view default_spec_ref, uint64_t start_bit);
  SyntaxBufferState(const SyntaxBufferState&) = delete;
  SyntaxBufferState& operator=(const SyntaxBufferState&) = delete;
  SyntaxBufferState(SyntaxBufferState&&) = delete;
  SyntaxBufferState& operator=(SyntaxBufferState&&) = delete;
  ~SyntaxBufferState() = default;

  SyntaxNodeData* new_node();
  DiagnosticData* new_diagnostic();
  /// Copies `s` into the arena; an empty view stays empty and allocates nothing.
  std::string_view intern(std::string_view s);
  std::span<const uint8_t> intern(std::span<const uint8_t> b);
  static void append_child(SyntaxNodeData& parent, SyntaxNodeData* child) noexcept;
  static void append_diagnostic(SyntaxNodeData& node, DiagnosticData* d) noexcept;

  std::pmr::monotonic_buffer_resource arena;
  CountingResource counter{&arena};
  SyntaxNodeData root;
  size_t node_count = 0;  // nodes below the root
};

class SyntaxNodeBuffer {
 public:
  SyntaxNodeBuffer() noexcept;
  explicit SyntaxNodeBuffer(std::unique_ptr<SyntaxBufferState> state) noexcept;
  SyntaxNodeBuffer(SyntaxNodeBuffer&&) noexcept;
  SyntaxNodeBuffer& operator=(SyntaxNodeBuffer&&) noexcept;
  SyntaxNodeBuffer(const SyntaxNodeBuffer&) = delete;
  SyntaxNodeBuffer& operator=(const SyntaxNodeBuffer&) = delete;
  ~SyntaxNodeBuffer();

  /// True when nothing was recorded: recording was off and no diagnostic was raised. root() is then nullptr.
  [[nodiscard]] bool empty() const noexcept { return state_ == nullptr; }
  [[nodiscard]] const SyntaxNodeData* root() const noexcept;
  /// Renames the root node (no-op on an empty buffer).
  void set_root_name(std::string_view name);
  /// Bytes allocated from the arena for nodes, strings and diagnostics.
  [[nodiscard]] size_t arena_bytes() const noexcept;
  /// Nodes below the root.
  [[nodiscard]] size_t node_count() const noexcept;

  /// Serialises the tree (root included) into `fbb`. Strings that repeat (names, descriptors, spec_refs) are
  /// shared. Deterministic: the same tree always produces the same bytes (§0.7.5).
  flatbuffers::Offset<fb::SyntaxNode> to_flatbuffer(flatbuffers::FlatBufferBuilder& fbb) const;
  /// A finished FlatBuffer whose root table is the root SyntaxNode (no file identifier). Empty when empty().
  [[nodiscard]] std::vector<uint8_t> to_flatbuffer() const;

 private:
  std::unique_ptr<SyntaxBufferState> state_;
};

// ---- writers (syntax_dump.cpp) ------------------------------------------------------------------------------

/// JSON dump of the tree. Keys are the ovt.fb.SyntaxNode / ovt.fb.Diagnostic field names, enums are written
/// by name, fields at their default value are left out except `span` and the value field of the node's kind.
/// Two-space indentation, '\n' line ends, deterministic. "null\n" for an empty buffer.
[[nodiscard]] std::string to_json(const SyntaxNodeBuffer& buffer);

/// `--format trace` lines in the FFmpeg trace_headers layout of chapter 7 §7.7.1:
/// `<bit_offset> <name>[<i>][<j>] <bits> = <value>\n`, in document order, for every leaf node that holds a
/// value (kind != None) and has bit_length > 0. Struct nodes, derived nodes and Bytes nodes are skipped. The
/// indices are the array_index of the enclosing begin_array_element nodes. `unit_bytes` is the buffer the
/// BitReader read (the RBSP incl. NAL header, or the OBU); bits outside it print as '?'. Values over 64 bits
/// show their first 64 bits followed by "...".
[[nodiscard]] std::string to_trace(const SyntaxNodeBuffer& buffer, std::span<const uint8_t> unit_bytes);

}  // namespace ovt::syntax

#endif  // OVT_SRC_SYNTAX_COMMON_SYNTAX_NODE_BUFFER_H
