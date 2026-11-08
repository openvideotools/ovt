// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// SyntaxNodeBuffer: arena tree and its ovt.fb.SyntaxNode FlatBuffer conversion (chapter 7 §7.3.2, WI-1.106).

#include "syntax/common/syntax_node_buffer.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ovt::syntax {

static_assert(static_cast<int>(ValueKind::None) == static_cast<int>(fb::SyntaxValueKind::None));
static_assert(static_cast<int>(ValueKind::UInt) == static_cast<int>(fb::SyntaxValueKind::UInt));
static_assert(static_cast<int>(ValueKind::Int) == static_cast<int>(fb::SyntaxValueKind::Int));
static_assert(static_cast<int>(ValueKind::Bool) == static_cast<int>(fb::SyntaxValueKind::Bool));
static_assert(static_cast<int>(ValueKind::Bytes) == static_cast<int>(fb::SyntaxValueKind::Bytes));
static_assert(static_cast<int>(ValueKind::String) == static_cast<int>(fb::SyntaxValueKind::String));
static_assert(static_cast<int>(ValueKind::Enum) == static_cast<int>(fb::SyntaxValueKind::Enum));
static_assert(static_cast<int>(ValueKind::Fixed) == static_cast<int>(fb::SyntaxValueKind::Fixed));
static_assert(static_cast<int>(Severity::Info) == static_cast<int>(fb::Severity::Info));
static_assert(static_cast<int>(Severity::Warning) == static_cast<int>(fb::Severity::Warning));
static_assert(static_cast<int>(Severity::Error) == static_cast<int>(fb::Severity::Error));
static_assert(static_cast<int>(Severity::Fatal) == static_cast<int>(fb::Severity::Fatal));

namespace {

// First arena block. A typical parameter set or slice header fits; the arena grows geometrically after that.
constexpr size_t kInitialArenaBytes = 4096;

}  // namespace

// ---- SyntaxNodeData -----------------------------------------------------------------------------------------

size_t SyntaxNodeData::child_count() const noexcept {
  size_t n = 0;
  for (const SyntaxNodeData* c = first_child; c != nullptr; c = c->next_sibling) {
    ++n;
  }
  return n;
}

size_t SyntaxNodeData::diagnostic_count() const noexcept {
  size_t n = 0;
  for (const DiagnosticData* d = first_diag; d != nullptr; d = d->next) {
    ++n;
  }
  return n;
}

const SyntaxNodeData* SyntaxNodeData::child(size_t idx) const noexcept {
  const SyntaxNodeData* c = first_child;
  for (; c != nullptr && idx > 0; --idx) {
    c = c->next_sibling;
  }
  return c;
}

// ---- CountingResource ---------------------------------------------------------------------------------------

void* CountingResource::do_allocate(size_t bytes, size_t alignment) {
  void* p = upstream_->allocate(bytes, alignment);
  bytes_ += bytes;
  return p;
}

void CountingResource::do_deallocate(void* p, size_t bytes, size_t alignment) {
  upstream_->deallocate(p, bytes, alignment);
}

bool CountingResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
  return this == &other;
}

// ---- SyntaxBufferState --------------------------------------------------------------------------------------

SyntaxBufferState::SyntaxBufferState(std::string_view default_spec_ref, uint64_t start_bit)
    : arena(kInitialArenaBytes) {
  root.name = "unit";
  root.spec_ref = intern(default_spec_ref);
  root.span = BitSpan{start_bit, 0};
}

SyntaxNodeData* SyntaxBufferState::new_node() {
  static_assert(std::is_trivially_destructible_v<SyntaxNodeData>, "arena nodes are never destroyed");
  std::pmr::polymorphic_allocator<SyntaxNodeData> alloc(&counter);
  auto* n = alloc.new_object<SyntaxNodeData>();
  ++node_count;
  return n;
}

DiagnosticData* SyntaxBufferState::new_diagnostic() {
  static_assert(std::is_trivially_destructible_v<DiagnosticData>, "arena diagnostics are never destroyed");
  std::pmr::polymorphic_allocator<DiagnosticData> alloc(&counter);
  return alloc.new_object<DiagnosticData>();
}

std::string_view SyntaxBufferState::intern(std::string_view s) {
  if (s.empty()) {
    return {};
  }
  auto* p = static_cast<char*>(counter.allocate(s.size(), alignof(char)));
  std::memcpy(p, s.data(), s.size());
  return {p, s.size()};
}

std::span<const uint8_t> SyntaxBufferState::intern(std::span<const uint8_t> b) {
  if (b.empty()) {
    return {};
  }
  auto* p = static_cast<uint8_t*>(counter.allocate(b.size(), alignof(uint8_t)));
  std::memcpy(p, b.data(), b.size());
  return {p, b.size()};
}

void SyntaxBufferState::append_child(SyntaxNodeData& parent, SyntaxNodeData* child) noexcept {
  if (parent.last_child == nullptr) {
    parent.first_child = child;
  } else {
    parent.last_child->next_sibling = child;
  }
  parent.last_child = child;
}

void SyntaxBufferState::append_diagnostic(SyntaxNodeData& node, DiagnosticData* d) noexcept {
  if (node.last_diag == nullptr) {
    node.first_diag = d;
  } else {
    node.last_diag->next = d;
  }
  node.last_diag = d;
}

// ---- SyntaxNodeBuffer ---------------------------------------------------------------------------------------

