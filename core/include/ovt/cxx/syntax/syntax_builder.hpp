// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::syntax::SyntaxBuilder: the recording helper that every header parser reads through (chapter 7 §7.3.2).
// Each read_* call forwards to the BitReader and, while recording is on, emits one SyntaxNode with the spec
// element name, the descriptor text, the value and the bit span. C++ helper header (D22): semi-public, no ABI
// promise, never included from a C header, not installed.
//
// The public declarations below are chapter 7 §7.3.2 verbatim, plus: [[nodiscard]] on finish() (reads stay
// discardable: parsers skip reserved values), noexcept on the calls that only close spans, `final` on the class (it has
// a virtual function and a public non-virtual destructor), deleted moves, a constraint that sends string-like values of
// derived() to the text overload, the Severity enum and the private state.
//
// SPEC-ASSUMPTION(#96): §7.3.2 uses `Severity` without declaring a C++ type. It is declared here as
// ovt::syntax::Severity with the values of ovt.fb.Severity (checked by a static_assert in syntax_builder.cpp).
// SPEC-ASSUMPTION(#97): SyntaxNodeBuffer is only forward-declared here. Its definition is the module
// header core/src/syntax/common/syntax_node_buffer.h (the WI-1.106 Produces line), which every caller of
// finish() includes; the root node, the arena and the FlatBuffer conversion live there.

#ifndef OVT_CXX_SYNTAX_SYNTAX_BUILDER_HPP
#define OVT_CXX_SYNTAX_SYNTAX_BUILDER_HPP

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ovt/cxx/bitreader.hpp"

namespace ovt::syntax {

/// Diagnostic severity; the values equal ovt.fb.Severity (backbone §0.5.2).
enum class Severity : std::uint8_t { Info = 0, Warning = 1, Error = 2, Fatal = 3 };

class SyntaxNodeBuffer;               // core/src/syntax/common/syntax_node_buffer.h
struct SyntaxNodeData;                // one arena node; defined in syntax_node_buffer.h
struct SyntaxBufferState;             // arena + root; defined in syntax_node_buffer.h
enum class ValueKind : std::uint8_t;  // ovt.fb.SyntaxValueKind; defined in syntax_node_buffer.h

class SyntaxBuilder final : public BitReader::DiagnosticSink {
 public:
  // Attaches itself to `br` (br.set_diagnostic_sink(this)) and restores the previous sink in the destructor.
  // A reader warning (e.g. BR-0009) becomes diag(Severity::Warning, code, message) on the current struct.
  explicit SyntaxBuilder(BitReader& br, std::string_view default_spec_ref = {});
  ~SyntaxBuilder();
  SyntaxBuilder(const SyntaxBuilder&) = delete;
  SyntaxBuilder& operator=(const SyntaxBuilder&) = delete;
  SyntaxBuilder(SyntaxBuilder&&) = delete;
  SyntaxBuilder& operator=(SyntaxBuilder&&) = delete;
  void reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) override;

  // Structure nesting. begin_struct() creates a node with kind None whose span is closed by end_struct().
  void begin_struct(std::string_view name, std::string_view spec_ref = {});
  void end_struct() noexcept;  // sets span = {start, bit_pos()-start}
  // RAII convenience
  struct Scope {
    SyntaxBuilder& b;
    ~Scope() { b.end_struct(); }
  };
  [[nodiscard]] Scope scoped(std::string_view name, std::string_view spec_ref = {});

  // Array element nesting: every read_* inside gets array_index = idx (nodes named like "offset_for_ref_frame").
  void begin_array_element(std::string_view name, int idx, std::string_view spec_ref = {});
  void end_array_element() noexcept;

