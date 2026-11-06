// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::syntax::SyntaxBuilder (chapter 7 §7.3.2, WI-1.106).
//
// Every read first calls the BitReader, so a ParseError leaves the tree exactly as it was before the failing
// read (§7.3.1 rule 1) and recording on or off returns the same values. With recording off a read is the
// BitReader call plus one branch: no node, no string and no arena block is created.

#include "ovt/cxx/syntax/syntax_builder.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "syntax/common/syntax_node_buffer.h"

namespace ovt::syntax {

namespace {

// Descriptor texts are short ("u(64)", "b(8)*65535", "su(1+6)"); 32 bytes leave room for any argument.
using DescriptorText = std::array<char, 32>;

template <class... Args>
std::string_view descriptor_text(DescriptorText& buf, fmt::format_string<Args...> f, Args&&... args) {
  const auto r = fmt::format_to_n(buf.data(), buf.size(), f, std::forward<Args>(args)...);
  return {buf.data(), std::min(static_cast<size_t>(r.size), buf.size())};
}

uint32_t saturate32(uint64_t v) noexcept {
  return static_cast<uint32_t>(std::min<uint64_t>(v, std::numeric_limits<uint32_t>::max()));
}

// read_bytes copies the bytes into the node only up to this size (§7.3.2); larger runs keep only the span.
constexpr size_t kMaxCopiedBytes = 256;

}  // namespace

SyntaxBuilder::SyntaxBuilder(BitReader& br, std::string_view default_spec_ref)
    : br_(br), prev_sink_(br.diagnostic_sink()), default_spec_ref_(default_spec_ref), start_(br.mark()) {
  br_.set_diagnostic_sink(this);
}

SyntaxBuilder::~SyntaxBuilder() {
  br_.set_diagnostic_sink(prev_sink_);
}

// ---- internals ----------------------------------------------------------------------------------------------

SyntaxBufferState& SyntaxBuilder::state() {
  if (!state_) {
    state_ = std::make_unique<SyntaxBufferState>(default_spec_ref_, start_);
  }
  return *state_;
}

SyntaxNodeData* SyntaxBuilder::current() noexcept {
  for (size_t i = depth_; i > 0; --i) {
    if (stack_[i - 1] != nullptr) {
      return stack_[i - 1];
    }
  }
  return state_ ? &state_->root : nullptr;
}

SyntaxNodeData* SyntaxBuilder::emit_value(std::string_view name, BitReader::Mark m, std::string_view descriptor,
                                          ValueKind kind) {
  SyntaxNodeData* n = emit(name, m);
  n->descriptor = state_->intern(descriptor);
  n->kind = kind;
  return n;
}

SyntaxNodeData* SyntaxBuilder::emit(std::string_view name, BitReader::Mark m) {
  SyntaxBufferState& st = state();
  SyntaxNodeData* parent = current();
  SyntaxNodeData* n = st.new_node();
  n->name = st.intern(name);
  n->span = br_.span_since(m);
  n->spec_ref = parent->spec_ref;  // FR-SYN-007: every node names its syntax table; the arena string is shared
  if (parent->array_element) {
    n->array_index = parent->array_index;
  }
  SyntaxBufferState::append_child(*parent, n);
  last_ = n;
  return n;
}

void SyntaxBuilder::open(std::string_view name, std::string_view spec_ref, int array_index) {
  if (depth_ == kMaxDepth) {
    throw std::logic_error("SyntaxBuilder: structs nested deeper than kMaxDepth");
  }
  if (!recording_) {
    stack_[depth_++] = nullptr;
    return;
  }
  SyntaxBufferState& st = state();
  SyntaxNodeData* parent = current();
  SyntaxNodeData* n = st.new_node();
  n->name = st.intern(name);
  n->spec_ref = spec_ref.empty() ? parent->spec_ref : st.intern(spec_ref);
  n->span = BitSpan{br_.bit_pos(), 0};
  n->array_index = array_index;
  SyntaxBufferState::append_child(*parent, n);
  stack_[depth_++] = n;
}

void SyntaxBuilder::close() noexcept {
  if (depth_ == 0) {
    return;  // unbalanced end, or a Scope that outlives finish()
  }
  SyntaxNodeData* n = stack_[--depth_];
  if (n != nullptr) {
    n->span.bit_length = saturate32(br_.bit_pos() - n->span.bit_offset);
  }
}

void SyntaxBuilder::add_diag(Severity severity, std::string_view code, std::string_view message,
                             std::string_view spec_ref, SyntaxNodeData* target, BitSpan span) {
  SyntaxBufferState& st = state();
  if (target == nullptr) {
    target = current();
  }
  DiagnosticData* d = st.new_diagnostic();
  d->severity = severity;
  d->code = st.intern(code);
  d->message = st.intern(message);
  d->spec_ref = spec_ref.empty() ? target->spec_ref : st.intern(spec_ref);
  d->span = span;
  SyntaxBufferState::append_diagnostic(*target, d);
}

// ---- DiagnosticSink -----------------------------------------------------------------------------------------

void SyntaxBuilder::reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) {
  add_diag(Severity::Warning, code, message, {}, nullptr, BitSpan{bit_offset, 0});
}