SyntaxNodeBuffer::SyntaxNodeBuffer() noexcept = default;
SyntaxNodeBuffer::SyntaxNodeBuffer(std::unique_ptr<SyntaxBufferState> state) noexcept : state_(std::move(state)) {}
SyntaxNodeBuffer::SyntaxNodeBuffer(SyntaxNodeBuffer&&) noexcept = default;
SyntaxNodeBuffer& SyntaxNodeBuffer::operator=(SyntaxNodeBuffer&&) noexcept = default;
SyntaxNodeBuffer::~SyntaxNodeBuffer() = default;

const SyntaxNodeData* SyntaxNodeBuffer::root() const noexcept {
  return state_ ? &state_->root : nullptr;
}

void SyntaxNodeBuffer::set_root_name(std::string_view name) {
  if (state_) {
    state_->root.name = state_->intern(name);
  }
}

size_t SyntaxNodeBuffer::arena_bytes() const noexcept {
  return state_ ? state_->counter.bytes() : 0;
}

size_t SyntaxNodeBuffer::node_count() const noexcept {
  return state_ ? state_->node_count : 0;
}

namespace {

flatbuffers::Offset<flatbuffers::String> shared(flatbuffers::FlatBufferBuilder& fbb, std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  return fbb.CreateSharedString(s.data(), s.size());
}

flatbuffers::Offset<flatbuffers::String> unique(flatbuffers::FlatBufferBuilder& fbb, std::string_view s) {
  if (s.empty()) {
    return 0;
  }
  return fbb.CreateString(s.data(), s.size());
}

// Recursion depth is bounded by SyntaxBuilder's fixed nesting stack (kMaxDepth) plus read_delta_q's one level.
// NOLINTNEXTLINE(misc-no-recursion)
flatbuffers::Offset<fb::SyntaxNode> build_node(flatbuffers::FlatBufferBuilder& fbb, const SyntaxNodeData& n) {
  // Children and every string first: a table cannot be started while another object is being built.
  std::vector<flatbuffers::Offset<fb::SyntaxNode>> children;
  children.reserve(n.child_count());
  for (const SyntaxNodeData* c = n.first_child; c != nullptr; c = c->next_sibling) {
    children.push_back(build_node(fbb, *c));
  }
  std::vector<flatbuffers::Offset<fb::Diagnostic>> diags;
  diags.reserve(n.diagnostic_count());
  for (const DiagnosticData* d = n.first_diag; d != nullptr; d = d->next) {
    const auto code = shared(fbb, d->code);
    const auto message = unique(fbb, d->message);
    const auto spec_ref = shared(fbb, d->spec_ref);
    const fb::BitSpan span(d->span.bit_offset, d->span.bit_length);
    diags.push_back(fb::CreateDiagnostic(fbb, static_cast<fb::Severity>(d->severity), code, message, &span, spec_ref));
  }

  const auto name = fbb.CreateSharedString(n.name.data(), n.name.size());  // required, even when empty
  const auto descriptor = shared(fbb, n.descriptor);
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> bytes = 0;
  if (!n.bytes.empty()) {
    bytes = fbb.CreateVector(n.bytes.data(), n.bytes.size());
  }
  const auto svalue = unique(fbb, n.svalue);
  const auto enum_name = shared(fbb, n.enum_name);
  const auto derived = unique(fbb, n.derived);
  const auto spec_ref = shared(fbb, n.spec_ref);
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::SyntaxNode>>> children_vec = 0;
  if (!children.empty()) {
    children_vec = fbb.CreateVector(children);
  }
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fb::Diagnostic>>> diags_vec = 0;
  if (!diags.empty()) {
    diags_vec = fbb.CreateVector(diags);
  }

  const fb::BitSpan span(n.span.bit_offset, n.span.bit_length);
  fb::SyntaxNodeBuilder b(fbb);
  b.add_name(name);
  if (!descriptor.IsNull()) {
    b.add_descriptor(descriptor);
  }
  b.add_kind(static_cast<fb::SyntaxValueKind>(n.kind));
  b.add_uvalue(n.uvalue);
  b.add_ivalue(n.ivalue);
  b.add_bvalue(n.bvalue);
  if (!bytes.IsNull()) {
    b.add_bytes(bytes);
  }
  if (!svalue.IsNull()) {
    b.add_svalue(svalue);
  }
  if (!enum_name.IsNull()) {
    b.add_enum_name(enum_name);
  }
  if (!derived.IsNull()) {
    b.add_derived(derived);
  }
  b.add_span(&span);
  if (!spec_ref.IsNull()) {
    b.add_spec_ref(spec_ref);
  }
  if (!children_vec.IsNull()) {
    b.add_children(children_vec);
  }
  b.add_array_index(n.array_index);
  if (!diags_vec.IsNull()) {
    b.add_diagnostics(diags_vec);
  }
  return b.Finish();
}

}  // namespace

flatbuffers::Offset<fb::SyntaxNode> SyntaxNodeBuffer::to_flatbuffer(flatbuffers::FlatBufferBuilder& fbb) const {
  if (!state_) {
    return 0;
  }
  return build_node(fbb, state_->root);
}

std::vector<uint8_t> SyntaxNodeBuffer::to_flatbuffer() const {
  if (!state_) {
    return {};
  }
  flatbuffers::FlatBufferBuilder fbb(4096);
  fbb.Finish(to_flatbuffer(fbb));
  const std::span<const uint8_t> out(fbb.GetBufferPointer(), fbb.GetSize());
  return {out.begin(), out.end()};
}

}  // namespace ovt::syntax