  // Reads. Each one: m = br.mark(); v = br.<descriptor>(...); emit node {name, descriptor text, kind, value,
  // span_since(m)}; return v.
  uint64_t read_u(std::string_view name, unsigned n);                     // "u(n)"  kind UInt
  uint64_t read_f(std::string_view name, unsigned n, uint64_t expected);  // "f(n)"; Diagnostic Error SYN-GEN-0001
                                                                          // if != expected, continue
  bool read_flag(std::string_view name);                                  // "u(1)"  kind Bool
  int64_t read_i(std::string_view name, unsigned n);                      // "i(n)"  kind Int
  uint32_t read_ue(std::string_view name);                                // "ue(v)" kind UInt
  int32_t read_se(std::string_view name);                                 // "se(v)" kind Int
  uint32_t read_te(std::string_view name, uint32_t cmax);                 // "te(v)"
  std::string read_st(std::string_view name);                             // "st(v)" kind String
  std::span<const uint8_t> read_bytes(std::string_view name, size_t n);   // "b(8)*n" kind Bytes (copied into
                                                                          // node.bytes when n <= 256, else only span)
  // AV1
  uint64_t read_leb128(std::string_view name);          // "leb128()"
  uint32_t read_uvlc(std::string_view name);            // "uvlc()"
  int64_t read_su(std::string_view name, unsigned n);   // "su(n)" (descriptor text "su(1+6)" when the caller
                                                        // passes a label, see below)
  uint32_t read_ns(std::string_view name, uint32_t n);  // "ns(n)"
  uint64_t read_le(std::string_view name, unsigned n);  // "le(n)"
  int32_t read_delta_q(std::string_view name);          // "delta_q" → two child nodes delta_coded f(1),
                                                        // delta_q su(1+6)
  // Enum decoration: sets enum_name of the LAST emitted node (e.g. after read_ue("slice_type"), set_enum("P")).
  void set_enum(std::string_view enum_name);
  // Overrides the descriptor text of the LAST emitted node (e.g. "u(v)" after read_u(name, bits)).
  void set_descriptor_text(std::string_view descriptor);
  // Derived values: appended as a child node {name, kind None, derived:text, span:{bit_pos,0}} of the current
  // struct.
  void derived(std::string_view name, std::string_view text);
  template <class T>
    requires(!std::is_convertible_v<const T&, std::string_view>)  // text goes to the overload above
  void derived(std::string_view name, T value) {
    if (recording_) {
      derived(name, std::string_view{fmt::format("{} = {}", name, value)});
    }
  }
  // Diagnostics attached to the current struct (or the last node if attach_to_last).
  void diag(Severity severity, std::string_view code, std::string_view message, std::string_view spec_ref = {},
            bool attach_to_last = false);
  // Opaque remainder (slice_data / tile data): node kind Bytes with span [bit_pos, end) and no copy.
  void opaque_rest(std::string_view name);

  // Result: the root node (the unit) and its children; call exactly once. Partial trees are returned after a
  // ParseError.
  [[nodiscard]] SyntaxNodeBuffer finish();  // SyntaxNodeBuffer = in-memory tree convertible to the
                                            // ovt.fb.SyntaxNode FlatBuffer (model module)
  // Flag for performance mode (7.4.5): when false, read_* records nothing and only returns values.
  void set_recording(bool on) noexcept;

 private:
  // Deepest nesting of begin_struct/begin_array_element. The syntax tables nest far less than this; going
  // deeper is a parser bug and throws std::logic_error (the stack is fixed so that a non-recording parse
  // allocates nothing).
  static constexpr size_t kMaxDepth = 64;

  SyntaxBufferState& state();
  SyntaxNodeData* current() noexcept;
  SyntaxNodeData* emit(std::string_view name, BitReader::Mark m);
  SyntaxNodeData* emit_value(std::string_view name, BitReader::Mark m, std::string_view descriptor, ValueKind kind);
  void open(std::string_view name, std::string_view spec_ref, int array_index);
  void close() noexcept;
  void add_diag(Severity severity, std::string_view code, std::string_view message, std::string_view spec_ref,
                SyntaxNodeData* target, BitSpan span);

  BitReader& br_;
  BitReader::DiagnosticSink* prev_sink_;
  std::string default_spec_ref_;
  BitReader::Mark start_;
  std::unique_ptr<SyntaxBufferState> state_;        // created on the first recorded node or diagnostic
  SyntaxNodeData* last_ = nullptr;                  // last emitted node (set_enum, set_descriptor_text, diag)
  std::array<SyntaxNodeData*, kMaxDepth> stack_{};  // open structs; nullptr for one opened while not recording
  size_t depth_ = 0;
  bool recording_ = true;
  bool finished_ = false;
};

}  // namespace ovt::syntax

#endif  // OVT_CXX_SYNTAX_SYNTAX_BUILDER_HPP