// ---- structure ----------------------------------------------------------------------------------------------

void SyntaxBuilder::begin_struct(std::string_view name, std::string_view spec_ref) {
  open(name, spec_ref, -1);
}

void SyntaxBuilder::end_struct() noexcept {
  close();
}

SyntaxBuilder::Scope SyntaxBuilder::scoped(std::string_view name, std::string_view spec_ref) {
  begin_struct(name, spec_ref);
  return Scope{*this};
}

void SyntaxBuilder::begin_array_element(std::string_view name, int idx, std::string_view spec_ref) {
  open(name, spec_ref, idx);
  if (recording_) {
    stack_[depth_ - 1]->array_element = true;
  }
}

void SyntaxBuilder::end_array_element() noexcept {
  close();
}

// ---- reads --------------------------------------------------------------------------------------------------

uint64_t SyntaxBuilder::read_u(std::string_view name, unsigned n) {
  const auto m = br_.mark();
  const uint64_t v = br_.u(n);
  if (recording_) {
    DescriptorText t{};
    emit_value(name, m, descriptor_text(t, "u({})", n), ValueKind::UInt)->uvalue = v;
  }
  return v;
}

uint64_t SyntaxBuilder::read_f(std::string_view name, unsigned n, uint64_t expected) {
  const auto m = br_.mark();
  const uint64_t v = br_.f(n);
  SyntaxNodeData* nd = nullptr;
  if (recording_) {
    DescriptorText t{};
    nd = emit_value(name, m, descriptor_text(t, "f({})", n), ValueKind::UInt);
    nd->uvalue = v;
  }
  if (v != expected) {
    // SYN-GEN-0001 (§7.9): "%s: fixed-pattern f(%u) read %llu, expected %llu"; parsing continues.
    add_diag(Severity::Error, "SYN-GEN-0001",
             fmt::format("{}: fixed-pattern f({}) read {}, expected {}", name, n, v, expected), {}, nd,
             br_.span_since(m));
  }
  return v;
}

bool SyntaxBuilder::read_flag(std::string_view name) {
  const auto m = br_.mark();
  const bool v = br_.u(1) != 0;
  if (recording_) {
    emit_value(name, m, "u(1)", ValueKind::Bool)->bvalue = v;
  }
  return v;
}

int64_t SyntaxBuilder::read_i(std::string_view name, unsigned n) {
  const auto m = br_.mark();
  const int64_t v = br_.i(n);
  if (recording_) {
    DescriptorText t{};
    emit_value(name, m, descriptor_text(t, "i({})", n), ValueKind::Int)->ivalue = v;
  }
  return v;
}

uint32_t SyntaxBuilder::read_ue(std::string_view name) {
  const auto m = br_.mark();
  const uint32_t v = br_.ue();
  if (recording_) {
    emit_value(name, m, "ue(v)", ValueKind::UInt)->uvalue = v;
  }
  return v;
}

int32_t SyntaxBuilder::read_se(std::string_view name) {
  const auto m = br_.mark();
  const int32_t v = br_.se();
  if (recording_) {
    emit_value(name, m, "se(v)", ValueKind::Int)->ivalue = v;
  }
  return v;
}

uint32_t SyntaxBuilder::read_te(std::string_view name, uint32_t cmax) {
  const auto m = br_.mark();
  const uint32_t v = br_.te(cmax);
  if (recording_) {
    emit_value(name, m, "te(v)", ValueKind::UInt)->uvalue = v;
  }
  return v;
}

std::string SyntaxBuilder::read_st(std::string_view name) {
  const auto m = br_.mark();
  std::string v = br_.st();
  if (recording_) {
    SyntaxNodeData* nd = emit_value(name, m, "st(v)", ValueKind::String);  // creates state_ before intern()
    nd->svalue = state_->intern(v);
  }
  return v;
}

std::span<const uint8_t> SyntaxBuilder::read_bytes(std::string_view name, size_t n) {
  const auto m = br_.mark();
  const std::span<const uint8_t> rest = br_.remaining_bytes();  // BR-0004 when not byte aligned
  br_.skip_bytes(n);                                            // BR-0001 when n exceeds the unit
  const std::span<const uint8_t> v = rest.first(n);
  if (recording_) {
    DescriptorText t{};
    SyntaxNodeData* nd = emit(name, m);
    nd->descriptor = state_->intern(descriptor_text(t, "b(8)*{}", n));
    nd->kind = ValueKind::Bytes;
    if (n <= kMaxCopiedBytes) {
      nd->bytes = state_->intern(v);
    }
  }
  return v;
}

uint64_t SyntaxBuilder::read_leb128(std::string_view name) {
  const auto m = br_.mark();
  const uint64_t v = br_.leb128();  // a BR-0009 warning lands on the current struct through reader_warning()
  if (recording_) {
    emit_value(name, m, "leb128()", ValueKind::UInt)->uvalue = v;
  }
  return v;
}

uint32_t SyntaxBuilder::read_uvlc(std::string_view name) {
  const auto m = br_.mark();
  const uint32_t v = br_.uvlc();
  if (recording_) {
    emit_value(name, m, "uvlc()", ValueKind::UInt)->uvalue = v;
  }
  return v;
}

int64_t SyntaxBuilder::read_su(std::string_view name, unsigned n) {
  const auto m = br_.mark();
  const int64_t v = br_.su(n);
  if (recording_) {
    DescriptorText t{};
    emit_value(name, m, descriptor_text(t, "su({})", n), ValueKind::Int)->ivalue = v;
  }
  return v;
}

uint32_t SyntaxBuilder::read_ns(std::string_view name, uint32_t n) {
  const auto m = br_.mark();
  const uint32_t v = br_.ns(n);
  if (recording_) {
    DescriptorText t{};
    emit_value(name, m, descriptor_text(t, "ns({})", n), ValueKind::UInt)->uvalue = v;
  }
  return v;
}

uint64_t SyntaxBuilder::read_le(std::string_view name, unsigned n) {
  const auto m = br_.mark();
  const uint64_t v = br_.le(n);
  if (recording_) {
    DescriptorText t{};
    emit_value(name, m, descriptor_text(t, "le({})", n), ValueKind::UInt)->uvalue = v;
  }
  return v;
}

int32_t SyntaxBuilder::read_delta_q(std::string_view name) {
  if (!recording_) {
    return br_.delta_q();
  }
  // AV1 §5.9.13 read_delta_q(): delta_coded f(1); if (delta_coded) delta_q su(1+6). The two reads are the
  // children of a node `name` that carries the resulting value.
  const auto m = br_.mark();
  open(name, {}, -1);
  SyntaxNodeData* parent = stack_[depth_ - 1];
  struct Closer {
    SyntaxBuilder& b;
    explicit Closer(SyntaxBuilder& sb) noexcept : b(sb) {}
    Closer(const Closer&) = delete;
    Closer& operator=(const Closer&) = delete;
    Closer(Closer&&) = delete;
    Closer& operator=(Closer&&) = delete;
    ~Closer() { b.close(); }
  } const closer(*this);
  const auto m_coded = br_.mark();
  const bool coded = br_.u(1) != 0;
  emit_value("delta_coded", m_coded, "f(1)", ValueKind::Bool)->bvalue = coded;
  int32_t v = 0;
  if (coded) {
    const auto m_q = br_.mark();
    v = static_cast<int32_t>(br_.su(1 + 6));
    emit_value("delta_q", m_q, "su(1+6)", ValueKind::Int)->ivalue = v;
  }
  parent->descriptor = "delta_q";
  parent->kind = ValueKind::Int;
  parent->ivalue = v;
  parent->span = br_.span_since(m);
  last_ = parent;
  return v;
}

// ---- decoration ---------------------------------------------------------------------------------------------

void SyntaxBuilder::set_enum(std::string_view enum_name) {
  if (recording_ && last_ != nullptr) {
    last_->enum_name = state_->intern(enum_name);
  }
}

void SyntaxBuilder::set_descriptor_text(std::string_view descriptor) {
  if (recording_ && last_ != nullptr) {
    last_->descriptor = state_->intern(descriptor);
  }
}

void SyntaxBuilder::derived(std::string_view name, std::string_view text) {
  if (!recording_) {
    return;
  }
  SyntaxNodeData* nd = emit(name, br_.mark());
  nd->derived = state_->intern(text);
}

void SyntaxBuilder::diag(Severity severity, std::string_view code, std::string_view message, std::string_view spec_ref,
                         bool attach_to_last) {
  // Diagnostics are kept with recording off too (they then land on the root), so performance mode loses no
  // finding. A diagnostic on the last node takes that node's span; one on a struct points at the current bit.
  SyntaxNodeData* target = (attach_to_last && last_ != nullptr) ? last_ : nullptr;
  const BitSpan span = target != nullptr ? target->span : BitSpan{br_.bit_pos(), 0};
  add_diag(severity, code, message, spec_ref, target, span);
}

void SyntaxBuilder::opaque_rest(std::string_view name) {
  const auto m = br_.mark();
  br_.skip_bits(br_.bits_left());  // the remainder is consumed, so recording on and off leave the reader alike
  if (recording_) {
    SyntaxNodeData* nd = emit(name, m);
    nd->kind = ValueKind::Bytes;
  }
}

// ---- result -------------------------------------------------------------------------------------------------

SyntaxNodeBuffer SyntaxBuilder::finish() {
  if (finished_) {
    throw std::logic_error("SyntaxBuilder::finish() called twice");
  }
  finished_ = true;
  while (depth_ > 0) {
    close();  // structs left open by a ParseError (or a missing end_struct)
  }
  last_ = nullptr;
  if (!state_ && !recording_) {
    return SyntaxNodeBuffer{};
  }
  SyntaxBufferState& st = state();
  st.root.span.bit_length = saturate32(br_.bit_pos() - st.root.span.bit_offset);
  return SyntaxNodeBuffer{std::move(state_)};
}

void SyntaxBuilder::set_recording(bool on) noexcept {
  recording_ = on;
  if (!on) {
    last_ = nullptr;
  }
}

}  // namespace ovt::syntax
